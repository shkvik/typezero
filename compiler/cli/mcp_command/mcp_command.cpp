#include "compiler/cli/mcp_command/mcp_command.h"
#include "compiler/cli/args/args.h"
#include "compiler/mcp/mcp_server/mcp_server.h"
#include "llvm/Support/FileSystem.h"
#include <cstdint>

namespace tz {

McpCommand::McpCommand(const char *argv0) : argv0_(argv0) {}

int McpCommand::run(llvm::ArrayRef<const char *> args) const {
  if (!args.empty()) {
    return Args::usageError("'tz mcp' takes no arguments: an AI agent starts it and talks to it "
                            "over stdin and stdout");
  }
  std::string self = llvm::sys::fs::getMainExecutable(argv0_, (void *)(intptr_t)&Args::printUsage);
  McpServer server(self);

  return server.run();
}

}
