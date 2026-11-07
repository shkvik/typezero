#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class PlaceLowerer {
public:
  explicit PlaceLowerer(Lowering &owner);

  llvm::Constant *frameDescriptor(llvm::SMLoc loc, const std::string *function = nullptr);
  std::string frameFunctionName() const;

private:
  Lowering &lowering;
  std::map<std::pair<std::string, const char *>, llvm::Constant *> frames;
  llvm::StringMap<std::string> filePaths;
};

}
