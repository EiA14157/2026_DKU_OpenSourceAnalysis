#include "memdb.h"

#include <map>
#include <memory>
#include <utility>

// Each MemTable owns one skip list and tracks its approximate byte usage.
// The mutable table accepts new writes; full tables are moved into the
// immutable list and are still searched for reads/range scans.
InMemoryDB::MemTable::MemTable(const MemDBOptions& options)
    : list(options.skiplist_max_height, options.skiplist_p),
      size_bytes(0),
      immutable(false) {}

InMemoryDB::InMemoryDB(const MemDBOptions& options)
    : options_(options), mutable_(std::make_unique<MemTable>(options_)) {}

// Insert always goes to the current mutable memtable.
// If the write would exceed the configured limit, rotate the current mutable
// table into the immutable set and start writing into a fresh one.
void InMemoryDB::Put(int key, const std::string& value) {
  const size_t entry_bytes = EntryBytes(key, value);
  EnsureMutableCapacity(entry_bytes);

  mutable_->list.Put(key, value);
  mutable_->size_bytes += entry_bytes;
}

// Lookup must search the newest memtable first.
// That is why we probe mutable_ before immutables_, and immutables_ in reverse
// creation order. The first match is the newest visible version of the key.
bool InMemoryDB::Get(int key, std::string* out_value) const {
  auto probe = [&](const MemTable& memtable) -> int {
    // RangeScan(key, key) returns at most one newest version for this key from
    // the target skip list.
    std::vector<SkipList::RangeEntry> entries = memtable.list.RangeScan(key, key);
    if (entries.empty()) {
      return 0;
    }

    if (entries[0].tombstone) {
      // A tombstone means the newest version is logically deleted.
      return -1;
    }

    if (out_value != nullptr) {
      *out_value = entries[0].value;
    }
    return 1;
  };

  int state = probe(*mutable_);
  if (state != 0) {
    return state > 0;
  }

  for (auto it = immutables_.rbegin(); it != immutables_.rend(); ++it) {
    state = probe(*(*it));
    if (state != 0) {
      return state > 0;
    }
  }

  return false;
}

// Delete also appends to the mutable memtable, but as a tombstone entry.
// Older values remain in older memtables and are masked by this tombstone.
void InMemoryDB::Delete(int key) {
  const size_t entry_bytes = EntryBytes(key, "");
  EnsureMutableCapacity(entry_bytes);

  mutable_->list.Delete(key);
  mutable_->size_bytes += entry_bytes;
}

// Range scan merges all memtables from newest to oldest.
// The first time we see a key, we keep that version and ignore any older ones.
// Tombstoned keys are recorded first as deleted, then filtered out at the end.
std::vector<std::pair<int, std::string>> InMemoryDB::RangeScan(
    int start_key, int end_key) const {
  std::vector<std::pair<int, std::string>> out;
  std::map<int, std::pair<bool, std::string>> latest;

  auto merge = [&](const MemTable& memtable) {
    std::vector<SkipList::RangeEntry> entries =
        memtable.list.RangeScan(start_key, end_key);
    for (const auto& entry : entries) {
      if (latest.find(entry.key) != latest.end()) {
        continue;
      }
      latest[entry.key] = std::make_pair(!entry.tombstone, entry.value);
    }
  };

  merge(*mutable_);
  for (auto it = immutables_.rbegin(); it != immutables_.rend(); ++it) {
    merge(*(*it));
  }

  for (const auto& kv : latest) {
    if (kv.second.first) {
      out.push_back(std::make_pair(kv.first, kv.second.second));
    }
  }

  return out;
}

// Rotate the mutable memtable only when it already contains data and the next
// write would overflow the configured size limit.
void InMemoryDB::EnsureMutableCapacity(size_t entry_bytes) {
  if (mutable_->size_bytes == 0) {
    return;
  }

  if (mutable_->size_bytes + entry_bytes <= options_.max_memtable_bytes) {
    return;
  }

  mutable_->immutable = true;
  immutables_.push_back(std::move(mutable_));
  mutable_ = std::make_unique<MemTable>(options_);
}

// Helper methods used by tests/benchmarks.
size_t InMemoryDB::ImmutableCount() const { return immutables_.size(); }

size_t InMemoryDB::MutableSizeBytes() const { return mutable_->size_bytes; }

size_t InMemoryDB::EntryBytes(int key, const std::string& value) const {
  // This is a simple approximation used only for memtable rotation logic.
  return sizeof(key) + value.size();
}
