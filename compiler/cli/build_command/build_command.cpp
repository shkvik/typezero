#include "compiler/cli/build_command/build_command.h"
#include "compiler/cli/args/args.h"
#include "compiler/cli/error_printer/error_printer.h"
#include "compiler/cli/exit_codes/exit_codes.h"
#include "compiler/cli/project_packages/project_packages.h"
#include "compiler/pipeline/build_options/build_options.h"
#include "compiler/pipeline/pipeline/pipeline.h"
#include "compiler/program/program/program.h"

namespace tz {

BuildCommand::BuildCommand(const RuntimeFiles &runtime) : runtime_(runtime) {}

int BuildCommand::run(const Args &args, llvm::StringRef output) const {
  ProjectPackages packages;
  if (!packages.prepare(args.input)) {
    return kExitError;
  }
  BuildOptions options{args.input,    output.str(),     args.optimize,
                       args.emitLLVM, packages.graph(), runtime_};
  Program program;
  ErrorPrinter printer(program.sourceMgr);

  Errors checkErrors = checkProgram(program, options);
  printer.print(checkErrors);
  if (checkErrors) {
    return kExitError;
  }

  Errors buildErrors = buildProgram(program, options);
  printer.print(buildErrors);
  if (buildErrors) {
    return kExitError;
  }

  return 0;
}

}
