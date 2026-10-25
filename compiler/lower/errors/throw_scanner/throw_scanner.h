#pragma once

#include <memory>
#include "compiler/lower/codegen/codegen.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include <functional>
#include <map>
#include <optional>

namespace tz {

// Which functions may throw, from what they call.

// What some code may run and throw: the calls it makes, the built-ins of it
// that may throw, and the functions it makes values of.
struct ThrowSummary {
  // A `throw` or a built-in that may throw, not caught in the code itself.
  bool intrinsic = false;
  // Resumes an iterator object, whose code (a generator, the callbacks of
  // helpers) may throw.
  bool resumes = false;
  // The calls not caught in the code, of functions and through values of
  // function types.
  std::vector<const FunctionDecl *> calls;
  std::vector<const Type *> valueCalls;
  // Every call, caught or not, for finding the functions that recurse.
  std::vector<std::pair<const FunctionDecl *, const CallExpr *>> allCalls;
  std::vector<const Type *> allValueCalls;
  // The functions the code makes values of, by the type of the value, and
  // the functions Result.fromThrowable makes, which throw what the function
  // that turns the error throws.
  std::vector<std::pair<const Type *, const FunctionDecl *>> values;
  std::vector<std::pair<const Type *, const Expr *>> fromThrowables;
  // The function values given a type with another result (see
  // CastExpr::adaptsResult): a value of the first type may be one of the
  // second.
  std::vector<std::pair<const Type *, const Type *>> adapters;
  // The functions declared or written in it.
  std::vector<const FunctionDecl *> nested;
};

// What a call through the method table of `fn` may run: fn and the methods
// that override it in the classes down the chain (not an abstract one, which
// has no code).
void forEachImplementation(const FunctionDecl *fn,
                           llvm::function_ref<void(const FunctionDecl *)> visit);

// Constant integers, as the operands of `/` and `new Array(n)`.
std::optional<int64_t> constantInt(const Expr *expr);

// The function a name refers to directly, as a value (see knownFunction in
// codegen_closure.cpp).
const FunctionDecl *namedFunction(const Expr *expr);

// Walks code and fills a ThrowSummary. `caught`: the code is in the block of
// a `try` that has a `catch`, which takes what it throws.
class ThrowCalls;

class ThrowScanner {
public:
  explicit ThrowScanner(ThrowSummary &out);
  void stmt(const Stmt *stmt, bool caught);
  void expr(const Expr *expr, bool caught);
private:
  ThrowSummary &out_;

public:
  ~ThrowScanner();

  ThrowCalls &throwCalls() const { return *throwCallsPart_; }

private:
  friend class ThrowCalls;
  std::unique_ptr<ThrowCalls> throwCallsPart_;
};

} // namespace tz
