#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// Generators: function*, yield, and the types of iterators (docs/iterators.md).
class GeneratorChecker {
public:
  explicit GeneratorChecker(Sema &sema);

  // Generators and iterators (sema_iter.cpp). A generator function returns
  // `Generator<T, R, N>`: declared, or found from its `yield` and `return`
  // statements as a return type is. Iterators are consumed by `for...of`,
  // spreads, destructuring, Array.from and the helpers of ES2025; a chain of
  // helpers written where it is consumed runs as one loop (see markFused).
  //
  // `Generator<...>` and `IterableIterator<T>` in a type annotation.
  const Type *resolveIteratorType(const TypeRef &ref);
  // Checks the declared return type of a generator function.
  const Type *generatorReturnType(const FunctionDecl *fn, const Type *declared);
  // The type of a generator found from its body: what its yields give and
  // what its returns give; null if nothing is known yet (in a trial).
  const Type *inferGeneratorType(const FuncSymbol *fn, llvm::ArrayRef<YieldExpr *> yields,
                                 llvm::ArrayRef<ReturnStmt *> returns, bool fallsOff, bool report);
  Expr *checkYield(YieldExpr *yield);
  // The values iterating a value of `type` gives: the elements of an array,
  // the code points of a string, the values of an iterator; null otherwise.
  static const Type *iteratedType(const Type *type);
  // Iterating an iterator sends nothing to its `yield`: reports one whose
  // `yield` needs a value. `what` says who iterates.
  bool checkSendsNothing(const Type *iterator, llvm::SMLoc loc, llvm::StringRef what);
  // `const [a, b] = it`: the first values of the iterator, as an array.
  Expr *destructuredIterator(Expr *init, const VarDeclarator &d);
  // What `for await` over a value of `type` gives (docs/iterators.md, step
  // 4): the values of an async iterator, the lines of readline, or the
  // elements of an array or an iterator, awaited; null for none.
  const Type *asyncIteratedType(const Type *type);

private:
  Sema &sema_;
};

} // namespace tz
