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
class Type;

struct FuncSymbol {
  llvm::StringRef name;
  llvm::SMLoc loc;
  std::vector<const Type *> params;
  size_t minArgs = 0;
  bool hasRest = false;
  bool hasThis = false;
  const Type *ret = nullptr;
  bool inferring = false;
  const Type *assumed = nullptr;
  FunctionDecl *decl = nullptr;
  bool discardsResult = false;
  bool pendingParams = false;
  std::vector<const Type *> typeParams;
  std::vector<const Type *> typeDefaults;
  bool isGeneric() const { return !typeParams.empty(); }
  int predicateParam = -2;
  const Type *predicateType = nullptr;
};

}
