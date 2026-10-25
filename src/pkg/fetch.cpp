#include "pkg/fetch.h"

#include "pkg/json.h"
#include "pkg/net.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/SHA256.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>

namespace tz::pkg {

namespace fs = llvm::sys::fs;
namespace path = llvm::sys::path;

std::string cacheDir() {
  if (std::optional<std::string> dir = llvm::sys::Process::GetEnv("TZ_CACHE"); dir && !dir->empty())
    return *dir;
  llvm::SmallString<256> dir;
  if (!path::cache_directory(dir))
    path::system_temp_directory(/*ErasedOnReboot=*/false, dir);
#ifdef _WIN32
  path::append(dir, "tz", "cache");
#else
  path::append(dir, "tz");
#endif
  return std::string(dir);
}

std::string sha256Hex(llvm::StringRef data) {
  llvm::SHA256 hasher;
  hasher.update(data);
  std::array<uint8_t, 32> digest = hasher.final();
  return llvm::toHex(digest, /*LowerCase=*/true);
}

std::string treeHash(std::vector<std::pair<std::string, std::string>> files) {
  std::sort(files.begin(), files.end());
  std::string list;
  for (const auto &[file, hex] : files)
    list += file + " sha256-" + hex + "\n";
  return "sha256-" + sha256Hex(list);
}

bool hashDirectory(llvm::StringRef dir, std::string &hash, std::string &error) {
  std::vector<std::pair<std::string, std::string>> files;
  std::error_code ec;
  for (fs::recursive_directory_iterator it(dir, ec), end; it != end && !ec; it.increment(ec)) {
    if (!fs::is_regular_file(it->path()))
      continue;
    auto buffer = llvm::MemoryBuffer::getFile(it->path(), /*IsText=*/false,
                                              /*RequiresNullTerminator=*/false);
    if (!buffer) {
      error = "cannot read '" + it->path() + "': " + buffer.getError().message();
      return false;
    }
    llvm::StringRef relative = llvm::StringRef(it->path()).drop_front(dir.size());
    relative = relative.ltrim("/\\");
    files.push_back({path::convert_to_slash(relative), sha256Hex((*buffer)->getBuffer())});
  }
  if (ec) {
    error = "cannot read '" + dir.str() + "': " + ec.message();
    return false;
  }
  hash = treeHash(std::move(files));
  return true;
}

void progress(const llvm::Twine &message) { llvm::errs() << "tz: " << message << "\n"; }

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

} // namespace

bool writeFileAtomically(llvm::StringRef file, llvm::StringRef data, std::string &error) {
  if (std::error_code ec = fs::create_directories(path::parent_path(file))) {
    error = "cannot create '" + path::parent_path(file).str() + "': " + ec.message();
    return false;
  }
  int fd;
  llvm::SmallString<256> temp;
  std::error_code ec = fs::createUniqueFile(file + ".tmp-%%%%%%", fd, temp);
  if (!ec) {
    llvm::raw_fd_ostream out(fd, /*shouldClose=*/true);
    out << data;
    out.close();
    if (out.has_error()) {
      ec = out.error();
      out.clear_error();
    }
  }
  if (!ec)
    ec = fs::rename(temp, file);
  if (ec) {
    if (!temp.empty())
      fs::remove(temp);
    error = "cannot write '" + file.str() + "': " + ec.message();
    return false;
  }
  return true;
}

namespace {

// A directory filled under a temporary name next to where it belongs, then
// renamed: another tz that looks into the cache meanwhile sees it whole or
// not at all. If another tz finished the same directory first, its copy is
// kept.
class StagedDir {
public:
  bool create(llvm::StringRef final, std::string &error) {
    final_ = final.str();
    if (std::error_code ec = fs::create_directories(path::parent_path(final))) {
      error = "cannot create '" + path::parent_path(final).str() + "': " + ec.message();
      return false;
    }
    if (std::error_code ec = fs::createUniqueDirectory(final + ".tmp", temp_)) {
      error = "cannot create a directory next to '" + final_ + "': " + ec.message();
      return false;
    }
    return true;
  }
  llvm::StringRef path() const { return temp_; }
  bool commit(std::string &error) {
    std::error_code ec = fs::rename(temp_, final_);
    if (!ec) {
      temp_.clear();
      return true;
    }
    if (fs::is_directory(final_))
      return true; // another tz was faster; the destructor drops this copy
    error = "cannot move '" + std::string(temp_) + "' to '" + final_ + "': " + ec.message();
    return false;
  }
  ~StagedDir() {
    if (!temp_.empty())
      fs::remove_directories(temp_, /*IgnoreErrors=*/true);
  }

private:
  std::string final_;
  llvm::SmallString<256> temp_;
};

// A path from a registry's list of files must stay inside the package.
bool safeRelativePath(llvm::StringRef file) {
  if (file.empty() || file.contains('\\') || file.contains(':') || file.starts_with("/"))
    return false;
  llvm::SmallVector<llvm::StringRef, 8> parts;
  file.split(parts, '/');
  for (llvm::StringRef part : parts)
    if (part.empty() || part == "." || part == "..")
      return false;
  return true;
}

std::string cachePath(std::initializer_list<llvm::StringRef> parts) {
  llvm::SmallString<256> result(cacheDir());
  for (llvm::StringRef part : parts)
    path::append(result, part);
  return std::string(result);
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
    if (exports->isString())
      out.exports.push_back({".", packageFile(exports->string)});
    for (size_t i = 0; i < exports->keys.size(); ++i)
      if (exports->items[i].isString())
        out.exports.push_back({exports->keys[i], packageFile(exports->items[i].string)});
  }
  out.hash = treeHash(out.files);
  return true;
}

} // namespace

std::string jsrBaseUrl() {
  std::string base = "https://jsr.io";
  std::optional<std::string> url = llvm::sys::Process::GetEnv("TZ_JSR_URL");
  if (url && !url->empty())
    base = *url;
  while (!base.empty() && base.back() == '/')
    base.pop_back();
  return base;
}

bool jsrVersions(llvm::StringRef scope, llvm::StringRef name, JsrVersions &out,
                 std::string &error) {
  std::string url = jsrPackageUrl(scope, name) + "/meta.json", text;
  JsonValue meta;
  if (!fetchJson(url, text, meta, error))
    return false;
  const JsonValue *versions = meta.get("versions");
  if (!versions || !versions->isObject()) {
    error = url + " has no list of versions";
    return false;
  }
  out.versions.clear();
  for (size_t i = 0; i < versions->keys.size(); ++i) {
    JsrVersions::Entry entry;
    if (!Version::parse(versions->keys[i], entry.version))
      continue; // not semver: no range can name it
    const JsonValue *yanked = versions->items[i].get("yanked");
    entry.yanked = yanked && yanked->kind == JsonValue::Kind::Bool && yanked->boolean;
    out.versions.push_back(std::move(entry));
  }
  return true;
}

bool jsrVersion(llvm::StringRef scope, llvm::StringRef name, llvm::StringRef version,
                JsrVersion &out, std::string &error) {
  std::string cached = cachePath({"jsr", "@" + scope.str(), name, version.str() + "_meta.json"});
  if (auto buffer = llvm::MemoryBuffer::getFile(cached))
    return readJsrVersion((*buffer)->getBuffer(), cached, out, error);

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
  if (!readJsrVersion(text, url, out, error))
    return false;
  // Kept only once it reads well: the version's list of files never changes.
  return writeFileAtomically(cached, text, error);
}

bool jsrDownload(llvm::StringRef scope, llvm::StringRef name, llvm::StringRef version,
                 const JsrVersion &meta, std::string &dir, std::string &error) {
  dir = cachePath({"jsr", "@" + scope.str(), name, version});
  if (fs::is_directory(dir))
    return true;
  progress("downloading jsr:@" + scope + "/" + name + "@" + version);
  StagedDir staged;
  if (!staged.create(dir, error))
    return false;
  std::string base = jsrPackageUrl(scope, name) + "/" + version.str() + "/";
  for (const auto &[file, checksum] : meta.files) {
    std::string url = base + file, body;
    if (fetchUrl(url, body, error) != FetchResult::Ok) {
      error = "cannot download " + url +
              (error.empty() ? std::string(": not found") : ": " + error);
      return false;
    }
    if (sha256Hex(body) != checksum) {
      error = url + " is not the file the registry lists: its SHA-256 differs";
      return false;
    }
    llvm::SmallString<256> target(staged.path());
    path::append(target, file);
    if (!writeFile(target, body, error))
      return false;
  }
  return staged.commit(error);
}

int runProgram(llvm::StringRef program, llvm::ArrayRef<std::string> args, std::string &out,
               std::string &err, std::string &error) {
  llvm::SmallString<128> outFile, errFile;
  if (fs::createTemporaryFile("tz-out", "txt", outFile) ||
      fs::createTemporaryFile("tz-err", "txt", errFile)) {
    error = "cannot create a temporary file";
    return -1;
  }
  llvm::FileRemover removeOut(outFile), removeErr(errFile);
  std::vector<llvm::StringRef> argv{program};
  for (const std::string &arg : args)
    argv.push_back(arg);
  // No input: git must not wait for a password.
  std::optional<llvm::StringRef> redirects[] = {llvm::StringRef(""), llvm::StringRef(outFile),
                                                llvm::StringRef(errFile)};
  int code = llvm::sys::ExecuteAndWait(program, argv, std::nullopt, redirects, 0, 0, &error);
  if (auto buffer = llvm::MemoryBuffer::getFile(outFile))
    out = (*buffer)->getBuffer().str();
  if (auto buffer = llvm::MemoryBuffer::getFile(errFile))
    err = (*buffer)->getBuffer().str();
  return code < 0 ? -1 : code;
}

namespace {

// Runs git; false with git's own message in `error` if it fails.
bool git(llvm::ArrayRef<std::string> args, std::string *output, std::string &error) {
  static llvm::ErrorOr<std::string> program = llvm::sys::findProgramByName("git");
  if (!program) {
    error = "git packages need the git program, and it is not installed (or not on PATH)";
    return false;
  }
  // The user's settings must not change the files: no line-ending
  // conversion, no prompts for passwords.
  std::vector<std::string> all{"-c", "core.autocrlf=false", "-c", "core.eol=lf",
                               "-c", "core.longpaths=true", "-c", "credential.interactive=never",
                               "-c", "protocol.ext.allow=never"};
  all.insert(all.end(), args.begin(), args.end());
  std::string out, err;
  int code = runProgram(*program, all, out, err, error);
  if (code == 0) {
    if (output)
      *output = llvm::StringRef(out).trim().str();
    return true;
  }
  if (code > 0)
    error = llvm::StringRef(err).trim().str();
  if (error.empty())
    error = "git failed";
  return false;
}

// The cache's name for a repository: its name, and a hash of the address
// that makes it unique. Short, because git for Windows refuses paths longer
// than 260 characters, and a checkout puts .git/logs/refs/... under it.
std::string repositoryKey(llvm::StringRef url) {
  llvm::StringRef name = url.rtrim("/\\");
  size_t slash = name.find_last_of("/:\\");
  if (slash != llvm::StringRef::npos)
    name = name.drop_front(slash + 1);
  name.consume_back(".git");
  std::string key;
  for (char c : name.take_front(24)) {
    char lower = llvm::toLower(c);
    key += llvm::isAlnum(lower) || lower == '.' || lower == '-' ? lower : '_';
  }
  return key + "-" + sha256Hex(url).substr(0, 8);
}

} // namespace

bool gitResolve(const std::string &url, const std::string &ref, bool refresh,
                std::string &commit, std::string &error) {
  std::string key = repositoryKey(url);
  std::string mirror = cachePath({"git", key + ".git"});
  // A repository is fetched once per run of tz at most: `tz add` reads the
  // package, then resolves the versions of all.
  static llvm::StringSet<> current;
  bool fresh = current.contains(url);
  if (!fs::is_directory(mirror)) {
    progress("cloning " + url);
    StagedDir staged;
    if (!staged.create(mirror, error))
      return false;
    if (!git({"clone", "--mirror", "--quiet", "--", url, std::string(staged.path())}, nullptr,
             error)) {
      error = "cannot clone " + url + ": " + error;
      return false;
    }
    if (!staged.commit(error))
      return false;
    fresh = true;
    current.insert(url);
  }
  std::string name = (ref.empty() ? std::string("HEAD") : ref) + "^{commit}";
  auto lookup = [&] {
    std::string ignored;
    return git({"--git-dir", mirror, "rev-parse", "--verify", "--quiet", name}, &commit, ignored);
  };
  bool known = !refresh && lookup();
  if (!known && !fresh) {
    progress("fetching " + url);
    if (!git({"--git-dir", mirror, "fetch", "--quiet", "--prune", "--tags", "--force", "origin"},
             nullptr, error)) {
      error = "cannot fetch " + url + ": " + error;
      return false;
    }
    current.insert(url);
  }
  if (known || lookup())
    return true;
  error = ref.empty() ? url + " has no commits"
                      : url + " has no tag, branch or commit '" + ref + "'";
  return false;
}

bool gitCheckout(const std::string &url, const std::string &commit, std::string &dir,
                 std::string &error) {
  std::string key = repositoryKey(url);
  // 16 digits of the commit tell it apart from the others of the repository.
  dir = cachePath({"git", key, llvm::StringRef(commit).take_front(16)});
  if (fs::is_directory(dir))
    return true;
  std::string mirror = cachePath({"git", key + ".git"});
  std::string ignored;
  if (!fs::is_directory(mirror) ||
      !git({"--git-dir", mirror, "cat-file", "-e", commit + "^{commit}"}, nullptr, ignored)) {
    std::string found;
    if (!gitResolve(url, commit, /*refresh=*/false, found, error))
      return false;
  }
  StagedDir staged;
  if (!staged.create(dir, error))
    return false;
  std::string work(staged.path());
  // A clone that borrows the mirror's objects, checked out at the commit;
  // then only the files are kept.
  llvm::SmallString<256> dotGit(work);
  path::append(dotGit, ".git");
  if (!git({"clone", "--quiet", "--shared", "--no-checkout", "--", mirror, work}, nullptr,
           error) ||
      !git({"--git-dir", std::string(dotGit), "--work-tree", work, "-c",
            "advice.detachedHead=false", "checkout", "--quiet", commit},
           nullptr, error)) {
    error = "cannot check out " + commit + " of " + url + ": " + error;
    return false;
  }
  fs::remove_directories(dotGit, /*IgnoreErrors=*/true);
  if (fs::exists(dotGit)) {
    error = "cannot remove '" + std::string(dotGit) + "'";
    return false;
  }
  return staged.commit(error);
}

} // namespace tz::pkg
