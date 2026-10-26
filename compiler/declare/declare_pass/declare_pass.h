#pragma once

#include "compiler/program/errors/errors.h"

namespace tz {

struct EventInference;
class FeatureRegistry;
struct Program;

class DeclarePass {
public:
  DeclarePass(EventInference &inference, const FeatureRegistry &features);

  Errors run(Program &program) const;

private:
  EventInference &inference_;
  const FeatureRegistry &features_;
};

}
