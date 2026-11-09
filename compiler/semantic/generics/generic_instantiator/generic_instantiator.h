#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/generics/generic_instantiator/generic_instantiator_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class TypeParams;
class LateClasses;
class TypeResolver;
class ArgListChecker;
class ReturnInferrer;
class FunctionChecker;
class TypeArgInferrer;
class GenericClassChecker;

class GenericInstantiator {
public:
  GenericInstantiator(SemanticState &owner, SemanticParts &all);

  Expr *checkGenericValue(NameExpr *name, FuncSymbol *generic, const Signature *expected);
  bool returnsOwnClass(const FuncSymbol *generic, const Type *ret);
  FuncSymbol *instantiate(FuncSymbol *generic, llvm::ArrayRef<const Type *> args, llvm::SMLoc loc,
                          llvm::ArrayRef<const Type *> exact = {});
  std::vector<const Type *> exactClassArgs(CallExpr *call, const FuncSymbol *generic);
  FunctionDecl *instanceDecl(FuncSymbol *generic, llvm::ArrayRef<const Type *> args,
                             llvm::ArrayRef<const Type *> exact = {});
  void checkInstance(InstanceRecord *instance);
  void reportInstance(InstanceRecord *instance, llvm::SMLoc callLoc,
                      llvm::ArrayRef<const Type *> concrete);
  void checkInstances();

private:
  using ResultTypeArgsCount = Result<size_t, GenericInstantiatorTypeArgCountError>;
  using ResultTypeArgsGiven = Result<const NameExpr *, GenericInstantiatorTypeArgsGivenError>;
  using ResultNoClassReturn = Result<const FuncSymbol *, GenericInstantiatorOwnClassReturnError>;
  using ResultChainIsFinite = Result<unsigned, GenericInstantiatorChainBoundedError>;
  using ResultParamsMeaning = Result<const FuncSymbol *, GenericInstantiatorParamsMeaningfulError>;

  static ResultTypeArgsCount typeArgCount(const FuncSymbol *generic, const NameExpr *name);
  static ResultTypeArgsGiven typeArgsGiven(const FuncSymbol *generic, const NameExpr *name,
                                           const Signature *expected);
  static ResultNoClassReturn ownClassReturn(const FuncSymbol *generic, const ClassDecl *cls);
  ResultChainIsFinite chainBounded(const FuncSymbol *generic,
                                   llvm::ArrayRef<const Type *> args) const;
  ResultParamsMeaning paramsMeaningful(const FuncSymbol *generic, llvm::ArrayRef<const Type *> reps,
                                       llvm::ArrayRef<const Type *> args);
  static const ClassDecl *literalClassIn(const Type *type);
  static std::string typeList(llvm::ArrayRef<const Type *> types);
  static std::string bindingList(llvm::ArrayRef<const Type *> params,
                                 llvm::ArrayRef<const Type *> args);

  SemanticState &state;
  TypeParams *typeParams = nullptr;
  LateClasses *lateClasses = nullptr;
  TypeResolver *typeResolver = nullptr;
  ArgListChecker *argListChecker = nullptr;
  ReturnInferrer *returnInferrer = nullptr;
  FunctionChecker *functionChecker = nullptr;
  TypeArgInferrer *typeArgInferrer = nullptr;
  GenericClassChecker *genericClassChecker = nullptr;
};

}
