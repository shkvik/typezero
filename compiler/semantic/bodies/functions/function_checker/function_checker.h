#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/functions/function_checker/function_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"
#include "llvm/ADT/StringSet.h"

namespace tz {

class SemanticParts;
class Scopes;
class ByteProofs;
class ChunkTypes;
class TypeParams;
class ExprChecker;
class FlowChecker;
class IndexProofs;
class LateClasses;
class AsyncChecker;
class ClassChecker;
class TypeResolver;
class TypeRelations;
class ReturnChecker;
class ArgListChecker;
class ClosureChecker;
class EmitterChecker;
class ReturnInferrer;
class IterableChecker;
class GeneratorChecker;
class PredicateChecker;
class TzModulesChecker;
class GenericClassChecker;

class FunctionChecker {
public:
  FunctionChecker(SemanticState &owner, SemanticParts &all);

  void walkFunctionBody(FunctionDecl *functionDecl);
  void walkFunctionFromScratch(FunctionDecl *functionDecl);
  void declareSignature(FunctionDecl *functionDecl);
  void declareFunction(FunctionDecl *functionDecl, VarSymbol *variable = nullptr);

private:
  using ResultRestParamIsArray = Result<const Type *, FunctionCheckerRestTypeError>;
  using ResultParamOrderIsKept = Result<const Param *, FunctionCheckerRequiredOrderError>;
  using ResultParamTypeIsKnown = Result<const Param *, FunctionCheckerParamTypeKnownError>;
  using ResultDecoratorAllowed = Result<const Param *, FunctionCheckerParamDecoratorsError>;
  using ResultThisParamAllowed = Result<const FunctionDecl *, FunctionCheckerThisParamError>;
  using ResultConstructorSuper = Result<const FunctionDecl *, FunctionCheckerSuperCalledError>;
  using ResultDefaultUsesPrior = Result<const FunctionDecl *, FunctionCheckerDefaultOrderError>;
  using ResultTopLevelGenerics = Result<const FunctionDecl *, FunctionCheckerTopLevelGenericError>;
  using ResultEveryPathReturns =
      Result<const FunctionDecl *, FunctionCheckerReturnsEverywhereError>;

  ResultThisParamAllowed thisParam(const FunctionDecl *functionDecl) const;
  ResultParamTypeIsKnown paramTypeKnown(const Param &param) const;
  ResultTopLevelGenerics topLevelGeneric(const FunctionDecl *functionDecl) const;
  ResultRestParamIsArray restType(const Type *type, const Param &param) const;
  ResultConstructorSuper superCalled(const FunctionDecl *functionDecl, unsigned supers) const;
  ResultParamOrderIsKept requiredOrder(const Param &param, bool afterOptional) const;
  ResultEveryPathReturns returnsEverywhere(const FunctionDecl *functionDecl,
                                           const Type *returnType) const;
  ResultDecoratorAllowed paramDecorators(const FunctionDecl *functionDecl,
                                         const Param &param) const;
  ResultDefaultUsesPrior defaultOrder(const FunctionDecl *functionDecl, size_t index,
                                      const llvm::StringSet<> &names) const;
  bool isGenericDeclaration(const FunctionDecl *functionDecl);
  void declareFunctionTypeParams(FunctionDecl *functionDecl, FuncSymbol *function,
                                 VarSymbol *variable);
  void padHookParams(FunctionDecl *functionDecl,
                     const std::optional<std::vector<const Type *>> &hook);
  bool hasRestParam(const FunctionDecl *functionDecl) const;
  void declareFunctionParam(FunctionDecl *functionDecl, FuncSymbol *function, size_t index,
                            const Type *hooked, bool &afterOptional);
  const Type *declaredParamType(FunctionDecl *functionDecl, FuncSymbol *function,
                                const Param &param, const Type *hooked);
  const Type *writtenParamType(FunctionDecl *functionDecl, FuncSymbol *function, const Param &param,
                               const Type *hooked);
  void walkThisParam(const FunctionDecl *functionDecl, FuncSymbol *function, const Param &param);
  void declareFunctionReturn(FunctionDecl *functionDecl, FuncSymbol *function);
  void declareSymbol(FunctionDecl *functionDecl, VarSymbol *variable);
  void declareFunctionName(FunctionDecl *functionDecl, FuncSymbol *function, VarSymbol *variable);
  void bindBodyTypeParams(FunctionDecl *functionDecl);
  bool entersInitializer(FunctionDecl *functionDecl);
  bool isPresetField(const FunctionDecl *functionDecl, const ClassField &field, bool derived) const;
  void walkRootSupers(FunctionDecl *functionDecl);
  CallExpr *superCallOf(Stmt *stmt) const;
  void declareBodyParam(FunctionDecl *functionDecl, size_t index);
  bool defaultValueChecked(FunctionDecl *functionDecl, size_t index, DefaultedParam *defaulted,
                           const Type *type);
  void walkBodyStatements(FunctionDecl *functionDecl);
  const Type *bodyReturnType(const FunctionDecl *functionDecl) const;

  SemanticState &state;
  Scopes *scopes = nullptr;
  ByteProofs *byteProofs = nullptr;
  ChunkTypes *chunkTypes = nullptr;
  TypeParams *typeParams = nullptr;
  ExprChecker *exprChecker = nullptr;
  FlowChecker *flowChecker = nullptr;
  IndexProofs *indexProofs = nullptr;
  LateClasses *lateClasses = nullptr;
  AsyncChecker *asyncChecker = nullptr;
  ClassChecker *classChecker = nullptr;
  TypeResolver *typeResolver = nullptr;
  TypeRelations *typeRelations = nullptr;
  ReturnChecker *returnChecker = nullptr;
  ArgListChecker *argListChecker = nullptr;
  ClosureChecker *closureChecker = nullptr;
  EmitterChecker *emitterChecker = nullptr;
  ReturnInferrer *returnInferrer = nullptr;
  IterableChecker *iterableChecker = nullptr;
  GeneratorChecker *generatorChecker = nullptr;
  PredicateChecker *predicateChecker = nullptr;
  TzModulesChecker *tzModulesChecker = nullptr;
  GenericClassChecker *genericClassChecker = nullptr;
};

}
