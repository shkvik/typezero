#pragma once

#include "compiler/program/diagnostics/diagnostics.h"

namespace tz {

struct BuildOptions;
struct Program;

class LoadPass {
public:
  LoadPass(Program &program, const BuildOptions &options);

  Diagnostics::ResultTaken run();

private:
  Program &program;
  const BuildOptions &options;
};

}
