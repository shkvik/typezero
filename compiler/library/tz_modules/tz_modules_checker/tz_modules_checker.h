#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

// The built-in modules written in tz: their externals and accessors.
class TzModulesChecker {
public:
  explicit TzModulesChecker(Checker &checker);

  // Any function value where a function type with the same parameters and
  // a result its own converts to is expected, as TypeScript allows: an
  // async handler `async req => ...` where `(req: Request) => Response |
  // Promise<Response>` is (sema_library.cpp). Null if the types differ
  // otherwise.
  Expr *adaptResult(Expr *expr, const Type *to);
  // The built-in modules written in tz (sema_library.cpp, library/), which
  // prepareLibraries and inLibrary (sema_events.cpp) find. An intrinsic, `function __tz_name(...):
  // T` without a body: its types must be ones the runtime takes and gives.
  void checkExternal(FunctionDecl *fn);
  // Accessors of their classes (FunctionDecl::accessor): the getter or the
  // setter `name` of `cls` or a class up its chain, or null.
  static FunctionDecl *accessorOf(ClassDecl *cls, llvm::StringRef name,
                                  FunctionDecl::Accessor kind);
  // `o.x` that reads through a getter: a call of it; null if x is none.
  Expr *checkAccessorRead(MemberExpr *member, ClassDecl *cls, const Type *expected);
  // `o.x = v` of an accessor x: the type of v, and the assignment becomes a
  // call of the setter (checkSetterCall), which pendingSetter_ waits for;
  // null if x is no accessor.
  const Type *checkAccessorTarget(MemberExpr *member, ClassDecl *cls, bool compound);
  Expr *checkSetterCall(AssignExpr *assign);
  // A call the program writes of an accessor, `o.x()`: reported.
  bool rejectAccessorCall(CallExpr *call, MemberExpr *member, FuncSymbol *sym);
  // TLS (docs/net.md, section 4), which tz links only into a program that may use https: a call
  // of fetch or serve of the program that may (checked), or either taken as a value, sets
  // SourceFile::usesTls.
  void noteHttpsCall(CallExpr *call, const FunctionDecl *fn);
  void noteHttpsValue(const FunctionDecl *fn, llvm::SMLoc loc);
  // The ASCII text a string is known to start with: a literal, the first text of a template or of
  // a `+`, the start of a constant; "" when nothing is known.
  std::string knownPrefix(const Expr *expr) const;

private:
  Checker &checker_;
  // The calls of accessors that reading and assigning made.
  llvm::SmallPtrSet<const CallExpr *, 8> accessorCalls_;
};

} // namespace tz
