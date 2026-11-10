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
  std::vector<Diagnostics::Captured> *capture = nullptr;
};

class ReturnInferrer {
public:
  ReturnInferrer(SemanticState &owner, SemanticParts &all);

  static bool returnsValue(const Stmt *body);

  void inferReturnType(FuncSymbol *fn);
  void checkAndInfer(FunctionDecl *decl);
  const Type *returnType(FuncSymbol *fn);
  const Type *functionType(FuncSymbol *fn);
  void leaveBody(BodyState &saved);
  FunctionDecl *freshCopy(const FunctionDecl *fn);
  void inferCycle(llvm::ArrayRef<FuncSymbol *> cycle);
  BodyState enterBody(const FunctionDecl *fn);

private:
  using ResultCycleTypeKnown = Result<const Type *, ReturnInferrerCycleTypeError>;
  using ResultReturnsUnified = Result<const Type *, ReturnInferrerCommonReturnError>;
  using ResultNotCalledEarly = Result<const FuncSymbol *, ReturnInferrerEarlyCallError>;
  using ResultReturnsDefined = Result<const FuncSymbol *, ReturnInferrerDefinedReturnError>;

  static bool givesValue(const ReturnStmt *result);
  static std::vector<ReturnStmt *> literalsLast(llvm::ArrayRef<ReturnStmt *> returns);

  const Type *trialReturn(FuncSymbol *fn);
  const Type *templateReturn(FuncSymbol *fn);
  ResultNotCalledEarly earlyCall(const FuncSymbol *fn) const;
  ResultCycleTypeKnown cycleType(const FuncSymbol *fn, int cycleSize, bool failed) const;
  ResultReturnsUnified commonReturn(const FuncSymbol *fn, const Type *before,
                                    const ReturnStmt *ret) const;
  const Type *unifyReturns(const FuncSymbol *fn, llvm::ArrayRef<ReturnStmt *> returns,
                           bool fallsOff, bool report);
  ResultReturnsDefined definedReturn(const FuncSymbol *fn, llvm::ArrayRef<ReturnStmt *> returns,
                                     bool undefined) const;
  void convertReturns(const FunctionDecl *decl, llvm::ArrayRef<ReturnStmt *> returns,
                      llvm::ArrayRef<YieldExpr *> yields);

  SemanticState &state;
  ClassLinks *classLinks = nullptr;
  TypeParams *typeParams = nullptr;
  LateClasses *lateClasses = nullptr;
  AsyncChecker *asyncChecker = nullptr;
  ImportBinder *importBinder = nullptr;
  TypeRelations *typeRelations = nullptr;
  FunctionChecker *functionChecker = nullptr;
  GeneratorChecker *generatorChecker = nullptr;
  GenericClassChecker *genericClassChecker = nullptr;
};

}
