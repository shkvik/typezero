#include "compiler/cli/build/build_command/build_command.h"
#include "compiler/cli/command_line/args/args.h"
#include "compiler/cli/command_line/error_printer/error_printer.h"
#include "compiler/cli/command_line/exit_codes/exit_codes.h"
#include "compiler/packages/project_packages/project_packages.h"
#include "compiler/pipeline/build_options/build_options.h"
#include "compiler/pipeline/check_program/check_program.h"
#include "compiler/pipeline/pipeline/pipeline.h"
#include "compiler/program/program/program.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

namespace tz {

BuildCommand::BuildCommand(const RuntimeFiles &runtime)
    : runtime(runtime) {}

int BuildCommand::run(const Args &args, llvm::StringRef output) const {
  ProjectPackages packages;
  if (!packages.prepare(args.input)) {
    return kExitError;
  }
  BuildOptions options{args.input,    output.str(),     args.optimize,
                       args.emitLLVM, packages.graph(), this->runtime};
  options.debug = args.debug ? DebugLevel::Full : DebugLevel::Lines;
  Program program;
  ErrorPrinter printer(program.sourceMgr, args.hints);

  Errors checkErrors = checkProgram(program, options);
  printer.print(checkErrors);
  if (checkErrors) {
    return kExitError;
  }

  llvm::StringRef dir = llvm::sys::path::parent_path(output);
  if (!dir.empty()) {
    if (std::error_code ec = llvm::sys::fs::create_directories(dir)) {
      llvm::errs() << "tz: cannot create the directory " << dir << ": " << ec.message() << "\n";
      return kExitError;
    }
  }
  Errors buildErrors = buildProgram(program, options);
  printer.print(buildErrors);
  if (buildErrors) {
    return kExitError;
  }

  return 0;
}

}
