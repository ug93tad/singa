#ifndef PARAM_SERVER_H_
#define PARAM_SERVER_H_


#include <czmq.h>
#include <memory>
#include <vector>
#include <map>
#include <string.h>
#include "utils/param_shard.h"
#include "proto/topology.pb.h"
#include "utils/pm_base.h"
using std::vector;
using std::string;
using std::shared_ptr;
using google::protobuf::Message;

namespace singa{

#define REQUEUE_ID "REQUEUE" /** id of the zmsg_t message that gets requeued */
#define SYNC_MSG "SYNC"
#define RESPONSE_HEADER "REP"
/**
 * Parameter manager at the server side: repsonding to client's get/udpate
 * request, and periodically syncing with other servers.
 *
 * Each server thread has a PMServer object, but those objects share the same ParamShard object
 * created in the main thread.
 *
 * Messages processed by the PMServer object are of the format:
 * <worker id><worker thread id>  <content>
 */
class PMServer: public PMBase{
public:
	PMServer(int id, ParamShard *shard, void *socket):PMBase(id,shard,socket){}

	~PMServer();

	/**
	 * Process GET request. Return NULL if successful, in this case the response
	 * is also sent back. Non-NULL return value needed to be re-queued.
	 */
	zmsg_t* HandleGet(int paramId, zmsg_t** msg);

	/**
	 * Process Update request. Return NULL if successful: the update is applied and msg destroyed.
	 * Else, return the the request  to be re-processed.
	 */
	zmsg_t* HandleUpdate(int paramId, zmsg_t** msg);


	/**
	 * Create new Param object and insert to the map
	 * using paramId as the key. Always return NULL.
	 */
	zmsg_t* HandlePut(int paramId, zmsg_t **msg);

	/**
	 * Process Sync request from a neighbor server. Return values are the same as HandleGet/Update.
	 */
	zmsg_t* HandleSyncRequest(int paramId, zmsg_t** msg);

	/**
	 * Simply update, do not send response back.
	 */
	zmsg_t* HandleSyncResponse(int paramId, zmsg_t** msg);

};

/**
 * Testing of the new server function.
 * Read a config file of the server nework, start the socket and threads.
 *
 * One front-end socket (tcp ROUTER) is used for communicating with other servers + worker
 *
 * One back-end socket (inproc DEALER) is used to distribute messages to threads.
 *
 * Each thread has a DEALER socket to connect to the back-end, and another DEALER socket to
 * the frontend. The second socket is used to re-queue messages that fail during processing.
 *
 * Each connection to another server is a DEALER socket (connects to the remote ROUTER socket)
 *
 * The ParamShard is created in the main thread, which is then used to create different
 * ParamBase object at each server thread.
 *
 * The message received by the server is of the form:
 * <worker id><worker_thread_id><type | <REQUEUE_ID><worker id> ><paramId><content>
 *
 * from which the 2nd and 3rd are stripped before passing to the PMServer object.
 */
class SingaServer{
public:
	SingaServer(int id, Topology &topology, vector<string> &hosts); /**< Read from topology config file */
	void StartServer();
	ParamShard* param_shard(){ return param_shard_;} /** To be shared among PMBase object at each thread */
	int id(){ return id_;}
	char *backend_endpoint(){ return backend_endpoint_;}
	char *frontend_endpoint(){ return frontend_endpoint_;}
private:
	int id_;
	char frontend_endpoint_[256], backend_endpoint_[256]; /** socket addresses */
	ParamShard* param_shard_;

	vector<char*> neighbors_;
};

// Zthread function, which is in the global namespace.
void ServerThread(void *args, zctx_t *ctx, void *pipe);

} // namespace singa

#endif /* PARAM_SERVER_H_ */
