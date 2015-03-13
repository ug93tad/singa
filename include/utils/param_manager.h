#ifndef INCLUDE_WORKER_PARAM_MANAGER_H_
#define INCLUDE_WORKER_PARAM_MANAGER_H_

#include <czmq.h>
#include <thread>
#include <mutex>
#include <condition_variable>
#include "utils/param.h"
#include "utils/router.h"
#include "utils/updater.h"
#include "worker/neuralnet.h"

#define kGradFrame 2
#define kDataFrame 4
#define kGradReady 8
#define kDataReady 16

namespace singa{

/**
 * ParamManager manages Param objects within the process.
 * It allocates the memory space for all Param objects that are used within this
 * process. It also synchronizes with parameter servers;
 *
 * This is the base class, to which ParamServer and ParamWorker extend
 *
 * TODO syn with other processes for parameters shared across procs.
 */
class ParamManager{
 public:
  /**
   * Allocate memory for local Param objects and init network settings.
   */
  ParamManager(shared_ptr<NeuralNet> net, const UpdaterProto& updater, vector<int> *neighbors=NULL);
  ~ParamManager();


  //Start added stuff by Anh

   /**
    * Blocking operation to get the parameter (from the PM that maintains it).
    */
    bool Get(Param* param);


   /**
    * Non-blocking opeartion. It passes the parameter to the PM that maintains it.
    */
    bool Put(Param* param);

   /**
    * Non-blocking opeartion for updating parameters. It may synchronize the updates to other PMs.
    */
    bool Update(Param* param);

    /**
     * Called by the worker thread
     */
    bool Collect(Param *param);


   /**
    * Process Get request. Return true if success, otherwise return false
    */
    bool HandleGet(int paramId, zmsg_t* msg);

   /**
    * processes Update requests. It returns true if success, otherwise returns false.
    */
    bool HandleUpdate(int paramId, zmsg_t* msg);


   /**
    * Process Put request. Return true if success, otherwise return false.
    */
    bool HandlePut(int paramId, zmsg_t* msg);


  //End added stuff by Anh




  /**
   * called by local worker threads;
   * can be implemented in hogwild way, i.e., done asynchornously; or in batch
   * mode, i.e., wait until all threads update for this param is ready.
   * can be done by the stub thread or the calling thread
   */
  void UpdateParam(shared_ptr<Param> param, int step, int threadid);
  /**
   * call UpdateParam to update all params used by the calling thread
   * blocked until all params are updated.
  void UpdateParams(int step, int threadid);
   */
  /**
   * will be blocked if the param is not updated.
   */
  void WaitUpdate(shared_ptr<Param> param, int step, int threadid);
  /**
    * Initialize neural network parameters and put them to
    * distributed parameter table on parameter servers.
    * @param net, neural network
    */
  void SendParamsToServers();
  /**
   * get params to run step-th iteration
   */
  void GetParamsFromServers(int step);// will be blocked until recv all parameters.
  /**
   * randomlly init allocated parameters and set them ready */
  void InitParams();

    /**
   * Poll messages and conduct updates for parameters.
   */
  void Update(int step, int threadid);
  void SyncConfig(float compute_time);

 protected:

  //Added stuff by Anh
  int Shard(int paramId); /**< the nodeID for given param ID */

  bool SyncNow(int paramId); /**< true if time to sync with a remove PM */

  //End stuff added by Anh


  bool hogwild_;
  bool running_;
  int warmup_steps_;
  float sample_ratio_, moving_rate_;
  int sync_frequency_;
  vector<int> *neighbors_;

  shared_ptr<NeuralNet> net_;
  //!< sgd updater
  shared_ptr<Updater> updater_;
  //!< a big param which allocates mem for all local params.
  shared_ptr<Param> param_;
  map<int, vector<shared_ptr<Param>>> ownerid2Params_;
  //!< aggregated updates for one param
  map<int, size_t> aggregatedUpdates_;
  map<int, int> paramid2Offset_;
  map<int, int> paramid2version_;
  map<int, shared_ptr<Param>> paramid2Param_;
  std::mutex mtx_;
  //std::condition_variable cv_;

  shared_ptr<Router> router_;
};
}
#endif // INCLUDE_WORKER_PARAM_MANAGER_H_

