// Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.
//

#include "yb/master/clone/clone_state_manager.h"

#include <mutex>

#include "yb/common/snapshot.h"
#include "yb/gutil/macros.h"
#include "yb/gutil/map-util.h"
#include "yb/master/async_rpc_tasks.h"
#include "yb/master/catalog_entity_info.pb.h"
#include "yb/master/clone/clone_state_entity.h"
#include "yb/master/master_snapshot_coordinator.h"
#include "yb/master/sys_catalog.h"
#include "yb/rpc/rpc_context.h"
#include "yb/util/oid_generator.h"
#include "yb/util/status.h"
#include "yb/util/status_format.h"

DEFINE_RUNTIME_PREVIEW_bool(enable_db_clone, false, "Enable DB cloning.");

namespace yb {
namespace master {

using std::string;
using namespace std::literals;
using namespace std::placeholders;

std::unique_ptr<CloneStateManager> CloneStateManager::Create(
    CatalogManagerIf* catalog_manager, Master* master, SysCatalogTable* sys_catalog) {
  ExternalFunctions external_functions = {
    .Restore = [catalog_manager, &snapshot_coordinator = catalog_manager->snapshot_coordinator()]
        (const TxnSnapshotId& snapshot_id, HybridTime restore_at) {
      return snapshot_coordinator.Restore(snapshot_id, restore_at,
          catalog_manager->leader_ready_term());
    },
    .ListRestorations = [&snapshot_coordinator = catalog_manager->snapshot_coordinator()]
        (const TxnSnapshotId& snapshot_id, ListSnapshotRestorationsResponsePB* resp) {
      return snapshot_coordinator.ListRestorations(
          TxnSnapshotRestorationId::Nil(), snapshot_id, resp);
    },

    .GetTabletInfo = [catalog_manager](const TabletId& tablet_id) {
      return catalog_manager->GetTabletInfo(tablet_id);
    },
    .FindNamespaceById = [catalog_manager](const NamespaceId& namespace_id) {
      return catalog_manager->FindNamespaceById(namespace_id);
    },
    .ScheduleCloneTabletCall = [catalog_manager, master]
        (const TabletInfoPtr& source_tablet, LeaderEpoch epoch, tablet::CloneTabletRequestPB req) {
      auto call = std::make_shared<AsyncCloneTablet>(
        master, catalog_manager->AsyncTaskPool(), source_tablet, epoch, std::move(req));
      return catalog_manager->ScheduleTask(call);
    },
    .DoCreateSnapshot = [catalog_manager] (
        const CreateSnapshotRequestPB* req, CreateSnapshotResponsePB* resp,
        CoarseTimePoint deadline, const LeaderEpoch& epoch) {
      return catalog_manager->DoCreateSnapshot(req, resp, deadline, epoch);
    },
    .GenerateSnapshotInfoFromSchedule = [catalog_manager] (
        const SnapshotScheduleId& snapshot_schedule_id, HybridTime export_time,
        CoarseTimePoint deadline) {
      return catalog_manager->GenerateSnapshotInfoFromSchedule(
          snapshot_schedule_id, export_time, deadline);
    },
    .DoImportSnapshotMeta = [catalog_manager] (
        const SnapshotInfoPB& snapshot_pb, const LeaderEpoch& epoch,
        const std::optional<std::string>& clone_target_namespace_name, NamespaceMap* namespace_map,
        UDTypeMap* type_map, ExternalTableSnapshotDataMap* tables_data,
        CoarseTimePoint deadline) {
      return catalog_manager->DoImportSnapshotMeta(
          snapshot_pb, epoch, clone_target_namespace_name, namespace_map, type_map, tables_data,
          deadline);
    },


    .Upsert = [catalog_manager, sys_catalog](const CloneStateInfoPtr& clone_state){
      return sys_catalog->Upsert(catalog_manager->leader_ready_term(), clone_state);
    },
    .Load = [sys_catalog](
        const std::string& type,
        std::function<Status(const std::string&, const SysCloneStatePB&)> inserter) {
      return sys_catalog->Load<CloneStateLoader, SysCloneStatePB>(type, inserter);
    }
  };

  return std::unique_ptr<CloneStateManager>(new CloneStateManager(std::move(external_functions)));
}

CloneStateManager::CloneStateManager(ExternalFunctions external_functions):
    external_funcs_(std::move(external_functions)) {}

Status CloneStateManager::IsCloneDone(
    const IsCloneDoneRequestPB* req, IsCloneDoneResponsePB* resp) {
  auto clone_state = VERIFY_RESULT(GetCloneStateFromSourceNamespace(req->source_namespace_id()));
  auto lock = clone_state->LockForRead();
  auto seq_no = req->seq_no();
  auto current_seq_no = lock->pb.clone_request_seq_no();
  if (current_seq_no > seq_no) {
    resp->set_is_done(true);
  } else if (current_seq_no == seq_no) {
    resp->set_is_done(lock->pb.aggregate_state() == SysCloneStatePB::RESTORED);
  } else {
    return STATUS_FORMAT(IllegalState,
        "Clone seq_no $0 never started for namespace $1 (current seq no $2)",
        seq_no, req->source_namespace_id(), current_seq_no);
  }
  return Status::OK();
}

Status CloneStateManager::CloneFromSnapshotSchedule(
    const CloneFromSnapshotScheduleRequestPB* req,
    CloneFromSnapshotScheduleResponsePB* resp,
    rpc::RpcContext* rpc,
    const LeaderEpoch& epoch) {
  SCHECK(!req->target_namespace_name().empty(), InvalidArgument, "Got empty target namespace name");
  LOG(INFO) << "Servicing CloneFromSnapshotSchedule request: " << req->ShortDebugString();
  auto snapshot_schedule_id = TryFullyDecodeSnapshotScheduleId(req->snapshot_schedule_id());
  auto restore_time = HybridTime(req->restore_ht());
  auto [source_namespace_id, seq_no] = VERIFY_RESULT(CloneFromSnapshotSchedule(
      snapshot_schedule_id, restore_time, req->target_namespace_name(), rpc->GetClientDeadline(),
      epoch));
  resp->set_source_namespace_id(source_namespace_id);
  resp->set_seq_no(seq_no);
  return Status::OK();
}

Result<std::pair<NamespaceId, uint32_t>> CloneStateManager::CloneFromSnapshotSchedule(
    const SnapshotScheduleId& snapshot_schedule_id,
    const HybridTime& restore_time,
    const std::string& target_namespace_name,
    CoarseTimePoint deadline,
    const LeaderEpoch& epoch) {
  if (!FLAGS_enable_db_clone) {
    return STATUS_FORMAT(ConfigurationError, "FLAGS_enable_db_clone is disabled");
  }

  // Export snapshot info.
  auto snapshot_info = VERIFY_RESULT(external_funcs_.GenerateSnapshotInfoFromSchedule(
      snapshot_schedule_id, restore_time, deadline));
  auto source_snapshot_id = VERIFY_RESULT(FullyDecodeTxnSnapshotId(snapshot_info.id()));

  // Import snapshot info.
  NamespaceMap namespace_map;
  UDTypeMap type_map;
  ExternalTableSnapshotDataMap tables_data;
  RETURN_NOT_OK(external_funcs_.DoImportSnapshotMeta(
      snapshot_info, epoch, target_namespace_name, &namespace_map, &type_map, &tables_data,
      deadline));
  if (namespace_map.size() != 1) {
    return STATUS_FORMAT(IllegalState, "Expected 1 namespace, got $0", namespace_map.size());
  }

  // Generate a new snapshot.
  // All indexes already are in the request. Do not add them twice.
  // It is safe to trigger the clone op immediately after this since imported snapshots are created
  // synchronously.
  // TODO: Change this and xrepl_catalog_manager to use CreateSnapshot so all our snapshots go
  // through the same path.
  CreateSnapshotRequestPB create_snapshot_req;
  CreateSnapshotResponsePB create_snapshot_resp;
  for (const auto& [old_table_id, table_data] : tables_data) {
    const auto& table_meta = *table_data.table_meta;
    const string& new_table_id = table_meta.table_ids().new_id();
    if (!ImportSnapshotMetaResponsePB_TableType_IsValid(table_meta.table_type())) {
      return STATUS_FORMAT(InternalError, "Found unknown table type: ",
          table_meta.table_type());
    }

    create_snapshot_req.mutable_tables()->Add()->set_table_id(new_table_id);
  }
  create_snapshot_req.set_add_indexes(false);
  create_snapshot_req.set_transaction_aware(true);
  create_snapshot_req.set_imported(true);
  RETURN_NOT_OK(external_funcs_.DoCreateSnapshot(
      &create_snapshot_req, &create_snapshot_resp, deadline, epoch));
  if (create_snapshot_resp.has_error()) {
    return StatusFromPB(create_snapshot_resp.error().status());
  }
  auto target_snapshot_id = VERIFY_RESULT(
      FullyDecodeTxnSnapshotId(create_snapshot_resp.snapshot_id()));

  const auto source_namespace_id = namespace_map.begin()->first;
  auto source_namespace = VERIFY_RESULT(external_funcs_.FindNamespaceById(source_namespace_id));
  auto seq_no = source_namespace->FetchAndIncrementCloneSeqNo();

  // Set up persisted clone state.
  auto clone_state = VERIFY_RESULT(CreateCloneState(
    seq_no, source_namespace_id, target_namespace_name, source_snapshot_id,
    target_snapshot_id, restore_time, tables_data));

  RETURN_NOT_OK(ScheduleCloneOps(clone_state, epoch));
  return make_pair(source_namespace_id, seq_no);
}


Status CloneStateManager::ClearAndRunLoaders() {
  {
    std::lock_guard l(mutex_);
    source_clone_state_map_.clear();
  }
  RETURN_NOT_OK(external_funcs_.Load(
      "Clone states",
      std::function<Status(const std::string&, const SysCloneStatePB&)>(
          std::bind(&CloneStateManager::LoadCloneState, this, _1, _2))));

  return Status::OK();
}

Status CloneStateManager::LoadCloneState(const std::string& id, const SysCloneStatePB& metadata) {
  auto clone_state = CloneStateInfoPtr(new CloneStateInfo(id));
  clone_state->Load(metadata);
  std::lock_guard lock(mutex_);
  auto read_lock = clone_state->LockForRead();
  auto& source_namespace_id = read_lock->pb.source_namespace_id();
  auto seq_no = read_lock->pb.clone_request_seq_no();

  auto it = source_clone_state_map_.find(source_namespace_id);
  if (it != source_clone_state_map_.end()) {
    auto existing_seq_no = it->second->LockForRead()->pb.clone_request_seq_no();
    LOG(INFO) << Format(
        "Found existing clone state for source namespace $0 with seq_no $1. This clone "
        "state's seq_no is $2", source_namespace_id, existing_seq_no, seq_no);
    if (seq_no < existing_seq_no) {
      // Do not overwrite the higher seq_no clone state.
      return Status::OK();
    }
    // TODO: Delete clone state with lower seq_no from sys catalog.
  }
  source_clone_state_map_[source_namespace_id] = clone_state;
  return Status::OK();
}

Result<CloneStateInfoPtr> CloneStateManager::CreateCloneState(
    uint32_t seq_no,
    const NamespaceId& source_namespace_id,
    const string& target_namespace_name,
    const TxnSnapshotId& source_snapshot_id,
    const TxnSnapshotId& target_snapshot_id,
    const HybridTime& restore_time,
    const ExternalTableSnapshotDataMap& table_snapshot_data) {
  CloneStateInfoPtr clone_state;
  {
    std::lock_guard lock(mutex_);
    auto it = source_clone_state_map_.find(source_namespace_id);
    if (it != source_clone_state_map_.end()) {
      auto state = it->second->LockForRead()->pb.aggregate_state();
      if (state != SysCloneStatePB::RESTORED) {
        return STATUS_FORMAT(
            AlreadyPresent, "Cannot create new clone state because there is already an ongoing "
            "clone for source namespace $0 in state $1", source_namespace_id, state);
      }
      // One day we might want to clean up the replaced clone state object here instead of at load
      // time.
    }
    clone_state = CloneStateInfoPtr(new CloneStateInfo(GenerateObjectId()));
    source_clone_state_map_[source_namespace_id] = clone_state;
  }

  clone_state->mutable_metadata()->StartMutation();
  auto* pb = &clone_state->mutable_metadata()->mutable_dirty()->pb;
  pb->set_clone_request_seq_no(seq_no);
  pb->set_source_snapshot_id(source_snapshot_id.data(), source_snapshot_id.size());
  pb->set_target_snapshot_id(target_snapshot_id.data(), target_snapshot_id.size());
  pb->set_source_namespace_id(source_namespace_id);
  pb->set_restore_time(restore_time.ToUint64());

  // Add data for each tablet in this table.
  std::unordered_map<TabletId, int> target_tablet_to_index;
  for (const auto& [_, table_snapshot_data] : table_snapshot_data) {
    for (auto& tablet : table_snapshot_data.table_meta->tablets_ids()) {
      auto* tablet_data = pb->add_tablet_data();
      tablet_data->set_source_tablet_id(tablet.old_id());
      tablet_data->set_target_tablet_id(tablet.new_id());
      target_tablet_to_index[tablet.new_id()] = pb->tablet_data_size() - 1;
    }
  }
  // If this namespace somehow has 0 tablets, create it as RESTORED.
  pb->set_aggregate_state(
      target_tablet_to_index.empty() ? SysCloneStatePB::RESTORED : SysCloneStatePB::CREATING);

  RETURN_NOT_OK(external_funcs_.Upsert(clone_state));
  clone_state->mutable_metadata()->CommitMutation();
  return clone_state;
}

Status CloneStateManager::ScheduleCloneOps(
    const CloneStateInfoPtr& clone_state, const LeaderEpoch& epoch) {
  auto lock = clone_state->LockForRead();
  auto& pb = lock->pb;
  for (auto& tablet_data : pb.tablet_data()) {
    auto source_tablet = VERIFY_RESULT(
        external_funcs_.GetTabletInfo(tablet_data.source_tablet_id()));
    auto target_tablet = VERIFY_RESULT(
        external_funcs_.GetTabletInfo(tablet_data.target_tablet_id()));
    auto target_table = target_tablet->table();
    auto target_table_lock = target_table->LockForRead();

    tablet::CloneTabletRequestPB req;
    req.set_tablet_id(tablet_data.source_tablet_id());
    req.set_target_tablet_id(tablet_data.target_tablet_id());
    req.set_source_snapshot_id(pb.source_snapshot_id().data(), pb.source_snapshot_id().size());
    req.set_target_snapshot_id(pb.target_snapshot_id().data(), pb.target_snapshot_id().size());
    req.set_target_table_id(target_table->id());
    req.set_target_namespace_name(target_table_lock->namespace_name());
    req.set_clone_request_seq_no(pb.clone_request_seq_no());
    req.set_target_pg_table_id(target_table_lock->pb.pg_table_id());
    if (target_table_lock->pb.has_index_info()) {
      *req.mutable_target_index_info() = target_table_lock->pb.index_info();
    }
    *req.mutable_target_schema() = target_table_lock->pb.schema();
    *req.mutable_target_partition_schema() = target_table_lock->pb.partition_schema();
    RETURN_NOT_OK(external_funcs_.ScheduleCloneTabletCall(source_tablet, epoch, std::move(req)));
  }
  return Status::OK();
}

Result<CloneStateInfoPtr> CloneStateManager::GetCloneStateFromSourceNamespace(
    const NamespaceId& namespace_id) {
  std::lock_guard lock(mutex_);
  return FIND_OR_RESULT(source_clone_state_map_, namespace_id);
}

Status CloneStateManager::HandleCreatingState(const CloneStateInfoPtr& clone_state) {
  auto lock = clone_state->LockForWrite();

  SCHECK_EQ(lock->pb.aggregate_state(), SysCloneStatePB::CREATING, IllegalState,
      "Expected clone to be in creating state");

  bool all_tablets_running = true;
  auto& pb = lock.mutable_data()->pb;
  for (auto& tablet_data : *pb.mutable_tablet_data()) {
    // Check to see if the tablet is done cloning (i.e. it is RUNNING).
    auto tablet = VERIFY_RESULT(external_funcs_.GetTabletInfo(tablet_data.target_tablet_id()));
    if (!tablet->LockForRead()->is_running()) {
      all_tablets_running = false;
    }
  }

  if (!all_tablets_running) {
    return Status::OK();
  }

  LOG(INFO) << Format("All tablets for cloned namespace $0 with seq_no $1 are running. "
      "Marking clone operation as restoring.",
      pb.source_namespace_id(), pb.clone_request_seq_no());
  auto target_snapshot_id = VERIFY_RESULT(FullyDecodeTxnSnapshotId(pb.target_snapshot_id()));

  // Check for an existing restoration id. This might have happened if a restoration was submitted
  // but we crashed / failed over before we were able to persist the clone state as RESTORING.
  ListSnapshotRestorationsResponsePB resp;
  RETURN_NOT_OK(external_funcs_.ListRestorations(target_snapshot_id, &resp));
  if (resp.restorations().empty()) {
    RETURN_NOT_OK(external_funcs_.Restore(target_snapshot_id, HybridTime(pb.restore_time())));
  }
  pb.set_aggregate_state(SysCloneStatePB::RESTORING);

  RETURN_NOT_OK(external_funcs_.Upsert(clone_state));
  lock.Commit();
  return Status::OK();
}

Status CloneStateManager::HandleRestoringState(const CloneStateInfoPtr& clone_state) {
  auto lock = clone_state->LockForWrite();
  auto target_snapshot_id = VERIFY_RESULT(FullyDecodeTxnSnapshotId(lock->pb.target_snapshot_id()));

  ListSnapshotRestorationsResponsePB resp;
  RETURN_NOT_OK(external_funcs_.ListRestorations(target_snapshot_id, &resp));
  SCHECK_EQ(resp.restorations_size(), 1, IllegalState, "Unexpected number of restorations.");

  if (resp.restorations(0).entry().state() != SysSnapshotEntryPB::RESTORED) {
    return Status::OK();
  }

  lock.mutable_data()->pb.set_aggregate_state(SysCloneStatePB::RESTORED);
  RETURN_NOT_OK(external_funcs_.Upsert(clone_state));
  lock.Commit();
  return Status::OK();
}

Status CloneStateManager::Run() {
  std::lock_guard lock(mutex_);
  for (auto& [source_namespace_id, clone_state] : source_clone_state_map_) {
    Status s;
    switch (clone_state->LockForRead()->pb.aggregate_state()) {
      case SysCloneStatePB::CREATING:
        s = HandleCreatingState(clone_state);
        break;
      case SysCloneStatePB::RESTORING:
        s = HandleRestoringState(clone_state);
        break;
      case SysCloneStatePB::RESTORED:
        break;
    }
    WARN_NOT_OK(s,
        Format("Could not handle clone state for source namespace $0", source_namespace_id));
  }
  return Status::OK();
}

} // namespace master
} // namespace yb
