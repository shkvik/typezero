#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/functions/closure_checker/closure_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class Scopes;
class TypeParams;
class StmtChecker;
class AsyncChecker;
class ClassChecker;
class TypeDeclarer;
class TypeResolver;
class ArgListChecker;
class ReturnInferrer;
class FunctionChecker;
class NestedFunctions;
class GeneratorChecker;
class PredicateChecker;

class ClosureChecker {
public:
  ClosureChecker(SemanticState &owner, SemanticParts &all);

  static Signature signatureOf(const Type *fnType);
  Expr *checkFunctionExpr(FunctionExpr *fe, const Signature *expected);
  void checkClosureBody(FunctionDecl *fn);
  void declareOwnName(FunctionDecl *fn);
  void checkStatements(BlockStmt *block);

private:
  using ResultExprNotGeneric = Result<const FunctionDecl *, ClosureCheckerNotGenericError>;
  using ResultParamTypeAgree = Result<const Type *, ClosureCheckerParamAgreesError>;
  using ResultDefaultMatters = Result<const Param *, ClosureCheckerDefaultMattersError>;
  using ResultParamTypeKnown = Result<const Param *, ClosureCheckerParamTypeKnownError>;
  using ResultThisParamValid = Result<const Param *, ClosureCheckerThisParamError>;
  using ResultRestParamArray = Result<const Type *, ClosureCheckerRestParamError>;
  using ResultParamOrderGood = Result<const Param *, ClosureCheckerParamOrderError>;
  using ResultPromiseAwaited = Result<const Signature *, ClosureCheckerPromiseAwaitedError>;

  static ResultExprNotGeneric notGeneric(const FunctionDecl *fn);
  static ResultParamTypeAgree paramAgrees(const Param &param, const Type *passed,
                                          const Type *given);
  static ResultDefaultMatters defaultMatters(const Param &param, const Type *given);
  static ResultParamTypeKnown paramTypeKnown(const Param &param);
  static ResultThisParamValid thisParam(const FunctionDecl *fn, const Param &param);
  static ResultRestParamArray restParam(const Param &param, const Type *type);
  static ResultParamOrderGood paramOrder(const Param &param, bool afterOptional);
  static ResultPromiseAwaited promiseAwaited(const Signature *sig);

  SemanticState &state;
  Scopes *scopes = nullptr;
  TypeParams *typeParams = nullptr;
  StmtChecker *stmtChecker = nullptr;
  AsyncChecker *asyncChecker = nullptr;
  ClassChecker *classChecker = nullptr;
  TypeDeclarer *typeDeclarer = nullptr;
  TypeResolver *typeResolver = nullptr;
  ArgListChecker *argListChecker = nullptr;
  ReturnInferrer *returnInferrer = nullptr;
  FunctionChecker *functionChecker = nullptr;
  NestedFunctions *nestedFunctions = nullptr;
  GeneratorChecker *generatorChecker = nullptr;
  PredicateChecker *predicateChecker = nullptr;
};

}
