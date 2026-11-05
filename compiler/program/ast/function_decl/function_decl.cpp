#include "compiler/program/ast/function_decl/function_decl.h"

namespace tz {

ClassMember *FunctionDecl::member() {
  return role.as<ClassMember>();
}

const ClassMember *FunctionDecl::member() const {
  return role.as<ClassMember>();
}

Overriding *FunctionDecl::overriding() {
  ClassMember *classMember = member();
  if (!classMember) {
    return nullptr;
  }

  return classMember->method.match(
      [](InstanceMethod &method) -> Overriding * { return &method.overriding; },
      [](StaticMethod &) -> Overriding * { return nullptr; },
      [](Constructor &) -> Overriding * { return nullptr; },
      [](AbstractMethod &method) -> Overriding * { return &method.overriding; });
}

const Overriding *FunctionDecl::overriding() const {
  const ClassMember *classMember = member();
  if (!classMember) {
    return nullptr;
  }

  return classMember->method.match(
      [](const InstanceMethod &method) -> const Overriding * { return &method.overriding; },
      [](const StaticMethod &) -> const Overriding * { return nullptr; },
      [](const Constructor &) -> const Overriding * { return nullptr; },
      [](const AbstractMethod &method) -> const Overriding * { return &method.overriding; });
}

ClassDecl *FunctionDecl::cls() const {
  const ClassMember *classMember = member();

  return classMember ? classMember->cls : nullptr;
}

bool FunctionDecl::isStatic() const {
  const ClassMember *classMember = member();

  return classMember && classMember->method.is<StaticMethod>();
}

bool FunctionDecl::isConstructor() const {
  const ClassMember *classMember = member();

  return classMember && classMember->method.is<Constructor>();
}

bool FunctionDecl::isAbstract() const {
  const ClassMember *classMember = member();

  return classMember && classMember->method.is<AbstractMethod>();
}

Access FunctionDecl::access() const {
  const ClassMember *classMember = member();

  return classMember ? classMember->access : Access::Public;
}

VarSymbol *FunctionDecl::thisVar() const {
  const ClassMember *classMember = member();
  if (!classMember) {
    return nullptr;
  }

  return classMember->method.match(
      [](const InstanceMethod &method) { return method.thisVar; },
      [](const StaticMethod &) -> VarSymbol * { return nullptr; },
      [](const Constructor &constructor) { return constructor.thisVar; },
      [](const AbstractMethod &method) { return method.thisVar; });
}

unsigned FunctionDecl::variant() const {
  const ClassMember *classMember = member();

  return classMember ? classMember->variant : 0;
}

bool FunctionDecl::isOverride() const {
  const ClassMember *classMember = member();

  return classMember && classMember->isOverride;
}

FunctionDecl *FunctionDecl::overrides() const {
  const Overriding *found = overriding();

  return found ? found->overrides : nullptr;
}

bool FunctionDecl::overridden() const {
  const Overriding *found = overriding();

  return found && found->overridden;
}

int FunctionDecl::slot() const {
  const Overriding *found = overriding();

  return found ? found->slot : -1;
}

bool FunctionDecl::openToLate() const {
  const Overriding *found = overriding();

  return found && found->openToLate;
}

const std::vector<Decorator> &FunctionDecl::decorators() const {
  static const std::vector<Decorator> none;
  const ClassMember *classMember = member();

  return classMember ? classMember->decorators : none;
}

VarSymbol *FunctionDecl::impl() const {
  const ClassMember *classMember = member();

  return classMember ? classMember->impl : nullptr;
}

bool FunctionDecl::takesThis() const {
  return cls() && !isStatic();
}

bool FunctionDecl::hasThisParam() const {
  return sym && sym->hasThis && !params.empty() && params[0].sym;
}

const FunctionDecl *FunctionDecl::thisSource() const {
  const FunctionDecl *function = this;
  while (function && function->isArrow && !function->takesThis()) {
    function = function->parent;
  }

  return function;
}

}
