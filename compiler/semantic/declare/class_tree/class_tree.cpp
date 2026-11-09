#include "compiler/semantic/declare/class_tree/class_tree.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

std::vector<ClassDecl *> ClassTree::descendants(const ClassDecl *cls) {
  std::vector<ClassDecl *> order;
  std::vector<ClassDecl *> stack(cls->children.rbegin(), cls->children.rend());
  while (!stack.empty()) {
    ClassDecl *next = stack.back();
    stack.pop_back();
    order.push_back(next);
    for (ClassDecl *child : llvm::reverse(next->children)) {
      stack.push_back(child);
    }
  }

  return order;
}

std::vector<ClassDecl *> ClassTree::treeOrder(const std::vector<ClassDecl *> &classes) {
  std::vector<ClassDecl *> order;
  for (ClassDecl *cls : classes) {
    bool root = cls->parent == nullptr && !cls->literal();
    if (root) {
      std::vector<ClassDecl *> below = ClassTree::descendants(cls);
      order.push_back(cls);
      order.insert(order.end(), below.begin(), below.end());
    }
  }

  return order;
}

}
