#include "compiler/packages/lock/lock.h"
#include "compiler/packages/json_parser/json_parser.h"
#include "compiler/packages/json_text/json_text.h"
#include "compiler/packages/json_value/json_value.h"
#include "compiler/packages/locked/locked.h"
#include "compiler/packages/report/report.h"
#include "compiler/packages/text_file/text_file.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

namespace tz::pkg {

namespace fs = llvm::sys::fs;

namespace {

constexpr int kLockVersion = 1;

}

bool readLock(llvm::StringRef file, Lock &out) {
  out = Lock();
  if (!fs::exists(file)) {
    return true;
  }
  std::string text;
  if (!readText(file, text)) {
    return false;
  }
  JsonValue root;
  size_t at;
  std::string error;
  if (!parseJson(text, root, at, error)) {
    reportAt(file, text, at, error + "; delete tz.lock to make it anew");
    return false;
  }
  const JsonValue *version = root.get("lockVersion");
  if (!version || version->kind != JsonValue::Kind::Number || version->number != kLockVersion) {
    reportAt(file, text, version ? version->begin : root.begin,
             "tz.lock is not in the format of this tz (\"lockVersion\": 1)");
    return false;
  }
  const JsonValue *packages = root.get("packages");
  if (!packages) {
    return true;
  }
  for (size_t i = 0; packages->isObject() && i < packages->keys.size(); ++i) {
    const JsonValue &entry = packages->items[i];
    Locked locked;
    locked.id = packages->keys[i];
    locked.version = entry.getString("version");
    locked.ref = entry.getString("ref");
    locked.commit = entry.getString("commit");
    locked.hash = entry.getString("hash");
    if (const JsonValue *deps = entry.get("dependencies")) {
      for (const JsonValue &dep : deps->items) {
        if (dep.isString()) {
          locked.dependencies.push_back(dep.string);
        }
      }
    }
    bool jsr = llvm::StringRef(locked.id).starts_with("jsr:");
    bool git = llvm::StringRef(locked.id).starts_with("git:");
    if ((!jsr && !git) || (jsr && locked.version.empty()) || (git && locked.commit.empty()) ||
        locked.hash.empty()) {
      reportAt(file, text, packages->keyOffsets[i],
               "tz.lock describes '" + locked.id + "' wrongly; delete tz.lock to make it anew");
      return false;
    }
    out.packages[locked.id] = std::move(locked);
  }

  return true;
}

std::string lockText(const Lock &lock) {
  std::string text = "{\n  \"lockVersion\": " + std::to_string(kLockVersion) + ",\n";
  text += "  \"packages\": {";
  size_t n = 0;
  for (const auto &[id, p] : lock.packages) {
    text += n++ ? ",\n" : "\n";
    text += "    " + jsonQuote(id) + ": {\n";
    std::vector<std::string> fields;
    if (!p.version.empty()) {
      fields.push_back("\"version\": " + jsonQuote(p.version));
    }
    if (!p.ref.empty()) {
      fields.push_back("\"ref\": " + jsonQuote(p.ref));
    }
    if (!p.commit.empty()) {
      fields.push_back("\"commit\": " + jsonQuote(p.commit));
    }
    fields.push_back("\"hash\": " + jsonQuote(p.hash));
    if (!p.dependencies.empty()) {
      std::string deps = "\"dependencies\": [";
      for (size_t i = 0; i < p.dependencies.size(); ++i) {
        deps += (i ? ", " : "") + jsonQuote(p.dependencies[i]);
      }
      fields.push_back(deps + "]");
    }
    for (size_t i = 0; i < fields.size(); ++i) {
      text += "      " + fields[i] + (i + 1 < fields.size() ? ",\n" : "\n");
    }
    text += "    }";
  }
  text += n ? "\n  }\n}\n" : "}\n}\n";

  return text;
}

}
