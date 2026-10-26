#pragma once

#include "compiler/whole_program/flows/flows.h"

namespace tz {

class FlowStmts {
public:
  explicit FlowStmts(Flows &flows);

  void function(FunctionDecl *fn);
  void classDecl(ClassDecl *cls);
  void stmt(Stmt *s);
  const VarSymbol *currentThis() const;
  Refs name(const NameExpr *n);
  void recordWrite(const Refs &target, bool element, llvm::StringRef field, const Expr *value,
                   const Type *through, llvm::SMLoc loc);
  void assignTo(Expr *target, const Refs &value, const Expr *valueExpr, bool compound);

private:
  Flows &flows_;
};

}
