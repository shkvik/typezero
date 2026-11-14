#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include "compiler/semantic/types_v2/intersections/types_intersections_registry_errors.h"
#include "compiler/union/union/union.h"

namespace tz {

class TypesIntersectionsRegistry {
public:
  using ResultIntersectionJoined =
      Result<const Type *, TypesIntersectionsRegistryResolveIntersectionError>;

  explicit TypesIntersectionsRegistry(SemanticState &owner);

  void startIntersection(const StartIntersection &task);
  void nextIntersectionPart(const NextIntersectionPart &task);
  void afterIntersectionPart(const AfterIntersectionPart &task);

private:
  using ResultObjectsAgreed = Result<void, TypesIntersectionsRegistryResolveIntersectionError>;
  using ResultObjectsMerged =
      Result<std::vector<const Type *>, TypesIntersectionsRegistryResolveIntersectionError>;

  void pushFilter(const Type *filter, const Type *other);
  void finishIntersection(const IntersectionDraft &draft);


  bool isPrimitive(const Type *type) const;
  bool isEmptyObject(const Type *type) const;
  bool isPlainObject(const Type *type) const;
  bool isFilteringBy(const Type *filter) const;


  const Type *firstNonObject(llvm::ArrayRef<const Type *> parts) const;
  const Type *commonFieldType(const Type *first, const Type *second) const;


  ResultObjectsAgreed checkAllObjects(llvm::ArrayRef<const Type *> parts) const;
  ResultObjectsAgreed joinField(std::vector<Field> &fields, const Field &field) const;


  ResultIntersectionJoined joinObjects(const Type *left, const Type *right) const;
  ResultIntersectionJoined joinedParts(const std::vector<const Type *> &parts) const;


  std::vector<const Type *> fieldCopies(const Type *part) const;
  llvm::ArrayRef<const Type *> membersOf(const Type *&type) const;
  Field *fieldNamed(std::vector<Field> &fields, llvm::StringRef name) const;
  ResultObjectsMerged mergedWith(llvm::ArrayRef<const Type *> merged, const Type *part) const;

  SemanticState &state;
};

}
