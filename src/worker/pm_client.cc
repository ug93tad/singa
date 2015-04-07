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
#include <glog/logging.h>

DECLARE_string(topology_config);
DECLARE_int32(client_threads);

namespace singa{

//id is the global worker id
SingaClient::SingaClient(int global_id, Topology &topology, vector<string> &hosts) {
	//Read the config files and store endpoints
	id_ = global_id;

	int n_workers = hosts.size() - topology.nservers();
	int n_worker_groups = topology.nworker_groups();
	int group_size = n_workers/n_worker_groups;
	int server_group_size = topology.nservers()/topology.server_group_size();
	FLAGS_client_threads = topology.worker_threads();

	local_id_ = (id_-topology.nservers())%group_size;//local worker id.
	group_id_ = (id_-topology.nservers())/group_size;

	VLOG(3) << "Parsing client config for "<<hosts[id_];

	//connect to all server in the server group group_id_
	int start_server_idx = group_id_*server_group_size;
	int end_server_idx = start_server_idx+server_group_size;

	for (int i = start_server_idx; i < end_server_idx; i++) {
		char *neighbor_endpoint = (char*) malloc(256);
		sprintf(neighbor_endpoint, "tcp://%s:%d", hosts[i].c_str(), topology.port());
		neighbors_.push_back(neighbor_endpoint);
		VLOG(3) << "Worker neighbor (server): "<<neighbor_endpoint;
	}

	sprintf(backend_endpoint_, "inproc://singanus%d",id_);

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
		VLOG(3) << "Connected to neighbor " <<neighbors_[i];
		assert(rc==0);
		server_sockets.push_back(socket);
	}

	//Create and bind backend socket
	void *backend = zsocket_new(context, ZMQ_ROUTER);
	rc = zsocket_bind(backend, backend_endpoint_);
	assert(rc==0);

	//Start client threads
	for (int i=0; i<FLAGS_client_threads; i++){
		void * socket = zthread_fork(context, ClientThread, this);
		zmsg_t *control_msg = zmsg_new();
		if (i==0 && local_id_==0)
			zmsg_pushstr(control_msg,POPULATE);
		else
			zmsg_pushstr(control_msg, WAIT);
		zmsg_send(&control_msg, socket);
	}

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

vector<Param*> gen_random_params() {
	int size[] = { 1960000, 2500, 5000000, 2000, 3000000, 1500, 1500000, 1000, 500000, 500, 5000, 10 };
	vector<Param*> params;
	for (int i = 0; i < 12; i++) {
		ParamProto proto;
		proto.set_id(i);
		proto.set_init_method(ParamProto::kGaussain);
		Param* p = new Param();
		p->Setup(proto, vector<int> { size[i] }, 0);
		p->Init();
		params.push_back(p);
	}
	return params;
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

	//FOR TESTING ONLY. REMOVE THIS!
	//wait for control from main thread
	vector<Param*> params = gen_random_params();
	zmsg_t *control_msg = zmsg_recv(pipe);
	zframe_t *msg = zmsg_pop(control_msg);
	if (zframe_streq(msg,WAIT))
		zclock_sleep(2000); //2s
	else{
		for (int i=0; i<params.size(); i++){
			pmclient->Put(i, params[i]);
		}
		VLOG(3)<<"Done PUT requests for populating servers.";
		zclock_sleep(2000); 
	}
	zframe_destroy(&msg);
	//END TESTING
	LOG(ERROR) << "Done putting";

	//first, get the params

	test_get(pmclient);
	test_collect(pmclient);


	int iterations = 1;
	while (iterations<=200){
		VLOG(3) << "Iteration "<<iterations; 
		test_update(pmclient, params);
		test_collect(pmclient);
		iterations++;
	}

	zsocket_destroy(ctx, backend);
}

void test_get(PMClient *client){
	for (int i=0; i<12; i++){
		Param pm;
		int status = client->Get(i, &pm);
		assert(status==NON_LOCAL);
	}
}

void test_collect(PMClient *client){
	for (int i=0; i<12; i++){
		Param pm;
		int64_t start_time = zclock_time(); 
		while (!client->Collect(&pm))
			zclock_sleep(1);
		int64_t end_time = zclock_time(); 
		VLOG(3) << "Collected: " <<(end_time-start_time); 
	}
}

void test_update(PMClient *client, vector<Param*> params){
	for (int i=0; i<params.size(); i++)
		client->Update(i, params[i]);
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
			zframe_t *tmp = zmsg_pop(msg);
			zframe_destroy(&tmp);
			tmp = zmsg_pop(msg);
			zframe_destroy(&tmp);
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
			zframe_t *tmp = zmsg_pop(msg);
			zframe_destroy(&tmp);
			tmp = zmsg_pop(msg);
			zframe_destroy(&tmp);
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
		zframe_t *tmp = zmsg_pop(msg);
		zframe_destroy(&tmp);
		tmp = zmsg_pop(msg);
		zframe_destroy(&tmp);
		param->ParseToParam(&msg);
		return true;
	}
	else return false;
}
} //namespace singa



