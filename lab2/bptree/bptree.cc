#include "bptree.h"

#include <algorithm>
#include <stdexcept>

// degree_ is interpreted as the maximum number of children for an internal
// node. We clamp it to at least 3 so split/merge logic stays meaningful.
BPlusTree::BPlusTree(int degree)
    : root_(nullptr), degree_(std::max(3, degree)) {
  // code
}

// Recursively free every node in the tree.
BPlusTree::~BPlusTree() {
  // code
  Destroy(root_);
}

void BPlusTree::Destroy(Node* node) {
  if (node == nullptr) {
    return;
  }

  // Internal nodes own their children; leaf nodes own only their own storage.
  if (!node->is_leaf) {
    for (Node* child : node->children) {
      Destroy(child);
    }
  }

  delete node;
}

BPlusTree::Node* BPlusTree::FindLeaf(int key) const {
  Node* node = root_;
  while (node != nullptr && !node->is_leaf) {
    // Internal keys store separators. upper_bound chooses the child whose range
    // should contain the target key.
    const int idx = static_cast<int>(
        std::upper_bound(node->keys.begin(), node->keys.end(), key) -
        node->keys.begin());
    node = node->children[idx];
  }
  return node;
}

int BPlusTree::FindChildIndex(const Node* parent, const Node* child) const {
  // Parent nodes store children in a vector, so we find the exact slot when
  // splitting/merging/rebalancing.
  for (size_t i = 0; i < parent->children.size(); ++i) {
    if (parent->children[i] == child) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

int BPlusTree::FirstKey(const Node* node) const {
  // Separator keys are derived from the first key reachable in each subtree.
  const Node* cur = node;
  while (cur != nullptr && !cur->is_leaf) {
    cur = cur->children.front();
  }

  if (cur == nullptr || cur->keys.empty()) {
    return 0;
  }
  return cur->keys.front();
}

void BPlusTree::RefreshKeys(Node* node) {
  if (node == nullptr || node->is_leaf) {
    return;
  }

  // In this representation, internal node keys[i] is the first key in
  // children[i + 1]. Rebuild them from the current child layout.
  node->keys.clear();
  for (size_t i = 1; i < node->children.size(); ++i) {
    node->keys.push_back(FirstKey(node->children[i]));
  }
}

void BPlusTree::RefreshKeysUpward(Node* node) {
  // Any structural change can invalidate separator keys all the way to root.
  Node* cur = node;
  while (cur != nullptr) {
    RefreshKeys(cur);
    cur = cur->parent;
  }
}

void BPlusTree::SplitLeaf(Node* leaf) {
  // Split the overflowing leaf into left/right halves and keep the linked list
  // of leaves intact for efficient range scans.
  const int split = static_cast<int>(leaf->keys.size() / 2);
  Node* right = new Node(true);

  right->keys.assign(leaf->keys.begin() + split, leaf->keys.end());
  right->values.assign(leaf->values.begin() + split, leaf->values.end());
  leaf->keys.erase(leaf->keys.begin() + split, leaf->keys.end());
  leaf->values.erase(leaf->values.begin() + split, leaf->values.end());

  right->next = leaf->next;
  leaf->next = right;
  right->parent = leaf->parent;

  if (leaf->parent == nullptr) {
    // Splitting the root creates a new internal root.
    Node* new_root = new Node(false);
    new_root->children.push_back(leaf);
    new_root->children.push_back(right);
    leaf->parent = new_root;
    right->parent = new_root;
    RefreshKeys(new_root);
    root_ = new_root;
    return;
  }

  Node* parent = leaf->parent;
  const int idx = FindChildIndex(parent, leaf);
  // Insert the new right sibling immediately after the original leaf.
  parent->children.insert(parent->children.begin() + idx + 1, right);
  RefreshKeys(parent);

  if (static_cast<int>(parent->children.size()) > degree_) {
    SplitInternal(parent);
  } else {
    RefreshKeysUpward(parent);
  }
}

void BPlusTree::SplitInternal(Node* node) {
  // Internal split moves roughly half of the children into a new right node.
  // Separator keys are rebuilt afterward from child first-keys.
  const int total_children = static_cast<int>(node->children.size());
  const int left_children = (total_children + 1) / 2;

  Node* right = new Node(false);
  right->children.assign(node->children.begin() + left_children,
                         node->children.end());
  node->children.erase(node->children.begin() + left_children,
                       node->children.end());

  for (Node* child : right->children) {
    child->parent = right;
  }

  right->parent = node->parent;
  RefreshKeys(node);
  RefreshKeys(right);

  if (node->parent == nullptr) {
    // Splitting the old root increases the tree height by one.
    Node* new_root = new Node(false);
    new_root->children.push_back(node);
    new_root->children.push_back(right);
    node->parent = new_root;
    right->parent = new_root;
    RefreshKeys(new_root);
    root_ = new_root;
    return;
  }

  Node* parent = node->parent;
  const int idx = FindChildIndex(parent, node);
  parent->children.insert(parent->children.begin() + idx + 1, right);
  RefreshKeys(parent);

  if (static_cast<int>(parent->children.size()) > degree_) {
    SplitInternal(parent);
  } else {
    RefreshKeysUpward(parent);
  }
}

// Insert updates an existing key in place or inserts a new key into the target
// leaf. Overflow is handled by splitting and propagating upward as needed.
void BPlusTree::Put(int key, const std::string& value) {
  // code
  if (root_ == nullptr) {
    // First insert creates a single leaf root.
    root_ = new Node(true);
    root_->keys.push_back(key);
    root_->values.push_back(value);
    return;
  }

  Node* leaf = FindLeaf(key);
  const auto it = std::lower_bound(leaf->keys.begin(), leaf->keys.end(), key);
  const int idx = static_cast<int>(it - leaf->keys.begin());

  if (it != leaf->keys.end() && *it == key) {
    // B+Tree variant in this lab uses in-place update for the same key.
    leaf->values[idx] = value;
    RefreshKeysUpward(leaf->parent);
    return;
  }

  leaf->keys.insert(it, key);
  leaf->values.insert(leaf->values.begin() + idx, value);

  if (static_cast<int>(leaf->keys.size()) >= degree_) {
    SplitLeaf(leaf);
  } else {
    RefreshKeysUpward(leaf->parent);
  }
}

// Lookup follows separator keys down to one leaf, then binary-searches there.
bool BPlusTree::Get(int key, std::string* value) const {
  // code
  Node* leaf = FindLeaf(key);
  if (leaf == nullptr) {
    return false;
  }

  const auto it = std::lower_bound(leaf->keys.begin(), leaf->keys.end(), key);
  if (it == leaf->keys.end() || *it != key) {
    return false;
  }

  if (value != nullptr) {
    *value = leaf->values[static_cast<int>(it - leaf->keys.begin())];
  }
  return true;
}

// Range scan starts from the leaf containing start_key and then walks through
// the linked list of leaves until end_key is exceeded.
std::vector<std::pair<int, std::string>> BPlusTree::RangeScan(int start_key,
                                                              int end_key) const {
  std::vector<std::pair<int, std::string>> out;
  // code
  if (root_ == nullptr || start_key > end_key) {
    return out;
  }

  Node* leaf = FindLeaf(start_key);
  if (leaf == nullptr) {
    return out;
  }

  int idx = static_cast<int>(
      std::lower_bound(leaf->keys.begin(), leaf->keys.end(), start_key) -
      leaf->keys.begin());

  while (leaf != nullptr) {
    while (idx < static_cast<int>(leaf->keys.size())) {
      if (leaf->keys[idx] > end_key) {
        return out;
      }
      out.push_back(std::make_pair(leaf->keys[idx], leaf->values[idx]));
      ++idx;
    }
    leaf = leaf->next;
    idx = 0;
  }

  return out;
}

void BPlusTree::RebalanceLeaf(Node* leaf) {
  // When a leaf underflows after deletion, try borrow from siblings first.
  // If borrowing is impossible, merge with a sibling and continue fixing the
  // parent because its child count has decreased.
  Node* parent = leaf->parent;
  const int idx = FindChildIndex(parent, leaf);
  Node* left = (idx > 0) ? parent->children[idx - 1] : nullptr;
  Node* right =
      (idx + 1 < static_cast<int>(parent->children.size())) ? parent->children[idx + 1]
                                                            : nullptr;
  const int min_keys = (degree_ - 1) / 2;

  if (left != nullptr && static_cast<int>(left->keys.size()) > min_keys) {
    // Borrow the largest key from the left sibling.
    leaf->keys.insert(leaf->keys.begin(), left->keys.back());
    leaf->values.insert(leaf->values.begin(), left->values.back());
    left->keys.pop_back();
    left->values.pop_back();
    RefreshKeysUpward(parent);
    return;
  }

  if (right != nullptr && static_cast<int>(right->keys.size()) > min_keys) {
    // Borrow the smallest key from the right sibling.
    leaf->keys.push_back(right->keys.front());
    leaf->values.push_back(right->values.front());
    right->keys.erase(right->keys.begin());
    right->values.erase(right->values.begin());
    RefreshKeysUpward(parent);
    return;
  }

  if (left != nullptr) {
    // Merge current leaf into the left sibling.
    left->keys.insert(left->keys.end(), leaf->keys.begin(), leaf->keys.end());
    left->values.insert(left->values.end(), leaf->values.begin(),
                        leaf->values.end());
    left->next = leaf->next;
    parent->children.erase(parent->children.begin() + idx);
    delete leaf;
    RefreshKeys(parent);
    RebalanceInternal(parent);
    return;
  }

  if (right != nullptr) {
    // If no left sibling exists, merge the right sibling into the current leaf.
    leaf->keys.insert(leaf->keys.end(), right->keys.begin(), right->keys.end());
    leaf->values.insert(leaf->values.end(), right->values.begin(),
                        right->values.end());
    leaf->next = right->next;
    parent->children.erase(parent->children.begin() + idx + 1);
    delete right;
    RefreshKeys(parent);
    RebalanceInternal(parent);
  }
}

void BPlusTree::RebalanceInternal(Node* node) {
  if (node == root_) {
    // If the root ends up with a single child, collapse the tree height.
    if (!node->is_leaf && node->children.size() == 1) {
      root_ = node->children[0];
      root_->parent = nullptr;
      delete node;
    } else if (!node->is_leaf && node->children.empty()) {
      delete node;
      root_ = nullptr;
    } else {
      RefreshKeys(node);
    }
    return;
  }

  const int min_children = (degree_ + 1) / 2;
  if (static_cast<int>(node->children.size()) >= min_children) {
    RefreshKeysUpward(node);
    return;
  }

  Node* parent = node->parent;
  const int idx = FindChildIndex(parent, node);
  Node* left = (idx > 0) ? parent->children[idx - 1] : nullptr;
  Node* right =
      (idx + 1 < static_cast<int>(parent->children.size())) ? parent->children[idx + 1]
                                                            : nullptr;

  // Same strategy as leaf rebalancing: borrow first, otherwise merge.
  if (left != nullptr && static_cast<int>(left->children.size()) > min_children) {
    node->children.insert(node->children.begin(), left->children.back());
    left->children.back()->parent = node;
    left->children.pop_back();
    RefreshKeys(left);
    RefreshKeys(node);
    RefreshKeysUpward(parent);
    return;
  }

  if (right != nullptr &&
      static_cast<int>(right->children.size()) > min_children) {
    node->children.push_back(right->children.front());
    right->children.front()->parent = node;
    right->children.erase(right->children.begin());
    RefreshKeys(node);
    RefreshKeys(right);
    RefreshKeysUpward(parent);
    return;
  }

  if (left != nullptr) {
    for (Node* child : node->children) {
      child->parent = left;
      left->children.push_back(child);
    }
    parent->children.erase(parent->children.begin() + idx);
    RefreshKeys(left);
    delete node;
    RefreshKeys(parent);
    RebalanceInternal(parent);
    return;
  }

  if (right != nullptr) {
    for (Node* child : right->children) {
      child->parent = node;
      node->children.push_back(child);
    }
    parent->children.erase(parent->children.begin() + idx + 1);
    RefreshKeys(node);
    delete right;
    RefreshKeys(parent);
    RebalanceInternal(parent);
  }
}

// Delete removes the key from its leaf directly because this B+Tree uses
// in-place updates, not tombstones. Underflow is repaired by rebalancing.
bool BPlusTree::Delete(int key) {
  // code
  Node* leaf = FindLeaf(key);
  if (leaf == nullptr) {
    return false;
  }

  const auto it = std::lower_bound(leaf->keys.begin(), leaf->keys.end(), key);
  if (it == leaf->keys.end() || *it != key) {
    return false;
  }

  const int idx = static_cast<int>(it - leaf->keys.begin());
  leaf->keys.erase(leaf->keys.begin() + idx);
  leaf->values.erase(leaf->values.begin() + idx);

  if (leaf == root_) {
    // A single root leaf can simply become an empty tree.
    if (leaf->keys.empty()) {
      delete root_;
      root_ = nullptr;
    }
    return true;
  }

  const int min_keys = (degree_ - 1) / 2;
  if (static_cast<int>(leaf->keys.size()) >= min_keys) {
    RefreshKeysUpward(leaf->parent);
    return true;
  }

  RebalanceLeaf(leaf);
  return true;
}
