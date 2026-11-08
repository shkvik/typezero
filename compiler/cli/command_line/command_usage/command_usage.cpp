#include "compiler/cli/command_line/command_usage/command_usage.h"
#include "compiler/cli/command_line/exit_codes/exit_codes.h"
#include "llvm/Support/raw_ostream.h"

namespace tz::pkg {

int CommandUsage::usage(const llvm::Twine &message, llvm::StringRef synopsis) {
  llvm::errs() << "tz: " << message << "\n\nusage: " << synopsis << "\n";

  return kExitUsage;
}

}
