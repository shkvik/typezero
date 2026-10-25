#include "compiler/packages/jsr_client/jsr_client.h"
#include "compiler/packages/http_client/http_client.h"
#include "compiler/packages/json_parser/json_parser.h"
#include "compiler/packages/json_value/json_value.h"
#include "compiler/packages/jsr_version/jsr_version.h"
#include "compiler/packages/jsr_versions/jsr_versions.h"
#include "compiler/packages/package_cache/package_cache.h"
#include "compiler/packages/report/report.h"
#include "compiler/packages/staged_dir/staged_dir.h"
#include "compiler/packages/version/version.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/raw_ostream.h"

namespace tz::pkg {

namespace fs = llvm::sys::fs;
namespace path = llvm::sys::path;

namespace {

bool writeFile(llvm::StringRef file, llvm::StringRef data, std::string &error) {
  if (std::error_code ec = fs::create_directories(path::parent_path(file))) {
    error = "cannot create '" + path::parent_path(file).str() + "': " + ec.message();
    return false;
  }
  std::error_code ec;
  llvm::raw_fd_ostream out(file, ec, fs::OF_None);
  if (!ec) {
    out << data;
    out.close();
    if (out.has_error()) {
      ec = out.error();
      out.clear_error();
    }
  }
  if (ec) {
    error = "cannot write '" + file.str() + "': " + ec.message();
    return false;
  }

  return true;
}

// A path from a registry's list of files must stay inside the package.
bool safeRelativePath(llvm::StringRef file) {
  if (file.empty() || file.contains('\\') || file.contains(':') || file.starts_with("/")) {
    return false;
  }
  llvm::SmallVector<llvm::StringRef, 8> parts;
  file.split(parts, '/');
  for (llvm::StringRef part : parts) {
    if (part.empty() || part == "." || part == "..") {
      return false;
    }
  }

  return true;
}

// Reads a JSON document of the registry; false with an empty error when the
// registry says there is none.
bool fetchJson(const std::string &url, std::string &text, JsonValue &out, std::string &error) {
  switch (fetchUrl(url, text, error)) {
  case FetchResult::NotFound:
    error.clear();
    return false;
  case FetchResult::Failed:
    error = "cannot download " + url + ": " + error;
    return false;
  case FetchResult::Ok:
    break;
  }
  size_t at;
  if (!parseJson(text, out, at, error)) {
    error = url + " is not valid JSON: " + error;
    return false;
  }

  return true;
}

std::string jsrPackageUrl(llvm::StringRef scope, llvm::StringRef name) {
  return jsrBaseUrl() + "/@" + scope.str() + "/" + name.str();
}

bool readJsrVersion(llvm::StringRef text, const std::string &where, JsrVersion &out,
                    std::string &error) {
  JsonValue meta;
  size_t at;
  if (!parseJson(text, meta, at, error)) {
    error = where + " is not valid JSON: " + error;
    return false;
  }
  const JsonValue *manifest = meta.get("manifest");
  if (!manifest || !manifest->isObject()) {
    error = where + " has no list of files ('manifest')";
    return false;
  }
  out = JsrVersion();
  for (size_t i = 0; i < manifest->keys.size(); ++i) {
    llvm::StringRef file = manifest->keys[i];
    file.consume_front("/");
    std::string listed = manifest->items[i].getString("checksum");
    llvm::StringRef checksum = listed;
    if (!safeRelativePath(file) || !checksum.consume_front("sha256-") || checksum.size() != 64) {
      error = where + " lists the file '" + manifest->keys[i] + "' wrongly";
      return false;
    }
    out.files.push_back({file.str(), checksum.lower()});
  }
  // "./mod.tz" is the file mod.tz of the package.
  auto packageFile = [](llvm::StringRef target) {
    target.consume_front("./");

    return target.str();
  };
  if (const JsonValue *exports = meta.get("exports")) {
    if (exports->isString()) {
      out.exports.push_back({".", packageFile(exports->string)});
    }
    for (size_t i = 0; i < exports->keys.size(); ++i) {
      if (exports->items[i].isString()) {
        out.exports.push_back({exports->keys[i], packageFile(exports->items[i].string)});
      }
    }
  }
  out.hash = treeHash(out.files);

  return true;
}

} // namespace

std::string jsrBaseUrl() {
  std::string base = "https://jsr.io";
  std::optional<std::string> url = llvm::sys::Process::GetEnv("TZ_JSR_URL");
  if (url && !url->empty()) {
    base = *url;
  }
  while (!base.empty() && base.back() == '/') {
    base.pop_back();
  }

  return base;
}

bool jsrVersions(llvm::StringRef scope, llvm::StringRef name, JsrVersions &out,
                 std::string &error) {
  std::string url = jsrPackageUrl(scope, name) + "/meta.json", text;
  JsonValue meta;
  if (!fetchJson(url, text, meta, error)) {
    return false;
  }
  const JsonValue *versions = meta.get("versions");
  if (!versions || !versions->isObject()) {
    error = url + " has no list of versions";
    return false;
  }
  out.versions.clear();
  for (size_t i = 0; i < versions->keys.size(); ++i) {
    JsrVersions::Entry entry;
    if (!Version::parse(versions->keys[i], entry.version)) {
      continue; // not semver: no range can name it
    }
    const JsonValue *yanked = versions->items[i].get("yanked");
    entry.yanked = yanked && yanked->kind == JsonValue::Kind::Bool && yanked->boolean;
    out.versions.push_back(std::move(entry));
  }

  return true;
}

bool jsrVersion(llvm::StringRef scope, llvm::StringRef name, llvm::StringRef version,
                JsrVersion &out, std::string &error) {
  std::string cached = cachePath({"jsr", "@" + scope.str(), name, version.str() + "_meta.json"});
  if (auto buffer = llvm::MemoryBuffer::getFile(cached)) {
    return readJsrVersion((*buffer)->getBuffer(), cached, out, error);
  }

  std::string url = jsrPackageUrl(scope, name) + "/" + version.str() + "_meta.json", text;
  switch (fetchUrl(url, text, error)) {
  case FetchResult::NotFound:
    error = "the registry has no version " + version.str() + " of jsr:@" + scope.str() + "/" +
            name.str() + " (" + url + ")";
    return false;
  case FetchResult::Failed:
    error = "cannot download " + url + ": " + error;
    return false;
  case FetchResult::Ok:
    break;
  }
  if (!readJsrVersion(text, url, out, error)) {
    return false;
  }

  // Kept only once it reads well: the version's list of files never changes.
  return writeFileAtomically(cached, text, error);
}

bool jsrDownload(llvm::StringRef scope, llvm::StringRef name, llvm::StringRef version,
                 const JsrVersion &meta, std::string &dir, std::string &error) {
  dir = cachePath({"jsr", "@" + scope.str(), name, version});
  if (fs::is_directory(dir)) {
    return true;
  }
  progress("downloading jsr:@" + scope + "/" + name + "@" + version);
  StagedDir staged;
  if (!staged.create(dir, error)) {
    return false;
  }
  std::string base = jsrPackageUrl(scope, name) + "/" + version.str() + "/";
  for (const auto &[file, checksum] : meta.files) {
    std::string url = base + file, body;
    if (fetchUrl(url, body, error) != FetchResult::Ok) {
      error =
          "cannot download " + url + (error.empty() ? std::string(": not found") : ": " + error);
      return false;
    }
    if (sha256Hex(body) != checksum) {
      error = url + " is not the file the registry lists: its SHA-256 differs";
      return false;
    }
    llvm::SmallString<256> target(staged.path());
    path::append(target, file);
    if (!writeFile(target, body, error)) {
      return false;
    }
  }

  return staged.commit(error);
}

} // namespace tz::pkg
