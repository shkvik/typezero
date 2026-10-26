#include "compiler/cli/run_command/run_command.h"
#include "compiler/cli/args/args.h"
#include "compiler/cli/build_command/build_command.h"
#include "compiler/cli/exit_codes/exit_codes.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"
#include <string>
#include <vector>

namespace tz {

RunCommand::RunCommand(const RuntimeFiles &runtime) : runtime_(runtime) {}

int RunCommand::run(const Args &args) const {
  llvm::SmallString<128> exe;
  if (std::error_code ec = llvm::sys::fs::createTemporaryFile("tz-run", "exe", exe)) {
    llvm::errs() << "tz: cannot create a temporary file: " << ec.message() << "\n";
    return kExitError;
  }
  llvm::FileRemover removeExe(exe);
  BuildCommand build(runtime_);
  if (int status = build.run(args, exe)) {
    return status;
  }

  std::vector<llvm::StringRef> programArgs{exe};
  for (const std::string &arg : args.programArgs) {
    programArgs.push_back(arg);
  }
  std::string error;
  int code = llvm::sys::ExecuteAndWait(exe, programArgs, std::nullopt, {}, 0, 0, &error);
  if (code == -1) {
    llvm::errs() << "tz: cannot run the program: " << error << "\n";
    return kExitError;
  }
  if (code == -2) {
    llvm::errs() << "tz: the program crashed: " << error << "\n";
    return 2;
  }

  return code;
}

}
