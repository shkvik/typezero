#pragma once

#include "compiler/program/errors/errors.h"

namespace tz {

class CheckState;

class InstancesPass {
public:
  explicit InstancesPass(CheckState &owner);

  Errors run() const;

private:
  CheckState &state;
};

}
