#include "compiler/cli/remove_command/remove_command.h"
#include "compiler/cli/command_usage/command_usage.h"
#include "compiler/cli/exit_codes/exit_codes.h"
#include "compiler/cli/project_file/project_file.h"
#include "compiler/packages/dependency/dependency.h"
#include "compiler/packages/manifest/manifest.h"
#include "compiler/packages/prepare_options/prepare_options.h"
#include "compiler/packages/report/report.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

namespace tz::pkg {

int RemoveCommand::run(llvm::ArrayRef<const char *> args) const {
  if (args.empty()) {
    return usage("'tz remove' needs the name of a package", "tz remove <name>...");
  }
  Manifest project;
  if (!currentProject(project)) {
    return kExitError;
  }
  std::vector<Dependency> deps = project.dependencies;
  for (llvm::StringRef name : args) {
    auto it = std::find_if(deps.begin(), deps.end(),
                           [&](const Dependency &d) { return d.alias == name; });
    if (it == deps.end()) {
      report("'" + name + "' is not a dependency in " + project.path);
      return kExitError;
    }
    deps.erase(it);
  }
  if (!applyDependencies(project, deps, PrepareOptions(), nullptr, nullptr)) {
    return kExitError;
  }
  for (llvm::StringRef name : args) {
    llvm::outs() << "removed " << name << "\n";
  }

  return 0;
}

}
