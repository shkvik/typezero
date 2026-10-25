#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// Argument lists (docs/generics.md, section 9): `A extends unknown[]`, the parameters of
// any function, and the functions that wrap others.
class ArgListChecker {
public:
  explicit ArgListChecker(Sema &sema);

  // Argument lists (sema_generic.cpp, docs/generics.md section 9). A rest
  // parameter whose type is an argument list (`...args: [int, string]`, what
  // `A` stands for in a copy) becomes the parameters of the list; `args`
  // spread is them, one by one, and `args` used as a value a tuple made of
  // them when the function starts.
  void expandArgumentList(FunctionDecl *fn, FuncSymbol *sym);
  // `args` in the body of such a function, as it starts.
  void declareArgumentList(FunctionDecl *fn);
  // `args.length` and `args[1]` of a list that is not made a tuple: the
  // count, and the parameter; null for anything else.
  Expr *listPart(Expr *expr);
  // `...args` in the arguments of a call or the elements of an array
  // literal, of such a list: its parameters in its place.
  void expandListSpreads(std::vector<Expr *> &args, std::vector<bool> &spread);
  // In generic code, the arguments of a call for a rest parameter of a list
  // of types (`...args: A`, `[...A, ...B]`): spreads of those lists, in
  // order, and values for the other elements; reports anything else.
  bool checkListArguments(CallExpr *call, size_t fixed, const Type *list, llvm::SMLoc loc,
                          const std::vector<bool> *checked);
  // The type of `args[i]` and of the values of `for (const x of args)` in
  // generic code: some type, as a type parameter is.
  const Type *listElement(const Type *list);
  // `T[K]` of known types: the type of the member K of T (an element of a
  // tuple or an array, a field of an object, by a union of names the union
  // of theirs); null if there is none.
  const Type *memberType(const Type *object, const Type *key);

private:
  Sema &sema_;
  // The parameters each list variable stands for (see expandArgumentList).
  llvm::DenseMap<const VarSymbol *, std::vector<llvm::StringRef>> argumentLists_;
};

} // namespace tz
