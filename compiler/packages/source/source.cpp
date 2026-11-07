#include "compiler/packages/source/source.h"
#include "compiler/packages/paths/paths.h"
#include "compiler/packages/range/range.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

namespace tz::pkg {

namespace fs = llvm::sys::fs;
namespace path = llvm::sys::path;

namespace {

bool isJsrNamePart(llvm::StringRef part) {
  return !part.empty() && llvm::all_of(part, [](char c) {
    return llvm::isLower(c) || llvm::isDigit(c) || c == '-';
  });
}

}

std::string repositoryName(llvm::StringRef url) {
  llvm::StringRef name = url.rtrim("/");
  size_t slash = name.find_last_of("/:\\");
  if (slash != llvm::StringRef::npos) {
    name = name.drop_front(slash + 1);
  }
  name.consume_back(".git");

  return name.str();
}

namespace {

const std::string kNoText;

}

const std::string &Source::scope() const {
  const JsrSource *jsr = this->form.as<JsrSource>();

  return jsr ? jsr->scope : kNoText;
}

const std::string &Source::name() const {
  const JsrSource *jsr = this->form.as<JsrSource>();

  return jsr ? jsr->name : kNoText;
}

const std::string &Source::range() const {
  const JsrSource *jsr = this->form.as<JsrSource>();

  return jsr ? jsr->range : kNoText;
}

const std::string &Source::url() const {
  const GitSource *git = this->form.as<GitSource>();

  return git ? git->url : kNoText;
}

const std::string &Source::ref() const {
  const GitSource *git = this->form.as<GitSource>();

  return git ? git->ref : kNoText;
}

const std::string &Source::dir() const {
  const LocalSource *local = this->form.as<LocalSource>();

  return local ? local->dir : kNoText;
}

std::string Source::id() const {
  std::string text;
  switch (this->form.kind()) {
    case SourceKind::JSR_SOURCE:
      text = "jsr:@" + this->scope() + "/" + this->name();
      break;
    case SourceKind::GIT_SOURCE:
      text = "git:" + this->url();
      break;
    case SourceKind::LOCAL_SOURCE:
      text = "file:" + path::convert_to_slash(this->dir());
      break;
  }

  return text;
}

std::string Source::shortName() const {
  std::string text;
  switch (this->form.kind()) {
    case SourceKind::JSR_SOURCE:
      text = "@" + this->scope() + "/" + this->name();
      break;
    case SourceKind::GIT_SOURCE:
      text = repositoryName(this->url());
      break;
    case SourceKind::LOCAL_SOURCE:
      text = path::filename(this->dir()).str();
      break;
  }

  return text;
}

bool parseSource(llvm::StringRef spec, llvm::StringRef baseDir, Source &out, std::string &error) {
  out = Source();
  llvm::StringRef rest = spec;
  if (rest.consume_front("jsr:")) {
    rest.consume_front("/");
    llvm::StringRef scope, name;
    if (rest.consume_front("@")) {
      std::tie(scope, rest) = rest.split('/');
      size_t at = rest.find('@');
      name = rest.substr(0, at);
      rest = at == llvm::StringRef::npos ? llvm::StringRef() : rest.drop_front(at + 1);
    }
    if (!isJsrNamePart(scope) || !isJsrNamePart(name)) {
      error = "'" + spec.str() +
              "' does not name a JSR package: write 'jsr:@scope/name' and a "
              "range of versions, e.g. 'jsr:@std/path@^1'";
      return false;
    }
    JsrSource jsr{.scope = scope.str(), .name = name.str(), .range = rest.str()};
    Range range;
    if (!Range::parse(jsr.range, range, error)) {
      error = "in '" + spec.str() + "': " + error;
      return false;
    }
    out.form = std::move(jsr);
    return true;
  }
  if (rest.consume_front("github:") || rest.consume_front("git:")) {
    GitSource git;
    bool github = spec.starts_with("github:");
    size_t slash = rest.find_last_of('/');
    size_t at = rest.find_last_of('@');
    llvm::StringRef where = rest;
    if (at != llvm::StringRef::npos && (slash == llvm::StringRef::npos || at > slash)) {
      where = rest.substr(0, at);
      git.ref = rest.drop_front(at + 1).str();
    }
    if (where.starts_with("-") || llvm::StringRef(git.ref).starts_with("-")) {
      error = "'" + spec.str() + "': an address or a tag of git does not start with '-'";
      return false;
    }
    if (github) {
      auto [user, repo] = where.split('/');
      if (user.empty() || repo.empty() || repo.contains('/')) {
        error = "'" + spec.str() +
                "' does not name a GitHub repository: write "
                "'github:user/repo@v1.2.0'";
        return false;
      }
      repo.consume_back(".git");
      git.url = "https://github.com/" + user.str() + "/" + repo.str() + ".git";
    } else if (where.starts_with("./") || where.starts_with("../")) {
      llvm::SmallString<256> full(baseDir);
      path::append(full, where);
      path::remove_dots(full, true);
      git.url = path::convert_to_slash(full);
    } else {
      git.url = where.str();
    }
    if (git.url.empty()) {
      error = "'" + spec.str() +
              "' names no repository: write 'git:' and what 'git clone' takes, "
              "e.g. 'git:https://example.com/lib.git@v1.0.0'";
      return false;
    }
    out.form = std::move(git);
    return true;
  }
  bool local = rest.consume_front("file:");
  if (local || rest.starts_with("./") || rest.starts_with("../") || rest == "." || rest == "..") {
    llvm::SmallString<256> full(rest);
    if (!path::is_absolute(full)) {
      full = baseDir;
      path::append(full, rest);
    }
    if (!fs::is_directory(full)) {
      error = "there is no directory '" + rest.str() + "'" +
              (baseDir.empty() ? std::string() : " in " + baseDir.str());
      return false;
    }
    out.form = LocalSource{.dir = canonical(full)};
    return true;
  }
  if (rest.starts_with("npm:")) {
    error = "npm packages are not supported: npm has compiled JavaScript, not TypeScript sources "
            "(docs/packages.md); look for the package on JSR ('jsr:@scope/name@^1') or take its "
            "sources from git ('github:user/repo@v1.0.0')";
    return false;
  }
  Range range;
  std::string ignored;
  if (!rest.empty() && (rest == "latest" || Range::parse(rest, range, ignored))) {
    error = "'" + spec.str() +
            "' is a version, but not where the package comes from; tz takes "
            "packages from JSR and git: write e.g. 'jsr:@scope/name@" +
            (rest == "latest" ? std::string("^1") : rest.str()) + "'";
    return false;
  }
  error = "'" + spec.str() +
          "' does not say where the package comes from; write "
          "'jsr:@scope/name@^1', 'github:user/repo@v1.0.0', 'git:<address>@v1.0.0' or "
          "'file:<directory>'";

  return false;
}

}
