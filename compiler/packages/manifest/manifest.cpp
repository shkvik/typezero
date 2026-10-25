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

std::string findManifest(llvm::StringRef dir) {
  llvm::SmallString<256> current(dir);
  if (current.empty()) {
    fs::current_path(current);
  }
  fs::make_absolute(current);
  path::remove_dots(current, /*remove_dot_dot=*/true);
  while (!current.empty()) {
    llvm::SmallString<256> candidate(current);
    path::append(candidate, "tz.json");
    if (fs::is_regular_file(candidate)) {
      return std::string(candidate);
    }
    llvm::StringRef parent = path::parent_path(current);
    if (parent == current) {
      break;
    }
    current = parent;
  }

  return "";
}

namespace {

bool readExports(const Manifest &out, const JsonValue &value,
                 std::vector<std::pair<std::string, std::string>> &exports) {
  auto add = [&](llvm::StringRef key, const JsonValue &target) {
    if (!target.isString()) {
      reportAt(out.path, out.text, target.begin, "an export names a .tz file of the package");
      return false;
    }
    llvm::StringRef file = target.string;
    file.consume_front("./");
    exports.push_back({key.str(), file.str()});

    return true;
  };
  if (value.isString()) {
    return add(".", value);
  }
  if (!value.isObject()) {
    reportAt(out.path, out.text, value.begin,
             "'exports' is a file, or an object such as { \".\": \"./mod.tz\", \"./mini\": "
             "\"./mini.tz\" }");
    return false;
  }
  for (size_t i = 0; i < value.keys.size(); ++i) {
    llvm::StringRef key = value.keys[i];
    if (key != "." && !key.starts_with("./")) {
      reportAt(out.path, out.text, value.keyOffsets[i],
               "an export is named '.' or './name', as imports write it after the package's "
               "name");
      return false;
    }
    if (!add(key, value.items[i])) {
      return false;
    }
  }

  return true;
}

bool parseManifest(Manifest &out) {
  JsonValue root;
  size_t at;
  std::string error;
  if (!parseJson(out.text, root, at, error)) {
    reportAt(out.path, out.text, at, error);
    return false;
  }
  if (!root.isObject()) {
    reportAt(out.path, out.text, root.begin,
             "tz.json holds an object: { \"name\": ..., "
             "\"dependencies\": { ... } }");
    return false;
  }
  out.rootBegin = root.begin;
  out.rootEnd = root.end;
  if (!root.keys.empty()) {
    out.lastMemberEnd = root.items.back().end;
    // The indentation of the first member.
    size_t lineStart = out.text.rfind('\n', root.keyOffsets[0]);
    lineStart = lineStart == std::string::npos ? 0 : lineStart + 1;
    llvm::StringRef lead = llvm::StringRef(out.text).slice(lineStart, root.keyOffsets[0]);
    if (!lead.empty() && llvm::all_of(lead, [](char c) { return c == ' ' || c == '\t'; })) {
      out.indent = lead.str();
    }
  }
  for (size_t i = 0; i < root.keys.size(); ++i) {
    llvm::StringRef key = root.keys[i];
    const JsonValue &value = root.items[i];
    if (key == "name" || key == "version" || key == "main") {
      if (!value.isString()) {
        reportAt(out.path, out.text, value.begin, "'" + key + "' is a string");
        return false;
      }
      (key == "name" ? out.name : key == "version" ? out.version : out.main) = value.string;
      if (key == "main") {
        out.mainOffset = value.begin;
      }
    } else if (key == "exports") {
      if (!readExports(out, value, out.exports)) {
        return false;
      }
    } else if (key == "dependencies") {
      if (!value.isObject()) {
        reportAt(out.path, out.text, value.begin,
                 "'dependencies' is an object: { \"zod\": \"jsr:@tz/zod@^4\" }");
        return false;
      }
      out.depsBegin = value.begin;
      out.depsEnd = value.end;
      for (size_t j = 0; j < value.keys.size(); ++j) {
        if (!checkAlias(value.keys[j], error)) {
          reportAt(out.path, out.text, value.keyOffsets[j], error);
          return false;
        }
        const JsonValue &spec = value.items[j];
        if (!spec.isString()) {
          reportAt(out.path, out.text, spec.begin,
                   "a dependency is a string that says where the package comes from, e.g. "
                   "\"jsr:@std/path@^1\"");
          return false;
        }
        out.dependencies.push_back({value.keys[j], spec.string, spec.begin});
      }
    } else {
      reportAt(out.path, out.text, root.keyOffsets[i],
               "unknown field '" + key +
                   "' in tz.json; the fields are name, version, main, exports and dependencies");
      return false;
    }
  }

  return true;
}

} // namespace

bool readManifest(llvm::StringRef file, Manifest &out) {
  std::string text;

  return readText(file, text) && readManifestText(file, std::move(text), out);
}

bool readManifestText(llvm::StringRef file, std::string text, Manifest &out) {
  out = Manifest();
  out.path = file.str();
  out.dir = canonical(path::parent_path(file));
  out.isTzJson = true;
  out.text = std::move(text);

  return parseManifest(out);
}

bool readPackageDir(llvm::StringRef dir, Manifest &out) {
  llvm::SmallString<256> file(dir);
  path::append(file, "tz.json");
  if (fs::is_regular_file(file)) {
    return readManifest(file, out);
  }
  out = Manifest();
  out.dir = canonical(dir);
  // A package made for Deno or JSR: its name and exports, no more.
  for (const char *name : {"jsr.json", "deno.json", "deno.jsonc"}) {
    file = dir;
    path::append(file, name);
    if (!fs::is_regular_file(file)) {
      continue;
    }
    out.path = std::string(file);
    if (!readText(file, out.text)) {
      return false;
    }
    JsonValue root;
    size_t at;
    std::string error;
    if (!parseJson(out.text, root, at, error)) {
      reportAt(out.path, out.text, at, error);
      return false;
    }
    out.name = root.getString("name");
    out.version = root.getString("version");
    if (const JsonValue *exports = root.get("exports")) {
      if (!readExports(out, *exports, out.exports)) {
        return false;
      }
    }
    break;
  }

  return true;
}

std::string withDependencies(const Manifest &m, std::vector<Dependency> deps) {
  std::sort(deps.begin(), deps.end(),
            [](const Dependency &a, const Dependency &b) { return a.alias < b.alias; });
  const std::string &indent = m.indent;
  std::string object = "{}";
  if (!deps.empty()) {
    object = "{\n";
    for (size_t i = 0; i < deps.size(); ++i) {
      object += indent + indent + jsonQuote(deps[i].alias) + ": " + jsonQuote(deps[i].spec) +
                (i + 1 < deps.size() ? ",\n" : "\n");
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

} // namespace tz::pkg
