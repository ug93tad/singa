#ifndef PARAM_CLIENT_H_
#define PARAM_CLIENT_H_

#include <czmq.h>
#include <memory>
#include <vector>
#include <map>
#include <string.h>
#include "utils/param_shard.h"
#include "utils/pm_base.h"
#include "proto/topology.pb.h"

using std::string;
using std::vector;
using std::shared_ptr;

namespace singa {


enum RequestReturnType {
	NON_LOCAL, LOCAL_SUCCESS, LOCAL_FAIL
};

#define POPULATE "put"
#define WAIT "wait"

/**
 * Parameter manager at the worker side, support get/update requests from the worker.
 * Each worker thread has a PMClient object, these objects share the same ParamShard.
 */
class PMClient: public PMBase {
public:
	PMClient(int id, ParamShard *shard, void *socket) :
			PMBase(id, shard, socket) {
	}
	~PMClient();

	/**
	 * Get the parameter object with key paramId. Return getReturnType:
	 * 1. If non-local, send the message to remote server. Collect later
	 * 2. If local and HandleGet() return true -> LOCAL_SUCCESS, can use *param object
	 * 3. Local but block at HandleGet() -> LOCAL_FAIL -> to call Get() again
	 */
	int Get(int paramId, Param *param);

	/**
	 * Update operation, similar to Get.
	 */
	int Update(int paramId, Param* param);

	/**
	 * Collect a Param object returned from remote server. Return FALSE
	 * if no object is ready.
	 */
	bool Collect(Param *param);

	/**
	 * Send put request to remote server.
	 */
	void Put(int paramId, Param* param);
};

/**
 * Testing worker functionality.The main thread reads the config file and set up the socket.
 *
 * Create the shared ParamShard, then starts worker thread which basically carries out the work.
 * Each thread creates a PMClient object.
 *
 * The main thread then enter the loops to forward messages.
 *
 * Requests from the worker thread is prepend the paramId, which is stripped by the main thread
 * before forwarding to the correct server.
 *
 * The 1st thread in Client 0 populates the servers with data (PUT request). Wait
 * for a while before starting the client thread (which does get/update
 * continuously).
 */
class SingaClient {
public:
	SingaClient(int worker_id, Topology &topology, vector<string> &hosts);
	void StartClient();

	int id() {
		return id_;
	}
	ParamShard *param_shard() {
		return param_shard_;
	}
	char *backend_endpoint() {
		return backend_endpoint_;
	}

private:
	int id_, local_id_, group_id_;
	char backend_endpoint_[256];
	vector<char*> neighbors_;
	ParamShard *param_shard_;

	int param_to_server_id(int paramId);/**< mapping paramId to server ID */
};

//Zthread function for the worker thread, in the global namespace.
//Basically a loop of: compute, get, update, compute, etc.
void ClientThread(void *args, zctx_t *ctx, void *pipe);

vector<Param*> gen_random_params();
void test_get(PMClient *client);
void test_update(PMClient *client, vector<Param*> params);
void test_collect(PMClient *client);

} // namespace singa
#endif /* PARAM_SERVER_H_ */
