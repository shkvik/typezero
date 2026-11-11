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
class ArgListChecker;
class ClosureChecker;
class EmitterChecker;
class IterableChecker;
class GeneratorChecker;
class PredicateChecker;
class TzModulesChecker;
class GenericClassChecker;

class FunctionChecker {
public:
  FunctionChecker(SemanticState &owner, SemanticParts &all);

  void walkFunctionBody(FunctionDecl *fn);
  void walkFunctionFromScratch(FunctionDecl *fn);
  void declareFunction(FunctionDecl *fn, VarSymbol *var = nullptr, bool scoped = true);

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

  static ResultThisParamAllowed thisParam(const FunctionDecl *fn);
  static ResultParamTypeIsKnown paramTypeKnown(const Param &param);
  static ResultTopLevelGenerics topLevelGeneric(const FunctionDecl *fn);
  static ResultRestParamIsArray restType(const Type *type, const Param &param);
  static ResultConstructorSuper superCalled(const FunctionDecl *fn, unsigned supers);
  static ResultParamOrderIsKept requiredOrder(const Param &param, bool afterOptional);
  static ResultEveryPathReturns returnsEverywhere(const FunctionDecl *fn, const Type *ret);
  static ResultDecoratorAllowed paramDecorators(const FunctionDecl *fn, const Param &param);
  static ResultDefaultUsesPrior defaultOrder(const FunctionDecl *fn, size_t i,
                                             const llvm::StringSet<> &names);
  bool isGenericDeclaration(const FunctionDecl *fn);
  void declareFunctionTypeParams(FunctionDecl *fn, FuncSymbol *sym, VarSymbol *var);
  void padHookParams(FunctionDecl *fn, const std::optional<std::vector<const Type *>> &hook);
  static bool hasRestParam(const FunctionDecl *fn);
  void declareFunctionParam(FunctionDecl *fn, FuncSymbol *sym, size_t i, const Type *hooked,
                            bool &afterOptional);
  const Type *declaredParamType(FunctionDecl *fn, FuncSymbol *sym, const Param &param,
                                const Type *hooked);
  const Type *writtenParamType(FunctionDecl *fn, FuncSymbol *sym, const Param &param,
                               const Type *hooked);
  void walkThisParam(const FunctionDecl *fn, FuncSymbol *sym, const Param &param);
  void declareFunctionReturn(FunctionDecl *fn, FuncSymbol *sym);
  void declareFunctionName(FunctionDecl *fn, FuncSymbol *sym, VarSymbol *var);
  void bindBodyTypeParams(FunctionDecl *fn);
  bool entersInitializer(FunctionDecl *fn);
  static bool isPresetField(const FunctionDecl *fn, const ClassField &f, bool derived);
  void walkRootSupers(FunctionDecl *fn);
  static CallExpr *superCallOf(Stmt *stmt);
  void declareBodyParam(FunctionDecl *fn, size_t i);
  bool defaultValueChecked(FunctionDecl *fn, size_t i, DefaultedParam *defaulted, const Type *type);
  void walkBodyStatements(FunctionDecl *fn);
  static const Type *bodyReturnType(const FunctionDecl *fn);

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
  ArgListChecker *argListChecker = nullptr;
  ClosureChecker *closureChecker = nullptr;
  EmitterChecker *emitterChecker = nullptr;
  IterableChecker *iterableChecker = nullptr;
  GeneratorChecker *generatorChecker = nullptr;
  PredicateChecker *predicateChecker = nullptr;
  TzModulesChecker *tzModulesChecker = nullptr;
  GenericClassChecker *genericClassChecker = nullptr;
};

}
