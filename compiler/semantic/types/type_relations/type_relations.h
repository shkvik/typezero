#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/semantic_state/semantic_state.h"
#include "compiler/semantic/types/type_relations/type_relations_errors.h"
#include "compiler/union/union/union.h"

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

enum class ConversionTaskKind { GOAL, MEMBER, CHOICE };

struct ConversionGoal {
  static constexpr ConversionTaskKind kind = ConversionTaskKind::GOAL;
  const Type *from;
  const Type *to;
};

struct MemberGoal {
  static constexpr ConversionTaskKind kind = ConversionTaskKind::MEMBER;
  const Type *from;
  const Type *to;
};

struct MemberChoice {
  static constexpr ConversionTaskKind kind = ConversionTaskKind::CHOICE;
  const Type *from;
  const Type *to;
  size_t next;
};

using ConversionTask = Union<ConversionGoal, MemberGoal, MemberChoice>;

struct ConversionStep {
  bool possible;
  std::vector<ConversionTask> needs;
};

enum class UnifyForm { WHOLE, HALF, UNION };
enum class UnifyTaskKind { ENTER, COLLECT };

struct EnterUnify {
  static constexpr UnifyTaskKind kind = UnifyTaskKind::ENTER;
  const Type *a;
  const Type *b;
  UnifyForm form;
};

struct CollectUnify {
  static constexpr UnifyTaskKind kind = UnifyTaskKind::COLLECT;
  const Type *a;
  const Type *b;
  UnifyForm form;
};

using UnifyTask = Union<EnterUnify, CollectUnify>;

struct UnifyShortcut {
  bool decided;
  const Type *type;
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
  static const Type *unifyAs(UnifyForm form, const Type *a, const Type *b);
  static const Type *comparisonType(const Type *a, const Type *b);
  static bool isIntLiteralTree(const Expr *expr);
  bool literalAdapts(const Expr *expr, const Type *to);
  const Type *literalTarget(const Expr *expr, const Type *to);
  Expr *adaptLiteral(Expr *expr, const Type *to);
  static std::string operandText(llvm::StringRef text);
  static std::string conversionHint(const Expr *expr, const Type *to);
  static std::string conversionMethod(const Type *to);

private:
  static ConversionStep taskStep(const ConversionTask &task);
  static ConversionStep conversionStep(const Type *from, const Type *to);
  static std::optional<ConversionStep> targetStep(const Type *from, const Type *to);
  static ConversionStep anyObjectStep(const Type *from, const Type *to);
  static ConversionStep classRefStep(const Type *from, const Type *to);
  static std::optional<ConversionStep> sourceStep(const Type *from, const Type *to);
  static ConversionStep unionSourceStep(const Type *from, const Type *to);
  static ConversionStep stepHolds();
  static ConversionStep stepDecided(bool possible);
  static ConversionStep stepNeeds(std::vector<ConversionGoal> goals);
  static ConversionStep jsonStep(const Type *from, const Type *to);
  static ConversionStep resultStep(const Type *from, const Type *to);
  static ConversionStep memberGoals(const Type *from, const Type *to);
  static ConversionStep memberStep(const MemberGoal &goal);
  static ConversionStep memberTrial(MemberChoice choice);
  static bool retryMember(std::vector<ConversionTask> &tasks);
  static bool recordCopyShape(const Type *from, const Type *to);
  static const Type *copiedFieldType(const Field &field, const Type *value);
  static ConversionStep recordCopyStep(const Type *from, const Type *to);
  static std::optional<bool> plainConvertible(const Type *from, const Type *to);
  static void enterUnify(const EnterUnify &enter, std::vector<UnifyTask> &tasks,
                         std::vector<const Type *> &answers);
  static void enterWholeUnify(const Type *a, const Type *b, std::vector<UnifyTask> &tasks,
                              std::vector<const Type *> &answers);
  static void collectUnify(const CollectUnify &collect, std::vector<const Type *> &answers);
  static const Type *optionalLike(const Type *a, const Type *b, const Type *base);
  static UnifyShortcut shortUnify(const Type *a, const Type *b);
  static const Type *baseUnify(const Type *x, const Type *y);
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
  Expr *adaptedValue(Expr *expr, const Type *to);
  Expr *iteratorValue(Expr *expr, const Type *to);
  Expr *asyncIteratorValue(Expr *expr, const Type *to);
  Expr *adaptedFunction(Expr *expr, const Type *function, const Type *to);
  static std::vector<std::pair<int64_t, int64_t>> targetIntervals(const Type *to);
  bool provesSomeInterval(const Expr *expr, llvm::ArrayRef<std::pair<int64_t, int64_t>> intervals);
  void walkAssignable(const Expr *expr, const Type *to, llvm::StringRef what);

  struct AssignAttempt {
    const Expr *expr = nullptr;
    const Type *from = nullptr;
    const Type *to = nullptr;
    const Type *fromObject = nullptr;
    const Type *toObject = nullptr;
  };

  static bool isFormData(const Type *type);
  static bool isObjectType(const Type *type);
  static bool isWebStream(const Type *type);
  static bool isRequestInit(const AssignAttempt &attempt);
  static bool isStreamIntoBody(const AssignAttempt &attempt);
  static std::string unionText(const Type *toObject);
  static std::string genericClassText(const ClassDecl *generic);
  static AssignAttempt describeAttempt(const Expr *expr, const Type *to);
  static std::string mismatchPrefix(const AssignAttempt &attempt, llvm::StringRef what);
  static bool allHaveLiteralField(llvm::ArrayRef<const Type *> members, llvm::StringRef name);
  std::string objectMismatchText(const AssignAttempt &attempt) const;
  TypeRelationsAssignableErrorCode mismatchOf(const AssignAttempt &attempt) const;
  std::string mismatchReason(const AssignAttempt &attempt,
                             TypeRelationsAssignableErrorCode code) const;
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
