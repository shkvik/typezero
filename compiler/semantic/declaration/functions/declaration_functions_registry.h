#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/declaration/functions/declaration_functions_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include "llvm/ADT/StringSet.h"
#include <vector>

namespace tz {

class DeclarationFunctionsRegistry {
public:
  explicit DeclarationFunctionsRegistry(SemanticState &owner);

  void startReturnType(const StartReturnType &task);
  void finishMethodReturn(const FinishMethodReturn &task);
  void declareMethodReturn(const DeclareMethodReturn &task);
  void declareItemFunction(Stmt *item, Module *module);
  void startFunctionParam(const StartFunctionParam &task);
  void finishAssertsReturn(const FinishAssertsReturn &task);
  void finishFunctionParam(const FinishFunctionParam &task);
  void startFunctionParams(const StartFunctionParams &task);
  void startFunctionReturn(const StartFunctionReturn &task);
  void startFunctionSymbol(const StartFunctionSymbol &task);
  void finishFunctionReturn(const FinishFunctionReturn &task);
  void finishFunctionSymbol(const FinishFunctionSymbol &task);
  void finishPredicateReturn(const FinishPredicateReturn &task);
  void expandArgumentList(FunctionDecl *functionDecl, FuncSymbol *function);


  bool isReturningValue(const Stmt *body) const;


  const Type *declaredReturnType(const FunctionDecl *functionDecl) const;

private:
  using ResultPredicateArg = Result<int, DeclarationFunctionsRegistryPredicateArgError>;
  using ResultRestParamIsArray = Result<const Type *, DeclarationFunctionsRegistryRestTypeError>;
  using ResultNotAsserting = Result<const TypeRef *, DeclarationFunctionsRegistryNotAssertingError>;
  using ResultParamOrderIsKept =
      Result<const Param *, DeclarationFunctionsRegistryRequiredOrderError>;
  using ResultParamTypeIsKnown =
      Result<const Param *, DeclarationFunctionsRegistryParamTypeKnownError>;
  using ResultDecoratorAllowed =
      Result<const Param *, DeclarationFunctionsRegistryParamDecoratorsError>;
  using ResultThisParamAllowed =
      Result<const FunctionDecl *, DeclarationFunctionsRegistryThisParamError>;
  using ResultTopLevelGenerics =
      Result<const FunctionDecl *, DeclarationFunctionsRegistryTopLevelGenericError>;

  void pushPredicateType(TypeRef &ref);
  void padHookParams(FunctionDecl *functionDecl);
  void pushFunctionTypeParams(const FunctionSymbolDraft &draft);
  void declareRestParam(FuncSymbol *function, Param &param, const Type *type);
  void placeFunctionParam(const FunctionSymbolDraft &draft, size_t index, const Type *declared);
  void walkThisParam(const FunctionDecl *functionDecl, FuncSymbol *function, const Param &param);
  void declareFunctionName(FunctionDecl *functionDecl, FuncSymbol *function, VarSymbol *variable);


  bool isGivingValue(const ReturnStmt *returnStmt) const;
  bool hasRestParam(const FunctionDecl *functionDecl) const;
  bool isGenericDeclaration(const FunctionDecl *functionDecl) const;
  bool hasOptionalBefore(const FunctionDecl *functionDecl, size_t index) const;
  bool isExpandableRest(const FunctionDecl *functionDecl, const FuncSymbol *function) const;


  Param listRestParam(const Param &rest, const Type *more);
  Param listPartParam(const Param &rest, const Type *list, size_t index);


  std::string callText(const FunctionDecl *functionDecl) const;
  std::string receiverText(const FunctionDecl *functionDecl) const;
  std::string shownPredicate(const TypeRef &ref, const Type *type) const;


  const Type *predicateType(const TypeRef &ref) const;
  const Type *unwrittenParamType(const Param &param, const Type *hooked);
  const Type *hookedParam(const FunctionDecl *functionDecl, size_t index) const;


  ResultParamTypeIsKnown checkParamTypeKnown(const Param &param) const;
  ResultThisParamAllowed checkThisParam(const FunctionDecl *functionDecl) const;
  ResultRestParamIsArray checkRestType(const Type *type, const Param &param) const;
  ResultTopLevelGenerics checkTopLevelGeneric(const FunctionDecl *functionDecl) const;
  ResultParamOrderIsKept checkRequiredOrder(const Param &param, bool isAfterOptional) const;
  ResultNotAsserting notAsserting(const FunctionDecl *functionDecl, const TypeRef &ref) const;
  ResultPredicateArg predicateArg(const FunctionDecl *functionDecl, const TypeRef &ref) const;
  std::optional<int> lastParamNamed(const FunctionDecl *functionDecl, llvm::StringRef name) const;
  ResultDecoratorAllowed checkParamDecorators(const FunctionDecl *functionDecl,
                                              const Param &param) const;

  SemanticState &state;
};

}
