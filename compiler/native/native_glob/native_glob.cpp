#include "compiler/native/native_glob/native_glob.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/GlobPattern.h"
#include "llvm/Support/Path.h"
#include <algorithm>
#include <functional>

namespace tz {

std::vector<std::string> NativeGlob::findFiles(llvm::StringRef pattern) const {
  auto slashed = llvm::sys::path::convert_to_slash(pattern);
  auto parts = llvm::SmallVector<llvm::StringRef, 16>();
  llvm::StringRef(slashed).split(parts, '/');
  auto isPlain = std::bind_front(&NativeGlob::isPlainSegment, this);
  auto *firstPattern = llvm::find_if_not(parts, isPlain);
  auto literal = llvm::ArrayRef<llvm::StringRef>(parts.begin(), firstPattern);
  auto base = llvm::join(literal, "/");
  if (firstPattern == parts.end()) {
    bool isFile = llvm::sys::fs::is_regular_file(base);
    return isFile ? std::vector<std::string>{base} : std::vector<std::string>();
  }
  auto segments = std::vector<std::string>(firstPattern, parts.end());
  auto files = std::vector<std::string>();
  auto steps = std::vector<GlobStep>{{.dir = base, .segment = 0}};
  while (!steps.empty()) {
    GlobStep step = steps.back();
    steps.pop_back();
    this->followSegment(step, segments, steps, files);
  }
  std::sort(files.begin(), files.end());
  files.erase(std::unique(files.begin(), files.end()), files.end());

  return files;
}

bool NativeGlob::isPattern(llvm::StringRef path) const {
  return path.find_first_of("*?[{") != llvm::StringRef::npos;
}

bool NativeGlob::isPlainSegment(llvm::StringRef segment) const {
  return !this->isPattern(segment);
}

void NativeGlob::followSegment(const GlobStep &step, llvm::ArrayRef<std::string> segments,
                               std::vector<GlobStep> &steps,
                               std::vector<std::string> &files) const {
  if (step.segment >= segments.size()) {
    return;
  }
  if (segments[step.segment] == "**") {
    steps.push_back({.dir = step.dir, .segment = step.segment + 1});
    return this->followAnyDepth(step, steps);
  }

  return this->followMatches(step, segments, steps, files);
}

void NativeGlob::followAnyDepth(const GlobStep &step, std::vector<GlobStep> &steps) const {
  std::error_code ec;
  for (llvm::sys::fs::directory_iterator it(step.dir, ec), end; it != end && !ec;
       it.increment(ec)) {
    if (it->type() == llvm::sys::fs::file_type::directory_file) {
      steps.push_back({.dir = this->joinPath(step.dir, llvm::sys::path::filename(it->path())),
                       .segment = step.segment});
    }
  }
}

void NativeGlob::followMatches(const GlobStep &step, llvm::ArrayRef<std::string> segments,
                               std::vector<GlobStep> &steps,
                               std::vector<std::string> &files) const {
  auto glob = llvm::GlobPattern::create(segments[step.segment]);
  if (!glob) {
    llvm::consumeError(glob.takeError());
    return;
  }
  bool isLast = step.segment + 1 == segments.size();
  std::error_code ec;
  for (llvm::sys::fs::directory_iterator it(step.dir, ec), end; it != end && !ec;
       it.increment(ec)) {
    llvm::StringRef name = llvm::sys::path::filename(it->path());
    auto type = it->type();
    bool isMatch = glob->match(name);
    if (isMatch && isLast && type == llvm::sys::fs::file_type::regular_file) {
      files.push_back(this->joinPath(step.dir, name));
    }
    if (isMatch && !isLast && type == llvm::sys::fs::file_type::directory_file) {
      steps.push_back({.dir = this->joinPath(step.dir, name), .segment = step.segment + 1});
    }
  }
}

std::string NativeGlob::joinPath(llvm::StringRef dir, llvm::StringRef name) const {
  return dir.str() + "/" + name.str();
}

}
