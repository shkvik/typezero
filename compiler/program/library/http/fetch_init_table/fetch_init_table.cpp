#include "compiler/program/library/http/fetch_init_table/fetch_init_table.h"
#include "llvm/Support/ErrorHandling.h"

namespace tz {

namespace {

const FetchInitInfo kRows[] = {
    {
        .name = "status",
        .value = FetchInit::STATUS,
    },
    {
        .name = "statusText",
        .value = FetchInit::STATUS_TEXT,
    },
    {
        .name = "headers",
        .value = FetchInit::HEADERS,
    },
    {
        .name = "method",
        .value = FetchInit::METHOD,
    },
    {
        .name = "body",
        .value = FetchInit::BODY,
    },
};

}

std::optional<FetchInit> FetchInitTable::find(llvm::StringRef name) {
  for (const FetchInitInfo &info : kRows) {
    if (info.name == name) {
      return info.value;
    }
  }

  return std::nullopt;
}

llvm::StringRef FetchInitTable::getName(FetchInit value) {
  for (const FetchInitInfo &info : kRows) {
    if (info.value == value) {
      return info.name;
    }
  }
  llvm_unreachable("fetch init missing from the table");
}

}
