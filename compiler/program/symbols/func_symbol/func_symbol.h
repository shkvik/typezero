#pragma once

#include "compiler/program/symbols/completer/completer.h"
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

struct TypePredicate {
  int param = -1;
  const Type *type = nullptr;
};

class FuncSymbol {
public:
  bool isGeneric() const;
  bool hasReturnType() const;
  const Type *getReturnType();
  void setCompleter(Completer *engine);
  void setReturnType(const Type *type);
  const Type *getKnownReturnType() const;

  llvm::StringRef name;
  llvm::SMLoc loc;
  std::vector<const Type *> params;
  size_t minArgs = 0;
  bool hasRest = false;
  bool hasThis = false;
  bool inferring = false;
  const Type *assumed = nullptr;
  FunctionDecl *decl = nullptr;
  bool discardsResult = false;
  bool pendingParams = false;
  std::vector<const Type *> typeParams;
  std::vector<const Type *> typeDefaults;
  std::optional<TypePredicate> predicate;

private:
  const Type *ret = nullptr;
  Completer *completer = nullptr;
};

}
