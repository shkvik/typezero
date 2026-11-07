#include "compiler/packages/manifest/manifest.h"
#include "compiler/packages/dependency/dependency.h"
#include "compiler/packages/json_parser/json_parser.h"
#include "compiler/packages/json_text/json_text.h"
#include "compiler/packages/json_value/json_value.h"
#include "compiler/packages/paths/paths.h"
#include "compiler/packages/report/report.h"
#include "compiler/packages/source/source.h"
#include "compiler/packages/text_file/text_file.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>

namespace tz::pkg {

namespace fs = llvm::sys::fs;
namespace path = llvm::sys::path;

bool ManifestFile::readExports(const Manifest &out, const JsonValue &value,
                               std::vector<std::pair<std::string, std::string>> &exports) {
  auto add = [&](llvm::StringRef key, const JsonValue &target) {
    if (!target.isString()) {
      Report::reportAt(out.path, out.text, target.begin,
                       "an export names a .tz file of the package");
      return false;
    }
    llvm::StringRef file = target.string();
    file.consume_front("./");
    exports.push_back({key.str(), file.str()});

    return true;
  };
  if (value.isString()) {
    return add(".", value);
  }
  if (!value.isObject()) {
    Report::reportAt(out.path, out.text, value.begin,
                     "'exports' is a file, or an object such as { \".\": \"./mod.tz\", \"./mini\": "
                     "\"./mini.tz\" }");
    return false;
  }
  for (size_t i = 0; i < value.keys().size(); ++i) {
    llvm::StringRef key = value.keys()[i];
    if (key != "." && !key.starts_with("./")) {
      Report::reportAt(
          out.path, out.text, value.keyOffsets()[i],
          "an export is named '.' or './name', as imports write it after the package's "
          "name");
      return false;
    }
    if (!add(key, value.items()[i])) {
      return false;
    }
  }

  return true;
}

bool ManifestFile::parseManifest(Manifest &out) {
  JsonValue root;
  size_t at;
  std::string error;
  if (!JsonParser::parseJson(out.text, root, at, error)) {
    Report::reportAt(out.path, out.text, at, error);
    return false;
  }
  if (!root.isObject()) {
    Report::reportAt(out.path, out.text, root.begin,
                     "package.json holds an object: { \"name\": ..., "
                     "\"dependencies\": { ... } }");
    return false;
  }
  out.rootBegin = root.begin;
  out.rootEnd = root.end;
  if (!root.keys().empty()) {
    out.lastMemberEnd = root.items().back().end;
    size_t lineStart = out.text.rfind('\n', root.keyOffsets()[0]);
    lineStart = lineStart == std::string::npos ? 0 : lineStart + 1;
    llvm::StringRef lead = llvm::StringRef(out.text).slice(lineStart, root.keyOffsets()[0]);
    if (!lead.empty() && llvm::all_of(lead, [](char c) { return c == ' ' || c == '\t'; })) {
      out.indent = lead.str();
    }
  }
  for (size_t i = 0; i < root.keys().size(); ++i) {
    llvm::StringRef key = root.keys()[i];
    const JsonValue &value = root.items()[i];
    if (key == "name" || key == "version" || key == "main") {
      if (!value.isString()) {
        Report::reportAt(out.path, out.text, value.begin, "'" + key + "' is a string");
        return false;
      }
      (key == "name" ? out.name : key == "version" ? out.version : out.main) = value.string();
      if (key == "main") {
        out.mainOffset = value.begin;
      }
    } else if (key == "scripts") {
      if (!value.isObject()) {
        Report::reportAt(
            out.path, out.text, value.begin,
            "'scripts' is an object: { \"start\": \"tz run\", \"build\": \"tz build\" }");
        return false;
      }
      for (size_t j = 0; j < value.keys().size(); ++j) {
        const JsonValue &command = value.items()[j];
        if (!command.isString()) {
          Report::reportAt(out.path, out.text, command.begin,
                           "a script is a command line, e.g. \"tz build\"");
          return false;
        }
        out.scripts.push_back({value.keys()[j], command.string()});
      }
    } else if (key == "exports") {
      if (!ManifestFile::readExports(out, value, out.exports)) {
        return false;
      }
    } else if (key == "dependencies") {
      if (!value.isObject()) {
        Report::reportAt(out.path, out.text, value.begin,
                         "'dependencies' is an object: { \"zod\": \"jsr:@tz/zod@^4\" }");
        return false;
      }
      out.depsBegin = value.begin;
      out.depsEnd = value.end;
      for (size_t j = 0; j < value.keys().size(); ++j) {
        if (!DependencyAlias::checkAlias(value.keys()[j], error)) {
          Report::reportAt(out.path, out.text, value.keyOffsets()[j], error);
          return false;
        }
        const JsonValue &spec = value.items()[j];
        if (!spec.isString()) {
          Report::reportAt(out.path, out.text, spec.begin,
                           "a dependency is a string that says where the package comes from, e.g. "
                           "\"jsr:@std/path@^1\"");
          return false;
        }
        out.dependencies.push_back({value.keys()[j], spec.string(), spec.begin});
      }
    }
  }

  return true;
}

std::string ManifestFile::findManifest(llvm::StringRef dir) {
  return Paths::findAbove(dir, "package.json");
}

bool ManifestFile::readManifest(llvm::StringRef file, Manifest &out) {
  std::string text;

  return TextFile::readText(file, text) &&
         ManifestFile::readManifestText(file, std::move(text), out);
}

bool ManifestFile::readManifestText(llvm::StringRef file, std::string text, Manifest &out) {
  out = Manifest();
  out.path = file.str();
  out.dir = Paths::canonical(path::parent_path(file));
  out.isPackageJson = true;
  out.text = std::move(text);

  return ManifestFile::parseManifest(out);
}

bool ManifestFile::namesTzFiles(llvm::StringRef packageJson) {
  std::string text;
  if (!TextFile::readText(packageJson, text)) {
    return false;
  }
  JsonValue root;
  size_t at;
  std::string error;
  if (!JsonParser::parseJson(text, root, at, error) || !root.isObject()) {
    return true;
  }
  const JsonValue *main = root.get("main");
  const JsonValue *exports = root.get("exports");
  if (!main && !exports) {
    return true;
  }
  auto tz = [](const JsonValue *value) {
    return value && value->isString() && llvm::StringRef(value->string()).ends_with(".tz");
  };
  if (tz(main) || tz(exports)) {
    return true;
  }
  if (exports && exports->isObject()) {
    return llvm::any_of(exports->items(), [&](const JsonValue &target) { return tz(&target); });
  }

  return false;
}

bool ManifestFile::readPackageDir(llvm::StringRef dir, Manifest &out) {
  llvm::SmallString<256> file(dir);
  path::append(file, "package.json");
  if (fs::is_regular_file(file) && ManifestFile::namesTzFiles(file)) {
    return ManifestFile::readManifest(file, out);
  }
  out = Manifest();
  out.dir = Paths::canonical(dir);
  for (const char *name : {"jsr.json", "deno.json", "deno.jsonc"}) {
    file = dir;
    path::append(file, name);
    if (!fs::is_regular_file(file)) {
      continue;
    }
    out.path = std::string(file);
    if (!TextFile::readText(file, out.text)) {
      return false;
    }
    JsonValue root;
    size_t at;
    std::string error;
    if (!JsonParser::parseJson(out.text, root, at, error)) {
      Report::reportAt(out.path, out.text, at, error);
      return false;
    }
    out.name = root.getString("name");
    out.version = root.getString("version");
    if (const JsonValue *exports = root.get("exports")) {
      if (!ManifestFile::readExports(out, *exports, out.exports)) {
        return false;
      }
    }
    break;
  }

  return true;
}

std::string ManifestFile::withDependencies(const Manifest &m, std::vector<Dependency> deps) {
  std::sort(deps.begin(), deps.end(),
            [](const Dependency &a, const Dependency &b) { return a.alias < b.alias; });
  const std::string &indent = m.indent;
  std::string object = "{}";
  if (!deps.empty()) {
    object = "{\n";
    for (size_t i = 0; i < deps.size(); ++i) {
      object += indent + indent + JsonText::jsonQuote(deps[i].alias) + ": " +
                JsonText::jsonQuote(deps[i].spec) + (i + 1 < deps.size() ? ",\n" : "\n");
    }
    object += indent + "}";
  }
  const std::string &text = m.text;
  if (m.depsBegin != std::string::npos) {
    return text.substr(0, m.depsBegin) + object + text.substr(m.depsEnd);
  }
  if (m.lastMemberEnd != std::string::npos) {
    return text.substr(0, m.lastMemberEnd) + ",\n" + indent + "\"dependencies\": " + object +
           text.substr(m.lastMemberEnd);
  }

  return text.substr(0, m.rootBegin) + "{\n" + indent + "\"dependencies\": " + object + "\n}" +
         text.substr(m.rootEnd);
}

}
