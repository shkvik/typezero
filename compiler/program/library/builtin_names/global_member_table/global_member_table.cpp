#include "compiler/program/library/builtin_names/global_member_table/global_member_table.h"
#include "llvm/Support/ErrorHandling.h"

namespace tz {

namespace {

const GlobalMemberInfo kGlobalMembers[] = {
    {
        .owner = GlobalName::ARRAY,
        .name = "from",
        .member = GlobalMember::ARRAY_FROM,
    },
    {
        .owner = GlobalName::ARRAY,
        .name = "isArray",
        .member = GlobalMember::ARRAY_IS_ARRAY,
    },
    {
        .owner = GlobalName::ITERATOR,
        .name = "from",
        .member = GlobalMember::ITERATOR_FROM,
    },
    {
        .owner = GlobalName::JSON_TYPE,
        .name = "isJson",
        .member = GlobalMember::JSON_IS_JSON,
    },
    {
        .owner = GlobalName::MAP,
        .name = "groupBy",
        .member = GlobalMember::MAP_GROUP_BY,
    },
    {
        .owner = GlobalName::OBJECT,
        .name = "keys",
        .member = GlobalMember::OBJECT_KEYS,
    },
    {
        .owner = GlobalName::OBJECT,
        .name = "values",
        .member = GlobalMember::OBJECT_VALUES,
    },
    {
        .owner = GlobalName::OBJECT,
        .name = "entries",
        .member = GlobalMember::OBJECT_ENTRIES,
    },
    {
        .owner = GlobalName::OBJECT,
        .name = "fromEntries",
        .member = GlobalMember::OBJECT_FROM_ENTRIES,
    },
    {
        .owner = GlobalName::OBJECT,
        .name = "groupBy",
        .member = GlobalMember::OBJECT_GROUP_BY,
    },
    {
        .owner = GlobalName::SYMBOL,
        .name = "iterator",
        .member = GlobalMember::SYMBOL_ITERATOR,
    },
    {
        .owner = GlobalName::SYMBOL,
        .name = "asyncIterator",
        .member = GlobalMember::SYMBOL_ASYNC_ITERATOR,
    },
    {
        .owner = GlobalName::CRYPTO,
        .name = "subtle",
        .member = GlobalMember::CRYPTO_SUBTLE,
    },
    {
        .owner = GlobalName::DATE,
        .name = "now",
        .member = GlobalMember::DATE_NOW,
    },
    {
        .owner = GlobalName::PERFORMANCE,
        .name = "now",
        .member = GlobalMember::PERFORMANCE_NOW,
    },
};

}

std::optional<GlobalMember> GlobalMemberTable::find(GlobalName owner, llvm::StringRef name) {
  for (const GlobalMemberInfo &info : kGlobalMembers) {
    if (info.owner == owner && info.name == name) {
      return info.member;
    }
  }

  return std::nullopt;
}

llvm::StringRef GlobalMemberTable::getName(GlobalMember member) {
  for (const GlobalMemberInfo &info : kGlobalMembers) {
    if (info.member == member) {
      return info.name;
    }
  }
  llvm_unreachable("global member missing from the table");
}

GlobalName GlobalMemberTable::getOwner(GlobalMember member) {
  for (const GlobalMemberInfo &info : kGlobalMembers) {
    if (info.member == member) {
      return info.owner;
    }
  }
  llvm_unreachable("global member missing from the table");
}

std::string GlobalMemberTable::getQualifiedName(GlobalMember member) {
  auto owner = GlobalMemberTable::getOwner(member);
  auto ownerName = GlobalNameTable::getName(owner);
  auto memberName = GlobalMemberTable::getName(member);

  return ownerName.str() + "." + memberName.str();
}

}
