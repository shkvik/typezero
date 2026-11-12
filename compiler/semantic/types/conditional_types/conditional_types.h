#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/semantic_state/semantic_state.h"
#include "compiler/semantic/types/conditional_types/conditional_types_errors.h"
#include "compiler/union/union/union.h"

namespace tz {

class SemanticParts;
class TypeParams;
class TypeDeclarer;
class TypeResolver;
class ClassDeclarer;
class TypeOperators;
class ReturnInferrer;
class ExtendsRelation;
class TopLevelChecker;

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

enum class BranchCall {
  PLAIN,
  TAIL,
};

struct InferRef {
  const TypeRef *ref;
  bool isList;
};

struct SpreadRef {
  const TypeRef *ref;
  bool isSpread;
};

enum class PreparationKind { ANSWERED, PREPARED };

struct AnsweredConditional {
  static constexpr PreparationKind kind = PreparationKind::ANSWERED;
  const Type *answer;
};

struct PreparedConditional {
  static constexpr PreparationKind kind = PreparationKind::PREPARED;
  const Type *check;
  const Type *pattern;
  std::vector<InferRef> inferRefs;
  std::vector<const Type *> infers;
};

using ConditionalPreparation = Union<AnsweredConditional, PreparedConditional>;

class ConditionalTypes {
public:
  using ResultIntersectionJoined = Result<const Type *, ConditionalTypesResolveIntersectionError>;

  ConditionalTypes(SemanticState &owner, SemanticParts &all);

  const Type *resolveConditional(const TypeRef &ref);
  const Type *signatureForUtility(const TypeRef &ref);
  const Type *resolveTypeArgument(const TypeRef &ref);
  bool isFittingConditional(const Type *from, const Type *to);
  ResultIntersectionJoined resolveIntersection(const TypeRef &ref);
  const Type *evalConditional(llvm::ArrayRef<const Type *> operands);
  std::vector<const Type *> conditionalBranches(const Type *conditional);
  bool isMentioningParamBesides(const Type *type, llvm::ArrayRef<const Type *> allowed) const;

private:
  using ResultObjectsAgreed = Result<void, ConditionalTypesResolveIntersectionError>;
  using ResultObjectsMerged =
      Result<std::vector<const Type *>, ConditionalTypesResolveIntersectionError>;

  ResultObjectsAgreed checkAllObjects(llvm::ArrayRef<const Type *> parts) const;
  ResultObjectsAgreed joinField(std::vector<Field> &fields, const Field &field) const;
  ResultIntersectionJoined joinObjects(const Type *left, const Type *right) const;
  ResultObjectsMerged mergedWith(llvm::ArrayRef<const Type *> merged, const Type *part) const;

  bool isBinding(const Stmt *decl) const;
  bool isPrimitive(const Type *type) const;
  bool isPlainParam(const Type *type) const;
  bool isPlainObject(const Type *type) const;
  bool isEmptyObject(const Type *type) const;
  bool isDistributing(const TypeRef &ref) const;
  bool canRetryFits(std::vector<FitsStep> &steps);
  bool isSpreadAt(const TypeRef &ref, size_t index) const;
  bool isExtendingWithParams(const Type *from, const Type *branch);
  bool isFittingBy(FitsWay way, const Type *from, const Type *branch);
  bool isGoalFitting(const FitsGoal &goal, std::vector<FitsStep> &steps);
  bool canTryFitsAlternative(FitsChoice choice, std::vector<FitsStep> &steps);
  bool isFirstMention(const Type *type, llvm::SmallPtrSetImpl<const Type *> &seen) const;

  const Type *deferConditional(const TypeRef &ref);
  const Type *memberConditional(const TypeRef &ref);
  const Type *erasedSignature(FuncSymbol *function);
  const Type *resolvePattern(const TypeRef &patternRef);
  const Type *writtenConstraint(const TypeRef &infer);
  const Type *boundType(llvm::StringRef name) const;
  const Type *erasedParam(const Type *param) const;
  std::string spellingOf(const TypeRef &ref) const;
  const Type *genericFunctionSignature(llvm::StringRef name);
  const ConditionalInfo *knownConditional(const TypeRef &ref);
  std::vector<InferRef> infersIn(const TypeRef &ref) const;
  TypeAliasDecl *genericAliasNamed(const TypeRef &branch) const;
  ConditionalPreparation prepareConditional(const TypeRef &ref);
  const Type *narrowedBy(const Type *filter, const Type *other);
  std::vector<const Type *> paramParts(const Type *type) const;
  std::vector<const Type *> fieldCopies(const Type *part) const;
  std::vector<llvm::StringRef> namesIn(const TypeRef &ref) const;
  std::vector<const Type *> plainParamsIn(const Type *type) const;
  std::vector<const Type *> operandParts(const Type *type) const;
  std::vector<const Type *> objectParts(const Type *object) const;
  const Type *resolveBranch(const TypeRef &branch, BranchCall call);
  llvm::ArrayRef<const Type *> membersOf(const Type *&type) const;
  std::vector<const Type *> intersectionParts(const TypeRef &ref);
  void noteInfer(SpreadRef part, std::vector<InferRef> &infers) const;
  std::vector<const Type *> functionParts(const Type *function) const;
  void bindType(llvm::StringRef name, const Type *type, llvm::SMLoc loc);
  const Type *firstNonObject(llvm::ArrayRef<const Type *> parts) const;
  const Type *boundOrDefault(const Type *bound, const Type *infer) const;
  const Type *rememberTailCall(TypeAliasDecl *alias, const TypeRef &branch);
  const Type *commonFieldType(const Type *first, const Type *second) const;
  const Type *narrowedByIntersection(llvm::ArrayRef<const Type *> parts);
  Field *fieldNamed(std::vector<Field> &fields, llvm::StringRef name) const;
  std::vector<const Type *> bindInferParams(llvm::ArrayRef<InferRef> inferRefs);
  void pushSpreadArgs(const TypeRef &ref, std::vector<SpreadRef> &stack) const;
  const Type *classMethodSignature(const Type *object, llvm::StringRef methodName);
  const Type *methodSignature(llvm::StringRef objectName, llvm::StringRef methodName);
  void bindInferred(const PreparedConditional &prepared, const InferMatch &inference);
  const Type *distributeConditional(const TypeRef &ref, llvm::ArrayRef<const Type *> members);
  void enterConditionalScope(const ConditionalInfo &info, llvm::ArrayRef<const Type *> operands);
  const Type *constrainInfers(llvm::ArrayRef<InferRef> inferRefs,
                              llvm::ArrayRef<const Type *> infers);
  const Type *chooseBranch(const TypeRef &ref, const PreparedConditional &prepared,
                           BranchCall call);
  std::vector<const Type *> otherParamParts(const Type *type,
                                            llvm::SmallPtrSetImpl<const Type *> &seen) const;
  std::vector<const Type *> objectParamParts(const Type *type,
                                             llvm::SmallPtrSetImpl<const Type *> &seen) const;

  SemanticState &state;
  TypeParams *typeParams = nullptr;
  TypeDeclarer *typeDeclarer = nullptr;
  TypeResolver *typeResolver = nullptr;
  ClassDeclarer *classDeclarer = nullptr;
  TypeOperators *typeOperators = nullptr;
  ReturnInferrer *returnInferrer = nullptr;
  ExtendsRelation *extendsRelation = nullptr;
  TopLevelChecker *topLevelChecker = nullptr;
};

}
