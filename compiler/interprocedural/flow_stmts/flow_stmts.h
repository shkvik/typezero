#pragma once

#include "compiler/interprocedural/flow_graph_builder/flow_graph_builder.h"

namespace tz {

class FlowStmts {
public:
  explicit FlowStmts(FlowGraphBuilder &owner);

  void function(FunctionDecl *fn);
  void classDecl(ClassDecl *cls);
  void stmt(Stmt *s);
  Refs name(const NameExpr *n);
  void recordWrite(const Refs &target, bool element, llvm::StringRef field, const Expr *value,
                   const Type *through, llvm::SMLoc loc);
  void assignTo(Expr *target, const Refs &value, const Expr *valueExpr, bool compound);

private:
  FlowGraphBuilder &builder;
};

}
