#include "compiler/program/library/regex/match_member_table/match_member_table.h"
#include "llvm/Support/ErrorHandling.h"

namespace tz {

namespace {

const MatchMemberInfo kRows[] = {
    {
        .name = "index",
        .value = MatchMember::INDEX,
    },
    {
        .name = "input",
        .value = MatchMember::INPUT,
    },
    {
        .name = "groups",
        .value = MatchMember::GROUPS,
    },
    {
        .name = "indices",
        .value = MatchMember::INDICES,
    },
};

}

std::optional<MatchMember> MatchMemberTable::find(llvm::StringRef name) {
  for (const MatchMemberInfo &info : kRows) {
    if (info.name == name) {
      return info.value;
    }
  }

  return std::nullopt;
}

llvm::StringRef MatchMemberTable::getName(MatchMember value) {
  for (const MatchMemberInfo &info : kRows) {
    if (info.value == value) {
      return info.name;
    }
  }
  llvm_unreachable("match member missing from the table");
}

}
