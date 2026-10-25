#pragma once

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/SMLoc.h"
#include <cstdint>
#include <deque>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace tz {

struct FunctionDecl;
class Type;

struct FuncSymbol {
  llvm::StringRef name;
  llvm::SMLoc loc;
  // The types callers pass: `T | undefined` for a parameter that is optional
  // or has a default value, as a caller may leave it out.
  std::vector<const Type *> params;
  // The arguments a call must give: those up to the last parameter that is
  // neither optional nor has a default value.
  size_t minArgs = 0;
  // The last parameter is `...rest: T[]`: the arguments from its position on
  // become the elements of one array.
  bool hasRest = false;
  // `function (this: unknown, ...)`: the first parameter is the `this` the
  // function gets (see Type::hasThis).
  bool hasThis = false;
  // Without an annotation, null until Sema infers it from the body (see
  // Sema::returnType).
  const Type *ret = nullptr;
  // Set while Sema infers the return type: a call now is recursion, and
  // returns `assumed`, the type found so far (null at first).
  bool inferring = false;
  const Type *assumed = nullptr;
  FunctionDecl *decl = nullptr;
  // An arrow passed where a callback returns void, as in forEach: what its
  // body returns is dropped.
  bool discardsResult = false;
  // Some parameter has its type from a default value that is not a literal,
  // found when first needed (Sema::resolveParams).
  bool pendingParams = false;
  // A generic function: its type parameters, which its parameter and return
  // types mention. A call gives them types and calls a copy of the function
  // made for those (see Sema::checkGenericCall).
  std::vector<const Type *> typeParams;
  std::vector<const Type *> typeDefaults; // `<T = int>`; null without one
  bool isGeneric() const { return !typeParams.empty(); }
  // `x is T`, `asserts x is T` or `asserts x` as the return type: the
  // parameter (by position; -1 for `this`, -2 for none), T (null for
  // `asserts x`), and whether it asserts (see sema_predicate.cpp).
  int predicateParam = -2;
  const Type *predicateType = nullptr;
  bool asserts = false;
};

} // namespace tz
