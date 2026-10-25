#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// The collections of the program: classes with [Symbol.iterator]().
class IterableChecker {
public:
  explicit IterableChecker(Sema &sema);

  // Collections of the program (sema_iterable.cpp, docs/iterators.md,
  // section 2): an object whose class (or literal) has a method
  // `[Symbol.iterator]()` is iterated through it, as in JavaScript; an
  // iterator written by hand, an object with next() that gives `{ done,
  // value }`, through a generator that calls next() (see iterateFunction),
  // which consumers emit inline. A parameter `xs: Iterable<int>` makes its
  // function generic: a copy for each kind of iterable it is called with.
  //
  // `x[Symbol.iterator]()` of a checked `x` whose class has the method, as
  // an iterator; null if it has none.
  Expr *userIterator(Expr *expr);
  // `x[Symbol.asyncIterator]()` of an object whose class (or literal) has
  // that method, an async generator; null for any other.
  Expr *asyncUserIterator(Expr *expr);
  // The async iterator `[Symbol.asyncIterator]()` of an object of `type`
  // gives, for the inference of `AsyncIterable<T>`; null without one.
  const Type *asyncIterableType(const Type *type);
  // `it`, an iterator written by hand (or an iterator already), as an
  // iterator; null if it has no next() that gives `{ done, value }`.
  Expr *iteratorOf(Expr *it);
  // The values next() of an object of `type` gives when it is not done, or
  // null if it has no such next().
  const Type *nextValueType(const Type *type);
  // The values iterating a value of `type` gives, whatever it is (an array,
  // a Map, an object with `[Symbol.iterator]()`...); null if it is not
  // iterable.
  const Type *elementTypeOf(const Type *type);
  // The generic generator that iterates an iterator written by hand,
  // `iterate<I, V>(it: I): Generator<V>`, made once.
  FuncSymbol *iterateFunction();
  // `IteratorResult<T, R>`: `{ done: false; value: T } | { done: true;
  // value: R }`, as in TypeScript.
  static const Type *iteratorResultType(const Type *value, const Type *returned);
  // `xs: Iterable<int>` in the signature of `fn`: a type parameter of its
  // own; null if `ref` is not that.
  const Type *iterableParam(FunctionDecl *fn, FuncSymbol *sym, const TypeRef &ref);
  // Whether `ref` is `Iterable<T>`, which makes a function generic.
  bool isIterableRef(const TypeRef &ref) const;

private:
  Sema &sema_;
  FuncSymbol *iterate_ = nullptr;
};

} // namespace tz
