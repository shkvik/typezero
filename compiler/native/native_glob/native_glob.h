#pragma once

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include <string>
#include <vector>

namespace tz {

class NativeGlob {
public:
  std::vector<std::string> findFiles(llvm::StringRef pattern) const;
  bool isPattern(llvm::StringRef path) const;

private:
  struct GlobStep {
    std::string dir;
    size_t segment = 0;
  };

  bool isPlainSegment(llvm::StringRef segment) const;
  void followSegment(const GlobStep &step, llvm::ArrayRef<std::string> segments,
                     std::vector<GlobStep> &steps, std::vector<std::string> &files) const;
  void followAnyDepth(const GlobStep &step, std::vector<GlobStep> &steps) const;
  void followMatches(const GlobStep &step, llvm::ArrayRef<std::string> segments,
                     std::vector<GlobStep> &steps, std::vector<std::string> &files) const;
  std::string joinPath(llvm::StringRef dir, llvm::StringRef name) const;
};

}
