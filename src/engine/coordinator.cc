// The Firmament project
// Copyright (c) 2011-2012 Malte Schwarzkopf <malte.schwarzkopf@cl.cam.ac.uk>
//
// Platform-independent coordinator class implementation. This is subclassed by
// the platform-specific coordinator classes.

#include "engine/coordinator.h"

#include <set>
#include <string>
#include <utility>

#ifdef __PLATFORM_HAS_BOOST__
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#endif

#include <google/protobuf/descriptor.h>

#include "base/resource_desc.pb.h"
#include "base/resource_topology_node_desc.pb.h"
#include "base/task_final_report.pb.h"
#include "base/task_perf_statistics_sample.pb.h"
#include "engine/health_monitor.h"
#include "messages/base_message.pb.h"
#include "messages/storage_registration_message.pb.h"
#include "messages/storage_message.pb.h"
#include "messages/task_state_message.pb.h"
#include "messages/task_heartbeat_message.pb.h"
#include "misc/pb_utils.h"
#include "misc/protobuf_envelope.h"
#include "misc/map-util.h"
#include "misc/container_monitor.h"
#include "misc/utils.h"
#include "scheduling/flow/flow_scheduler.h"
#include "scheduling/knowledge_base.h"
#include "scheduling/simple/simple_scheduler.h"
#include "storage/simple_object_store.h"

// It is necessary to declare listen_uri here, since "node.o" comes after
// "coordinator.o" in linking order (I *think*).
DECLARE_string(listen_uri);
DECLARE_bool(enable_resource_reservation_decay);
DEFINE_string(parent_uri, "", "The URI of the parent coordinator to register "
        "with.");
DEFINE_bool(include_local_resources, true, "Add local machine's resources; "
            "will instantiate a resource-less coordinator if false.");
DEFINE_string(scheduler, "simple", "Scheduler to use: one of 'simple' or "
              "'flow'.");
DECLARE_int32(container_monitor_port);
#ifdef __HTTP_UI__
DEFINE_bool(http_ui, true, "Enable HTTP interface");
DEFINE_int32(http_ui_port, 8080,
        "The port that the HTTP UI will be served on; -1 to disable.");
#endif
DECLARE_uint64(heartbeat_interval);
DEFINE_int64(overreserved_wait_period, 10, "How many checks that find "
             "reservations exceed capacity before tasks are pre-empted.");
DEFINE_uint64(monitor_resource_usage_interval, 10000000,
              "Monitor resources interval in microseconds.");
DEFINE_bool(populate_knowledge_base_from_file, false,
            "True if we should load the knowledge base from file.");
DEFINE_bool(determine_unused_resources, false,
            "True if we should send unused resource information to parent.");
DEFINE_bool(log_child_unused_resources, false,
            "True if we should output childrens' unused resource information.");
DEFINE_bool(preempt_by_start_time, true,
            "True if we should sort pre-empted tasks by start time first.");
DEFINE_bool(reschedule_preempted_tasks, true,
            "True if we should reschedule tasks that have been pre-empted.");


namespace firmament {

Coordinator::Coordinator(PlatformID platform_id)
  : Node(platform_id,
      GenerateResourceID(
        boost::asio::ip::host_name() + "/" + FLAGS_listen_uri)),
    associated_resources_(new ResourceMap_t),
    local_resource_topology_(new ResourceTopologyNodeDescriptor),
    job_table_(new JobMap_t),
    task_table_(new TaskMap_t),
    killed_tasks_to_reschedule_(new set<TaskDescriptor*>),
    topology_manager_(new TopologyManager()),
    object_store_(new store::SimpleObjectStore(uuid_)),
    parent_chan_(NULL),
    hostname_(boost::asio::ip::host_name()) {
  // Start up a coordinator according to the platform parameter
  string desc_name = "Coordinator on " + hostname_;
  resource_desc_.set_uuid(to_string(uuid_));
  resource_desc_.set_friendly_name(desc_name);
  resource_desc_.set_type(ResourceDescriptor::RESOURCE_COORDINATOR);
  //resource_desc_.set_storage_engine(object_store_->get_listening_interface());
  local_resource_topology_->mutable_resource_desc()->CopyFrom(
      resource_desc_);

  shared_ptr<KnowledgeBase> knowledge_base(new KnowledgeBase());
  // Set up the scheduler
  if (FLAGS_scheduler == "simple") {
    // Simple random first-available scheduler
    LOG(INFO) << "Using simple random scheduler.";
    scheduler_ = new SimpleScheduler(
        job_table_, associated_resources_, local_resource_topology_,
        object_store_, task_table_, knowledge_base, topology_manager_,
        m_adapter_, NULL, uuid_, FLAGS_listen_uri);
  } else if (FLAGS_scheduler == "flow") {
    // Quincy-style flow-based scheduling
    LOG(INFO) << "Using Quincy-style min cost flow-based scheduler.";
    scheduler_ = new FlowScheduler(
        job_table_, associated_resources_, local_resource_topology_,
        object_store_, task_table_, knowledge_base, topology_manager_,
        m_adapter_, NULL, uuid_, FLAGS_listen_uri);
  } else {
    // Unknown scheduler specified, error.
    LOG(FATAL) << "Unknown or unrecognized scheduler '" << FLAGS_scheduler
               << " specified on coordinator command line!";
  }

  // Log information
  LOG(INFO) << "Coordinator starting on host " << FLAGS_listen_uri
          << ", platform " << platform_id << ", uuid " << uuid_;
  //LOG(INFO) << "Storage Engine is listening on interface : "
  //          << object_store_->get_listening_interface();
  switch (platform_id) {
      case PL_UNIX:
      {
          break;
      }
      default:
          LOG(FATAL) << "Unimplemented!";
  }

  // Start health monitor thread (won't have much to do initially)
  health_monitor_thread_ =
    new boost::thread(boost::bind(&HealthMonitor::Run, health_monitor_,
                                  scheduler_, associated_resources_));

  pull_task_messages_thread_
    = new boost::thread(boost::bind(&Coordinator::PullTaskMessages, this));

  resource_monitor_thread_
    = new boost::thread(boost::bind(&Coordinator::MonitorResourceUsage, this));

  if (FLAGS_populate_knowledge_base_from_file) {
    scheduler_->knowledge_base()->LoadKnowledgeBaseFromFile();
  }
}

Coordinator::~Coordinator() {
  // TODO(malte): check destruction order in C++; c_http_ui_ may already
  // have been destructed when we get here.
  /*#ifdef __HTTP_UI__
    if (FLAGS_http_ui && c_http_ui_)
      c_http_ui_->Shutdown(false);
  #endif*/
}

bool Coordinator::RegisterWithCoordinator(
    StreamSocketsChannel<BaseMessage>* chan) {
  BaseMessage bm;
  ResourceDescriptor* rd = bm.mutable_registration()->mutable_res_desc();
  rd->CopyFrom(resource_desc_); // copies current local RD!
  ResourceTopologyNodeDescriptor* rtnd =
      bm.mutable_registration()->mutable_rtn_desc();
  rtnd->CopyFrom(*local_resource_topology_);
  SUBMSG_WRITE(bm, registration, uuid, to_string(uuid_));
  SUBMSG_WRITE(bm, registration, location, chan->LocalEndpointString());
  // wrap in envelope
  VLOG(2) << "Sending registration message...";
  // send heartbeat message
  return SendMessageToRemote(chan, &bm);
}

void Coordinator::DetectLocalResources() {
  // Inform the user about the number of local PUs.
  uint64_t num_local_pus = topology_manager_->NumProcessingUnits();
  LOG(INFO) << "Found " << num_local_pus << " local PUs.";
  LOG(INFO) << "Resource URI is " << node_uri_;
  // Get local resource topology and save it to the topology protobuf
  // TODO(malte): Figure out how this interacts with dynamically added
  // resources; currently, we only run detection (i.e. the DetectLocalResources
  // method) once at startup.
  ResourceTopologyNodeDescriptor* root_node =
      local_resource_topology_->add_children();
  topology_manager_->AsProtobuf(root_node);
  root_node->set_parent_id(to_string(uuid_));
  BFSTraverseResourceProtobufTreeReturnRTND(
      local_resource_topology_,
      boost::bind(&Coordinator::AddResource, this, _1, node_uri_, true));
}

void Coordinator::AddResource(ResourceTopologyNodeDescriptor* rtnd,
                              const string& endpoint_uri,
                              bool local) {
  CHECK_NOTNULL(rtnd);
  ResourceDescriptor* resource_desc = rtnd->mutable_resource_desc();
  CHECK_NOTNULL(resource_desc);
  // Compute resource ID
  ResourceID_t res_id = ResourceIDFromString(resource_desc->uuid());
  // Add resource to local resource set
  VLOG(1) << "Adding resource " << res_id << " to resource map; "
          << "endpoint URI is " << endpoint_uri;
  CHECK(InsertIfNotPresent(associated_resources_.get(), res_id,
          new ResourceStatus(resource_desc, rtnd, endpoint_uri,
                             GetCurrentTimestamp())));
  // Record the machine UUID if this is the local topology
  if (resource_desc->type() == ResourceDescriptor::RESOURCE_MACHINE &&
      local) {
    // Store the machine UUID (different from the coordinator resource UUID,
    // which is stored in uuid_).
    machine_uuid_ = ResourceIDFromString(resource_desc->uuid());
    // Figure out the machine's resource capacity.
    ResourceVector* cap = resource_desc->mutable_resource_capacity();
    machine_monitor_.GetMachineCapacity(cap);
    machine_capacity_ = *cap;
    ResourceVector machine_reservations;
    scheduler_->knowledge_base()->UpdateMachineReservations(
        machine_uuid_, machine_reservations);
  }
  // Register with scheduler if this resource is schedulable
  if (resource_desc->type() == ResourceDescriptor::RESOURCE_PU) {
    // TODO(malte): We make the assumption here that any local PU resource is
    // exclusively owned by this coordinator, and set its state to IDLE if it is
    // currently unknown. If coordinators were to ever shared PUs, we'd need
    // something more clever here.
    resource_desc->set_schedulable(true);
    if (resource_desc->state() == ResourceDescriptor::RESOURCE_UNKNOWN)
      resource_desc->set_state(ResourceDescriptor::RESOURCE_IDLE);
    scheduler_->RegisterResource(res_id, local);
    VLOG(1) << "Added " << (local ? "local" : "remote") << " resource "
            << resource_desc->uuid()
            << " [" << resource_desc->friendly_name()
            << "] to scheduler.";
  }
}

void Coordinator::Run() {
  // Test topology detection
  LOG(INFO) << "Detecting resource topology:";
  topology_manager_->DebugPrintRawTopology();
  CreateContainerMonitor();
  if (FLAGS_include_local_resources) {
    // Add the local processing resources
    DetectLocalResources();
  } else {
    // We still add a node: a "virtual" resource representing this coordinator
    AddResource(local_resource_topology_, node_uri_, true);
  }

  // Coordinator starting -- set up and wait for workers to connect.
  m_adapter_->ListenURI(FLAGS_listen_uri);
  m_adapter_->RegisterAsyncMessageReceiptCallback(
          boost::bind(&Coordinator::HandleIncomingMessage, this, _1, _2));
  m_adapter_->RegisterAsyncErrorPathCallback(
          boost::bind(&Coordinator::HandleIncomingReceiveError, this,
          boost::asio::placeholders::error, _2));

#ifdef __HTTP_UI__
  InitHTTPUI();
#endif

  // Do we have a parent? If so, register with it now.
  if (FLAGS_parent_uri != "") {
    parent_chan_ =
        new StreamSocketsChannel<BaseMessage > (
            StreamSocketsChannel<BaseMessage>::SS_TCP);
    CHECK(ConnectToRemote(FLAGS_parent_uri, parent_chan_))
        << "Failed to connect to parent at " << FLAGS_parent_uri << "!";
    VLOG(1) << "Establishing channel with parent coordinator: "
            << parent_chan_->LocalEndpointString() << " -> "
            << parent_chan_->RemoteEndpointString();
    RegisterWithCoordinator(parent_chan_);
    parent_uri_ = FLAGS_parent_uri;
  }

  uint64_t cur_time = 0;
  uint64_t last_heartbeat_time = 0;
  // Main loop
  while (!exit_) {
    // Wait for events (i.e. messages from workers.
    // TODO(malte): we need to think about any actions that the coordinator
    // itself might need to take, and how they can be triggered
    VLOG(3) << "Hello from main loop!";
    AwaitNextMessage();
    // TODO(malte): wrap this in a timer
    cur_time = GetCurrentTimestamp();
    if (cur_time - last_heartbeat_time > FLAGS_heartbeat_interval) {
      MachinePerfStatisticsSample stats;
      stats.set_timestamp(GetCurrentTimestamp());
      stats.set_resource_id(to_string(machine_uuid_));

      machine_monitor_.CreateStatistics(&stats);
      const ResourceVector* machine_reservations =
          scheduler_->knowledge_base()->GetMachineReservations(machine_uuid_);

      if (machine_reservations) {
        ResourceVector* stats_resource_reservations = stats.mutable_resource_reservations();
        stats_resource_reservations->CopyFrom(*machine_reservations);
      }

      if (FLAGS_determine_unused_resources) {
        DetermineUnusedResources(
            stats.mutable_unused_reserved_resources(),
            stats.mutable_unused_capacity_resources());
      }

      // Record this sample locally
      scheduler_->knowledge_base()->AddMachineSample(stats);
      if (parent_chan_ != NULL) {
        SendHeartbeatToParent(stats);
      }
      last_heartbeat_time = cur_time;
    }
  }

  // We have dropped out of the main loop and are exiting
  // TODO(malte): any cleanup we need to do; hand-over to another coordinator if
  // possible?
  Shutdown("dropped out of main loop");
}

void Coordinator::DetermineUnusedResources(
    ResourceVector* unused_reserved_resources,
    ResourceVector* unused_capacity_resources) {
  CHECK_NOTNULL(unused_reserved_resources);
  CHECK_NOTNULL(unused_capacity_resources);

  unused_capacity_resources->CopyFrom(machine_capacity_);

  for(auto& task_entry : *task_table_) {
    const TaskPerfStatisticsSample* task_stats =
        scheduler_->knowledge_base()->GetLatestStatsForTask(task_entry.first);
    TaskDescriptor* td_ptr = task_entry.second;
    CHECK_NOTNULL(td_ptr);
    if (td_ptr->has_scheduled_to_resource()
        && td_ptr->state() == TaskDescriptor::RUNNING
        && !td_ptr->has_delegated_to()
        && task_stats
        && task_stats->has_resources()
        && (td_ptr->has_resource_reservations()
            || td_ptr->has_resource_request())) {
      const ResourceVector& reservation =
          td_ptr->has_resource_reservations()
              ? td_ptr->resource_reservations()
              : td_ptr->resource_request();
      if (unused_capacity_resources->ram_cap()
          > task_stats->resources().ram_cap()) {
        unused_capacity_resources->set_ram_cap(
            unused_capacity_resources->ram_cap()
            - task_stats->resources().ram_cap());
      } else {
        unused_capacity_resources->set_ram_cap(0);
      }
      if (reservation.ram_cap() > task_stats->resources().ram_cap()) {
        unused_reserved_resources->set_ram_cap(
          unused_reserved_resources->ram_cap()
          + reservation.ram_cap()
          - task_stats->resources().ram_cap());
      }
      if (unused_capacity_resources->disk_bw()
          > task_stats->resources().disk_bw()) {
        unused_capacity_resources->set_disk_bw(
            unused_capacity_resources->disk_bw()
            - task_stats->resources().disk_bw());
      } else {
        unused_capacity_resources->set_disk_bw(0);
      }
      if (reservation.disk_bw() > task_stats->resources().disk_bw()) {
        unused_reserved_resources->set_disk_bw(
          unused_reserved_resources->disk_bw()
          + reservation.disk_bw()
          - task_stats->resources().disk_bw());
      }
      if (unused_capacity_resources->disk_cap()
          > task_stats->resources().disk_cap()) {
        unused_capacity_resources->set_disk_cap(
            unused_capacity_resources->disk_cap()
            - task_stats->resources().disk_cap());
      } else {
        unused_capacity_resources->set_disk_cap(0);
      }
      if (reservation.disk_cap() > task_stats->resources().disk_cap()) {
        unused_reserved_resources->set_disk_cap(
          unused_reserved_resources->disk_cap()
          + reservation.disk_cap()
          - task_stats->resources().disk_cap());
      }
    }
  }
}

JobDescriptor* Coordinator::DescriptorForJob(const string& job_id) {
  JobID_t job_uuid = JobIDFromString(job_id);
  JobDescriptor *jd = FindOrNull(*job_table_, job_uuid);
  return jd;
}

bool Coordinator::HasJobCompleted(const JobDescriptor& jd) {
  queue<TaskID_t> q;
  q.push(jd.root_task().uid());
  while (!q.empty()) {
    TaskID_t cur_task_id = q.front();
    q.pop();
    TaskDescriptor* td = FindPtrOrNull(*task_table_, cur_task_id);
    CHECK_NOTNULL(td);
    if (!(td->state() == TaskDescriptor::COMPLETED ||
          td->state() == TaskDescriptor::ABORTED)) {
      LOG(INFO) << "Job " << jd.uuid() << " has not yet completed, as task "
                << td->uid() << " is in state "
                << ENUM_TO_STRING(TaskDescriptor::TaskState, td->state());
      return false;
    }
    // TODO(malte): this needs to take a copy or a lock, otherwise we can race
    // with concurrent spawns.
    for (RepeatedPtrField<TaskDescriptor>::const_iterator it =
         td->spawned().begin();
         it != td->spawned().end();
         it++) {
      q.push(it->uid());
    }
  }
  LOG(INFO) << "Job " << jd.uuid() << " has completed.";
  return true;
}

void Coordinator::MonitorResourceUsage() {
  while (true) {
    usleep(FLAGS_monitor_resource_usage_interval);
    bool reservation_too_high = false;
    const ResourceVector* machine_reservations =
          scheduler_->knowledge_base()->GetMachineReservations(machine_uuid_);

    if (machine_reservations) {
      ResourceVector reservations_to_free;

      if (machine_reservations->ram_cap() > machine_capacity_.ram_cap()) {
        reservations_to_free.set_ram_cap(
            machine_reservations->ram_cap() - machine_capacity_.ram_cap());
        reservation_too_high = true;
      }

      if (machine_reservations->disk_bw() > machine_capacity_.disk_bw()) {
        reservations_to_free.set_disk_bw(
            machine_reservations->disk_bw() - machine_capacity_.disk_bw());
        reservation_too_high = true;
      }

      if (machine_reservations->disk_cap() > machine_capacity_.disk_cap()) {
        reservations_to_free.set_disk_cap(
            machine_reservations->disk_cap() - machine_capacity_.disk_cap());
        reservation_too_high = true;
      }

      if (reservation_too_high) {
        reservation_exceeded_counter_++;
        LOG(INFO) << "Machine reservation too high count: "
                  << reservation_exceeded_counter_;
        uint64_t wait_period = FLAGS_overreserved_wait_period;
        if (reservation_exceeded_counter_ == wait_period) {
          LOG(INFO) << "Machine reservation too high count, freeing resources";
          FreeResources(reservations_to_free);
          reservation_too_high = false;
        } else if (FLAGS_overreserved_wait_period == -1) {
          reservation_too_high = false;
        }
      }
    }

    if (!reservation_too_high) {
      reservation_exceeded_counter_ = 0;
    }

    if (FLAGS_log_child_unused_resources) {
      ResourceVector unused_reserved_resources_sum;
      ResourceVector unused_capacity_resources_sum;
      for (auto& res_item : *associated_resources_) {
        const ResourceDescriptor& res_desc = res_item.second->descriptor();
        MachinePerfStatisticsSample stats;
        if (scheduler_->knowledge_base()->GetLatestStatsForMachine(
                ResourceIDFromString(res_desc.uuid()), &stats)) {
          if (stats.has_unused_reserved_resources()) {
            VLOG(1) << "Unused reserved resources on machine "
                    << res_desc.uuid() << ": "
                    << stats.unused_reserved_resources().ram_cap () << " / "
                    << stats.unused_reserved_resources().disk_bw () << " / "
                    << stats.unused_reserved_resources().disk_cap ();
            unused_reserved_resources_sum.set_ram_cap(
                unused_reserved_resources_sum.ram_cap()
                + stats.unused_reserved_resources().ram_cap());
            unused_reserved_resources_sum.set_disk_bw(
                unused_reserved_resources_sum.disk_bw()
                + stats.unused_reserved_resources().disk_bw());
            unused_reserved_resources_sum.set_disk_cap(
                unused_reserved_resources_sum.disk_cap()
                + stats.unused_reserved_resources().disk_cap());
          }
          if (stats.has_unused_capacity_resources()) {
            VLOG(1) << "Unused capacity resources on machine "
                    << res_desc.uuid() << ": "
                    << stats.unused_capacity_resources().ram_cap () << " / "
                    << stats.unused_capacity_resources().disk_bw () << " / "
                    << stats.unused_capacity_resources().disk_cap ();
            unused_capacity_resources_sum.set_ram_cap(
                unused_capacity_resources_sum.ram_cap()
                + stats.unused_capacity_resources().ram_cap());
            unused_capacity_resources_sum.set_disk_bw(
                unused_capacity_resources_sum.disk_bw()
                + stats.unused_capacity_resources().disk_bw());
            unused_capacity_resources_sum.set_disk_cap(
                unused_capacity_resources_sum.disk_cap()
                + stats.unused_capacity_resources().disk_cap());
          }
        }
      }
      VLOG(1) << "Child unused reserved resource: "
              << unused_reserved_resources_sum.ram_cap() << " / "
              << unused_reserved_resources_sum.disk_bw() << " / "
              << unused_reserved_resources_sum.disk_cap();
      VLOG(1) << "Child unused capacity resource: "
              << unused_capacity_resources_sum.ram_cap() << " / "
              << unused_capacity_resources_sum.disk_bw() << " / "
              << unused_capacity_resources_sum.disk_cap();
    }
  }
}

void Coordinator::FreeResources(ResourceVector resources_to_free) {
  auto comp = [&resources_to_free](TaskDescriptor* a, TaskDescriptor* b) {
    if (a->priority() != b->priority())
      return a->priority() > b->priority() ? true : false;

    if (a->has_start_time() && b->has_start_time() && FLAGS_preempt_by_start_time) {
      return a->start_time() < b->start_time() ? true : false;
    }

    if (!a->has_resource_reservations() || !b->has_resource_reservations()) {
      return !a->has_resource_reservations() ? true : false;
    }
    // the order in which resource types are considered is arbitrary
    ResourceVector a_reservations = a->resource_reservations();
    ResourceVector b_reservations = b->resource_reservations();
    if (resources_to_free.has_ram_cap()) {
      uint64_t a_ram = a_reservations.ram_cap();
      uint64_t b_ram = b_reservations.ram_cap();
      if (a_ram != b_ram) {
        return a_ram < b_ram ? true : false;
      }
    }

    if (resources_to_free.has_disk_bw()) {
      uint64_t a_disk_bw = a_reservations.disk_bw();
      uint64_t b_disk_bw = b_reservations.disk_bw();
      if (a_disk_bw != b_disk_bw) {
        return a_disk_bw < b_disk_bw ? true : false;
      }
    }

    if (resources_to_free.has_disk_cap()) {
      uint64_t a_disk_cap = a_reservations.disk_cap();
      uint64_t b_disk_cap = b_reservations.disk_cap();
      if (a_disk_cap != b_disk_cap) {
        return a_disk_cap < b_disk_cap ? true : false;
      }
    }

    // The two resources are equal in every way.
    return true;
  };

  priority_queue<TaskDescriptor*, vector<TaskDescriptor*>, decltype(comp)>
      machine_running_task_descs(comp);
  for(thread_safe::map<TaskID_t, TaskDescriptor*>::iterator it
          = task_table_->begin();
      it != task_table_->end(); it++) {
    TaskDescriptor* td_ptr = it->second;
    if (td_ptr->has_scheduled_to_resource()) {
      ResourceID_t task_machine_res_id = scheduler_->MachineResIDForResource(
                      ResourceIDFromString(td_ptr->scheduled_to_resource()));
      if (td_ptr->state() == TaskDescriptor::RUNNING
          && task_machine_res_id == machine_uuid_) {
        machine_running_task_descs.push(td_ptr);
      }

    }
  }

  ResourceVector resource_freed;
  while (machine_running_task_descs.size() > 0
      && (resource_freed.ram_cap() < resources_to_free.ram_cap()
          || resource_freed.disk_bw() < resources_to_free.disk_bw()
          || resource_freed.disk_cap() < resources_to_free.disk_cap())) {
    TaskDescriptor* lowest_task_desc = machine_running_task_descs.top();
    resource_freed.set_ram_cap(
        resource_freed.ram_cap()
        + lowest_task_desc->resource_reservations().ram_cap());
    resource_freed.set_disk_bw(
        resource_freed.disk_bw()
        + lowest_task_desc->resource_reservations().disk_bw());
    resource_freed.set_disk_cap(
        resource_freed.disk_cap()
        + lowest_task_desc->resource_reservations().disk_cap());

    LOG(INFO) << "Freeing resources by aborting task: "
              << lowest_task_desc->uid();
    if (FLAGS_reschedule_preempted_tasks) {
      killed_tasks_to_reschedule_->insert(lowest_task_desc);

      delay_task_state_change_.insert(lowest_task_desc->uid());
    }
    scheduler_->KillRunningTask(lowest_task_desc->uid(),
                                TaskKillMessage::RESOURCE_EXCEEDED);
    if (FLAGS_reschedule_preempted_tasks) {
      delay_task_state_change_.erase(lowest_task_desc->uid());
    }

    machine_running_task_descs.pop();
  }
}

void Coordinator::PullTaskMessages() {
  uint64_t last_pull_time = 0;
  while (true) {
    uint64_t cur_time = GetCurrentTimestamp();
    if (cur_time - last_pull_time > FLAGS_heartbeat_interval) {
      VLOG(1) << "Pulling messages from tasks";
      vector<TaskStateMessage> states = scheduler_->CreateTaskStateChanges();
      for (vector<TaskStateMessage>::iterator it = states.begin();
           it != states.end();
           ++it) {
        if (delay_task_state_change_.find(it->id())
            == delay_task_state_change_.end()) {
          HandleTaskStateChange(*it);
        } else {
          delayed_state_changes_.push_back(*it);
        }
      }

      deque<TaskStateMessage> keep_state_changes;
      for (auto& state_change : delayed_state_changes_) {
        if (delay_task_state_change_.find(state_change.id())
            == delay_task_state_change_.end()) {
          HandleTaskStateChange(state_change);
        } else {
          keep_state_changes.push_back(state_change);
        }
      }

      delayed_state_changes_ = keep_state_changes;

      vector<TaskHeartbeatMessage> heartbeats =
          scheduler_->CreateTaskHeartbeats();
      for (vector<TaskHeartbeatMessage>::iterator it = heartbeats.begin();
           it != heartbeats.end();
           ++it) {
        HandleTaskHeartbeat(*it);
      }
      last_pull_time = cur_time;
    }
    usleep(10);
  }
}

void Coordinator::HandleIncomingMessage(BaseMessage *bm,
                                        const string& remote_endpoint) {
  uint32_t handled_extensions = 0;
  // Registration message
  if (bm->has_registration()) {
    const RegistrationMessage& msg = bm->registration();
    HandleRegistrationRequest(msg);
    handled_extensions++;
  }
  // Resource Heartbeat message
  if (bm->has_heartbeat()) {
    const HeartbeatMessage& msg = bm->heartbeat();
    HandleHeartbeat(msg);
    handled_extensions++;
  }
  // Task heartbeat message
  if (bm->has_task_heartbeat()) {
    const TaskHeartbeatMessage& msg = bm->task_heartbeat();
    HandleTaskHeartbeat(msg);
    handled_extensions++;
  }
  // Task state change message
  if (bm->has_task_state()) {
    const TaskStateMessage& msg = bm->task_state();
    HandleTaskStateChange(msg);
    handled_extensions++;
  }
  // Task spawn message
  if (bm->has_task_spawn()) {
    const TaskSpawnMessage& msg = bm->task_spawn();
    HandleTaskSpawn(msg);
    handled_extensions++;
  }
  // Task info request message
  if (bm->has_task_info_request()) {
    const TaskInfoRequestMessage& msg = bm->task_info_request();
    HandleTaskInfoRequest(msg, remote_endpoint);
    handled_extensions++;
  }
  // Task delegation message
  if (bm->has_task_delegation_request()) {
    const TaskDelegationRequestMessage& msg = bm->task_delegation_request();
    HandleTaskDelegationRequest(msg, remote_endpoint);
    handled_extensions++;
  }
  // Task delegation request message (at delegatee coordinator)
  if (bm->has_task_delegation_response()) {
    const TaskDelegationResponseMessage& msg = bm->task_delegation_response();
    HandleTaskDelegationResponse(msg, remote_endpoint);
    handled_extensions++;
  }
  // Task kill message
  if (bm->has_task_kill()) {
    const TaskKillMessage& msg = bm->task_kill();
    if (!KillRunningTask(msg.task_id(), msg.reason())) {
      LOG(ERROR)  << "Failed to kill task " << msg.task_id() << "!";
    }
    handled_extensions++;
  }
  // Task final report
  // TODO(malte): The TaskFinalReport protobuf lives in the wrong place (base
  // instead of messages). Move over.
  if (bm->has_task_final_report()) {
    const TaskFinalReport& msg = bm->task_final_report();
    TaskDescriptor *td_ptr = FindPtrOrNull(*task_table_, msg.task_id());
    CHECK_NOTNULL(td_ptr);
    scheduler_->HandleTaskFinalReport(msg, td_ptr);
    handled_extensions++;
  }
  // DIOS syscall: create message
  if (bm->has_create_request()) {
    const CreateRequest& msg = bm->create_request();
    HandleCreateRequest(msg, remote_endpoint);
    handled_extensions++;
  }
  // DIOS syscall: lookup message
  if (bm->has_lookup_request()) {
    const LookupRequest& msg = bm->lookup_request();
    HandleLookupRequest(msg, remote_endpoint);
    handled_extensions++;
  }
  // DIOS I/O notification
  if (bm->has_end_write_notification()) {
    HandleIONotification(*bm, remote_endpoint);
    handled_extensions++;
  }
  // Check that we have handled at least one sub-message
  if (handled_extensions == 0)
    LOG(ERROR) << "Ignored incoming message, no known extension present, "
               << "so cannot handle it: " << bm->DebugString();
}

void Coordinator::HandleIncomingReceiveError(
    const boost::system::error_code& error,
    const string& remote_endpoint) {
  // Notify of receive error
  if (error.value() == boost::asio::error::eof) {
    // Connection terminated, handle accordingly
    LOG(INFO) << "Connection to " << remote_endpoint << " closed.";
    // XXX(malte): Need to figure out if this relates to a resource, and if so,
    // if we should declare it failed; or whether this is an expected task
    // completion.
  } else {
    LOG(WARNING) << "Failed to complete a message receive cycle from "
                 << remote_endpoint << ". The message was discarded, or the "
                 << "connection failed (error: " << error.message() << ", "
                 << "code " << error.value() << ").";
  }
}



void Coordinator::HandleCreateRequest(const CreateRequest& msg,
                                      const string& remote_endpoint) {
  // Try to insert the reference descriptor conveyed into the object table.
  ReferenceDescriptor* new_rd = new ReferenceDescriptor;
  new_rd->CopyFrom(msg.reference());
  new_rd->set_producing_task(msg.task_id());
  bool succ = !object_store_->AddReference(
      DataObjectIDFromProtobuf(msg.reference().id()), new_rd);
  // Manufacture and send a response
  BaseMessage resp_msg;
  SUBMSG_WRITE(resp_msg, create_response, name, msg.reference().id());
  SUBMSG_WRITE(resp_msg, create_response, success, succ);
  m_adapter_->SendMessageToEndpoint(remote_endpoint, resp_msg);
}

void Coordinator::HandleHeartbeat(const HeartbeatMessage& msg) {
  boost::uuids::string_generator gen;
  boost::uuids::uuid uuid = gen(msg.uuid());
  ResourceStatus* rsp = FindPtrOrNull(*associated_resources_, uuid);
  if (!rsp) {
      LOG(WARNING) << "HEARTBEAT from UNKNOWN resource (uuid: "
              << msg.uuid() << ")!";
  } else {
      VLOG(1) << "HEARTBEAT from resource " << msg.uuid()
              << " (last seen at " << rsp->last_heartbeat() << ")";
      if (msg.has_load())
        VLOG(2) << "Remote resource stats: " << msg.load().ShortDebugString();
      // Update timestamp
      rsp->set_last_heartbeat(GetCurrentTimestamp());
      // Record resource statistics sample
      scheduler_->knowledge_base()->AddMachineSample(msg.load());
      if (msg.has_load() && msg.load().has_resource_reservations()) {
        scheduler_->knowledge_base()->UpdateMachineReservations(
            ResourceIDFromString(msg.load().resource_id()),
            msg.load().resource_reservations());
      }
  }
}

void Coordinator::HandleIONotification(const BaseMessage& bm,
                                       const string& remote_uri) {
  if (bm.has_end_write_notification()) {
    const EndWriteNotification msg = bm.end_write_notification();
    DataObjectID_t id = DataObjectIDFromProtobuf(msg.reference().id());
    set<ReferenceInterface*>* refs = object_store_->GetReferences(id);
    vector<ReferenceInterface*> remove;
    vector<ConcreteReference*> add;
    for (set<ReferenceInterface*>::iterator it = refs->begin();
         it != refs->end();
         ++it) {
      if ((*it)->desc().type() == ReferenceDescriptor::FUTURE &&
          (*it)->desc().producing_task() == msg.reference().producing_task()) {
        // Upgrade to a concrete reference
        VLOG(2) << "Found future reference for " << id
                << ", upgrading to concrete!";
        remove.push_back(*it);
        // XXX(malte): skanky, skanky...
        add.push_back(new ConcreteReference(
            *dynamic_cast<FutureReference*>(*it)));  // NOLINT
      }
    }
    VLOG(1) << "Found " << remove.size() << " matching references for "
            << id << ", and converted them into concrete refs.";
    TaskDescriptor** td_ptr = FindOrNull(*task_table_,
                                         msg.reference().producing_task());
    CHECK_NOTNULL(td_ptr);
    for (uint64_t i = 0; i < remove.size(); ++i) {
      refs->erase(remove[i]);
      refs->insert(add[i]);
      scheduler_->HandleReferenceStateChange(*remove[i], *add[i], *td_ptr);
      delete remove[i];
    }
    // Call into scheduler, as this change may have made things runnable
    JobDescriptor* jd = DescriptorForJob((*td_ptr)->job_id());
    scheduler_->ScheduleJob(jd, NULL);
  }
}

void Coordinator::HandleLookupRequest(const LookupRequest& msg,
                                      const string& remote_endpoint) {
  // Check if the name requested exists in the object table, and return all
  // reference descriptors for it if so.
  // XXX(malte): This currently returns a single reference; we should return
  // multiple if they exist.
  set<ReferenceInterface*>* refs = object_store_->GetReferences(
      DataObjectIDFromProtobuf(msg.name()));
  // Manufacture and send a response
  BaseMessage resp_msg;
  if (refs && refs->size() > 0) {
    for (set<ReferenceInterface*>::const_iterator ref_iter = refs->begin();
         ref_iter != refs->end();
         ++ref_iter) {
      ReferenceDescriptor* resp_rd =
          resp_msg.mutable_lookup_response()->add_references();
      resp_rd->CopyFrom((*ref_iter)->desc());
    }
  }
  m_adapter_->SendMessageToEndpoint(remote_endpoint, resp_msg);
}

void Coordinator::HandleRegistrationRequest(
    const RegistrationMessage& msg) {
  boost::uuids::string_generator gen;
  boost::uuids::uuid uuid = gen(msg.uuid());
  ResourceStatus** rdp = FindOrNull(*associated_resources_, uuid);
  if (!rdp) {
    LOG(INFO) << "REGISTERING NEW RESOURCE (uuid: " << msg.uuid() << ")";
    // N.B.: below creates a new resource descriptor
    //ResourceDescriptor* rd = new ResourceDescriptor(msg.res_desc());
    // Insert the root of the registered topology into the topology tree
    ResourceTopologyNodeDescriptor* rtnd =
        local_resource_topology_->add_children();
    rtnd->CopyFrom(msg.rtn_desc());
    rtnd->set_parent_id(resource_desc_.uuid());
    // Recursively add its child resources to resource map and topology tree
    BFSTraverseResourceProtobufTreeReturnRTND(
        rtnd, boost::bind(&Coordinator::AddResource, this, _1,
                          msg.location(), false));
    //InformStorageEngineNewResource(rd);
  } else {
    LOG(INFO) << "REGISTRATION request from resource " << msg.uuid()
              << " that we already know about. "
              << "Checking if this is a recovery.";
    // TODO(malte): Implement checking logic, deal with recovery case
    // Update timestamp (registration request is an implicit heartbeat)
    (*rdp)->set_last_heartbeat(GetCurrentTimestamp());
  }
}

void Coordinator::HandleTaskHeartbeat(const TaskHeartbeatMessage& msg) {
  TaskID_t task_id = msg.task_id();
  TaskDescriptor* tdp = FindPtrOrNull(*task_table_, task_id);
  if (!tdp) {
    LOG(WARNING) << "HEARTBEAT from UNKNOWN task (ID: "
                 << task_id << ")!";
  } else {
    VLOG(1) << "HEARTBEAT from task " << task_id;
    // Remember the current location from which this task reports
    tdp->set_last_heartbeat_location(msg.location());
    // Remember the heartbeat time
    tdp->set_last_heartbeat_time(GetCurrentTimestamp());
    // Process the profiling information submitted by the task, add it to
    // the knowledge base
    scheduler_->knowledge_base()->AddTaskSample(msg.stats());
  }

  if (msg.has_resource_reservations()) {
    tdp->mutable_resource_reservations()->CopyFrom(
        msg.resource_reservations());
  }

  // If we have a parent coordinator on whose behalf we are managing this task,
  // forward the message.
  // TODO(malte): this does not currently perform any aggregation, so we're
  // likely to DoS the parent coordinator on a large deployment. Instead, we
  // should only selectively forward heartbeats and aggregate them.
  if (parent_chan_ != NULL) {
    BaseMessage bm;
    TaskHeartbeatMessage* heartbeat = bm.mutable_task_heartbeat();
    heartbeat->CopyFrom(msg);
    if (!msg.has_resource_reservations()
        && tdp->has_resource_reservations()) {
      heartbeat->mutable_resource_reservations()->CopyFrom(
          tdp->resource_reservations());
    }
    if (!SendMessageToRemote(parent_chan_, &bm)) {
      LOG(ERROR) << "Failed to forward heartbeat to parent coordinator!";
      // Try to re-register
      RegisterWithCoordinator(parent_chan_);
    }
  } else {
    scheduler_->knowledge_base()->AddTaskSample(msg.stats());
  }
}

void Coordinator::HandleTaskDelegationRequest(
    const TaskDelegationRequestMessage& msg,
    const string& remote_endpoint) {
  boost::unique_lock<boost::mutex> delegation_lock(task_delegation_mutex_);

  VLOG(1) << "Handling requested delegation of task "
          << msg.task_descriptor().uid() << " from resource "
          << msg.delegating_resource_id();
  // Check if there is room for this task here
  TaskDescriptor* td = new TaskDescriptor(msg.task_descriptor());

  bool result = HaveUnreservedMachineCapacity(td->resource_request());
  if (result) {
    result = scheduler_->PlaceDelegatedTask(
        td, ResourceIDFromString(msg.target_resource_id()));
  }
  // Return ACK/NACK
  BaseMessage response;
  SUBMSG_WRITE(response, task_delegation_response, task_id, td->uid());
  if (result) {
    // Successfully placed
    VLOG(1) << "Succeeded, task placed on resource " << msg.target_resource_id()
            << "!";
    SUBMSG_WRITE(response, task_delegation_response, success, true);
  } else {
    // Failure; delegator needs to try again
    VLOG(1) << "Failed to place!";
    SUBMSG_WRITE(response, task_delegation_response, success, false);
    delete td;
  }
  m_adapter_->SendMessageToEndpoint(remote_endpoint, response);
}

void Coordinator::HandleTaskDelegationResponse(
    const TaskDelegationResponseMessage& msg,
    const string& remote_endpoint) {
  TaskDescriptor* td = FindPtrOrNull(*task_table_, msg.task_id());
  CHECK_NOTNULL(td);
  if (msg.success()) {
    LOG(INFO) << "Task delegation for " << msg.task_id() << " to "
              << remote_endpoint << " succeeded!";
    // Confirm that we've successfully started the task remotely
    td->set_state(TaskDescriptor::DELEGATED);
    td->set_delegated_to(remote_endpoint);
  } else {
    LOG(WARNING) << "Task delegation for " << msg.task_id() << " to "
                 << remote_endpoint << " FAILED. Trying again to schedule.";
    ResourceStatus* rsp = FindPtrOrNull(*associated_resources_,
        ResourceIDFromString(td->scheduled_to_resource()));
    scheduler_->HandleTaskEviction(td, rsp->mutable_descriptor());
    scheduler_->RescheduleTask(td);
  }
}

void Coordinator::HandleTaskInfoRequest(const TaskInfoRequestMessage& msg,
                                        const string& remote_endpoint) {
  // Send response: the task descriptor if the task is known to this
  // coordinator
  VLOG(1) << "Resource " << msg.requesting_resource_id()
          << " requests task information for " << msg.task_id();
  TaskDescriptor* task_desc_ptr = FindPtrOrNull(*task_table_, msg.task_id());
  CHECK_NOTNULL(task_desc_ptr);
  // Remember the current location of this task
  task_desc_ptr->set_last_heartbeat_location(remote_endpoint);
  BaseMessage resp;
  // XXX(malte): ugly hack!
  SUBMSG_WRITE(resp, task_info_response, task_id, msg.task_id());
  resp.mutable_task_info_response()->
      mutable_task_desc()->CopyFrom(*task_desc_ptr);
  m_adapter_->SendMessageToEndpoint(remote_endpoint, resp);
}

void Coordinator::HandleTaskSpawn(const TaskSpawnMessage& msg) {
  VLOG(1) << "Handling task spawn for new task "
          << msg.spawned_task_desc().uid() << ", child of "
          << msg.creating_task_id();
  // Get the descriptor for the spawning task
  TaskDescriptor** spawner;
  CHECK(spawner = FindOrNull(*task_table_, msg.creating_task_id()));
  // Extract new task descriptor from the message received
  //TaskDescriptor* spawnee = new TaskDescriptor;
  TaskDescriptor* spawnee = (*spawner)->add_spawned();
  spawnee->CopyFrom(msg.spawned_task_desc());
  InsertIfNotPresent(task_table_.get(), spawnee->uid(), spawnee);
  // Extract job ID (we expect it to be set)
  CHECK(msg.spawned_task_desc().has_job_id());
  JobID_t job_id = JobIDFromString(msg.spawned_task_desc().job_id());
  // Update references with producing task, if necessary
  // TODO(malte): implement this properly; below is a hack that delegates
  // outputs by simple modifying their producing task.
  for (RepeatedPtrField<ReferenceDescriptor>::const_iterator o_iter =
       msg.spawned_task_desc().outputs().begin();
       o_iter != msg.spawned_task_desc().outputs().end();
       ++o_iter) {
    set<ReferenceInterface*>* refs =
        object_store_->GetReferences(DataObjectIDFromProtobuf(o_iter->id()));
    for (set<ReferenceInterface*>::iterator r_iter = refs->begin();
         r_iter != refs->end();
         ++r_iter) {
      if ((*r_iter)->desc().producing_task() == (*spawner)->uid())
        (*r_iter)->set_producing_task(spawnee->uid());
    }
  }
  // Run the scheduler for this job
  JobDescriptor* job = FindOrNull(*job_table_, job_id);
  CHECK_NOTNULL(job);
  uint64_t tasks_scheduled = scheduler_->ScheduleJob(job, NULL);
  LOG(INFO) << "Scheduled " << tasks_scheduled << " tasks for job "
            << job_id;
}

void Coordinator::HandleTaskStateChange(
    const TaskStateMessage& msg) {

  LOG(INFO) << "Task " << msg.id() << " now in state "
            << ENUM_TO_STRING(TaskDescriptor::TaskState, msg.new_state())
            << ".";
  TaskDescriptor* td_ptr = FindPtrOrNull(*task_table_, msg.id());
  CHECK(td_ptr) << "Received task state change message for task "
                << msg.id();
  if (td_ptr->state() == TaskDescriptor::FAILED ||
      td_ptr->state() == TaskDescriptor::ABORTED) {
    LOG(ERROR) << "Spurious task state change: Task  " << msg.id() << " has "
               << "already failed or aborted, but we received a "
               << "TaskStateChangeMessage for it!";
    return;
  } else if (td_ptr->state() == TaskDescriptor::COMPLETED) {
    LOG(ERROR) << "Spurious task state change: Task  " << msg.id() << " has "
               << "already completed, but we received a "
               << "TaskStateChangeMessage for it!";
    return;
  }
  // Update the task's state
  td_ptr->set_state(msg.new_state());
  switch (msg.new_state()) {
    case TaskDescriptor::COMPLETED:
    case TaskDescriptor::ABORTED:
      HandleTaskCompletion(msg, td_ptr);
      break;
    case TaskDescriptor::FAILED:
      // Set the task to "failed" state and deal with the consequences
      scheduler_->HandleTaskFailure(td_ptr);
      // Fall through into default case, which sends message to
      // parent if required
    default:
      VLOG(1) << "Task " << msg.id() << "'s state changed to "
              << static_cast<uint64_t> (msg.new_state());
      // Check if this is a delegated task, and forward the message if so
      if (td_ptr->has_delegated_from()) {
        BaseMessage bm;
        bm.mutable_task_state()->CopyFrom(msg);
        m_adapter_->SendMessageToEndpoint(td_ptr->delegated_from(), bm);
      }
      break;
  }
  // Do not run scheduler if delegated
  if (td_ptr->has_delegated_from()) {
    return;
  }

  // This task state change may have caused the job to have schedulable tasks
  // TODO(malte): decide if we should do invoke the scheduler here, or kick off
  // the scheduling iteration from within the earlier handler call into the
  // scheduler
  JobDescriptor* jd = DescriptorForJob(td_ptr->job_id());
  scheduler_->ScheduleJob(jd, NULL);
  // XXX(malte): tear down the respective connection, cleanup
}

void Coordinator::HandleTaskCompletion(const TaskStateMessage& msg,
                                       TaskDescriptor* td_ptr) {
  bool reschedule_task = false;
  if (killed_tasks_to_reschedule_->find(td_ptr)
      != killed_tasks_to_reschedule_->end()) {
    killed_tasks_to_reschedule_->erase(td_ptr);
    if (msg.new_state() != TaskDescriptor::COMPLETED) {
      reschedule_task = true;
      if (!td_ptr->has_delegated_from()) {
        scheduler_->RescheduleTask(td_ptr);
        return;
      }
    }
  } else if (msg.has_report()
             && msg.report().has_reschedule()
             && msg.report().reschedule()) {
    ResourceStatus* rsp = FindPtrOrNull(*associated_resources_,
        ResourceIDFromString(td_ptr->scheduled_to_resource()));
    scheduler_->HandleTaskEviction(td_ptr, rsp->mutable_descriptor());
    scheduler_->RescheduleTask(td_ptr);
    return;
  }

  TaskFinalReport report(msg.report());
  if (msg.new_state() == TaskDescriptor::COMPLETED || !reschedule_task) {
    // Report will be filled in if the task is local (currently)
    scheduler_->HandleTaskCompletion(td_ptr, &report);
  } else {
    // Report will be otherwise empty (tells the delegator to reschedule)
    report.set_task_id(td_ptr->uid());
    scheduler_->HandleTaskAbortion(td_ptr);
    report.set_reschedule(reschedule_task);
  }

  // First check if this is a delegated task, and forward the message if so
  if (td_ptr->has_delegated_from()) {
    BaseMessage bm;
    bm.mutable_task_state()->CopyFrom(msg);
    // Send along completion statistics to the coordinator as well.
    // TODO(malte): Make this all const including handle task completion method
    TaskFinalReport *sending_rep = bm.mutable_task_state()->mutable_report();
    sending_rep->CopyFrom(report);
    sending_rep->set_task_id(td_ptr->uid());
    m_adapter_->SendMessageToEndpoint(td_ptr->delegated_from(), bm);
  } else {
    // Check if this is the last task in the job to complete; if so, the job
    // has completed. This only needs to happen on the delegating coordinator,
    // who is responsible for maintaining the job information. Subordinate
    // delegatees only know about their respective tasks.
    JobDescriptor* jd = DescriptorForJob(td_ptr->job_id());
    CHECK_NOTNULL(jd);
    if (HasJobCompleted(*jd)) {
      LOG(INFO) << "Job " << jd->uuid() << " has completed!";
      scheduler_->HandleJobCompletion(JobIDFromString(jd->uuid()));
    }
  }
  if (reschedule_task) {
    // Delete task descriptor to avoid confusion if the task rescheduled to
    // this machine by the delegator
    task_table_->erase(td_ptr->uid());
  } else if (report.has_task_id()) {
    // Process the final report locally
    scheduler_->HandleTaskFinalReport(report, td_ptr);
  }
}

#ifdef __HTTP_UI__
void Coordinator::InitHTTPUI() {
  // Start up HTTP interface
  if (FLAGS_http_ui && FLAGS_http_ui_port > 0) {
    // TODO(malte): This is a hack to avoid failure of shared_from_this()
    // because we do not have a shared_ptr to this object yet. Not sure if this
    // is safe, though.... (I think it is, as long as the Coordinator's main()
    // still holds a shared_ptr to the Coordinator).
    //shared_ptr<Coordinator> dummy(this);
    c_http_ui_.reset(new CoordinatorHTTPUI(shared_from_this()));
    c_http_ui_->Init(FLAGS_http_ui_port);
  }
}
#endif

bool Coordinator::KillRunningJob(JobID_t job_id) {
  // Check if this is a local job
  JobDescriptor* jd = FindOrNull(*job_table_, job_id);
  if (!jd) {
    LOG(ERROR) << "Tried to kill unknown job " << job_id;
    return false;
  }
  LOG(INFO) << "Termination of job " << job_id << " requested.";
  // Iterate over all tasks in the job and kill them
  queue<TaskID_t> q;
  q.push(jd->root_task().uid());
  while (!q.empty()) {
    TaskID_t cur_task_id = q.front();
    q.pop();
    TaskDescriptor* td = FindPtrOrNull(*task_table_, cur_task_id);
    CHECK_NOTNULL(td);
    // TODO(malte): this needs to take a copy or a lock, otherwise we can race
    // with concurrent spawns.
    for (RepeatedPtrField<TaskDescriptor>::const_iterator it =
         td->spawned().begin();
         it != td->spawned().end();
         it++) {
      q.push(it->uid());
    }
    // Now kill it
    if (td->state() == TaskDescriptor::RUNNING) {
      LOG(INFO) << "Killing task " << td->uid();
      KillRunningTask(cur_task_id, TaskKillMessage::USER_ABORT);
    } else if (td->state() == TaskDescriptor::RUNNABLE ||
               td->state() == TaskDescriptor::BLOCKING) {
      td->set_state(TaskDescriptor::ABORTED);
    }
  }
  jd->set_state(JobDescriptor::ABORTED);
  return true;
}


bool Coordinator::KillRunningTask(TaskID_t task_id,
                                  TaskKillMessage::TaskKillReason reason) {
  // Check if this is a local task
  TaskDescriptor* td_ptr = FindPtrOrNull(*task_table_, task_id);
  if (!td_ptr) {
    LOG(ERROR) << "Tried to kill unknown task " << task_id;
    return false;
  } else if (!td_ptr->has_delegated_to() &&
             (!td_ptr->has_last_heartbeat_location() ||
              td_ptr->last_heartbeat_location().empty())) {
    LOG(ERROR) << "Tried to kill task " << task_id << " at unknown location";
    return false;
  }
  // Check if we have a bound resource for the task and if it is marked as
  // running
  ResourceID_t* rid = scheduler_->BoundResourceForTask(task_id);
  if (!rid || !(td_ptr->state() == TaskDescriptor::RUNNING ||
                td_ptr->state() == TaskDescriptor::DELEGATED)) {
    LOG(ERROR) << "Task " << task_id << " is not running locally, "
               << "so cannot kill it!";
    return false;
  }
  // Manufacture the message
  BaseMessage bm;
  SUBMSG_WRITE(bm, task_kill, task_id, task_id);
  SUBMSG_WRITE(bm, task_kill, reason, reason);
  // Send the message -- either directly or via delegation path
  if (td_ptr->has_delegated_to()) {
    LOG(INFO) << "Forwarding KILL message to task " << task_id << " via "
              << "coordinator at " << td_ptr->delegated_to();
    m_adapter_->SendMessageToEndpoint(td_ptr->delegated_to(), bm);
  } else {
    // Kill local tasks via the scheduler
    scheduler_->KillRunningTask(task_id, reason);
  }
  return true;
}

void Coordinator::AddJobsTasksToTables(TaskDescriptor* td, JobID_t job_id) {
  // Set job ID field on task. We do this here since we've only just generated
  // the job ID in the job submission, which passes it in.
  td->set_job_id(to_string(job_id));
  // Set the submission timestamp for the task.
  td->set_submit_time(GetCurrentTimestamp());
  // Insert task into task table
  VLOG(1) << "Adding task " << td->uid() << " to task table.";
  if (!InsertIfNotPresent(task_table_.get(), td->uid(), td)) {
    VLOG(1) << "Task " << td->uid() << " already exists in "
            << "task table, so not adding it again.";
    TaskDescriptor* existing_td = FindPtrOrNull(*task_table_, td->uid());
    CHECK_NOTNULL(existing_td);
    CHECK_EQ(existing_td->state(), TaskDescriptor::COMPLETED);
    InsertOrUpdate(task_table_.get(), td->uid(), td);
  }
  // Adds its outputs to the object table and generate future references for
  // them.
  for (RepeatedPtrField<ReferenceDescriptor>::iterator output_iter =
       td->mutable_outputs()->begin();
       output_iter != td->mutable_outputs()->end();
       ++output_iter) {
    // First set the producing task field on the task outputs
    output_iter->set_producing_task(td->uid());
    DataObjectID_t output_id(DataObjectIDFromProtobuf(output_iter->id()));
    VLOG(1) << "Considering task output " << output_id << ", "
            << "adding to local object table";
    if (object_store_->AddReference(output_id, &(*output_iter))) {
      VLOG(1) << "Output " << output_id << " already exists in "
              << "local object table. Not adding again.";
    }
    // Check that the object was actually stored
    set<ReferenceInterface*>* refs = object_store_->GetReferences(output_id);
    if (refs && refs->size() > 0)
      VLOG(3) << "Object is indeed in object store";
    else
      VLOG(3) << "Error: Object is not in object store";
  }
  // Process children recursively
  uint64_t i = 0;
  for (RepeatedPtrField<TaskDescriptor>::iterator task_iter =
       td->mutable_spawned()->begin();
       task_iter != td->mutable_spawned()->end();
       ++task_iter) {
    // Tasks that are submitted with the job also do not have a task ID to begin
    // with (unlike those dynamically spawned)
    if (task_iter->uid() == 0)
      task_iter->set_uid(GenerateTaskID(*td, i));
    AddJobsTasksToTables(&(*task_iter), job_id);
    ++i;
  }
}

void Coordinator::SendHeartbeatToParent(
    const MachinePerfStatisticsSample& stats) {
  BaseMessage bm;
  // TODO(malte): we do not always need to send the location string; it
  // sufficies to send it if our location changed (which should be rare).
  SUBMSG_WRITE(bm, heartbeat, uuid, to_string(uuid_));
  SUBMSG_WRITE(bm, heartbeat, location, node_uri_);
  SUBMSG_WRITE(bm, heartbeat, capacity,
               topology_manager_->NumProcessingUnits());
  // Include resource usage stats
  bm.mutable_heartbeat()->mutable_load()->CopyFrom(stats);
  VLOG(2) << "Sending heartbeat to parent coordinator!";
  if (!SendMessageToRemote(parent_chan_, &bm)) {
    LOG(ERROR) << "Failed to send heartbeat to parent coordinator!";
    // Try to re-register
    RegisterWithCoordinator(parent_chan_);
  }
}

const string Coordinator::SubmitJob(const JobDescriptor& job_descriptor) {
  // Generate a job ID
  // TODO(malte): This should become deterministic, and based on the
  // inputs/outputs somehow, maybe.
  JobID_t new_job_id = GenerateJobID();

  LOG(INFO) << "NEW JOB: " << new_job_id;
  VLOG(2) << "Details:\n" << job_descriptor.DebugString();
  // Clone the submitted JD and add job to local job table
  JobDescriptor* new_jd = new JobDescriptor;
  new_jd->CopyFrom(job_descriptor);
  CHECK(InsertIfNotPresent(job_table_.get(), new_job_id, job_descriptor));
  // The pointer to the JD has now changed, so reassign it
  new_jd = FindOrNull(*job_table_, new_job_id);
  // Clone the JD and update it with some information
  new_jd->set_uuid(to_string(new_job_id));

  // Set the root task ID (which is 0 or unset on submission)
  TaskDescriptor *root_task = new_jd->mutable_root_task();
  root_task->set_uid(GenerateRootTaskID(*new_jd));
  // Compute the absolute deadline for the root task if it has a deadline
  // set.
  if (root_task->has_relative_deadline()) {
    root_task->set_absolute_deadline(
        GetCurrentTimestamp() + root_task->relative_deadline() * 1000000);
  }
  // Add itself and its spawned tasks (if any) to the relevant tables:
  // - tasks to the task_table_
  // - inputs/outputs to the object_table_
  // and set the job_id field on every task.
  AddJobsTasksToTables(root_task, new_job_id);
  // Set up job outputs
  for (RepeatedPtrField<string>::const_iterator output_iter =
       new_jd->output_ids().begin();
       output_iter != new_jd->output_ids().end();
       ++output_iter) {
    // The root task must produce all of the non-existent job outputs, so they
    // should all be in the object table now.
    DataObjectID_t output_id(DataObjectIDFromProtobuf(*output_iter));
    VLOG(1) << "Considering job output " << output_id;
    set<ReferenceInterface*>* refs = object_store_->GetReferences(output_id);
    CHECK(refs && refs->size() > 0)
        << "Could not find reference to data object ID " << output_id
        << ", which we just added!";
  }

  // Kick off the scheduler for this job.
  uint64_t num_scheduled =
    scheduler_->ScheduleJob(FindOrNull(*job_table_, new_job_id), NULL);
  LOG(INFO) << "Attempted to schedule job " << new_job_id << ", successfully "
            << "scheduled " << num_scheduled << " tasks.";

  // Finally, return the new job's ID
  return to_string(new_job_id);
}

void Coordinator::CreateContainerMonitor() {
  ContainerMonitor::StartContainerMonitor(FLAGS_container_monitor_port);
}

bool Coordinator::HaveUnreservedMachineCapacity(
    const ResourceVector& request) {
  ResourceVector unreserved_resources(machine_capacity_);

  const ResourceVector* machine_reservations =
      scheduler_->knowledge_base()->GetMachineReservations(machine_uuid_);
  if (machine_reservations) {
    unreserved_resources.set_ram_cap(
        unreserved_resources.ram_cap() - machine_reservations->ram_cap());
    unreserved_resources.set_disk_bw(
        unreserved_resources.disk_bw() - machine_reservations->disk_bw());
    unreserved_resources.set_disk_cap(
        unreserved_resources.disk_cap() - machine_reservations->disk_cap());
  }

  return (request.ram_cap() <= unreserved_resources.ram_cap()
          && request.disk_bw() <= unreserved_resources.disk_bw()
          && request.disk_cap() <= unreserved_resources.disk_cap());
}

void Coordinator::Shutdown(const string& reason) {
  LOG(INFO) << "Coordinator shutting down; reason: " << reason;
#ifdef __HTTP_UI__
  if (FLAGS_http_ui && c_http_ui_ && c_http_ui_->active())
      c_http_ui_->Shutdown(false);
#endif
  m_adapter_->StopListen();
  VLOG(1) << "All connections shut down; now exiting...";
  // Toggling the exit flag will make the Coordinator drop out of its main loop.
  exit_ = true;
}

} // namespace firmament
