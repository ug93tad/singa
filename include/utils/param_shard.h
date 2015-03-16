/*
 * param_shard.h
 *
 *  Created on: 14 Mar, 2015
 *      Author: dinhtta
 */

#ifndef PARAM_SHARD_H_
#define PARAM_SHARD_H_

#include "utils/param.h"
#include <map>
using std::map;

/**
 * A shard (collection) of Param objects, to be shared between different thread (PMServer and PMClient).
 * These are local objects.
 *
 * The main thread creates this object, and it deletes it when all other threads finish.
 *
 * Note we don't do consistency here, but defer it to the Param object.
 */

namespace singa{

class ParamShard{
public:
	ParamShard(int id, int sync_interval): id_(id), sync_interval_(sync_interval){}
	~ParamShard();

	int id(){ return id_;}

	bool is_local(int paramId); /**< true if paramID is a local key */

	//all zmsg_t messages are destroyed by the Param object
	zmsg_t* get(int paramId, zmsg_t **msg); /**< local get, assuming the Param object is present */
	zmsg_t* update(int paramId, zmsg_t **msg); /**< local update, return true if sucessful */
	zmsg_t* put(int paramId, zmsg_t **msg); /**< insert to local shard */
	zmsg_t *sync_update(int paramId, zmsg_t **msg); /**< sync param with new data */

	bool sync_now(int paramId); /**< true if time to sync this param object with other servers */
private:
	int id_;
	map<int, Param*> params_;
	map<int, int> update_counters_;
	int sync_interval_;
};

} // namespace singa


#endif /* PARAM_SHARD_H_ */
