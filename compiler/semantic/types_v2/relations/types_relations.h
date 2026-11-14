#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/program/library/builtin_names/type_name_table/type_name_table.h"
#include "compiler/program/types/types.h"
#include "compiler/semantic/semantic_state/v2/types/type_context/type_context.h"
#include "compiler/union/union/union.h"
#include <vector>

namespace tz {

struct TypeStorage;

enum class ExtendsStepKind { GOAL, BIND, CHOICE };

struct ExtendsGoal {
  static constexpr ExtendsStepKind kind = ExtendsStepKind::GOAL;
  const Type *type;
  const Type *pattern;
};

struct BindInfer {
  static constexpr ExtendsStepKind kind = ExtendsStepKind::BIND;
  size_t index;
  const Type *type;
};

struct ExtendsChoice {
  static constexpr ExtendsStepKind kind = ExtendsStepKind::CHOICE;
  const Type *type;
  const Type *pattern;
  size_t next;
  std::vector<const Type *> saved;
};

using ExtendsStep = Union<ExtendsGoal, BindInfer, ExtendsChoice>;

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

enum class FitsStepKind { GOAL, CHOICE };

struct FitsGoal {
  static constexpr FitsStepKind kind = FitsStepKind::GOAL;
  const Type *from;
  const Type *to;
  unsigned depth;
};

struct FitsChoice {
  static constexpr FitsStepKind kind = FitsStepKind::CHOICE;
  const Type *from;
  unsigned depth;
  std::vector<const Type *> branches;
  size_t next;
};

using FitsStep = Union<FitsGoal, FitsChoice>;

enum class FitsWay {
  CONVERSION,
  NESTED,
  INFERENCE,
};

class TypesRelations {
public:
  explicit TypesRelations(TypeStorage &owner);

  bool isHoldingWideInteger(const Type *type);
  bool isExpectingLiteral(const Type *param) const;
  bool isCopyingIntoRecord(const Type *from, const Type *to);
  bool isExtendingType(const Type *type, const Type *pattern);
  bool isFittingConditional(const Type *from, const Type *to);
  bool isImplicitlyConvertible(const Type *from, const Type *to);
  bool isExtendingType(const Type *type, const Type *pattern, InferMatch *inference);
  bool isMentioningParamBesides(const Type *type, llvm::ArrayRef<const Type *> allowed) const;
  bool isMatchingTemplate(const std::u16string &text, llvm::ArrayRef<const Type *> parts,
                          InferMatch *inference);


  const Type *unify(const Type *first, const Type *second) const;
  const Type *commonNumericType(const Type *first, const Type *second);
  const Type *comparisonType(const Type *first, const Type *second) const;
  const Type *unifyAs(UnifyForm form, const Type *first, const Type *second);


  std::vector<FuncSymbol *> findExtendsNeeds(const Type *type, const Type *pattern,
                                             const InferMatch *inference);

private:
  void bindInfer(const BindInfer &bind, InferMatch *inference) const;
  void collectUnify(const CollectUnify &collect, std::vector<const Type *> &answers);
  void pushResultUnify(std::vector<UnifyTask> &tasks, const Type *first, const Type *second);
  void pushDefinedGoal(const Type *type, const Type *pattern,
                       std::vector<ExtendsStep> &steps) const;
  void enterUnify(const EnterUnify &enter, std::vector<UnifyTask> &tasks,
                  std::vector<const Type *> &answers);
  void pushMemberGoals(llvm::ArrayRef<const Type *> types, const Type *pattern,
                       std::vector<ExtendsStep> &steps) const;
  void pushUnify(std::vector<UnifyTask> &tasks, const Type *first, const Type *second,
                 UnifyForm collected, UnifyForm entered);
  void enterHalfUnify(const Type *first, const Type *second, std::vector<UnifyTask> &tasks,
                      std::vector<const Type *> &answers);
  void enterWholeUnify(const Type *first, const Type *second, std::vector<UnifyTask> &tasks,
                       std::vector<const Type *> &answers);


  bool isNumberLike(const Type *type);
  bool isWideInteger(const Type *type);
  bool isRuntimeObject(const Type *type);
  bool isComparable(const Type *base) const;
  bool isPlainParam(const Type *type) const;
  bool canRetryFits(std::vector<FitsStep> &steps);
  bool hasSameFields(const Type *from, const Type *to);
  bool canRetryMember(std::vector<ConversionTask> &tasks);
  bool isSameKindOfShape(const Type *from, const Type *to);
  bool hasRecordCopyShape(const Type *from, const Type *to);
  bool isExtendingWithParams(const Type *from, const Type *branch);
  bool isFittingBy(FitsWay way, const Type *from, const Type *branch);
  bool isGoalFitting(const FitsGoal &goal, std::vector<FitsStep> &steps);
  bool isNumberHole(const Type *hole, const InferMatch *inference) const;
  bool canTryFitsAlternative(FitsChoice choice, std::vector<FitsStep> &steps);
  bool isParamFitting(const Type *my, const Type *their, InferMatch *inference);
  bool canRetryChoice(std::vector<ExtendsStep> &steps, InferMatch *inference) const;
  bool isExtendingRest(const Type *type, const Type *pattern, InferMatch *inference);
  bool canTryAlternative(ExtendsChoice choice, std::vector<ExtendsStep> &steps) const;
  bool isExtendingClass(const Type *type, const Type *pattern, InferMatch *inference);
  bool isExtendingTuple(const Type *type, const Type *pattern, InferMatch *inference);
  bool isExtendingObject(const Type *type, const Type *pattern, InferMatch *inference);
  bool canTryNextMember(const MemberChoice &choice, std::vector<ConversionTask> &tasks);
  bool isExtendingFunction(const Type *type, const Type *pattern, InferMatch *inference);
  bool isFirstMention(const Type *type, llvm::SmallPtrSetImpl<const Type *> &seen) const;
  bool isInstanceOf(const Type *candidate, const ClassDecl *generic, size_t typeArgCount) const;
  bool isPieceFittingHole(const std::u16string &piece, const Type *hole, InferMatch *inference);
  bool isDictHolding(const Type *type, const Type *pattern, std::vector<ExtendsStep> &steps) const;
  bool isArrayHolding(const Type *type, const Type *pattern, std::vector<ExtendsStep> &steps) const;
  bool isResultHolding(const Type *type, const Type *pattern,
                       std::vector<ExtendsStep> &steps) const;
  bool isGoalHolding(const ExtendsGoal &goal, InferMatch *inference,
                     std::vector<ExtendsStep> &steps);
  bool isPromiseHolding(const Type *type, const Type *pattern,
                        std::vector<ExtendsStep> &steps) const;
  bool isStepHolding(const ExtendsStep &step, InferMatch *inference,
                     std::vector<ExtendsStep> &steps);
  bool isIteratorHolding(const Type *type, const Type *pattern,
                         std::vector<ExtendsStep> &steps) const;
  bool isExtendingPlainTuple(llvm::ArrayRef<const Type *> mine, const Type *pattern,
                             InferMatch *inference);
  bool canTryNext(const ExtendsChoice &choice, InferMatch *inference,
                  std::vector<ExtendsStep> &steps) const;
  bool isExtendingField(const Type *type, const ClassDecl *classDecl, const Field &field,
                        InferMatch *inference);
  bool isLeafHolding(const Type *type, const Type *pattern, InferMatch *inference,
                     std::vector<ExtendsStep> &steps);
  bool isTupleHolding(const Type *type, const Type *pattern, InferMatch *inference,
                      std::vector<ExtendsStep> &steps);
  bool isExtendingEach(llvm::ArrayRef<const Type *> types, llvm::ArrayRef<const Type *> patterns,
                       InferMatch *inference);
  bool isStructureHolding(const Type *type, const Type *pattern, InferMatch *inference,
                          std::vector<ExtendsStep> &steps);
  bool canStartChoice(const Type *type, const Type *pattern, InferMatch *inference,
                      std::vector<ExtendsStep> &steps) const;
  bool isBindingInferred(const Type *type, const Type *pattern, InferMatch *inference,
                         std::vector<ExtendsStep> &steps) const;
  bool isExtendingSpreadTuple(llvm::ArrayRef<const Type *> mine,
                              llvm::ArrayRef<const Type *> theirs, size_t spread,
                              InferMatch *inference);
  bool isExtendingRestTuple(llvm::ArrayRef<const Type *> mine, llvm::ArrayRef<const Type *> theirs,
                            const Type *restElement, InferMatch *inference);


  const Type *numberLiteral(double value) const;
  const Type *baseUnify(const Type *first, const Type *second);
  const Type *widerJson(const Type *first, const Type *second);
  const Type *collectedHalf(std::vector<const Type *> &answers);
  const Type *paramAt(const Type *function, size_t index) const;
  const Type *knownReturnType(const FuncSymbol *function) const;
  const Type *knownFunctionType(const FuncSymbol *function) const;
  const Type *widerArray(const Type *first, const Type *second);
  const Type *commonParent(const Type *first, const Type *second);
  const Type *commonInstance(const Type *first, const Type *second);
  const Type *settledPromise(const Type *first, const Type *second);
  const Type *widerOfRelated(const Type *first, const Type *second);
  const Type *commonAnyObject(const Type *first, const Type *second);
  const Type *copiedFieldType(const Field &field, const Type *value);
  const Type *restArguments(const Type *function, size_t from) const;
  const Type *commonConstructor(const Type *first, const Type *second);
  const Type *optionalLike(const Type *first, const Type *second, const Type *base);
  const Type *collectedType(const CollectUnify &collect, std::vector<const Type *> &answers);
  const Type *collectedUnion(const CollectUnify &collect, std::vector<const Type *> &answers);
  const Type *collectedResult(const CollectUnify &collect, std::vector<const Type *> &answers);
  const Type *memberType(const Type *type, const ClassDecl *classDecl, llvm::StringRef name) const;
  const Type *pieceType(const std::u16string &piece, const Type *hole,
                        const InferMatch *inference) const;


  UnifyShortcut decided(const Type *type);
  UnifyShortcut shortUnify(const Type *first, const Type *second);


  ConversionStep stepFails();
  ConversionStep stepHolds();
  ConversionStep stepDecided(bool isPossible);
  ConversionStep memberTrial(MemberChoice choice);
  ConversionStep memberStep(const MemberGoal &goal);
  ConversionStep taskStep(const ConversionTask &task);
  ConversionStep jsonStep(const Type *from, const Type *to);
  ConversionStep resultStep(const Type *from, const Type *to);
  ConversionStep stepNeeds(std::vector<ConversionGoal> goals);
  ConversionStep memberGoals(const Type *from, const Type *to);
  ConversionStep classRefStep(const Type *from, const Type *to);
  ConversionStep anyObjectStep(const Type *from, const Type *to);
  ConversionStep conversionStep(const Type *from, const Type *to);
  ConversionStep recordCopyStep(const Type *from, const Type *to);
  ConversionStep unionSourceStep(const Type *from, const Type *to);


  std::vector<const Type *> paramParts(const Type *type) const;
  std::vector<const Type *> fieldTypes(const Type *object) const;
  std::vector<const Type *> operandParts(const Type *type) const;
  std::vector<const Type *> objectParts(const Type *object) const;
  std::vector<const Type *> plainParamsIn(const Type *type) const;
  std::vector<const Type *> functionParts(const Type *function) const;
  std::vector<const Type *> otherParamParts(const Type *type,
                                            llvm::SmallPtrSetImpl<const Type *> &seen) const;
  std::vector<const Type *> objectParamParts(const Type *type,
                                             llvm::SmallPtrSetImpl<const Type *> &seen) const;


  std::optional<ConversionStep> sourceStep(const Type *from, const Type *to);
  std::optional<ConversionStep> targetStep(const Type *from, const Type *to);


  size_t fixedParamCount(const Type *function) const;
  llvm::ArrayRef<const Type *> membersOf(const Type *&type) const;
  const ClassDecl *genericOrigin(const ClassDecl *classDecl) const;
  std::optional<bool> plainConvertible(const Type *from, const Type *to);
  std::optional<size_t> soleSpread(llvm::ArrayRef<const Type *> elements) const;
  FuncSymbol *unknownMethod(const Type *type, const ClassDecl *classDecl,
                            llvm::StringRef name) const;

  TypeStorage &types;
  std::vector<FuncSymbol *> *neededFunctions = nullptr;
};

}
