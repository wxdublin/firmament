// The Firmament project
// Copyright (c) 2011-2013 Ionel Gog <ionel.gog@cl.cam.ac.uk>
//
// Simple wrapper to poll information from ProcFS.

#ifndef FIRMAMENT_PLATFORMS_UNIX_PROCFS_MACHINE_H
#define FIRMAMENT_PLATFORMS_UNIX_PROCFS_MACHINE_H

#include <vector>

#include "base/machine_perf_statistics_sample.pb.h"
#include "platforms/unix/common.h"

namespace firmament {
namespace platform_unix {

typedef struct {
  uint64_t user;
  uint64_t nice;
  uint64_t system;
  uint64_t idle;
  uint64_t iowait;
  uint64_t irq;
  uint64_t soft_irq;
  uint64_t steal;
  uint64_t guest;
  uint64_t guest_nice;
  uint64_t total;
  time_t systime;
} CPUStatistics_t;

typedef struct {
  uint64_t read;
  uint64_t write;
} DiskStatistics_t;

typedef struct {
  uint64_t capacity_space;
  uint64_t free_space;
  uint64_t available_space;
} DiskSpaceStatistics_t;

typedef struct {
  uint64_t mem_total;
  uint64_t mem_free;
  uint64_t mem_buffers;
  uint64_t mem_pagecache;
} MemoryStatistics_t;

typedef struct {
  uint64_t send;
  uint64_t recv;
} NetworkStatistics_t;

class ProcFSMachine {
 public:
  ProcFSMachine();
  const MachinePerfStatisticsSample* CreateStatistics(
    MachinePerfStatisticsSample* stats);
  void GetMachineCapacity(ResourceVector* cap);

 private:
  vector<CPUStatistics_t> GetCPUStats();
  vector<CpuUsage> GetCPUUsage();
  DiskStatistics_t GetDiskStats();
  DiskSpaceStatistics_t GetDiskSpaceStats();
  MemoryStatistics_t GetMemoryStats();
  NetworkStatistics_t GetNetworkStats();

  inline void readunsigned(FILE* input, uint64_t *x) {
    fscanf(input, "%ju ", x);
  }

  vector<CPUStatistics_t> cpu_stats_;
  DiskStatistics_t disk_stats_;
  NetworkStatistics_t net_stats_;
};

}  // namespace platform_unix
}  // namespace firmament

#endif  // FIRMAMENT_PLATFORMS_UNIX_PROCFS_MACHINE_H
