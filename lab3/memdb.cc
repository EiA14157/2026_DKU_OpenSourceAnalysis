#include "memdb.h"

#include "compaction.h"

#include <algorithm>
#include <limits>
#include <map>
#include <stdexcept>
#include <utility>

LSMDB::MemTable::MemTable(const MemDBOptions& options)
    : list(options.skiplist_max_height, options.skiplist_p), size_bytes(0),
      immutable(false) {}

LSMDB::LSMDB(const MemDBOptions& options)
    : options_(options), mutable_(std::make_unique<MemTable>(options_)),
      next_file_id_(1), next_seq_(1) {
  EnsureSSTDir(options_.sst_dir);
  if (options_.use_existing_db) {
    flushed_files_ =
        ListSSTables(options_.sst_dir, options_.enable_sstable_bloom_filter);
    if (!flushed_files_.empty()) {
      next_file_id_ = flushed_files_.back().id + 1;
    }
    int64_t max_seq = 0;
    for (const auto& file : flushed_files_) {
      max_seq = std::max(max_seq, file.newest_seq);
    }
    next_seq_ = max_seq + 1;
  } else if (!IsSSTDirEmpty(options_.sst_dir)) {
    throw std::runtime_error(
        "sst_dir is not empty; set use_existing_db=true to reuse it");
  }
}

void LSMDB::Put(int key, const std::string& value) {
  const size_t entry_bytes = EntryBytes(key, value);
  EnsureMutableCapacity(entry_bytes);
  // Sequence numbers keep out-of-place updates globally ordered.
  mutable_->list.PutWithSequence(key, value, next_seq_++);
  mutable_->size_bytes += entry_bytes;
}

bool LSMDB::Get(int key, std::string* out_value) const {
  std::string value;
  bool tombstone = false;
  // Search order: mutable memtable -> immutable memtables -> newest SSTables.
  // The first matching entry is the newest visible version of the key.
  if (mutable_->list.GetLatest(key, &value, &tombstone)) {
    if (tombstone) {
      return false;
    }
    if (out_value != nullptr) {
      *out_value = value;
    }
    return true;
  }

  for (auto it = immutables_.rbegin(); it != immutables_.rend(); ++it) {
    if ((*it)->list.GetLatest(key, &value, &tombstone)) {
      if (tombstone) {
        return false;
      }
      if (out_value != nullptr) {
        *out_value = value;
      }
      return true;
    }
  }

  for (auto it = flushed_files_.rbegin(); it != flushed_files_.rend(); ++it) {
    if (GetFromSSTable(*it, key, &value, &tombstone)) {
      if (tombstone) {
        return false;
      }
      if (out_value != nullptr) {
        *out_value = value;
      }
      return true;
    }
  }
  return false;
}

void LSMDB::Delete(int key) {
  const size_t entry_bytes = EntryBytes(key, "");
  EnsureMutableCapacity(entry_bytes);
  // Delete is stored as a tombstone instead of removing older versions in place.
  mutable_->list.DeleteWithSequence(key, next_seq_++);
  mutable_->size_bytes += entry_bytes;
}

std::vector<std::pair<int, std::string>> LSMDB::RangeScan(int start_key,
                                                          int end_key) const {
  std::vector<std::pair<int, std::string>> out;
  if (start_key > end_key) {
    return out;
  }

  std::map<int, std::pair<bool, std::string>> latest;
  // Merge every level from newest to oldest and keep only the first version
  // seen for each user key.
  auto merge_skiplist = [&](const std::vector<SkipList::RangeEntry>& entries) {
    for (const auto& entry : entries) {
      if (latest.find(entry.key) != latest.end()) {
        continue;
      }
      latest[entry.key] = {!entry.tombstone, entry.value};
    }
  };

  merge_skiplist(mutable_->list.RangeScanLatest(start_key, end_key));
  for (auto it = immutables_.rbegin(); it != immutables_.rend(); ++it) {
    merge_skiplist((*it)->list.RangeScanLatest(start_key, end_key));
  }
  for (auto it = flushed_files_.rbegin(); it != flushed_files_.rend(); ++it) {
    auto entries = RangeScanSSTable(*it, start_key, end_key);
    for (const auto& entry : entries) {
      if (latest.find(entry.key) != latest.end()) {
        continue;
      }
      latest[entry.key] = {!entry.tombstone, entry.value};
    }
  }

  for (const auto& kv : latest) {
    if (kv.second.first) {
      out.emplace_back(kv.first, kv.second.second);
    }
  }
  return out;
}

size_t LSMDB::ImmutableCount() const { return immutables_.size(); }

size_t LSMDB::FlushedFileCount() const { return flushed_files_.size(); }

size_t LSMDB::MutableSizeBytes() const { return mutable_->size_bytes; }

void LSMDB::EnsureMutableCapacity(size_t entry_bytes) {
  if (mutable_->size_bytes == 0) {
    return;
  }
  if (mutable_->size_bytes + entry_bytes <= options_.max_memtable_bytes) {
    return;
  }

  // Once the mutable memtable is full, freeze it and flush it to disk.
  mutable_->immutable = true;
  immutables_.push_back(std::move(mutable_));
  mutable_ = std::make_unique<MemTable>(options_);
  FlushAllImmutables();
}

size_t LSMDB::EntryBytes(int key, const std::string& value) const {
  return sizeof(key) + value.size();
}

void LSMDB::FlushAllImmutables() {
  while (!immutables_.empty()) {
    FlushOneImmutable(immutables_.front().get());
    immutables_.erase(immutables_.begin());
  }
}

void LSMDB::FlushOneImmutable(const MemTable* table) {
  const uint64_t file_id = next_file_id_++;
  std::vector<SSTableEntry> flush_entries;
  // Flush only the newest version of each key from this memtable.
  auto entries = table->list.RangeScanLatest(std::numeric_limits<int>::min(),
                                             std::numeric_limits<int>::max());
  flush_entries.reserve(entries.size());
  for (const auto& entry : entries) {
    flush_entries.push_back(
        SSTableEntry{entry.key, entry.seq, entry.value, entry.tombstone});
  }

  flushed_files_.push_back(WriteSSTable(
      options_.sst_dir, file_id, flush_entries,
      options_.enable_sstable_bloom_filter, options_.sstable_bloom_bits_per_key,
      options_.sstable_bloom_hash_count));
  MaybeCompactSSTables();
}

void LSMDB::MaybeCompactSSTables() {
  if (!options_.enable_compaction || options_.compaction_file_threshold == 0 ||
      flushed_files_.size() <= options_.compaction_file_threshold) {
    return;
  }

  // This lab compacts the full SSTable set whenever the file count crosses the
  // configured threshold.
  const uint64_t file_id = next_file_id_++;
  auto compacted_file = CompactAllSSTables(
      options_.sst_dir, flushed_files_, file_id,
      options_.enable_sstable_bloom_filter, options_.sstable_bloom_bits_per_key,
      options_.sstable_bloom_hash_count);

  flushed_files_.clear();
  if (compacted_file.has_value()) {
    flushed_files_.push_back(std::move(*compacted_file));
  }
}
