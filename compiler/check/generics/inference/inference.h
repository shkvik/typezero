#pragma once

#include "compiler/check/sema/sema.h"
#include "compiler/check/generics/generic_check_helpers/generic_check_helpers.h"
#include "compiler/whole_program/flows/flows.h"
#include "llvm/ADT/STLExtras.h"
#include <functional>
#include <set>

namespace tz {

// What the arguments of a call say about the type parameters of the callee.

// Whether `type` mentions one of `params`.
bool mentionsAnyOf(const Type *type, llvm::ArrayRef<const Type *> params);

// The parameters of the function type `fn` from position `from` on, as an
// argument list (see Type::argumentList).
const Type *paramsAsList(const Type *fn, size_t from);

// The elements `[begin, end)` of an argument list, as one; the last part
// keeps the rest of the list, and the elements it may leave out.
const Type *listSlice(const Type *list, size_t begin, size_t end, bool last);

// What the arguments of a call say about the type parameters of the callee:
// the types each one would be, unified later. Those of literals are weak:
// a literal takes the type its parameter gets, so `{ kind: "dog" }` next to
// a Pet makes T a Pet, and 2.5 next to an int makes it a double.
struct Inference {
  using Origins =
      llvm::DenseMap<const Type *, std::pair<TypeAliasDecl *, std::vector<const Type *>>>;
  // `origins` are the generic aliases that made object types, and their
  // type arguments: an interface of a generic one, `Repo<T>`, matches the
  // same one with other arguments.
  // `elementOf` gives the values iterating a type gives, for `xs:
  // Iterable<T>`.
  Inference(llvm::ArrayRef<const Type *> params, const Origins &origins,
            std::function<const Type *(const Type *)> elementOf);

  // Matches `pattern`, a type of the callee that mentions its type
  // parameters, against `actual`.
  void from(const Type *pattern, const Type *actual);

  // `[...A, X]`, `[...A, ...B]` against the argument list `actual`: each
  // part takes the elements its place in the list says. With two lists of
  // types, one must be known (`bound`, those found so far): what the other
  // arguments give is used first; until then the match waits (see retry).
  void matchList(const Type *pattern, const Type *actual, const std::vector<const Type *> *bound);

  // The matches of lists that waited for other arguments.
  void retry(const std::vector<const Type *> &bound);

  llvm::ArrayRef<const Type *> params;
  std::vector<std::vector<const Type *>> candidates, weakCandidates;
  std::vector<bool> undefinedSeen;
  std::set<std::pair<const Type *, const Type *>> visited;
  bool weak = false;
  const Origins &origins;
  std::function<const Type *(const Type *)> elementOf;
  // What `[Symbol.asyncIterator]()` of an object of a type gives, if any.
  std::function<const Type *(const Type *)> asyncOf;
  std::vector<std::pair<const Type *, const Type *>> deferred;
};

} // namespace tz
