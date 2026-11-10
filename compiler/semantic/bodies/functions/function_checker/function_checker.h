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

  void checkFunctionBody(FunctionDecl *fn);
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
