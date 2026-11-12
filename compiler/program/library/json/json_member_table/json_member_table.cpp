#include "compiler/program/library/json/json_member_table/json_member_table.h"
#include "llvm/Support/ErrorHandling.h"

namespace tz {

namespace {

const JsonMemberInfo kRows[] = {
    {
        .name = "toJSON",
        .value = JsonMember::TO_JSON,
    },
};

}

std::optional<JsonMember> JsonMemberTable::find(llvm::StringRef name) {
  for (const JsonMemberInfo &info : kRows) {
    if (info.name == name) {
      return info.value;
    }
  }

  return std::nullopt;
}

llvm::StringRef JsonMemberTable::getName(JsonMember value) {
  for (const JsonMemberInfo &info : kRows) {
    if (info.value == value) {
      return info.name;
    }
  }
  llvm_unreachable("json member missing from the table");
}

}
