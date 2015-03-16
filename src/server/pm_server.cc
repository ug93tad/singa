/*
 * pm_server.cc
 *
 *  Created on: 14 Mar, 2015
 *      Author: dinhtta
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <gflags/gflags.h>
#include <string.h>
#include <czmq.h>
#include <stdlib.h>
#include <set>
#include "server/pm_server.h"
#include "proto/topology.pb.h"

#include <google/protobuf/text_format.h>
#include <goodle/protobuf/io/zero_copy_stream_impl.h>
DECLARE_string(topology_config);
DECLARE_int32(server_threads);

using namespace google::protobuf:io;
using google::protobuf::TextFormat;
using std::set;

namespace singa{

SingaServer::SingaServer(int id){
	id_ = id;

	//Read Topology message from the file
	int fd = open(FLAGS_topology_config.c_str(),O_RDONLY);
	assert(fd);
	Topology topology;
	TextFormat::Parse(new FileInputStream(fd), &topology);
	int n_servers = topology.server_size();
	map<int, ServerConfig*> other_servers;


	for (int i=0; i<n_servers; i++){
		ServerConfig *server = topology.mutable_server(i);
		if (server->id()==id_){
			sprintf(frontend_endpoint_,"tcp://%s:%d",server->ip(),server->port());
			sprintf(backend_endpoint_,"inproc://singanus:%d",id_);
		}
		else {
			char *neighbor_endpoint = (char*)malloc(256);
			sprintf(neighbor_endpoint,"tcp://%s:%d",server->ip(),server->port());
			other_servers[server->id()] = neighbor_endpoint;
		}
	}

	for (int i=0; i<n_servers; i++){
		ServerConfig *server = topology.mutable_server(i);
		if (server->id()==id_){
			for (int j=0; j<server->neighbor_size(); j++)
				neighbors_.push_back(other_servers[server->neighbor(j)]);
			break;
		}
	}

	param_shard_ = new ParamShard(id_, topology.sync_interval());

}

void SingaServer::StartServer() {
	//Create and bind sockets
	zctx_t *context = zctx_new();
	void *frontend = zsocket_new(context, ZMQ_ROUTER);
	void *backend = zsocket_new(context, ZMQ_DEALER);
	int rc = zsocket_bind(frontend, frontend_endpoint_);
	assert(rc);
	rc = zsocket_bind(backend, backend_endpoint_);
	assert(rc == 0);

	vector<void *> neighbor_socket;
	for (int i=0; i<neighbors_.size(); i++) {
		void *socket = zsocket_new(context, ZMQ_DEALER);
		zsocket_connect(socket, neighbors_[i]);
		neighbor_socket.push_back(socket);
	}

	//create and start server threads
	for (int i=0; i<FLAGS_server_threads; i++)
		zthread_fork(context, ServerThread, this);

	//Start the loop that forwards messages between socket
	int nsockets = 2 + neighbor_socket.size();
	bool is_running = true;
	while (is_running) {
		zmq_pollitem_t items[nsockets];
		items[0] = {frontend, 0, ZMQ_POLLIN, 0};
		items[1] = {backend, 0, ZMQ_POLLIN, 0};
		for (int i = 2; i < nsockets; i++)
			items[i] = {neighbor_socket[i-2], 0, ZMQ_POLLIN, 0};

		int rc = zmq_poll(items, nsockets, -1);
		if (rc < 0)
			break;

		if (items[0].revent & ZMQ_POLLIN) {
			zmsg_t *msg = zmsg_recv(frontend);
			if (!msg)
				break;
			//send to backend
			zmsg_send(&msg, backend);
		}
		if (items[1].reevent & ZMQ_POLLIN) {
			zmsg_t *msg = zmsg_recv(backend);
			if (!msg)	break;

			//if SYNC message -> forward to all the neighbors
			//also forward to the frontend.
			zframe_t *first = zmsg_pop(msg);
			if (zframe_streq(first, SYNC_MSG)) {
				for (int i=0; i<nsockets-2; i++){
					zmsg_t *dup = zmsg_dup(msg);
					zframe_t *id = zmsg_pop(dup);
					zframe_t *tid = zmsg_pop(dup);
					zframe_t *type = zmsg_pop(dup);
					zframe_destroy(&type);
					zmsg_pushstrf(dup,"%d",kSyncRequest);
					zmsg_prepend(dup,&tid);
					zmsg_preprend(dup,&id);
					zmsg_send(&dup,neighbor_socket[i]);
				}
			}
			zframe_destroy(&first);
			msg_send(&msg, frontend);
		}

		for (int i = 2; i < nsockets; i++)
			if (items[i].revent & ZMQ_POLLIN) {
				zmsg_t *msg = zmsg_recv(neighbor_socket[i - 2]);
				if (!msg) {
					is_running = false;
					break;
				}
				zmsg_send(&msg, backend);
			}
	}

	//Stop when ^C
	zsocket_destroy(context, frontend);
	zsocket_destroy(context, backend);
	for (int i=0; i<nsockets-2; i++)
		zsocket_destroy(context, neighbor_socket[i]);
	zctx_destroy (&context);
}

void ServerThread(void *args, ztcx_t *ctx, void *pipe){
	SingaServer *server = static_cast<SingaServer*>(args);

	//create and connect socket
	void *backend = zsocket_new(ctx, ZMQ_DEALER);
	void *frontend = zsocket_new(ctx, ZMQ_DEALER);

	int rc = zsocket_connect(backend, server->backend_endpoint());
	assert(rc==0);
	rc = zsocket_connect(frontend,server->frontend_endpoint());

	//create new PMServer object
	PMServer *pmserver = new PMServer(server->id(), server->param_shard(), backend);

	//start recv loop and process requests
	int type;
	int paramId;
	while (true){
		zmsg_t *msg = zmsg_recv(backend);
		if (!msg) break;

		//process the request
		//1. pop the identity frame
		//2. pop the next string:
		// 	if equal to REQUEUED_ID -> pop 2 more frames as the worker and thread ID
		//	else it is the type
		//3. invoke PMServer to process it. Send back to the frontend socket if fail.
		//   send back to the worker if successful.
		zframe_t *identity = zmsg_pop(msg);
		zframe_t *thread_id;
		char *next_frame = zmsg_popstr(msg);
		if (memcmp(next_frame, REQUEUE_ID, strlen(REQUEUE_ID))==0){
			//requeued msg, pop another frame
			zframe_destroy(&identity);
			free(next_frame);
			identity = zmsg_pop(msg); //identity is the next frame
			thread_id = zmsg_pop(msg);
			next_frame = zmsg_popstr(msg);
			sscanf(next_frame, "%d",&type);
		}
		else{
			thread_id = zmsg_pop(msg);
			sscanf(next_frame,"%d",&type);
		}
		free(next_frame);

		sscanf(zmsg_popstr(msg),"%d",&paramId);
		zmsg_prepend(msg, &thread_id);
		zmsg_prepend(msg,&identity); //construct (<identity><worker thread id><content>) message

		zmsg_t *data;
		switch (type){
			case kPut:
				data = pmserver->HandlePut(paramId, &msg);
				break;
			case kGet:
				data = pmserver->HandleGet(paramId, &msg);
				break;
			case kUpdate:
				data = pmserver->HandleUpdate(paramId, &msg);
				break;
			case kSyncRequest:
				data = pmserver->HandleSyncRequest(paramId, &msg);
				break;
			case kSyncResponse:
				data = pmserver->HandleSyncResponse(paramId,&msg);
				break;
		}

		if (data)
			zmsg_send(&data, frontend);
	}
	zsocket_destroy(ctx,frontend);
	zsocket_destroy(ctx,backend);
}

zmsg_t* PMServer::HandlePut(int paramId, zmsg_t **msg){
	zframe_t *identity = zmsg_pop(*msg);
	zframe_t *thread_id = zmsg_pop(*msg);
	zframe_destroy(&identity);
	zframe_destroy(&thread_id);
	return (this->param_shard_->put(paramId, msg));
}

zmsg_t* PMServer::HandleGet(int paramId, zmsg_t **msg){
	zframe_t *identity = zmsg_pop(*msg);
	zframe_t *thread_id = zmsg_pop(*msg);
	zmsg_t *param = this->param_shard()->get(paramId, msg);
	if (param) {
		//repsonse of the format: <identity><type: kData><id><param content>
		zmsg_pushstrf(param, "%d", paramId);
		zmsg_pushstrf(param, "%d", kData);
		zmsg_prepend(param, &thread_id);
		zmsg_preprend(param, &identity);
		zmsg_pushstr(param, RESPONSE_HEADER);

		//send off
		zmsg_send(&param, this->socket_);
		return NULL;
	} else {
		//re-construct msg to be re-queued.
		zmsg_t *response = zmsg_new();
		zmsg_pushstrf(response, "%d", paramId);
		zmsg_pushstrf(response, "%d", kGet);
		zmsg_preprend(response, &thread_id);
		zmsg_preprend(response, &identity);
		zmsg_pushstr(response, REQUEUE_ID);
		//the calling function will send this message off
		return response;
	}
}

zmsg_t* PMServer::HandleUpdate(int paramId, zmsg_t **msg) {
	zframe_t *identity = zmsg_pop(*msg);
	zframe_t *thread_id = zmsg_pop(*msg);
	zmsg_t *content = zmsg_dup(*msg);

	zmsg_t *param = this->param_shard()->update(paramId, msg);

	if (param) {
		//repsonse of the format: <identity><type: kData><paramId><param content>
		zmsg_pushstrf(param, "%d", paramId);
		zmsg_pushstrf(param, "%d", kData);
		zmsg_preprend(param, &thread_id);
		zmsg_preprend(param, &identity);
		zmsg_pushstr(param, this->param_shard()->sync_now(paramId)?SYNC_MSG:RESPONSE_HEADER);

		//send off
		zmsg_send(&param, this->socket_);
		zmsg_destroy(&content);
		zmsg_destroy(&identity);
		zmsg_destroy(&thread_id);
		return NULL;
	} else {
		//re-construct msg to be re-queued.
		zmsg_pushstrf(content, "%d", paramId);
		zmsg_pushstrf(content, "%d", kUpdate);
		zmsg_preprend(content, &thread_id);
		zmsg_preprend(content, &identity);
		zmsg_pushstr(content, REQUEUE_ID);
		//the calling function will send this message off
		return content;
	}
}

zmsg_t* PMServer::HandleSyncRequest(int paramId, zmsg_t **msg){
	zframe_t *identity = zmsg_pop(*msg);
	zframe_t *thread_id = zmsg_pop(*msg);
	zmsg_t *content = zmsg_dup(*msg);
	zmsg_t *param = this->param_shard()->sync_update(paramId, msg);
	if (param) {
		//repsonse of the format: <identity><type: kData><id><param content>
		zmsg_pushstrf(param, "%d", paramId);
		zmsg_pushstrf(param, "%d", kSyncResponse);
		zmsg_preprend(param, &thread_id);
		zmsg_preprend(param, &identity);
		zmsg_pushstr(param, RESPONSE_HEADER);

		//send off
		zmsg_send(&param, this->socket_);
		zmsg_destroy(&content);
		return NULL;
	} else {
		//re-construct msg to be re-queued.
		zmsg_pushstrf(content, "%d", paramId);
		zmsg_pushstrf(content, "%d", kSyncRequest);
		zmsg_preprend(content, &thread_id);
		zmsg_preprend(content, &identity);
		zmsg_pushstr(content, REQUEUE_ID);
		//the calling function will send this message off
		return content;
	}
}


zmsg_t* PMServer::HandleSyncResponse(int paramId, zmsg_t **msg){
	zfarme_t *identity = zmsg_pop(*msg);

	zframe_t *thread_id = zmsg_pop(*msg);

	zmsg_t *content = zmsg_dup(*msg);
	zmsg_t *param = this->param_shard()->update(paramId, msg);
	if (param){
		zmsg_destroy(&content);
		zmsg_destroy(&identity);
		zmsg_destroy(&thread_id);
		return NULL;
	}

	zmsg_pushstrf(content, "%d", paramId);
	zmsg_pushstrf(content, "%d", kSyncRequest);
	zmsg_preprend(content, &thread_id);
	zmsg_preprend(content, &identity);
	zmsg_pushstr(content, REQUEUE_ID);
	//the calling function will send this message off
	return content;
}

} // namespace singa

