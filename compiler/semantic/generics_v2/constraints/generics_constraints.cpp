#include "compiler/semantic/generics_v2/constraints/generics_constraints.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include "compiler/semantic/types/type_parts/type_parts.h"
#include "compiler/semantic/types/type_parts/type_routes.h"

namespace tz {

GenericsConstraints::GenericsConstraints(SemanticState &owner)
    : state(owner) {}

bool GenericsConstraints::isSatisfying(const ConstraintCheck &check) const {
  auto types = MethodTypes{.known = check.known};

  return this->evaluate(check, types);
}

FuncSymbol *GenericsConstraints::findUnknownMethod(const ConstraintCheck &check) const {
  auto types = MethodTypes{.known = check.known};
  this->evaluate(check, types);

  return types.missing;
}

bool GenericsConstraints::evaluate(const ConstraintCheck &check, MethodTypes &types) const {
  const Type *arg = check.arg;
  const Type *own = check.own;
  if (arg->isError()) {
    return true;
  }
  if (own != nullptr && own->isError()) {
    return true;
  }
  if (arg->isParam() && arg->constraint() != nullptr && own != nullptr &&
      this->state.types.relations.isImplicitlyConvertible(arg->constraint(), own)) {
    return true;
  }
  if (own != nullptr) {
    return this->isFittingConstraint(arg, own, types);
  }
  llvm::ArrayRef<const Type *> alternatives = check.param->alternatives();
  if (alternatives.empty()) {
    return true;
  }
  for (const Type *alternative : alternatives) {
    if (this->isFittingConstraint(arg, alternative, types)) {
      return true;
    }
  }

  return false;
}

bool GenericsConstraints::isFittingConstraint(const Type *arg, const Type *constraint,
                                              MethodTypes &types) const {
  if (this->state.types.relations.isImplicitlyConvertible(arg, constraint)) {
    return true;
  }

  return constraint->isObject() && this->hasMembersOf(arg, constraint, types);
}

bool GenericsConstraints::hasMembersOf(const Type *arg, const Type *constraint,
                                       MethodTypes &types) const {
  auto parts = TypeParts::collect(arg, kMembersOfRoute);
  for (const Type *part : parts.getTypes()) {
    bool isConstrained = part->isParam() && part->constraint() != nullptr;
    bool isLeaf = !isConstrained && !part->isUnion();
    if (isLeaf && !this->hasObjectMembersOf(part, constraint, types)) {
      return false;
    }
  }

  return true;
}

bool GenericsConstraints::hasObjectMembersOf(const Type *arg, const Type *constraint,
                                             MethodTypes &types) const {
  bool isComparable = arg->isObject() && arg->isTuple() == constraint->isTuple() &&
                      !constraint->isClass() && !arg->isAnyObject();
  if (!isComparable) {
    return false;
  }
  const ClassDecl *classDecl = arg->classDecl();
  for (const Field &field : constraint->fields()) {
    if (!this->hasMatchingField(arg, classDecl, field, types)) {
      return false;
    }
  }
  for (const Overload &overload : constraint->overloads()) {
    if (!this->hasMatchingOverload(classDecl, overload, types)) {
      return false;
    }
  }

  return true;
}

bool GenericsConstraints::isTextUnion(const Type *type) const {
  if (!type->isUnion()) {
    return false;
  }
  for (const Type *member : type->members()) {
    if (!member->isStringLiteral()) {
      return false;
    }
  }

  return true;
}

bool GenericsConstraints::isMatchingField(const Field &have, const Field &wanted) const {
  bool isText = have.type->isStringLiteral() || this->isTextUnion(have.type);
  bool isLiteral = wanted.type->isString() && isText;
  if (have.type != wanted.type && !isLiteral) {
    return false;
  }

  return !have.readonly || wanted.readonly;
}

bool GenericsConstraints::hasMatchingField(const Type *arg, const ClassDecl *classDecl,
                                           const Field &field, MethodTypes &types) const {
  const auto *have = arg->field(field.name);
  if (have != nullptr) {
    return this->isMatchingField(*have, field);
  }
  if (this->hasMethodField(classDecl, field, types)) {
    return true;
  }

  return field.type->isOptional();
}

bool GenericsConstraints::hasMethodField(const ClassDecl *classDecl, const Field &field,
                                         MethodTypes &types) const {
  if (classDecl == nullptr || !field.readonly || !field.type->isFunction()) {
    return false;
  }
  bool isMethod = false;
  for (FunctionDecl *method : classDecl->findMethodsNamed(field.name)) {
    bool isPlain = method->sym != nullptr && method->typeParams.empty();
    if (isPlain && this->methodType(method->sym, types) == field.type) {
      isMethod = true;
    }
  }

  return isMethod;
}

bool GenericsConstraints::hasMatchingOverload(const ClassDecl *classDecl, const Overload &overload,
                                              MethodTypes &types) const {
  if (classDecl == nullptr) {
    return false;
  }
  auto methods = classDecl->findMethodsNamed(overload.name);
  for (const Type *signature : overload.signatures) {
    if (!this->hasMethodOfType(methods, signature, types)) {
      return false;
    }
  }

  return true;
}

bool GenericsConstraints::hasMethodOfType(llvm::ArrayRef<FunctionDecl *> methods,
                                          const Type *signature, MethodTypes &types) const {
  for (FunctionDecl *method : methods) {
    if (method->sym != nullptr && this->methodType(method->sym, types) == signature) {
      return true;
    }
  }

  return false;
}

const Type *GenericsConstraints::methodType(FuncSymbol *function, MethodTypes &types) const {
  for (const auto &[known, type] : types.known) {
    if (known == function) {
      return type;
    }
  }
  if (function->hasReturnType()) {
    const Type *returned = function->getKnownReturnType();

    return Type::function(function->params, function->minArgs, function->hasRest, returned,
                          function->hasThis);
  }
  if (types.missing == nullptr) {
    types.missing = function;
  }

  return Type::error();
}

}
