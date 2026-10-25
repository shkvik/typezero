#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// Imports of the built-in modules of node and tz, and the names they give.
class BuiltinModuleChecker {
public:
  explicit BuiltinModuleChecker(Sema &sema);

  // `import { ... } from "node:fs"` and the like in `module`.
  void importBuiltinModule(Module *module, const ImportDecl &decl);
  // What `name` stands for where it is used, if a built-in module gave it.
  const Sema::NodeName *nodeName(llvm::StringRef name) const;
  // Calls, reads and assignments that are about built-in modules; null if
  // `call`, `name` or `member` is not one. checkNodeCall may instead change
  // the callee to what it stands for (`process.exit` for `exit` of
  // node:process) and return null.
  Expr *checkNodeCall(CallExpr *call, const Type *expected = nullptr);
  Expr *checkNodeName(NameExpr *name);
  Expr *checkNodeMember(MemberExpr *member);
  // A name of node:process, `argv`, as what it is: `process.argv`; other
  // expressions as they are.
  Expr *processAlias(Expr *expr);
  // The value of a name of a module that is not called: a constant, or an
  // error.
  Expr *nodeValue(const Sema::NodeName &n, Expr *expr);

private:
  Sema &sema_;
};

} // namespace tz
