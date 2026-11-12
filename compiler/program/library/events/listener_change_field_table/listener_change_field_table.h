#pragma once

#include "llvm/ADT/StringRef.h"
#include <optional>

namespace tz {

enum class ListenerChangeField {
  EVENT,
  LISTENER,
};

struct ListenerChangeFieldInfo {
  llvm::StringLiteral name;
  ListenerChangeField value;
};

class ListenerChangeFieldTable {
public:
  static std::optional<ListenerChangeField> find(llvm::StringRef name);
  static llvm::StringRef getName(ListenerChangeField value);
};

}
