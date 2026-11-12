#pragma once

#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class TypeRelations;

class FieldFacts {
public:
  FieldFacts(SemanticState &owner, SemanticParts &all);

  std::optional<FieldPath> fieldPath(const Expr *expr) const;
  bool isFieldDefined(const FieldPath &path) const;
  void defineField(Flow &flow, const FieldPath &path) const;
  void fieldsAssigned(llvm::StringRef name);
  void forgetFieldsOf(Flow &flow, const VarSymbol *variable) const;
  Expr *narrowedField(MemberExpr *member);

private:
  bool isHoldingField(const MemberExpr *member) const;
  bool isPathOf(const VarSymbol *variable, const FieldPath &path) const;
  bool isNarrowedPathOf(const VarSymbol *variable,
                        const std::pair<FieldPath, const Type *> &entry) const;
  const Type *streamView(const MemberExpr *member) const;

  SemanticState &state;
  TypeRelations *typeRelations = nullptr;
};

}
