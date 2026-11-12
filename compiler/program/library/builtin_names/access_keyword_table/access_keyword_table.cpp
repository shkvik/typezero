#include "compiler/program/library/builtin_names/access_keyword_table/access_keyword_table.h"

namespace tz {

namespace {

const AccessKeywordInfo kRows[] = {
    {
        .name = "public",
        .access = Access::Public,
    },
    {
        .name = "protected",
        .access = Access::Protected,
    },
    {
        .name = "private",
        .access = Access::Private,
    },
};

}

llvm::StringRef AccessKeywordTable::getName(Access access) {
  for (const AccessKeywordInfo &info : kRows) {
    if (info.access == access) {
      return info.name;
    }
  }

  return "private";
}

}
