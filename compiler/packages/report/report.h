#pragma once

#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"

namespace tz::pkg {

void report(const llvm::Twine &message);
void reportAt(llvm::StringRef file, llvm::StringRef text, size_t offset,
              const llvm::Twine &message);

void progress(const llvm::Twine &message);

}
