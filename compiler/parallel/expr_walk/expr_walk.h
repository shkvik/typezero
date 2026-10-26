#pragma once

#include "compiler/parallel/analysis/analysis.h"

namespace tz::parallel {

class ExprWalk {
public:
  explicit ExprWalk(Analysis &analysis);

  void expr(const Expr *e, const Use &use = {});
  void name(const NameExpr *n, const Use &use);
  void member(const MemberExpr *m);
  void assign(const AssignExpr *a);
  void update(const Expr *target);
  void awaitExpr(const AwaitExpr *a);
  void yieldExpr(const YieldExpr *y);
  void fieldAccess(const Expr *object, llvm::StringRef name, bool write, llvm::SMLoc at);
  void indexAccess(const IndexExpr *ix, bool write);
  void elements(const Expr *e, bool write, llvm::SMLoc at);
  void contents(const Expr *e, bool write, llvm::SMLoc at);
  void readSpread(const Expr *e, bool object, llvm::SMLoc at);

private:
  Analysis &analysis_;
};

}
