#pragma once

#include "compiler/parallel/analysis/analysis.h"

namespace tz::parallel {

// What a call reaches, with the references of its arguments put in.
class CallResolver {
public:
  explicit CallResolver(Analysis &analysis);

  void resolve();
  void fold();
  std::optional<Access> substitute(const Access &a, const FnInfo &callee,
                                   const std::optional<Ref> &self, const CallRec &rec) const;
  std::optional<Ref> bind(const std::optional<Ref> &self, const Activation &caller) const;

private:
  Analysis &analysis_;
};

} // namespace tz::parallel
