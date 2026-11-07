#include "compiler/packages/tz_config/tz_config.h"
#include "compiler/packages/json_parser/json_parser.h"
#include "compiler/packages/json_value/json_value.h"
#include "compiler/packages/paths/paths.h"
#include "compiler/packages/report/report.h"
#include "compiler/packages/text_file/text_file.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/Path.h"

namespace tz::pkg {

namespace path = llvm::sys::path;

bool TzConfigFile::readOptions(TzConfig &out, llvm::StringRef text, const JsonValue &options) {
  if (!options.isObject()) {
    Report::reportAt(out.path, text, options.begin,
                     "'compilerOptions' is an object: { \"outDir\": \"dist\" }");
    return false;
  }
  for (size_t i = 0; i < options.keys().size(); ++i) {
    llvm::StringRef key = options.keys()[i];
    const JsonValue &value = options.items()[i];
    if (key != "outDir") {
      Report::reportAt(out.path, text, options.keyOffsets()[i],
                       "unknown compiler option '" + key +
                           "' in tzconfig.json; the options are outDir");
      return false;
    }
    if (!value.isString() || value.string().empty()) {
      Report::reportAt(out.path, text, value.begin,
                       "'outDir' is the directory tz build writes to, e.g. \"dist\"");
      return false;
    }
    if (path::is_absolute(value.string())) {
      out.outDir = value.string();
      continue;
    }
    llvm::SmallString<256> dir(out.dir);
    path::append(dir, value.string());
    out.outDir = std::string(dir);
  }

  return true;
}

std::string TzConfigFile::findTzConfig(llvm::StringRef dir) {
  return Paths::findAbove(dir, "tzconfig.json");
}

bool TzConfigFile::readTzConfig(llvm::StringRef file, TzConfig &out) {
  out = TzConfig();
  out.path = file.str();
  out.dir = Paths::canonical(path::parent_path(file));
  std::string text;
  if (!TextFile::readText(file, text)) {
    return false;
  }
  JsonValue root;
  size_t at;
  std::string error;
  if (!JsonParser::parseJson(text, root, at, error)) {
    Report::reportAt(out.path, text, at, error);
    return false;
  }
  if (!root.isObject()) {
    Report::reportAt(
        out.path, text, root.begin,
        "tzconfig.json holds an object: { \"compilerOptions\": { \"outDir\": \"dist\" } }");
    return false;
  }
  for (size_t i = 0; i < root.keys().size(); ++i) {
    llvm::StringRef key = root.keys()[i];
    if (key != "compilerOptions") {
      Report::reportAt(out.path, text, root.keyOffsets()[i],
                       "unknown field '" + key +
                           "' in tzconfig.json; the fields are compilerOptions");
      return false;
    }
    if (!TzConfigFile::readOptions(out, text, root.items()[i])) {
      return false;
    }
  }

  return true;
}

}
