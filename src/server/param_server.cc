#include "server/pm_server.h"

namespace singa{


bool PMServer::HandleUpdate(int paramId, zmsg_t *msg){
	shared_ptr<Param> param = paramid2Param_[paramId];
	zmsg_t *updated_param = param->HandleUpdateMsg(&msg);
	if (updated_param==NULL)
		return false;
	int status = zmsg_send(&updated_param,socket_);
	return status>0;
}

bool PMServer::HandlePut(int paramId, zmsg_t *msg){
	shared_ptr<Param> param(new Param());
	zmsg_t *res = param->HandlePutMsg(&msg);
	assert(res==NULL);
	paramid2Param_[paramId] = param;
	return true;
}

}
