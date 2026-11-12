#pragma once

#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "llvm/ADT/StringRef.h"
#include <optional>

namespace tz {

enum class GlobalMember {
  ARRAY_FROM,
  ARRAY_IS_ARRAY,
  ITERATOR_FROM,
  JSON_IS_JSON,
  MAP_GROUP_BY,
  OBJECT_KEYS,
  OBJECT_VALUES,
  OBJECT_ENTRIES,
  OBJECT_FROM_ENTRIES,
  OBJECT_GROUP_BY,
  SYMBOL_ITERATOR,
  SYMBOL_ASYNC_ITERATOR,
  CRYPTO_SUBTLE,
  DATE_NOW,
  PERFORMANCE_NOW,
};

struct GlobalMemberInfo {
  GlobalName owner;
  llvm::StringLiteral name;
  GlobalMember member;
};

class GlobalMemberTable {
public:
  static std::optional<GlobalMember> find(GlobalName owner, llvm::StringRef name);
  static llvm::StringRef getName(GlobalMember member);
  static GlobalName getOwner(GlobalMember member);
};

}
