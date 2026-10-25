#include "pkg/editor.h"

#include "pkg/fetch.h"
#include "pkg/json.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace tz::pkg {

namespace fs = llvm::sys::fs;
namespace path = llvm::sys::path;

namespace {

std::string &typesDirectory() {
  static std::string dir;
  return dir;
}

// `file` in `dir`, with '/' and without "./" or "..".
std::string joined(llvm::StringRef dir, llvm::StringRef file) {
  llvm::SmallString<256> result(dir);
  path::append(result, file);
  path::remove_dots(result, /*remove_dot_dot=*/true);
  return path::convert_to_slash(result);
}

// What imports of a package can name, as paths of tsconfig.json: each
// specifier and the files it stands for, the first that exists counting.
using Paths = std::vector<std::pair<std::string, std::vector<std::string>>>;

// The names of the package `alias` of the project, as the loader finds them
// (src/driver/loader.cpp, Loader::resolvePackage).
void addPackage(Paths &paths, const std::string &alias, const Package &package) {
  if (!package.exports.empty()) {
    for (const auto &[key, file] : package.exports)
      paths.push_back({alias + (key == "." ? std::string() : key.substr(1)),
                       {joined(package.dir, file)}});
    return;
  }
  // The file the loader would take, or, while there is none, all it looks
  // for.
  std::vector<std::string> mains;
  if (!package.main.empty())
    mains.push_back(joined(package.dir, package.main));
  else
    for (const char *file : {"mod.ts", "index.ts", "src/index.ts", "src/mod.ts"})
      mains.push_back(joined(package.dir, file));
  auto found = std::find_if(mains.begin(), mains.end(),
                            [](const std::string &file) { return fs::is_regular_file(file); });
  if (found != mains.end())
    mains = {*found};
  paths.push_back({alias, mains});
  // A path inside it: tsc adds .ts or /index.ts as the loader does.
  paths.push_back({alias + "/*", {joined(package.dir, "*")}});
}

std::string editorConfig(const Graph *graph) {
  const std::string &types = typesDirectory();
  Paths paths;
  paths.push_back({"neverthrow", {joined(types, "node_modules/neverthrow/index.d.ts")}});
  if (graph && !graph->packages.empty()) {
    const Package &project = graph->packages[0];
    std::vector<std::pair<std::string, unsigned>> aliases;
    for (const auto &entry : project.aliases)
      aliases.push_back({entry.getKey().str(), entry.getValue()});
    std::sort(aliases.begin(), aliases.end());
    for (const auto &[alias, index] : aliases)
      addPackage(paths, alias, graph->packages[index]);
  }

  std::string text =
      "// Written by tz for the editor (tsc in VS Code), and again whenever tz builds\n"
      "// the project or changes its packages: what tz has built in, and where the\n"
      "// packages are on this computer. tsconfig.json extends it; git does not keep it.\n"
      "{\n"
      "  \"extends\": " +
      jsonQuote(joined(types, "tsconfig.json")) +
      ",\n"
      "  \"compilerOptions\": {\n"
      "    \"paths\": {\n";
  for (size_t i = 0; i < paths.size(); ++i) {
    text += "      " + jsonQuote(paths[i].first) + ": [";
    for (size_t j = 0; j < paths[i].second.size(); ++j)
      text += (j == 0 ? "" : ", ") + jsonQuote(paths[i].second[j]);
    text += i + 1 == paths.size() ? "]\n" : "],\n";
  }
  text += "    }\n"
          "  },\n"
          "  \"include\": [\"../**/*.ts\"]\n"
          "}\n";
  return text;
}

} // namespace

void setTypesDir(std::string dir) { typesDirectory() = std::move(dir); }

const char kProjectTsconfig[] =
    "// The editor's view of the project (tsc in VS Code): what tz has built in and\n"
    "// where the packages are come from .tz/tsconfig.json, which tz writes.\n"
    "{\n"
    "  \"extends\": \"./.tz/tsconfig.json\"\n"
    "}\n";

bool editorSetUp(llvm::StringRef dir) {
  auto buffer = llvm::MemoryBuffer::getFile(joined(dir, "tsconfig.json"));
  return buffer && (*buffer)->getBuffer().contains(".tz/tsconfig.json");
}

bool writeEditorConfig(llvm::StringRef dir, const Graph *graph) {
  std::string file = joined(dir, ".tz/tsconfig.json");
  std::string text = editorConfig(graph);
  if (auto buffer = llvm::MemoryBuffer::getFile(file); buffer && (*buffer)->getBuffer() == text)
    return true;
  std::string error;
  if (writeFileAtomically(file, text, error))
    return true;
  report(error);
  return false;
}

} // namespace tz::pkg
