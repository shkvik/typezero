#pragma once

#include "compiler/program/errors/errors.h"

namespace tz {

class CheckState;

class InferPass {
public:
  explicit InferPass(CheckState &owner);

  Errors run() const;

private:
  CheckState &state;
};

}
