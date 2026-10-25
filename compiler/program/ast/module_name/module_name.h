#pragma once

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/SMLoc.h"
#include <cstdint>
#include <deque>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace tz {

// `a` or `a as b` in the braces of an import or an export.
struct ModuleName {
  llvm::StringRef name; // in the file it comes from
  llvm::SMLoc loc;
  llvm::StringRef as; // the name it gets; the same as `name` without `as`
  llvm::SMLoc asLoc;
};

} // namespace tz
