#pragma once

#include "compiler/parallel/analysis/analysis_types.h"
#include "compiler/parallel/parallel_model/parallel_model.h"
#include "compiler/program/ast/ast.h"
#include <cstddef>
#include <optional>

namespace tz::parallel {

class CallResolver {
public:
  explicit CallResolver(Analysis &analysis);

  void resolve();
  void fold();
  std::optional<Access> substitute(const Access &a, const FnInfo &callee, int caller,
                                   const CallEdge &call, const CallRec &rec) const;
  std::optional<Ref> bind(const std::optional<Ref> &self, const Activation &caller) const;
  static const Expr *argument(const CallRec &rec, size_t index);
  static const FunctionDecl *functionOf(const Expr *e);
  static const VarSymbol *callableParam(const FunctionDecl *fn, const Expr *e);

private:
  Analysis &analysis_;
};

}
