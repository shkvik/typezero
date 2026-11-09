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
  static void defineField(Flow &flow, const FieldPath &path);
  void fieldsAssigned(llvm::StringRef name);
  static void forgetFieldsOf(Flow &flow, const VarSymbol *var);
  Expr *narrowedField(MemberExpr *member);

private:
  static bool holdsField(const MemberExpr *member);

  SemanticState &state;
  TypeRelations *typeRelations = nullptr;
};

}
