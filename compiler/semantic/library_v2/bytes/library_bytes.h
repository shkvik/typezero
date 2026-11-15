#pragma once

#include "compiler/program/types/types.h"
#include "llvm/ADT/StringRef.h"

namespace tz {

class LibraryBytes {
public:
  const Type *bytesTypeNamed(llvm::StringRef name) const;
};

}
