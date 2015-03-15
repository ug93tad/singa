/*
 * pm_base.h
 *
 *  Created on: 14 Mar, 2015
 *      Author: dinhtta
 */

#ifndef PM_BASE_H_
#define PM_BASE_H_

/**
 * Base class for parameter manager. It contains the local shard and the send/recv sockets.
 * PMClient and PMServer extend this.
 *
 * Each parameter is also identified by an id.
 */

namespace singa{
class PMBase{
public:
	PMBase(int id, ParamShard *param_shard, void *socket):
		id_(id), param_shard_(param_shard), socket_(socket){}
	~PMBase();

	ParamShard *param_shard(){ return param_shard_;}
protected:
	int id_;
	ParamShard *param_shard_;
	void *socket_;
};

} // namespace singa

#endif /* PM_BASE_H_ */
