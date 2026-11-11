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
  using ResultVariantChosen = Result<FuncSymbol *, OverloadResolverChooseVariantError>;
  using ResultVariantOfType = Result<FuncSymbol *, OverloadResolverVariantForTypeError>;

  OverloadResolver(SemanticState &owner, SemanticParts &all);

  ResultVariantChosen chooseVariant(llvm::ArrayRef<FunctionDecl *> variants,
                                    std::vector<Expr *> &args, const std::vector<bool> &spread,
                                    bool &checked, std::vector<const Type *> *bound = nullptr);
  Expr *callGenericVariant(CallExpr *call, FuncSymbol *sym, llvm::ArrayRef<const Type *> bound,
                           llvm::SMLoc loc, bool method);
  ResultVariantOfType variantForType(llvm::ArrayRef<FunctionDecl *> variants, const Type *expected,
                                     const MemberExpr *member);
  Expr *checkOverloadCall(CallExpr *call, MemberExpr *member);
  Expr *checkOverloadMember(MemberExpr *member, const Type *expected);
  std::vector<FunctionDecl *> signatureVariants(const Type *iface, const Overload &overload);

private:
  static int conversionRank(const Type *from, const Type *to);
  static void enterRank(const Type *from, const Type *to, RankRun &run);
  static std::optional<int> rankLeaf(const Type *from, const Type *to);
  static int jsonRank(const Type *from);
  static int iteratorRank(const Type *from, const Type *to);
  static bool isOpaqueRanked(const Type *t);
  static std::optional<int> objectRank(const Type *from, const Type *to);
  static bool sameFieldObjects(const Type *from, const Type *to);
  static bool sameShapeObjects(const Type *from, const Type *to);
  static RankJoin rankJoin(const Type *from, const Type *to);
  static void joinRanks(const RankJoin &join, RankRun &run);
  static int worstRank(llvm::ArrayRef<int> ranks);
  static int bestRank(llvm::ArrayRef<int> ranks);
  static int resultRank(const RankJoin &join, llvm::ArrayRef<int> ranks);
  static const Type *parameterAt(const FuncSymbol *sym, size_t i);
  static bool hasFloatLiteral(const Expr *expr);

  SemanticState &state;
  TypeParams *typeParams = nullptr;
  ExprChecker *exprChecker = nullptr;
  FlowChecker *flowChecker = nullptr;
  TypeRelations *typeRelations = nullptr;
  ReturnInferrer *returnInferrer = nullptr;
  ArgumentChecker *argumentChecker = nullptr;
  TypeArgInferrer *typeArgInferrer = nullptr;
  GenericInstantiator *genericInstantiator = nullptr;
};

}
