#include "compiler/cli/version_command/version_command.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/Support/raw_ostream.h"

namespace tz {

int VersionCommand::run() const {
  llvm::outs() << "tz version " TZ_VERSION " (LLVM " LLVM_VERSION_STRING ")\n";

  return 0;
}

}
