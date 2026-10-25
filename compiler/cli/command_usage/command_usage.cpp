#include "compiler/cli/command_usage/command_usage.h"
#include "compiler/cli/exit_codes/exit_codes.h"
#include "llvm/Support/raw_ostream.h"

namespace tz::pkg {

int usage(const llvm::Twine &message, llvm::StringRef synopsis) {
  llvm::errs() << "tz: " << message << "\n\nusage: " << synopsis << "\n";

  return kExitUsage;
}

} // namespace tz::pkg
