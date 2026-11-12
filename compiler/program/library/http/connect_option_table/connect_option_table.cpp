#include "compiler/program/library/http/connect_option_table/connect_option_table.h"
#include "llvm/Support/ErrorHandling.h"

namespace tz {

namespace {

const ConnectOptionInfo kRows[] = {
    {
        .name = "secure",
        .value = ConnectOption::SECURE,
    },
    {
        .name = "transport",
        .value = ConnectOption::TRANSPORT,
    },
};

}

std::optional<ConnectOption> ConnectOptionTable::find(llvm::StringRef name) {
  for (const ConnectOptionInfo &info : kRows) {
    if (info.name == name) {
      return info.value;
    }
  }

  return std::nullopt;
}

llvm::StringRef ConnectOptionTable::getName(ConnectOption value) {
  for (const ConnectOptionInfo &info : kRows) {
    if (info.value == value) {
      return info.name;
    }
  }
  llvm_unreachable("connect option missing from the table");
}

}
