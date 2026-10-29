#pragma once

#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

class SourceOverlay {
public:
  void set(llvm::StringRef path, llvm::StringRef text);
  const std::string *find(llvm::StringRef path) const;
  bool empty() const { return texts_.empty(); }

  static std::string key(llvm::StringRef path);

private:
  llvm::StringMap<std::string> texts_;
};

}
