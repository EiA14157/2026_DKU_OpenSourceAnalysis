#ifndef LAB2_SKIPLIST_H
#define LAB2_SKIPLIST_H

#include <cstdint>
#include <limits>
#include <random>
#include <string>
#include <vector>

// Skip list used as the underlying memtable structure.
// Keys are ordered by (key ascending, seq descending) so that the newest
// version of the same key is visited first during lookups and scans.
class SkipList {
 public:
  struct RangeEntry {
    int key;
    std::string value;
    bool tombstone;
  };

  explicit SkipList(int max_level = 16, float p = 0.5f);
  ~SkipList();

  SkipList(const SkipList&) = delete;
  SkipList& operator=(const SkipList&) = delete;

  void Put(int key, const std::string& value);
  bool Get(int key, std::string* out_value) const;
  bool Delete(int key);
  std::vector<RangeEntry> RangeScan(int start_key, int end_key) const;

 private:
  struct Node {
    int key;
    int64_t seq;
    std::string value;
    bool tombstone;
    Node* next;
    Node* down;
  };

  int RandomLevel();
  Node* FindGreaterOrEqual(int key, int64_t seq,
                           std::vector<Node*>* update) const;
  static bool Less(int a_key, int64_t a_seq, int b_key, int64_t b_seq);

  Node* head_;
  int max_level_;
  int current_level_;
  float p_;
  int64_t next_seq_;
  mutable std::mt19937 rng_;
};

#endif  // LAB2_SKIPLIST_H
