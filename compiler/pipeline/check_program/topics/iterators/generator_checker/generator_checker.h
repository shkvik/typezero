#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/topics/iterators/generator_checker/generator_checker_errors.h"
#include "compiler/result/result/result.h"

namespace tz {

class GeneratorChecker {
public:
  using ResultNextNeedsNoValue = Result<const Type *, GeneratorCheckerCheckSendsNothingError>;
  using ResultResolvedIterator = Result<const Type *, GeneratorCheckerResolveIteratorTypeError>;
  using ResultGeneratorReturns = Result<const Type *, GeneratorCheckerGeneratorReturnTypeError>;

  explicit GeneratorChecker(CheckState &owner);

  Expr *checkYield(YieldExpr *yield);
  const Type *asyncIteratedType(const Type *type);
  static const Type *iteratedType(const Type *type);
  ResultResolvedIterator resolveIteratorType(const TypeRef &ref);
  Expr *destructuredIterator(Expr *init, const VarDeclarator &d);
  ResultNextNeedsNoValue checkSendsNothing(const Type *iterator, llvm::StringRef what);
  ResultGeneratorReturns generatorReturnType(const FunctionDecl *fn, const Type *declared);
  const Type *inferGeneratorType(const FuncSymbol *fn, llvm::ArrayRef<YieldExpr *> yields,
                                 llvm::ArrayRef<ReturnStmt *> returns, bool fallsOff, bool report);

private:
  using ResultNothingSentAsync = Result<const Type *, GeneratorCheckerAsyncSentError>;
  using ResultYieldedTypesJoin = Result<const Type *, GeneratorCheckerYieldsJoinError>;
  using ResultReturnsOfOneType = Result<const Type *, GeneratorCheckerReturnsJoinError>;
  using ResultYieldedTypeKnown = Result<const Type *, GeneratorCheckerYieldsKnownError>;
  using ResultPromiseYieldFits = Result<const Type *, GeneratorCheckerPromiseYieldError>;
  using ResultYieldGivesAValue = Result<const Type *, GeneratorCheckerValueYieldedError>;
  using ResultDelegateSyncFits = Result<const Type *, GeneratorCheckerDelegateSyncError>;
  using ResultSentTypeDeclared = Result<const Type *, GeneratorCheckerSentDeclaredError>;
  using ResultDelegateAsyncFit = Result<const Type *, GeneratorCheckerDelegateAsyncError>;

  static ResultYieldGivesAValue valueYielded(const Type *yielded, const FuncSymbol *fn);
  static ResultNothingSentAsync asyncSent(const Type *sent, const Type *yielded, bool loose);
  static ResultYieldedTypesJoin yieldsJoin(const Type *yielded, const Type *t,
                                           const FuncSymbol *fn);
  static ResultYieldedTypeKnown yieldsKnown(const Type *yielded, const FuncSymbol *fn,
                                            bool noYields);
  static ResultReturnsOfOneType returnsJoin(const Type *returned, const Type *t,
                                            const FuncSymbol *fn);
  static ResultPromiseYieldFits promiseYield(const Type *t, const Type *given, const Type *yielded,
                                             const FuncSymbol *fn);
  static ResultDelegateAsyncFit delegateAsync(const Type *type, const Type *element,
                                              const Type *yielded, const FuncSymbol *fn);
  static ResultSentTypeDeclared sentDeclared(const YieldExpr *yield, bool async, const Type *sent,
                                             const Type *yielded, bool loose);
  static ResultDelegateSyncFits delegateSync(const Type *type, const Type *element,
                                             const Type *yielded, const Type *sent, bool loose,
                                             const FuncSymbol *fn);

  CheckState &state;
};

}
