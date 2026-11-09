#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/semantic_state/semantic_state.h"
#include "compiler/semantic/types/type_relations/type_relations_errors.h"

namespace tz {

class SemanticParts;
class ByteProofs;
class ObjectChecker;
class ConstantFolder;
class GenericChecker;
class ReflectChecker;
class UnknownChecker;
class IterableChecker;
class ConditionalTypes;
class TzModulesChecker;
class WideningRelations;
class FunctionValueChecker;

struct ConversionGoal {
  const Type *from;
  const Type *to;
};

struct ConversionStep {
  bool possible;
  std::vector<ConversionGoal> needs;
};

class TypeRelations {
public:
  TypeRelations(SemanticState &owner, SemanticParts &all);

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
  static ConversionStep conversionStep(const Type *from, const Type *to);
  static ConversionStep stepHolds();
  static ConversionStep stepDecided(bool possible);
  static ConversionStep stepNeeds(std::vector<ConversionGoal> goals);
  static ConversionStep jsonStep(const Type *from, const Type *to);
  static ConversionStep resultStep(const Type *from, const Type *to);
  static std::optional<bool> plainConvertible(const Type *from, const Type *to);
  using ResultRangeWasProven = Result<const Expr *, TypeRelationsRangeProvenError>;
  using ResultTypeAssignable = Result<const Expr *, TypeRelationsAssignableError>;

  static bool isIntLiteralNode(const Expr *expr);
  static bool isWideInteger(const Type *type);
  static ResultRangeWasProven rangeProven(const Expr *expr, const Type *to, llvm::StringRef what,
                                          llvm::ArrayRef<std::pair<int64_t, int64_t>> intervals);
  static std::string rangeCheck(const Expr *expr, const Type *target,
                                llvm::ArrayRef<std::pair<int64_t, int64_t>> intervals);
  static const Type *innerObject(const Type *type, const Type *other);
  ResultTypeAssignable assignable(const Expr *expr, const Type *to, llvm::StringRef what) const;
  const Type *otherPlace(const Expr *expr, const Type *to) const;
  llvm::SMLoc placeLoc(const Expr *expr, const Type *place) const;

  SemanticState &state;
  ByteProofs *byteProofs = nullptr;
  ObjectChecker *objectChecker = nullptr;
  ConstantFolder *constantFolder = nullptr;
  GenericChecker *genericChecker = nullptr;
  ReflectChecker *reflectChecker = nullptr;
  UnknownChecker *unknownChecker = nullptr;
  IterableChecker *iterableChecker = nullptr;
  ConditionalTypes *conditionalTypes = nullptr;
  TzModulesChecker *tzModulesChecker = nullptr;
  WideningRelations *wideningRelations = nullptr;
  FunctionValueChecker *functionValueChecker = nullptr;
};

}
