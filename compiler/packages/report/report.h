#pragma once

#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"

namespace tz::pkg {

// Error output: "tz: message", or "file:line:col: error: message" with the
// line and a caret.
void report(const llvm::Twine &message);
void reportAt(llvm::StringRef file, llvm::StringRef text, size_t offset,
              const llvm::Twine &message);

// Says what tz downloads, on stderr: "tz: downloading jsr:@std/path 1.0.8".
void progress(const llvm::Twine &message);

} // namespace tz::pkg
