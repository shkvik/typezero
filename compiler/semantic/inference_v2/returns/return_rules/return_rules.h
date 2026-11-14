#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/inference_v2/returns/inference_returns_registry_errors.h"
#include "compiler/semantic/semantic_state/return_inference/return_inference.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include <optional>
#include <string>
#include <vector>

namespace tz {

class ReturnRules {
public:
  explicit ReturnRules(SemanticState &owner);

  FunctionDecl *freshCopy(const FunctionDecl *functionDecl);
  InstanceRecord *lateOwnerOf(const FunctionDecl *functionDecl) const;
  const Type *collectedType(const FunctionDecl *functionDecl, const FuncSymbol *function,
                            const CollectedReturns &collected, bool isFallingOff,
                            ReturnErrors errors);

private:
  struct YieldJoin {
    const Type *yielded;
    bool isBroken;
    bool hasFailed;
  };
  struct KnownYield {
    const YieldExpr *yield;
    const Type *type;
  };
  struct KnownReturn {
    const ReturnStmt *returnStmt;
    const Type *type;
  };

  using ResultReturnsUnified = Result<const Type *, InferenceReturnsRegistryCommonReturnError>;
  using ResultYieldedTypesJoin = Result<const Type *, InferenceReturnsRegistryYieldsJoinError>;
  using ResultYieldedTypeKnown = Result<const Type *, InferenceReturnsRegistryYieldsKnownError>;
  using ResultReturnsOfOneType =
      Result<const Type *, InferenceReturnsRegistryGeneratorReturnsError>;
  using ResultReturnsDefined =
      Result<const FuncSymbol *, InferenceReturnsRegistryDefinedReturnError>;

  void copyParams(const FunctionDecl *functionDecl, FunctionDecl *copy) const;
  void copyMemberRole(const FunctionDecl *functionDecl, FunctionDecl *copy) const;


  bool isUndefinedReturn(const ReturnStmt *returnStmt) const;
  bool isIntLiteralReturn(const ReturnStmt *returnStmt) const;
  bool hasValuedReturn(llvm::ArrayRef<ReturnStmt *> returns) const;
  bool hasUndefinedReturn(llvm::ArrayRef<ReturnStmt *> returns) const;
  bool hasUndefinedGeneratorReturn(llvm::ArrayRef<ReturnStmt *> returns, bool isAsync) const;


  const Type *yieldedValueType(const YieldExpr *yield, bool isAsync) const;
  const Type *returnedValueType(const ReturnStmt *returnStmt, bool isAsync) const;
  const Type *unifyReturns(const FuncSymbol *function, llvm::ArrayRef<ReturnStmt *> returns,
                           bool isFallingOff, ReturnErrors errors);
  const Type *unknownYields(const FuncSymbol *function, llvm::ArrayRef<YieldExpr *> yields,
                            const YieldJoin &join, ReturnErrors errors);
  const Type *inferGeneratorType(const FuncSymbol *function, llvm::ArrayRef<YieldExpr *> yields,
                                 llvm::ArrayRef<ReturnStmt *> returns, bool isFallingOff,
                                 ReturnErrors errors);


  std::string nothingYieldedReason(const Type *yielded, bool hasNoYields) const;
  std::vector<ReturnStmt *> literalsLast(llvm::ArrayRef<ReturnStmt *> returns) const;
  std::vector<const ReturnStmt *> typedReturns(llvm::ArrayRef<ReturnStmt *> returns) const;
  std::vector<KnownYield> knownYields(llvm::ArrayRef<YieldExpr *> yields, bool isAsync) const;
  std::vector<KnownReturn> knownReturns(llvm::ArrayRef<ReturnStmt *> returns, bool isAsync) const;
  ResultYieldedTypesJoin checkYieldsJoin(const Type *yielded, const Type *type,
                                         const FuncSymbol *function);
  ResultReturnsOfOneType checkReturnsJoin(const Type *returned, const Type *type,
                                          const FuncSymbol *function);
  ResultReturnsUnified commonReturn(const FuncSymbol *function, const Type *before,
                                    const ReturnStmt *returnStmt);
  ResultYieldedTypeKnown checkYieldsKnown(const Type *yielded, const FuncSymbol *function,
                                          bool hasNoYields) const;
  YieldJoin joinYields(const FuncSymbol *function, llvm::ArrayRef<YieldExpr *> yields, bool isAsync,
                       ReturnErrors errors);
  ResultReturnsDefined definedReturn(const FuncSymbol *function,
                                     llvm::ArrayRef<ReturnStmt *> returns, bool isUndefined) const;
  std::optional<const Type *> joinedReturns(const FuncSymbol *function,
                                            llvm::ArrayRef<ReturnStmt *> returns, bool isFallingOff,
                                            bool isAsync, ReturnErrors errors);

  SemanticState &state;
};

}
