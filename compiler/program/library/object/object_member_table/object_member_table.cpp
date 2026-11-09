#include "compiler/program/library/object/object_member_table/object_member_table.h"
#include "llvm/Support/ErrorHandling.h"

namespace tz {

namespace {

const ObjectMemberInfo kObjectMembers[] = {
    {
        .name = "toString",
        .member = ObjectMember::TO_STRING,
    },
    {
        .name = "valueOf",
        .member = ObjectMember::VALUE_OF,
    },
    {
        .name = "hasOwnProperty",
        .member = ObjectMember::HAS_OWN_PROPERTY,
    },
    {
        .name = "isPrototypeOf",
        .member = ObjectMember::IS_PROTOTYPE_OF,
    },
    {
        .name = "toLocaleString",
        .member = ObjectMember::TO_LOCALE_STRING,
    },
    {
        .name = "propertyIsEnumerable",
        .member = ObjectMember::PROPERTY_IS_ENUMERABLE,
    },
    {
        .name = "constructor",
        .member = ObjectMember::CONSTRUCTOR,
    },
    {
        .name = "__proto__",
        .member = ObjectMember::PROTO,
    },
};

}

std::optional<ObjectMember> ObjectMemberTable::find(llvm::StringRef name) {
  for (const ObjectMemberInfo &info : kObjectMembers) {
    if (info.name == name) {
      return info.member;
    }
  }

  return std::nullopt;
}

llvm::StringRef ObjectMemberTable::getName(ObjectMember member) {
  for (const ObjectMemberInfo &info : kObjectMembers) {
    if (info.member == member) {
      return info.name;
    }
  }
  llvm_unreachable("object member missing from the table");
}

}
