/*
 * param_shard.cc
 *
 *  Created on: 14 Mar, 2015
 *      Author: dinhtta
 */

#include "utils/param_shard.h"

namespace singa{

ParamShard::~ParamShard(){
	for (map<int,Param*>::iterator it = params_.begin(); it!=params_.end(); it++)
		delete(it->second);
}

zmsg_t* ParamShard::get(int paramId, zmsg_t **msg){
	map<int,Param*>::iterator it = params_.find(paramId);
	assert(it!=params_.end());

	Param* ret =  params_[paramId];
	return ret->HandleGetMsg(msg);
}

zmsg_t* ParamShard::update(int paramId, zmsg_t **msg){
	map<int,Param*>::iterator it = params_.find(paramId);
	assert(it!=params_.end());
	Param *old = params_[paramId];
	zmsg_t *response =  old->HandleUpdateMsg(msg);
	if (response)
		update_counters_[paramId]++;
	return response;
}

//same as update for now
zmsg_t* ParamShard::sync_update(int paramId, zmsg_t **msg){
	return this->update(paramId, msg);
}

zmsg_t* ParamShard::put(int paramId, zmsg_t **msg){
	//map<int,Param*>::iterator it = params_.find(paramId);
	//assert(it==params_.end());

	Param *param = new Param();
	param->HandlePutMsg(msg);

	params_[paramId] = param;
	update_counters_[paramId] = 0;
	return NULL;
}

bool ParamShard::sync_now(int paramId){
	if (update_counters_[paramId]>=this->sync_interval_){
		update_counters_[paramId] = 0;
		return true;
	}
	return false;
}

bool ParamShard::is_local(int paramId){
	map<int, Param*>::iterator it = params_.find(paramId);
	return it!=params_.end();
//	return false; 
}
} // namespace singa



