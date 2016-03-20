// The Firmament project
// Copyright (c) 2016 Joshua Bambrick <jpbambrick@gmail.com>
//
// Container monitor utils.

#include "base/common.h"
#include "base/resource_vector.pb.h"
#include "misc/utils.h"
#include "misc/container_disk_usage_tracker.h"
using std::string;

namespace firmament {

class ContainerMonitorUtils {
  public:
    static void StartContainerMonitor(int port);
    static bool CreateResourceVector(
        int port, string container_monitor_host, string task_container_name,
        ResourceVector* rv);
    static bool CreateResourceVector(
        int port, string container_monitor_host, string task_container_name,
        ContainerDiskUsageTracker* disk_tracker, ResourceVector* rv);

  protected:
    static bool CreateResourceVector(
        string json_input, string task_container_name,
        ContainerDiskUsageTracker* disk_tracker, ResourceVector* rv);
    static size_t WriteCallback(void *contents,
                                size_t size,
                                size_t nmemb,
                                void *userp);
    static string GetHttpResponse(string url);
};

}  // namespace firmament
