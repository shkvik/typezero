#pragma once

#include "compiler/lower/codegen/codegen.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/Support/ConvertUTF.h"
#include <functional>
#include <map>
#include <set>

namespace tz {

// Which values each function owns, and which it only borrows.

std::string utf8Of(const std::u16string &text);

// Looks through what does not change which object a value is: implicit
// conversions, `x!` and `(x)`.
const Expr *bare(const Expr *e);

// Layer 2 of docs/objects.md, section 11: an object that another one makes
// for itself and never gives away (a dialog's button) lives no longer than
// its owner; a function the owner stores in it that captures the owner
// (`this.button.onClick = () => this.close()`) needs no count of it, and the
// cycle owner → button → function → owner is no cycle of counts.
//
// The proof, over the whole program, for a field f of class C and a field g
// of class D: every write to a field f is `this.f = new D(...)` in the
// construction of a C (with arguments that do not hold `this`); a field f
// is read only to reach a field or a method of what it holds, never as a
// value; D's construction and methods let `this` go nowhere; and a field g is
// only ever called, never read as a value. Then the D lives only in its C's
// field, the function only in the D's field, and the function's reference
// to the C is not counted.
class OwnerScanner {
public:
  struct BackRef {
    const ClassDecl *owner;   // C
    llvm::StringRef field;    // f
    llvm::StringRef callback; // g
    const FunctionDecl *fn;   // the function stored in g
    const VarSymbol *self;    // C's `this`, which it captures
  };

  void function(const FunctionDecl *fn);

  void topLevel(const Stmt *s);

  // The proofs that hold, given what escapes(cls) says of classes.
  std::vector<BackRef> proven(const std::function<bool(const ClassDecl *)> &escapes) const;

  // The class each owned field makes, by name, for the checks of debug mode.
  const ClassDecl *madeBy(llvm::StringRef field) const;

private:
  enum class Use { Value, Receiver, Callee };
  struct Write {
    const ClassDecl *cls; // whose construction makes it; null elsewhere
    const ClassDecl *made;
  };

  void stmt(const Stmt *stmt);

  void expr(const Expr *e, Use use);

  void readAll(llvm::StringRef name);

  void readFields(const Type *type);

  // `x.f = v`: of the construction of a C when x is its `this` and v makes
  // a new object of a class; and `this.f.g = () => ... this ...`.
  void wrote(const MemberExpr *target, const AssignExpr *assign);

  static bool mentions(const Expr *e, const VarSymbol *self);

  static void visitChildren(const Expr *e, const std::function<void(const Expr *)> &visit);

  llvm::DenseSet<const FunctionDecl *> scanned_;
  const FunctionDecl *current_ = nullptr;
  llvm::StringSet<> valueReads_, nonCallReads_, bad_;
  llvm::StringMap<std::vector<Write>> writes_;
  std::vector<BackRef> candidates_;
};

} // namespace tz
