#include "compiler/semantic/types/type_parts/type_parts.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

TypeParts::TypeParts(llvm::ArrayRef<TypeEdge> edges)
    : route(edges) {}

TypeParts TypeParts::collect(const Type *type, llvm::ArrayRef<TypeEdge> edges) {
  auto parts = TypeParts(edges);
  auto stack = std::vector<const Type *>{type};
  while (!stack.empty()) {
    const Type *next = stack.back();
    stack.pop_back();
    parts.addPart(next, stack);
  }

  return parts;
}

llvm::ArrayRef<const Type *> TypeParts::getTypes() const {
  return this->types;
}

bool TypeParts::isFollowing(TypeEdge edge) const {
  return llvm::is_contained(this->route, edge);
}

void TypeParts::addPart(const Type *type, std::vector<const Type *> &stack) {
  if (type == nullptr) {
    return;
  }
  auto [_, isFirstVisit] = this->seen.insert(type);
  if (!isFirstVisit) {
    return;
  }
  this->types.push_back(type);
  auto children = this->childrenOf(type);
  for (const Type *child : llvm::reverse(children)) {
    stack.push_back(child);
  }
}

std::vector<const Type *> TypeParts::childrenOf(const Type *type) const {
  auto children = std::vector<const Type *>();
  this->addWrapperParts(children, type);
  this->addResultParts(children, type);
  this->addFunctionParts(children, type);
  this->addIteratorParts(children, type);
  this->addDictParts(children, type);
  this->addUnionParts(children, type);
  this->addObjectParts(children, type);
  this->addParamParts(children, type);
  this->addClassRefParts(children, type);

  return children;
}

void TypeParts::addWrapperParts(std::vector<const Type *> &children, const Type *type) const {
  bool isOptional = type->isOptional() && this->isFollowing(TypeEdge::OPTIONAL);
  bool isArray = type->isArray() && this->isFollowing(TypeEdge::ARRAY);
  bool isPromise = type->isPromise() && this->isFollowing(TypeEdge::PROMISE);
  if (isOptional || isArray || isPromise) {
    children.push_back(type->inner());
  }
}

void TypeParts::addResultParts(std::vector<const Type *> &children, const Type *type) const {
  if (type->isResult() && this->isFollowing(TypeEdge::RESULT)) {
    children.push_back(type->okType());
    children.push_back(type->errType());
  }
}

void TypeParts::addFunctionParts(std::vector<const Type *> &children, const Type *type) const {
  if (type->isFunction() && this->isFollowing(TypeEdge::FUNCTION)) {
    for (const Type *param : type->params()) {
      children.push_back(param);
    }
    children.push_back(type->ret());
  }
}

void TypeParts::addIteratorParts(std::vector<const Type *> &children, const Type *type) const {
  if (!type->isIterator()) {
    return;
  }
  if (this->isFollowing(TypeEdge::ITERATOR)) {
    children.push_back(type->yielded());
    children.push_back(type->returned());
    children.push_back(type->sent());
  }
  if (this->isFollowing(TypeEdge::ITERATOR_VALUES)) {
    children.push_back(type->yielded());
    children.push_back(type->returned());
  }
  if (this->isFollowing(TypeEdge::YIELDED)) {
    children.push_back(type->yielded());
  }
}

void TypeParts::addDictParts(std::vector<const Type *> &children, const Type *type) const {
  if (!type->isDict()) {
    return;
  }
  if (this->isFollowing(TypeEdge::DICT)) {
    children.push_back(type->keyType());
    children.push_back(type->valueType());
  }
  if (type->isRecord() && this->isFollowing(TypeEdge::RECORD_VALUE)) {
    children.push_back(type->valueType());
  }
}

void TypeParts::addUnionParts(std::vector<const Type *> &children, const Type *type) const {
  if (type->isUnion() && this->isFollowing(TypeEdge::UNION)) {
    for (const Type *member : type->members()) {
      children.push_back(member);
    }
  }
}

void TypeParts::addObjectParts(std::vector<const Type *> &children, const Type *type) const {
  if (!type->isObject()) {
    return;
  }
  if (type->isAsyncIterator() && this->isFollowing(TypeEdge::ASYNC_ITERATOR)) {
    children.push_back(type->yielded());
    children.push_back(type->returned());
    children.push_back(type->sent());
    return;
  }
  if (type->isAsyncIterator() && this->isFollowing(TypeEdge::YIELDED)) {
    children.push_back(type->yielded());
    return;
  }
  if (!type->typeArgs().empty() && this->isFollowing(TypeEdge::TYPE_ARGS)) {
    for (const Type *arg : type->typeArgs()) {
      children.push_back(arg);
    }
    return;
  }
  if (this->isFollowing(TypeEdge::TUPLE_REST)) {
    children.push_back(type->tupleRest());
  }
  this->addObjectFields(children, type);
}

void TypeParts::addObjectFields(std::vector<const Type *> &children, const Type *type) const {
  bool isClassObject = type->classDecl() != nullptr;
  bool isClassFields = isClassObject && this->isFollowing(TypeEdge::CLASS_FIELDS);
  bool isPlainFields = !isClassObject && this->isFollowing(TypeEdge::FIELDS);
  if (isClassFields || isPlainFields) {
    for (const Field &field : type->fields()) {
      children.push_back(field.type);
    }
  }
}

void TypeParts::addParamParts(std::vector<const Type *> &children, const Type *type) const {
  if (!type->isParam()) {
    return;
  }
  if (type->typeOp() != TypeOp::None && this->isFollowing(TypeEdge::OPERANDS)) {
    for (const Type *operand : type->operands()) {
      children.push_back(operand);
    }
  }
  if (type->constraint() != nullptr && this->isFollowing(TypeEdge::CONSTRAINT)) {
    children.push_back(type->constraint());
  }
}

void TypeParts::addClassRefParts(std::vector<const Type *> &children, const Type *type) const {
  if (type->isConstructor() && this->isFollowing(TypeEdge::CONSTRUCTED)) {
    children.push_back(type->constructed());
  }
}

}
