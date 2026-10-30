#pragma once

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/SMLoc.h"
#include "llvm/Support/SourceMgr.h"
#include <optional>
#include <string>
#include <vector>

namespace tz {

struct Module;

struct SourcePlace {
  std::string path;
  unsigned line = 0;
  unsigned character = 0;
  unsigned endLine = 0;
  unsigned endCharacter = 0;
  bool file = true;
};

class SourcePlaces {
public:
  explicit SourcePlaces(const llvm::SourceMgr &sourceMgr);

  void nameFiles(const std::vector<Module *> &modules);

  std::optional<SourcePlace> find(llvm::SMLoc loc) const;
  std::optional<SourcePlace> locate(llvm::SMLoc loc) const;
  std::optional<SourcePlace> span(const char *begin, const char *end) const;
  llvm::StringRef lineText(llvm::SMLoc loc) const;
  std::string describe(llvm::SMLoc loc) const;

  static unsigned utf16Length(llvm::StringRef text);
  static std::string absolute(llvm::StringRef path);
  static const char *tokenEnd(const char *at, const char *end);

private:
  unsigned bufferOf(const char *at) const;
  void position(unsigned id, const char *at, unsigned &line, unsigned &character) const;
  std::string pathOf(unsigned id, bool &file) const;

  const llvm::SourceMgr &sourceMgr_;
  mutable unsigned last_ = 0;
  mutable llvm::DenseMap<unsigned, std::pair<std::string, bool>> paths_;
};

}
