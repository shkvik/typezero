#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class PlaceLowerer {
public:
  explicit PlaceLowerer(Lowering &lowering);

  llvm::Constant *frameDescriptor(llvm::SMLoc loc, const std::string *function = nullptr);
  std::string frameFunctionName() const;

private:
  Lowering &lowering_;
  std::map<std::pair<std::string, const char *>, llvm::Constant *> frames_;
  llvm::StringMap<std::string> filePaths_;
};

}
