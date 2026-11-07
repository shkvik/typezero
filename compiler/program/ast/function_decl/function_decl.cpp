#include "compiler/program/ast/function_decl/function_decl.h"

namespace tz {

bool FunctionDecl::isArrow() const {
  return this->syntax.is<ArrowFunction>();
}

bool FunctionDecl::concise() const {
  const ArrowFunction *arrow = this->syntax.as<ArrowFunction>();

  return arrow && arrow->concise;
}

llvm::StringRef FunctionDecl::ownName() const {
  const RegularFunction *regular = this->syntax.as<RegularFunction>();

  return regular ? regular->ownName : llvm::StringRef();
}

bool FunctionDecl::isExpression() const {
  return this->place == FunctionPlace::EXPRESSION;
}

bool FunctionDecl::lifted() const {
  return this->place == FunctionPlace::LIFTED;
}

bool FunctionDecl::external() const {
  return this->bodySource != FunctionBodySource::WRITTEN;
}

bool FunctionDecl::intrinsic() const {
  return this->bodySource == FunctionBodySource::INTRINSIC;
}

ClassMember *FunctionDecl::member() {
  return this->role.as<ClassMember>();
}

const ClassMember *FunctionDecl::member() const {
  return this->role.as<ClassMember>();
}

Overriding *FunctionDecl::overriding() {
  ClassMember *classMember = this->member();
  if (!classMember) {
    return nullptr;
  }

  Overriding *found = nullptr;
  switch (classMember->method.kind()) {
    case MethodKind::INSTANCE_METHOD:
      found = &classMember->method.as<InstanceMethod>()->overriding;
      break;
    case MethodKind::STATIC_METHOD:
      break;
    case MethodKind::CONSTRUCTOR:
      break;
    case MethodKind::ABSTRACT_METHOD:
      found = &classMember->method.as<AbstractMethod>()->overriding;
      break;
  }

  return found;
}

const Overriding *FunctionDecl::overriding() const {
  const ClassMember *classMember = this->member();
  if (!classMember) {
    return nullptr;
  }

  const Overriding *found = nullptr;
  switch (classMember->method.kind()) {
    case MethodKind::INSTANCE_METHOD:
      found = &classMember->method.as<InstanceMethod>()->overriding;
      break;
    case MethodKind::STATIC_METHOD:
      break;
    case MethodKind::CONSTRUCTOR:
      break;
    case MethodKind::ABSTRACT_METHOD:
      found = &classMember->method.as<AbstractMethod>()->overriding;
      break;
  }

  return found;
}

ClassDecl *FunctionDecl::cls() const {
  const ClassMember *classMember = this->member();

  return classMember ? classMember->cls : nullptr;
}

bool FunctionDecl::isStatic() const {
  const ClassMember *classMember = this->member();

  return classMember && classMember->method.is<StaticMethod>();
}

bool FunctionDecl::isConstructor() const {
  const ClassMember *classMember = this->member();

  return classMember && classMember->method.is<Constructor>();
}

bool FunctionDecl::isAbstract() const {
  const ClassMember *classMember = this->member();

  return classMember && classMember->method.is<AbstractMethod>();
}

Access FunctionDecl::access() const {
  const ClassMember *classMember = this->member();

  return classMember ? classMember->access : Access::Public;
}

VarSymbol *FunctionDecl::thisVar() const {
  const ClassMember *classMember = this->member();
  if (!classMember) {
    return nullptr;
  }

  VarSymbol *found = nullptr;
  switch (classMember->method.kind()) {
    case MethodKind::INSTANCE_METHOD:
      found = classMember->method.as<InstanceMethod>()->thisVar;
      break;
    case MethodKind::STATIC_METHOD:
      break;
    case MethodKind::CONSTRUCTOR:
      found = classMember->method.as<Constructor>()->thisVar;
      break;
    case MethodKind::ABSTRACT_METHOD:
      found = classMember->method.as<AbstractMethod>()->thisVar;
      break;
  }

  return found;
}

unsigned FunctionDecl::variant() const {
  const ClassMember *classMember = this->member();

  return classMember ? classMember->variant : 0;
}

bool FunctionDecl::isOverride() const {
  const ClassMember *classMember = this->member();

  return classMember && classMember->isOverride;
}

FunctionDecl *FunctionDecl::overrides() const {
  const Overriding *found = this->overriding();

  return found ? found->overrides : nullptr;
}

bool FunctionDecl::overridden() const {
  const Overriding *found = this->overriding();

  return found && found->overridden;
}

int FunctionDecl::slot() const {
  const Overriding *found = this->overriding();

  return found ? found->slot : -1;
}

bool FunctionDecl::openToLate() const {
  const Overriding *found = this->overriding();

  return found && found->openToLate;
}

const std::vector<Decorator> &FunctionDecl::decorators() const {
  static const std::vector<Decorator> none;
  const ClassMember *classMember = this->member();

  return classMember ? classMember->decorators : none;
}

VarSymbol *FunctionDecl::impl() const {
  const ClassMember *classMember = this->member();

  return classMember ? classMember->impl : nullptr;
}

bool FunctionDecl::takesThis() const {
  return this->cls() && !this->isStatic();
}

bool FunctionDecl::hasThisParam() const {
  return this->sym && this->sym->hasThis && !this->params.empty() && this->params[0].sym;
}

const FunctionDecl *FunctionDecl::thisSource() const {
  const FunctionDecl *function = this;
  while (function && function->isArrow() && !function->takesThis()) {
    function = function->parent;
  }

  return function;
}

}
