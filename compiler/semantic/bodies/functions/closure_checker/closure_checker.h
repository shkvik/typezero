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

  Signature signatureOf(const Type *functionType) const;
  Expr *checkFunctionExpr(FunctionExpr *functionExpr, const Signature *expected);
  void walkClosureBody(FunctionDecl *functionDecl);
  void declareOwnName(FunctionDecl *functionDecl);
  void walkStatements(BlockStmt *block);

private:
  using ResultExprNotGeneric = Result<const FunctionDecl *, ClosureCheckerNotGenericError>;
  using ResultParamTypeAgree = Result<const Type *, ClosureCheckerParamAgreesError>;
  using ResultDefaultMatters = Result<const Param *, ClosureCheckerDefaultMattersError>;
  using ResultParamTypeKnown = Result<const Param *, ClosureCheckerParamTypeKnownError>;
  using ResultThisParamValid = Result<const Param *, ClosureCheckerThisParamError>;
  using ResultRestParamArray = Result<const Type *, ClosureCheckerRestParamError>;
  using ResultParamOrderGood = Result<const Param *, ClosureCheckerParamOrderError>;
  using ResultPromiseAwaited = Result<const Signature *, ClosureCheckerPromiseAwaitedError>;

  ResultExprNotGeneric notGeneric(const FunctionDecl *functionDecl) const;
  ResultParamTypeAgree paramAgrees(const Param &param, const Type *passed, const Type *given) const;
  ResultDefaultMatters defaultMatters(const Param &param, const Type *given) const;
  ResultParamTypeKnown paramTypeKnown(const Param &param) const;
  ResultThisParamValid thisParam(const FunctionDecl *functionDecl, const Param &param) const;
  ResultRestParamArray restParam(const Param &param, const Type *type) const;
  ResultParamOrderGood paramOrder(const Param &param, bool afterOptional) const;
  ResultPromiseAwaited promiseAwaited(const Signature *signature) const;

  struct ClosureParams {
    FuncSymbol *sym;
    bool thisFromContext;
    bool afterOptional;
  };

  struct ParamTypes {
    const Type *type;
    const Type *passed;
  };

  FuncSymbol *makeClosureSymbol(FunctionDecl *functionDecl);
  bool reportsGenericClosure(FunctionExpr *functionExpr);
  bool addsContextThis(FunctionExpr *functionExpr, const Signature *signature);
  void walkClosureParam(FunctionDecl *functionDecl, size_t index, const Signature *signature,
                        ClosureParams &params);
  ParamTypes closureParamTypes(const Param &param, const Type *given);
  ParamTypes writtenParamTypes(const Param &param, const Type *given);
  void walkClosureThis(const FunctionDecl *functionDecl, const Param &param, bool mayOmit,
                       ClosureParams &params);
  void padHiddenParams(FunctionExpr *functionExpr, FuncSymbol *function,
                       const Signature *signature);
  void walkClosureReturn(FunctionExpr *functionExpr, FuncSymbol *function,
                         const Signature *signature);
  void walkGeneratorReturn(FunctionDecl *functionDecl, FuncSymbol *function,
                           const Signature *signature);
  void walkAsyncReturn(FunctionExpr *functionExpr, FuncSymbol *function,
                       const Signature *signature);
  void walkPlainReturn(FunctionDecl *functionDecl, FuncSymbol *function,
                       const Signature *signature);
  void inferRecursiveReturn(FunctionDecl *functionDecl, FuncSymbol *function);
  bool isRecursiveClosure(FunctionDecl *functionDecl);
  void walkInitializerBody(FunctionDecl *functionDecl);

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
