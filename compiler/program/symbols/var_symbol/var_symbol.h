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

struct FunctionDecl;
class RegexLitExpr;
class Type;

struct VarSymbol {
  llvm::StringRef name;
  llvm::SMLoc loc;
  const Type *type = nullptr;
  bool isConst = false;
  bool isGlobal = false;
  bool usedInFunction = false;
  bool literalConst = false;
  bool read = false;

  FunctionDecl *owner = nullptr;
  unsigned declSeq = 0;
  bool captured = false;
  bool reassigned = false;
  bool capturedEarly = false;
  FunctionDecl *function = nullptr;
  FunctionDecl *lambda = nullptr;
  const RegexLitExpr *regex = nullptr;
  const FunctionDecl *reflectMethod = nullptr;

  bool boxed() const { return captured && (reassigned || capturedEarly); }
};

}
