#pragma once

#include "compiler/program/ast/access/access.h"
#include "compiler/program/ast/decorator/decorator.h"
#include "compiler/union/union/union.h"
#include <vector>

namespace tz {

struct ClassDecl;
struct FunctionDecl;
struct VarSymbol;

struct Overriding {
  FunctionDecl *overrides = nullptr;
  bool overridden = false;
  int slot = -1;
  bool openToLate = false;
};

struct InstanceMethod {
  VarSymbol *thisVar = nullptr;
  Overriding overriding;
};

struct StaticMethod {};

struct Constructor {
  VarSymbol *thisVar = nullptr;
};

struct AbstractMethod {
  VarSymbol *thisVar = nullptr;
  Overriding overriding;
};

struct ClassMember {
  ClassDecl *cls = nullptr;
  Access access = Access::Public;
  bool isOverride = false;
  unsigned variant = 0;
  std::vector<Decorator> decorators;
  VarSymbol *impl = nullptr;
  Union<InstanceMethod, StaticMethod, Constructor, AbstractMethod> kind = InstanceMethod{};
};

struct PlainFunction {};

}
