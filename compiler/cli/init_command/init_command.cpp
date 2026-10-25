#include "compiler/cli/init_command/init_command.h"
#include "compiler/cli/command_usage/command_usage.h"
#include "compiler/cli/exit_codes/exit_codes.h"
#include "compiler/cli/project_file/project_file.h"
#include "compiler/packages/dependency/dependency.h"
#include "compiler/packages/json_text/json_text.h"
#include "compiler/packages/manifest/manifest.h"
#include "compiler/packages/report/report.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

namespace tz::pkg {

namespace fs = llvm::sys::fs;
namespace path = llvm::sys::path;

int InitCommand::run(llvm::ArrayRef<const char *> args) const {
  if (args.size() > 1) {
    return usage("'tz init' takes one directory", "tz init [directory]");
  }
  llvm::SmallString<256> dir;
  if (args.empty()) {
    fs::current_path(dir);
  } else {
    dir = args[0];
    fs::make_absolute(dir);
    if (std::error_code ec = fs::create_directories(dir)) {
      report("cannot create '" + dir + "': " + ec.message());
      return kExitError;
    }
  }
  path::remove_dots(dir, /*remove_dot_dot=*/true);
  auto file = [&](llvm::StringRef name) {
    llvm::SmallString<256> result(dir);
    path::append(result, name);

    return std::string(result);
  };
  if (fs::exists(file("tz.json"))) {
    report("there is a tz.json already in " + dir);
    return kExitError;
  }
  std::string name = path::filename(dir).str();
  std::string error;
  if (!checkAlias(name, error)) {
    name = "app";
  }
  std::vector<std::string> created{"tz.json"};
  if (!writeText(file("tz.json"), "{\n  \"name\": " + jsonQuote(name) +
                                      ",\n  \"main\": \"main.tz\",\n  \"dependencies\": {}\n}\n")) {
    return kExitError;
  }
  if (!fs::exists(file("main.tz"))) {
    if (!writeText(file("main.tz"), "console.log(\"Hello, world!\")\n")) {
      return kExitError;
    }
    created.push_back("main.tz");
  }
  if (!fs::exists(file(".gitignore"))) {
    // What tz build makes. tz.lock belongs in git: it makes every build use
    // the same sources.
    if (!writeText(file(".gitignore"), "*.exe\n*.ll\n/" + name + "\n")) {
      return kExitError;
    }
    created.push_back(".gitignore");
  }
  llvm::outs() << "created ";
  for (size_t i = 0; i < created.size(); ++i) {
    llvm::outs() << (i == 0 ? "" : i + 1 == created.size() ? " and " : ", ") << created[i];
  }
  llvm::outs() << (args.empty() ? std::string() : " in " + std::string(args[0]))
               << "; run the program with 'tz run'\n";

  return 0;
}

} // namespace tz::pkg
