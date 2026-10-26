#pragma once

#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"

namespace tz::pkg {

int usage(const llvm::Twine &message, llvm::StringRef synopsis);

}
