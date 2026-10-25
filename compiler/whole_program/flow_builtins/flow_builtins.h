#pragma once

#include "compiler/whole_program/flows/flows.h"

namespace tz {

// The flows of the built-in functions and methods.
class FlowBuiltins {
public:
  explicit FlowBuiltins(Flows &flows);

  Refs builtin(CallExpr *c, MemberExpr *member, const Refs &receiver,
               const std::vector<Refs> &args);

private:
  Flows &flows_;
};

} // namespace tz
