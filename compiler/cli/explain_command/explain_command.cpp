#include "compiler/cli/explain_command/explain_command.h"
#include "compiler/cli/args/args.h"
#include "compiler/cli/error_printer/error_printer.h"
#include "compiler/cli/exit_codes/exit_codes.h"
#include "compiler/cli/project_packages/project_packages.h"
#include "compiler/parallel/parallel_analysis/parallel_analysis.h"
#include "compiler/pipeline/build_options/build_options.h"
#include "compiler/pipeline/pipeline/pipeline.h"
#include "compiler/program/program/program.h"
#include "llvm/Support/raw_ostream.h"

namespace tz {

ExplainCommand::ExplainCommand(const RuntimeFiles &runtime) : runtime_(runtime) {}

int ExplainCommand::run(const Args &args) const {
  ProjectPackages packages;
  if (!packages.prepare(args.input)) {
    return kExitError;
  }
  BuildOptions options{args.input, "", args.optimize, false, packages.graph(), runtime_};
  Program program;
  ErrorPrinter printer(program.sourceMgr);

  Errors checkErrors = checkProgram(program, options);
  printer.print(checkErrors);
  if (checkErrors) {
    return kExitError;
  }
  explainParallel(program.file, program.sourceMgr, llvm::outs());

  return 0;
}

} // namespace tz
