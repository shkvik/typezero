#pragma once

#include "compiler/program/types/type/type.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallPtrSet.h"
#include <vector>

namespace tz {

enum class TypeEdge {
  OPTIONAL,
  ARRAY,
  PROMISE,
  RESULT,
  FUNCTION,
  ITERATOR,
  ITERATOR_VALUES,
  YIELDED,
  ASYNC_ITERATOR,
  DICT,
  RECORD_VALUE,
  UNION,
  FIELDS,
  CLASS_FIELDS,
  TUPLE_REST,
  TYPE_ARGS,
  OPERANDS,
  CONSTRUCTED,
  CONSTRAINT,
};

class TypeParts {
public:
  static TypeParts collect(const Type *type, llvm::ArrayRef<TypeEdge> edges);

  llvm::ArrayRef<const Type *> getTypes() const;

private:
  explicit TypeParts(llvm::ArrayRef<TypeEdge> edges);

  bool isFollowing(TypeEdge edge) const;
  void addPart(const Type *type, std::vector<const Type *> &stack);
  std::vector<const Type *> childrenOf(const Type *type) const;
  void addDictParts(std::vector<const Type *> &children, const Type *type) const;
  void addParamParts(std::vector<const Type *> &children, const Type *type) const;
  void addUnionParts(std::vector<const Type *> &children, const Type *type) const;
  void addResultParts(std::vector<const Type *> &children, const Type *type) const;
  void addObjectParts(std::vector<const Type *> &children, const Type *type) const;
  void addWrapperParts(std::vector<const Type *> &children, const Type *type) const;
  void addIteratorParts(std::vector<const Type *> &children, const Type *type) const;
  void addFunctionParts(std::vector<const Type *> &children, const Type *type) const;
  void addClassRefParts(std::vector<const Type *> &children, const Type *type) const;
  void addObjectFields(std::vector<const Type *> &children, const Type *type) const;

  llvm::ArrayRef<TypeEdge> route;
  std::vector<const Type *> types;
  llvm::SmallPtrSet<const Type *, 16> seen;
};

}
