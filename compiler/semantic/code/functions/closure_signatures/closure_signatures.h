#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/code/functions/code_functions_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include <optional>

namespace tz {

class ClosureSignatures {
public:
  explicit ClosureSignatures(SemanticState &owner);

  void nextClosureParam(const NextClosureParam &task);
  void checkFunctionExpr(const CheckFunctionExpr &task);
  void afterClosureParamType(const AfterClosureParamType &task);
  void afterClosureReturnType(const AfterClosureReturnType &task);

private:
  struct ParamTypes {
    const Type *type;
    const Type *passed;
  };

  using ResultRestParamArray = Result<const Type *, CodeFunctionsRegistryRestParamError>;
  using ResultParamOrderGood = Result<const Param *, CodeFunctionsRegistryParamOrderError>;
  using ResultParamTypeAgree = Result<const Type *, CodeFunctionsRegistryParamAgreesError>;
  using ResultDefaultMatters = Result<const Param *, CodeFunctionsRegistryDefaultMattersError>;
  using ResultThisParamValid = Result<const Param *, CodeFunctionsRegistryClosureThisParamError>;
  using ResultExprNotGeneric = Result<const FunctionDecl *, CodeFunctionsRegistryNotGenericError>;
  using ResultPromiseAwaited = Result<const Signature *, CodeFunctionsRegistryPromiseAwaitedError>;
  using ResultParamTypeKnown =
      Result<const Param *, CodeFunctionsRegistryClosureParamTypeKnownError>;

  void padHiddenParams(const ClosureDraft &draft);
  void startAsyncReturn(const ClosureDraft &draft);
  void startPlainReturn(const ClosureDraft &draft);
  void finishClosureParams(const ClosureDraft &draft);
  void startGeneratorReturn(const ClosureDraft &draft);
  void resolveClosureReturn(const ClosureDraft &draft);
  void expandArgumentList(FunctionDecl *functionDecl, FuncSymbol *function);
  void declareClosureParam(const ClosureDraft &draft, const ParamTypes &types);
  void walkClosureRest(FuncSymbol *function, const Param &param, const Type *type);
  void walkClosureThis(const ClosureDraft &draft, const Param &param, bool isParamOmittable);
  void walkRequiredParam(const FunctionDecl *functionDecl, FuncSymbol *function, size_t index);


  bool isOmittableParam(const Param &param) const;
  bool hasOmittableBefore(const FunctionDecl *functionDecl, size_t index) const;
  bool isExpandableRest(const FunctionDecl *functionDecl, const FuncSymbol *function) const;
  bool addContextThis(FunctionExpr *functionExpr, const std::optional<Signature> &signature);


  Param listRestParam(const Param &rest, const Type *more);
  Param listPartParam(const Param &rest, const Type *list, size_t index);


  const Type *givenParam(const ClosureDraft &draft) const;
  FuncSymbol *makeClosureSymbol(FunctionDecl *functionDecl);
  ParamTypes givenParamTypes(const Param &param, const Type *given);
  ParamTypes unwrittenParamTypes(const Param &param, const Type *given);
  const Type *givenInnerType(const Param &param, const Type *given) const;
  ParamTypes writtenParamTypes(const Param &param, const Type *written, const Type *given);


  ResultParamTypeKnown checkParamTypeKnown(const Param &param) const;
  ResultPromiseAwaited checkPromiseAwaited(const Signature &signature) const;
  ResultExprNotGeneric checkNotGeneric(const FunctionDecl *functionDecl) const;
  ResultRestParamArray checkRestParam(const Param &param, const Type *type) const;
  ResultParamOrderGood checkParamOrder(const Param &param, bool isAfterOptional) const;
  ResultDefaultMatters checkDefaultMatters(const Param &param, const Type *given) const;
  ResultThisParamValid checkThisParam(const FunctionDecl *functionDecl, const Param &param) const;
  ResultParamTypeAgree checkParamAgrees(const Param &param, const Type *passed,
                                        const Type *given) const;

  SemanticState &state;
};

}
