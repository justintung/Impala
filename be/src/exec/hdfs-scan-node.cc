// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "exec/hdfs-scan-node.h"

#include <sstream>

#include "common/logging.h"
#include "exec/hdfs-scanner.h"
#include "exec/scanner-context.h"
#include "runtime/descriptors.h"
#include "runtime/runtime-filter.inline.h"
#include "runtime/runtime-state.h"
#include "runtime/mem-tracker.h"
#include "runtime/row-batch.h"
#include "util/debug-util.h"
#include "util/disk-info.h"
#include "util/runtime-profile-counters.h"

#include "common/names.h"

DEFINE_int32(max_row_batches, 0, "the maximum size of materialized_row_batches_");

#ifndef NDEBUG
DECLARE_bool(skip_file_runtime_filtering);
#endif

using namespace impala;

// Amount of memory that we approximate a scanner thread will use not including IoBuffers.
// The memory used does not vary considerably between file formats (just a couple of MBs).
// This value is conservative and taken from running against the tpch lineitem table.
// TODO: revisit how we do this.
const int SCANNER_THREAD_MEM_USAGE = 32 * 1024 * 1024;

// Estimated upper bound on the compression ratio of compressed text files. Used to
// estimate scanner thread memory usage.
const int COMPRESSED_TEXT_COMPRESSION_RATIO = 11;

// Amount of time to block waiting for GetNext() to release scanner threads between
// checking if a scanner thread should yield itself back to the global thread pool.
const int SCANNER_THREAD_WAIT_TIME_MS = 20;

HdfsScanNode::HdfsScanNode(ObjectPool* pool, const TPlanNode& tnode,
                           const DescriptorTbl& descs)
    : HdfsScanNodeBase(pool, tnode, descs),
      ranges_issued_barrier_(1),
      scanner_thread_bytes_required_(0),
      done_(false),
      all_ranges_started_(false),
      thread_avail_cb_id_(-1),
      rm_callback_id_(-1),
      max_num_scanner_threads_(CpuInfo::num_cores()) {
  max_materialized_row_batches_ = FLAGS_max_row_batches;
  if (max_materialized_row_batches_ <= 0) {
    // TODO: This parameter has an U-shaped effect on performance: increasing the value
    // would first improve performance, but further increasing would degrade performance.
    // Investigate and tune this.
    max_materialized_row_batches_ =
        10 * (DiskInfo::num_disks() + DiskIoMgr::REMOTE_NUM_DISKS);
  }
  materialized_row_batches_.reset(new RowBatchQueue(max_materialized_row_batches_));
}

HdfsScanNode::~HdfsScanNode() {
}

Status HdfsScanNode::GetNext(RuntimeState* state, RowBatch* row_batch, bool* eos) {
  SCOPED_TIMER(runtime_profile_->total_time_counter());

  if (!initial_ranges_issued_) {
    // We do this in GetNext() to maximise the amount of work we can do while waiting for
    // runtime filters to show up. The scanner threads have already started (in Open()),
    // so we need to tell them there is work to do.
    // TODO: This is probably not worth splitting the organisational cost of splitting
    // initialisation across two places. Move to before the scanner threads start.
    RETURN_IF_ERROR(IssueInitialScanRanges(state));

    // Release the scanner threads
    ranges_issued_barrier_.Notify();

    if (progress_.done()) SetDone();
  }

  Status status = GetNextInternal(state, row_batch, eos);
  if (!status.ok() || *eos) {
    unique_lock<mutex> l(lock_);
    lock_guard<SpinLock> l2(file_type_counts_);
    StopAndFinalizeCounters();
  }
  return status;
}

Status HdfsScanNode::GetNextInternal(
    RuntimeState* state, RowBatch* row_batch, bool* eos) {
  RETURN_IF_ERROR(ExecDebugAction(TExecNodePhase::GETNEXT, state));
  RETURN_IF_CANCELLED(state);
  RETURN_IF_ERROR(QueryMaintenance(state));

  if (ReachedLimit()) {
    // LIMIT 0 case.  Other limit values handled below.
    DCHECK_EQ(limit_, 0);
    *eos = true;
    return Status::OK();
  }
  *eos = false;
  RowBatch* materialized_batch = materialized_row_batches_->GetBatch();
  if (materialized_batch != NULL) {
    num_owned_io_buffers_.Add(-materialized_batch->num_io_buffers());
    row_batch->AcquireState(materialized_batch);
    // Update the number of materialized rows now instead of when they are materialized.
    // This means that scanners might process and queue up more rows than are necessary
    // for the limit case but we want to avoid the synchronized writes to
    // num_rows_returned_.
    num_rows_returned_ += row_batch->num_rows();
    COUNTER_SET(rows_returned_counter_, num_rows_returned_);

    if (ReachedLimit()) {
      int num_rows_over = num_rows_returned_ - limit_;
      row_batch->set_num_rows(row_batch->num_rows() - num_rows_over);
      num_rows_returned_ -= num_rows_over;
      COUNTER_SET(rows_returned_counter_, num_rows_returned_);

      *eos = true;
      SetDone();
    }
    DCHECK_EQ(materialized_batch->num_io_buffers(), 0);
    delete materialized_batch;
    return Status::OK();
  }
  // The RowBatchQueue was shutdown either because all scan ranges are complete or a
  // scanner thread encountered an error.  Check status_ to distinguish those cases.
  *eos = true;
  unique_lock<mutex> l(lock_);
  return status_;
}

Status HdfsScanNode::Prepare(RuntimeState* state) {
  SCOPED_TIMER(runtime_profile_->total_time_counter());
  RETURN_IF_ERROR(HdfsScanNodeBase::Prepare(state));

  // Compute the minimum bytes required to start a new thread. This is based on the
  // file format.
  // The higher the estimate, the less likely it is the query will fail but more likely
  // the query will be throttled when it does not need to be.
  // TODO: how many buffers should we estimate per range. The IoMgr will throttle down to
  // one but if there are already buffers queued before memory pressure was hit, we can't
  // reclaim that memory.
  if (per_type_files_[THdfsFileFormat::PARQUET].size() > 0) {
    // Parquet files require buffers per column
    scanner_thread_bytes_required_ =
        materialized_slots_.size() * 3 * runtime_state_->io_mgr()->max_read_buffer_size();
  } else {
    scanner_thread_bytes_required_ =
        3 * runtime_state_->io_mgr()->max_read_buffer_size();
  }
  // scanner_thread_bytes_required_ now contains the IoBuffer requirement.
  // Next we add in the other memory the scanner thread will use.
  // e.g. decompression buffers, tuple buffers, etc.
  // For compressed text, we estimate this based on the file size (since the whole file
  // will need to be decompressed at once). For all other formats, we use a constant.
  // TODO: can we do something better?
  int64_t scanner_thread_mem_usage = SCANNER_THREAD_MEM_USAGE;
  for (HdfsFileDesc* file: per_type_files_[THdfsFileFormat::TEXT]) {
    if (file->file_compression != THdfsCompression::NONE) {
      int64_t bytes_required = file->file_length * COMPRESSED_TEXT_COMPRESSION_RATIO;
      scanner_thread_mem_usage = ::max(bytes_required, scanner_thread_mem_usage);
    }
  }
  scanner_thread_bytes_required_ += scanner_thread_mem_usage;
  return Status::OK();
}

// This function registers the ThreadTokenAvailableCb to start up the initial scanner
// threads. Scan ranges are not issued until the first GetNext() call; scanner threads
// will block on ranges_issued_barrier_ until ranges are issued.
Status HdfsScanNode::Open(RuntimeState* state) {
  SCOPED_TIMER(runtime_profile_->total_time_counter());
  RETURN_IF_ERROR(HdfsScanNodeBase::Open(state));

  if (file_descs_.empty() || progress_.done()) return Status::OK();

  // We need at least one scanner thread to make progress. We need to make this
  // reservation before any ranges are issued.
  runtime_state_->resource_pool()->ReserveOptionalTokens(1);
  if (runtime_state_->query_options().num_scanner_threads > 0) {
    max_num_scanner_threads_ = runtime_state_->query_options().num_scanner_threads;
  }
  DCHECK_GT(max_num_scanner_threads_, 0);

  thread_avail_cb_id_ = runtime_state_->resource_pool()->AddThreadAvailableCb(
      bind<void>(mem_fn(&HdfsScanNode::ThreadTokenAvailableCb), this, _1));

  return Status::OK();
}

void HdfsScanNode::Close(RuntimeState* state) {
  if (is_closed()) return;
  SetDone();

  if (thread_avail_cb_id_ != -1) {
    state->resource_pool()->RemoveThreadAvailableCb(thread_avail_cb_id_);
  }

  scanner_threads_.JoinAll();

  num_owned_io_buffers_.Add(-materialized_row_batches_->Cleanup());
  DCHECK_EQ(num_owned_io_buffers_.Load(), 0) << "ScanNode has leaked io buffers";

  HdfsScanNodeBase::Close(state);
}

void HdfsScanNode::SetFileMetadata(const string& filename, void* metadata) {
  unique_lock<mutex> l(metadata_lock_);
  DCHECK(per_file_metadata_.find(filename) == per_file_metadata_.end());
  per_file_metadata_[filename] = metadata;
}

void* HdfsScanNode::GetFileMetadata(const string& filename) {
  unique_lock<mutex> l(metadata_lock_);
  map<string, void*>::iterator it = per_file_metadata_.find(filename);
  if (it == per_file_metadata_.end()) return NULL;
  return it->second;
}

void HdfsScanNode::RangeComplete(const THdfsFileFormat::type& file_type,
    const std::vector<THdfsCompression::type>& compression_type) {
  lock_guard<SpinLock> l(file_type_counts_);
  HdfsScanNodeBase::RangeComplete(file_type, compression_type);
}

void HdfsScanNode::TransferToScanNodePool(MemPool* pool) {
  unique_lock<mutex> l(lock_);
  scan_node_pool_->AcquireData(pool, false);
}

void HdfsScanNode::AddMaterializedRowBatch(RowBatch* row_batch) {
  InitNullCollectionValues(row_batch);
  materialized_row_batches_->AddBatch(row_batch);
}

Status HdfsScanNode::AddDiskIoRanges(const vector<DiskIoMgr::ScanRange*>& ranges,
    int num_files_queued) {
  RETURN_IF_ERROR(
      runtime_state_->io_mgr()->AddScanRanges(reader_context_, ranges));
  num_unqueued_files_.Add(-num_files_queued);
  DCHECK_GE(num_unqueued_files_.Load(), 0);
  if (!ranges.empty()) ThreadTokenAvailableCb(runtime_state_->resource_pool());
  return Status::OK();
}

// For controlling the amount of memory used for scanners, we approximate the
// scanner mem usage based on scanner_thread_bytes_required_, rather than the
// consumption in the scan node's mem tracker. The problem with the scan node
// trackers is that it does not account for the memory the scanner will use.
// For example, if there is 110 MB of memory left (based on the mem tracker)
// and we estimate that a scanner will use 100MB, we want to make sure to only
// start up one additional thread. However, after starting the first thread, the
// mem tracker value will not change immediately (it takes some time before the
// scanner is running and starts using memory). Therefore we just use the estimate
// based on the number of running scanner threads.
bool HdfsScanNode::EnoughMemoryForScannerThread(bool new_thread) {
  int64_t committed_scanner_mem =
      active_scanner_thread_counter_.value() * scanner_thread_bytes_required_;
  int64_t tracker_consumption = mem_tracker()->consumption();
  int64_t est_additional_scanner_mem = committed_scanner_mem - tracker_consumption;
  if (est_additional_scanner_mem < 0) {
    // This is the case where our estimate was too low. Expand the estimate based
    // on the usage.
    int64_t avg_consumption =
        tracker_consumption / active_scanner_thread_counter_.value();
    // Take the average and expand it by 50%. Some scanners will not have hit their
    // steady state mem usage yet.
    // TODO: how can we scale down if we've overestimated.
    // TODO: better heuristic?
    scanner_thread_bytes_required_ = static_cast<int64_t>(avg_consumption * 1.5);
    est_additional_scanner_mem = 0;
  }

  // If we are starting a new thread, take that into account now.
  if (new_thread) est_additional_scanner_mem += scanner_thread_bytes_required_;
  return est_additional_scanner_mem < mem_tracker()->SpareCapacity();
}

void HdfsScanNode::ThreadTokenAvailableCb(ThreadResourceMgr::ResourcePool* pool) {
  // This is called to start up new scanner threads. It's not a big deal if we
  // spin up more than strictly necessary since they will go through and terminate
  // promptly. However, we want to minimize that by checking a conditions.
  //  1. Don't start up if the ScanNode is done
  //  2. Don't start up if all the ranges have been taken by another thread.
  //  3. Don't start up if the number of ranges left is less than the number of
  //     active scanner threads.
  //  4. Don't start up if no initial ranges have been issued (see IMPALA-1722).
  //  5. Don't start up a ScannerThread if materialized_row_batches_ is full since
  //     we are not scanner bound.
  //  6. Don't start up a thread if there isn't enough memory left to run it.
  //  7. Don't start up more than maximum number of scanner threads configured.
  //  8. Don't start up if there are no thread tokens.

  // Case 4. We have not issued the initial ranges so don't start a scanner thread.
  // Issuing ranges will call this function and we'll start the scanner threads then.
  // TODO: It would be good to have a test case for that.
  if (!initial_ranges_issued_) return;

  while (true) {
    // The lock must be given up between loops in order to give writers to done_,
    // all_ranges_started_ etc. a chance to grab the lock.
    // TODO: This still leans heavily on starvation-free locks, come up with a more
    // correct way to communicate between this method and ScannerThreadHelper
    unique_lock<mutex> lock(lock_);
    // Cases 1, 2, 3.
    if (done_ || all_ranges_started_ ||
        active_scanner_thread_counter_.value() >= progress_.remaining()) {
      break;
    }

    // Cases 5 and 6.
    if (active_scanner_thread_counter_.value() > 0 &&
        (materialized_row_batches_->GetSize() >= max_materialized_row_batches_ ||
         !EnoughMemoryForScannerThread(true))) {
      break;
    }

    // Case 7 and 8.
    bool is_reserved = false;
    if (active_scanner_thread_counter_.value() >= max_num_scanner_threads_ ||
        !pool->TryAcquireThreadToken(&is_reserved)) {
      break;
    }

    COUNTER_ADD(&active_scanner_thread_counter_, 1);
    COUNTER_ADD(num_scanner_threads_started_counter_, 1);
    stringstream ss;
    ss << "scanner-thread(" << num_scanner_threads_started_counter_->value() << ")";
    scanner_threads_.AddThread(
        new Thread("hdfs-scan-node", ss.str(), &HdfsScanNode::ScannerThread, this));
  }
}

void HdfsScanNode::ScannerThread() {
  SCOPED_THREAD_COUNTER_MEASUREMENT(scanner_thread_counters());
  SCOPED_TIMER(runtime_state_->total_cpu_timer());

  // Make thread-local copy of filter contexts to prune scan ranges, and to pass to the
  // scanner for finer-grained filtering.
  vector<FilterContext> filter_ctxs;
  Status filter_status = Status::OK();
  for (auto& filter_ctx: filter_ctxs_) {
    FilterContext filter;
    filter_status = filter.CloneFrom(filter_ctx, runtime_state_);
    if (!filter_status.ok()) break;
    filter_ctxs.push_back(filter);
  }

  while (!done_) {
    {
      // Check if we have enough resources (thread token and memory) to keep using
      // this thread.
      unique_lock<mutex> l(lock_);
      if (active_scanner_thread_counter_.value() > 1) {
        if (runtime_state_->resource_pool()->optional_exceeded() ||
            !EnoughMemoryForScannerThread(false)) {
          // We can't break here. We need to update the counter with the lock held or else
          // all threads might see active_scanner_thread_counter_.value > 1
          COUNTER_ADD(&active_scanner_thread_counter_, -1);
          // Unlock before releasing the thread token to avoid deadlock in
          // ThreadTokenAvailableCb().
          l.unlock();
          runtime_state_->resource_pool()->ReleaseThreadToken(false);
          if (filter_status.ok()) {
            for (auto& ctx: filter_ctxs) {
              ctx.expr->FreeLocalAllocations();
              ctx.expr->Close(runtime_state_);
            }
          }
          return;
        }
      } else {
        // If this is the only scanner thread, it should keep running regardless
        // of resource constraints.
      }
    }

    bool unused = false;
    // Wake up every SCANNER_THREAD_COUNTERS to yield scanner threads back if unused, or
    // to return if there's an error.
    ranges_issued_barrier_.Wait(SCANNER_THREAD_WAIT_TIME_MS, &unused);

    DiskIoMgr::ScanRange* scan_range;
    // Take a snapshot of num_unqueued_files_ before calling GetNextRange().
    // We don't want num_unqueued_files_ to go to zero between the return from
    // GetNextRange() and the check for when all ranges are complete.
    int num_unqueued_files = num_unqueued_files_.Load();
    // TODO: the Load() acts as an acquire barrier.  Is this needed? (i.e. any earlier
    // stores that need to complete?)
    AtomicUtil::MemoryBarrier();
    Status status = runtime_state_->io_mgr()->GetNextRange(reader_context_, &scan_range);

    if (status.ok() && scan_range != NULL) {
      // Got a scan range. Process the range end to end (in this thread).
      status = ProcessSplit(filter_status.ok() ? filter_ctxs : vector<FilterContext>(),
          scan_range);
    }

    if (!status.ok()) {
      {
        unique_lock<mutex> l(lock_);
        // If there was already an error, the main thread will do the cleanup
        if (!status_.ok()) break;

        if (status.IsCancelled() && done_) {
          // Scan node initiated scanner thread cancellation.  No need to do anything.
          break;
        }
        // Set status_ before calling SetDone() (which shuts down the RowBatchQueue),
        // to ensure that GetNextInternal() notices the error status.
        status_ = status;
      }

      SetDone();
      break;
    }

    // Done with range and it completed successfully
    if (progress_.done()) {
      // All ranges are finished.  Indicate we are done.
      SetDone();
      break;
    }

    if (scan_range == NULL && num_unqueued_files == 0) {
      // TODO: Based on the usage pattern of all_ranges_started_, it looks like it is not
      // needed to acquire the lock in x86.
      unique_lock<mutex> l(lock_);
      // All ranges have been queued and GetNextRange() returned NULL. This means that
      // every range is either done or being processed by another thread.
      all_ranges_started_ = true;
      break;
    }
  }

  if (filter_status.ok()) {
    for (auto& ctx: filter_ctxs) {
      ctx.expr->FreeLocalAllocations();
      ctx.expr->Close(runtime_state_);
    }
  }

  COUNTER_ADD(&active_scanner_thread_counter_, -1);
  runtime_state_->resource_pool()->ReleaseThreadToken(false);
}

namespace {

// Returns true if 'format' uses a scanner derived from BaseSequenceScanner. Used to
// workaround IMPALA-3798.
bool FileFormatIsSequenceBased(THdfsFileFormat::type format) {
  return format == THdfsFileFormat::SEQUENCE_FILE ||
      format == THdfsFileFormat::RC_FILE ||
      format == THdfsFileFormat::AVRO;
}

}

Status HdfsScanNode::ProcessSplit(const vector<FilterContext>& filter_ctxs,
    DiskIoMgr::ScanRange* scan_range) {

  DCHECK(scan_range != NULL);

  ScanRangeMetadata* metadata = static_cast<ScanRangeMetadata*>(scan_range->meta_data());
  int64_t partition_id = metadata->partition_id;
  HdfsPartitionDescriptor* partition = hdfs_table_->GetPartition(partition_id);
  DCHECK(partition != NULL) << "table_id=" << hdfs_table_->id()
                            << " partition_id=" << partition_id
                            << "\n" << PrintThrift(runtime_state_->fragment_params());

  // IMPALA-3798: Filtering before the scanner is created can cause hangs if a header
  // split is filtered out, for sequence-based file formats. If the scanner does not
  // process the header split, the remaining scan ranges in the file will not be marked as
  // done. See FilePassesFilterPredicates() for the correct logic to mark all splits in a
  // file as done; the correct fix here is to do that for every file in a thread-safe way.
  if (!FileFormatIsSequenceBased(partition->file_format())) {
    if (!PartitionPassesFilters(partition_id, FilterStats::SPLITS_KEY, filter_ctxs)) {
      // Avoid leaking unread buffers in scan_range.
      scan_range->Cancel(Status::CANCELLED);
      // Mark scan range as done.
      scan_ranges_complete_counter()->Add(1);
      progress_.Update(1);
      return Status::OK();
    }
  }

  ScannerContext context(runtime_state_, this, partition, scan_range, filter_ctxs);
  scoped_ptr<HdfsScanner> scanner;
  Status status = CreateAndOpenScanner(partition, &context, &scanner);
  if (!status.ok()) {
    // If preparation fails, avoid leaking unread buffers in the scan_range.
    scan_range->Cancel(status);

    if (VLOG_QUERY_IS_ON) {
      stringstream ss;
      ss << "Error preparing scanner for scan range " << scan_range->file() <<
          "(" << scan_range->offset() << ":" << scan_range->len() << ").";
      ss << endl << runtime_state_->ErrorLog();
      VLOG_QUERY << ss.str();
    }
    return status;
  }

  status = scanner->ProcessSplit();
  if (VLOG_QUERY_IS_ON && !status.ok() && !status.IsCancelled()) {
    // This thread hit an error, record it and bail
    stringstream ss;
    ss << "Scan node (id=" << id() << ") ran into a parse error for scan range "
       << scan_range->file() << "(" << scan_range->offset() << ":"
       << scan_range->len() << ").";
    // Parquet doesn't read the range end to end so the current offset isn't useful.
    // TODO: make sure the parquet reader is outputting as much diagnostic
    // information as possible.
    if (partition->file_format() != THdfsFileFormat::PARQUET) {
      ScannerContext::Stream* stream = context.GetStream();
      ss << " Processed " << stream->total_bytes_returned() << " bytes.";
    }
    VLOG_QUERY << ss.str();
  }

  // Transfer the remaining resources to the final row batch (if any) and add it to
  // the row batch queue.
  scanner->Close(scanner->batch());
  return status;
}

void HdfsScanNode::SetDone() {
  {
    unique_lock<mutex> l(lock_);
    if (done_) return;
    done_ = true;
  }
  if (reader_context_ != NULL) {
    runtime_state_->io_mgr()->CancelContext(reader_context_);
  }
  materialized_row_batches_->Shutdown();
}
