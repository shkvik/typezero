#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/generics/inference/inference.h"
#include "compiler/semantic/generics/type_arg_inferrer/type_arg_inferrer_errors.h"
#include "compiler/semantic/generics/type_arg_inferrer/type_arg_inferrer_types.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class LiteralTypes;
class SemanticParts;
class TypeParams;
class CallChecker;
class ExprChecker;
class LateClasses;
class ResultTypes;
class TypeResolver;
class UtilityTypes;
class TypeRelations;
class ClosureChecker;
class GenericChecker;
class ArgumentChecker;
class IterableChecker;
class GenericInstantiator;
class FunctionValueChecker;

struct Signature;

class TypeArgInferrer {
public:
  TypeArgInferrer(SemanticState &owner, SemanticParts &all);

  bool finishTypeArgs(FuncSymbol *generic, std::vector<const Type *> &bound, llvm::SMLoc loc,
                      TypeArgUse use);
  bool inferFromArguments(FuncSymbol *generic, llvm::ArrayRef<Expr *> args,
                          std::vector<const Type *> &bound);
  Expr *checkGenericCall(CallExpr *call, FuncSymbol *generic, llvm::SMLoc loc, const Type *expected,
                         llvm::ArrayRef<TypeRef> typeArgs);
  bool bindGenericCall(CallExpr *call, FuncSymbol *generic, llvm::SMLoc loc, const Type *expected,
                       llvm::ArrayRef<TypeRef> typeArgs, std::vector<const Type *> &bound);

private:
  enum class GrowthPlace {
    TOP,
    INSIDE,
  };

  enum class MissingTypeArgs {
    REPORTED,
    SILENT,
  };

  enum class ArgumentChecks {
    KEEP_CHECKED,
    CHECK_ALL,
  };

  using ResultTypeArgsCount = Result<size_t, TypeArgInferrerTypeArgCountError>;
  using ResultParamInferred = Result<const Type *, TypeArgInferrerParamInferredError>;
  using ResultIterableMatch = Result<const Type *, TypeArgInferrerIterableFitsError>;
  using ResultConstraintMet = Result<const Type *, TypeArgInferrerConstraintMetError>;
  using ResultCandidateJoin = Result<const Type *, TypeArgInferrerCandidatesJoinError>;
  using ResultCallbackParam = Result<const Type *, TypeArgInferrerCallbackParamError>;
  using ResultArgNotGrowing = Result<const Type *, TypeArgInferrerNotGrowingError>;
  using ResultCandidateType = Result<const Type *, TypeArgInferrerJoinCandidatesError>;

  ResultParamInferred paramInferred(const FuncSymbol *generic, const Type *bound, size_t index,
                                    TypeArgUse use) const;
  ResultIterableMatch iterableFits(const FuncSymbol *generic, const Type *bound, const Type *given,
                                   const Type *wanted) const;
  ResultConstraintMet constraintMet(const FuncSymbol *generic, std::vector<const Type *> &bound,
                                    size_t index);
  ResultTypeArgsCount typeArgCount(const FuncSymbol *generic,
                                   llvm::ArrayRef<TypeRef> typeArgs) const;
  ResultCandidateJoin candidatesJoin(const FuncSymbol *generic, const Type *param, const Type *type,
                                     const Type *candidate) const;
  ResultCallbackParam callbackParam(const FuncSymbol *generic, const Type *param, const Type *bound,
                                    bool isMentioned, const FunctionExpr *functionExpr,
                                    size_t position) const;
  ResultArgNotGrowing notGrowing(const FuncSymbol *generic, const Type *bound,
                                 const Type *param) const;
  ResultCandidateType joinCandidates(const GenericCallBinding &binding, const Inference &inference,
                                     size_t index);
  ResultCandidateType joinStrongCandidates(const GenericCallBinding &binding,
                                           const Inference &inference, size_t index) const;
  ResultCandidateType joinWeakCandidates(const GenericCallBinding &binding,
                                         const Inference &inference, size_t index,
                                         const Type *strong) const;
  ResultCandidateType joinLiteralCandidates(const GenericCallBinding &binding,
                                            const Inference &inference, size_t index,
                                            const Type *known) const;

  Inference::TypeOf asyncOfType() const;
  Inference::TypeOf elementOfType() const;
  bool isMakingResults(const Expr *arg) const;
  bool isBareGenericName(Expr *arg) const;
  bool isNeedingContext(const Expr *arg) const;
  bool isContextSensitive(const Expr *arg) const;
  void finishBinding(GenericCallBinding &binding);
  bool fillTypeDefaults(GenericCallBinding &binding);
  const Type *listRestOf(const FuncSymbol *generic) const;
  void walkUncheckedArguments(GenericCallBinding &binding);
  bool hasRejectedSpread(const GenericCallBinding &binding);
  bool hasSpecialProperty(const ObjectLitExpr *literal) const;
  bool hasUntypedParam(const FunctionDecl *functionDecl) const;
  bool isOtherNumber(const Type *type, const Type *candidate) const;
  std::string sampleArgs(llvm::ArrayRef<const Type *> params) const;
  bool hasArgumentErrors(const GenericCallBinding &binding) const;
  const Type *typeDefault(const FuncSymbol *generic, size_t index) const;
  void inferTypeArgs(GenericCallBinding &binding, const Type *expected);
  const Type *argumentPattern(const FuncSymbol *generic, size_t index) const;
  bool isGrowing(const Type *type, llvm::ArrayRef<const Type *> params) const;
  bool instantiateCall(GenericCallBinding &binding, ArgumentChecks checks);
  void inferFromExpected(GenericCallBinding &binding, const Type *expected);
  void inferFromListRest(GenericCallBinding &binding, Inference &inference);
  const Type *expectedCandidate(const Inference &fromExpected, size_t index) const;
  void settleWithCandidates(GenericCallBinding &binding, Inference &inference);
  const Type *parameterFor(const GenericCallBinding &binding, size_t index) const;
  void settleTypeParam(GenericCallBinding &binding, Inference &inference, size_t index);
  bool isConcreteCall(const FuncSymbol *generic, llvm::ArrayRef<const Type *> bound) const;
  void bindExplicitTypeArgs(GenericCallBinding &binding, llvm::ArrayRef<TypeRef> typeArgs);
  GenericArgumentForm argumentForm(const GenericCallBinding &binding, size_t index);
  void walkArgument(GenericCallBinding &binding, Inference &inference, size_t index);
  void walkArgumentNow(GenericCallBinding &binding, size_t index, const Type *hint);
  std::optional<const Type *> argumentCandidate(const Inference &inference, size_t index) const;
  void applyTypeDefault(const FuncSymbol *generic, std::vector<const Type *> &bound, size_t index);
  bool isFittingArgument(const FuncSymbol *generic, std::vector<const Type *> &bound, size_t index);
  void deferLiteral(GenericCallBinding &binding, Inference &inference, size_t index);
  Signature callbackSignature(llvm::ArrayRef<const Type *> typeParams, const Type *function) const;
  Expr *checkCallbackValue(Expr *arg, FuncSymbol *other, Signature &signature);
  const Type *withUndefined(const Inference &inference, size_t index, const Type *type) const;
  bool isGrowingAt(const Type *type, llvm::ArrayRef<const Type *> params, GrowthPlace place) const;
  void inferFromCheckedArgument(GenericCallBinding &binding, Inference &inference, size_t index);
  void walkContextualArgument(GenericCallBinding &binding, Inference &inference, size_t index);
  void walkCallbackArgument(GenericCallBinding &binding, size_t index, const Type *function);
  void walkCallbackParams(GenericCallBinding &binding, size_t index, const Type *function);
  bool isMentioningOpen(const Type *type, llvm::ArrayRef<const Type *> params,
                        llvm::ArrayRef<const Type *> bound) const;
  bool isPrecheckable(const Field *field, const Expr *value,
                      llvm::ArrayRef<const Type *> genericTypeParams) const;
  const Type *widenToConstraint(const GenericCallBinding &binding, const Inference &inference,
                                size_t index, const Type *type) const;
  void walkCallbackParam(GenericCallBinding &binding, size_t index, const Type *callbackParam,
                         size_t position);
  void inferFromLiteralText(GenericCallBinding &binding, Inference &inference, size_t index,
                            const StringLitExpr *text);
  void inferFromLiteralShape(GenericCallBinding &binding, Inference &inference, size_t index,
                             const Type *shape);
  void walkArgumentWithDefaults(GenericCallBinding &binding, size_t index, const Type *pattern,
                                const Type *partial);
  void adoptArgumentType(GenericCallBinding &binding, Inference &inference, size_t index,
                         const Type *pattern);
  bool fitIteratedArg(const FuncSymbol *generic, std::vector<const Type *> &bound, size_t index,
                      const Type *element, llvm::SMLoc loc);
  void precheckFields(ObjectLitExpr *literal, const Type *pattern,
                      llvm::ArrayRef<const Type *> genericTypeParams, Inference &inference);
  bool finishTypeArg(FuncSymbol *generic, std::vector<const Type *> &bound, size_t index,
                     llvm::SMLoc loc, TypeArgUse use, MissingTypeArgs missing);
  bool finishTypeArgsWith(FuncSymbol *generic, std::vector<const Type *> &bound, llvm::SMLoc loc,
                          TypeArgUse use, MissingTypeArgs missing);

  SemanticState &state;
  TypeParams *typeParams = nullptr;
  CallChecker *callChecker = nullptr;
  ExprChecker *exprChecker = nullptr;
  LateClasses *lateClasses = nullptr;
  ResultTypes *resultTypes = nullptr;
  TypeResolver *typeResolver = nullptr;
  UtilityTypes *utilityTypes = nullptr;
  LiteralTypes *literalTypes = nullptr;
  TypeRelations *typeRelations = nullptr;
  ClosureChecker *closureChecker = nullptr;
  GenericChecker *genericChecker = nullptr;
  ArgumentChecker *argumentChecker = nullptr;
  IterableChecker *iterableChecker = nullptr;
  GenericInstantiator *genericInstantiator = nullptr;
  FunctionValueChecker *functionValueChecker = nullptr;
};

}
