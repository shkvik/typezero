#include "compiler/cli/command_line/command_line.h"
#include "compiler/cli/add_command/add_command.h"
#include "compiler/cli/args/args.h"
#include "compiler/cli/build_command/build_command.h"
#include "compiler/cli/entry_file/entry_file.h"
#include "compiler/cli/explain_command/explain_command.h"
#include "compiler/cli/init_command/init_command.h"
#include "compiler/cli/knowledge_command/knowledge_command.h"
#include "compiler/cli/licenses_command/licenses_command.h"
#include "compiler/cli/lsp_command/lsp_command.h"
#include "compiler/cli/mcp_command/mcp_command.h"
#include "compiler/cli/port_command/port_command.h"
#include "compiler/cli/remove_command/remove_command.h"
#include "compiler/cli/run_command/run_command.h"
#include "compiler/cli/update_command/update_command.h"
#include "compiler/cli/version_command/version_command.h"
#include "compiler/pipeline/runtime_files/runtime_files.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include <vector>

namespace tz {

int CommandLine::run(int argc, char **argv) const {
  std::vector<const char *> words(argv + 1, argv + argc);
  if (!words.empty()) {
    llvm::StringRef name = words[0];
    llvm::ArrayRef<const char *> rest = llvm::ArrayRef(words).drop_front();
    if (name == "init") {
      pkg::InitCommand command;
      return command.run(rest);
    }
    if (name == "add") {
      pkg::AddCommand command;
      return command.run(rest);
    }
    if (name == "remove") {
      pkg::RemoveCommand command;
      return command.run(rest);
    }
    if (name == "update") {
      pkg::UpdateCommand command;
      return command.run(rest);
    }
    if (name == "port") {
      pkg::PortCommand command;
      return command.run(rest);
    }
    if (name == "lsp") {
      LspCommand command(argv[0]);
      return command.run(rest);
    }
    if (name == "mcp") {
      McpCommand command(argv[0]);
      return command.run(rest);
    }
    if (name == "map" || name == "context" || name == "query" || name == "impact") {
      KnowledgeCommand command(argv[0], name);
      return command.run(rest);
    }
  }

  Args args;
  if (int code = args.parse(words); code >= 0) {
    return code;
  }
  if (args.command == Args::Command::Help) {
    Args::printUsage();
    return 0;
  }
  if (args.command == Args::Command::Version) {
    VersionCommand version;
    return version.run();
  }
  EntryFile entry;
  if (int code = entry.find(args); code >= 0) {
    return code;
  }

  RuntimeFiles runtime(argv[0]);
  if (args.command == Args::Command::Licenses) {
    LicensesCommand licenses(runtime);
    return licenses.run(args);
  }
  if (args.explainParallel) {
    ExplainCommand explain(runtime);
    return explain.run(args);
  }
  if (args.command == Args::Command::Run) {
    RunCommand run(runtime);
    return run.run(args);
  }
  BuildCommand build(runtime);

  return build.run(args, args.outputPath());
}

}
