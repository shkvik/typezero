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

// Base of every AST node. Nodes are owned by AstContext and linked by raw
// pointers. Subclasses use LLVM-style RTTI (`classof`), since the compiler is
// built without C++ RTTI.
class Node {
public:
  virtual ~Node() = default;
};

} // namespace tz
