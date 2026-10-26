#include "compiler/packages/git_client/git_client.h"
#include "compiler/packages/package_cache/package_cache.h"
#include "compiler/packages/program_runner/program_runner.h"
#include "compiler/packages/report/report.h"
#include "compiler/packages/staged_dir/staged_dir.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"

namespace tz::pkg {

namespace fs = llvm::sys::fs;
namespace path = llvm::sys::path;

namespace {

bool git(llvm::ArrayRef<std::string> args, std::string *output, std::string &error) {
  static llvm::ErrorOr<std::string> program = llvm::sys::findProgramByName("git");
  if (!program) {
    error = "git packages need the git program, and it is not installed (or not on PATH)";
    return false;
  }
  std::vector<std::string> all{
      "-c", "core.autocrlf=false",     "-c", "core.eol=lf",
      "-c", "core.longpaths=true",     "-c", "credential.interactive=never",
      "-c", "protocol.ext.allow=never"};
  all.insert(all.end(), args.begin(), args.end());
  std::string out, err;
  int code = runProgram(*program, all, out, err, error);
  if (code == 0) {
    if (output) {
      *output = llvm::StringRef(out).trim().str();
    }
    return true;
  }
  if (code > 0) {
    error = llvm::StringRef(err).trim().str();
  }
  if (error.empty()) {
    error = "git failed";
  }

  return false;
}

std::string repositoryKey(llvm::StringRef url) {
  llvm::StringRef name = url.rtrim("/\\");
  size_t slash = name.find_last_of("/:\\");
  if (slash != llvm::StringRef::npos) {
    name = name.drop_front(slash + 1);
  }
  name.consume_back(".git");
  std::string key;
  for (char c : name.take_front(24)) {
    char lower = llvm::toLower(c);
    key += llvm::isAlnum(lower) || lower == '.' || lower == '-' ? lower : '_';
  }

  return key + "-" + sha256Hex(url).substr(0, 8);
}

}

bool gitResolve(const std::string &url, const std::string &ref, bool refresh, std::string &commit,
                std::string &error) {
  std::string key = repositoryKey(url);
  std::string mirror = cachePath({"git", key + ".git"});
  static llvm::StringSet<> current;
  bool fresh = current.contains(url);
  if (!fs::is_directory(mirror)) {
    progress("cloning " + url);
    StagedDir staged;
    if (!staged.create(mirror, error)) {
      return false;
    }
    if (!git({"clone", "--mirror", "--quiet", "--", url, std::string(staged.path())}, nullptr,
             error)) {
      error = "cannot clone " + url + ": " + error;
      return false;
    }
    if (!staged.commit(error)) {
      return false;
    }
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
  if (known || lookup()) {
    return true;
  }
  error =
      ref.empty() ? url + " has no commits" : url + " has no tag, branch or commit '" + ref + "'";

  return false;
}

bool gitCheckout(const std::string &url, const std::string &commit, std::string &dir,
                 std::string &error) {
  std::string key = repositoryKey(url);
  dir = cachePath({"git", key, llvm::StringRef(commit).take_front(16)});
  if (fs::is_directory(dir)) {
    return true;
  }
  std::string mirror = cachePath({"git", key + ".git"});
  std::string ignored;
  if (!fs::is_directory(mirror) ||
      !git({"--git-dir", mirror, "cat-file", "-e", commit + "^{commit}"}, nullptr, ignored)) {
    std::string found;
    if (!gitResolve(url, commit, false, found, error)) {
      return false;
    }
  }
  StagedDir staged;
  if (!staged.create(dir, error)) {
    return false;
  }
  std::string work(staged.path());
  llvm::SmallString<256> dotGit(work);
  path::append(dotGit, ".git");
  if (!git({"clone", "--quiet", "--shared", "--no-checkout", "--", mirror, work}, nullptr, error) ||
      !git({"--git-dir", std::string(dotGit), "--work-tree", work, "-c",
            "advice.detachedHead=false", "checkout", "--quiet", commit},
           nullptr, error)) {
    error = "cannot check out " + commit + " of " + url + ": " + error;
    return false;
  }
  fs::remove_directories(dotGit, true);
  if (fs::exists(dotGit)) {
    error = "cannot remove '" + std::string(dotGit) + "'";
    return false;
  }

  return staged.commit(error);
}

}
