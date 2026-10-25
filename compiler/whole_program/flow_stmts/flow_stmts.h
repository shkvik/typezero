#pragma once

#include "compiler/whole_program/flows/flows.h"

namespace tz {

// The flows of statements, functions and classes.
class FlowStmts {
public:
  explicit FlowStmts(Flows &flows);

  void function(FunctionDecl *fn);
  void classDecl(ClassDecl *cls);
  void stmt(Stmt *s);
  // The `this` of the function being walked, for `super`.
  const VarSymbol *currentThis() const;
  Refs name(const NameExpr *n);
  void recordWrite(const Refs &target, bool element, llvm::StringRef field, const Expr *value,
                   const Type *through, llvm::SMLoc loc);
  // The target of an assignment or `++`: the write, and what flows there.
  void assignTo(Expr *target, const Refs &value, const Expr *valueExpr, bool compound);

private:
  Flows &flows_;
};

} // namespace tz
