#include "compiler/cli/args/args.h"
#include "compiler/cli/exit_codes/exit_codes.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/Triple.h"

namespace tz {

namespace {

const char kUsage[] = R"(usage: tz <command> [options] [<file.tz>] [arguments]

commands:
  build     compile a program into a native executable
  run       compile a program and run it; arguments after the file go to it
  init      start a project: tz.json, main.tz and .gitignore
  add       add packages to tz.json: tz add zod, tz add jsr:@std/path@^1,
            tz add github:user/repo@v1.2.0, tz add ../lib
  remove    remove packages from tz.json
  update    move packages to the newest versions tz.json allows
  port      copy a TypeScript library and adapt it to tz
  licenses  print the licenses of the code of others an executable embeds,
            which go along with it: tz licenses app.exe > LICENSES.txt
  lsp       the language server: an editor starts it to show errors as you
            type (the VS Code extension of editors/vscode does)
  map       an overview of a program: modules, what they declare with types,
            how much each is used: tz map [--budget <tokens>] [--json]
  context   all about a symbol or place: source, uses, users, docs:
            tz context parseAge, tz context main.tz:12 [--budget <tokens>]
  query     tz query definition|references|callers|type <name | file.tz:line:col>
  impact    what a change to a symbol can affect: tz impact Vec.add
  mcp       the tools above for AI agents (MCP over stdin and stdout)
  version   print the version

Without a file, build and run take "main" of tz.json; map, context, query and
impact take it too, or the program named by --entry <file.tz>.

options:
  -o <path>         where to write the output (build only)
  --emit-llvm       write LLVM IR (.ll) instead of an executable (build only)
  --explain-parallel
                    print what runs in parallel and why, instead of building
                    (build only)
)";

}

int Args::usageError(const llvm::Twine &message) {
  llvm::errs() << "tz: " << message << "\n\n" << kUsage;

  return kExitUsage;
}

void Args::printUsage() { llvm::outs() << kUsage; }

int Args::parse(llvm::ArrayRef<const char *> args) {
  if (args.empty()) {
    llvm::errs() << kUsage;
    return kExitUsage;
  }

  llvm::StringRef name = args[0];
  if (name == "help" || name == "-h" || name == "--help") {
    command = Command::Help;
    return -1;
  }
  if (name == "version" || name == "--version") {
    command = Command::Version;
    return -1;
  }
  if (name == "build") {
    command = Command::Build;
  } else if (name == "run") {
    command = Command::Run;
  } else if (name == "licenses") {
    command = Command::Licenses;
  } else {
    return usageError("unknown command '" + name + "'");
  }

  bool rest = false;
  for (size_t i = 1; i < args.size(); ++i) {
    llvm::StringRef arg = args[i];
    if (!input.empty() || rest) {
      programArgs.push_back(arg.str());
    } else if (arg == "--" && command == Command::Run) {
      rest = true;
    } else if (arg == "-o") {
      if (++i == args.size()) {
        return usageError("'-o' needs a path");
      }
      output = args[i];
    } else if (arg == "-O0") {
      optimize = false;
    } else if (arg == "--emit-llvm") {
      emitLLVM = true;
    } else if (arg == "--explain-parallel") {
      explainParallel = true;
    } else if (arg.starts_with("-")) {
      return usageError("unknown option '" + arg + "'");
    } else if (command == Command::Run && !arg.ends_with_insensitive(".tz") &&
               !arg.ends_with_insensitive(".ts")) {
      programArgs.push_back(arg.str());
      rest = true;
    } else {
      input = arg.str();
    }
  }

  if (command == Command::Licenses) {
    if (input.empty()) {
      return usageError("no executable given");
    }
    if (!programArgs.empty() || emitLLVM || !output.empty()) {
      return usageError("'tz licenses' takes one executable");
    }
  }
  if (command == Command::Build && !programArgs.empty()) {
    return usageError("unexpected argument '" + programArgs.front() + "' after the source file");
  }
  if (command == Command::Run && (emitLLVM || !output.empty())) {
    return usageError("'-o' and '--emit-llvm' only apply to 'tz build'");
  }
  if (command == Command::Run && explainParallel) {
    return usageError("'--explain-parallel' only applies to 'tz build'");
  }
  if (explainParallel && (emitLLVM || !output.empty())) {
    return usageError("'--explain-parallel' builds nothing, so it takes no '-o' or '--emit-llvm'");
  }

  return -1;
}

std::string Args::outputPath() const {
  if (!output.empty()) {
    return output;
  }
  llvm::SmallString<128> name(outputStem.empty() ? llvm::sys::path::stem(input)
                                                 : llvm::StringRef(outputStem));
  if (emitLLVM) {
    name += ".ll";
  } else if (llvm::Triple(llvm::sys::getDefaultTargetTriple()).isOSWindows()) {
    name += ".exe";
  }

  return std::string(name);
}

}
