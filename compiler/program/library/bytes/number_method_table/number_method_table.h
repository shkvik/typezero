#pragma once

#include "compiler/program/types/type/type.h"
#include "llvm/ADT/StringRef.h"
#include <optional>

namespace tz {

struct BytesNumberMethod {
  bool write = false;
  Type::Bytes kind = Type::Bytes::None;
  bool big = false;
  bool variable = false, isSigned = false;
  bool bigint = false;
};

struct NumberKindInfo {
  llvm::StringLiteral name;
  Type::Bytes kind;
};

class NumberMethodTable {
public:
  static std::optional<BytesNumberMethod> parse(llvm::StringRef name, bool dataView);

private:
  static bool readAccess(llvm::StringRef &rest, bool dataView, BytesNumberMethod &method);
  static bool readKind(llvm::StringRef rest, bool dataView, BytesNumberMethod &method);
};

}
