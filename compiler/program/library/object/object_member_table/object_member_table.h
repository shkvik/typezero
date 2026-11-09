#pragma once

#include "llvm/ADT/StringRef.h"
#include <optional>

namespace tz {

enum class ObjectMember {
  TO_STRING,
  VALUE_OF,
  HAS_OWN_PROPERTY,
  IS_PROTOTYPE_OF,
  TO_LOCALE_STRING,
  PROPERTY_IS_ENUMERABLE,
  CONSTRUCTOR,
  PROTO,
};

struct ObjectMemberInfo {
  llvm::StringLiteral name;
  ObjectMember member;
};

class ObjectMemberTable {
public:
  static std::optional<ObjectMember> find(llvm::StringRef name);
  static llvm::StringRef getName(ObjectMember member);
};

}
