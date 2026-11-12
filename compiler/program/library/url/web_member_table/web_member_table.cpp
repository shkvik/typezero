#include "compiler/program/library/url/web_member_table/web_member_table.h"
#include "llvm/Support/ErrorHandling.h"

namespace tz {

namespace {

const WebMemberInfo kRows[] = {
    {
        .name = "url",
        .value = WebMember::URL_FIELD,
    },
    {
        .name = "href",
        .value = WebMember::HREF,
    },
};

}

std::optional<WebMember> WebMemberTable::find(llvm::StringRef name) {
  for (const WebMemberInfo &info : kRows) {
    if (info.name == name) {
      return info.value;
    }
  }

  return std::nullopt;
}

llvm::StringRef WebMemberTable::getName(WebMember value) {
  for (const WebMemberInfo &info : kRows) {
    if (info.value == value) {
      return info.name;
    }
  }
  llvm_unreachable("web member missing from the table");
}

}
