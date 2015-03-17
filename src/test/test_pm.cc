#include <gflags/gflags.h>
#include <glog/logging.h>
#include "utils/cluster.h"
#include "utils/common.h"
#include "proto/model.pb.h"
#include "proto/cluster.pb.h"
#include "server/server.h"
#include "server/pm_server.h"
#include "worker/pm_client.h"
#include "worker/worker.h"
#include <string.h>

/**
 * Testing put/get/update performance of the new zeromq-based parameter
 * servers.
 */
DEFINE_int32(procsID, 0, "global process ID");
DEFINE_string(hostfile, "examples/imagenet12/hostfile", "hostfile");
DEFINE_string(cluster_conf, "examples/imagenet12/cluster.conf",
    "configuration file for the cluster");
DEFINE_string(model_conf, "examples/imagenet12/model.conf",
    "Deep learning model configuration file");

DEFINE_string(topology_config,"examples/imagenet12/topology.conf", "Network of servers");
DEFINE_int32(server_threads,1,"Number of server's worker threads per process");
DEFINE_int32(client_threads,1,"Number of client's worker threads per process");

DEFINE_string(mode, "client", "client or server mode");
DEFINE_int32(node_id, 0, "ID of the node, client or server");
DEFINE_int32(primary_set, 0, "ID of the primary server set (for client mode only)");

/**
 * test_pm --mode=client/server --node_id=... [--primary_set==XXX]
 *
 * primary_set is applicable only to client/worker
 */


int main(int argc, char **argv) {
  //FLAGS_logtostderr = 1;
  google::InitGoogleLogging(argv[0]);
  gflags::ParseCommandLineFlags(&argc, &argv, true);

  if (strcmp(FLAGS_mode.c_str(),"client")==0){
	  singa::SingaClient *client = new singa::SingaClient(FLAGS_node_id, FLAGS_primary_set);
	  client->StartClient();
  }
  else{
	  singa::SingaServer *server = new singa::SingaServer(FLAGS_node_id);
	  server->StartServer();
  }
  return 0;
}
