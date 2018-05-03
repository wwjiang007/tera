// Copyright (c) 2015, Baidu.com, Inc. All Rights Reserved
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "master/tablet_manager.h"

#include <fstream>
#include <limits>
#include <string>
#include <vector>

#include <gflags/gflags.h>
#include <glog/logging.h>
#include <sofa/pbrpc/pbrpc.h>

#include "common/base/string_ext.h"
#include "common/base/string_format.h"
#include "common/base/string_number.h"
#include "common/file/file_path.h"
#include "common/timer.h"
#include "db/filename.h"
#include "io/io_utils.h"
#include "io/utils_leveldb.h"
#include "master/master_impl.h"
#include "proto/kv_helper.h"
#include "proto/proto_helper.h"
#include "proto/tabletnode_client.h"
#include "types.h"
#include "utils/string_util.h"

DECLARE_string(tera_working_dir);
DECLARE_string(tera_master_meta_table_path);
DECLARE_string(tera_master_meta_table_name);
DECLARE_bool(tera_zk_enabled);

DECLARE_string(tera_master_gc_strategy);
DECLARE_int32(tera_master_impl_retry_times);
DECLARE_int32(tera_tabletnode_connect_retry_period);

DECLARE_bool(tera_delete_obsolete_tabledir_enabled);

DECLARE_string(tera_tabletnode_path_prefix);

namespace tera {
namespace master {

std::ostream& operator << (std::ostream& o, const TabletFile& file) {
    o << file.tablet_id << "-" << file.lg_id << "-" << file.file_id;
    return o;
}

std::ostream& operator << (std::ostream& o, const Tablet& tablet) {
    MutexLock lock(&tablet.mutex_);
    o << tablet.meta_.path() << " ["
      << DebugString(tablet.meta_.key_range().key_start()) << ", "
      << DebugString(tablet.meta_.key_range().key_end()) << "] @ "
      << tablet.meta_.server_addr() << "/" << tablet.server_id_;
    return o;
}

std::ostream& operator << (std::ostream& o, const TabletPtr& tablet) {
    o << *tablet;
    return o;
}

Tablet::Tablet(const TabletMeta& meta)
    : meta_(meta),
      update_time_(common::timer::get_micros()),
      ready_time_(std::numeric_limits<int64_t>::max()),
      merge_param_(NULL),
      gc_reported_(false) {}

Tablet::Tablet(const TabletMeta& meta, TablePtr table)
    : meta_(meta),
      table_(table),
      update_time_(common::timer::get_micros()),
      ready_time_(std::numeric_limits<int64_t>::max()),
      merge_param_(NULL),
      gc_reported_(false) {}

Tablet::~Tablet() {
    table_.reset();
}

void Tablet::ToMeta(TabletMeta* meta) {
    MutexLock lock(&mutex_);
    meta->CopyFrom(meta_);
}

const std::string& Tablet::GetTableName() {
    MutexLock lock(&mutex_);
    return meta_.table_name();
}

const std::string& Tablet::GetServerAddr() {
    MutexLock lock(&mutex_);
    return meta_.server_addr();
}

std::string Tablet::GetServerId() {
    MutexLock lock(&mutex_);
    return server_id_;
}

const std::string& Tablet::GetPath() {
    MutexLock lock(&mutex_);
    return meta_.path();
}

int64_t Tablet::GetDataSize() {
    MutexLock lock(&mutex_);
    return meta_.size();
}

void Tablet::GetDataSize(int64_t* size, std::vector<int64_t>* lg_size) {
    MutexLock lock(&mutex_);
    if (size) {
        *size = meta_.size();
    }
    if (lg_size) {
        lg_size->clear();
        for (int64_t i = 0; i < meta_.lg_size_size(); ++i) {
            lg_size->push_back(meta_.lg_size(i));
        }
    }
}

int64_t Tablet::GetQps() {
    MutexLock lock(&mutex_);
    return average_counter_.read_rows() + average_counter_.write_rows()
        + average_counter_.scan_rows();
}

const std::string& Tablet::GetKeyStart() {
    MutexLock lock(&mutex_);
    return meta_.key_range().key_start();
}

const std::string& Tablet::GetKeyEnd() {
    MutexLock lock(&mutex_);
    return meta_.key_range().key_end();
}

const KeyRange& Tablet::GetKeyRange() {
    MutexLock lock(&mutex_);
    return meta_.key_range();
}

const TableSchema& Tablet::GetSchema() {
    return table_->GetSchema();
}

const TabletCounter& Tablet::GetCounter() {
    MutexLock lock(&mutex_);
    if (counter_list_.size() > 0) {
        return counter_list_.back();
    } else {
        return average_counter_;
    }
}

const TabletCounter& Tablet::GetAverageCounter() {
    MutexLock lock(&mutex_);
    return average_counter_;
}

TabletStatus Tablet::GetStatus() {
    MutexLock lock(&mutex_);
    return meta_.status();
}

CompactStatus Tablet::GetCompactStatus() {
    MutexLock lock(&mutex_);
    return meta_.compact_status();
}

std::string Tablet::GetExpectServerAddr() {
    MutexLock lock(&mutex_);
    return expect_server_addr_;
}

TablePtr Tablet::GetTable() {
    return table_;
}

bool Tablet::IsBusy() {
    MutexLock lock(&mutex_);
    if (counter_list_.size() > 0) {
        return counter_list_.back().is_on_busy();
    } else {
        return false;
    }
}

std::string Tablet::DebugString() {
    MutexLock lock(&mutex_);
    return meta_.DebugString();
}

void Tablet::SetCounter(const TabletCounter& counter) {
    MutexLock lock(&mutex_);
    average_counter_.set_low_read_cell(
        CounterWeightedSum(counter.low_read_cell(), average_counter_.low_read_cell()));
    average_counter_.set_scan_rows(
        CounterWeightedSum(counter.scan_rows(), average_counter_.scan_rows()));
    average_counter_.set_scan_kvs(
        CounterWeightedSum(counter.scan_kvs(), average_counter_.scan_kvs()));
    average_counter_.set_scan_size(
        CounterWeightedSum(counter.scan_size(), average_counter_.scan_size()));
    average_counter_.set_read_rows(
        CounterWeightedSum(counter.read_rows(), average_counter_.read_rows()));
    average_counter_.set_read_kvs(
        CounterWeightedSum(counter.read_kvs(), average_counter_.read_kvs()));
    average_counter_.set_read_size(
        CounterWeightedSum(counter.read_size(), average_counter_.read_size()));
    average_counter_.set_write_rows(
        CounterWeightedSum(counter.write_rows(), average_counter_.write_rows()));
    average_counter_.set_write_kvs(
        CounterWeightedSum(counter.write_kvs(), average_counter_.write_kvs()));
    average_counter_.set_write_size(
        CounterWeightedSum(counter.write_size(), average_counter_.write_size()));
    average_counter_.set_write_workload(counter.write_workload());
    average_counter_.set_is_on_busy(
        CounterWeightedSum(counter.is_on_busy(), average_counter_.is_on_busy()));
}

void Tablet::UpdateSize(const TabletMeta& meta) {
    MutexLock lock(&mutex_);
    meta_.set_size(meta.size());
    meta_.mutable_lg_size()->CopyFrom(meta.lg_size());
}

void Tablet::SetCompactStatus(CompactStatus compact_status) {
    MutexLock lock(&mutex_);
    meta_.set_compact_status(compact_status);
}

void Tablet::SetAddr(const std::string& server_addr) {
    MutexLock lock(&mutex_);
    meta_.set_server_addr(server_addr);
}

void Tablet::SetServerId(const std::string& server_id) {
    MutexLock lock(&mutex_);
    server_id_ = server_id;
}

void Tablet::SetExpectServerAddr(const std::string& server_addr) {
    MutexLock lock(&mutex_);
    expect_server_addr_ = server_addr;
}

bool Tablet::SetStatus(TabletStatus new_status, TabletStatus* old_status) {
    MutexLock lock(&mutex_);
    if (NULL != old_status) {
        *old_status = meta_.status();
    }
    if (CheckStatusSwitch(meta_.status(), new_status)) {
        meta_.set_status(new_status);
        if (new_status == kTableReady) {
            ready_time_ = get_micros();
        }
        return true;
    }
    return false;
}

bool Tablet::SetStatusIf(TabletStatus new_status, TabletStatus if_status,
                         TabletStatus* old_status) {
    MutexLock lock(&mutex_);
    if (NULL != old_status) {
        *old_status = meta_.status();
    }
    if (meta_.status() == if_status
        && CheckStatusSwitch(meta_.status(), new_status)) {
        meta_.set_status(new_status);
        if (new_status == kTableReady) {
            ready_time_ = get_micros();
        }
        return true;
    }
    return false;
}

bool Tablet::SetStatusIf(TabletStatus new_status, TabletStatus if_status,
                         TableStatus if_table_status, TabletStatus* old_status) {
    if (!IsBound()) {
        return false;
    }
    MutexLock lock(&table_->mutex_);
    MutexLock lock2(&mutex_);
    if (NULL != old_status) {
        *old_status = meta_.status();
    }
    if (meta_.status() == if_status && table_->status_ == if_table_status
        && CheckStatusSwitch(meta_.status(), new_status)) {
        meta_.set_status(new_status);
        if (new_status == kTableReady) {
            ready_time_ = get_micros();
        }
        return true;
    }
    return false;
}

bool Tablet::SetAddrIf(const std::string& server_addr, TabletStatus if_status,
                       TabletStatus* old_status) {
    MutexLock lock(&mutex_);
    if (NULL != old_status) {
        *old_status = meta_.status();
    }
    if (meta_.status() == if_status) {
        meta_.set_server_addr(server_addr);
        return true;
    }
    return false;
}

bool Tablet::SetAddrAndStatus(const std::string& server_addr,
                              TabletStatus new_status,
                              TabletStatus* old_status) {
    MutexLock lock(&mutex_);
    if (NULL != old_status) {
        *old_status = meta_.status();
    }
    if (CheckStatusSwitch(meta_.status(), new_status)) {
        meta_.set_status(new_status);
        meta_.set_server_addr(server_addr);
        if (new_status == kTableReady) {
            ready_time_ = get_micros();
        }
        return true;
    }
    return false;
}

bool Tablet::SetAddrAndStatusIf(const std::string& server_addr,
                                TabletStatus new_status, TabletStatus if_status,
                                TabletStatus* old_status) {
    MutexLock lock(&mutex_);
    if (NULL != old_status) {
        *old_status = meta_.status();
    }
    if (meta_.status() == if_status
        && CheckStatusSwitch(meta_.status(), new_status)) {
        meta_.set_status(new_status);
        meta_.set_server_addr(server_addr);
        if (new_status == kTableReady) {
            ready_time_ = get_micros();
        }
        return true;
    }
    return false;
}

int64_t Tablet::UpdateTime() {
    MutexLock lock(&mutex_);
    return update_time_;
}

int64_t Tablet::SetUpdateTime(int64_t timestamp) {
    MutexLock lock(&mutex_);
    int64_t ts = update_time_;
    update_time_ = timestamp;
    return ts;
}

int64_t Tablet::ReadyTime() {
    MutexLock lock(&mutex_);
    if (meta_.status() != kTableReady) {
        return std::numeric_limits<int>::max();
    } else {
        return ready_time_;
    }
}

int32_t Tablet::AddSnapshot(uint64_t snapshot) {
    MutexLock lock(&mutex_);
    meta_.add_snapshot_list(snapshot);
    return meta_.snapshot_list_size() - 1;
}

void Tablet::ListSnapshot(std::vector<uint64_t>* snapshot) {
    MutexLock lock(&mutex_);
    for (int i = 0; i < meta_.snapshot_list_size(); i++) {
        snapshot->push_back(meta_.snapshot_list(i));
    }
}

void Tablet::DelSnapshot(int32_t id) {
    MutexLock lock(&mutex_);
    google::protobuf::RepeatedField<google::protobuf::uint64>* snapshot_list =
        meta_.mutable_snapshot_list();
    assert(id < snapshot_list->size());
    snapshot_list->SwapElements(id, snapshot_list->size() - 1);
    snapshot_list->RemoveLast();
}

int32_t Tablet::AddRollback(std::string name, uint64_t snapshot_id, uint64_t rollback_point) {
    MutexLock lock(&mutex_);
    Rollback rollback;
    rollback.set_name(name);
    rollback.set_snapshot_id(snapshot_id);
    rollback.set_rollback_point(rollback_point);
    meta_.add_rollbacks()->CopyFrom(rollback);
    return meta_.rollbacks_size() - 1;
}

void Tablet::ListRollback(std::vector<Rollback>* rollbacks) {
    MutexLock lock(&mutex_);
    for (int i = 0; i < meta_.rollbacks_size(); i++) {
        rollbacks->push_back(meta_.rollbacks(i));
    }
}

bool Tablet::IsBound() {
    TablePtr null_ptr;
    if (table_ != null_ptr) {
        return true;
    }
    return false;
}

bool Tablet::Verify(const std::string& table_name, const std::string& key_start,
            const std::string& key_end, const std::string& path,
            const std::string& server_addr, StatusCode* ret_status) {
    MutexLock lock(&mutex_);
    if (meta_.table_name() != table_name
        || meta_.key_range().key_start() != key_start
        || meta_.key_range().key_end() != key_end
        || meta_.path() != path
        || meta_.server_addr() != server_addr) {
        SetStatusCode(kTableInvalidArg, ret_status);
        LOG(WARNING) << "tablet verify failed ["
            << meta_.table_name() << ","
            << meta_.key_range().key_start() << ","
            << meta_.key_range().key_end() << ","
            << meta_.path() << ","
            << meta_.server_addr() << "] vs ["
            << table_name << ","
            << key_start << ","
            << key_end << ","
            << path << ","
            << server_addr << "].";
        return false;
    }
    return true;
}

void Tablet::ToMetaTableKeyValue(std::string* packed_key,
                                 std::string* packed_value) {
    MutexLock lock(&mutex_);
    MakeMetaTableKeyValue(meta_, packed_key, packed_value);
}

void* Tablet::GetMergeParam() {
    MutexLock lock(&mutex_);
    return merge_param_;
}

void Tablet::SetMergeParam(void* merge_param) {
    MutexLock lock(&mutex_);
    merge_param_ = merge_param;
}

bool Tablet::CheckStatusSwitch(TabletStatus old_status,
                               TabletStatus new_status) {
    switch (old_status) {
    case kTableNotInit:
        if (new_status == kTableReady         // tablet is loaded when master up
            || new_status == kTableOffLine) { // tablet is unload when master up
            return true;
        }
        break;
    case kTableReady:
        if (new_status == kTabletPending        // tabletnode down
            || new_status == kTableOffLine      // tabletnode down (move immidiately)
            || new_status == kTableUnLoading    // ready to move tablet
            || new_status == kTableOnSplit      // begin to split
            || new_status == kTabletOnSnapshot
            || new_status == kTabletDelSnapshot) {
            return true;
        }
        break;
    case kTabletOnSnapshot:
        if (new_status == kTableReady) {
            return true;
        }
        break;
    case kTabletDelSnapshot:
        if (new_status == kTableReady) {
            return true;
        }
        break;
    case kTableOnLoad:
        if (new_status == kTableReady           // load succe
            || new_status == kTableOffLine      // tabletnode down
            || new_status == kTableLoadFail) {  // don't know result, wait tabletnode to be killed
            return true;
        }
        break;
    case kTableLoadFail:
        if (new_status == kTableOffLine) {     // tabletnode is killed
            return true;
        }
        break;
    case kTableOnSplit:
        if (new_status == kTableReady             // request rejected
            || new_status == kTableOffLine        // split fail
            || new_status == kTableSplitFail) {   // don't know result, wait tabletnode to be killed
            return true;
        }
        break;
    case kTableSplitFail:
        if (new_status == kTableOnSplit) {       // tabletnode is killed, ready to scan meta
            return true;
        }
        break;
    case kTabletPending:
        if (new_status == kTableReady            // tabletnode up
            || new_status == kTableOffLine) {    // tabletnode down timeout
            return true;
        }
        break;
    case kTableOffLine:
        if (new_status == kTableReady            // tabletnode up
            || new_status == kTableOnLoad        // begin to load
            || new_status == kTabletPending      // tabletnode down before load
            || new_status == kTabletDisable) {   // table is disabled
            return true;
        }
        break;
    case kTableUnLoading:
        if (new_status == kTableOffLine           // unload succe
            || new_status == kTableReady          // unload status rollback when merge failed
            || new_status == kTableOnMerge        // unload success, ready to merge phase2
            || new_status == kTableUnLoadFail) {  // don't know result, wait tabletnode to be killed
            return true;
        }
        break;
    case kTableUnLoadFail:
        if (new_status == kTableOffLine           // tabletnode is killed, ready to load
            || new_status == kTableOnMerge) {     // tabletnode is killed, ready to merge phase2
            return true;
        }
        break;
    case kTableOnMerge:
        if (new_status == kTableOffLine) {        // merge failed, ready to reload
            return true;
        }
        break;
    case kTabletDisable:
        if (new_status == kTableOffLine) {
            return true;
        }
        break;
    default:
        break;
    }

    LOG(ERROR) << "not support status switch "
        << StatusCodeToString(old_status) << " to "
        << StatusCodeToString(new_status);
    return false;
}

std::ostream& operator << (std::ostream& o, const Table& table) {
    MutexLock lock(&table.mutex_);
    o << "table: " << table.name_ << ", schema: "
        << table.schema_.ShortDebugString();
    return o;
}

std::ostream& operator << (std::ostream& o, const TablePtr& table) {
    o << *table;
    return o;
}

Table::Table(const std::string& table_name)
    : name_(table_name),
      status_(kTableEnable),
      deleted_tablet_num_(0),
      max_tablet_no_(0),
      create_time_((int64_t)time(NULL)),
      schema_is_syncing_(false),
      rangefragment_(NULL),
      update_rpc_response_(NULL),
      update_rpc_done_(NULL),
      old_schema_(NULL),
      reported_live_tablets_num_(0) {
}

bool Table::FindTablet(const std::string& key_start, TabletPtr* tablet) {
    MutexLock lock(&mutex_);
    Table::TabletList::iterator it2 = tablets_list_.find(key_start);
    if (it2 == tablets_list_.end()) {
        return false;
    }
    *tablet = it2->second;
    return true;
}

void Table::FindTablet(const std::string& server_addr,
                       std::vector<TabletPtr>* tablet_meta_list) {
    MutexLock lock(&mutex_);
    Table::TabletList::iterator it2 = tablets_list_.begin();
    for (; it2 != tablets_list_.end(); ++it2) {
        TabletPtr tablet = it2->second;
        tablet->mutex_.Lock();
        if (tablet->meta_.server_addr() == server_addr) {
            tablet_meta_list->push_back(tablet);
        }
        tablet->mutex_.Unlock();
    }
}

void Table::GetTablet(std::vector<TabletPtr>* tablet_meta_list) {
    MutexLock lock(&mutex_);
    Table::TabletList::iterator it2 = tablets_list_.begin();
    for (; it2 != tablets_list_.end(); ++it2) {
        TabletPtr tablet = it2->second;
        tablet_meta_list->push_back(tablet);
    }
}

const std::string& Table::GetTableName() {
    MutexLock lock(&mutex_);
    return name_;
}

TableStatus Table::GetStatus() {
    MutexLock lock(&mutex_);
    return status_;
}

bool Table::SetStatus(TableStatus new_status, TableStatus* old_status) {
    MutexLock lock(&mutex_);
    if (NULL != old_status) {
        *old_status = status_;
    }
    if (CheckStatusSwitch(status_, new_status)) {
        status_ = new_status;
        return true;
    }
    return false;
}

bool Table::CheckStatusSwitch(TableStatus old_status,
                              TableStatus new_status) {
    switch (old_status) {
    // table is either in the process of being enable or is enabled
    case kTableEnable:
        if (new_status == kTableDisable) {    // begin to disable table
            return true;
        }
        break;
    // table is either in the process of being disable or is disabled
    case kTableDisable:
        if (new_status == kTableEnable         // begin to enable table
            || new_status == kTableDeleting) {  // begin to delete table
            return true;
        }
        break;
    // table is in the process of deleting
    case kTableDeleting:
        if (new_status == kTableDisable         // begin to enable table
            || new_status == kTableDeleting) {  // begin to delete table
            return true;
        }
        break;
    default:
        break;
    }
    return false;
}

const TableSchema& Table::GetSchema() {
    MutexLock lock(&mutex_);
    return schema_;
}

void Table::SetSchema(const TableSchema& schema) {
    MutexLock lock(&mutex_);
    schema_.CopyFrom(schema);
}

const TableCounter& Table::GetCounter() {
    MutexLock lock(&mutex_);
    return counter_;
}

int32_t Table::AddSnapshot(uint64_t snapshot) {
    MutexLock lock(&mutex_);
    snapshot_list_.push_back(snapshot);
    return snapshot_list_.size() - 1;
}

int32_t Table::DelSnapshot(uint64_t snapshot) {
    MutexLock lock(&mutex_);
    std::vector<uint64_t>::iterator it =
        std::find(snapshot_list_.begin(), snapshot_list_.end(), snapshot);
    if (it == snapshot_list_.end()) {
        return -1;
    } else {
        int id = it - snapshot_list_.begin();
        snapshot_list_[id] = snapshot_list_[snapshot_list_.size()-1];
        snapshot_list_.resize(snapshot_list_.size()-1);
        return id;
    }
}
void Table::ListSnapshot(std::vector<uint64_t>* snapshots) {
    MutexLock lock(&mutex_);
    *snapshots = snapshot_list_;
}

int32_t Table::AddRollback(std::string rollback_name) {
    MutexLock lock(&mutex_);
    rollback_names_.push_back(rollback_name);
    return rollback_names_.size() - 1;
}

void Table::ListRollback(std::vector<std::string>* rollback_names) {
    MutexLock lock(&mutex_);
    *rollback_names = rollback_names_;
}

int64_t Table::GetTabletsCount() {
    MutexLock lock(&mutex_);
    return tablets_list_.size();
}

void Table::AddDeleteTabletCount() {
    MutexLock lock(&mutex_);
    deleted_tablet_num_++;
}

bool Table::NeedDelete() {
    MutexLock lock(&mutex_);
    if (deleted_tablet_num_ == tablets_list_.size()) {
        return true;
    }
    return false;
}

void Table::ToMetaTableKeyValue(std::string* packed_key,
                                std::string* packed_value) {
    MutexLock lock(&mutex_);
    TableMeta meta;
    ToMeta(&meta);
    MakeMetaTableKeyValue(meta, packed_key, packed_value);
}

bool Table::PrepareUpdate(const TableSchema& schema) {
    if (!GetSchemaSyncLockOrFailed()) {
        return false;
    }
    TableSchema* origin_schema = new TableSchema;
    origin_schema->CopyFrom(GetSchema());
    SetOldSchema(origin_schema);
    SetSchema(schema);
    return true;
}

void Table::AbortUpdate() {
    TableSchema old_schema;
    if (GetOldSchema(&old_schema)) {
        SetSchema(old_schema);
        ClearOldSchema();
    }
}

void Table::CommitUpdate() {
    ClearOldSchema();
}

void Table::ToMeta(TableMeta* meta) {
    meta->set_table_name(name_);
    meta->set_status(status_);
    meta->mutable_schema()->CopyFrom(schema_);
    meta->set_create_time(create_time_);
    for (size_t i = 0; i < snapshot_list_.size(); i++) {
        meta->add_snapshot_list(snapshot_list_[i]);
    }
    for (size_t i = 0; i < rollback_names_.size(); ++i) {
        meta->add_rollback_names(rollback_names_[i]);
    }
}

uint64_t Table::GetNextTabletNo() {
    MutexLock lock(&mutex_);
    max_tablet_no_++;
    LOG(INFO) << "generate new tablet number: " << max_tablet_no_;
    return max_tablet_no_;
}

bool Table::GetSchemaIsSyncing() {
    MutexLock lock(&mutex_);
    return schema_is_syncing_;
}

bool Table::GetSchemaSyncLockOrFailed() {
    MutexLock lock(&mutex_);
    if (schema_is_syncing_) {
        return false;
    }
    schema_is_syncing_ = true;
    return true;
}

void Table::SetOldSchema(TableSchema* schema) {
    MutexLock lock(&mutex_);
    delete old_schema_;
    old_schema_ = schema;
}

bool Table::GetOldSchema(TableSchema* schema) {
    MutexLock lock(&mutex_);
    if ((schema != NULL) && (old_schema_ != NULL)) {
        schema->CopyFrom(*old_schema_);
        return true;
    }
    return false;
}

void Table::ClearOldSchema() {
    MutexLock lock(&mutex_);
    delete old_schema_;
    old_schema_ = NULL;
}

void Table::ResetRangeFragment() {
    MutexLock lock(&mutex_);
    delete rangefragment_;
    rangefragment_ = new RangeFragment;
}

RangeFragment* Table::GetRangeFragment() {
    MutexLock lock(&mutex_);
    return rangefragment_;
}

bool Table::AddToRange(const std::string& start, const std::string& end) {
    MutexLock lock(&mutex_);
    return rangefragment_->AddToRange(start, end);
}

bool Table::IsCompleteRange() const {
    MutexLock lock(&mutex_);
    return rangefragment_->IsCompleteRange();
}

bool Table::IsSchemaSyncedAtRange(const std::string& start, const std::string& end) {
    MutexLock lock(&mutex_);
    return rangefragment_->IsCoverRange(start, end);
}

void Table::StoreUpdateRpc(UpdateTableResponse* response, google::protobuf::Closure* done) {
    MutexLock lock(&mutex_);
    update_rpc_response_ = response;
    update_rpc_done_ = done;
}

void Table::UpdateRpcDone() {
    MutexLock lock(&mutex_);
    if (update_rpc_response_ != NULL) {
        update_rpc_response_->set_status(kMasterOk);
        update_rpc_done_->Run();

        update_rpc_response_ = NULL;
        update_rpc_done_ = NULL;
    }
}

void Table::SetSchemaIsSyncing(bool flag) {
    MutexLock lock(&mutex_);
    schema_is_syncing_ = flag;
}

void Table::RefreshCounter() {
    MutexLock lock(&mutex_);
    int64_t size = 0;
    int64_t tablet_num = 0;
    int64_t notready = 0;
    int64_t lread = 0;
    int64_t read = 0;
    int64_t rmax = 0;
    int64_t rspeed = 0;
    int64_t write = 0;
    int64_t wmax = 0;
    int64_t wspeed = 0;
    int64_t scan = 0;
    int64_t smax = 0;
    int64_t sspeed = 0;
    size_t lg_num = 0;
    std::vector<int64_t> lg_size;

    std::vector<TabletPtr> tablet_list;
    Table::TabletList::iterator it = tablets_list_.begin();
    for (; it != tablets_list_.end(); ++it) {
        tablet_num++;
        TabletPtr tablet = it->second;
        if (tablet->GetStatus() != kTableReady) {
            notready++;
        }
        int64_t size_tmp;
        std::vector<int64_t> lg_size_tmp;
        tablet->GetDataSize(&size_tmp, &lg_size_tmp);

        size += size_tmp;
        if (lg_num == 0) {
            lg_num = lg_size_tmp.size();
            lg_size.resize(lg_num, 0);
        }
        for (size_t l = 0; l < lg_num; ++l) {
            if (lg_size_tmp.size() > l) {
                lg_size[l] += lg_size_tmp[l];
            }
        }

        const TabletCounter& counter = tablet->GetCounter();
        lread += counter.low_read_cell();
        read += counter.read_rows();
        if (counter.read_rows() > rmax) {
            rmax = counter.read_rows();
        }
        rspeed += counter.read_size();
        write += counter.write_rows();
        if (counter.write_rows() > wmax) {
            wmax = counter.write_rows();
        }
        wspeed += counter.write_size();
        scan += counter.scan_rows();
        if (counter.scan_rows() > smax) {
            smax = counter.scan_rows();
        }
        sspeed += counter.scan_size();
    }

    counter_.set_size(size);
    counter_.set_tablet_num(tablet_num);
    counter_.set_notready_num(notready);
    counter_.set_lread(lread);
    counter_.set_read_rows(read);
    counter_.set_read_max(rmax);
    counter_.set_read_size(rspeed);
    counter_.set_write_rows(write);
    counter_.set_write_max(wmax);
    counter_.set_write_size(wspeed);
    counter_.set_scan_rows(scan);
    counter_.set_scan_max(smax);
    counter_.set_scan_size(sspeed);
    counter_.clear_lg_size();
    for (size_t l = 0; l < lg_num; ++l) {
        counter_.add_lg_size(lg_size[l]);
    }
}

void Table::MergeTablets(TabletPtr first_tablet, TabletPtr second_tablet,
                         const TabletMeta& merged_meta, TabletPtr* merged_tablet) {
    CHECK_EQ(first_tablet->GetKeyStart(), merged_meta.key_range().key_start());
    CHECK_EQ(second_tablet->GetKeyEnd(), merged_meta.key_range().key_end());
    CHECK_EQ(first_tablet->GetKeyEnd(), second_tablet->GetKeyStart());

    MutexLock lock(&mutex_);
    merged_tablet->reset(new Tablet(merged_meta, first_tablet->GetTable()));
    uint64_t tablet_num = leveldb::GetTabletNumFromPath(merged_meta.path());
    if (max_tablet_no_ < tablet_num) {
        max_tablet_no_ = tablet_num;
    }

    if (FLAGS_tera_master_gc_strategy == "trackable") {
        uint64_t tablet_num1 = leveldb::GetTabletNumFromPath(first_tablet->GetPath());
        std::multiset<TabletFile>::iterator it = first_tablet->inh_files_.begin();
        for (; it != first_tablet->inh_files_.end(); ++it) {
            const TabletFile& file = *it;
            InheritedFileInfo& file_info = useful_inh_files_[file.tablet_id][file];
            CHECK_GT(file_info.ref, 0u);
            VLOG(10) << "[gc] [" << name_ << "] file " << file << " inherited by " << tablet_num1
                << " pass to " << tablet_num << " ref is " << file_info.ref;
            (*merged_tablet)->inh_files_.insert(file);
        }
        uint64_t tablet_num2 = leveldb::GetTabletNumFromPath(second_tablet->GetPath());
        it = second_tablet->inh_files_.begin();
        // ref: +1 for add child tablets, -1 for del parent tablets
        for (; it != second_tablet->inh_files_.end(); ++it) {
            const TabletFile& file = *it;
            InheritedFileInfo& file_info = useful_inh_files_[file.tablet_id][file];
            CHECK_GT(file_info.ref, 0u);
            VLOG(10) << "[gc] [" << name_ << "] file " << file << " inherited by " << tablet_num2
                << " pass to " << tablet_num << " ref is " << file_info.ref;
            (*merged_tablet)->inh_files_.insert(file);
        }

        if (first_tablet->gc_reported_) {
            --reported_live_tablets_num_;
        }
        if (second_tablet->gc_reported_) {
            --reported_live_tablets_num_;
        }
    }

    tablets_list_.erase(first_tablet->GetKeyStart());
    tablets_list_.erase(second_tablet->GetKeyStart());
    tablets_list_[merged_meta.key_range().key_start()] = *merged_tablet;
}

void Table::SplitTablet(TabletPtr splited_tablet,
                        const TabletMeta& first_half, const TabletMeta& second_half,
                        TabletPtr* first_tablet, TabletPtr* second_tablet) {
    CHECK_EQ(splited_tablet->GetKeyStart(), first_half.key_range().key_start());
    CHECK_EQ(splited_tablet->GetKeyEnd(), second_half.key_range().key_end());
    CHECK_EQ(first_half.key_range().key_end(), second_half.key_range().key_start());

    MutexLock lock(&mutex_);
    first_tablet->reset(new Tablet(first_half, splited_tablet->GetTable()));
    uint64_t tablet_num1 = leveldb::GetTabletNumFromPath(first_half.path());
    if (max_tablet_no_ < tablet_num1) {
        max_tablet_no_ = tablet_num1;
    }
    second_tablet->reset(new Tablet(second_half, splited_tablet->GetTable()));
    uint64_t tablet_num2 = leveldb::GetTabletNumFromPath(second_half.path());
    if (max_tablet_no_ < tablet_num2) {
        max_tablet_no_ = tablet_num2;
    }

    if (FLAGS_tera_master_gc_strategy == "trackable") {
        uint64_t tablet_num = leveldb::GetTabletNumFromPath(splited_tablet->GetPath());
        (*first_tablet)->inh_files_ = splited_tablet->inh_files_;
        (*second_tablet)->inh_files_ = splited_tablet->inh_files_;
        std::multiset<TabletFile>::iterator it = splited_tablet->inh_files_.begin();
        for (; it != splited_tablet->inh_files_.end(); ++it) {
            const TabletFile& file = *it;
            InheritedFileInfo& file_info = useful_inh_files_[file.tablet_id][file];
            CHECK_GT(file_info.ref, 0u);
            file_info.ref++; // ref: +2 for add child tablets, -1 for del parent tablets
            VLOG(10) << "[gc] [" << name_ << "] file " << file << " inherited by " << tablet_num
                << " pass to " << tablet_num1 << " and " << tablet_num2
                << " ref increment to " << file_info.ref;
        }

        if (splited_tablet->gc_reported_) {
            --reported_live_tablets_num_;
        }
    }

    tablets_list_.erase(first_half.key_range().key_start());
    tablets_list_[first_half.key_range().key_start()] = *first_tablet;
    tablets_list_[second_half.key_range().key_start()] = *second_tablet;
}

void Table::GarbageCollect(const TabletInheritedFileInfo& tablet_inh_info) {
    // sort reported files
    std::multiset<TabletFile> report_inh_files;
    for (int32_t i = 0; i < tablet_inh_info.lg_inh_files_size(); i++) {
        const LgInheritedLiveFiles& lg_inh_files = tablet_inh_info.lg_inh_files(i);
        struct TabletFile inh_file;
        inh_file.lg_id = lg_inh_files.lg_no();
        for (int32_t j = 0; j < lg_inh_files.file_number_size(); j++) {
            leveldb::ParseFullFileNumber(lg_inh_files.file_number(j),
                                         &inh_file.tablet_id,
                                         &inh_file.file_id);
            report_inh_files.insert(inh_file);
        }
    }

    MutexLock l(&mutex_);
    Table::TabletList::iterator tablet_it = tablets_list_.find(tablet_inh_info.key_start());
    if (tablet_it == tablets_list_.end()) {
        return;
    }
    TabletPtr tablet = tablet_it->second;
    if (tablet->GetKeyEnd() != tablet_inh_info.key_end()) {
        return;
    }

    // insert a MAX element to simplify two sets' comparason
    struct TabletFile max = {UINT64_MAX, INT32_MAX, UINT64_MAX};
    report_inh_files.insert(max);
    tablet->inh_files_.insert(max);
    std::multiset<TabletFile>::iterator old_it = tablet->inh_files_.begin();
    std::multiset<TabletFile>::iterator new_it = report_inh_files.begin();
    while (old_it != tablet->inh_files_.end() && new_it != report_inh_files.end()) {
        if (*old_it == *new_it) {
            ++old_it;
            ++new_it;
        } else if (*old_it < *new_it) {
            VLOG(10) << "[gc] " << tablet->GetPath() << " release file " << *old_it;
            ReleaseInheritedFile(*old_it);
            old_it = tablet->inh_files_.erase(old_it); // desc ref for tablet->inh_files_
        } else if (!tablet->gc_reported_) {
            VLOG(10) << "[gc] " << tablet->GetPath() << " report file " << *new_it;
            AddInheritedFile(*new_it, true); // inc ref for tablet->inh_files_
            tablet->inh_files_.insert(*new_it);
            ++new_it;
        } else {
            LOG(WARNING) << "[gc] ignore(query error) " << tablet->GetPath() << " report new file " << *new_it;
            ++new_it;
        }
    }
    tablet->inh_files_.erase(max);

    if (!tablet->gc_reported_) {
        tablet->gc_reported_ = true;
        if (++reported_live_tablets_num_ == tablets_list_.size()) {
            // now all live tablets report finish
            std::set<uint64_t>::iterator it = gc_disabled_dead_tablets_.begin();
            for (; it != gc_disabled_dead_tablets_.end(); ++it) {
                EnableDeadTabletGarbageCollect(*it);
            }
            gc_disabled_dead_tablets_.clear();
        }
    }
}

void Table::EnableDeadTabletGarbageCollect(uint64_t tablet_id) {
    mutex_.AssertHeld();
    LOG(INFO) << "[gc] [" << name_ << "] enable gc dir " << tablet_id;
    std::map<TabletFile, InheritedFileInfo>& dead_tablet_files = useful_inh_files_[tablet_id];
    std::map<TabletFile, InheritedFileInfo>::iterator it = dead_tablet_files.begin();
    while (it != dead_tablet_files.end()) {
        const TabletFile& file = it->first;
        InheritedFileInfo& file_info = it->second;
        CHECK_GT(file_info.ref, 0u);
        VLOG(10) << "[gc] [" << name_ << "] file " << file << " ref decrement to " << file_info.ref - 1;
        if (--file_info.ref == 0) { // desc refs for gc_disabled_dead_tablets_
            // delete file
            obsolete_inh_files_.push(file);
            it = dead_tablet_files.erase(it);
        } else {
            ++it;
        }
    }
    if (dead_tablet_files.size() == 0) {
        // delete tablet dir
        VLOG(10) << "[gc] [" << name_ << "] dir " << tablet_id << " has no useful file";
        TabletFile tablet_dir = {tablet_id, 0, 0};
        obsolete_inh_files_.push(tablet_dir);
        useful_inh_files_.erase(tablet_id);
    }
}

void Table::ReleaseInheritedFile(const TabletFile& file) {
    mutex_.AssertHeld();

    InheritedFiles::iterator it = useful_inh_files_.find(file.tablet_id);
    CHECK(it != useful_inh_files_.end());
    std::map<TabletFile, InheritedFileInfo>& dead_tablet_files = it->second;

    std::map<TabletFile, InheritedFileInfo>::iterator it2 = dead_tablet_files.find(file);
    CHECK(it2 != dead_tablet_files.end());
    InheritedFileInfo& inh_file = it2->second;

    CHECK_GT(inh_file.ref, 0u);
    VLOG(10) << "[gc] [" << name_ << "] file " << file << " ref decrement to " << inh_file.ref - 1;
    if (--inh_file.ref == 0) {
        // delete file
        obsolete_inh_files_.push(file);
        dead_tablet_files.erase(it2);
        if (dead_tablet_files.size() == 0) {
            // delete tablet dir
            VLOG(10) << "[gc] [" << name_ << "] dir " << file.tablet_id << " has no useful file";
            TabletFile tablet_dir = {file.tablet_id, 0, 0};
            obsolete_inh_files_.push(tablet_dir);
            useful_inh_files_.erase(it);
        }
    }
}

bool Table::TryCollectInheritedFile() {
    std::set<uint64_t> live_tablets, dead_tablets;
    GetTabletsForGc(&live_tablets, &dead_tablets, true);

    std::set<uint64_t>::iterator it = dead_tablets.begin();
    for (; it != dead_tablets.end(); ++it) {
        std::vector<TabletFile> tablet_files;
        CollectInheritedFileFromFilesystem(name_, *it, &tablet_files);

        for (uint32_t i = 0; i < tablet_files.size(); i++) {
            MutexLock l(&mutex_);
            AddInheritedFile(tablet_files[i], false);
        }
    }
    return dead_tablets.size() > 0;
}

bool Table::CollectInheritedFileFromFilesystem(const std::string& tablename,
                                               uint64_t tablet_num,
                                               std::vector<TabletFile>* tablet_files) {
    std::string tablepath = FLAGS_tera_tabletnode_path_prefix + tablename;
    std::string tablet_path = leveldb::GetTabletPathFromNum(tablepath, tablet_num);
    leveldb::Env* env = io::LeveldbBaseEnv();

    // list lg dir
    std::vector<std::string> children;
    env->GetChildren(tablet_path, &children);
    for (size_t lg = 0; lg < children.size(); ++lg) {
        std::string lg_path = tablet_path + "/" + children[lg];
        leveldb::FileType type = leveldb::kUnknown;
        uint64_t number = 0;
        if (ParseFileName(children[lg], &number, &type)) {
            LOG(INFO) << "[gc] parent tablet has log_file: " << lg_path;
            continue;
        }

        leveldb::Slice rest(children[lg]);
        uint64_t lg_num = 0;
        if (!leveldb::ConsumeDecimalNumber(&rest, &lg_num)) {
            LOG(ERROR) << "[gc] skip unknown dir(not log_num nor lg_num): " << lg_path;
            continue;
        }

        // collector sst file
        std::vector<std::string> files;
        env->GetChildren(lg_path, &files);
        for (size_t f = 0; f < files.size(); ++f) {
            std::string file_path = lg_path + "/" + files[f];
            type = leveldb::kUnknown;
            number = 0;
            if (ParseFileName(files[f], &number, &type) &&
                type == leveldb::kTableFile) {
                struct TabletFile tablet_file = {tablet_num, (uint32_t)lg_num, number};
                tablet_files->push_back(tablet_file);
            }
        }
    }
    return true;
}

bool Table::GetTabletsForGc(std::set<uint64_t>* live_tablets,
                            std::set<uint64_t>* dead_tablets,
                            bool ignore_not_ready) {
    MutexLock lock(&mutex_);

    std::vector<std::string> children;
    leveldb::Env* env = io::LeveldbBaseEnv();
    std::string table_path = FLAGS_tera_tabletnode_path_prefix + name_;
    mutex_.Unlock();

    leveldb::Status s = env->GetChildren(table_path, &children);
    mutex_.Lock();
    if (!s.ok()) {
        LOG(ERROR) << "[gc] fail to list directory: " << table_path;
        return false;
    }

    std::vector<TabletPtr> tablet_list;
    Table::TabletList::iterator it = tablets_list_.begin();
    for (; it != tablets_list_.end(); ++it) {
        TabletPtr tablet = it->second;
        if (tablet->GetStatus() != kTableReady) {
            if (!ignore_not_ready) {
                // any tablet not ready, stop gc
                return false;
            }
        }
        const std::string& path = tablet->GetPath();
        live_tablets->insert(leveldb::GetTabletNumFromPath(path));
        VLOG(20) << "[gc] add live tablet: " << path;
    }

    for (size_t i = 0; i < children.size(); ++i) {
        if (children[i].size() < 5) {
            // skip directory . and ..
            continue;
        }
        std::string path = table_path + "/" + children[i];
        uint64_t tabletnum = leveldb::GetTabletNumFromPath(path);
        if (live_tablets->find(tabletnum) == live_tablets->end()) {
            VLOG(10) << "[gc] add dead tablet: " << path;
            dead_tablets->insert(tabletnum);
        }
    }
    if (dead_tablets->size() == 0) {
        VLOG(10) << "[gc] there is none dead tablets: " << name_;
        return false;
    }
    return true;
}

void Table::AddInheritedFile(const TabletFile& file, bool need_ref) {
    mutex_.AssertHeld();

    bool is_gc_disabled = false;
    if (useful_inh_files_.find(file.tablet_id) == useful_inh_files_.end()) {
        LOG(INFO) << "[gc] [" << name_ << "] new report dir " << file.tablet_id << ", gc disabled";
        gc_disabled_dead_tablets_.insert(file.tablet_id);
    }
    if (gc_disabled_dead_tablets_.find(file.tablet_id) != gc_disabled_dead_tablets_.end()) {
        is_gc_disabled = true;
    }

    InheritedFileInfo& file_info = useful_inh_files_[file.tablet_id][file];
    if (is_gc_disabled && file_info.ref == 0) {
        VLOG(10) << "[gc] [" << name_ << "] new report file " << file;
        file_info.ref = 1; // gc_disabled_dead_tablets_ ref it
    }
    if (need_ref) {
        ++file_info.ref;
    }
    VLOG(10) << "[gc] [" << name_ << "] file " << file << " ref increment to " << file_info.ref;
}

uint64_t Table::CleanObsoleteFile() {
    leveldb::Env* env = io::LeveldbBaseEnv();
    std::string table_path = FLAGS_tera_tabletnode_path_prefix + name_;
    uint64_t delete_file_num = 0;
    int64_t start_ts = get_micros();

    MutexLock l(&mutex_);
    while (!obsolete_inh_files_.empty()) {
        TabletFile file = obsolete_inh_files_.front();
        mutex_.Unlock();
        std::string path;
        leveldb::Status s;
        if (file.lg_id == 0 && file.file_id == 0) {
            std::string path = leveldb::BuildTabletPath(table_path, file.tablet_id);
            LOG(INFO) << "[gc] [" << name_ << "] delete dir " << path;
            s = io::DeleteEnvDir(path); //safely delete dir and all file in it
        } else {
            std::string path = leveldb::BuildTableFilePath(table_path, file.tablet_id,
                                                           file.lg_id, file.file_id);
            LOG(INFO) << "[gc] [" << name_ << "] delete file " << file << " path " << path;
            s = env->DeleteFile(path);
        }
        mutex_.Lock();
        if (!s.ok()) {
            LOG(WARNING) << "[gc] fail to delete: " << path << " status: " << s.ToString();
            break;
        }
        delete_file_num++;
        obsolete_inh_files_.pop();
    }
    LOG(INFO) << "[gc] [" << name_ << "] clean obsolete file/dir, total: " << delete_file_num
        << ", cost: " << (get_micros() - start_ts) / 1000 << " ms";
    return delete_file_num;
}

TabletManager::TabletManager(Counter* sequence_id,
                             MasterImpl* master_impl,
                             ThreadPool* thread_pool)
    : this_sequence_id_(sequence_id),
      master_impl_(master_impl) {}

TabletManager::~TabletManager() {
    ClearTableList();
}

void TabletManager::Init() {
}

void TabletManager::Stop() {
}

bool TabletManager::AddTable(const std::string& table_name,
                             const TableMeta& meta,
                             TablePtr* table, StatusCode* ret_status) {
    // lock table list
    mutex_.Lock();

    // search table
    TablePtr null_table;
    std::pair<TableList::iterator, bool> ret =
        all_tables_.insert(std::pair<std::string, TablePtr>(table_name, null_table));
    TableList::iterator it = ret.first;
    if (!ret.second) {
        mutex_.Unlock();
        LOG(WARNING) << "table: " << table_name << " exist";
        SetStatusCode(kTableExist, ret_status);
        return false;
    }

    it->second.reset(new Table(table_name));
    *table = it->second;
    (*table)->mutex_.Lock();
    mutex_.Unlock();
    (*table)->schema_.CopyFrom(meta.schema());
    (*table)->status_ = meta.status();
    (*table)->create_time_ = meta.create_time();
    for (int32_t i = 0; i < meta.snapshot_list_size(); ++i) {
        (*table)->snapshot_list_.push_back(meta.snapshot_list(i));
        LOG(INFO) << table_name << " add snapshot " << meta.snapshot_list(i);
    }
    for (int32_t i = 0; i < meta.rollback_names_size(); ++i) {
        (*table)->rollback_names_.push_back(meta.rollback_names(i));
        LOG(INFO) << table_name << " add rollback " << meta.rollback_names(i);
    }
    (*table)->mutex_.Unlock();
    return true;
}

bool TabletManager::AddTablet(const TabletMeta& meta, const TableSchema& schema,
                              TabletPtr* tablet, StatusCode* ret_status) {
    // lock table list
    mutex_.Lock();

    // search table
    TablePtr null_table;
    std::pair<TableList::iterator, bool> ret =
        all_tables_.insert(std::pair<std::string, TablePtr>(meta.table_name(), null_table));
    TableList::iterator it = ret.first;
    std::string key_start = meta.key_range().key_start();
    if (!ret.second) {
        // search tablet
        Table& table = *it->second;
        table.mutex_.Lock();
        mutex_.Unlock();
        if (table.tablets_list_.end() != table.tablets_list_.find(key_start)) {
            table.mutex_.Unlock();
            LOG(WARNING) << "table: " << meta.table_name() << ", start: ["
                << DebugString(key_start) << "] exist";
            SetStatusCode(kTableExist, ret_status);
            return false;
        }
    } else {
        it->second.reset(new Table(meta.table_name()));
        Table& table = *it->second;
        table.mutex_.Lock();
        mutex_.Unlock();
        table.schema_.CopyFrom(schema);
        table.status_ = kTableEnable;
    }
    TablePtr table = it->second;
    tablet->reset(new Tablet(meta, table));
    uint64_t tablet_num = leveldb::GetTabletNumFromPath(meta.path());
    if (table->max_tablet_no_ < tablet_num) {
        table->max_tablet_no_ = tablet_num;
    }
    table->tablets_list_[key_start] = *tablet;
    table->mutex_.Unlock();
    return true;
}

bool TabletManager::AddTablet(const std::string& table_name,
                              const std::string& key_start,
                              const std::string& key_end,
                              const std::string& path,
                              const std::string& server_addr,
                              const TableSchema& schema,
                              const TabletStatus& table_status,
                              int64_t data_size, TabletPtr* tablet,
                              StatusCode* ret_status) {
    TabletMeta meta;
    PackTabletMeta(&meta, table_name, key_start, key_end, path,
                   server_addr, table_status, data_size);

    return AddTablet(meta, schema, tablet, ret_status);
}

int64_t TabletManager::GetAllTabletsCount() {
    MutexLock lock(&mutex_);
    int64_t count = 0;
    TableList::iterator it;
    for (it = all_tables_.begin(); it != all_tables_.end(); ++it) {
        count += it->second->GetTabletsCount();
    }
    return count;
}

bool TabletManager::FindTablet(const std::string& table_name,
                               const std::string& key_start,
                               TabletPtr* tablet, StatusCode* ret_status) {
    // lock table list
    mutex_.Lock();

    // search table
    TableList::iterator it = all_tables_.find(table_name);
    if (it == all_tables_.end()) {
        mutex_.Unlock();
        VLOG(5) << "tablet: " << table_name << " [start: "
            << DebugString(key_start) << "] not exist";
        SetStatusCode(kTableNotFound, ret_status);
        return false;
    }
    Table& table = *it->second;

    // lock table
    table.mutex_.Lock();
    mutex_.Unlock();

    // search tablet
    Table::TabletList::iterator it2 = table.tablets_list_.find(key_start);
    if (it2 == table.tablets_list_.end()) {
        table.mutex_.Unlock();
        VLOG(5) << "table: " << table_name << "[start: "
            << DebugString(key_start) << "] not exist";
        SetStatusCode(kTableNotFound, ret_status);
        return false;
    }
    *tablet = it2->second;
    table.mutex_.Unlock();
    return true;
}

void TabletManager::FindTablet(const std::string& server_addr,
                               std::vector<TabletPtr>* tablet_meta_list,
                               bool need_disabled_tables) {
    mutex_.Lock();
    TableList::iterator it = all_tables_.begin();
    for (; it != all_tables_.end(); ++it) {
        Table& table = *it->second;
        table.mutex_.Lock();
        if (table.status_ == kTableDisable && !need_disabled_tables) {
            VLOG(10) << "FindTablet skip disable table: " << table.name_;
            table.mutex_.Unlock();
            continue;
        }
        Table::TabletList::iterator it2 = table.tablets_list_.begin();
        for (; it2 != table.tablets_list_.end(); ++it2) {
            TabletPtr tablet = it2->second;
            tablet->mutex_.Lock();
            if (tablet->meta_.server_addr() == server_addr) {
                tablet_meta_list->push_back(tablet);
            }
            tablet->mutex_.Unlock();
        }
        table.mutex_.Unlock();
    }
    mutex_.Unlock();
}

bool TabletManager::FindOverlappedTablets(const std::string& table_name,
                                          const std::string& key_start,
                                          const std::string& key_end,
                                          std::vector<TabletPtr>* tablets,
                                          StatusCode* ret_status) {
    // lock table list
    mutex_.Lock();

    // search table
    TableList::iterator it = all_tables_.find(table_name);
    if (it == all_tables_.end()) {
        mutex_.Unlock();
        VLOG(5) << "table: " << table_name << " not exist";
        SetStatusCode(kTableNotFound, ret_status);
        return false;
    }
    Table& table = *it->second;

    // lock table
    table.mutex_.Lock();
    mutex_.Unlock();

    // search tablet
    Table::TabletList::iterator it2 = table.tablets_list_.upper_bound(key_start);
    CHECK(it2 != table.tablets_list_.begin());
    --it2;
    while (it2 != table.tablets_list_.end() &&
           (key_end.empty() || it2->second->meta_.key_range().key_start() < key_end)) {
        tablets->push_back(it2->second);
        ++it2;
    }
    table.mutex_.Unlock();
    CHECK_GT(tablets->size(), 0u);
    return true;
}

bool TabletManager::FindTable(const std::string& table_name,
                              std::vector<TabletPtr>* tablet_meta_list,
                              StatusCode* ret_status) {
    // lock table list
    mutex_.Lock();

    // search table
    TableList::iterator it = all_tables_.find(table_name);
    if (it == all_tables_.end()) {
        mutex_.Unlock();
        LOG(WARNING) << "table: " << table_name << " not exist";
        SetStatusCode(kTableNotFound, ret_status);
        return false;
    }
    Table& table = *it->second;

    // lock table
    table.mutex_.Lock();
    mutex_.Unlock();

    // search tablet
    Table::TabletList::iterator it2 = table.tablets_list_.begin();
    for (; it2 != table.tablets_list_.end(); ++it2) {
        TabletPtr tablet = it2->second;
        tablet_meta_list->push_back(tablet);
    }

    table.mutex_.Unlock();
    return true;
}

bool TabletManager::FindTable(const std::string& table_name, TablePtr* tablet) {
    mutex_.Lock();
    TableList::iterator it = all_tables_.find(table_name);
    if (it == all_tables_.end()) {
        mutex_.Unlock();
        VLOG(5) << "table: " << table_name << " not exist";
        return false;
    }
    *tablet = it->second;
    mutex_.Unlock();
    return true;
}

int64_t TabletManager::SearchTable(std::vector<TabletPtr>* tablet_meta_list,
                                   const std::string& prefix_table_name,
                                   const std::string& start_table_name,
                                   const std::string& start_tablet_key,
                                   uint32_t max_found, StatusCode* ret_status) {
    if (max_found == 0) {
        return 0;
    }
    if (start_table_name.find(prefix_table_name) != 0) {
        return 0;
    }

    mutex_.Lock();

    TableList::iterator lower_it = all_tables_.lower_bound(start_table_name);
    TableList::iterator upper_it = all_tables_.upper_bound(prefix_table_name + "\xFF");
    if (upper_it == all_tables_.begin() || lower_it == all_tables_.end()) {
        mutex_.Unlock();
        SetStatusCode(kTableNotFound, ret_status);
        return -1;
    }

    uint32_t found_num = 0;
    for (TableList::iterator it = lower_it; it != upper_it; ++it) {
        Table& table = *it->second;
        Table::TabletList::iterator it2;
        table.mutex_.Lock();
        if (start_table_name == it->first) {
            it2 = table.tablets_list_.lower_bound(start_tablet_key);
        } else {
            it2 = table.tablets_list_.begin();
        }

        for (; it2 != table.tablets_list_.end(); ++it2) {
            TabletPtr tablet = it2->second;
            tablet_meta_list->push_back(tablet);
            if (++found_num >= max_found) {
                break;
            }
        }
        table.mutex_.Unlock();
        if (found_num >= max_found) {
            break;
        }
    }

    mutex_.Unlock();
    return found_num;
}

bool TabletManager::ShowTable(std::vector<TablePtr>* table_meta_list,
                              std::vector<TabletPtr>* tablet_meta_list,
                              const std::string& start_table_name,
                              const std::string& start_tablet_key,
                              uint32_t max_table_found,
                              uint32_t max_tablet_found,
                              bool* is_more, StatusCode* ret_status) {
    // lock table list
    mutex_.Lock();

    TableList::iterator it = all_tables_.lower_bound(start_table_name);
    if (it == all_tables_.end()) {
        mutex_.Unlock();
        LOG(ERROR) << "table not found: " << start_table_name;
        SetStatusCode(kTableNotFound, ret_status);
        return false;
    }

    uint32_t table_found_num = 0;
    uint32_t tablet_found_num = 0;
    for (; it != all_tables_.end(); ++it) {
        TablePtr table = it->second;
        Table::TabletList::iterator it2;

        table->mutex_.Lock();
        if (table_meta_list != NULL) {
            table_meta_list->push_back(table);
        }
        table_found_num++;
        if (it->first == start_table_name) {
            it2 = table->tablets_list_.lower_bound(start_tablet_key);
        } else {
            it2 = table->tablets_list_.begin();
        }
        for (; it2 != table->tablets_list_.end(); ++it2) {
            if (tablet_found_num >= max_tablet_found) {
                break;
            }
            TabletPtr tablet = it2->second;
            tablet_found_num++;
            if (tablet_meta_list != NULL) {
                tablet_meta_list->push_back(tablet);
            }
        }
        table->mutex_.Unlock();
        if (table_found_num >= max_table_found) {
            break;
        }
    }

    mutex_.Unlock();
    return true;
}

bool TabletManager::DeleteTable(const std::string& table_name,
                                StatusCode* ret_status) {
    // lock table list
    MutexLock lock(&mutex_);

    // search table
    TableList::iterator it = all_tables_.find(table_name);
    if (it == all_tables_.end()) {
        LOG(WARNING) << "table: " << table_name << " not exist";
        SetStatusCode(kTableNotFound, ret_status);
        return true;
    }
    Table& table = *it->second;

    // make sure no other thread ref this table
    table.mutex_.Lock();
    table.mutex_.Unlock();

    table.tablets_list_.clear();
//    // delete every tablet
//    Table::TabletList::iterator it2 = table.tablets_list_.begin();
//    for (; it2 != table.tablets_list_.end(); ++it) {
//        Tablet& tablet = *it2->second;
//        // make sure no other thread ref this tablet
//        tablet.mutex_.Lock();
//        tablet.mutex_.Unlock();
//        delete &tablet;
//        table.tablets_list_.erase(it2);
//    }

    // delete &table;
    all_tables_.erase(it);
    return true;
}

bool TabletManager::DeleteTablet(const std::string& table_name,
                                 const std::string& key_start,
                                 StatusCode* ret_status) {
    // lock table list
    MutexLock lock(&mutex_);

    // search table
    TableList::iterator it = all_tables_.find(table_name);
    if (it == all_tables_.end()) {
        LOG(WARNING) << "table: " << table_name << " [start: "
            << DebugString(key_start) << "] not exist";
        SetStatusCode(kTableNotFound, ret_status);
        return true;
    }
    Table& table = *it->second;

    // make sure no other thread ref this table
    table.mutex_.Lock();
    table.mutex_.Unlock();

    // search tablet
    Table::TabletList::iterator it2 = table.tablets_list_.find(key_start);
    if (it2 == table.tablets_list_.end()) {
        LOG(WARNING) << "table: " << table_name << " [start: "
            << DebugString(key_start) << "] not exist";
        SetStatusCode(kTableNotFound, ret_status);
        return true;
    }
//    Tablet& tablet = *it2->second;
//    // make sure no other thread ref this tablet
//    tablet.mutex_.Lock();
//    tablet.mutex_.Unlock();
//    delete &tablet;
    table.tablets_list_.erase(it2);

    if (table.tablets_list_.empty()) {
        // clean up specific table dir in file system
        if (FLAGS_tera_delete_obsolete_tabledir_enabled &&
            !io::MoveEnvDirToTrash(table.GetTableName())) {
            LOG(ERROR) << "fail to move droped table to trash dir, tablename: "
                << table.GetTableName();
        }
        // delete &table;
        all_tables_.erase(it);
    }
    return true;
}

void TabletManager::WriteToStream(std::ofstream& ofs,
                                  const std::string& key,
                                  const std::string& value) {
    uint32_t key_size = key.size();
    uint32_t value_size = value.size();
    ofs.write((char*)&key_size, sizeof(key_size));
    ofs.write(key.data(), key_size);
    ofs.write((char*)&value_size, sizeof(value_size));
    ofs.write(value.data(), value_size);
}

bool TabletManager::DumpMetaTableToFile(const std::string& filename,
                                        StatusCode* status) {
    std::ofstream ofs(filename.c_str(), std::ofstream::binary | std::ofstream::trunc);
    if (!ofs.is_open()) {
        LOG(WARNING) << "fail to open file " << filename << " for write";
        SetStatusCode(kIOError, status);
        return false;
    }

    // get all table and tablet meta
    std::vector<TablePtr> table_list;
    std::vector<TabletPtr> tablet_list;
    ShowTable(&table_list, &tablet_list);

    // dump table meta
    for (size_t i = 0; i < table_list.size(); i++) {
        TablePtr table = table_list[i];
        std::string key, value;
        table->ToMetaTableKeyValue(&key, &value);
        WriteToStream(ofs, key, value);
    }

    // dump tablet meta
    for (size_t i = 0; i < tablet_list.size(); i++) {
        TabletPtr tablet = tablet_list[i];
        std::string key, value;
        tablet->ToMetaTableKeyValue(&key, &value);
        WriteToStream(ofs, key, value);
    }

    if (ofs.fail()) {
        LOG(WARNING) << "fail to write to file " << filename;
        SetStatusCode(kIOError, status);
        return false;
    }
    ofs.close();
    return true;
}

void TabletManager::LoadTableMeta(const std::string& key,
                                  const std::string& value) {
    TableMeta meta;
    ParseMetaTableKeyValue(key, value, &meta);
    TablePtr table;
    StatusCode ret_status = kTabletNodeOk;
    if (meta.table_name() == FLAGS_tera_master_meta_table_name) {
        LOG(INFO) << "ignore meta table record in meta table";
    } else if (!AddTable(meta.table_name(), meta, &table, &ret_status)) {
        LOG(ERROR) << "duplicate table in meta table: table="
            << meta.table_name();
        // TODO: try correct invalid record
    } else {
        VLOG(5) << "load table record: " << table;
    }
}

void TabletManager::LoadTabletMeta(const std::string& key,
                                   const std::string& value) {
    TabletMeta meta;
    ParseMetaTableKeyValue(key, value, &meta);
    TabletPtr tablet;
    StatusCode ret_status = kTabletNodeOk;
    if (meta.table_name() == FLAGS_tera_master_meta_table_name) {
        LOG(INFO) << "ignore meta tablet record in meta table";
    } else {
        TablePtr table;
        if (!FindTable(meta.table_name(), &table)) {
            LOG(WARNING) << "table schema not exist, skip this tablet: "
                << meta.path();
            return;
        }
        meta.set_status(kTableNotInit);
        if (!AddTablet(meta, table->GetSchema(), &tablet, &ret_status)) {
            LOG(ERROR) << "duplicate tablet in meta table: table=" << meta.table_name()
                << " start=" << DebugString(meta.key_range().key_start());
            // TODO: try correct invalid record
        }
    }
}

bool TabletManager::ClearMetaTable(const std::string& meta_tablet_addr,
                                   StatusCode* ret_status) {
    WriteTabletRequest write_request;
    WriteTabletResponse write_response;

    ScanTabletRequest scan_request;
    ScanTabletResponse scan_response;
    scan_request.set_sequence_id(this_sequence_id_->Inc());
    scan_request.set_table_name(FLAGS_tera_master_meta_table_name);
    scan_request.set_start("");
    scan_request.set_end("");

    tabletnode::TabletNodeClient meta_node_client(meta_tablet_addr);

    bool scan_success = false;
    while (meta_node_client.ScanTablet(&scan_request, &scan_response)) {
        if (scan_response.status() != kTabletNodeOk) {
            SetStatusCode(scan_response.status(), ret_status);
            LOG(WARNING) << "fail to scan meta table: "
                << StatusCodeToString(scan_response.status());
            return false;
        }
        if (scan_response.results().key_values_size() <= 0) {
            LOG(INFO) << "scan meta table success";
            scan_success = true;
            break;
        }
        uint32_t record_size = scan_response.results().key_values_size();
        std::string last_record_key;
        for (uint32_t i = 0; i < record_size; i++) {
            const KeyValuePair& record = scan_response.results().key_values(i);
            last_record_key = record.key();
            RowMutationSequence* mu_seq = write_request.add_row_list();
            mu_seq->set_row_key(record.key());
            Mutation* mutation = mu_seq->add_mutation_sequence();
            mutation->set_type(kDeleteRow);
        }
        std::string next_record_key = NextKey(last_record_key);
        scan_request.set_start(next_record_key);
        scan_request.set_end("");
        scan_request.set_sequence_id(this_sequence_id_->Inc());
        scan_response.Clear();
    }

    if (!scan_success) {
        SetStatusCode(kRPCError, ret_status);
        LOG(WARNING) << "fail to scan meta table: "
            << StatusCodeToString(kRPCError);
        return false;
    }

    write_request.set_sequence_id(this_sequence_id_->Inc());
    write_request.set_tablet_name(FLAGS_tera_master_meta_table_name);
    if (!meta_node_client.WriteTablet(&write_request, &write_response)) {
        SetStatusCode(kRPCError, ret_status);
        LOG(WARNING) << "fail to clear meta tablet: "
            << StatusCodeToString(kRPCError);
        return false;
    }
    StatusCode status = write_response.status();
    if (status == kTabletNodeOk && write_response.row_status_list_size() > 0) {
        status = write_response.row_status_list(0);
    }
    if (status != kTabletNodeOk) {
        SetStatusCode(status, ret_status);
        LOG(WARNING) << "fail to clear meta tablet: "
            << StatusCodeToString(status);
        return false;
    }

    LOG(INFO) << "clear meta tablet";
    return true;
}

bool TabletManager::DumpMetaTable(const std::string& meta_tablet_addr,
                                  StatusCode* ret_status) {
    std::vector<TablePtr> tables;
    std::vector<TabletPtr> tablets;
    ShowTable(&tables, &tablets);

    WriteTabletRequest request;
    WriteTabletResponse response;
    request.set_sequence_id(this_sequence_id_->Inc());
    request.set_tablet_name(FLAGS_tera_master_meta_table_name);
    request.set_is_sync(true);
    request.set_is_instant(true);
    // dump table record
    for (size_t i = 0; i < tables.size(); i++) {
        std::string packed_key;
        std::string packed_value;
        tables[i]->ToMetaTableKeyValue(&packed_key, &packed_value);
        RowMutationSequence* mu_seq = request.add_row_list();
        mu_seq->set_row_key(packed_key);
        Mutation* mutation = mu_seq->add_mutation_sequence();
        mutation->set_type(kPut);
        mutation->set_value(packed_value);
    }
    // dump tablet record
    uint64_t request_size = 0;
    for (size_t i = 0; i < tablets.size(); i++) {
        std::string packed_key;
        std::string packed_value;
        if (tablets[i]->GetPath().empty()) {
            std::string path = leveldb::GetTabletPathFromNum(tablets[i]->GetTableName(),
                                                             tablets[i]->GetTable()->GetNextTabletNo());
            tablets[i]->meta_.set_path(path);
        }
        tablets[i]->ToMetaTableKeyValue(&packed_key, &packed_value);
        RowMutationSequence* mu_seq = request.add_row_list();
        mu_seq->set_row_key(packed_key);
        Mutation* mutation = mu_seq->add_mutation_sequence();
        mutation->set_type(kPut);
        mutation->set_value(packed_value);
        request_size += mu_seq->ByteSize();

        if (i == tablets.size() - 1 || request_size >= kMaxRpcSize) {
            tabletnode::TabletNodeClient meta_node_client(meta_tablet_addr);
            if (!meta_node_client.WriteTablet(&request, &response)) {
                SetStatusCode(kRPCError, ret_status);
                LOG(WARNING) << "fail to dump meta tablet: "
                    << StatusCodeToString(kRPCError);
                return false;
            }
            StatusCode status = response.status();
            if (status == kTabletNodeOk && response.row_status_list_size() > 0) {
                status = response.row_status_list(0);
            }
            if (status != kTabletNodeOk) {
                SetStatusCode(status, ret_status);
                LOG(WARNING) << "fail to dump meta tablet: "
                    << StatusCodeToString(status);
                return false;
            }
            request.clear_row_list();
            response.Clear();
            request_size = 0;
        }
    }

    LOG(INFO) << "dump meta tablet";
    return true;
}

void TabletManager::ClearTableList() {
    MutexLock lock(&mutex_);
    TableList::iterator it = all_tables_.begin();
    for (; it != all_tables_.end(); ++it) {
        Table& table = *it->second;
        table.mutex_.Lock();
        table.mutex_.Unlock();
        table.tablets_list_.clear();
        //delete &table;
    }
    all_tables_.clear();
}

void TabletManager::PackTabletMeta(TabletMeta* meta,
                                   const std::string& table_name,
                                   const std::string& key_start,
                                   const std::string& key_end,
                                   const std::string& path,
                                   const std::string& server_addr,
                                   const TabletStatus& table_status,
                                   int64_t data_size) {
    meta->set_table_name(table_name);
    meta->set_path(path);
    meta->set_server_addr(server_addr);
    meta->set_status(table_status);
    meta->set_size(data_size);

    KeyRange* key_range = meta->mutable_key_range();
    key_range->set_key_start(key_start);
    key_range->set_key_end(key_end);
}

bool TabletManager::GetMetaTabletAddr(std::string* addr) {
    TabletPtr meta_tablet;
    if (FindTablet(FLAGS_tera_master_meta_table_name, "", &meta_tablet)
        && meta_tablet->GetStatus() == kTableReady) {
        *addr = meta_tablet->GetServerAddr();
        return true;
    }
    VLOG(5) << "fail to get meta addr";
    return false;
}

bool TabletManager::PickMergeTablet(TabletPtr& tablet, TabletPtr* tablet2) {
    std::string table_name = tablet->GetTableName();

    mutex_.Lock();
    // search table
    TableList::iterator it = all_tables_.find(table_name);
    if (it == all_tables_.end()) {
        mutex_.Unlock();
        LOG(ERROR) << "[merge] table: " << table_name << " not exist";
        return false;
    }
    Table& table = *it->second;
    MutexLock table_lock(&table.mutex_);
    mutex_.Unlock();

    if (table.tablets_list_.size() < 2) {
        VLOG(20) << "[merge] table: " << table_name << " only have 1 tablet.";
        return false;
    }

    // search tablet
    Table::TabletList::iterator it2 = table.tablets_list_.find(tablet->GetKeyStart());
    if (it2 == table.tablets_list_.end()) {
        LOG(ERROR) << "[merge] table: " << table_name << " [start: "
            << DebugString(tablet->GetKeyStart()) << "] not exist";
        return false;
    }
    TabletPtr prev, next;
    if (it2 != table.tablets_list_.begin()) {
        it2--;
        prev = it2->second;
        it2++;
    } else {
        // only have 1 neighbour tablet
        *tablet2 = (++it2)->second;
        if ((*tablet2)->GetDataSize() < 0) {
            // tablet not ready, skip merge
            return false;
        }
        return true;
    }
    if (++it2 != table.tablets_list_.end()) {
        next = it2->second;
    } else {
        // only have 1 neighbour tablet
        *tablet2 = prev;
        if ((*tablet2)->GetDataSize() < 0) {
            // tablet not ready, skip merge
            return false;
        }
        return true;
    }
    if (prev->GetDataSize() < 0 || next->GetDataSize() < 0) {
        // some tablet not ready, skip merge
        return false;
    }
    // choose the smaller neighbour tablet
    *tablet2 = prev->GetDataSize() > next->GetDataSize() ? next : prev;
    return true;
}

bool TabletManager::RpcChannelHealth(int32_t err_code) {
    return err_code != sofa::pbrpc::RPC_ERROR_CONNECTION_CLOSED
        && err_code != sofa::pbrpc::RPC_ERROR_SERVER_SHUTDOWN
        && err_code != sofa::pbrpc::RPC_ERROR_SERVER_UNREACHABLE
        && err_code != sofa::pbrpc::RPC_ERROR_SERVER_UNAVAILABLE;
}

void TabletManager::TryMajorCompact(Tablet* tablet) {
    if (!tablet) {
        VLOG(5) << "TryMajorCompact() tablet is NULL";
        return;
    }
    VLOG(5) << "TryMajorCompact() for " << tablet->meta_.path();
    MutexLock lock(&tablet->mutex_);
    if (tablet->meta_.compact_status() != kTableNotCompact) {
        return;
    } else {
        tablet->meta_.set_compact_status(kTableOnCompact);
    }

    CompactTabletRequest* request = new CompactTabletRequest;
    CompactTabletResponse* response = new CompactTabletResponse;
    request->set_sequence_id(this_sequence_id_->Inc());
    request->set_tablet_name(tablet->meta_.table_name());
    request->mutable_key_range()->CopyFrom(tablet->meta_.key_range());

    tabletnode::TabletNodeClient node_client(tablet->meta_.server_addr());
    std::function<void (CompactTabletRequest*, CompactTabletResponse*, bool, int)> done =
        std::bind(&TabletManager::MajorCompactCallback, this, tablet,
                   FLAGS_tera_master_impl_retry_times, _1, _2, _3, _4);
    node_client.CompactTablet(request, response, done);
}

void TabletManager::MajorCompactCallback(Tablet* tb, int32_t retry,
                                         CompactTabletRequest* request,
                                         CompactTabletResponse* response,
                                         bool failed, int error_code) {
    VLOG(9) << "MajorCompactCallback() for " << tb->meta_.path()
        << ", status: " << StatusCodeToString(tb->meta_.compact_status())
        << ", retry: " << retry;
    {
        MutexLock lock(&tb->mutex_);
        if (tb->meta_.compact_status() == kTableCompacted) {
            return;
        }
    }

    if (failed || response->status() != kTabletNodeOk
        || response->compact_status() == kTableOnCompact
        || response->compact_size() == 0) {
        LOG(ERROR) << "fail to major compact for " << tb->meta_.path()
            << ", rpc status: " << StatusCodeToString(response->status())
            << ", compact status: " << StatusCodeToString(response->compact_status());
        if (retry <= 0 || !RpcChannelHealth(error_code)) {
            delete request;
            delete response;
        } else {
            int32_t wait_time = FLAGS_tera_tabletnode_connect_retry_period
                * (FLAGS_tera_master_impl_retry_times - retry);
            ThisThread::Sleep(wait_time);
            tabletnode::TabletNodeClient node_client(tb->meta_.server_addr());
            std::function<void (CompactTabletRequest*, CompactTabletResponse*, bool, int)> done =
                std::bind(&TabletManager::MajorCompactCallback, this, tb, retry - 1, _1, _2, _3, _4);
            node_client.CompactTablet(request, response, done);
        }
        return;
    }
    delete request;
    delete response;

    MutexLock lock(&tb->mutex_);
    tb->meta_.set_compact_status(kTableCompacted);
    VLOG(5) << "compact success: " << tb->meta_.path();
}

double TabletManager::OfflineTabletRatio() {
    uint32_t offline_tablet_count = 0, tablet_count = 0;
    mutex_.Lock();
    TableList::iterator it = all_tables_.begin();
    for (; it != all_tables_.end(); ++it) {
        Table& table = *it->second;
        table.mutex_.Lock();
        Table::TabletList::iterator it2 = table.tablets_list_.begin();
        for (; it2 != table.tablets_list_.end(); ++it2) {
            TabletPtr tablet = it2->second;
            if (tablet->GetStatus() == kTableOffLine) {
                offline_tablet_count++;
            }
            tablet_count++;
        }
        table.mutex_.Unlock();
    }
    mutex_.Unlock();

    if (tablet_count == 0) {
        return 0;
    }
    return (double)offline_tablet_count / tablet_count;
}

int64_t CounterWeightedSum(int64_t a1, int64_t a2) {
    const int64_t w1 = 2;
    const int64_t w2 = 1;
    return (a1 * w1 + a2 * w2) / (w1 + w2);
}

} // namespace master
} // namespace tera
