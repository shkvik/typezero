#include "compiler/program/library/regex/regex_member_table/regex_member_table.h"

namespace tz {

namespace {

const RegexMemberInfo kRows[] = {
    {
        .name = "test",
        .value = RegexMember::TEST,
    },
    {
        .name = "exec",
        .value = RegexMember::EXEC,
    },
    {
        .name = "toString",
        .value = RegexMember::TO_STRING,
    },
    {
        .name = "compile",
        .value = RegexMember::COMPILE,
    },
    {
        .name = "lastIndex",
        .value = RegexMember::LAST_INDEX,
    },
    {
        .name = "source",
        .value = RegexMember::SOURCE,
    },
    {
        .name = "flags",
        .value = RegexMember::FLAGS,
    },
};

}

std::optional<RegexMember> RegexMemberTable::find(llvm::StringRef name) {
  for (const RegexMemberInfo &info : kRows) {
    if (info.name == name) {
      return info.value;
    }
  }

  return std::nullopt;
}

}
