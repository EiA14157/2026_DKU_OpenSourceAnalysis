#include "memdb.h"

InMemoryDB::InMemoryDB(const MemDBOptions& options)
    : tree_(options.bptree_degree) {}

// The B+Tree version of MemDB is just a thin wrapper around one tree.
// Unlike the skip list version, there is no mutable/immutable memtable split.
void InMemoryDB::Put(int key, const std::string& value) { tree_.Put(key, value); }

bool InMemoryDB::Get(int key, std::string* out_value) const {
  return tree_.Get(key, out_value);
}

bool InMemoryDB::Delete(int key) { return tree_.Delete(key); }

std::vector<std::pair<int, std::string>> InMemoryDB::RangeScan(
    int start_key, int end_key) const {
  return tree_.RangeScan(start_key, end_key);
}
