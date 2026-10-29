#include "compiler/cli/lsp_command/lsp_command.h"
#include "compiler/cli/args/args.h"
#include "compiler/lsp/lsp_channel/lsp_channel.h"
#include "compiler/lsp/lsp_check/lsp_check.h"
#include "compiler/lsp/lsp_server/lsp_server.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/FileSystem.h"
#include <cstdint>

namespace tz {

LspCommand::LspCommand(const char *argv0) : argv0_(argv0) {}

int LspCommand::run(llvm::ArrayRef<const char *> args) const {
  if (args.size() == 3 && llvm::StringRef(args[0]) == "--check") {
    LspCheck check(argv0_);
    return check.run(args[1], args[2]);
  }
  for (const char *arg : args) {
    if (llvm::StringRef(arg) != "--stdio") {
      return Args::usageError("'tz lsp' takes no arguments: an editor starts it and talks to it "
                              "over stdin and stdout");
    }
  }
  std::string self = llvm::sys::fs::getMainExecutable(argv0_, (void *)(intptr_t)&Args::printUsage);
  LspChannel channel;
  LspServer server(channel, self);

  return server.run();
}

}
