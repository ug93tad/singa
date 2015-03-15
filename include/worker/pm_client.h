#ifndef PARAM_SERVER_H_
#define PARAM_SERVER_H_

#include <czmq.h>
#include <memory>
#include <vector>
#include <map>
#include "utils/param_shard.h"

using std::vector;
using std::map;
using std::shared_ptr;

namespace singa{

/**
 * Parameter manager at the worker side, support get/update requests from the worker.
 * Each worker thread has a PMClient object, these objects share the same ParamShard.
 */
class PMClient: public PMBase{
public:
	PMClient(int id, ParamShard *shard, void *socket):PMBase(id,shard,socket);
	~PMClient();

	/**
	 * Get the parameter object with key paramId. Return true if local,
	 * else send a request to the server (in this case *param does not contain any content).
	 */
	bool Get(int paramId, Param *param);

	/**
	 * Send the update of the Param object to the server.
	 */
	void Update(int paramId, Param& param);

	/**
	 * Collect a Param object returned from remote server. Return FALSE
	 * if no object is ready.
	 */
	bool Collect(Param *param);
};

} // namespace singa

#endif /* PARAM_SERVER_H_ */
