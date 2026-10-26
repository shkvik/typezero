#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz::pkg {

struct Source {
  enum class Kind { Jsr, Git, Local };
  Kind kind = Kind::Jsr;
  std::string scope, name;
  std::string range;
  std::string url;
  std::string ref;
  std::string dir;

  std::string id() const;
  std::string shortName() const;
};

bool parseSource(llvm::StringRef spec, llvm::StringRef baseDir, Source &out, std::string &error);

std::string repositoryName(llvm::StringRef url);

}
