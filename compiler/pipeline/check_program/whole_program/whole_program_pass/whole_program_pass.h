#pragma once

#include "compiler/program/errors/errors.h"

namespace tz {

class CheckState;
struct Program;

class WholeProgramPass {
public:
  WholeProgramPass(CheckState &owner, Program &checked);

  Errors run() const;

private:
  CheckState &state;
  Program &program;
};

}
