#pragma once

#include "compiler/parallel/analysis/analysis.h"

namespace tz::parallel {

class CallResolver {
public:
  explicit CallResolver(Analysis &analysis);

  void resolve();
  void fold();
  std::optional<Access> substitute(const Access &a, const FnInfo &callee, const FnInfo &caller,
                                   const Callee &call, const CallRec &rec) const;
  std::optional<Ref> bind(const std::optional<Ref> &self, const Activation &caller) const;

private:
  Analysis &analysis_;
};

}
