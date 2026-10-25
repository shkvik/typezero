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
class RegexLitExpr;
class Type;

// A variable, parameter or global. Created by Checker, shared by all its uses.
struct VarSymbol {
  llvm::StringRef name;
  llvm::SMLoc loc;
  const Type *type = nullptr;
  bool isConst = false;
  bool isGlobal = false;
  // A global whose name some function mentions, so a call may read or change
  // it. Other globals live in main's stack frame, where LLVM can keep them in
  // registers, and calls cannot touch them.
  bool usedInFunction = false;

  // Closures (see FunctionDecl::captures). The function whose frame holds the
  // variable: null for main and for globals.
  FunctionDecl *owner = nullptr;
  // The order of declarations, to tell whether a closure is created before
  // the variable is.
  unsigned declSeq = 0;
  // A function nested in some function uses the variable.
  bool captured = false;
  // Assigned after its declaration, anywhere.
  bool reassigned = false;
  // Captured by a closure created before the variable is declared.
  bool capturedEarly = false;
  // The variable a nested function declaration introduces: its value is that
  // function.
  FunctionDecl *function = nullptr;
  // A constant initialized with an arrow or a function expression (or the
  // name of a function expression inside it): calls through it are direct,
  // and the function calls itself without capturing itself.
  FunctionDecl *lambda = nullptr;
  // A variable that holds the regex of this literal all along (a constant,
  // or a `let` nothing assigns to): its flags and groups are known.
  const RegexLitExpr *regex = nullptr;
  // A constant that holds the method Reflect.get gave of an object of a
  // class known when compiling (see Builtin::ReflectMethod): Reflect.apply of
  // it may call the method itself.
  const FunctionDecl *reflectMethod = nullptr;

  // Closures copy the value of a variable that never changes after they are
  // created; any other variable they capture lives in a box on the heap,
  // shared by the function that declares it and the closures.
  bool boxed() const { return captured && (reassigned || capturedEarly); }
};

} // namespace tz
