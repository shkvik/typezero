#pragma once

#include "compiler/union/union/union.h"
#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz::pkg {

enum class SourceKind {
  JSR_SOURCE,
  GIT_SOURCE,
  LOCAL_SOURCE,
};

struct JsrSource {
  static constexpr SourceKind kind = SourceKind::JSR_SOURCE;
  std::string scope;
  std::string name;
  std::string range;
};

struct GitSource {
  static constexpr SourceKind kind = SourceKind::GIT_SOURCE;
  std::string url;
  std::string ref;
};

struct LocalSource {
  static constexpr SourceKind kind = SourceKind::LOCAL_SOURCE;
  std::string dir;
};

using SourceForm = Union<JsrSource, GitSource, LocalSource>;

struct Source {
  SourceForm form = JsrSource{};

  const std::string &scope() const;
  const std::string &name() const;
  const std::string &range() const;
  const std::string &url() const;
  const std::string &ref() const;
  const std::string &dir() const;
  std::string id() const;
  std::string shortName() const;
};

bool parseSource(llvm::StringRef spec, llvm::StringRef baseDir, Source &out, std::string &error);

std::string repositoryName(llvm::StringRef url);

}
