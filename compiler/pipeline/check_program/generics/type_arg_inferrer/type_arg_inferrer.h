#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/generics/type_arg_inferrer/type_arg_inferrer_errors.h"
#include "compiler/result/result/result.h"

namespace tz {

struct Inference;

class TypeArgInferrer {
public:
  explicit TypeArgInferrer(CheckState &owner);

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
                      llvm::ArrayRef<const Type *> typeParams, Inference &inference);
  CheckState &state;
};

}
