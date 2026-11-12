#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/semantic_state/semantic_state.h"
#include "compiler/semantic/types/type_relations/type_relations_errors.h"
#include "compiler/union/union/union.h"

namespace tz {

class LiteralTypes;
class SemanticParts;
class ByteProofs;
class ObjectChecker;
class ConstantFolder;
class GenericChecker;
class ReflectChecker;
class UnknownChecker;
class IterableChecker;
class GenericWidening;
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
  bool isPossible;
  std::vector<ConversionTask> needs;
};

enum class UnifyForm { WHOLE, HALF, UNION };
enum class UnifyTaskKind { ENTER, COLLECT };

struct EnterUnify {
  static constexpr UnifyTaskKind kind = UnifyTaskKind::ENTER;
  const Type *first;
  const Type *second;
  UnifyForm form;
};

struct CollectUnify {
  static constexpr UnifyTaskKind kind = UnifyTaskKind::COLLECT;
  const Type *first;
  const Type *second;
  UnifyForm form;
};

using UnifyTask = Union<EnterUnify, CollectUnify>;

struct UnifyShortcut {
  bool isDecided;
  const Type *type;
};

class TypeRelations {
public:
  TypeRelations(SemanticState &owner, SemanticParts &all);

  Expr *convert(Expr *expr, const Type *to);
  Expr *coerce(Expr *expr, const Type *to, llvm::StringRef what = {});
  static bool isImplicitlyConvertible(const Type *from, const Type *to);
  static bool isCopyingIntoRecord(const Type *from, const Type *to);
  static bool isHoldingWideInteger(const Type *type);
  static const Type *commonNumericType(const Type *first, const Type *second);
  const Type *unify(const Type *first, const Type *second) const;
  static const Type *unifyAs(UnifyForm form, const Type *first, const Type *second);
  const Type *comparisonType(const Type *first, const Type *second) const;
  bool isIntLiteralTree(const Expr *expr) const;
  bool isLiteralAdapting(const Expr *expr, const Type *to);
  const Type *literalTarget(const Expr *expr, const Type *to);
  Expr *adaptLiteral(Expr *expr, const Type *to);
  static std::string operandText(llvm::StringRef text);
  std::string conversionHint(const Expr *expr, const Type *to) const;
  std::string conversionMethod(const Type *to) const;

private:
  struct AssignAttempt {
    const Expr *expr = nullptr;
    const Type *from = nullptr;
    const Type *to = nullptr;
    const Type *fromObject = nullptr;
    const Type *toObject = nullptr;
  };

  using ResultTypeAssignable = Result<const Expr *, TypeRelationsAssignableError>;
  using ResultRangeWasProven = Result<const Expr *, TypeRelationsRangeProvenError>;

  ResultTypeAssignable assignable(const Expr *expr, const Type *to, llvm::StringRef what) const;
  ResultRangeWasProven rangeProven(const Expr *expr, const Type *to, llvm::StringRef what,
                                   llvm::ArrayRef<std::pair<int64_t, int64_t>> intervals) const;

  Expr *iteratorOf(Expr *expr);
  static ConversionStep stepHolds();
  static ConversionStep stepFails();
  static bool isNameUnit(char unit);
  bool isFormData(const Type *type) const;
  bool isWebStream(const Type *type) const;
  bool isComparable(const Type *base) const;
  static bool isNumberLike(const Type *type);
  static bool isWideInteger(const Type *type);
  static bool isRuntimeObject(const Type *type);
  bool isIntLiteralNode(const Expr *expr) const;
  bool isReflectingTarget(const Type *to) const;
  static UnifyShortcut decided(const Type *type);
  Expr *adaptedValue(Expr *expr, const Type *to);
  const Type *numberTarget(const Type *to) const;
  Expr *iteratorValue(Expr *expr, const Type *to);
  static bool isPlainOperand(llvm::StringRef text);
  const Type *functionTarget(const Type *to) const;
  std::string shownOperand(const Expr *expr) const;
  Expr *adaptedTo(Expr *expr, const Type *function);
  bool hasObjectMember(const Type *unionType) const;
  std::string unionText(const Type *toObject) const;
  static ConversionStep stepDecided(bool isPossible);
  static bool isWrappedInParens(llvm::StringRef text);
  void rememberView(const Type *from, const Type *to);
  Expr *asyncIteratorValue(Expr *expr, const Type *to);
  static ConversionStep memberTrial(MemberChoice choice);
  bool isRequestInit(const AssignAttempt &attempt) const;
  std::string shownMember(const MemberExpr *member) const;
  static ConversionStep memberStep(const MemberGoal &goal);
  bool isStreamIntoBody(const AssignAttempt &attempt) const;
  static ConversionStep taskStep(const ConversionTask &task);
  static bool hasSameFields(const Type *from, const Type *to);
  IntLitExpr *retypedLiteral(Expr *expr, const Type *to) const;
  const Field *sharedLiteralField(const Type *unionType) const;
  std::string genericClassText(const ClassDecl *generic) const;
  static bool hasRecordCopyShape(const Type *from, const Type *to);
  static bool canRetryMember(std::vector<ConversionTask> &tasks);
  const Type *otherPlace(const Expr *expr, const Type *to) const;
  static bool isSameKindOfShape(const Type *from, const Type *to);
  llvm::SMLoc placeLoc(const Expr *expr, const Type *place) const;
  static ConversionStep jsonStep(const Type *from, const Type *to);
  static ConversionStep stepNeeds(std::vector<ConversionGoal> goals);
  static ConversionStep resultStep(const Type *from, const Type *to);
  const Type *innerObject(const Type *type, const Type *other) const;
  std::string objectMismatchText(const AssignAttempt &attempt) const;
  static const Type *widerJson(const Type *first, const Type *second);
  static ConversionStep memberGoals(const Type *from, const Type *to);
  static const Type *baseUnify(const Type *first, const Type *second);
  static ConversionStep classRefStep(const Type *from, const Type *to);
  static const Type *widerArray(const Type *first, const Type *second);
  static const Type *collectedHalf(std::vector<const Type *> &answers);
  static ConversionStep anyObjectStep(const Type *from, const Type *to);
  AssignAttempt describeAttempt(const Expr *expr, const Type *to) const;
  static ConversionStep conversionStep(const Type *from, const Type *to);
  static const Type *commonParent(const Type *first, const Type *second);
  static ConversionStep recordCopyStep(const Type *from, const Type *to);
  static UnifyShortcut shortUnify(const Type *first, const Type *second);
  static ConversionStep unionSourceStep(const Type *from, const Type *to);
  Expr *adaptedFunction(Expr *expr, const Type *function, const Type *to);
  static const Type *settledPromise(const Type *first, const Type *second);
  static const Type *widerOfRelated(const Type *first, const Type *second);
  static const Type *commonInstance(const Type *first, const Type *second);
  static const Type *commonAnyObject(const Type *first, const Type *second);
  static const Type *copiedFieldType(const Field &field, const Type *value);
  static const Type *commonConstructor(const Type *first, const Type *second);
  void walkAssignable(const Expr *expr, const Type *to, llvm::StringRef what);
  static std::optional<bool> plainConvertible(const Type *from, const Type *to);
  std::vector<std::pair<int64_t, int64_t>> targetIntervals(const Type *to) const;
  bool isValueFitting(int64_t value, const Type *from, const Type *target) const;
  TypeRelationsAssignableErrorCode mismatchOf(const AssignAttempt &attempt) const;
  static std::optional<ConversionStep> targetStep(const Type *from, const Type *to);
  static std::optional<ConversionStep> sourceStep(const Type *from, const Type *to);
  std::string mismatchPrefix(const AssignAttempt &attempt, llvm::StringRef what) const;
  static const Type *optionalLike(const Type *first, const Type *second, const Type *base);
  static void collectUnify(const CollectUnify &collect, std::vector<const Type *> &answers);
  bool hasLiteralFieldInAll(llvm::ArrayRef<const Type *> members, llvm::StringRef name) const;
  static bool canTryNextMember(const MemberChoice &choice, std::vector<ConversionTask> &tasks);
  static void pushResultUnify(std::vector<UnifyTask> &tasks, const Type *first, const Type *second);
  static const Type *collectedType(const CollectUnify &collect, std::vector<const Type *> &answers);
  static const Type *collectedUnion(const CollectUnify &collect,
                                    std::vector<const Type *> &answers);
  bool canProveSomeInterval(const Expr *expr,
                            llvm::ArrayRef<std::pair<int64_t, int64_t>> intervals);
  static const Type *collectedResult(const CollectUnify &collect,
                                     std::vector<const Type *> &answers);
  std::string mismatchReason(const AssignAttempt &attempt,
                             TypeRelationsAssignableErrorCode code) const;
  std::pair<int64_t, int64_t>
  widestInterval(llvm::ArrayRef<std::pair<int64_t, int64_t>> intervals) const;
  static void enterUnify(const EnterUnify &enter, std::vector<UnifyTask> &tasks,
                         std::vector<const Type *> &answers);
  std::string rangeCheck(const Expr *expr, const Type *target,
                         llvm::ArrayRef<std::pair<int64_t, int64_t>> intervals) const;
  static void pushUnify(std::vector<UnifyTask> &tasks, const Type *first, const Type *second,
                        UnifyForm collected, UnifyForm entered);
  static void enterHalfUnify(const Type *first, const Type *second, std::vector<UnifyTask> &tasks,
                             std::vector<const Type *> &answers);
  static void enterWholeUnify(const Type *first, const Type *second, std::vector<UnifyTask> &tasks,
                              std::vector<const Type *> &answers);

  SemanticState &state;
  ByteProofs *byteProofs = nullptr;
  LiteralTypes *literalTypes = nullptr;
  ObjectChecker *objectChecker = nullptr;
  ConstantFolder *constantFolder = nullptr;
  GenericChecker *genericChecker = nullptr;
  ReflectChecker *reflectChecker = nullptr;
  UnknownChecker *unknownChecker = nullptr;
  IterableChecker *iterableChecker = nullptr;
  GenericWidening *genericWidening = nullptr;
  ConditionalTypes *conditionalTypes = nullptr;
  TzModulesChecker *tzModulesChecker = nullptr;
  WideningRelations *wideningRelations = nullptr;
  FunctionValueChecker *functionValueChecker = nullptr;
};

}
