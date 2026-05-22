#ifndef LAB2_BPTREE_H
#define LAB2_BPTREE_H

#include <string>
#include <utility>
#include <vector>

// 필요시 내부 function, 변수 등 선언 가능
class BPlusTree {
 public:
  explicit BPlusTree(int degree = 4);
  ~BPlusTree();

  BPlusTree(const BPlusTree&) = delete;
  BPlusTree& operator=(const BPlusTree&) = delete;

  void Put(int key, const std::string& value);
  bool Get(int key, std::string* value) const;
  bool Delete(int key);
  std::vector<std::pair<int, std::string>> RangeScan(int start_key,
                                                     int end_key) const;

 private:
  struct Node;
  struct Node {
    bool is_leaf;
    std::vector<int> keys;
    std::vector<Node*> children;
    std::vector<std::string> values;
    Node* parent;
    Node* next;

    explicit Node(bool leaf)
        : is_leaf(leaf), parent(nullptr), next(nullptr) {}
  };

  void Destroy(Node* node);
  Node* FindLeaf(int key) const;
  int FindChildIndex(const Node* parent, const Node* child) const;
  int FirstKey(const Node* node) const;
  void RefreshKeys(Node* node);
  void RefreshKeysUpward(Node* node);

  void SplitLeaf(Node* leaf);
  void SplitInternal(Node* node);

  void RebalanceLeaf(Node* leaf);
  void RebalanceInternal(Node* node);

  Node* root_;
  int degree_;
};

#endif  // LAB2_BPTREE_H
