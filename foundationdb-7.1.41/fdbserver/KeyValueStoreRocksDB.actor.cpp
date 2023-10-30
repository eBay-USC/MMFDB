/*
 * KeyValueStoreRocksDB.actor.cpp
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2022 Apple Inc. and the FoundationDB project authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifdef SSD_ROCKSDB_EXPERIMENTAL

#include <rocksdb/cache.h>
#include <rocksdb/db.h>
#include <rocksdb/filter_policy.h>
#include <rocksdb/listener.h>
#include <rocksdb/options.h>
#include <rocksdb/metadata.h>
#include <rocksdb/slice_transform.h>
#include <rocksdb/sst_file_reader.h>
#include <rocksdb/sst_file_writer.h>
#include <rocksdb/slice.h>
#include <rocksdb/env.h>
#include <rocksdb/options.h>
#include <rocksdb/statistics.h>
#include <rocksdb/table.h>
#include <rocksdb/version.h>
#include <rocksdb/types.h>
#include <rocksdb/utilities/checkpoint.h>
#include <rocksdb/utilities/table_properties_collectors.h>
#include <rocksdb/version.h>

#include <rocksdb/rate_limiter.h>
#include <rocksdb/perf_context.h>
#include <rocksdb/c.h>
#if defined __has_include
#if __has_include(<liburing.h>)
#include <liburing.h>
#endif
#endif
#include "fdbclient/SystemData.h"
#include "fdbserver/CoroFlow.h"
#include "fdbserver/FDBRocksDBVersion.h"
#include "flow/flow.h"
#include "flow/IThreadPool.h"
#include "flow/ThreadHelper.actor.h"
#include "flow/Histogram.h"

#include <memory>
#include <tuple>
#include <vector>

#endif // SSD_ROCKSDB_EXPERIMENTAL

#include "fdbserver/IKeyValueStore.h"
#include "fdbserver/RocksDBCheckpointUtils.actor.h"

#include "flow/actorcompiler.h" // has to be last include

#ifdef SSD_ROCKSDB_EXPERIMENTAL

// Enforcing rocksdb version.
static_assert((ROCKSDB_MAJOR == FDB_ROCKSDB_MAJOR && ROCKSDB_MINOR == FDB_ROCKSDB_MINOR &&
               ROCKSDB_PATCH == FDB_ROCKSDB_PATCH),
              "Unsupported rocksdb version.");

namespace {
using rocksdb::BackgroundErrorReason;
using DB = rocksdb::DB*;
using CF = rocksdb::ColumnFamilyHandle*;

#define PERSIST_PREFIX "\xff\xff"
const KeyRef persistVersion = LiteralStringRef(PERSIST_PREFIX "Version");
const StringRef ROCKSDBSTORAGE_HISTOGRAM_GROUP = LiteralStringRef("RocksDBStorage");
const StringRef ROCKSDB_COMMIT_LATENCY_HISTOGRAM = LiteralStringRef("RocksDBCommitLatency");
const StringRef ROCKSDB_COMMIT_ACTION_HISTOGRAM = LiteralStringRef("RocksDBCommitAction");
const StringRef ROCKSDB_COMMIT_QUEUEWAIT_HISTOGRAM = LiteralStringRef("RocksDBCommitQueueWait");
const StringRef ROCKSDB_WRITE_HISTOGRAM = LiteralStringRef("RocksDBWrite");
const StringRef ROCKSDB_DELETE_COMPACTRANGE_HISTOGRAM = LiteralStringRef("RocksDBDeleteCompactRange");
const StringRef ROCKSDB_READRANGE_LATENCY_HISTOGRAM = LiteralStringRef("RocksDBReadRangeLatency");
const StringRef ROCKSDB_READVALUE_LATENCY_HISTOGRAM = LiteralStringRef("RocksDBReadValueLatency");
const StringRef ROCKSDB_READPREFIX_LATENCY_HISTOGRAM = LiteralStringRef("RocksDBReadPrefixLatency");
const StringRef ROCKSDB_READRANGE_ACTION_HISTOGRAM = LiteralStringRef("RocksDBReadRangeAction");
const StringRef ROCKSDB_READVALUE_ACTION_HISTOGRAM = LiteralStringRef("RocksDBReadValueAction");
const StringRef ROCKSDB_READPREFIX_ACTION_HISTOGRAM = LiteralStringRef("RocksDBReadPrefixAction");
const StringRef ROCKSDB_READRANGE_QUEUEWAIT_HISTOGRAM = LiteralStringRef("RocksDBReadRangeQueueWait");
const StringRef ROCKSDB_READVALUE_QUEUEWAIT_HISTOGRAM = LiteralStringRef("RocksDBReadValueQueueWait");
const StringRef ROCKSDB_READPREFIX_QUEUEWAIT_HISTOGRAM = LiteralStringRef("RocksDBReadPrefixQueueWait");
const StringRef ROCKSDB_READRANGE_NEWITERATOR_HISTOGRAM = LiteralStringRef("RocksDBReadRangeNewIterator");
const StringRef ROCKSDB_READVALUE_GET_HISTOGRAM = LiteralStringRef("RocksDBReadValueGet");
const StringRef ROCKSDB_READPREFIX_GET_HISTOGRAM = LiteralStringRef("RocksDBReadPrefixGet");
const StringRef ROCKSDB_READ_RANGE_BYTES_RETURNED_HISTOGRAM = LiteralStringRef("RocksDBReadRangeBytesReturned");
const StringRef ROCKSDB_READ_RANGE_KV_PAIRS_RETURNED_HISTOGRAM = LiteralStringRef("RocksDBReadRangeKVPairsReturned");

std::shared_ptr<rocksdb::Cache> rocksdb_block_cache = nullptr;

class SharedRocksDBState {
public:
	SharedRocksDBState(UID id);

	std::vector<std::shared_ptr<LatencySample>> readLatency;
	std::vector<std::shared_ptr<LatencySample>> scanLatency;
	std::vector<std::shared_ptr<LatencySample>> readQueueLatency;
	LatencySample commitLatency;
	LatencySample commitQueueLatency;
	LatencySample dbWriteLatency;

private:
	const UID id;
};

SharedRocksDBState::SharedRocksDBState(UID id)
  : id(id), commitLatency(LatencySample("RocksDBCommitLatency",
                                        id,
                                        SERVER_KNOBS->LATENCY_METRICS_LOGGING_INTERVAL,
                                        SERVER_KNOBS->LATENCY_SAMPLE_SIZE)),
    commitQueueLatency(LatencySample("RocksDBCommitQueueLatency",
                                     id,
                                     SERVER_KNOBS->LATENCY_METRICS_LOGGING_INTERVAL,
                                     SERVER_KNOBS->LATENCY_SAMPLE_SIZE)),
    dbWriteLatency(LatencySample("RocksDBWriteLatency",
                                 id,
                                 SERVER_KNOBS->LATENCY_METRICS_LOGGING_INTERVAL,
                                 SERVER_KNOBS->LATENCY_SAMPLE_SIZE)) {
	for (int i = 0; i < SERVER_KNOBS->ROCKSDB_READ_PARALLELISM; i++) {
		readLatency.push_back(std::make_shared<LatencySample>(format("RocksDBReadLatency-%d", i),
		                                                      id,
		                                                      SERVER_KNOBS->LATENCY_METRICS_LOGGING_INTERVAL,
		                                                      SERVER_KNOBS->LATENCY_SAMPLE_SIZE));
		scanLatency.push_back(std::make_shared<LatencySample>(format("RocksDBScanLatency-%d", i),
		                                                      id,
		                                                      SERVER_KNOBS->LATENCY_METRICS_LOGGING_INTERVAL,
		                                                      SERVER_KNOBS->LATENCY_SAMPLE_SIZE));
		readQueueLatency.push_back(std::make_shared<LatencySample>(format("RocksDBReadQueueLatency-%d", i),
		                                                           id,
		                                                           SERVER_KNOBS->LATENCY_METRICS_LOGGING_INTERVAL,
		                                                           SERVER_KNOBS->LATENCY_SAMPLE_SIZE));
	}
}

// Returns string representation of RocksDB background error reason.
// Error reason code:
// https://github.com/facebook/rocksdb/blob/12d798ac06bcce36be703b057d5f5f4dab3b270c/include/rocksdb/listener.h#L125
// This function needs to be updated when error code changes.
std::string getErrorReason(BackgroundErrorReason reason) {
	switch (reason) {
	case BackgroundErrorReason::kFlush:
		return format("%d Flush", reason);
	case BackgroundErrorReason::kCompaction:
		return format("%d Compaction", reason);
	case BackgroundErrorReason::kWriteCallback:
		return format("%d WriteCallback", reason);
	case BackgroundErrorReason::kMemTable:
		return format("%d MemTable", reason);
	case BackgroundErrorReason::kManifestWrite:
		return format("%d ManifestWrite", reason);
	case BackgroundErrorReason::kFlushNoWAL:
		return format("%d FlushNoWAL", reason);
	case BackgroundErrorReason::kManifestWriteNoWAL:
		return format("%d ManifestWriteNoWAL", reason);
	default:
		return format("%d Unknown", reason);
	}
}
// Background error handling is tested with Chaos test.
// TODO: Test background error in simulation. RocksDB doesn't use flow IO in simulation, which limits our ability to
// inject IO errors. We could implement rocksdb::FileSystem using flow IO to unblock simulation. Also, trace event is
// not available on background threads because trace event requires setting up special thread locals. Using trace event
// could potentially cause segmentation fault.
class RocksDBErrorListener : public rocksdb::EventListener {
public:
	RocksDBErrorListener(UID id) : id(id){};
	void OnBackgroundError(rocksdb::BackgroundErrorReason reason, rocksdb::Status* bg_error) override {
		TraceEvent(SevError, "RocksDBBGError", id)
		    .detail("Reason", getErrorReason(reason))
		    .detail("RocksDBSeverity", bg_error->severity())
		    .detail("Status", bg_error->ToString());
		std::unique_lock<std::mutex> lock(mutex);
		if (!errorPromise.isValid())
			return;
		// RocksDB generates two types of background errors, IO Error and Corruption
		// Error type and severity map could be found at
		// https://github.com/facebook/rocksdb/blob/2e09a54c4fb82e88bcaa3e7cfa8ccbbbbf3635d5/db/error_handler.cc#L138.
		// All background errors will be treated as storage engine failure. Send the error to storage server.
		if (bg_error->IsIOError()) {
			errorPromise.sendError(io_error());
		} else if (bg_error->IsCorruption()) {
			errorPromise.sendError(file_corrupt());
		} else {
			errorPromise.sendError(unknown_error());
		}
	}
	Future<Void> getFuture() {
		std::unique_lock<std::mutex> lock(mutex);
		return errorPromise.getFuture();
	}
	~RocksDBErrorListener() {
		std::unique_lock<std::mutex> lock(mutex);
		if (!errorPromise.isValid())
			return;
		errorPromise.send(Never());
	}

private:
	ThreadReturnPromise<Void> errorPromise;
	std::mutex mutex;
	UID id;
};

rocksdb::ExportImportFilesMetaData getMetaData(const CheckpointMetaData& checkpoint) {
	rocksdb::ExportImportFilesMetaData metaData;
	if (checkpoint.getFormat() != RocksDBColumnFamily) {
		return metaData;
	}

	RocksDBColumnFamilyCheckpoint rocksCF = getRocksCF(checkpoint);
	metaData.db_comparator_name = rocksCF.dbComparatorName;

	for (const LiveFileMetaData& fileMetaData : rocksCF.sstFiles) {
		rocksdb::LiveFileMetaData liveFileMetaData;
		liveFileMetaData.relative_filename = fileMetaData.relative_filename;
		liveFileMetaData.directory = fileMetaData.directory;
		liveFileMetaData.file_number = fileMetaData.file_number;
		liveFileMetaData.file_type = static_cast<rocksdb::FileType>(fileMetaData.file_type);
		liveFileMetaData.size = fileMetaData.size;
		liveFileMetaData.temperature = static_cast<rocksdb::Temperature>(fileMetaData.temperature);
		liveFileMetaData.file_checksum = fileMetaData.file_checksum;
		liveFileMetaData.file_checksum_func_name = fileMetaData.file_checksum_func_name;
		liveFileMetaData.smallest_seqno = fileMetaData.smallest_seqno;
		liveFileMetaData.largest_seqno = fileMetaData.largest_seqno;
		liveFileMetaData.smallestkey = fileMetaData.smallestkey;
		liveFileMetaData.largestkey = fileMetaData.largestkey;
		liveFileMetaData.num_reads_sampled = fileMetaData.num_reads_sampled;
		liveFileMetaData.being_compacted = fileMetaData.being_compacted;
		liveFileMetaData.num_entries = fileMetaData.num_entries;
		liveFileMetaData.num_deletions = fileMetaData.num_deletions;
		liveFileMetaData.oldest_blob_file_number = fileMetaData.oldest_blob_file_number;
		liveFileMetaData.oldest_ancester_time = fileMetaData.oldest_ancester_time;
		liveFileMetaData.file_creation_time = fileMetaData.file_creation_time;
		liveFileMetaData.epoch_number = fileMetaData.epoch_number;
		liveFileMetaData.name = fileMetaData.name;
		liveFileMetaData.db_path = fileMetaData.db_path;
		liveFileMetaData.column_family_name = fileMetaData.column_family_name;
		liveFileMetaData.level = fileMetaData.level;
		metaData.files.push_back(liveFileMetaData);
	}

	return metaData;
}

void populateMetaData(CheckpointMetaData* checkpoint, const rocksdb::ExportImportFilesMetaData& metaData) {
	RocksDBColumnFamilyCheckpoint rocksCF;
	rocksCF.dbComparatorName = metaData.db_comparator_name;
	for (const rocksdb::LiveFileMetaData& fileMetaData : metaData.files) {
		LiveFileMetaData liveFileMetaData;
		liveFileMetaData.relative_filename = fileMetaData.relative_filename;
		liveFileMetaData.directory = fileMetaData.directory;
		liveFileMetaData.file_number = fileMetaData.file_number;
		liveFileMetaData.file_type = static_cast<int>(fileMetaData.file_type);
		liveFileMetaData.size = fileMetaData.size;
		liveFileMetaData.temperature = static_cast<uint8_t>(fileMetaData.temperature);
		liveFileMetaData.file_checksum = fileMetaData.file_checksum;
		liveFileMetaData.file_checksum_func_name = fileMetaData.file_checksum_func_name;
		liveFileMetaData.smallest_seqno = fileMetaData.smallest_seqno;
		liveFileMetaData.largest_seqno = fileMetaData.largest_seqno;
		liveFileMetaData.smallestkey = fileMetaData.smallestkey;
		liveFileMetaData.largestkey = fileMetaData.largestkey;
		liveFileMetaData.num_reads_sampled = fileMetaData.num_reads_sampled;
		liveFileMetaData.being_compacted = fileMetaData.being_compacted;
		liveFileMetaData.num_entries = fileMetaData.num_entries;
		liveFileMetaData.num_deletions = fileMetaData.num_deletions;
		liveFileMetaData.oldest_blob_file_number = fileMetaData.oldest_blob_file_number;
		liveFileMetaData.oldest_ancester_time = fileMetaData.oldest_ancester_time;
		liveFileMetaData.file_creation_time = fileMetaData.file_creation_time;
		liveFileMetaData.epoch_number = fileMetaData.epoch_number;
		liveFileMetaData.name = fileMetaData.name;
		liveFileMetaData.db_path = fileMetaData.db_path;
		liveFileMetaData.column_family_name = fileMetaData.column_family_name;
		liveFileMetaData.level = fileMetaData.level;
		rocksCF.sstFiles.push_back(liveFileMetaData);
	}
	checkpoint->setFormat(RocksDBColumnFamily);
	checkpoint->serializedCheckpoint = ObjectWriter::toValue(rocksCF, IncludeVersion());
}

rocksdb::Slice toSlice(StringRef s) {
	return rocksdb::Slice(reinterpret_cast<const char*>(s.begin()), s.size());
}

StringRef toStringRef(rocksdb::Slice s) {
	return StringRef(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

rocksdb::ColumnFamilyOptions getCFOptions() {
	rocksdb::ColumnFamilyOptions options;
	options.level_compaction_dynamic_level_bytes = SERVER_KNOBS->ROCKSDB_LEVEL_COMPACTION_DYNAMIC_LEVEL_BYTES;
	if (SERVER_KNOBS->ROCKSDB_LEVEL_STYLE_COMPACTION) {
		options.OptimizeLevelStyleCompaction(SERVER_KNOBS->ROCKSDB_MEMTABLE_BYTES);
	} else {
		options.OptimizeUniversalStyleCompaction(SERVER_KNOBS->ROCKSDB_MEMTABLE_BYTES);
	}

	if (SERVER_KNOBS->ROCKSDB_DISABLE_AUTO_COMPACTIONS) {
		options.disable_auto_compactions = SERVER_KNOBS->ROCKSDB_DISABLE_AUTO_COMPACTIONS;
	}

	if (SERVER_KNOBS->ROCKSDB_PERIODIC_COMPACTION_SECONDS > 0) {
		options.periodic_compaction_seconds = SERVER_KNOBS->ROCKSDB_PERIODIC_COMPACTION_SECONDS;
	}
	if (SERVER_KNOBS->ROCKSDB_SOFT_PENDING_COMPACT_BYTES_LIMIT > 0) {
		options.soft_pending_compaction_bytes_limit = SERVER_KNOBS->ROCKSDB_SOFT_PENDING_COMPACT_BYTES_LIMIT;
	}
	if (SERVER_KNOBS->ROCKSDB_HARD_PENDING_COMPACT_BYTES_LIMIT > 0) {
		options.hard_pending_compaction_bytes_limit = SERVER_KNOBS->ROCKSDB_HARD_PENDING_COMPACT_BYTES_LIMIT;
	}
	options.paranoid_file_checks = SERVER_KNOBS->ROCKSDB_PARANOID_FILE_CHECKS;

	// Compact sstables when there's too much deleted stuff.
	if (SERVER_KNOBS->ROCKSDB_ENABLE_COMPACT_ON_DELETION) {
		// Creates a factory of a table property collector that marks a SST
		// file as need-compaction when it observe at least "D" deletion
		// entries in any "N" consecutive entries, or the ratio of tombstone
		// entries >= deletion_ratio.

		// @param sliding_window_size "N". Note that this number will be
		//     round up to the smallest multiple of 128 that is no less
		//     than the specified size.
		// @param deletion_trigger "D".  Note that even when "N" is changed,
		//     the specified number for "D" will not be changed.
		// @param deletion_ratio, if <= 0 or > 1, disable triggering compaction
		//     based on deletion ratio. Disabled by default.
		options.table_properties_collector_factories = { rocksdb::NewCompactOnDeletionCollectorFactory(
			SERVER_KNOBS->ROCKSDB_CDCF_SLIDING_WINDOW_SIZE,
			SERVER_KNOBS->ROCKSDB_CDCF_DELETION_TRIGGER,
			SERVER_KNOBS->ROCKSDB_CDCF_DELETION_RATIO) };
	}

	rocksdb::BlockBasedTableOptions bbOpts;
	// TODO: Add a knob for the block cache size. (Default is 8 MB)
	if (SERVER_KNOBS->ROCKSDB_PREFIX_LEN > 0) {
		// Prefix blooms are used during Seek.
		options.prefix_extractor.reset(rocksdb::NewFixedPrefixTransform(SERVER_KNOBS->ROCKSDB_PREFIX_LEN));

		// Also turn on bloom filters in the memtable.
		options.memtable_prefix_bloom_size_ratio = SERVER_KNOBS->ROCKSDB_MEMTABLE_PREFIX_BLOOM_SIZE_RATIO;

		// 5 -- Can be read by RocksDB's versions since 6.6.0. Full and partitioned
		// filters use a generally faster and more accurate Bloom filter
		// implementation, with a different schema.
		// https://github.com/facebook/rocksdb/blob/b77569f18bfc77fb1d8a0b3218f6ecf571bc4988/include/rocksdb/table.h#L391
		bbOpts.format_version = 5;

		// Create and apply a bloom filter using the 10 bits
		// which should yield a ~1% false positive rate:
		// https://github.com/facebook/rocksdb/wiki/RocksDB-Bloom-Filter#full-filters-new-format
		bbOpts.filter_policy.reset(rocksdb::NewBloomFilterPolicy(SERVER_KNOBS->ROCKSDB_BLOOM_BITS_PER_KEY));

		// The whole key blooms are only used for point lookups.
		// https://github.com/facebook/rocksdb/wiki/RocksDB-Bloom-Filter#prefix-vs-whole-key
		bbOpts.whole_key_filtering = SERVER_KNOBS->ROCKSDB_BLOOM_WHOLE_KEY_FILTERING;
	}

	if (SERVER_KNOBS->ROCKSDB_BLOCK_CACHE_SIZE > 0) {
		if (rocksdb_block_cache == nullptr) {
			rocksdb_block_cache = rocksdb::NewLRUCache(SERVER_KNOBS->ROCKSDB_BLOCK_CACHE_SIZE);
		}
		bbOpts.block_cache = rocksdb_block_cache;
	}
	if (SERVER_KNOBS->ROCKSDB_BLOCK_SIZE > 0) {
		bbOpts.block_size = SERVER_KNOBS->ROCKSDB_BLOCK_SIZE;
	}

	// The readahead size starts with 8KB and is exponentially increased on each additional sequential IO,
	// up to a max of BlockBasedTableOptions.max_auto_readahead_size (default 256 KB)
	if (SERVER_KNOBS->ROCKSDB_MAX_AUTO_READAHEAD_SIZE > 0) {
		bbOpts.max_auto_readahead_size = SERVER_KNOBS->ROCKSDB_MAX_AUTO_READAHEAD_SIZE;
	}

	options.table_factory.reset(rocksdb::NewBlockBasedTableFactory(bbOpts));

	return options;
}

rocksdb::Options getOptions() {
	rocksdb::Options options({}, getCFOptions());
	options.avoid_unnecessary_blocking_io = true;
	options.create_if_missing = true;
	if (SERVER_KNOBS->ROCKSDB_BACKGROUND_PARALLELISM > 0) {
		options.IncreaseParallelism(SERVER_KNOBS->ROCKSDB_BACKGROUND_PARALLELISM);
	}
	if (SERVER_KNOBS->ROCKSDB_MAX_SUBCOMPACTIONS > 0) {
		options.max_subcompactions = SERVER_KNOBS->ROCKSDB_MAX_SUBCOMPACTIONS;
	}
	if (SERVER_KNOBS->ROCKSDB_COMPACTION_READAHEAD_SIZE > 0) {
		options.compaction_readahead_size = SERVER_KNOBS->ROCKSDB_COMPACTION_READAHEAD_SIZE;
	}
	// The following two fields affect how archived logs will be deleted.
	// 1. If both set to 0, logs will be deleted asap and will not get into
	//    the archive.
	// 2. If WAL_ttl_seconds is 0 and WAL_size_limit_MB is not 0,
	//    WAL files will be checked every 10 min and if total size is greater
	//    then WAL_size_limit_MB, they will be deleted starting with the
	//    earliest until size_limit is met. All empty files will be deleted.
	// 3. If WAL_ttl_seconds is not 0 and WAL_size_limit_MB is 0, then
	//    WAL files will be checked every WAL_ttl_seconds / 2 and those that
	//    are older than WAL_ttl_seconds will be deleted.
	// 4. If both are not 0, WAL files will be checked every 10 min and both
	//    checks will be performed with ttl being first.
	options.WAL_ttl_seconds = SERVER_KNOBS->ROCKSDB_WAL_TTL_SECONDS;
	options.WAL_size_limit_MB = SERVER_KNOBS->ROCKSDB_WAL_SIZE_LIMIT_MB;
	if (SERVER_KNOBS->ROCKSDB_LOG_LEVEL_DEBUG) {
		options.info_log_level = rocksdb::InfoLogLevel::DEBUG_LEVEL;
	}

	options.statistics = rocksdb::CreateDBStatistics();
	options.statistics->set_stats_level(rocksdb::StatsLevel(SERVER_KNOBS->ROCKSDB_STATS_LEVEL));

	options.max_log_file_size = SERVER_KNOBS->ROCKSDB_MAX_LOG_FILE_SIZE;
	options.keep_log_file_num = SERVER_KNOBS->ROCKSDB_KEEP_LOG_FILE_NUM;
	options.db_log_dir = SERVER_KNOBS->LOG_DIRECTORY;
	return options;
}

// Set some useful defaults desired for all reads.
rocksdb::ReadOptions getReadOptions() {
	rocksdb::ReadOptions options;
	options.auto_prefix_mode = (SERVER_KNOBS->ROCKSDB_PREFIX_LEN > 0);
	options.background_purge_on_iterator_cleanup = true;
	return options;
}

struct Counters {
	CounterCollection cc;
	Counter immediateThrottle;
	Counter failedToAcquire;
	Counter deleteKeyReqs;
	Counter deleteRangeReqs;
	Counter convertedDeleteKeyReqs;
	Counter convertedDeleteRangeReqs;
	Counter rocksdbReadRangeQueries;
	Counter commitDelayed;

	Counters()
	  : cc("RocksDBThrottle"), immediateThrottle("ImmediateThrottle", cc), failedToAcquire("FailedToAcquire", cc),
	    deleteKeyReqs("DeleteKeyRequests", cc), deleteRangeReqs("DeleteRangeRequests", cc),
	    convertedDeleteKeyReqs("ConvertedDeleteKeyRequests", cc),
	    convertedDeleteRangeReqs("ConvertedDeleteRangeRequests", cc),
	    rocksdbReadRangeQueries("RocksdbReadRangeQueries", cc), commitDelayed("CommitDelayed", cc) {}
};

struct ReadIterator {
	uint64_t index; // incrementing counter to uniquely identify read iterator.
	bool inUse;
	std::shared_ptr<rocksdb::Iterator> iter;
	double creationTime;
	KeyRange keyRange;
	std::shared_ptr<rocksdb::Slice> beginSlice, endSlice;
	ReadIterator(CF& cf, uint64_t index, DB& db)
	  : index(index), inUse(true), creationTime(now()), iter(db->NewIterator(getReadOptions(), cf)) {}
	ReadIterator(CF& cf, uint64_t index, DB& db, KeyRange keyRange)
	  : index(index), inUse(true), creationTime(now()), keyRange(keyRange) {
		rocksdb::ReadOptions options = getReadOptions();
		beginSlice = std::shared_ptr<rocksdb::Slice>(new rocksdb::Slice(toSlice(keyRange.begin)));
		options.iterate_lower_bound = beginSlice.get();
		endSlice = std::shared_ptr<rocksdb::Slice>(new rocksdb::Slice(toSlice(keyRange.end)));
		options.iterate_upper_bound = endSlice.get();

		iter = std::shared_ptr<rocksdb::Iterator>(db->NewIterator(options, cf));
	}
};

/*
ReadIteratorPool: Collection of iterators. Reuses iterators on non-concurrent multiple read operations,
instead of creating and deleting for every read.

Read: IteratorPool provides an unused iterator if exists or creates and gives a new iterator.
Returns back the iterator after the read is done.

Write: Iterators in the pool are deleted, forcing new iterator creation on next reads. The iterators
which are currently used by the reads can continue using the iterator as it is a shared_ptr. Once
the read is processed, shared_ptr goes out of scope and gets deleted. Eventually the iterator object
gets deleted as the ref count becomes 0.
*/
class ReadIteratorPool {
public:
	ReadIteratorPool(UID id, DB& db, CF& cf) : db(db), cf(cf), index(0), deletedUptoIndex(0), iteratorsReuseCount(0) {
		TraceEvent("ReadIteratorPool", id)
		    .detail("KnobRocksDBReadRangeReuseIterators", SERVER_KNOBS->ROCKSDB_READ_RANGE_REUSE_ITERATORS)
		    .detail("KnobRocksDBReadRangeReuseBoundedIterators",
		            SERVER_KNOBS->ROCKSDB_READ_RANGE_REUSE_BOUNDED_ITERATORS)
		    .detail("KnobRocksDBReadRangeBoundedIteratorsMaxLimit",
		            SERVER_KNOBS->ROCKSDB_READ_RANGE_BOUNDED_ITERATORS_MAX_LIMIT)
		    .detail("KnobRocksDBPrefixLen", SERVER_KNOBS->ROCKSDB_PREFIX_LEN);
		if (SERVER_KNOBS->ROCKSDB_READ_RANGE_REUSE_ITERATORS &&
		    SERVER_KNOBS->ROCKSDB_READ_RANGE_REUSE_BOUNDED_ITERATORS) {
			TraceEvent(SevWarn, "ReadIteratorKnobsMismatch");
		}
	}

	// Called on every db commit.
	void update() {
		if (SERVER_KNOBS->ROCKSDB_READ_RANGE_REUSE_ITERATORS ||
		    SERVER_KNOBS->ROCKSDB_READ_RANGE_REUSE_BOUNDED_ITERATORS) {
			mutex.lock();
			// The latest index might contain the current iterator which is getting created.
			// But, that should be ok to avoid adding more code complexity.
			deletedUptoIndex = index;
			mutex.unlock();
			deleteIteratorsPromise.send(Void());
		}
	}

	// Called on every read operation.
	ReadIterator getIterator(KeyRange keyRange) {
		if (SERVER_KNOBS->ROCKSDB_READ_RANGE_REUSE_ITERATORS) {
			mutex.lock();
			for (it = iteratorsMap.begin(); it != iteratorsMap.end(); it++) {
				if (!it->second.inUse && it->second.index > deletedUptoIndex) {
					it->second.inUse = true;
					iteratorsReuseCount++;
					ReadIterator iter = it->second;
					mutex.unlock();
					return iter;
				}
			}
			index++;
			uint64_t readIteratorIndex = index;
			mutex.unlock();

			ReadIterator iter(cf, readIteratorIndex, db);
			mutex.lock();
			iteratorsMap.insert({ readIteratorIndex, iter });
			mutex.unlock();
			return iter;
		} else if (SERVER_KNOBS->ROCKSDB_READ_RANGE_REUSE_BOUNDED_ITERATORS) {
			// TODO: Based on the datasize in the keyrange, decide whether to store the iterator for reuse.
			mutex.lock();
			for (it = iteratorsMap.begin(); it != iteratorsMap.end(); it++) {
				if (!it->second.inUse && it->second.index > deletedUptoIndex &&
				    it->second.keyRange.contains(keyRange)) {
					it->second.inUse = true;
					iteratorsReuseCount++;
					ReadIterator iter = it->second;
					mutex.unlock();
					return iter;
				}
			}
			index++;
			uint64_t readIteratorIndex = index;
			mutex.unlock();

			ReadIterator iter(cf, readIteratorIndex, db, keyRange);
			if (iteratorsMap.size() < SERVER_KNOBS->ROCKSDB_READ_RANGE_BOUNDED_ITERATORS_MAX_LIMIT) {
				// Not storing more than ROCKSDB_READ_RANGE_BOUNDED_ITERATORS_MAX_LIMIT of iterators
				// to avoid 'out of memory' issues.
				mutex.lock();
				iteratorsMap.insert({ readIteratorIndex, iter });
				mutex.unlock();
			}
			return iter;
		} else {
			index++;
			ReadIterator iter(cf, index, db, keyRange);
			return iter;
		}
	}

	// Called on every read operation, after the keys are collected.
	void returnIterator(ReadIterator& iter) {
		if (SERVER_KNOBS->ROCKSDB_READ_RANGE_REUSE_ITERATORS ||
		    SERVER_KNOBS->ROCKSDB_READ_RANGE_REUSE_BOUNDED_ITERATORS) {
			std::lock_guard<std::mutex> lock(mutex);
			it = iteratorsMap.find(iter.index);
			// iterator found: put the iterator back to the pool(inUse=false).
			// iterator not found: update would have removed the iterator from pool, so nothing to do.
			if (it != iteratorsMap.end()) {
				ASSERT(it->second.inUse);
				it->second.inUse = false;
			}
		}
	}

	// Called for every ROCKSDB_READ_RANGE_ITERATOR_REFRESH_TIME seconds in a loop.
	void refreshIterators() {
		std::lock_guard<std::mutex> lock(mutex);
		it = iteratorsMap.begin();
		auto currTime = now();
		while (it != iteratorsMap.end()) {
			if ((it->second.index <= deletedUptoIndex) ||
			    ((currTime - it->second.creationTime) > SERVER_KNOBS->ROCKSDB_READ_RANGE_ITERATOR_REFRESH_TIME)) {
				it = iteratorsMap.erase(it);
			} else {
				it++;
			}
		}
	}

	uint64_t numReadIteratorsCreated() { return index; }

	uint64_t numTimesReadIteratorsReused() { return iteratorsReuseCount; }

	FutureStream<Void> getDeleteIteratorsFutureStream() { return deleteIteratorsPromise.getFuture(); }

private:
	std::unordered_map<int, ReadIterator> iteratorsMap;
	std::unordered_map<int, ReadIterator>::iterator it;
	DB& db;
	CF& cf;
	std::mutex mutex;
	// incrementing counter for every new iterator creation, to uniquely identify the iterator in returnIterator().
	uint64_t index;
	uint64_t deletedUptoIndex;
	uint64_t iteratorsReuseCount;
	ThreadReturnPromiseStream<Void> deleteIteratorsPromise;
};

class PerfContextMetrics {
public:
	PerfContextMetrics();
	void reset();
	void set(int index);
	void log(bool ignoreZeroMetric);

private:
	std::vector<std::tuple<const char*, int, std::vector<uint64_t>>> metrics;
	uint64_t getRocksdbPerfcontextMetric(int metric);
};

PerfContextMetrics::PerfContextMetrics() {
	metrics = {
		{ "UserKeyComparisonCount", rocksdb_user_key_comparison_count, {} },
		{ "BlockCacheHitCount", rocksdb_block_cache_hit_count, {} },
		{ "BlockReadCount", rocksdb_block_read_count, {} },
		{ "BlockReadByte", rocksdb_block_read_byte, {} },
		{ "BlockReadTime", rocksdb_block_read_time, {} },
		{ "BlockChecksumTime", rocksdb_block_checksum_time, {} },
		{ "BlockDecompressTime", rocksdb_block_decompress_time, {} },
		{ "GetReadBytes", rocksdb_get_read_bytes, {} },
		{ "MultigetReadBytes", rocksdb_multiget_read_bytes, {} },
		{ "IterReadBytes", rocksdb_iter_read_bytes, {} },
		{ "InternalKeySkippedCount", rocksdb_internal_key_skipped_count, {} },
		{ "InternalDeleteSkippedCount", rocksdb_internal_delete_skipped_count, {} },
		{ "InternalRecentSkippedCount", rocksdb_internal_recent_skipped_count, {} },
		{ "InternalMergeCount", rocksdb_internal_merge_count, {} },
		{ "GetSnapshotTime", rocksdb_get_snapshot_time, {} },
		{ "GetFromMemtableTime", rocksdb_get_from_memtable_time, {} },
		{ "GetFromMemtableCount", rocksdb_get_from_memtable_count, {} },
		{ "GetPostProcessTime", rocksdb_get_post_process_time, {} },
		{ "GetFromOutputFilesTime", rocksdb_get_from_output_files_time, {} },
		{ "SeekOnMemtableTime", rocksdb_seek_on_memtable_time, {} },
		{ "SeekOnMemtableCount", rocksdb_seek_on_memtable_count, {} },
		{ "NextOnMemtableCount", rocksdb_next_on_memtable_count, {} },
		{ "PrevOnMemtableCount", rocksdb_prev_on_memtable_count, {} },
		{ "SeekChildSeekTime", rocksdb_seek_child_seek_time, {} },
		{ "SeekChildSeekCount", rocksdb_seek_child_seek_count, {} },
		{ "SeekMinHeapTime", rocksdb_seek_min_heap_time, {} },
		{ "SeekMaxHeapTime", rocksdb_seek_max_heap_time, {} },
		{ "SeekInternalSeekTime", rocksdb_seek_internal_seek_time, {} },
		{ "FindNextUserEntryTime", rocksdb_find_next_user_entry_time, {} },
		{ "WriteWalTime", rocksdb_write_wal_time, {} },
		{ "WriteMemtableTime", rocksdb_write_memtable_time, {} },
		{ "WriteDelayTime", rocksdb_write_delay_time, {} },
		{ "WritePreAndPostProcessTime", rocksdb_write_pre_and_post_process_time, {} },
		{ "DbMutexLockNanos", rocksdb_db_mutex_lock_nanos, {} },
		{ "DbConditionWaitNanos", rocksdb_db_condition_wait_nanos, {} },
		{ "MergeOperatorTimeNanos", rocksdb_merge_operator_time_nanos, {} },
		{ "ReadIndexBlockNanos", rocksdb_read_index_block_nanos, {} },
		{ "ReadFilterBlockNanos", rocksdb_read_filter_block_nanos, {} },
		{ "NewTableBlockIterNanos", rocksdb_new_table_block_iter_nanos, {} },
		{ "NewTableIteratorNanos", rocksdb_new_table_iterator_nanos, {} },
		{ "BlockSeekNanos", rocksdb_block_seek_nanos, {} },
		{ "FindTableNanos", rocksdb_find_table_nanos, {} },
		{ "BloomMemtableHitCount", rocksdb_bloom_memtable_hit_count, {} },
		{ "BloomMemtableMissCount", rocksdb_bloom_memtable_miss_count, {} },
		{ "BloomSstHitCount", rocksdb_bloom_sst_hit_count, {} },
		{ "BloomSstMissCount", rocksdb_bloom_sst_miss_count, {} },
		{ "KeyLockWaitTime", rocksdb_key_lock_wait_time, {} },
		{ "KeyLockWaitCount", rocksdb_key_lock_wait_count, {} },
		{ "EnvNewSequentialFileNanos", rocksdb_env_new_sequential_file_nanos, {} },
		{ "EnvNewRandomAccessFileNanos", rocksdb_env_new_random_access_file_nanos, {} },
		{ "EnvNewWritableFileNanos", rocksdb_env_new_writable_file_nanos, {} },
		{ "EnvReuseWritableFileNanos", rocksdb_env_reuse_writable_file_nanos, {} },
		{ "EnvNewRandomRwFileNanos", rocksdb_env_new_random_rw_file_nanos, {} },
		{ "EnvNewDirectoryNanos", rocksdb_env_new_directory_nanos, {} },
		{ "EnvFileExistsNanos", rocksdb_env_file_exists_nanos, {} },
		{ "EnvGetChildrenNanos", rocksdb_env_get_children_nanos, {} },
		{ "EnvGetChildrenFileAttributesNanos", rocksdb_env_get_children_file_attributes_nanos, {} },
		{ "EnvDeleteFileNanos", rocksdb_env_delete_file_nanos, {} },
		{ "EnvCreateDirNanos", rocksdb_env_create_dir_nanos, {} },
		{ "EnvCreateDirIfMissingNanos", rocksdb_env_create_dir_if_missing_nanos, {} },
		{ "EnvDeleteDirNanos", rocksdb_env_delete_dir_nanos, {} },
		{ "EnvGetFileSizeNanos", rocksdb_env_get_file_size_nanos, {} },
		{ "EnvGetFileModificationTimeNanos", rocksdb_env_get_file_modification_time_nanos, {} },
		{ "EnvRenameFileNanos", rocksdb_env_rename_file_nanos, {} },
		{ "EnvLinkFileNanos", rocksdb_env_link_file_nanos, {} },
		{ "EnvLockFileNanos", rocksdb_env_lock_file_nanos, {} },
		{ "EnvUnlockFileNanos", rocksdb_env_unlock_file_nanos, {} },
		{ "EnvNewLoggerNanos", rocksdb_env_new_logger_nanos, {} },
	};
	for (auto& [name, metric, vals] : metrics) { // readers, then writer
		for (int i = 0; i < SERVER_KNOBS->ROCKSDB_READ_PARALLELISM; i++) {
			vals.push_back(0); // add reader
		}
		vals.push_back(0); // add writer
	}
}

void PerfContextMetrics::reset() {
	rocksdb::get_perf_context()->Reset();
}

void PerfContextMetrics::set(int index) {
	for (auto& [name, metric, vals] : metrics) {
		vals[index] = getRocksdbPerfcontextMetric(metric);
	}
}

void PerfContextMetrics::log(bool ignoreZeroMetric) {
	TraceEvent e("RocksDBPerfContextMetrics");
	e.setMaxEventLength(20000);
	for (auto& [name, metric, vals] : metrics) {
		uint64_t s = 0;
		for (auto& v : vals) {
			s = s + v;
		}
		if (ignoreZeroMetric && s == 0)
			continue;
		e.detail("Sum" + (std::string)name, s);
		for (int i = 0; i < SERVER_KNOBS->ROCKSDB_READ_PARALLELISM; i++) {
			if (vals[i] != 0)
				e.detail("RD" + std::to_string(i) + name, vals[i]);
		}
		if (vals[SERVER_KNOBS->ROCKSDB_READ_PARALLELISM] != 0)
			e.detail("WR" + (std::string)name, vals[SERVER_KNOBS->ROCKSDB_READ_PARALLELISM]);
	}
}

uint64_t PerfContextMetrics::getRocksdbPerfcontextMetric(int metric) {
	switch (metric) {
	case rocksdb_user_key_comparison_count:
		return rocksdb::get_perf_context()->user_key_comparison_count;
	case rocksdb_block_cache_hit_count:
		return rocksdb::get_perf_context()->block_cache_hit_count;
	case rocksdb_block_read_count:
		return rocksdb::get_perf_context()->block_read_count;
	case rocksdb_block_read_byte:
		return rocksdb::get_perf_context()->block_read_byte;
	case rocksdb_block_read_time:
		return rocksdb::get_perf_context()->block_read_time;
	case rocksdb_block_checksum_time:
		return rocksdb::get_perf_context()->block_checksum_time;
	case rocksdb_block_decompress_time:
		return rocksdb::get_perf_context()->block_decompress_time;
	case rocksdb_get_read_bytes:
		return rocksdb::get_perf_context()->get_read_bytes;
	case rocksdb_multiget_read_bytes:
		return rocksdb::get_perf_context()->multiget_read_bytes;
	case rocksdb_iter_read_bytes:
		return rocksdb::get_perf_context()->iter_read_bytes;
	case rocksdb_internal_key_skipped_count:
		return rocksdb::get_perf_context()->internal_key_skipped_count;
	case rocksdb_internal_delete_skipped_count:
		return rocksdb::get_perf_context()->internal_delete_skipped_count;
	case rocksdb_internal_recent_skipped_count:
		return rocksdb::get_perf_context()->internal_recent_skipped_count;
	case rocksdb_internal_merge_count:
		return rocksdb::get_perf_context()->internal_merge_count;
	case rocksdb_get_snapshot_time:
		return rocksdb::get_perf_context()->get_snapshot_time;
	case rocksdb_get_from_memtable_time:
		return rocksdb::get_perf_context()->get_from_memtable_time;
	case rocksdb_get_from_memtable_count:
		return rocksdb::get_perf_context()->get_from_memtable_count;
	case rocksdb_get_post_process_time:
		return rocksdb::get_perf_context()->get_post_process_time;
	case rocksdb_get_from_output_files_time:
		return rocksdb::get_perf_context()->get_from_output_files_time;
	case rocksdb_seek_on_memtable_time:
		return rocksdb::get_perf_context()->seek_on_memtable_time;
	case rocksdb_seek_on_memtable_count:
		return rocksdb::get_perf_context()->seek_on_memtable_count;
	case rocksdb_next_on_memtable_count:
		return rocksdb::get_perf_context()->next_on_memtable_count;
	case rocksdb_prev_on_memtable_count:
		return rocksdb::get_perf_context()->prev_on_memtable_count;
	case rocksdb_seek_child_seek_time:
		return rocksdb::get_perf_context()->seek_child_seek_time;
	case rocksdb_seek_child_seek_count:
		return rocksdb::get_perf_context()->seek_child_seek_count;
	case rocksdb_seek_min_heap_time:
		return rocksdb::get_perf_context()->seek_min_heap_time;
	case rocksdb_seek_max_heap_time:
		return rocksdb::get_perf_context()->seek_max_heap_time;
	case rocksdb_seek_internal_seek_time:
		return rocksdb::get_perf_context()->seek_internal_seek_time;
	case rocksdb_find_next_user_entry_time:
		return rocksdb::get_perf_context()->find_next_user_entry_time;
	case rocksdb_write_wal_time:
		return rocksdb::get_perf_context()->write_wal_time;
	case rocksdb_write_memtable_time:
		return rocksdb::get_perf_context()->write_memtable_time;
	case rocksdb_write_delay_time:
		return rocksdb::get_perf_context()->write_delay_time;
	case rocksdb_write_pre_and_post_process_time:
		return rocksdb::get_perf_context()->write_pre_and_post_process_time;
	case rocksdb_db_mutex_lock_nanos:
		return rocksdb::get_perf_context()->db_mutex_lock_nanos;
	case rocksdb_db_condition_wait_nanos:
		return rocksdb::get_perf_context()->db_condition_wait_nanos;
	case rocksdb_merge_operator_time_nanos:
		return rocksdb::get_perf_context()->merge_operator_time_nanos;
	case rocksdb_read_index_block_nanos:
		return rocksdb::get_perf_context()->read_index_block_nanos;
	case rocksdb_read_filter_block_nanos:
		return rocksdb::get_perf_context()->read_filter_block_nanos;
	case rocksdb_new_table_block_iter_nanos:
		return rocksdb::get_perf_context()->new_table_block_iter_nanos;
	case rocksdb_new_table_iterator_nanos:
		return rocksdb::get_perf_context()->new_table_iterator_nanos;
	case rocksdb_block_seek_nanos:
		return rocksdb::get_perf_context()->block_seek_nanos;
	case rocksdb_find_table_nanos:
		return rocksdb::get_perf_context()->find_table_nanos;
	case rocksdb_bloom_memtable_hit_count:
		return rocksdb::get_perf_context()->bloom_memtable_hit_count;
	case rocksdb_bloom_memtable_miss_count:
		return rocksdb::get_perf_context()->bloom_memtable_miss_count;
	case rocksdb_bloom_sst_hit_count:
		return rocksdb::get_perf_context()->bloom_sst_hit_count;
	case rocksdb_bloom_sst_miss_count:
		return rocksdb::get_perf_context()->bloom_sst_miss_count;
	case rocksdb_key_lock_wait_time:
		return rocksdb::get_perf_context()->key_lock_wait_time;
	case rocksdb_key_lock_wait_count:
		return rocksdb::get_perf_context()->key_lock_wait_count;
	case rocksdb_env_new_sequential_file_nanos:
		return rocksdb::get_perf_context()->env_new_sequential_file_nanos;
	case rocksdb_env_new_random_access_file_nanos:
		return rocksdb::get_perf_context()->env_new_random_access_file_nanos;
	case rocksdb_env_new_writable_file_nanos:
		return rocksdb::get_perf_context()->env_new_writable_file_nanos;
	case rocksdb_env_reuse_writable_file_nanos:
		return rocksdb::get_perf_context()->env_reuse_writable_file_nanos;
	case rocksdb_env_new_random_rw_file_nanos:
		return rocksdb::get_perf_context()->env_new_random_rw_file_nanos;
	case rocksdb_env_new_directory_nanos:
		return rocksdb::get_perf_context()->env_new_directory_nanos;
	case rocksdb_env_file_exists_nanos:
		return rocksdb::get_perf_context()->env_file_exists_nanos;
	case rocksdb_env_get_children_nanos:
		return rocksdb::get_perf_context()->env_get_children_nanos;
	case rocksdb_env_get_children_file_attributes_nanos:
		return rocksdb::get_perf_context()->env_get_children_file_attributes_nanos;
	case rocksdb_env_delete_file_nanos:
		return rocksdb::get_perf_context()->env_delete_file_nanos;
	case rocksdb_env_create_dir_nanos:
		return rocksdb::get_perf_context()->env_create_dir_nanos;
	case rocksdb_env_create_dir_if_missing_nanos:
		return rocksdb::get_perf_context()->env_create_dir_if_missing_nanos;
	case rocksdb_env_delete_dir_nanos:
		return rocksdb::get_perf_context()->env_delete_dir_nanos;
	case rocksdb_env_get_file_size_nanos:
		return rocksdb::get_perf_context()->env_get_file_size_nanos;
	case rocksdb_env_get_file_modification_time_nanos:
		return rocksdb::get_perf_context()->env_get_file_modification_time_nanos;
	case rocksdb_env_rename_file_nanos:
		return rocksdb::get_perf_context()->env_rename_file_nanos;
	case rocksdb_env_link_file_nanos:
		return rocksdb::get_perf_context()->env_link_file_nanos;
	case rocksdb_env_lock_file_nanos:
		return rocksdb::get_perf_context()->env_lock_file_nanos;
	case rocksdb_env_unlock_file_nanos:
		return rocksdb::get_perf_context()->env_unlock_file_nanos;
	case rocksdb_env_new_logger_nanos:
		return rocksdb::get_perf_context()->env_new_logger_nanos;
	default:
		break;
	}
	return 0;
}

ACTOR Future<Void> refreshReadIteratorPool(std::shared_ptr<ReadIteratorPool> readIterPool) {
	if (SERVER_KNOBS->ROCKSDB_READ_RANGE_REUSE_ITERATORS || SERVER_KNOBS->ROCKSDB_READ_RANGE_REUSE_BOUNDED_ITERATORS) {
		state FutureStream<Void> deleteIteratorsFutureStream = readIterPool->getDeleteIteratorsFutureStream();
		loop {
			choose {
				when(wait(delay(SERVER_KNOBS->ROCKSDB_READ_RANGE_ITERATOR_REFRESH_TIME))) {
					readIterPool->refreshIterators();
				}
				when(waitNext(deleteIteratorsFutureStream)) {
					// Add a delay(0.0) to ensure the rest of the caller code runs before refreshing iterators,
					// i.e., making the refreshIterators() call here asynchronous.
					wait(delay(0.0));
					readIterPool->refreshIterators();
				}
			}
		}
	}
	return Void();
}

ACTOR Future<Void> flowLockLogger(UID id, const FlowLock* readLock, const FlowLock* fetchLock) {
	loop {
		wait(delay(SERVER_KNOBS->ROCKSDB_METRICS_DELAY));
		TraceEvent e("RocksDBFlowLock", id);
		e.detail("ReadAvailable", readLock->available());
		e.detail("ReadActivePermits", readLock->activePermits());
		e.detail("ReadWaiters", readLock->waiters());
		e.detail("FetchAvailable", fetchLock->available());
		e.detail("FetchActivePermits", fetchLock->activePermits());
		e.detail("FetchWaiters", fetchLock->waiters());
	}
}

ACTOR Future<Void> rocksDBMetricLogger(UID id,
                                       std::shared_ptr<rocksdb::Statistics> statistics,
                                       std::shared_ptr<PerfContextMetrics> perfContextMetrics,
                                       rocksdb::DB* db,
                                       std::shared_ptr<ReadIteratorPool> readIterPool,
                                       Counters* counters,
                                       CF cf) {
	state std::vector<std::tuple<const char*, uint32_t, uint64_t>> tickerStats = {
		{ "StallMicros", rocksdb::STALL_MICROS, 0 },
		{ "BytesRead", rocksdb::BYTES_READ, 0 },
		{ "IterBytesRead", rocksdb::ITER_BYTES_READ, 0 },
		{ "BytesWritten", rocksdb::BYTES_WRITTEN, 0 },
		{ "BlockCacheMisses", rocksdb::BLOCK_CACHE_MISS, 0 },
		{ "BlockCacheHits", rocksdb::BLOCK_CACHE_HIT, 0 },
		{ "BloomFilterUseful", rocksdb::BLOOM_FILTER_USEFUL, 0 },
		{ "BloomFilterFullPositive", rocksdb::BLOOM_FILTER_FULL_POSITIVE, 0 },
		{ "BloomFilterTruePositive", rocksdb::BLOOM_FILTER_FULL_TRUE_POSITIVE, 0 },
		// Deprecated in RocksDB 8.0
		// { "BloomFilterMicros", rocksdb::BLOOM_FILTER_MICROS, 0 },
		{ "MemtableHit", rocksdb::MEMTABLE_HIT, 0 },
		{ "MemtableMiss", rocksdb::MEMTABLE_MISS, 0 },
		{ "GetHitL0", rocksdb::GET_HIT_L0, 0 },
		{ "GetHitL1", rocksdb::GET_HIT_L1, 0 },
		{ "GetHitL2AndUp", rocksdb::GET_HIT_L2_AND_UP, 0 },
		{ "CountKeysWritten", rocksdb::NUMBER_KEYS_WRITTEN, 0 },
		{ "CountKeysRead", rocksdb::NUMBER_KEYS_READ, 0 },
		{ "CountDBSeek", rocksdb::NUMBER_DB_SEEK, 0 },
		{ "CountDBNext", rocksdb::NUMBER_DB_NEXT, 0 },
		{ "CountDBPrev", rocksdb::NUMBER_DB_PREV, 0 },
		{ "BloomFilterPrefixChecked", rocksdb::BLOOM_FILTER_PREFIX_CHECKED, 0 },
		{ "BloomFilterPrefixUseful", rocksdb::BLOOM_FILTER_PREFIX_USEFUL, 0 },
		// Deprecated in RocksDB 8.0
		// { "BlockCacheCompressedMiss", rocksdb::BLOCK_CACHE_COMPRESSED_MISS, 0 },
		// { "BlockCacheCompressedHit", rocksdb::BLOCK_CACHE_COMPRESSED_HIT, 0 },
		{ "CountWalFileSyncs", rocksdb::WAL_FILE_SYNCED, 0 },
		{ "CountWalFileBytes", rocksdb::WAL_FILE_BYTES, 0 },
		{ "CompactReadBytes", rocksdb::COMPACT_READ_BYTES, 0 },
		{ "CompactReadBytesMarked", rocksdb::COMPACT_READ_BYTES_MARKED, 0 },
		{ "CompactReadBytesPeriodic", rocksdb::COMPACT_READ_BYTES_PERIODIC, 0 },
		{ "CompactReadBytesTtl", rocksdb::COMPACT_READ_BYTES_TTL, 0 },
		{ "CompactWriteBytes", rocksdb::COMPACT_WRITE_BYTES, 0 },
		{ "CompactWriteBytesMarked", rocksdb::COMPACT_WRITE_BYTES_MARKED, 0 },
		{ "CompactWriteBytesPeriodic", rocksdb::COMPACT_WRITE_BYTES_PERIODIC, 0 },
		{ "CompactWriteBytesTtl", rocksdb::COMPACT_WRITE_BYTES_TTL, 0 },
		{ "FlushWriteBytes", rocksdb::FLUSH_WRITE_BYTES, 0 },
		{ "CountBlocksCompressed", rocksdb::NUMBER_BLOCK_COMPRESSED, 0 },
		{ "CountBlocksDecompressed", rocksdb::NUMBER_BLOCK_DECOMPRESSED, 0 },
		{ "RowCacheHit", rocksdb::ROW_CACHE_HIT, 0 },
		{ "RowCacheMiss", rocksdb::ROW_CACHE_MISS, 0 },
		{ "CountIterSkippedKeys", rocksdb::NUMBER_ITER_SKIP, 0 },
		{ "NoIteratorCreated", rocksdb::NO_ITERATOR_CREATED, 0 },
		{ "NoIteratorDeleted", rocksdb::NO_ITERATOR_DELETED, 0 },
	};

	// To control the rocksdb::StatsLevel, use ROCKSDB_STATS_LEVEL knob.
	// Refer StatsLevel: https://github.com/facebook/rocksdb/blob/main/include/rocksdb/statistics.h#L594
	state std::vector<std::pair<const char*, uint32_t>> histogramStats = {
		{ "CompactionTime", rocksdb::COMPACTION_TIME }, // enabled if rocksdb::StatsLevel > kExceptTimers(2)
		{ "CompactionCPUTime", rocksdb::COMPACTION_CPU_TIME }, // enabled if rocksdb::StatsLevel > kExceptTimers(2)
		{ "CompressionTimeNanos",
		  rocksdb::COMPRESSION_TIMES_NANOS }, // enabled if rocksdb::StatsLevel > kExceptDetailedTimers(3)
		{ "DecompressionTimeNanos",
		  rocksdb::DECOMPRESSION_TIMES_NANOS }, // enabled if rocksdb::StatsLevel > kExceptDetailedTimers(3)
		// Deprecated in RocksDB 8.0
		// { "HardRateLimitDelayCount",
		//   rocksdb::HARD_RATE_LIMIT_DELAY_COUNT }, // enabled if rocksdb::StatsLevel > kExceptHistogramOrTimers(1)
		// { "SoftRateLimitDelayCount",
		//   rocksdb::SOFT_RATE_LIMIT_DELAY_COUNT }, // enabled if rocksdb::StatsLevel > kExceptHistogramOrTimers(1)
		{ "WriteStall", rocksdb::WRITE_STALL }, // enabled if rocksdb::StatsLevel > kExceptHistogramOrTimers(1)
	};
	state std::vector<std::pair<const char*, std::string>> intPropertyStats = {
		{ "NumImmutableMemtables", rocksdb::DB::Properties::kNumImmutableMemTable },
		{ "NumImmutableMemtablesFlushed", rocksdb::DB::Properties::kNumImmutableMemTableFlushed },
		{ "IsMemtableFlushPending", rocksdb::DB::Properties::kMemTableFlushPending },
		{ "NumRunningFlushes", rocksdb::DB::Properties::kNumRunningFlushes },
		{ "IsCompactionPending", rocksdb::DB::Properties::kCompactionPending },
		{ "NumRunningCompactions", rocksdb::DB::Properties::kNumRunningCompactions },
		{ "CumulativeBackgroundErrors", rocksdb::DB::Properties::kBackgroundErrors },
		{ "CurrentSizeActiveMemtable", rocksdb::DB::Properties::kCurSizeActiveMemTable },
		{ "AllMemtablesBytes", rocksdb::DB::Properties::kCurSizeAllMemTables },
		{ "ActiveMemtableBytes", rocksdb::DB::Properties::kSizeAllMemTables },
		{ "CountEntriesActiveMemtable", rocksdb::DB::Properties::kNumEntriesActiveMemTable },
		{ "CountEntriesImmutMemtables", rocksdb::DB::Properties::kNumEntriesImmMemTables },
		{ "CountDeletesActiveMemtable", rocksdb::DB::Properties::kNumDeletesActiveMemTable },
		{ "CountDeletesImmutMemtables", rocksdb::DB::Properties::kNumDeletesImmMemTables },
		{ "EstimatedCountKeys", rocksdb::DB::Properties::kEstimateNumKeys },
		{ "EstimateSstReaderBytes", rocksdb::DB::Properties::kEstimateTableReadersMem },
		{ "CountActiveSnapshots", rocksdb::DB::Properties::kNumSnapshots },
		{ "OldestSnapshotTime", rocksdb::DB::Properties::kOldestSnapshotTime },
		{ "CountLiveVersions", rocksdb::DB::Properties::kNumLiveVersions },
		{ "EstimateLiveDataSize", rocksdb::DB::Properties::kEstimateLiveDataSize },
		{ "BaseLevel", rocksdb::DB::Properties::kBaseLevel },
		{ "EstPendCompactBytes", rocksdb::DB::Properties::kEstimatePendingCompactionBytes },
		{ "BlockCacheUsage", rocksdb::DB::Properties::kBlockCacheUsage },
		{ "BlockCachePinnedUsage", rocksdb::DB::Properties::kBlockCachePinnedUsage },
		{ "LiveSstFilesSize", rocksdb::DB::Properties::kLiveSstFilesSize },
	};

	state std::vector<std::pair<const char*, std::string>> strPropertyStats = {
		{ "LevelStats", rocksdb::DB::Properties::kLevelStats },
	};

	state std::vector<std::pair<const char*, std::string>> levelStrPropertyStats = {
		{ "CompressionRatioAtLevel", rocksdb::DB::Properties::kCompressionRatioAtLevelPrefix },
	};

	state std::unordered_map<std::string, uint64_t> readIteratorPoolStats = {
		{ "NumReadIteratorsCreated", 0 },
		{ "NumTimesReadIteratorsReused", 0 },
	};

	loop {
		wait(delay(SERVER_KNOBS->ROCKSDB_METRICS_DELAY));
		TraceEvent e("RocksDBMetrics", id);
		uint64_t stat;
		for (auto& [name, ticker, cum] : tickerStats) {
			stat = statistics->getTickerCount(ticker);
			e.detail(name, stat - cum);
			cum = stat;
		}

		// None of the histogramStats are enabled unless the ROCKSDB_STATS_LEVEL > kExceptHistogramOrTimers(1)
		// Refer StatsLevel: https://github.com/facebook/rocksdb/blob/main/include/rocksdb/statistics.h#L594
		if (SERVER_KNOBS->ROCKSDB_STATS_LEVEL > rocksdb::kExceptHistogramOrTimers) {
			for (auto& [name, histogram] : histogramStats) {
				rocksdb::HistogramData histogram_data;
				statistics->histogramData(histogram, &histogram_data);
				e.detail(format("%s%s", name, "P95"), histogram_data.percentile95);
				e.detail(format("%s%s", name, "P99"), histogram_data.percentile99);
			}
		}

		for (const auto& [name, property] : intPropertyStats) {
			stat = 0;
			// GetAggregatedIntProperty gets the aggregated int property from all column families.
			ASSERT(db->GetAggregatedIntProperty(property, &stat));
			e.detail(name, stat);
		}

		std::string propValue;
		for (const auto& [name, property] : strPropertyStats) {
			propValue = "";
			ASSERT(db->GetProperty(cf, property, &propValue));
			e.detail(name, propValue);
		}

		rocksdb::ColumnFamilyMetaData cf_meta_data;
		db->GetColumnFamilyMetaData(cf, &cf_meta_data);
		int numLevels = static_cast<int>(cf_meta_data.levels.size());
		std::string levelProp;
		for (const auto& [name, property] : levelStrPropertyStats) {
			levelProp = "";
			for (int level = 0; level < numLevels; level++) {
				propValue = "";
				ASSERT(db->GetProperty(cf, property + std::to_string(level), &propValue));
				levelProp += std::to_string(level) + ":" + propValue + (level == numLevels - 1 ? "" : ",");
			}
			e.detail(name, levelProp);
		}

		stat = readIterPool->numReadIteratorsCreated();
		e.detail("NumReadIteratorsCreated", stat - readIteratorPoolStats["NumReadIteratorsCreated"]);
		readIteratorPoolStats["NumReadIteratorsCreated"] = stat;

		stat = readIterPool->numTimesReadIteratorsReused();
		e.detail("NumTimesReadIteratorsReused", stat - readIteratorPoolStats["NumTimesReadIteratorsReused"]);
		readIteratorPoolStats["NumTimesReadIteratorsReused"] = stat;

		counters->cc.logToTraceEvent(e);

		if (SERVER_KNOBS->ROCKSDB_PERFCONTEXT_ENABLE) {
			perfContextMetrics->log(true);
		}
	}
}

void logRocksDBError(UID id, const rocksdb::Status& status, const std::string& method) {
	auto level = status.IsTimedOut() ? SevWarn : SevError;
	TraceEvent e(level, "RocksDBError", id);
	e.detail("Error", status.ToString()).detail("Method", method).detail("RocksDBSeverity", status.severity());
	if (status.IsIOError()) {
		e.detail("SubCode", status.subcode());
	}
}

Error statusToError(const rocksdb::Status& s) {
	if (s.IsIOError()) {
		return io_error();
	} else if (s.IsTimedOut()) {
		return transaction_too_old();
	} else {
		return unknown_error();
	}
}

struct RocksDBKeyValueStore : IKeyValueStore {
	struct Writer : IThreadPoolReceiver {
		DB& db;
		CF& cf;

		UID id;
		std::shared_ptr<rocksdb::RateLimiter> rateLimiter;
		std::shared_ptr<ReadIteratorPool> readIterPool;
		std::shared_ptr<PerfContextMetrics> perfContextMetrics;
		int threadIndex;
		ThreadReturnPromiseStream<std::tuple<int, std::string, double>> metricPromiseStream;
		std::shared_ptr<SharedRocksDBState> sharedState;

		explicit Writer(DB& db,
		                CF& cf,
		                UID id,
		                std::shared_ptr<SharedRocksDBState> sharedState,
		                std::shared_ptr<ReadIteratorPool> readIterPool,
		                std::shared_ptr<PerfContextMetrics> perfContextMetrics,
		                int threadIndex)
		  : db(db), cf(cf), id(id), sharedState(sharedState), readIterPool(readIterPool),
		    perfContextMetrics(perfContextMetrics), threadIndex(threadIndex),
		    rateLimiter(SERVER_KNOBS->ROCKSDB_WRITE_RATE_LIMITER_BYTES_PER_SEC > 0
		                    ? rocksdb::NewGenericRateLimiter(
		                          SERVER_KNOBS->ROCKSDB_WRITE_RATE_LIMITER_BYTES_PER_SEC, // rate_bytes_per_sec
		                          100 * 1000, // refill_period_us
		                          10, // fairness
		                          rocksdb::RateLimiter::Mode::kWritesOnly,
		                          SERVER_KNOBS->ROCKSDB_WRITE_RATE_LIMITER_AUTO_TUNE)
		                    : nullptr) {
			if (SERVER_KNOBS->ROCKSDB_PERFCONTEXT_ENABLE) {
				// Enable perf context on the same thread with the db thread
				rocksdb::SetPerfLevel(rocksdb::PerfLevel::kEnableTimeExceptForMutex);
				perfContextMetrics->reset();
			}
		}

		~Writer() override {
			if (db) {
				delete db;
			}
		}

		void init() override {}

		struct OpenAction : TypedAction<Writer, OpenAction> {
			std::string path;
			ThreadReturnPromise<Void> done;
			Optional<Future<Void>>& metrics;
			const FlowLock* readLock;
			const FlowLock* fetchLock;
			std::shared_ptr<RocksDBErrorListener> errorListener;
			Counters& counters;
			OpenAction(std::string path,
			           Optional<Future<Void>>& metrics,
			           const FlowLock* readLock,
			           const FlowLock* fetchLock,
			           std::shared_ptr<RocksDBErrorListener> errorListener,
			           Counters& counters)
			  : path(std::move(path)), metrics(metrics), readLock(readLock), fetchLock(fetchLock),
			    errorListener(errorListener), counters(counters) {}

			double getTimeEstimate() const override { return SERVER_KNOBS->COMMIT_TIME_ESTIMATE; }
		};
		void action(OpenAction& a) {
			ASSERT(cf == nullptr);

			std::vector<std::string> columnFamilies;
			rocksdb::Options options = getOptions();
			rocksdb::Status status = rocksdb::DB::ListColumnFamilies(options, a.path, &columnFamilies);
			if (std::find(columnFamilies.begin(), columnFamilies.end(), "default") == columnFamilies.end()) {
				columnFamilies.push_back("default");
			}

			rocksdb::ColumnFamilyOptions cfOptions = getCFOptions();
			std::vector<rocksdb::ColumnFamilyDescriptor> descriptors;
			for (const std::string& name : columnFamilies) {
				descriptors.push_back(rocksdb::ColumnFamilyDescriptor{ name, cfOptions });
			}

			options.listeners.push_back(a.errorListener);
			if (SERVER_KNOBS->ROCKSDB_WRITE_RATE_LIMITER_BYTES_PER_SEC > 0) {
				options.rate_limiter = rateLimiter;
			}

			std::vector<rocksdb::ColumnFamilyHandle*> handles;
			status = rocksdb::DB::Open(options, a.path, descriptors, &handles, &db);

			if (!status.ok()) {
				logRocksDBError(id, status, "Open");
				a.done.sendError(statusToError(status));
				return;
			}

			for (rocksdb::ColumnFamilyHandle* handle : handles) {
				if (handle->GetName() == SERVER_KNOBS->DEFAULT_FDB_ROCKSDB_COLUMN_FAMILY) {
					cf = handle;
					break;
				}
			}

			if (cf == nullptr) {
				status = db->CreateColumnFamily(cfOptions, SERVER_KNOBS->DEFAULT_FDB_ROCKSDB_COLUMN_FAMILY, &cf);
				if (!status.ok()) {
					logRocksDBError(id, status, "Open");
					a.done.sendError(statusToError(status));
				}
			}

			TraceEvent(SevInfo, "RocksDB", id)
			    .detail("Path", a.path)
			    .detail("Method", "Open")
			    .detail("KnobRocksDBWriteRateLimiterBytesPerSec",
			            SERVER_KNOBS->ROCKSDB_WRITE_RATE_LIMITER_BYTES_PER_SEC)
			    .detail("KnobRocksDBWriteRateLimiterAutoTune", SERVER_KNOBS->ROCKSDB_WRITE_RATE_LIMITER_AUTO_TUNE)
			    .detail("ColumnFamily", cf->GetName());
			if (g_network->isSimulated()) {
				// The current thread and main thread are same when the code runs in simulation.
				// blockUntilReady() is getting the thread into deadlock state, so directly calling
				// the metricsLogger.
				a.metrics = rocksDBMetricLogger(
				                id, options.statistics, perfContextMetrics, db, readIterPool, &a.counters, cf) &&
				            flowLockLogger(id, a.readLock, a.fetchLock) && refreshReadIteratorPool(readIterPool);
			} else {
				onMainThread([&] {
					a.metrics = rocksDBMetricLogger(
					                id, options.statistics, perfContextMetrics, db, readIterPool, &a.counters, cf) &&
					            flowLockLogger(id, a.readLock, a.fetchLock) && refreshReadIteratorPool(readIterPool);
					return Future<bool>(true);
				}).blockUntilReady();
			}
			a.done.send(Void());
		}

		struct DeleteVisitor : public rocksdb::WriteBatch::Handler {
			VectorRef<KeyRangeRef>& deletes;
			Arena& arena;

			DeleteVisitor(VectorRef<KeyRangeRef>& deletes, Arena& arena) : deletes(deletes), arena(arena) {}

			rocksdb::Status DeleteRangeCF(uint32_t /*column_family_id*/,
			                              const rocksdb::Slice& begin,
			                              const rocksdb::Slice& end) override {
				KeyRangeRef kr(toStringRef(begin), toStringRef(end));
				deletes.push_back_deep(arena, kr);
				return rocksdb::Status::OK();
			}

			rocksdb::Status PutCF(uint32_t column_family_id,
			                      const rocksdb::Slice& key,
			                      const rocksdb::Slice& value) override {
				return rocksdb::Status::OK();
			}

			rocksdb::Status DeleteCF(uint32_t column_family_id, const rocksdb::Slice& key) override {
				return rocksdb::Status::OK();
			}

			rocksdb::Status SingleDeleteCF(uint32_t column_family_id, const rocksdb::Slice& key) override {
				return rocksdb::Status::OK();
			}

			rocksdb::Status MergeCF(uint32_t column_family_id,
			                        const rocksdb::Slice& key,
			                        const rocksdb::Slice& value) override {
				return rocksdb::Status::OK();
			}
		};

		struct CommitAction : TypedAction<Writer, CommitAction> {
			std::unique_ptr<rocksdb::WriteBatch> batchToCommit;
			ThreadReturnPromise<Void> done;
			double startTime;
			bool getHistograms;
			double getTimeEstimate() const override { return SERVER_KNOBS->COMMIT_TIME_ESTIMATE; }
			CommitAction() : startTime(timer_monotonic()) {
				if (deterministicRandom()->random01() < SERVER_KNOBS->ROCKSDB_HISTOGRAMS_SAMPLE_RATE) {
					getHistograms = true;
				} else {
					getHistograms = false;
				}
			}
		};
		void action(CommitAction& a) {
			bool doPerfContextMetrics =
			    SERVER_KNOBS->ROCKSDB_PERFCONTEXT_ENABLE &&
			    (deterministicRandom()->random01() < SERVER_KNOBS->ROCKSDB_PERFCONTEXT_SAMPLE_RATE);
			if (doPerfContextMetrics) {
				perfContextMetrics->reset();
			}
			double commitBeginTime = timer_monotonic();
			sharedState->commitQueueLatency.addMeasurement(commitBeginTime - a.startTime);
			if (a.getHistograms) {
				metricPromiseStream.send(std::make_tuple(
				    threadIndex, ROCKSDB_COMMIT_QUEUEWAIT_HISTOGRAM.toString(), commitBeginTime - a.startTime));
			}
			Standalone<VectorRef<KeyRangeRef>> deletes;
			if (SERVER_KNOBS->ROCKSDB_SUGGEST_COMPACT_CLEAR_RANGE) {
				DeleteVisitor dv(deletes, deletes.arena());
				rocksdb::Status s = a.batchToCommit->Iterate(&dv);
				if (!s.ok()) {
					logRocksDBError(id, s, "CommitDeleteVisitor");
					a.done.sendError(statusToError(s));
					return;
				}
				// If there are any range deletes, we should have added them to be deleted.
				ASSERT(!deletes.empty() || !a.batchToCommit->HasDeleteRange());
			}

			rocksdb::WriteOptions options;
			options.sync = !SERVER_KNOBS->ROCKSDB_UNSAFE_AUTO_FSYNC;
			if (SERVER_KNOBS->ROCKSDB_DISABLE_WAL_EXPERIMENTAL) {
				options.disableWAL = true;
				options.sync = false;
			}

			double writeBeginTime = timer_monotonic();
			if (rateLimiter) {
				// Controls the total write rate of compaction and flush in bytes per second.
				// Request for batchToCommit bytes. If this request cannot be satisfied, the call is blocked.
				rateLimiter->Request(a.batchToCommit->GetDataSize() /* bytes */, rocksdb::Env::IO_HIGH);
			}
			rocksdb::Status s = db->Write(options, a.batchToCommit.get());
			readIterPool->update();
			double currTime = timer_monotonic();
			sharedState->dbWriteLatency.addMeasurement(currTime - writeBeginTime);
			if (a.getHistograms) {
				metricPromiseStream.send(
				    std::make_tuple(threadIndex, ROCKSDB_WRITE_HISTOGRAM.toString(), currTime - writeBeginTime));
			}

			if (!s.ok()) {
				logRocksDBError(id, s, "Commit");
				a.done.sendError(statusToError(s));
			} else {
				a.done.send(Void());

				if (SERVER_KNOBS->ROCKSDB_SUGGEST_COMPACT_CLEAR_RANGE) {
					double compactRangeBeginTime = a.getHistograms ? timer_monotonic() : 0;
					for (const auto& keyRange : deletes) {
						auto begin = toSlice(keyRange.begin);
						auto end = toSlice(keyRange.end);
						ASSERT(db->SuggestCompactRange(cf, &begin, &end).ok());
					}
					if (a.getHistograms) {
						metricPromiseStream.send(std::make_tuple(threadIndex,
						                                         ROCKSDB_DELETE_COMPACTRANGE_HISTOGRAM.toString(),
						                                         timer_monotonic() - compactRangeBeginTime));
					}
				}
			}
			currTime = timer_monotonic();
			sharedState->commitLatency.addMeasurement(currTime - a.startTime);
			if (a.getHistograms) {
				metricPromiseStream.send(std::make_tuple(
				    threadIndex, ROCKSDB_COMMIT_ACTION_HISTOGRAM.toString(), currTime - commitBeginTime));
				metricPromiseStream.send(
				    std::make_tuple(threadIndex, ROCKSDB_COMMIT_LATENCY_HISTOGRAM.toString(), currTime - a.startTime));
			}
			if (doPerfContextMetrics) {
				perfContextMetrics->set(threadIndex);
			}
		}

		struct CloseAction : TypedAction<Writer, CloseAction> {
			ThreadReturnPromise<Void> done;
			std::string path;
			bool deleteOnClose;
			CloseAction(std::string path, bool deleteOnClose) : path(path), deleteOnClose(deleteOnClose) {}
			double getTimeEstimate() const override { return SERVER_KNOBS->COMMIT_TIME_ESTIMATE; }
		};
		void action(CloseAction& a) {
			readIterPool.reset();
			if (db == nullptr) {
				a.done.send(Void());
				return;
			}
			auto s = db->Close();
			if (!s.ok()) {
				logRocksDBError(id, s, "Close");
			}
			if (a.deleteOnClose) {
				std::set<std::string> columnFamilies{ "default" };
				columnFamilies.insert(SERVER_KNOBS->DEFAULT_FDB_ROCKSDB_COLUMN_FAMILY);
				std::vector<rocksdb::ColumnFamilyDescriptor> descriptors;
				for (const std::string name : columnFamilies) {
					descriptors.push_back(rocksdb::ColumnFamilyDescriptor{ name, getCFOptions() });
				}
				s = rocksdb::DestroyDB(a.path, getOptions(), descriptors);
				if (!s.ok()) {
					logRocksDBError(id, s, "Destroy");
				} else {
					TraceEvent("RocksDB", id).detail("Path", a.path).detail("Method", "Destroy");
				}
			}
			TraceEvent("RocksDB", id).detail("Path", a.path).detail("Method", "Close");
			a.done.send(Void());
		}

		struct CheckpointAction : TypedAction<Writer, CheckpointAction> {
			CheckpointAction(const CheckpointRequest& request) : request(request) {}

			double getTimeEstimate() const override { return SERVER_KNOBS->COMMIT_TIME_ESTIMATE; }

			const CheckpointRequest request;
			ThreadReturnPromise<CheckpointMetaData> reply;
		};

		void action(CheckpointAction& a) {
			TraceEvent("RocksDBServeCheckpointBegin", id)
			    .detail("MinVersion", a.request.version)
			    .detail("Range", a.request.range.toString())
			    .detail("Format", static_cast<int>(a.request.format))
			    .detail("CheckpointDir", a.request.checkpointDir);

			rocksdb::Checkpoint* checkpoint;
			rocksdb::Status s = rocksdb::Checkpoint::Create(db, &checkpoint);
			if (!s.ok()) {
				logRocksDBError(id, s, "Checkpoint");
				a.reply.sendError(statusToError(s));
				return;
			}

			rocksdb::PinnableSlice value;
			rocksdb::ReadOptions readOptions = getReadOptions();
			s = db->Get(readOptions, cf, toSlice(persistVersion), &value);

			if (!s.ok() && !s.IsNotFound()) {
				logRocksDBError(id, s, "Checkpoint");
				a.reply.sendError(statusToError(s));
				return;
			}

			const Version version = s.IsNotFound()
			                            ? latestVersion
			                            : BinaryReader::fromStringRef<Version>(toStringRef(value), Unversioned());

			TraceEvent("RocksDBServeCheckpointVersion", id)
			    .detail("CheckpointVersion", a.request.version)
			    .detail("PersistVersion", version);

			// TODO: set the range as the actual shard range.
			CheckpointMetaData res(version, a.request.range, a.request.format, a.request.checkpointID);
			const std::string& checkpointDir = a.request.checkpointDir;

			if (a.request.format == RocksDBColumnFamily) {
				rocksdb::ExportImportFilesMetaData* pMetadata;
				platform::eraseDirectoryRecursive(checkpointDir);
				const std::string cwd = platform::getWorkingDirectory() + "/";
				s = checkpoint->ExportColumnFamily(cf, checkpointDir, &pMetadata);

				if (!s.ok()) {
					logRocksDBError(id, s, "Checkpoint");
					a.reply.sendError(statusToError(s));
					return;
				}

				populateMetaData(&res, *pMetadata);
				delete pMetadata;
				TraceEvent("RocksDBServeCheckpointSuccess", id)
				    .detail("CheckpointMetaData", res.toString())
				    .detail("RocksDBCF", getRocksCF(res).toString());
			} else {
				throw not_implemented();
			}

			res.setState(CheckpointMetaData::Complete);
			a.reply.send(res);
		}

		struct RestoreAction : TypedAction<Writer, RestoreAction> {
			RestoreAction(const std::string& path, const std::vector<CheckpointMetaData>& checkpoints)
			  : path(path), checkpoints(checkpoints) {}

			double getTimeEstimate() const override { return SERVER_KNOBS->COMMIT_TIME_ESTIMATE; }

			const std::string path;
			const std::vector<CheckpointMetaData> checkpoints;
			ThreadReturnPromise<Void> done;
		};

		void action(RestoreAction& a) {
			TraceEvent("RocksDBServeRestoreBegin", id).detail("Path", a.path);

			// TODO: Fail gracefully.
			ASSERT(!a.checkpoints.empty());

			if (a.checkpoints[0].format == RocksDBColumnFamily) {
				ASSERT_EQ(a.checkpoints.size(), 1);
				TraceEvent("RocksDBServeRestoreCF", id)
				    .detail("Path", a.path)
				    .detail("Checkpoint", a.checkpoints[0].toString())
				    .detail("RocksDBCF", getRocksCF(a.checkpoints[0]).toString());

				auto options = getOptions();
				rocksdb::Status status = rocksdb::DB::Open(options, a.path, &db);

				if (!status.ok()) {
					logRocksDBError(id, status, "Restore");
					a.done.sendError(statusToError(status));
					return;
				}

				rocksdb::ExportImportFilesMetaData metaData = getMetaData(a.checkpoints[0]);
				rocksdb::ImportColumnFamilyOptions importOptions;
				importOptions.move_files = true;
				status = db->CreateColumnFamilyWithImport(
				    getCFOptions(), SERVER_KNOBS->DEFAULT_FDB_ROCKSDB_COLUMN_FAMILY, importOptions, metaData, &cf);

				if (!status.ok()) {
					logRocksDBError(id, status, "Restore");
					a.done.sendError(statusToError(status));
				} else {
					TraceEvent(SevInfo, "RocksDB", id).detail("Path", a.path).detail("Method", "Restore");
					a.done.send(Void());
				}
			} else {
				throw not_implemented();
			}
		}
	};

	struct Reader : IThreadPoolReceiver {
		UID id;
		DB& db;
		CF& cf;
		std::shared_ptr<SharedRocksDBState> sharedState;
		double readValueTimeout;
		double readValuePrefixTimeout;
		double readRangeTimeout;
		std::shared_ptr<ReadIteratorPool> readIterPool;
		std::shared_ptr<PerfContextMetrics> perfContextMetrics;
		int threadIndex;
		ThreadReturnPromiseStream<std::tuple<int, std::string, double>> metricPromiseStream;

		explicit Reader(UID id,
		                DB& db,
		                CF& cf,
		                std::shared_ptr<SharedRocksDBState> sharedState,
		                std::shared_ptr<ReadIteratorPool> readIterPool,
		                std::shared_ptr<PerfContextMetrics> perfContextMetrics,
		                int threadIndex)
		  : id(id), db(db), cf(cf), sharedState(sharedState), readIterPool(readIterPool),
		    perfContextMetrics(perfContextMetrics), threadIndex(threadIndex) {
			if (g_network->isSimulated()) {
				// In simulation, increasing the read operation timeouts to 5 minutes, as some of the tests have
				// very high load and single read thread cannot process all the load within the timeouts.
				readValueTimeout = 5 * 60;
				readValuePrefixTimeout = 5 * 60;
				readRangeTimeout = 5 * 60;
			} else {
				readValueTimeout = SERVER_KNOBS->ROCKSDB_READ_VALUE_TIMEOUT;
				readValuePrefixTimeout = SERVER_KNOBS->ROCKSDB_READ_VALUE_PREFIX_TIMEOUT;
				readRangeTimeout = SERVER_KNOBS->ROCKSDB_READ_RANGE_TIMEOUT;
			}
			if (SERVER_KNOBS->ROCKSDB_PERFCONTEXT_ENABLE) {
				// Enable perf context on the same thread with the db thread
				rocksdb::SetPerfLevel(rocksdb::PerfLevel::kEnableTimeExceptForMutex);
				perfContextMetrics->reset();
			}
		}

		void init() override {}

		struct ReadValueAction : TypedAction<Reader, ReadValueAction> {
			Key key;
			ReadType type;
			Optional<UID> debugID;
			double startTime;
			bool getHistograms;
			ThreadReturnPromise<Optional<Value>> result;
			ReadValueAction(KeyRef key, ReadType type, Optional<UID> debugID)
			  : key(key), type(type), debugID(debugID), startTime(timer_monotonic()),
			    getHistograms(
			        (deterministicRandom()->random01() < SERVER_KNOBS->ROCKSDB_HISTOGRAMS_SAMPLE_RATE) ? true : false),
			    result(static_cast<TaskPriority>(SERVER_KNOBS->ROCKSDB_THREAD_PROMISE_PRIORITY)) {}
			double getTimeEstimate() const override { return SERVER_KNOBS->READ_VALUE_TIME_ESTIMATE; }
		};
		void action(ReadValueAction& a) {
			ASSERT(cf != nullptr);
			bool doPerfContextMetrics =
			    SERVER_KNOBS->ROCKSDB_PERFCONTEXT_ENABLE &&
			    (deterministicRandom()->random01() < SERVER_KNOBS->ROCKSDB_PERFCONTEXT_SAMPLE_RATE);
			if (doPerfContextMetrics) {
				perfContextMetrics->reset();
			}
			double readBeginTime = timer_monotonic();
			if (a.getHistograms) {
				metricPromiseStream.send(std::make_tuple(
				    threadIndex, ROCKSDB_READVALUE_QUEUEWAIT_HISTOGRAM.toString(), readBeginTime - a.startTime));
			}
			sharedState->readQueueLatency[threadIndex]->addMeasurement(readBeginTime - a.startTime);
			Optional<TraceBatch> traceBatch;
			if (a.debugID.present()) {
				traceBatch = { TraceBatch{} };
				traceBatch.get().addEvent("GetValueDebug", a.debugID.get().first(), "Reader.Before");
			}
			if (shouldThrottle(a.type, a.key) && readBeginTime - a.startTime > readValueTimeout) {
				TraceEvent(SevWarn, "KVSTimeout", id)
				    .detail("Error", "Read value request timedout")
				    .detail("Method", "ReadValueAction")
				    .detail("TimeoutValue", readValueTimeout);
				a.result.sendError(transaction_too_old());
				return;
			}

			rocksdb::PinnableSlice value;
			auto options = getReadOptions();
			if (shouldThrottle(a.type, a.key)) {
				uint64_t deadlineMircos =
				    db->GetEnv()->NowMicros() + (readValueTimeout - (readBeginTime - a.startTime)) * 1000000;
				std::chrono::seconds deadlineSeconds(deadlineMircos / 1000000);
				options.deadline = std::chrono::duration_cast<std::chrono::microseconds>(deadlineSeconds);
			}

			double dbGetBeginTime = a.getHistograms ? timer_monotonic() : 0;
			auto s = db->Get(options, cf, toSlice(a.key), &value);
			if (!s.ok() && !s.IsNotFound()) {
				logRocksDBError(id, s, "ReadValue");
				a.result.sendError(statusToError(s));
				return;
			}

			const double endTime = timer_monotonic();
			if (a.getHistograms) {
				metricPromiseStream.send(
				    std::make_tuple(threadIndex, ROCKSDB_READVALUE_GET_HISTOGRAM.toString(), endTime - dbGetBeginTime));
			}
			sharedState->readLatency[threadIndex]->addMeasurement(endTime - readBeginTime);

			if (a.debugID.present()) {
				traceBatch.get().addEvent("GetValueDebug", a.debugID.get().first(), "Reader.After");
				traceBatch.get().dump();
			}
			if (s.ok()) {
				a.result.send(Value(toStringRef(value)));
			} else if (s.IsNotFound()) {
				a.result.send(Optional<Value>());
			} else {
				logRocksDBError(id, s, "ReadValue");
				a.result.sendError(statusToError(s));
			}

			if (a.getHistograms) {
				double currTime = timer_monotonic();
				metricPromiseStream.send(std::make_tuple(
				    threadIndex, ROCKSDB_READVALUE_ACTION_HISTOGRAM.toString(), currTime - readBeginTime));
				metricPromiseStream.send(std::make_tuple(
				    threadIndex, ROCKSDB_READVALUE_LATENCY_HISTOGRAM.toString(), currTime - a.startTime));
			}
			if (doPerfContextMetrics) {
				perfContextMetrics->set(threadIndex);
			}
		}

		struct ReadValuePrefixAction : TypedAction<Reader, ReadValuePrefixAction> {
			Key key;
			int maxLength;
			ReadType type;
			Optional<UID> debugID;
			double startTime;
			bool getHistograms;
			ThreadReturnPromise<Optional<Value>> result;
			ReadValuePrefixAction(Key key, int maxLength, ReadType type, Optional<UID> debugID)
			  : key(key), maxLength(maxLength), type(type), debugID(debugID), startTime(timer_monotonic()),
			    getHistograms(
			        (deterministicRandom()->random01() < SERVER_KNOBS->ROCKSDB_HISTOGRAMS_SAMPLE_RATE) ? true : false),
			    result(static_cast<TaskPriority>(SERVER_KNOBS->ROCKSDB_THREAD_PROMISE_PRIORITY)) {}
			double getTimeEstimate() const override { return SERVER_KNOBS->READ_VALUE_TIME_ESTIMATE; }
		};
		void action(ReadValuePrefixAction& a) {
			bool doPerfContextMetrics =
			    SERVER_KNOBS->ROCKSDB_PERFCONTEXT_ENABLE &&
			    (deterministicRandom()->random01() < SERVER_KNOBS->ROCKSDB_PERFCONTEXT_SAMPLE_RATE);
			if (doPerfContextMetrics) {
				perfContextMetrics->reset();
			}
			double readBeginTime = timer_monotonic();
			if (a.getHistograms) {
				metricPromiseStream.send(std::make_tuple(
				    threadIndex, ROCKSDB_READPREFIX_QUEUEWAIT_HISTOGRAM.toString(), readBeginTime - a.startTime));
			}
			sharedState->readQueueLatency[threadIndex]->addMeasurement(readBeginTime - a.startTime);
			Optional<TraceBatch> traceBatch;
			if (a.debugID.present()) {
				traceBatch = { TraceBatch{} };
				traceBatch.get().addEvent("GetValuePrefixDebug",
				                          a.debugID.get().first(),
				                          "Reader.Before"); //.detail("TaskID", g_network->getCurrentTask());
			}
			if (shouldThrottle(a.type, a.key) && readBeginTime - a.startTime > readValuePrefixTimeout) {
				TraceEvent(SevWarn, "KVSTimeout", id)
				    .detail("Error", "Read value prefix request timedout")
				    .detail("Method", "ReadValuePrefixAction")
				    .detail("TimeoutValue", readValuePrefixTimeout);
				a.result.sendError(transaction_too_old());
				return;
			}

			rocksdb::PinnableSlice value;
			auto options = getReadOptions();
			if (shouldThrottle(a.type, a.key)) {
				uint64_t deadlineMircos =
				    db->GetEnv()->NowMicros() + (readValuePrefixTimeout - (readBeginTime - a.startTime)) * 1000000;
				std::chrono::seconds deadlineSeconds(deadlineMircos / 1000000);
				options.deadline = std::chrono::duration_cast<std::chrono::microseconds>(deadlineSeconds);
			}

			double dbGetBeginTime = a.getHistograms ? timer_monotonic() : 0;
			auto s = db->Get(options, cf, toSlice(a.key), &value);
			const double endTime = timer_monotonic();
			if (a.getHistograms) {
				metricPromiseStream.send(std::make_tuple(
				    threadIndex, ROCKSDB_READPREFIX_GET_HISTOGRAM.toString(), endTime - dbGetBeginTime));
			}
			sharedState->readLatency[threadIndex]->addMeasurement(endTime - readBeginTime);

			if (a.debugID.present()) {
				traceBatch.get().addEvent("GetValuePrefixDebug",
				                          a.debugID.get().first(),
				                          "Reader.After"); //.detail("TaskID", g_network->getCurrentTask());
				traceBatch.get().dump();
			}
			if (s.ok()) {
				a.result.send(Value(StringRef(reinterpret_cast<const uint8_t*>(value.data()),
				                              std::min(value.size(), size_t(a.maxLength)))));
			} else if (s.IsNotFound()) {
				a.result.send(Optional<Value>());
			} else {
				logRocksDBError(id, s, "ReadValuePrefix");
				a.result.sendError(statusToError(s));
			}
			if (a.getHistograms) {
				double currTime = timer_monotonic();
				metricPromiseStream.send(std::make_tuple(
				    threadIndex, ROCKSDB_READPREFIX_ACTION_HISTOGRAM.toString(), currTime - readBeginTime));
				metricPromiseStream.send(std::make_tuple(
				    threadIndex, ROCKSDB_READPREFIX_LATENCY_HISTOGRAM.toString(), currTime - a.startTime));
			}
			if (doPerfContextMetrics) {
				perfContextMetrics->set(threadIndex);
			}
		}

		struct ReadRangeAction : TypedAction<Reader, ReadRangeAction>, FastAllocated<ReadRangeAction> {
			KeyRange keys;
			int rowLimit, byteLimit;
			ReadType type;
			double startTime;
			bool getHistograms;
			ThreadReturnPromise<RangeResult> result;
			Counters& counters;
			ReadRangeAction(KeyRange keys, int rowLimit, int byteLimit, ReadType type, Counters& counters)
			  : keys(keys), rowLimit(rowLimit), byteLimit(byteLimit), type(type), startTime(timer_monotonic()),
			    counters(counters),
			    getHistograms(deterministicRandom()->random01() < SERVER_KNOBS->ROCKSDB_HISTOGRAMS_SAMPLE_RATE),
			    result(static_cast<TaskPriority>(SERVER_KNOBS->ROCKSDB_THREAD_PROMISE_PRIORITY)) {}
			double getTimeEstimate() const override { return SERVER_KNOBS->READ_RANGE_TIME_ESTIMATE; }
		};
		void action(ReadRangeAction& a) {
			++a.counters.rocksdbReadRangeQueries;
			bool doPerfContextMetrics =
			    SERVER_KNOBS->ROCKSDB_PERFCONTEXT_ENABLE &&
			    (deterministicRandom()->random01() < SERVER_KNOBS->ROCKSDB_PERFCONTEXT_SAMPLE_RATE);
			if (doPerfContextMetrics) {
				perfContextMetrics->reset();
			}
			double readBeginTime = timer_monotonic();
			if (a.getHistograms) {
				metricPromiseStream.send(std::make_tuple(
				    threadIndex, ROCKSDB_READRANGE_QUEUEWAIT_HISTOGRAM.toString(), readBeginTime - a.startTime));
			}
			sharedState->readQueueLatency[threadIndex]->addMeasurement(readBeginTime - a.startTime);
			if (shouldThrottle(a.type, a.keys.begin) && readBeginTime - a.startTime > readRangeTimeout) {
				TraceEvent(SevWarn, "KVSTimeout", id)
				    .detail("Error", "Read range request timedout")
				    .detail("Method", "ReadRangeAction")
				    .detail("TimeoutValue", readRangeTimeout);
				a.result.sendError(transaction_too_old());
				return;
			}

			RangeResult result;
			if (a.rowLimit == 0 || a.byteLimit == 0) {
				a.result.send(result);
			}
			int accumulatedBytes = 0;
			rocksdb::Status s;
			if (a.rowLimit >= 0) {
				double iterCreationBeginTime = a.getHistograms ? timer_monotonic() : 0;
				ReadIterator readIter = readIterPool->getIterator(a.keys);
				if (a.getHistograms) {
					metricPromiseStream.send(std::make_tuple(threadIndex,
					                                         ROCKSDB_READRANGE_NEWITERATOR_HISTOGRAM.toString(),
					                                         timer_monotonic() - iterCreationBeginTime));
				}
				auto cursor = readIter.iter;
				cursor->Seek(toSlice(a.keys.begin));
				while (cursor->Valid() && toStringRef(cursor->key()) < a.keys.end) {
					KeyValueRef kv(toStringRef(cursor->key()), toStringRef(cursor->value()));
					accumulatedBytes += sizeof(KeyValueRef) + kv.expectedSize();
					result.push_back_deep(result.arena(), kv);
					// Calling `cursor->Next()` is potentially expensive, so short-circut here just in case.
					if (result.size() >= a.rowLimit || accumulatedBytes >= a.byteLimit) {
						break;
					}
					if (shouldThrottle(a.type, a.keys.begin) && timer_monotonic() - a.startTime > readRangeTimeout) {
						TraceEvent(SevWarn, "KVSTimeout", id)
						    .detail("Error", "Read range request timedout")
						    .detail("Method", "ReadRangeAction")
						    .detail("TimeoutValue", readRangeTimeout);
						a.result.sendError(transaction_too_old());
						return;
					}
					cursor->Next();
				}
				s = cursor->status();
				readIterPool->returnIterator(readIter);
			} else {
				double iterCreationBeginTime = a.getHistograms ? timer_monotonic() : 0;
				ReadIterator readIter = readIterPool->getIterator(a.keys);
				if (a.getHistograms) {
					metricPromiseStream.send(std::make_tuple(threadIndex,
					                                         ROCKSDB_READRANGE_NEWITERATOR_HISTOGRAM.toString(),
					                                         timer_monotonic() - iterCreationBeginTime));
				}
				auto cursor = readIter.iter;
				cursor->SeekForPrev(toSlice(a.keys.end));
				if (cursor->Valid() && toStringRef(cursor->key()) == a.keys.end) {
					cursor->Prev();
				}
				while (cursor->Valid() && toStringRef(cursor->key()) >= a.keys.begin) {
					KeyValueRef kv(toStringRef(cursor->key()), toStringRef(cursor->value()));
					accumulatedBytes += sizeof(KeyValueRef) + kv.expectedSize();
					result.push_back_deep(result.arena(), kv);
					// Calling `cursor->Prev()` is potentially expensive, so short-circut here just in case.
					if (result.size() >= -a.rowLimit || accumulatedBytes >= a.byteLimit) {
						break;
					}
					if (shouldThrottle(a.type, a.keys.begin) && timer_monotonic() - a.startTime > readRangeTimeout) {
						TraceEvent(SevWarn, "KVSTimeout", id)
						    .detail("Error", "Read range request timedout")
						    .detail("Method", "ReadRangeAction")
						    .detail("TimeoutValue", readRangeTimeout);
						a.result.sendError(transaction_too_old());
						return;
					}
					cursor->Prev();
				}
				s = cursor->status();
				readIterPool->returnIterator(readIter);
			}

			if (!s.ok()) {
				logRocksDBError(id, s, "ReadRange");
				a.result.sendError(statusToError(s));
				return;
			}
			result.more =
			    (result.size() == a.rowLimit) || (result.size() == -a.rowLimit) || (accumulatedBytes >= a.byteLimit);
			if (result.more) {
				result.readThrough = result[result.size() - 1].key;
			}
			a.result.send(result);
			// Temporarily not sampling to understand the pattern of readRange results.
			if (SERVER_KNOBS->ROCKSDB_HISTOGRAMS_SAMPLE_RATE > 0) {
				metricPromiseStream.send(std::make_tuple(
				    threadIndex, ROCKSDB_READ_RANGE_BYTES_RETURNED_HISTOGRAM.toString(), result.logicalSize()));
				metricPromiseStream.send(std::make_tuple(
				    threadIndex, ROCKSDB_READ_RANGE_KV_PAIRS_RETURNED_HISTOGRAM.toString(), result.size()));
			}
			const double endTime = timer_monotonic();
			if (a.getHistograms) {
				metricPromiseStream.send(std::make_tuple(
				    threadIndex, ROCKSDB_READRANGE_ACTION_HISTOGRAM.toString(), endTime - readBeginTime));
				metricPromiseStream.send(std::make_tuple(
				    threadIndex, ROCKSDB_READRANGE_LATENCY_HISTOGRAM.toString(), endTime - a.startTime));
			}
			sharedState->scanLatency[threadIndex]->addMeasurement(endTime - readBeginTime);
			if (doPerfContextMetrics) {
				perfContextMetrics->set(threadIndex);
			}
		}
	};

	DB db = nullptr;
	std::string path;
	UID id;
	std::shared_ptr<SharedRocksDBState> sharedState;
	std::shared_ptr<PerfContextMetrics> perfContextMetrics;
	rocksdb::ColumnFamilyHandle* defaultFdbCF = nullptr;
	Reference<IThreadPool> writeThread;
	Reference<IThreadPool> readThreads;
	std::shared_ptr<RocksDBErrorListener> errorListener;
	Future<Void> errorFuture;
	Promise<Void> closePromise;
	Future<Void> openFuture;
	std::unique_ptr<rocksdb::WriteBatch> writeBatch;
	std::set<Key> keysSet;
	std::set<Key> previousCommitKeysSet;
	// maximum number of single key deletes in a commit, if ROCKSDB_SINGLEKEY_DELETES_ON_CLEARRANGE is enabled.
	int maxDeletes;
	Optional<Future<Void>> metrics;
	FlowLock readSemaphore;
	int numReadWaiters;
	FlowLock fetchSemaphore;
	int numFetchWaiters;
	std::shared_ptr<ReadIteratorPool> readIterPool;
	std::vector<Future<Void>> actors;
	Counters counters;

	explicit RocksDBKeyValueStore(const std::string& path, UID id)
	  : path(path), id(id), sharedState(std::make_shared<SharedRocksDBState>(id)),
	    perfContextMetrics(new PerfContextMetrics()), readIterPool(new ReadIteratorPool(id, db, defaultFdbCF)),
	    readSemaphore(SERVER_KNOBS->ROCKSDB_READ_QUEUE_SOFT_MAX),
	    fetchSemaphore(SERVER_KNOBS->ROCKSDB_FETCH_QUEUE_SOFT_MAX),
	    numReadWaiters(SERVER_KNOBS->ROCKSDB_READ_QUEUE_HARD_MAX - SERVER_KNOBS->ROCKSDB_READ_QUEUE_SOFT_MAX),
	    numFetchWaiters(SERVER_KNOBS->ROCKSDB_FETCH_QUEUE_HARD_MAX - SERVER_KNOBS->ROCKSDB_FETCH_QUEUE_SOFT_MAX),
	    errorListener(std::make_shared<RocksDBErrorListener>(id)), errorFuture(errorListener->getFuture()) {
		// In simluation, run the reader/writer threads as Coro threads (i.e. in the network thread. The storage engine
		// is still multi-threaded as background compaction threads are still present. Reads/writes to disk will also
		// block the network thread in a way that would be unacceptable in production but is a necessary evil here. When
		// performing the reads in background threads in simulation, the event loop thinks there is no work to do and
		// advances time faster than 1 sec/sec. By the time the blocking read actually finishes, simulation has advanced
		// time by more than 5 seconds, so every read fails with a transaction_too_old error. Doing blocking IO on the
		// main thread solves this issue. There are almost certainly better fixes, but my goal was to get a less
		// invasive change merged first and work on a more realistic version if/when we think that would provide
		// substantially more confidence in the correctness.
		// TODO: Adapt the simulation framework to not advance time quickly when background reads/writes are occurring.
		if (g_network->isSimulated()) {
			writeThread = CoroThreadPool::createThreadPool();
			readThreads = CoroThreadPool::createThreadPool();
		} else {
			writeThread = createGenericThreadPool(/*stackSize=*/0, SERVER_KNOBS->ROCKSDB_WRITER_THREAD_PRIORITY);
			readThreads = createGenericThreadPool(/*stackSize=*/0, SERVER_KNOBS->ROCKSDB_READER_THREAD_PRIORITY);
		}
		struct Writer* writer = new Writer(db,
		                                   defaultFdbCF,
		                                   id,
		                                   this->sharedState,
		                                   readIterPool,
		                                   perfContextMetrics,
		                                   SERVER_KNOBS->ROCKSDB_READ_PARALLELISM);
		if (SERVER_KNOBS->ROCKSDB_HISTOGRAMS_SAMPLE_RATE > 0) {
			actors.push_back(updateHistogram(writer->metricPromiseStream.getFuture()));
		}
		writeThread->addThread(writer, "fdb-rocksdb-wr");
		TraceEvent("RocksDBReadThreads", id)
		    .detail("KnobRocksDBReadParallelism", SERVER_KNOBS->ROCKSDB_READ_PARALLELISM);
		for (unsigned i = 0; i < SERVER_KNOBS->ROCKSDB_READ_PARALLELISM; ++i) {
			struct Reader* reader =
			    new Reader(id, db, defaultFdbCF, this->sharedState, readIterPool, perfContextMetrics, i);
			if (SERVER_KNOBS->ROCKSDB_HISTOGRAMS_SAMPLE_RATE > 0) {
				actors.push_back(updateHistogram(reader->metricPromiseStream.getFuture()));
			}
			readThreads->addThread(reader, "fdb-rocksdb-re");
		}
	}

	ACTOR Future<Void> updateHistogram(FutureStream<std::tuple<int, std::string, double>> metricFutureStream) {
		state Reference<Histogram> commitLatencyHistogram = Histogram::getHistogram(
		    ROCKSDBSTORAGE_HISTOGRAM_GROUP, ROCKSDB_COMMIT_LATENCY_HISTOGRAM, Histogram::Unit::microseconds);
		state Reference<Histogram> commitActionHistogram = Histogram::getHistogram(
		    ROCKSDBSTORAGE_HISTOGRAM_GROUP, ROCKSDB_COMMIT_ACTION_HISTOGRAM, Histogram::Unit::microseconds);
		state Reference<Histogram> commitQueueWaitHistogram = Histogram::getHistogram(
		    ROCKSDBSTORAGE_HISTOGRAM_GROUP, ROCKSDB_COMMIT_QUEUEWAIT_HISTOGRAM, Histogram::Unit::microseconds);
		state Reference<Histogram> writeHistogram = Histogram::getHistogram(
		    ROCKSDBSTORAGE_HISTOGRAM_GROUP, ROCKSDB_WRITE_HISTOGRAM, Histogram::Unit::microseconds);
		state Reference<Histogram> deleteCompactRangeHistogram = Histogram::getHistogram(
		    ROCKSDBSTORAGE_HISTOGRAM_GROUP, ROCKSDB_DELETE_COMPACTRANGE_HISTOGRAM, Histogram::Unit::microseconds);
		state Reference<Histogram> readRangeLatencyHistogram = Histogram::getHistogram(
		    ROCKSDBSTORAGE_HISTOGRAM_GROUP, ROCKSDB_READRANGE_LATENCY_HISTOGRAM, Histogram::Unit::microseconds);
		state Reference<Histogram> readValueLatencyHistogram = Histogram::getHistogram(
		    ROCKSDBSTORAGE_HISTOGRAM_GROUP, ROCKSDB_READVALUE_LATENCY_HISTOGRAM, Histogram::Unit::microseconds);
		state Reference<Histogram> readPrefixLatencyHistogram = Histogram::getHistogram(
		    ROCKSDBSTORAGE_HISTOGRAM_GROUP, ROCKSDB_READPREFIX_LATENCY_HISTOGRAM, Histogram::Unit::microseconds);
		state Reference<Histogram> readRangeActionHistogram = Histogram::getHistogram(
		    ROCKSDBSTORAGE_HISTOGRAM_GROUP, ROCKSDB_READRANGE_ACTION_HISTOGRAM, Histogram::Unit::microseconds);
		state Reference<Histogram> readValueActionHistogram = Histogram::getHistogram(
		    ROCKSDBSTORAGE_HISTOGRAM_GROUP, ROCKSDB_READVALUE_ACTION_HISTOGRAM, Histogram::Unit::microseconds);
		state Reference<Histogram> readPrefixActionHistogram = Histogram::getHistogram(
		    ROCKSDBSTORAGE_HISTOGRAM_GROUP, ROCKSDB_READPREFIX_ACTION_HISTOGRAM, Histogram::Unit::microseconds);
		state Reference<Histogram> readRangeQueueWaitHistogram = Histogram::getHistogram(
		    ROCKSDBSTORAGE_HISTOGRAM_GROUP, ROCKSDB_READRANGE_QUEUEWAIT_HISTOGRAM, Histogram::Unit::microseconds);
		state Reference<Histogram> readValueQueueWaitHistogram = Histogram::getHistogram(
		    ROCKSDBSTORAGE_HISTOGRAM_GROUP, ROCKSDB_READVALUE_QUEUEWAIT_HISTOGRAM, Histogram::Unit::microseconds);
		state Reference<Histogram> readPrefixQueueWaitHistogram = Histogram::getHistogram(
		    ROCKSDBSTORAGE_HISTOGRAM_GROUP, ROCKSDB_READPREFIX_QUEUEWAIT_HISTOGRAM, Histogram::Unit::microseconds);
		state Reference<Histogram> readRangeNewIteratorHistogram = Histogram::getHistogram(
		    ROCKSDBSTORAGE_HISTOGRAM_GROUP, ROCKSDB_READRANGE_NEWITERATOR_HISTOGRAM, Histogram::Unit::microseconds);
		state Reference<Histogram> readValueGetHistogram = Histogram::getHistogram(
		    ROCKSDBSTORAGE_HISTOGRAM_GROUP, ROCKSDB_READVALUE_GET_HISTOGRAM, Histogram::Unit::microseconds);
		state Reference<Histogram> readPrefixGetHistogram = Histogram::getHistogram(
		    ROCKSDBSTORAGE_HISTOGRAM_GROUP, ROCKSDB_READPREFIX_GET_HISTOGRAM, Histogram::Unit::microseconds);
		state Reference<Histogram> rocksdbReadRangeBytesReturnedHistogram = Histogram::getHistogram(
		    ROCKSDBSTORAGE_HISTOGRAM_GROUP, ROCKSDB_READ_RANGE_BYTES_RETURNED_HISTOGRAM, Histogram::Unit::bytes);
		state Reference<Histogram> rocksdbReadRangeKVPairsReturnedHistogram = Histogram::getHistogram(
		    ROCKSDBSTORAGE_HISTOGRAM_GROUP, ROCKSDB_READ_RANGE_KV_PAIRS_RETURNED_HISTOGRAM, Histogram::Unit::bytes);
		loop {
			choose {
				when(std::tuple<int, std::string, double> measure = waitNext(metricFutureStream)) {
					std::string metricName = std::get<1>(measure);
					double metricValue = std::get<2>(measure);
					if (metricName == ROCKSDB_COMMIT_LATENCY_HISTOGRAM.toString()) {
						commitLatencyHistogram->sampleSeconds(metricValue);
					} else if (metricName == ROCKSDB_COMMIT_ACTION_HISTOGRAM.toString()) {
						commitActionHistogram->sampleSeconds(metricValue);
					} else if (metricName == ROCKSDB_COMMIT_QUEUEWAIT_HISTOGRAM.toString()) {
						commitQueueWaitHistogram->sampleSeconds(metricValue);
					} else if (metricName == ROCKSDB_WRITE_HISTOGRAM.toString()) {
						writeHistogram->sampleSeconds(metricValue);
					} else if (metricName == ROCKSDB_DELETE_COMPACTRANGE_HISTOGRAM.toString()) {
						deleteCompactRangeHistogram->sampleSeconds(metricValue);
					} else if (metricName == ROCKSDB_READRANGE_LATENCY_HISTOGRAM.toString()) {
						readRangeLatencyHistogram->sampleSeconds(metricValue);
					} else if (metricName == ROCKSDB_READVALUE_LATENCY_HISTOGRAM.toString()) {
						readValueLatencyHistogram->sampleSeconds(metricValue);
					} else if (metricName == ROCKSDB_READPREFIX_LATENCY_HISTOGRAM.toString()) {
						readPrefixLatencyHistogram->sampleSeconds(metricValue);
					} else if (metricName == ROCKSDB_READRANGE_ACTION_HISTOGRAM.toString()) {
						readRangeActionHistogram->sampleSeconds(metricValue);
					} else if (metricName == ROCKSDB_READVALUE_ACTION_HISTOGRAM.toString()) {
						readValueActionHistogram->sampleSeconds(metricValue);
					} else if (metricName == ROCKSDB_READPREFIX_ACTION_HISTOGRAM.toString()) {
						readPrefixActionHistogram->sampleSeconds(metricValue);
					} else if (metricName == ROCKSDB_READRANGE_QUEUEWAIT_HISTOGRAM.toString()) {
						readRangeQueueWaitHistogram->sampleSeconds(metricValue);
					} else if (metricName == ROCKSDB_READVALUE_QUEUEWAIT_HISTOGRAM.toString()) {
						readValueQueueWaitHistogram->sampleSeconds(metricValue);
					} else if (metricName == ROCKSDB_READPREFIX_QUEUEWAIT_HISTOGRAM.toString()) {
						readPrefixQueueWaitHistogram->sampleSeconds(metricValue);
					} else if (metricName == ROCKSDB_READRANGE_NEWITERATOR_HISTOGRAM.toString()) {
						readRangeNewIteratorHistogram->sampleSeconds(metricValue);
					} else if (metricName == ROCKSDB_READVALUE_GET_HISTOGRAM.toString()) {
						readValueGetHistogram->sampleSeconds(metricValue);
					} else if (metricName == ROCKSDB_READPREFIX_GET_HISTOGRAM.toString()) {
						readPrefixGetHistogram->sampleSeconds(metricValue);
					} else if (metricName == ROCKSDB_READ_RANGE_BYTES_RETURNED_HISTOGRAM.toString()) {
						rocksdbReadRangeBytesReturnedHistogram->sample(metricValue);
					} else if (metricName == ROCKSDB_READ_RANGE_KV_PAIRS_RETURNED_HISTOGRAM.toString()) {
						rocksdbReadRangeKVPairsReturnedHistogram->sample(metricValue);
					} else {
						UNREACHABLE();
					}
				}
			}
		}
	}

	Future<Void> getError() const override { return errorFuture; }

	ACTOR static void doClose(RocksDBKeyValueStore* self, bool deleteOnClose) {
		// The metrics future retains a reference to the DB, so stop it before we delete it.
		self->metrics.reset();

		wait(self->readThreads->stop());
		self->readIterPool.reset();
		auto a = new Writer::CloseAction(self->path, deleteOnClose);
		auto f = a->done.getFuture();
		self->writeThread->post(a);
		wait(f);
		wait(self->writeThread->stop());
		if (self->closePromise.canBeSet())
			self->closePromise.send(Void());
		delete self;
	}

	Future<Void> onClosed() const override { return closePromise.getFuture(); }

	void dispose() override { doClose(this, true); }

	void close() override { doClose(this, false); }

	KeyValueStoreType getType() const override { return KeyValueStoreType(KeyValueStoreType::SSD_ROCKSDB_V1); }

	Future<Void> init() override {
		if (openFuture.isValid()) {
			return openFuture;
		}
		auto a = std::make_unique<Writer::OpenAction>(
		    path, metrics, &readSemaphore, &fetchSemaphore, errorListener, counters);
		openFuture = a->done.getFuture();
		writeThread->post(a.release());
		return openFuture;
	}

	void set(KeyValueRef kv, const Arena*) override {
		if (writeBatch == nullptr) {
			writeBatch.reset(new rocksdb::WriteBatch());
			keysSet.clear();
			maxDeletes = SERVER_KNOBS->ROCKSDB_SINGLEKEY_DELETES_MAX;
		}
		ASSERT(defaultFdbCF != nullptr);
		writeBatch->Put(defaultFdbCF, toSlice(kv.key), toSlice(kv.value));
		if (SERVER_KNOBS->ROCKSDB_SINGLEKEY_DELETES_ON_CLEARRANGE) {
			keysSet.insert(kv.key);
		}
	}

	void clear(KeyRangeRef keyRange, const Arena*) override {
		if (writeBatch == nullptr) {
			writeBatch.reset(new rocksdb::WriteBatch());
			keysSet.clear();
			maxDeletes = SERVER_KNOBS->ROCKSDB_SINGLEKEY_DELETES_MAX;
		}

		ASSERT(defaultFdbCF != nullptr);
		// Number of deletes to rocksdb = counters.deleteKeyReqs + convertedDeleteKeyReqs;
		// Number of deleteRanges to rocksdb = counters.deleteRangeReqs - counters.convertedDeleteRangeReqs;
		if (keyRange.singleKeyRange() && !SERVER_KNOBS->ROCKSDB_FORCE_DELETERANGE_FOR_CLEARRANGE) {
			writeBatch->Delete(defaultFdbCF, toSlice(keyRange.begin));
			++counters.deleteKeyReqs;
			--maxDeletes;
		} else {
			++counters.deleteRangeReqs;
			if (SERVER_KNOBS->ROCKSDB_SINGLEKEY_DELETES_ON_CLEARRANGE &&
			    !SERVER_KNOBS->ROCKSDB_FORCE_DELETERANGE_FOR_CLEARRANGE && maxDeletes > 0) {
				++counters.convertedDeleteRangeReqs;
				rocksdb::ReadOptions options = getReadOptions();
				auto beginSlice = toSlice(keyRange.begin);
				auto endSlice = toSlice(keyRange.end);
				options.iterate_lower_bound = &beginSlice;
				options.iterate_upper_bound = &endSlice;
				auto cursor = std::unique_ptr<rocksdb::Iterator>(db->NewIterator(options, defaultFdbCF));
				cursor->Seek(toSlice(keyRange.begin));
				while (cursor->Valid() && toStringRef(cursor->key()) < keyRange.end && maxDeletes > 0) {
					writeBatch->Delete(defaultFdbCF, cursor->key());
					++counters.convertedDeleteKeyReqs;
					--maxDeletes;
					cursor->Next();
				}
				if (!cursor->status().ok() || maxDeletes <= 0) {
					// if readrange iteration fails, then do a deleteRange.
					writeBatch->DeleteRange(defaultFdbCF, toSlice(keyRange.begin), toSlice(keyRange.end));
				} else {
					auto it = keysSet.lower_bound(keyRange.begin);
					while (it != keysSet.end() && *it < keyRange.end) {
						writeBatch->Delete(defaultFdbCF, toSlice(*it));
						++counters.convertedDeleteKeyReqs;
						--maxDeletes;
						it++;
					}
					it = previousCommitKeysSet.lower_bound(keyRange.begin);
					while (it != previousCommitKeysSet.end() && *it < keyRange.end) {
						writeBatch->Delete(defaultFdbCF, toSlice(*it));
						++counters.convertedDeleteKeyReqs;
						--maxDeletes;
						it++;
					}
				}
			} else {
				writeBatch->DeleteRange(defaultFdbCF, toSlice(keyRange.begin), toSlice(keyRange.end));
			}
		}
	}

	// Checks and waits for few seconds if rocskdb is overloaded.
	ACTOR Future<Void> checkRocksdbState(RocksDBKeyValueStore* self) {
		state uint64_t estPendCompactBytes;
		state int count = SERVER_KNOBS->ROCKSDB_CAN_COMMIT_DELAY_TIMES_ON_OVERLOAD;
		self->db->GetAggregatedIntProperty(rocksdb::DB::Properties::kEstimatePendingCompactionBytes,
		                                   &estPendCompactBytes);
		while (count && estPendCompactBytes > SERVER_KNOBS->ROCKSDB_CAN_COMMIT_COMPACT_BYTES_LIMIT) {
			wait(delay(SERVER_KNOBS->ROCKSDB_CAN_COMMIT_DELAY_ON_OVERLOAD));
			++self->counters.commitDelayed;
			count--;
			self->db->GetAggregatedIntProperty(rocksdb::DB::Properties::kEstimatePendingCompactionBytes,
			                                   &estPendCompactBytes);
			if (deterministicRandom()->random01() < 0.001)
				TraceEvent(SevWarn, "RocksDBCommitsDelayed1000x", self->id);
		}

		return Void();
	}

	Future<Void> canCommit() override { return checkRocksdbState(this); }

	ACTOR Future<Void> commitInRocksDB(RocksDBKeyValueStore* self) {
		// If there is nothing to write, don't write.
		if (self->writeBatch == nullptr) {
			return Void();
		}
		auto a = new Writer::CommitAction();
		a->batchToCommit = std::move(self->writeBatch);
		self->previousCommitKeysSet = std::move(self->keysSet);
		self->maxDeletes = SERVER_KNOBS->ROCKSDB_SINGLEKEY_DELETES_MAX;
		state Future<Void> fut = a->done.getFuture();
		self->writeThread->post(a);
		wait(fut);
		self->previousCommitKeysSet.clear();
		return Void();
	}

	Future<Void> commit(bool) override { return commitInRocksDB(this); }

	void checkWaiters(const FlowLock& semaphore, int maxWaiters) {
		if (semaphore.waiters() > maxWaiters) {
			++counters.immediateThrottle;
			throw server_overloaded();
		}
	}

	// We don't throttle eager reads and reads to the FF keyspace because FDB struggles when those reads fail.
	// Thus far, they have been low enough volume to not cause an issue.
	static bool shouldThrottle(IKeyValueStore::ReadType type, KeyRef key) {
		return type != IKeyValueStore::ReadType::EAGER && !(key.startsWith(systemKeys.begin));
	}

	ACTOR template <class Action>
	static Future<Optional<Value>> read(Action* action, FlowLock* semaphore, IThreadPool* pool, Counter* counter) {
		state std::unique_ptr<Action> a(action);
		state Optional<Void> slot = wait(timeout(semaphore->take(), SERVER_KNOBS->ROCKSDB_READ_QUEUE_WAIT));
		if (!slot.present()) {
			++(*counter);
			throw server_overloaded();
		}

		state FlowLock::Releaser release(*semaphore);

		auto fut = a->result.getFuture();
		pool->post(a.release());
		Optional<Value> result = wait(fut);

		return result;
	}

	Future<Optional<Value>> readValue(KeyRef key, IKeyValueStore::ReadType type, Optional<UID> debugID) override {
		if (!shouldThrottle(type, key)) {
			auto a = new Reader::ReadValueAction(key, type, debugID);
			auto res = a->result.getFuture();
			readThreads->post(a);
			return res;
		}

		auto& semaphore = (type == IKeyValueStore::ReadType::FETCH) ? fetchSemaphore : readSemaphore;
		int maxWaiters = (type == IKeyValueStore::ReadType::FETCH) ? numFetchWaiters : numReadWaiters;

		checkWaiters(semaphore, maxWaiters);
		auto a = std::make_unique<Reader::ReadValueAction>(key, type, debugID);
		return read(a.release(), &semaphore, readThreads.getPtr(), &counters.failedToAcquire);
	}

	Future<Optional<Value>> readValuePrefix(KeyRef key,
	                                        int maxLength,
	                                        IKeyValueStore::ReadType type,
	                                        Optional<UID> debugID) override {
		if (!shouldThrottle(type, key)) {
			auto a = new Reader::ReadValuePrefixAction(key, maxLength, type, debugID);
			auto res = a->result.getFuture();
			readThreads->post(a);
			return res;
		}

		auto& semaphore = (type == IKeyValueStore::ReadType::FETCH) ? fetchSemaphore : readSemaphore;
		int maxWaiters = (type == IKeyValueStore::ReadType::FETCH) ? numFetchWaiters : numReadWaiters;

		checkWaiters(semaphore, maxWaiters);
		auto a = std::make_unique<Reader::ReadValuePrefixAction>(key, maxLength, type, debugID);
		return read(a.release(), &semaphore, readThreads.getPtr(), &counters.failedToAcquire);
	}

	ACTOR static Future<Standalone<RangeResultRef>> read(Reader::ReadRangeAction* action,
	                                                     FlowLock* semaphore,
	                                                     IThreadPool* pool,
	                                                     Counter* counter) {
		state std::unique_ptr<Reader::ReadRangeAction> a(action);
		state Optional<Void> slot = wait(timeout(semaphore->take(), SERVER_KNOBS->ROCKSDB_READ_QUEUE_WAIT));
		if (!slot.present()) {
			++(*counter);
			throw server_overloaded();
		}

		state FlowLock::Releaser release(*semaphore);

		auto fut = a->result.getFuture();
		pool->post(a.release());
		Standalone<RangeResultRef> result = wait(fut);

		return result;
	}

	Future<RangeResult> readRange(KeyRangeRef keys,
	                              int rowLimit,
	                              int byteLimit,
	                              IKeyValueStore::ReadType type) override {
		if (!shouldThrottle(type, keys.begin)) {
			auto a = new Reader::ReadRangeAction(keys, rowLimit, byteLimit, type, counters);
			auto res = a->result.getFuture();
			readThreads->post(a);
			return res;
		}

		auto& semaphore = (type == IKeyValueStore::ReadType::FETCH) ? fetchSemaphore : readSemaphore;
		int maxWaiters = (type == IKeyValueStore::ReadType::FETCH) ? numFetchWaiters : numReadWaiters;

		checkWaiters(semaphore, maxWaiters);
		auto a = std::make_unique<Reader::ReadRangeAction>(keys, rowLimit, byteLimit, type, counters);
		return read(a.release(), &semaphore, readThreads.getPtr(), &counters.failedToAcquire);
	}

	StorageBytes getStorageBytes() const override {
		uint64_t live = 0;
		ASSERT(db->GetAggregatedIntProperty(rocksdb::DB::Properties::kLiveSstFilesSize, &live));

		int64_t free;
		int64_t total;
		g_network->getDiskBytes(path, free, total);

		return StorageBytes(free, total, live, free);
	}

	Future<CheckpointMetaData> checkpoint(const CheckpointRequest& request) override {
		auto a = new Writer::CheckpointAction(request);

		auto res = a->reply.getFuture();
		writeThread->post(a);
		return res;
	}

	Future<Void> restore(const std::vector<CheckpointMetaData>& checkpoints) override {
		auto a = new Writer::RestoreAction(path, checkpoints);
		auto res = a->done.getFuture();
		writeThread->post(a);
		return res;
	}

	// Delete a checkpoint.
	Future<Void> deleteCheckpoint(const CheckpointMetaData& checkpoint) override {
		if (checkpoint.format == RocksDBColumnFamily) {
			RocksDBColumnFamilyCheckpoint rocksCF;
			ObjectReader reader(checkpoint.serializedCheckpoint.begin(), IncludeVersion());
			reader.deserialize(rocksCF);

			std::unordered_set<std::string> dirs;
			for (const LiveFileMetaData& file : rocksCF.sstFiles) {
				dirs.insert(file.db_path);
			}
			for (const std::string dir : dirs) {
				platform::eraseDirectoryRecursive(dir);
				TraceEvent("DeleteCheckpointRemovedDir", id)
				    .detail("CheckpointID", checkpoint.checkpointID)
				    .detail("Dir", dir);
			}
		} else if (checkpoint.format == RocksDB) {
			throw not_implemented();
		} else {
			throw internal_error();
		}
		return Void();
	}
};

} // namespace

#endif // SSD_ROCKSDB_EXPERIMENTAL

IKeyValueStore* keyValueStoreRocksDB(std::string const& path,
                                     UID logID,
                                     KeyValueStoreType storeType,
                                     bool checkChecksums,
                                     bool checkIntegrity) {
#ifdef SSD_ROCKSDB_EXPERIMENTAL
	return new RocksDBKeyValueStore(path, logID);
#else
	TraceEvent(SevError, "RocksDBEngineInitFailure", logID).detail("Reason", "Built without RocksDB");
	ASSERT(false);
	return nullptr;
#endif // SSD_ROCKSDB_EXPERIMENTAL
}

#ifdef SSD_ROCKSDB_EXPERIMENTAL
#include "flow/UnitTest.h"

namespace {

TEST_CASE("noSim/fdbserver/KeyValueStoreRocksDB/RocksDBBasic") {
	state const std::string rocksDBTestDir = "rocksdb-kvstore-basic-test-db";
	platform::eraseDirectoryRecursive(rocksDBTestDir);

	state IKeyValueStore* kvStore = new RocksDBKeyValueStore(rocksDBTestDir, deterministicRandom()->randomUniqueID());
	wait(kvStore->init());

	state StringRef foo = "foo"_sr;
	state StringRef bar = "ibar"_sr;
	kvStore->set({ foo, foo });
	kvStore->set({ keyAfter(foo), keyAfter(foo) });
	kvStore->set({ bar, bar });
	kvStore->set({ keyAfter(bar), keyAfter(bar) });
	wait(kvStore->commit(false));

	{
		Optional<Value> val = wait(kvStore->readValue(foo));
		ASSERT(foo == val.get());
	}

	// Test single key deletion.
	kvStore->clear(singleKeyRange(foo));
	wait(kvStore->commit(false));

	{
		Optional<Value> val = wait(kvStore->readValue(foo));
		ASSERT(!val.present());
	}

	{
		Optional<Value> val = wait(kvStore->readValue(keyAfter(foo)));
		ASSERT(keyAfter(foo) == val.get());
	}

	// Test range deletion.
	kvStore->clear(KeyRangeRef(keyAfter(foo), keyAfter(bar)));
	wait(kvStore->commit(false));

	{
		Optional<Value> val = wait(kvStore->readValue(bar));
		ASSERT(!val.present());
	}

	{
		Optional<Value> val = wait(kvStore->readValue(keyAfter(bar)));
		ASSERT(keyAfter(bar) == val.get());
	}

	Future<Void> closed = kvStore->onClosed();
	kvStore->close();
	wait(closed);

	platform::eraseDirectoryRecursive(rocksDBTestDir);
	return Void();
}

TEST_CASE("noSim/fdbserver/KeyValueStoreRocksDB/RocksDBReopen") {
	state const std::string rocksDBTestDir = "rocksdb-kvstore-reopen-test-db";
	platform::eraseDirectoryRecursive(rocksDBTestDir);

	state IKeyValueStore* kvStore = new RocksDBKeyValueStore(rocksDBTestDir, deterministicRandom()->randomUniqueID());
	wait(kvStore->init());

	kvStore->set({ LiteralStringRef("foo"), LiteralStringRef("bar") });
	wait(kvStore->commit(false));

	Optional<Value> val = wait(kvStore->readValue(LiteralStringRef("foo")));
	ASSERT(Optional<Value>(LiteralStringRef("bar")) == val);

	Future<Void> closed = kvStore->onClosed();
	kvStore->close();
	wait(closed);

	kvStore = new RocksDBKeyValueStore(rocksDBTestDir, deterministicRandom()->randomUniqueID());
	wait(kvStore->init());
	// Confirm that `init()` is idempotent.
	wait(kvStore->init());

	Optional<Value> val = wait(kvStore->readValue(LiteralStringRef("foo")));
	ASSERT(Optional<Value>(LiteralStringRef("bar")) == val);

	Future<Void> closed = kvStore->onClosed();
	kvStore->close();
	wait(closed);

	platform::eraseDirectoryRecursive(rocksDBTestDir);
	return Void();
}

TEST_CASE("noSim/fdbserver/KeyValueStoreRocksDB/CheckpointRestore") {
	state std::string cwd = platform::getWorkingDirectory() + "/";
	state std::string rocksDBTestDir = "rocksdb-kvstore-br-test-db";
	platform::eraseDirectoryRecursive(rocksDBTestDir);

	state IKeyValueStore* kvStore = new RocksDBKeyValueStore(rocksDBTestDir, deterministicRandom()->randomUniqueID());
	wait(kvStore->init());

	kvStore->set({ LiteralStringRef("foo"), LiteralStringRef("bar") });
	wait(kvStore->commit(false));

	Optional<Value> val = wait(kvStore->readValue(LiteralStringRef("foo")));
	ASSERT(Optional<Value>(LiteralStringRef("bar")) == val);

	platform::eraseDirectoryRecursive("checkpoint");
	state std::string checkpointDir = cwd + "checkpoint";

	CheckpointRequest request(
	    latestVersion, allKeys, RocksDBColumnFamily, deterministicRandom()->randomUniqueID(), checkpointDir);
	CheckpointMetaData metaData = wait(kvStore->checkpoint(request));

	state std::string rocksDBRestoreDir = "rocksdb-kvstore-br-restore-db";
	platform::eraseDirectoryRecursive(rocksDBRestoreDir);

	state IKeyValueStore* kvStoreCopy =
	    new RocksDBKeyValueStore(rocksDBRestoreDir, deterministicRandom()->randomUniqueID());

	std::vector<CheckpointMetaData> checkpoints;
	checkpoints.push_back(metaData);
	wait(kvStoreCopy->restore(checkpoints));

	Optional<Value> val = wait(kvStoreCopy->readValue(LiteralStringRef("foo")));
	ASSERT(Optional<Value>(LiteralStringRef("bar")) == val);

	std::vector<Future<Void>> closes;
	closes.push_back(kvStore->onClosed());
	closes.push_back(kvStoreCopy->onClosed());
	kvStore->close();
	kvStoreCopy->close();
	wait(waitForAll(closes));

	platform::eraseDirectoryRecursive(rocksDBTestDir);
	platform::eraseDirectoryRecursive(rocksDBRestoreDir);

	return Void();
}

TEST_CASE("noSim/fdbserver/KeyValueStoreRocksDB/RocksDBTypes") {
	// If the following assertion fails, update SstFileMetaData and LiveFileMetaData in RocksDBCheckpointUtils.actor.h
	// to be the same as rocksdb::SstFileMetaData and rocksdb::LiveFileMetaData.
	ASSERT_EQ(sizeof(rocksdb::LiveFileMetaData), 208);
	ASSERT_EQ(sizeof(rocksdb::ExportImportFilesMetaData), 32);
	return Void();
}

} // namespace

#endif // SSD_ROCKSDB_EXPERIMENTAL