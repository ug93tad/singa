#ifndef PARAM_SERVER_H_
#define PARAM_SERVER_H_

#include <czmq.h>
#include <memory>
#include <vector>
#include <map>
#include "utils/param.h"

using std::vector;
using std::map;
using std::shared_ptr;

namespace singa{

/**
 * Parameter manager at the server side: repsonding to client's get/udpate
 * request, and periodically syncing with other servers.
 */
class PMServer{
public:
	PMServer(void *socket, vector<int> *neighbors) : socket_(socket),
	neighbors_(neighbors){}

	~PMServer();

	/**
	 * Do nothing for now, as we never send Get request.
	 * Get request is implied by Update.
	 */
	bool HandleGet(int paramId, zmsg_t* msg){}

	bool HandleUpdate(int paramId, zmsg_t* msg);


	/**
	 * Create new Param object and insert to the map
	 * using paramId as the key.
	 *
	 * msg is destroyed if successful.
	 */
	bool HandlePut(int paramId, zmsg_t *msg);

	bool Get(Param *param){return true;}
	bool Put(Param *param){ return true; }
	bool Update(Param *param){ return true;}

private:
	void *socket_; /**< send socket */
	vector<int> *neighbors_; /**< neighbors to the server to sync */
    map<int, shared_ptr<Param>> paramid2Param_; /**< map of key(int) to Param object */

};

} // namespace singa

#endif /* PARAM_SERVER_H_ */
