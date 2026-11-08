#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/semantic_state/semantic_state.h"
#include "compiler/semantic/types/type_relations/type_relations_errors.h"

namespace tz {

class TypeRelations {
public:
  explicit TypeRelations(SemanticState &owner);

  Expr *convert(Expr *expr, const Type *to);
  Expr *coerce(Expr *expr, const Type *to, llvm::StringRef what = {});
  static bool isImplicitlyConvertible(const Type *from, const Type *to);
  static bool copiesIntoRecord(const Type *from, const Type *to);
  static bool holdsWideInteger(const Type *type);
  static const Type *commonNumericType(const Type *a, const Type *b);
  static const Type *unify(const Type *a, const Type *b);
  static const Type *comparisonType(const Type *a, const Type *b);
  static bool isIntLiteralTree(const Expr *expr);
  bool literalAdapts(const Expr *expr, const Type *to);
  const Type *literalTarget(const Expr *expr, const Type *to);
  Expr *adaptLiteral(Expr *expr, const Type *to);
  static std::string operandText(llvm::StringRef text);
  static std::string conversionHint(const Expr *expr, const Type *to);
  static std::string conversionMethod(const Type *to);

private:
  using ResultRangeWasProven = Result<const Expr *, TypeRelationsRangeProvenError>;
  using ResultTypeAssignable = Result<const Expr *, TypeRelationsAssignableError>;

  static ResultRangeWasProven rangeProven(const Expr *expr, const Type *to, llvm::StringRef what,
                                          llvm::ArrayRef<std::pair<int64_t, int64_t>> intervals);
  static std::string rangeCheck(const Expr *expr, const Type *target,
                                llvm::ArrayRef<std::pair<int64_t, int64_t>> intervals);
  static const Type *innerObject(const Type *type, const Type *other);
  ResultTypeAssignable assignable(const Expr *expr, const Type *to, llvm::StringRef what) const;
  const Type *otherPlace(const Expr *expr, const Type *to) const;
  llvm::SMLoc placeLoc(const Expr *expr, const Type *place) const;

  SemanticState &state;
};

}
