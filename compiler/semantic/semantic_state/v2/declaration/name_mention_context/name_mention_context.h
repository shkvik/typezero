#pragma once

#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSet.h"

namespace tz {

class NameMentionContext {
public:
  void markUsedInFunction(llvm::StringRef name);


  bool isUsedInFunction(llvm::StringRef name) const;

private:
  llvm::StringSet<> namesInFunctions;
};

}
