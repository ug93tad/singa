/*
 * pm_server.cc
 *
 *  Created on: 14 Mar, 2015
 *      Author: dinhtta
 */

#include <gflags/gflags.h>
#include <glog/logging.h>
#include <string.h>
#include <czmq.h>
#include <stdlib.h>
#include "server/pm_server.h"
#include "proto/topology.pb.h"
#include <vector>

using std::vector;

DECLARE_string(topology_config);
DECLARE_int32(server_threads);

namespace singa{

SingaServer::SingaServer(int id, Topology &topology, vector<string> &hosts){
	id_ = id;

	VLOG(3) << "Parsing config file for host "<<hosts[id_] << " server id = " <<id_;
	int n_servers = topology.nservers();
	int n_server_groups = topology.server_group_size();
	int port = topology.port();
	int group_size = n_servers/n_server_groups;
	FLAGS_server_threads = topology.server_threads();

	map<int, char*> other_servers;

	sprintf(frontend_endpoint_, "tcp://%s:%d", hosts[id_].c_str(), port);
	sprintf(backend_endpoint_, "inproc://singanus%d", id_);

	int local_id_ = id_%group_size; //will connect to the same local id of its neighbor group
	int neighbor_group_id = 0;
	int sync_interval=0;
	for (int i=0; i<n_server_groups; i++){
		ServerGroup *group = topology.mutable_server_group(i);
		if (group->id()==(id_/group_size)){//the group where this server belongs
			sync_interval = group->sync_interval();
			neighbor_group_id = group->neighbor(0);//assume only one neighbor

			char *neighbor_endpoint = (char*) malloc(256);
			sprintf(neighbor_endpoint, "tcp://%s:%d",
					hosts[neighbor_group_id * group_size + local_id_].c_str(), port);
			neighbors_.push_back(neighbor_endpoint); 
			VLOG(3) << "Neighboring server is " << neighbor_endpoint;
			break;
		}
	}

	param_shard_ = new ParamShard(id_, sync_interval);

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

		if (items[0].revents & ZMQ_POLLIN) {
			zmsg_t *msg = zmsg_recv(frontend);
			if (!msg)
				break;

			//send to backend
			zmsg_send(&msg, backend);
		}
		if (items[1].revents & ZMQ_POLLIN) {
			zmsg_t *msg = zmsg_recv(backend);
			if (!msg)
				break;
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
					zframe_destroy(&id);
					zmsg_send(&dup,neighbor_socket[i]);
				}
			}
			zframe_destroy(&first);
			zmsg_send(&msg, frontend);
		}

		for (int i = 2; i < nsockets; i++)
			if (items[i].revents & ZMQ_POLLIN) {
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

void ServerThread(void *args, zctx_t *ctx, void *pipe){
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
		zframe_t *thread_id = zmsg_pop(msg);
		if (memcmp(zframe_data(thread_id), REQUEUE_ID, strlen(REQUEUE_ID))==0){
			//requeued msg, pop another frame
			zframe_destroy(&identity);
			zframe_destroy(&thread_id); 
			identity = zmsg_pop(msg); //identity is the next frame
			thread_id = zmsg_pop(msg);
		}
		char *typestr = zmsg_popstr(msg);
		sscanf(typestr, "%d",&type);
		free(typestr); 
		
		char *param_frame = zmsg_popstr(msg); 
		sscanf(param_frame,"%d",&paramId);
		free(param_frame); 		

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
				VLOG(3)<<"Handle SYNC-REQUEST"; 
				data = pmserver->HandleSyncRequest(paramId, &msg);
				break;
			case kSyncResponse:
				VLOG(3) << "Handle SYNC response";  
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
		zmsg_prepend(param, &identity);
		zmsg_pushstr(param, RESPONSE_HEADER);

		//send off
		zmsg_send(&param, this->socket_);
		return NULL;
	} else {
		//re-construct msg to be re-queued.
		zmsg_t *response = zmsg_new();
		zmsg_pushstrf(response, "%d", paramId);
		zmsg_pushstrf(response, "%d", kGet);
		zmsg_prepend(response, &thread_id);
		zmsg_prepend(response, &identity);
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
		zmsg_prepend(param, &thread_id);
		zmsg_prepend(param, &identity);

		zmsg_pushstr(param, this->param_shard()->sync_now(paramId)?SYNC_MSG:RESPONSE_HEADER);

		//send off
		zmsg_send(&param, this->socket_);
		zmsg_destroy(&content);
		zframe_destroy(&identity);
		zframe_destroy(&thread_id);
		return NULL;
	} else {
		//re-construct msg to be re-queued.
		zmsg_pushstrf(content, "%d", paramId);
		zmsg_pushstrf(content, "%d", kUpdate);
		zmsg_prepend(content, &thread_id);
		zmsg_prepend(content, &identity);
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
		zmsg_prepend(param, &thread_id);

		//push 2 identity frame, one stripped by the local router, another
		//by the DEALER at the other side
		zframe_t *id_dup = zframe_dup(identity);
		zmsg_prepend(param, &identity);
		zmsg_prepend(param,&id_dup);

		zmsg_pushstr(param, RESPONSE_HEADER);

		//send off
		zmsg_send(&param, this->socket_);
		zmsg_destroy(&content);
		return NULL;
	} else {
		//re-construct msg to be re-queued.
		zmsg_pushstrf(content, "%d", paramId);
		zmsg_pushstrf(content, "%d", kSyncRequest);
		zmsg_prepend(content, &thread_id);
		zmsg_prepend(content, &identity);
		zmsg_pushstr(content, REQUEUE_ID);
		//the calling function will send this message off
		return content;
	}
}


zmsg_t* PMServer::HandleSyncResponse(int paramId, zmsg_t **msg){
	zframe_t *identity = zmsg_pop(*msg);

	zframe_t *thread_id = zmsg_pop(*msg);

	zmsg_t *content = zmsg_dup(*msg);
	zmsg_t *param = this->param_shard()->update(paramId, msg);
	if (param){
		zmsg_destroy(&content);
		zframe_destroy(&identity);
		zframe_destroy(&thread_id);
		zmsg_destroy(&param); 
		return NULL;
	}

	zmsg_pushstrf(content, "%d", paramId);
	zmsg_pushstrf(content, "%d", kSyncRequest);
	zmsg_prepend(content, &thread_id);
	zmsg_prepend(content, &identity);
	zmsg_pushstr(content, REQUEUE_ID);
	//the calling function will send this message off
	return content;
}

} // namespace singa


