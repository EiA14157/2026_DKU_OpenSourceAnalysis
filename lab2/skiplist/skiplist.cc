#include "skiplist.h"

#include <algorithm>
#include <unordered_set>

SkipList::SkipList(int max_level, float p)
    : head_(nullptr),
      max_level_(std::max(1, max_level)),
      current_level_(1),
      p_(p),
      next_seq_(1),
      rng_(std::random_device{}()) {
  // Build a vertical tower of head nodes once at construction time.
  // Every level starts with a sentinel node so insertion/search logic can
  // treat the beginning of the list the same as any other position.
  head_ = new Node{std::numeric_limits<int>::min(),
                   std::numeric_limits<int64_t>::max(), "", false, nullptr,
                   nullptr};
  Node* current = head_;
  for (int level = 1; level < max_level_; ++level) {
    current->down = new Node{std::numeric_limits<int>::min(),
                             std::numeric_limits<int64_t>::max(), "", false,
                             nullptr, nullptr};
    current = current->down;
  }
}

SkipList::~SkipList() {
  // Each level owns its own horizontal linked list of nodes.
  // Because upper/lower nodes are allocated separately, we free one level at a
  // time by following next pointers, then move downward with down pointers.
  Node* level_head = head_;
  while (level_head != nullptr) {
    Node* next_level = level_head->down;
    Node* node = level_head;
    while (node != nullptr) {
      Node* next = node->next;
      delete node;
      node = next;
    }
    level_head = next_level;
  }
}

int SkipList::RandomLevel() {
  // A node starts at level 1 and is promoted while the coin flip succeeds.
  // This keeps the structure probabilistically balanced.
  int level = 1;
  std::bernoulli_distribution dist(p_);
  while (level < max_level_ && dist(rng_)) {
    ++level;
  }
  return level;
}

bool SkipList::Less(int a_key, int64_t a_seq, int b_key, int64_t b_seq) {
  // Primary order: smaller key comes first.
  // Secondary order for the same key: larger sequence number comes first.
  // This makes the newest version of a key appear before older versions.
  if (a_key != b_key) {
    return a_key < b_key;
  }
  return a_seq > b_seq;
}

SkipList::Node* SkipList::FindGreaterOrEqual(int key, int64_t seq,
                                             std::vector<Node*>* update) const {
  if (update != nullptr) {
    // update[i] stores the node that should precede the new node at level i.
    update->assign(max_level_, nullptr);
  }

  Node* current = head_;
  int level = max_level_ - 1;
  while (current != nullptr) {
    // Move right while the next node is still strictly smaller than the target
    // in our (key asc, seq desc) ordering.
    while (current->next != nullptr &&
           Less(current->next->key, current->next->seq, key, seq)) {
      current = current->next;
    }
    if (update != nullptr) {
      (*update)[level] = current;
    }
    if (current->down == nullptr) {
      // Bottom level reached. current->next is the first candidate node that is
      // >= (key, seq), or nullptr if there is no such node.
      return current->next;
    }
    current = current->down;
    --level;
  }
  return nullptr;
}

void SkipList::Put(int key, const std::string& value) {
  // Every insert creates a brand-new version with a fresh sequence number.
  // Older versions stay in the list to support out-of-place updates.
  const int64_t seq = next_seq_++;
  const int level = RandomLevel();
  current_level_ = std::max(current_level_, level);

  std::vector<Node*> update;
  FindGreaterOrEqual(key, seq, &update);

  Node* down = nullptr;
  for (int idx = 0; idx < level; ++idx) {
    Node* prev = update[idx];
    // Insert one node per level and connect the tower with down pointers.
    Node* node = new Node{key, seq, value, false, prev->next, down};
    prev->next = node;
    down = node;
  }
}

bool SkipList::Get(int key, std::string* out_value) const {
  // Search using the largest possible sequence number so the first matching
  // node for this key is the newest version.
  Node* node = FindGreaterOrEqual(key, std::numeric_limits<int64_t>::max(),
                                  nullptr);
  if (node == nullptr || node->key != key || node->tombstone) {
    return false;
  }

  if (out_value != nullptr) {
    *out_value = node->value;
  }
  return true;
}

bool SkipList::Delete(int key) {
  // Delete is implemented as another versioned insert, but marked as a
  // tombstone so upper layers can treat the key as removed.
  const int64_t seq = next_seq_++;
  const int level = RandomLevel();
  current_level_ = std::max(current_level_, level);

  std::vector<Node*> update;
  FindGreaterOrEqual(key, seq, &update);

  Node* down = nullptr;
  for (int idx = 0; idx < level; ++idx) {
    Node* prev = update[idx];
    // Tombstone nodes participate in ordering exactly like normal values.
    Node* node = new Node{key, seq, "", true, prev->next, down};
    prev->next = node;
    down = node;
  }
  return true;
}

std::vector<SkipList::RangeEntry> SkipList::RangeScan(int start_key,
                                                      int end_key) const {
  std::vector<RangeEntry> out;
  if (start_key > end_key) {
    return out;
  }

  // Start from the first entry whose key is >= start_key. Because entries of
  // the same key are stored in descending sequence order, the first instance
  // we meet for a key is the newest visible version inside this skip list.
  Node* node = FindGreaterOrEqual(start_key, std::numeric_limits<int64_t>::max(),
                                  nullptr);
  std::unordered_set<int> seen_keys;

  while (node != nullptr && node->key <= end_key) {
    // Consecutive duplicates belong to older versions of the same user key.
    // Only the first one should be returned from a single memtable scan.
    if (seen_keys.insert(node->key).second) {
      out.push_back(RangeEntry{node->key, node->value, node->tombstone});
    }
    node = node->next;
  }

  return out;
}
