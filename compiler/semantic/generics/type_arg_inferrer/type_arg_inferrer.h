#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/generics/type_arg_inferrer/type_arg_inferrer_errors.h"
#include "compiler/semantic/generics/type_arg_inferrer/type_arg_inferrer_types.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class TypeParams;
class CallChecker;
class ExprChecker;
class LateClasses;
class ResultTypes;
class TypeResolver;
class UtilityTypes;
class ClosureChecker;
class GenericChecker;
class ArgumentChecker;
class IterableChecker;
class GenericInstantiator;
class FunctionValueChecker;

class Inference;
struct Signature;

class TypeArgInferrer {
public:
  TypeArgInferrer(SemanticState &owner, SemanticParts &all);

  bool bindGenericCall(CallExpr *call, FuncSymbol *generic, llvm::SMLoc loc, const Type *expected,
                       llvm::ArrayRef<TypeRef> typeArgs, std::vector<const Type *> &bound);
  Expr *checkGenericCall(CallExpr *call, FuncSymbol *generic, llvm::SMLoc loc, const Type *expected,
                         llvm::ArrayRef<TypeRef> typeArgs);
  bool finishTypeArgs(FuncSymbol *generic, std::vector<const Type *> &bound, llvm::SMLoc loc,
                      bool call, bool quiet = false);
  bool inferFromArguments(FuncSymbol *generic, llvm::ArrayRef<Expr *> args,
                          std::vector<const Type *> &bound);

private:
  using ResultParamInferred = Result<const Type *, TypeArgInferrerParamInferredError>;
  using ResultIterableMatch = Result<const Type *, TypeArgInferrerIterableFitsError>;
  using ResultConstraintMet = Result<const Type *, TypeArgInferrerConstraintMetError>;
  using ResultTypeArgsCount = Result<size_t, TypeArgInferrerTypeArgCountError>;
  using ResultCandidateJoin = Result<const Type *, TypeArgInferrerCandidatesJoinError>;
  using ResultCallbackParam = Result<const Type *, TypeArgInferrerCallbackParamError>;
  using ResultArgNotGrowing = Result<const Type *, TypeArgInferrerNotGrowingError>;
  using ResultCandidateType = Result<const Type *, TypeArgInferrerJoinCandidatesError>;

  static ResultParamInferred paramInferred(const FuncSymbol *generic, const Type *bound, size_t k,
                                           bool call);
  static ResultIterableMatch iterableFits(const FuncSymbol *generic, const Type *bound,
                                          const Type *given, const Type *wanted);
  ResultConstraintMet constraintMet(const FuncSymbol *generic, std::vector<const Type *> &bound,
                                    size_t k);
  static ResultTypeArgsCount typeArgCount(const FuncSymbol *generic,
                                          llvm::ArrayRef<TypeRef> typeArgs);
  static ResultCandidateJoin candidatesJoin(const FuncSymbol *generic, const Type *param,
                                            const Type *type, const Type *candidate);
  static ResultCallbackParam callbackParam(const FuncSymbol *generic, const Type *param,
                                           const Type *bound, bool mentioned,
                                           const FunctionExpr *fe, size_t p);
  static ResultArgNotGrowing notGrowing(const FuncSymbol *generic, const Type *bound,
                                        const Type *param);
  static bool grows(const Type *type, llvm::ArrayRef<const Type *> params, bool inside);
  static bool growsHere(const Type *type, llvm::ArrayRef<const Type *> params, bool inside);
  static bool isContextSensitive(const Expr *arg);
  static bool needsContext(const Expr *arg);

  static const Type *listRestOf(const FuncSymbol *generic);
  static const Type *typeDefault(const FuncSymbol *generic, size_t k);
  static const Type *parameterFor(const GenericCallBinding &binding, size_t i);
  static Signature callbackSignature(llvm::ArrayRef<const Type *> typeParams, const Type *function);
  static const Type *widenToConstraint(const GenericCallBinding &binding,
                                       const Inference &inference, size_t k, const Type *type);
  static const Type *withUndefined(const Inference &inference, size_t k, const Type *type);
  bool makesResults(const Expr *arg) const;
  bool isBareGenericName(Expr *arg) const;
  bool hasRejectedSpread(const GenericCallBinding &binding);
  bool hasArgumentErrors(const GenericCallBinding &binding) const;
  void finishBinding(GenericCallBinding &binding);
  void walkUncheckedArguments(GenericCallBinding &binding);
  bool instantiateCall(GenericCallBinding &binding, bool inferredArgs);
  void inferTypeArgs(GenericCallBinding &binding, const Type *expected);
  void inferFromExpected(GenericCallBinding &binding, const Type *expected);
  void inferFromListRest(GenericCallBinding &binding, Inference &inference);
  void settleWithCandidates(GenericCallBinding &binding, Inference &inference);
  void settleTypeParam(GenericCallBinding &binding, Inference &inference, size_t k);
  void bindExplicitTypeArgs(GenericCallBinding &binding, llvm::ArrayRef<TypeRef> typeArgs);
  GenericArgumentForm argumentForm(const GenericCallBinding &binding, size_t i);
  void walkArgument(GenericCallBinding &binding, Inference &inference, size_t i);
  void walkArgumentNow(GenericCallBinding &binding, size_t i, const Type *hint);
  void inferFromLiteralText(GenericCallBinding &binding, Inference &inference, size_t i,
                            const StringLitExpr *text);
  void inferFromLiteralShape(GenericCallBinding &binding, Inference &inference, size_t i,
                             const Type *shape);
  void deferLiteral(GenericCallBinding &binding, Inference &inference, size_t i);
  void inferFromCheckedArgument(GenericCallBinding &binding, Inference &inference, size_t i);
  void walkContextualArgument(GenericCallBinding &binding, Inference &inference, size_t i);
  void walkCallbackArgument(GenericCallBinding &binding, size_t i, const Type *function);
  Expr *checkCallbackValue(Expr *arg, FuncSymbol *other, Signature &signature);
  void walkCallbackParams(GenericCallBinding &binding, size_t i, const Type *function);
  void walkArgumentWithDefaults(GenericCallBinding &binding, size_t i, const Type *pattern,
                                const Type *partial);
  void adoptArgumentType(GenericCallBinding &binding, Inference &inference, size_t i,
                         const Type *pattern);
  ResultCandidateType joinCandidates(const GenericCallBinding &binding, const Inference &inference,
                                     size_t k);

  void precheckFields(ObjectLitExpr *lit, const Type *pattern,
                      llvm::ArrayRef<const Type *> genericTypeParams, Inference &inference);
  SemanticState &state;
  TypeParams *typeParams = nullptr;
  CallChecker *callChecker = nullptr;
  ExprChecker *exprChecker = nullptr;
  LateClasses *lateClasses = nullptr;
  ResultTypes *resultTypes = nullptr;
  TypeResolver *typeResolver = nullptr;
  UtilityTypes *utilityTypes = nullptr;
  ClosureChecker *closureChecker = nullptr;
  GenericChecker *genericChecker = nullptr;
  ArgumentChecker *argumentChecker = nullptr;
  IterableChecker *iterableChecker = nullptr;
  GenericInstantiator *genericInstantiator = nullptr;
  FunctionValueChecker *functionValueChecker = nullptr;
};

}
