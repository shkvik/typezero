#include "compiler/semantic/declare/class_tree/class_tree.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

std::vector<ClassDecl *> ClassTree::descendants(const ClassDecl *classDecl) {
  auto order = std::vector<ClassDecl *>();
  std::vector<ClassDecl *> stack(classDecl->children.rbegin(), classDecl->children.rend());
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
  auto order = std::vector<ClassDecl *>();
  for (ClassDecl *classDecl : classes) {
    bool isRoot = classDecl->parent == nullptr && classDecl->literal() == nullptr;
    if (isRoot) {
      auto below = ClassTree::descendants(classDecl);
      order.push_back(classDecl);
      order.insert(order.end(), below.begin(), below.end());
    }
  }

  return order;
}

}
