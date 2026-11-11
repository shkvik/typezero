#pragma once

#include "compiler/interprocedural/wide_writes/flow_graph/flow_graph_builder/flow_graph_builder.h"

namespace tz {

class FlowExprs {
public:
  explicit FlowExprs(FlowGraphBuilder &owner);

  const Expr *ownExpr(const Expr *e) const;
  void startExpr(Expr *e, FlowRun &run);
  void finishArray(ArrayLitExpr *literal, FlowRun &run);
  void finishProperty(ObjectLitExpr::Property *property, FlowRun &run);
  void passObjectThis(FunctionDecl *fn, FlowRun &run);
  void finishFilter(const Expr *expr, FlowRun &run);
  void finishMember(MemberExpr *member, FlowRun &run);
  void finishIndex(IndexExpr *index, FlowRun &run);
  void finishJoin(const Expr *expr, FlowRun &run);
  void finishNew(NewExpr *create, FlowRun &run);
  void finishCast(CastExpr *conversion, FlowRun &run);
  void finishFunctionExpr(FunctionDecl *fn, FlowRun &run);
  void finishYield(YieldExpr *yield, FlowRun &run);
  void finishAwait(AwaitExpr *await, FlowRun &run);

private:
  std::vector<FlowRun::Step> exprParts(Expr *e, FlowRun &run);
  std::vector<FlowRun::Step> templateParts(TemplateExpr *text) const;
  std::vector<FlowRun::Step> arrayParts(ArrayLitExpr *literal) const;
  std::vector<FlowRun::Step> objectParts(ObjectLitExpr *literal, FlowRun &run) const;
  std::vector<FlowRun::Step> binaryParts(BinaryExpr *binary) const;
  std::vector<FlowRun::Step> newParts(NewExpr *create) const;
  std::vector<FlowRun::Step> sequenceParts(SequenceExpr *sequence, FlowRun &run) const;

  FlowGraphBuilder &builder;
};

}
