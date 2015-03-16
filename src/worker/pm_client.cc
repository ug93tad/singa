/*
 * pm_client.cc
 *
 *  Created on: Mar 16, 2015
 *      Author: dinhtta
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include "worker/pm_client.h"
#include "gflags/gflags.h"
#include "proto/topology.pb.h"

DECLARE_string(topology_config);
DECLARE_int32(client_threads);
#include <google/protobuf/text_format.h>
#include <google/protobuf/io/zero_copy_stream_impl.h>
using namespace google::protobuf::io;
using google::protobuf::TextFormat;

namespace singa{

SingaClient::SingaClient(int id, int server_set_id) {
	//Read the config files and store endpoints
	id_ = id;

	//Read Topology message from the file
	int fd = open(FLAGS_topology_config.c_str(), O_RDONLY);
	assert(fd);
	Topology topology;
	TextFormat::Parse(new FileInputStream(fd), &topology);
	int n_servers = topology.server_size();
	map<int, char*> all_servers;

	for (int i = 0; i < n_servers; i++) {
		ServerConfig *server = topology.mutable_server(i);
		char *neighbor_endpoint = (char*) malloc(256);
		sprintf(neighbor_endpoint, "tcp://%s:%d", server->ip().c_str(),
				server->port());
		all_servers[server->id()] = neighbor_endpoint;
	}

	for (int i=0; i< topology.primary_set_size(); i++){
		ServerSet *server_set = topology.mutable_primary_set(i);
		if (server_set->id()==server_set_id){
			for (int j=0; j<server_set->neighbor_size(); j++)
				neighbors_.push_back(all_servers[server_set->neighbor(j)]);
			break;
		}
	}
	sprintf(backend_endpoint_, "inproc://singanus:%d",id_);

	//Create shared paramshard
	param_shard_ = new ParamShard(id_,0);
}

void SingaClient::StartClient(){
	//Create and connect sockets to the server
	vector<void *> server_sockets;
	zctx_t *context = zctx_new();
	int nservers = neighbors_.size();
	int rc;
	for (int i=0; i<nservers; i++){
		void *socket = zsocket_new(context, ZMQ_DEALER);
		rc = zsocket_connect(socket, neighbors_[i]);
		assert(rc==0);
		server_sockets.push_back(socket);
	}

	//Create and bind backend socket
	void *backend = zsocket_new(context, ZMQ_ROUTER);
	rc = zsocket_bind(backend, backend_endpoint_);
	assert(rc==0);

	//Start clien threads
	for (int i=0; i<FLAGS_client_threads; i++)
		zthread_fork(context, ClientThread, this);

	//Star the message loop
	bool is_running = true;
	int nsockets= nservers+1;
	while (is_running) {
		zmq_pollitem_t items[nsockets];
		for (int i = 0; i < nsockets-1; i++)
			items[i] = {server_sockets[i], 0, ZMQ_POLLIN, 0};
		items[nsockets-1] = {backend, 0, ZMQ_POLLIN, 0};

		int rc = zmq_poll(items,nsockets,-1);
		if (rc<0) break;

		for (int i=0; i<nsockets-1; i++){
			if (items[i].revents & ZMQ_POLLIN){
				zmsg_t *msg = zmsg_recv(server_sockets[i]);
				if (!msg){
					is_running = false;
					break;
				}
				//forward to backend
				zmsg_send(&msg, backend);
			}
		}
		if (items[nsockets-1].revents & ZMQ_POLLIN){
			//compute serverId from paramId and forward to the socket
			zmsg_t *msg = zmsg_recv(backend);
			if (!msg) is_running=false;
			zframe_t *identity = zmsg_pop(msg);
			zframe_t *type = zmsg_pop(msg);
			int paramId;
			sscanf(zmsg_popstr(msg), "%d", &paramId);
			zmsg_pushstrf(msg,"%d",paramId);
			zmsg_prepend(msg,&type);
			zmsg_prepend(msg,&identity);
			zmsg_send(&msg, server_sockets[param_to_server_id(paramId)]);
		}
	}

	zsocket_destroy(context, backend);
	for (int i=0; i<nsockets-1; i++)
		zsocket_destroy(context, server_sockets[i]);
	zctx_destroy(&context);
}

//simple mapping
int SingaClient::param_to_server_id(int paramId){
	return paramId % neighbors_.size();
}

void ClientThread(void *args, zctx_t *ctx, void *pipe){
	SingaClient *client = static_cast<SingaClient*>(args);

	//Create back-end socket and connect to the main thread
	void *backend = zsocket_new(ctx, ZMQ_DEALER);
	int rc = zsocket_connect(backend, client->backend_endpoint());
	assert(rc==0);

	//Create PMClient object
	PMClient *pmclient = new PMClient(client->id(), client->param_shard(), backend);

	while (1){
		//do job here, compute/get/update/compute/... sequence
	}
	zsocket_destroy(ctx, backend);
}

void PMClient::Put(int paramId, Param *param){
	zmsg_t *data = param->ParseToMsg();
	zmsg_pushstrf(data,"%d",paramId);
	zmsg_pushstrf(data,"%d",kPut);
	zmsg_send(&data, this->socket_);
}

int PMClient::Get(int paramId, Param *param){
	if (!this->param_shard_->is_local(paramId)){
		zmsg_t *msg = zmsg_new();
		zmsg_pushstrf(msg, "%d",paramId);
		zmsg_pushstrf(msg, "%d", kGet);
		zmsg_send(&msg, this->socket_);
		return NON_LOCAL;
	}
	else{
		zmsg_t *msg = this->param_shard_->get(paramId, NULL);
		if (msg){
			param->ParseToParam(&msg);
			return LOCAL_SUCCESS;
		}
		else
			return LOCAL_FAIL;
	}
}

int PMClient::Update(int paramId, Param *param){
	if (!this->param_shard_->is_local(paramId)) {
		zmsg_t *msg = param->ParseToMsg();
		zmsg_pushstrf(msg, "%d", paramId);
		zmsg_pushstrf(msg, "%d", kUpdate);
		zmsg_send(&msg, this->socket_);
		return NON_LOCAL;
	} else {
		zmsg_t *msg = this->param_shard_->update(paramId, NULL);
		if (msg) {
			param->ParseToParam(&msg);
			return LOCAL_SUCCESS;
		} else
			return LOCAL_FAIL;
	}
}

bool PMClient::Collect(Param* param){
	zmq_pollitem_t items[] = {{this->socket_, 0, ZMQ_POLLIN, 0}};
	int rc = zmq_poll(items,1,0);
	if (rc<0) return false;

	if (items[0].revents & ZMQ_POLLIN){
		zmsg_t *msg = zmsg_recv(this->socket_);
		param->ParseToParam(&msg);
		return true;
	}
	else return false;
}
} //namespace singa



