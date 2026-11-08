#include "compiler/cli/lsp/lsp_command/lsp_command.h"
#include "compiler/cli/command_line/args/args.h"
#include "compiler/cli/command_line/exit_codes/exit_codes.h"
#include "compiler/lsp/lsp_channel/lsp_channel.h"
#include "compiler/lsp/lsp_check/lsp_check.h"
#include "compiler/lsp/lsp_server/lsp_server.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"
#include <cstdint>

namespace tz {

LspCommand::LspCommand(const char *argv0)
    : argv0(argv0) {}

int LspCommand::run(llvm::ArrayRef<const char *> args) const {
  if (args.size() == 3 && llvm::StringRef(args[0]) == "--check") {
    LspCheck check(this->argv0);
    LspCheck::ResultResponseFile checked = check.run(args[1], args[2]);
    if (checked.isErr()) {
      llvm::errs() << "tz: " << checked.error().message << "\n";

      return kExitError;
    }

    return 0;
  }
  for (const char *arg : args) {
    if (llvm::StringRef(arg) != "--stdio") {
      return Args::usageError("'tz lsp' takes no arguments: an editor starts it and talks to it "
                              "over stdin and stdout");
    }
  }
  std::string self =
      llvm::sys::fs::getMainExecutable(this->argv0, (void *)(intptr_t)&Args::printUsage);
  LspChannel channel;
  LspServer server(channel, self);

  return server.run();
}

}
