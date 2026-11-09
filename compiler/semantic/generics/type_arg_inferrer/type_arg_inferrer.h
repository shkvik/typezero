#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/generics/type_arg_inferrer/type_arg_inferrer_errors.h"
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
class ReturnInferrer;
class ArgumentChecker;
class IterableChecker;
class GenericInstantiator;
class FunctionValueChecker;

class Inference;

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
  static bool isContextSensitive(const Expr *arg);

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
  ReturnInferrer *returnInferrer = nullptr;
  ArgumentChecker *argumentChecker = nullptr;
  IterableChecker *iterableChecker = nullptr;
  GenericInstantiator *genericInstantiator = nullptr;
  FunctionValueChecker *functionValueChecker = nullptr;
};

}
