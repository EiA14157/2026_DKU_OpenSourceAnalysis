#ifndef LAB2_MEMDB_H
#define LAB2_MEMDB_H

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "skiplist.h"

struct MemDBOptions {
  size_t max_memtable_bytes = 4 * 1024 * 1024;
  float skiplist_p = 0.5f;
  int skiplist_max_height = 16;
};

class InMemoryDB {
 public:
  explicit InMemoryDB(const MemDBOptions& options);

  void Put(int key, const std::string& value);
  bool Get(int key, std::string* out_value) const;
  void Delete(int key);
  std::vector<std::pair<int, std::string>> RangeScan(int start_key,
                                                     int end_key) const;

  size_t ImmutableCount() const;
  size_t MutableSizeBytes() const;

 private:
  struct MemTable {
    explicit MemTable(const MemDBOptions& options);

    SkipList list;
    size_t size_bytes;
    bool immutable;
  };

  void EnsureMutableCapacity(size_t entry_bytes);
  size_t EntryBytes(int key, const std::string& value) const;

  MemDBOptions options_;
  std::unique_ptr<MemTable> mutable_;
  std::vector<std::unique_ptr<MemTable>> immutables_;
};

#endif  // LAB2_MEMDB_H
