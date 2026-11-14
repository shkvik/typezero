#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/declaration/functions/declaration_functions_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include "llvm/ADT/StringSet.h"
#include <vector>

namespace tz {

class ChunkTypes;
class TypeParams;
class AsyncChecker;
class TypeResolver;
class SemanticParts;
class IterableChecker;
class GeneratorChecker;
class GenericClassChecker;

class DeclarationFunctionsRegistry {
public:
  DeclarationFunctionsRegistry(SemanticState &owner, SemanticParts &all);

  void declareSignature(FunctionDecl *functionDecl);
  void expandArgumentList(FunctionDecl *functionDecl, FuncSymbol *function);
  void declareFunction(FunctionDecl *functionDecl, VarSymbol *variable = nullptr);
  void declareItemFunction(Stmt *item, Module *module, std::vector<FunctionDecl *> &functions);


  bool isReturningValue(const Stmt *body) const;


  const Type *resolveReturnType(FunctionDecl *functionDecl, FuncSymbol *function);

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

  void declareSymbol(FunctionDecl *functionDecl, VarSymbol *variable);
  void reportAsserting(const FunctionDecl *functionDecl, const TypeRef &ref);
  void declareRestParam(FuncSymbol *function, Param &param, const Type *type);
  void declareWrittenReturn(FunctionDecl *functionDecl, FuncSymbol *function);
  void declareFunctionReturn(FunctionDecl *functionDecl, FuncSymbol *function);
  void walkThisParam(const FunctionDecl *functionDecl, FuncSymbol *function, const Param &param);
  void declareFunctionName(FunctionDecl *functionDecl, FuncSymbol *function, VarSymbol *variable);
  void padHookParams(FunctionDecl *functionDecl,
                     const std::optional<std::vector<const Type *>> &hook);
  void declareFunctionTypeParams(FunctionDecl *functionDecl, FuncSymbol *function,
                                 VarSymbol *variable);
  void declareFunctionParam(FunctionDecl *functionDecl, FuncSymbol *function, size_t index,
                            const Type *hooked);


  bool isGivingValue(const ReturnStmt *returnStmt) const;
  bool hasRestParam(const FunctionDecl *functionDecl) const;
  bool isGenericDeclaration(const FunctionDecl *functionDecl);
  bool hasOptionalBefore(const FunctionDecl *functionDecl, size_t index) const;
  bool isExpandableRest(const FunctionDecl *functionDecl, const FuncSymbol *function) const;


  Param listRestParam(const Param &rest, const Type *more);
  Param listPartParam(const Param &rest, const Type *list, size_t index);


  std::string callText(const FunctionDecl *functionDecl) const;
  std::string receiverText(const FunctionDecl *functionDecl) const;
  std::string shownPredicate(const TypeRef &ref, const Type *type) const;


  const Type *predicateType(const TypeRef &ref) const;
  const Type *resolvedParamType(FunctionDecl *functionDecl, FuncSymbol *function,
                                const Param &param);
  const Type *writtenParamType(FunctionDecl *functionDecl, FuncSymbol *function, const Param &param,
                               const Type *hooked);
  const Type *declaredParamType(FunctionDecl *functionDecl, FuncSymbol *function,
                                const Param &param, const Type *hooked);


  ResultParamTypeIsKnown checkParamTypeKnown(const Param &param) const;
  ResultThisParamAllowed checkThisParam(const FunctionDecl *functionDecl) const;
  ResultRestParamIsArray checkRestType(const Type *type, const Param &param) const;
  ResultTopLevelGenerics checkTopLevelGeneric(const FunctionDecl *functionDecl) const;
  ResultNotAsserting notAsserting(const FunctionDecl *functionDecl, const TypeRef &ref);
  ResultParamOrderIsKept checkRequiredOrder(const Param &param, bool isAfterOptional) const;
  ResultPredicateArg predicateArg(const FunctionDecl *functionDecl, const TypeRef &ref) const;
  std::optional<int> lastParamNamed(const FunctionDecl *functionDecl, llvm::StringRef name) const;
  ResultDecoratorAllowed checkParamDecorators(const FunctionDecl *functionDecl,
                                              const Param &param) const;

  SemanticState &state;
  ChunkTypes *chunkTypes = nullptr;
  TypeParams *typeParams = nullptr;
  AsyncChecker *asyncChecker = nullptr;
  TypeResolver *typeResolver = nullptr;
  IterableChecker *iterableChecker = nullptr;
  GeneratorChecker *generatorChecker = nullptr;
  GenericClassChecker *genericClassChecker = nullptr;
};

}
