#pragma once

#include "compiler/program/source_file/source_file.h"
#include "llvm/Support/SMLoc.h"
#include "llvm/Support/SourceMgr.h"
#include <utility>
#include <vector>

namespace tz {

class LibraryRanges {
public:
  static LibraryRanges collect(const SourceFile &file, const llvm::SourceMgr &sourceMgr);

  bool contains(llvm::SMLoc loc) const;

private:
  std::vector<std::pair<const char *, const char *>> ranges;
};

}
