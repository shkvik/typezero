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

struct TypeRef {
  enum class Kind {
    Name,
    Array,
    Union,
    Generic,
    Function,
    Object,
    Tuple,
    Literal,
    Predicate,
    KeyOf,
    Indexed,
    TypeOf,
    Mapped,
    Conditional,
    Infer,
    Template,
    Intersection,
  };

  Kind kind = Kind::Name;
  llvm::StringRef name;
  llvm::SMLoc loc;
  std::vector<TypeRef> args;
  std::vector<bool> optional;
  bool rest = false;
  std::vector<llvm::StringRef> names;
  std::vector<llvm::SMLoc> locs;
  std::vector<bool> readonly;
  std::vector<bool> methods;
  std::vector<bool> spread;
  int mappedOptional = 0;
  int mappedReadonly = 0;
  std::u16string text;
  double number = 0;
  bool integer = false;
  bool asserts = false;
  std::vector<std::u16string> texts;
  llvm::StringRef source;
};

}
