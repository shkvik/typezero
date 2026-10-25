#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// The types of neverthrow and their names in a program.
class ResultTypes {
public:
  explicit ResultTypes(Sema &sema);

  // Result of neverthrow (sema_result.cpp). The names of neverthrow are
  // built in: what `name` stands for where it is used, "ok" or "Result" (also
  // through `import { ok as good } from "neverthrow"`), or empty if the
  // program declares that name itself.
  llvm::StringRef neverthrowName(llvm::StringRef name) const;
  llvm::StringRef neverthrowTypeName(llvm::StringRef name) const;
  // `import { ... } from "neverthrow"` in `module`.
  void importNeverthrow(Module *module, const ImportDecl &decl);
  // `Result<T, E>`, `Ok<T, E>` (Result<T, never>) and `Err<T, E>`.
  const Type *resolveResultType(const TypeRef &ref, llvm::StringRef name);
  // The type that holds both halves `a` and `b` of Results, or null.
  static const Type *unifyHalf(const Type *a, const Type *b);
  // An operand of type never (the parameter of a function for the half of a
  // Result that cannot be there) takes the type of the other one, as in
  // TypeScript: `err("x").map(v => v + 1)` is fine.
  void neverOperands(BinaryExpr *bin);

private:
  Sema &sema_;
};

} // namespace tz
