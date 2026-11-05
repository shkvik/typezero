#pragma once

#include "compiler/program/symbols/var_symbol/var_holds.h"
#include "compiler/program/symbols/var_symbol/var_place.h"
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
  bool literalConst = false;
  bool read = false;
  bool reassigned = false;

  VarPlace place = TopLevelLocalVar{};
  VarHolds holds = OrdinaryValue{};

  static VarPlace localPlace(FunctionDecl *owner, unsigned declSeq);

  LocalVarState *local();
  const LocalVarState *local() const;
  bool isGlobal() const;
  bool usedInFunction() const;
  FunctionDecl *owner() const;
  unsigned declSeq() const;
  bool captured() const;
  bool capturedEarly() const;
  bool unshared() const;
  bool boxed() const;

  FunctionDecl *function() const;
  FunctionDecl *lambda() const;
  const RegexLitExpr *regex() const;
  const FunctionDecl *reflectMethod() const;
  const std::vector<llvm::StringRef> &reflectNames() const;
};

}
