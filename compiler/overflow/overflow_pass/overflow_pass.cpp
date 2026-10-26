#include "compiler/overflow/overflow_pass/overflow_pass.h"
#include "compiler/overflow/overflow_check/overflow_check.h"
#include "compiler/program/program/program.h"

namespace tz {

Errors OverflowPass::run(Program &program) const {
  checkIntOverflow(program.file, *program.diags);

  return program.diags->take();
}

}
