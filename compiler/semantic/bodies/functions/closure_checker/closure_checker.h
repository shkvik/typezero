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

  struct ClosureParams {
    FuncSymbol *function = nullptr;
    bool isThisFromContext = false;
  };

  struct ParamTypes {
    const Type *type;
    const Type *passed;
  };

  ResultExprNotGeneric checkNotGeneric(const FunctionDecl *functionDecl) const;
  ResultParamTypeAgree checkParamAgrees(const Param &param, const Type *passed,
                                        const Type *given) const;
  ResultDefaultMatters checkDefaultMatters(const Param &param, const Type *given) const;
  ResultParamTypeKnown checkParamTypeKnown(const Param &param) const;
  ResultThisParamValid checkThisParam(const FunctionDecl *functionDecl, const Param &param) const;
  ResultRestParamArray checkRestParam(const Param &param, const Type *type) const;
  ResultParamOrderGood checkParamOrder(const Param &param, bool isAfterOptional) const;
  ResultPromiseAwaited checkPromiseAwaited(const Signature *signature) const;
  FuncSymbol *makeClosureSymbol(FunctionDecl *functionDecl);
  bool reportGenericClosure(FunctionExpr *functionExpr);
  bool addContextThis(FunctionExpr *functionExpr, const Signature *signature);
  void walkClosureParam(FunctionDecl *functionDecl, size_t index, const Signature *signature,
                        const ClosureParams &params);
  bool isOmittableParam(const Param &param) const;
  void walkClosureRest(FuncSymbol *function, const Param &param, const Type *type);
  void walkRequiredParam(const FunctionDecl *functionDecl, FuncSymbol *function, size_t index);
  bool hasOmittableBefore(const FunctionDecl *functionDecl, size_t index) const;
  ParamTypes closureParamTypes(const Param &param, const Type *given);
  ParamTypes givenParamTypes(const Param &param, const Type *given);
  const Type *givenInnerType(const Param &param, const Type *given) const;
  ParamTypes writtenParamTypes(const Param &param, const Type *given);
  void walkClosureThis(const FunctionDecl *functionDecl, const Param &param, bool isParamOmittable,
                       const ClosureParams &params);
  void padHiddenParams(FunctionExpr *functionExpr, FuncSymbol *function,
                       const Signature *signature);
  void walkClosureReturn(FunctionExpr *functionExpr, FuncSymbol *function,
                         const Signature *signature);
  void walkGeneratorReturn(FunctionDecl *functionDecl, FuncSymbol *function,
                           const Signature *signature);
  void walkWrittenGeneratorReturn(FunctionDecl *functionDecl, FuncSymbol *function);
  void walkAsyncReturn(FunctionExpr *functionExpr, FuncSymbol *function,
                       const Signature *signature);
  void walkPlainReturn(FunctionDecl *functionDecl, FuncSymbol *function,
                       const Signature *signature);
  void inferRecursiveReturn(FunctionDecl *functionDecl, FuncSymbol *function);
  bool isRecursiveClosure(FunctionDecl *functionDecl);
  void walkInitializerBody(FunctionDecl *functionDecl);
  void keepConstantFlow(const Flow &outer);
  bool isFixedTerm(const Term &term) const;
  BlockNames blockNames(const BlockStmt *block) const;
  void addDeclaratorNames(const VarDeclStmt *varDecl, BlockNames &names) const;
  void markGroupCreated(NestedGroup *group);
  bool isDecidedEnd(const BlockStmt *block, size_t index, const NestedGroup *group) const;
  bool isDeclarationStmt(const Stmt *stmt) const;

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
