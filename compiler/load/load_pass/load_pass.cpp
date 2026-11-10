#include "compiler/load/load_pass/load_pass.h"
#include "compiler/load/loader/loader.h"
#include "compiler/pipeline/build_options/build_options.h"
#include "compiler/program/program/program.h"

namespace tz {

LoadPass::LoadPass(Program &program, const BuildOptions &options)
    : program(program),
      options(options) {}

Diagnostics::ResultTaken LoadPass::run() {
  this->program.file = SourceFile();
  loadProgram(this->options.input, this->program.sourceMgr, this->program.ast, this->program.diags,
              this->program.file, this->options.packages, this->options.overlay);

  return this->program.diags.take();
}

}
