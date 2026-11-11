#pragma once

#include "compiler/interprocedural/wide_writes/flow_graph/flow_graph_builder/flow_graph_builder.h"

namespace tz {

class FlowCalls {
public:
  explicit FlowCalls(FlowGraphBuilder &owner);

  bool mayCall(const Type *type, const FunctionDecl *fn) const;
  Refs receiverConversion(const Expr *at, const Type *from, const FunctionDecl *method,
                          const Refs &refs, const VarSymbol *initializes, std::string role);
  void passArguments(const CallExpr *call, const FunctionDecl *target,
                     const std::vector<Refs> &args);
  void passThis(const Refs &receiver, const FunctionDecl *method);
  std::vector<const FunctionDecl *> overridersOf(const FunctionDecl *fn);
  std::vector<FlowRun::Step> callParts(CallExpr *c, FlowRun &run);
  void finishCall(CallExpr *c, FlowRun &run);
  void callback(const Expr *arg, const Refs &values);

private:
  void markRuntimeParams(const CallExpr *c);
  Refs valueCallResult(const CallExpr *c, const MemberExpr *member, const NameExpr *callee,
                       const std::vector<Refs> &args);

  FlowGraphBuilder &builder;
};

}
