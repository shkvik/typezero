#pragma once

#include "compiler/program/errors/errors.h"

namespace tz {

struct EventInference;
class FeatureRegistry;
struct Program;

// The declarations of every file: classes, functions, types, imports and exports, and the
// prelude of the language (Error and its kinds).
class DeclarePass {
public:
  DeclarePass(EventInference &inference, const FeatureRegistry &features);

  Errors run(Program &program) const;

private:
  EventInference &inference_;
  const FeatureRegistry &features_;
};

} // namespace tz
