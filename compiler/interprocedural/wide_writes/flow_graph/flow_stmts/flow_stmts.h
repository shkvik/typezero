#pragma once

#include "compiler/interprocedural/wide_writes/flow_graph/flow_graph_builder/flow_graph_builder.h"

namespace tz {

class FlowStmts {
public:
  explicit FlowStmts(FlowGraphBuilder &owner);

  void startStmt(Stmt *s, FlowRun &run);
  void startFunction(FunctionDecl *fn, FlowRun &run);
  void startClass(ClassDecl *cls, FlowRun &run);
  void bindVar(const VarSymbol *var, FlowRun &run);
  void bindField(ClassField *field, FlowRun &run);
  void bindForOf(ForOfStmt *loop, FlowRun &run);
  void finishReturn(FlowRun &run);
  void startAssign(const FlowRun::AssignTarget &assign, FlowRun &run);
  void finishAssign(const FlowRun::AssignTarget &assign, FlowRun &run);
  Refs name(const NameExpr *n);
  void recordWrite(const Refs &target, bool element, llvm::StringRef field, const Expr *value,
                   const Type *through, llvm::SMLoc loc);

private:
  std::vector<FlowRun::Step> stmtParts(Stmt *s) const;
  std::vector<FlowRun::Step> declaratorParts(VarDeclStmt *decl) const;
  std::vector<FlowRun::Step> forParts(ForStmt *loop) const;
  std::vector<FlowRun::Step> switchParts(SwitchStmt *choice) const;
  void finishIndexAssign(const FlowRun::AssignTarget &assign, IndexExpr *index, FlowRun &run);
  void finishAssignResult(const FlowRun::AssignTarget &assign, const Refs &value, FlowRun &run);

  FlowGraphBuilder &builder;
};

}
