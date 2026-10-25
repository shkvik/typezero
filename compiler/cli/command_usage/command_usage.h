#pragma once

#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"

namespace tz::pkg {

// "tz: message" and the synopsis of the command; the exit code of a usage error.
int usage(const llvm::Twine &message, llvm::StringRef synopsis);

} // namespace tz::pkg
