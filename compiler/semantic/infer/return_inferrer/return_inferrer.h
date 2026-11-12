#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/infer/return_inferrer/return_inferrer_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"
#include <vector>

namespace tz {

class SemanticParts;
class ClassLinks;
class TypeParams;
class LateClasses;
class AsyncChecker;
class ImportBinder;
class TypeRelations;
class ReturnChecker;
class FunctionChecker;
class GeneratorChecker;
class GenericClassChecker;

struct BodyState {
  Flow flow;
  ScopeSnapshot scopeStack;
  FunctionSnapshot functionStack;
  BlockSnapshot blockPosition;
  Module *module = nullptr;
  InstanceRecord *instance = nullptr;
};

struct BodyCheck {
  BodyState saved;
  InstanceRecord *lateOwner = nullptr;
};

class ReturnInferrer {
public:
  ReturnInferrer(SemanticState &owner, SemanticParts &all);

  void leaveBody(BodyState &saved);
  bool isReturningValue(const Stmt *body) const;
  void inferReturnType(FuncSymbol *function);
  void walkBodyNow(FunctionDecl *functionDecl);
  const Type *returnType(FuncSymbol *function);
  void inferFromBody(FunctionDecl *functionDecl);
  const Type *functionType(FuncSymbol *function);
  BodyState enterBody(const FunctionDecl *functionDecl);
  void inferCycle(llvm::ArrayRef<FuncSymbol *> cycle);
  FunctionDecl *freshCopy(const FunctionDecl *functionDecl);

private:
  enum class CycleStep {
    KEPT,
    CHANGED,
    FAILED,
  };

  struct CollectedReturns {
    std::vector<ReturnStmt *> returns;
    std::vector<YieldExpr *> yields;
  };

  using ResultCycleTypeKnown = Result<const Type *, ReturnInferrerCycleTypeError>;
  using ResultReturnsUnified = Result<const Type *, ReturnInferrerCommonReturnError>;
  using ResultNotCalledEarly = Result<const FuncSymbol *, ReturnInferrerEarlyCallError>;
  using ResultReturnsDefined = Result<const FuncSymbol *, ReturnInferrerDefinedReturnError>;

  ResultNotCalledEarly earlyCall(const FuncSymbol *function) const;
  ResultCycleTypeKnown cycleType(const FuncSymbol *function, int cycleSize, bool isFailed) const;
  ResultReturnsUnified commonReturn(const FuncSymbol *function, const Type *before,
                                    const ReturnStmt *returnStmt) const;
  ResultReturnsDefined definedReturn(const FuncSymbol *function,
                                     llvm::ArrayRef<ReturnStmt *> returns, bool isUndefined) const;

  bool isSettled(const Type *type) const;
  void finishBodyCheck(BodyCheck &check);
  CycleStep nextAssumption(FuncSymbol *function);
  BodyCheck startBodyCheck(FuncSymbol *function);
  const Type *trialReturn(FuncSymbol *function);
  const Type *templateReturn(FuncSymbol *function);
  bool isTemplateCopy(const FunctionDecl *functionDecl);
  void enterTopLevelBody(const FunctionDecl *functionDecl);
  CycleStep cycleRound(llvm::ArrayRef<FuncSymbol *> cycle);
  CycleStep settleCycle(llvm::ArrayRef<FuncSymbol *> cycle);
  bool isGivingValue(const ReturnStmt *returnStmt) const;
  bool isUndefinedReturn(const ReturnStmt *returnStmt) const;
  bool isIntLiteralReturn(const ReturnStmt *returnStmt) const;
  bool hasValuedReturn(llvm::ArrayRef<ReturnStmt *> returns) const;
  bool isConvertibleYield(const YieldExpr *yield, bool isAsync) const;
  void reportCycleTypes(llvm::ArrayRef<FuncSymbol *> cycle, bool isFailed);
  bool hasUndefinedReturn(llvm::ArrayRef<ReturnStmt *> returns) const;
  InstanceRecord *lateOwnerOf(const FunctionDecl *functionDecl) const;
  bool isConvertibleReturn(const ReturnStmt *returnStmt, bool isAsync) const;
  void copyParams(const FunctionDecl *functionDecl, FunctionDecl *copy) const;
  void copyMemberRole(const FunctionDecl *functionDecl, FunctionDecl *copy) const;
  DiagnosticRoute bodyRoute(bool isClosure, const InstanceRecord *lateOwner) const;
  std::vector<ReturnStmt *> literalsLast(llvm::ArrayRef<ReturnStmt *> returns) const;
  std::vector<const ReturnStmt *> typedReturns(llvm::ArrayRef<ReturnStmt *> returns) const;
  void convertReturns(const FunctionDecl *functionDecl, llvm::ArrayRef<ReturnStmt *> returns,
                      llvm::ArrayRef<YieldExpr *> yields);
  const Type *unifyReturns(const FuncSymbol *function, llvm::ArrayRef<ReturnStmt *> returns,
                           bool isFallingOff, ReturnErrors errors);
  const Type *collectedType(const FunctionDecl *functionDecl, const FuncSymbol *function,
                            const CollectedReturns &collected, bool isFallingOff,
                            ReturnErrors errors);

  SemanticState &state;
  ClassLinks *classLinks = nullptr;
  TypeParams *typeParams = nullptr;
  LateClasses *lateClasses = nullptr;
  AsyncChecker *asyncChecker = nullptr;
  ImportBinder *importBinder = nullptr;
  TypeRelations *typeRelations = nullptr;
  ReturnChecker *returnChecker = nullptr;
  FunctionChecker *functionChecker = nullptr;
  GeneratorChecker *generatorChecker = nullptr;
  GenericClassChecker *genericClassChecker = nullptr;
};

}
