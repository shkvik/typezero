#pragma once

#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include <optional>
#include <vector>

namespace tz {

class GenericsSubstitutionRegistry {
public:
  explicit GenericsSubstitutionRegistry(SemanticState &owner);

  void instanceType(const InstanceType &task);
  void substituteNode(const SubstituteNode &task);
  void substituteType(const SubstituteType &task);
  void afterTypeListItem(const AfterTypeListItem &task);
  void instanceValueType(const InstanceValueType &task);
  void nextInstanceField(const NextInstanceField &task);
  void afterInstanceField(const AfterInstanceField &task);
  void substituteTypeList(const SubstituteTypeList &task);
  void substituteValueType(const SubstituteValueType &task);
  void buildSubstitutedType(const BuildSubstitutedType &task);
  void substituteInstanceFields(const SubstituteInstanceFields &task);

private:
  void pushFormTask(const Type *type, SubstituteForm form, std::vector<const Type *> parts);


  bool hasListParam(const Type *tuple) const;
  bool isTaskForm(SubstituteForm form) const;


  std::vector<const Type *> fieldParts(const Type *type) const;
  std::vector<const Type *> tupleParts(const Type *type) const;


  std::optional<SubstituteForm> paramForm(const Type *type) const;
  std::optional<SubstituteForm> objectForm(const Type *type) const;
  std::optional<SubstituteForm> classRefForm(const Type *type) const;
  std::optional<SubstituteForm> substituteForm(const Type *type) const;


  std::optional<size_t> findUnresolvedField(const ClassDecl *instance, size_t from) const;
  std::vector<SubstituteNode> nodeParts(const SubstituteNode &node, SubstituteForm form) const;
  std::vector<SubstituteNode> nodesOf(const SubstituteNode &node,
                                      llvm::ArrayRef<const Type *> types, VoidAs voidAs) const;


  const Type *plainSubstitute(const SubstituteNode &node) const;
  const Type *substitutedObject(const Type *type, llvm::ArrayRef<const Type *> parts);
  const Type *substitutedTuple(const Type *type, std::vector<const Type *> parts) const;
  const Type *substitutedFunction(const Type *type, std::vector<const Type *> parts) const;
  const Type *builtType(const Type *type, SubstituteForm form, std::vector<const Type *> parts);

  SemanticState &state;
};

}
