#pragma once

#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"

namespace tz::pkg {

class Report {
public:
  static void report(const llvm::Twine &message);
  static void progress(const llvm::Twine &message);
  static void reportAt(llvm::StringRef file, llvm::StringRef text, size_t offset,
                       const llvm::Twine &message);
};

}
