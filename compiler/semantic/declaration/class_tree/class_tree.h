#pragma once

#include "compiler/program/ast/ast.h"
#include <vector>

namespace tz {

class ClassTree {
public:
  static std::vector<ClassDecl *> descendants(const ClassDecl *classDecl);
  static std::vector<ClassDecl *> treeOrder(const std::vector<ClassDecl *> &classes);
};

}
