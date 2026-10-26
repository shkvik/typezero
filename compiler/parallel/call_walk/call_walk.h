#pragma once

#include "compiler/parallel/analysis/analysis.h"

namespace tz::parallel {

class CallWalk {
public:
  explicit CallWalk(Analysis &analysis);

  void call(const CallExpr *c, const Use &use);
  void builtinCall(const CallExpr *c, const Use &use);
  void functionArg(const CallExpr *c, const Expr *arg, CallKind kind, const Use &use);
  void addFunctionCallees(const Expr *f, const Expr *arg, CallKind kind);
  void newExpr(const NewExpr *n);
  void addCallee(CallRec &rec, const FunctionDecl *decl, std::optional<Ref> self,
                 bool byType = false);
  std::vector<const FunctionDecl *> methodTargets(const FunctionDecl *method, bool virtualCall);
  std::optional<Ref> closureSelf(const FunctionDecl *callee, const FnInfo &caller) const;
  void registerValue(const FunctionDecl *fn, const Type *type);
  bool functionRef(const Expr *e, const Use &use, const Type *type);
  Ref classRef(const FunctionDecl *owner) const;

private:
  Analysis &analysis_;
};

}
