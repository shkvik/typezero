#pragma once

#include "compiler/interprocedural/flows/flows.h"

namespace tz {

class FlowStmts {
public:
  explicit FlowStmts(Flows &owner);

  void function(FunctionDecl *fn);
  void classDecl(ClassDecl *cls);
  void stmt(Stmt *s);
  Refs name(const NameExpr *n);
  void recordWrite(const Refs &target, bool element, llvm::StringRef field, const Expr *value,
                   const Type *through, llvm::SMLoc loc);
  void assignTo(Expr *target, const Refs &value, const Expr *valueExpr, bool compound);

private:
  Flows &flows;
};

}
