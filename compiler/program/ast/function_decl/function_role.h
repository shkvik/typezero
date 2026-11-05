#pragma once

#include "compiler/program/ast/access/access.h"
#include "compiler/program/ast/decorator/decorator.h"
#include "compiler/union/union/union.h"
#include <vector>

namespace tz {

struct ClassDecl;
struct FunctionDecl;
struct VarSymbol;

enum class MethodKind {
  INSTANCE_METHOD,
  STATIC_METHOD,
  CONSTRUCTOR,
  ABSTRACT_METHOD,
};

struct Overriding {
  FunctionDecl *overrides = nullptr;
  bool overridden = false;
  int slot = -1;
  bool openToLate = false;
};

struct InstanceMethod {
  static constexpr MethodKind kind = MethodKind::INSTANCE_METHOD;
  VarSymbol *thisVar = nullptr;
  Overriding overriding;
};

struct StaticMethod {
  static constexpr MethodKind kind = MethodKind::STATIC_METHOD;
};

struct Constructor {
  static constexpr MethodKind kind = MethodKind::CONSTRUCTOR;
  VarSymbol *thisVar = nullptr;
};

struct AbstractMethod {
  static constexpr MethodKind kind = MethodKind::ABSTRACT_METHOD;
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
  Union<InstanceMethod, StaticMethod, Constructor, AbstractMethod> method = InstanceMethod{};
};

struct PlainFunction {};

}
