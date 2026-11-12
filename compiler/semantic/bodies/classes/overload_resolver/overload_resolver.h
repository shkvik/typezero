#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/classes/overload_resolver/overload_resolver_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"
#include "compiler/union/union/union.h"

namespace tz {

class SemanticParts;
class TypeParams;
class ExprChecker;
class FlowChecker;
class TypeRelations;
class ReturnInferrer;
class DecimalChecker;
class ArgumentChecker;
class TypeArgInferrer;
class GenericInstantiator;

enum class RankForm {
  SAME,
  OPTIONAL_SAME,
  OPTIONAL_WRAP,
  WORST_MEMBER,
  BEST_TARGET,
  RESULT,
  FIELDS
};

enum class RankStepKind { GOAL, JOIN };

struct RankGoal {
  static constexpr RankStepKind kind = RankStepKind::GOAL;
  const Type *from;
  const Type *to;
};

struct RankJoin {
  static constexpr RankStepKind kind = RankStepKind::JOIN;
  const Type *from;
  const Type *to;
  RankForm form;
  std::vector<RankGoal> parts;
};

using RankStep = Union<RankGoal, RankJoin>;

struct RankRun {
  std::vector<RankStep> steps;
  std::vector<int> answers;
  std::vector<std::pair<const Type *, const Type *>> path;
};

class OverloadResolver {
public:
  struct ChosenVariant {
    FuncSymbol *function;
    bool isChecked;
    std::vector<const Type *> typeArgs;
  };

  using ResultVariantChosen = Result<ChosenVariant, OverloadResolverChooseVariantError>;
  using ResultVariantOfType = Result<FuncSymbol *, OverloadResolverVariantForTypeError>;

  OverloadResolver(SemanticState &owner, SemanticParts &all);

  Expr *checkOverloadCall(CallExpr *call, MemberExpr *member);
  Expr *checkOverloadMember(MemberExpr *member, const Type *expected);
  ResultVariantChosen chooseVariant(llvm::ArrayRef<FunctionDecl *> variants,
                                    std::vector<Expr *> &args, const std::vector<bool> &spread);
  ResultVariantOfType variantForType(llvm::ArrayRef<FunctionDecl *> variants, const Type *expected,
                                     const MemberExpr *member);
  Expr *callGenericMethod(CallExpr *call, FuncSymbol *function, llvm::ArrayRef<const Type *> bound,
                          llvm::SMLoc loc);
  ResultVariantChosen choosePlainVariant(llvm::ArrayRef<FunctionDecl *> variants,
                                         std::vector<Expr *> &args,
                                         const std::vector<bool> &spread);
  Expr *callGenericFunction(CallExpr *call, FuncSymbol *function,
                            llvm::ArrayRef<const Type *> bound, llvm::SMLoc loc);

private:
  enum class GenericVariants {
    ALLOWED,
    REJECTED,
  };

  enum class GenericCallee {
    METHOD,
    FUNCTION,
  };

  struct Candidate {
    FunctionDecl *functionDecl;
    std::vector<int> ranks;
    bool isLeavingOut;
    std::vector<const Type *> typeArgs;
    bool isAdapted;
  };

  struct VariantRanking {
    std::vector<Candidate> applicable;
    std::string why;
    std::string numberHint;
  };

  using ResultBestCandidate = Result<Candidate, OverloadResolverChooseVariantError>;
  using ResultChoiceAllowed = Result<void, OverloadResolverChooseVariantError>;
  using ResultVariantsFound =
      Result<std::vector<FunctionDecl *>, OverloadResolverChooseVariantError>;
  using ResultCandidatesFit = Result<std::vector<Candidate>, OverloadResolverChooseVariantError>;

  ResultBestCandidate soleBest(llvm::StringRef what, llvm::ArrayRef<Candidate> applicable) const;
  ResultChoiceAllowed checkGenericAllowed(const Candidate &candidate,
                                          GenericVariants generics) const;
  ResultChoiceAllowed checkArgumentsFor(llvm::ArrayRef<FunctionDecl *> fit,
                                        std::vector<Expr *> &args);
  ResultChoiceAllowed checkNoSpread(llvm::StringRef what, std::vector<Expr *> &args,
                                    const std::vector<bool> &spread);
  ResultCandidatesFit rankedCandidates(llvm::StringRef what, llvm::ArrayRef<FunctionDecl *> fit,
                                       std::vector<Expr *> &args);
  ResultVariantsFound fittingVariants(llvm::StringRef what, llvm::ArrayRef<FunctionDecl *> variants,
                                      std::vector<Expr *> &args);
  ResultVariantChosen chooseAmong(llvm::ArrayRef<FunctionDecl *> variants,
                                  std::vector<Expr *> &args, const std::vector<bool> &spread,
                                  GenericVariants generics);

  int jsonRank(const Type *from) const;
  int worstRank(llvm::ArrayRef<int> ranks) const;
  int bestRank(llvm::ArrayRef<int> ranks) const;
  void walkLoosely(std::vector<Expr *> &args);
  bool isShapeObject(const Type *type) const;
  bool isOpaqueRanked(const Type *type) const;
  bool hasFloatLiteral(const Expr *expr) const;
  bool isPlainObjectParam(const Type *param) const;
  int bytesRank(const Type *from, const Type *to) const;
  int matchRank(const Type *from, const Type *to) const;
  void joinRanks(const RankJoin &join, RankRun &run) const;
  void enterRank(const RankGoal &goal, RankRun &run) const;
  int iteratorRank(const Type *from, const Type *to) const;
  bool isSameShape(const Type *from, const Type *to) const;
  int conversionRank(const Type *from, const Type *to) const;
  bool hasSameFields(const Type *from, const Type *to) const;
  int subtypeDistance(const Type *from, const Type *to) const;
  RankJoin rankJoin(const Type *from, const Type *to) const;
  std::string shownObject(const MemberExpr *member) const;
  RankJoin fieldsJoin(const Type *from, const Type *to) const;
  RankJoin resultJoin(const Type *from, const Type *to) const;
  Param signatureParam(const Type *signature, size_t index);
  ChosenVariant uncheckedChoice(FunctionDecl *variant) const;
  RankJoin targetsJoin(const Type *from, const Type *to) const;
  RankJoin membersJoin(const Type *from, const Type *to) const;
  bool isInstantiatedNow(const FuncSymbol *function) const;
  std::string variantsName(const FunctionDecl *first) const;
  RankJoin optionalJoin(const Type *from, const Type *to) const;
  int halfRank(const Type *from, const Type *to, int rank) const;
  std::string signatureText(const FunctionDecl *variant) const;
  bool isOfType(const FunctionDecl *variant, const Type *wanted);
  bool isTakingCount(const FuncSymbol *function, size_t count) const;
  int joinedRank(const RankJoin &join, llvm::ArrayRef<int> ranks) const;
  int resultRank(const RankJoin &join, llvm::ArrayRef<int> ranks) const;
  std::optional<int> rankLeaf(const Type *from, const Type *to) const;
  std::optional<int> objectRank(const Type *from, const Type *to) const;
  const Type *parameterAt(const FuncSymbol *function, size_t index) const;
  bool isBetter(const Candidate &candidate, const Candidate &other) const;
  const Type *candidateParam(const Candidate &candidate, size_t index) const;
  int argumentRank(const Expr *arg, const Type *param, Candidate &candidate) const;
  bool isFittingCallback(const FuncSymbol *function, const Signature *callback) const;
  void setGenericTarget(CallExpr *call, FuncSymbol *callee, GenericCallee kind) const;
  FunctionDecl *signatureVariant(const Overload &overload, const Type *signature);
  const Type *boundReturnType(FuncSymbol *function, llvm::ArrayRef<const Type *> bound);
  const Type *commonParameter(llvm::ArrayRef<FunctionDecl *> fit, size_t index) const;
  int signatureIndex(llvm::ArrayRef<FunctionDecl *> variants, const FuncSymbol *function) const;
  void rankVariant(FunctionDecl *variant, std::vector<Expr *> &args, VariantRanking &ranking);
  std::vector<FunctionDecl *> signatureVariants(const Type *interfaceType,
                                                const Overload &overload);
  bool isBetterThanOthers(const Candidate &candidate, llvm::ArrayRef<Candidate> applicable) const;
  std::vector<FunctionDecl *> variantsOfType(llvm::ArrayRef<FunctionDecl *> variants,
                                             const Type *expected);
  std::vector<const Type *> boundParams(const FuncSymbol *function,
                                        llvm::ArrayRef<const Type *> bound);
  FuncSymbol *emittedCallee(FuncSymbol *function, llvm::ArrayRef<const Type *> bound,
                            llvm::SMLoc loc);
  const Type *soleObjectShape(llvm::ArrayRef<FunctionDecl *> fit, const Expr *arg,
                              size_t index) const;
  const Type *expectedArgument(llvm::ArrayRef<FunctionDecl *> fit, const Expr *arg,
                               size_t index) const;
  const Type *decimalForLiteral(llvm::ArrayRef<FunctionDecl *> fit, const Expr *arg,
                                size_t index) const;
  Expr *callGeneric(CallExpr *call, FuncSymbol *function, llvm::ArrayRef<const Type *> bound,
                    llvm::SMLoc loc, GenericCallee kind);
  void noteMismatch(const FunctionDecl *variant, const Expr *arg, const Type *param, size_t index,
                    VariantRanking &ranking) const;

  SemanticState &state;
  TypeParams *typeParams = nullptr;
  ExprChecker *exprChecker = nullptr;
  FlowChecker *flowChecker = nullptr;
  TypeRelations *typeRelations = nullptr;
  ReturnInferrer *returnInferrer = nullptr;
  DecimalChecker *decimalChecker = nullptr;
  ArgumentChecker *argumentChecker = nullptr;
  TypeArgInferrer *typeArgInferrer = nullptr;
  GenericInstantiator *genericInstantiator = nullptr;
};

}
