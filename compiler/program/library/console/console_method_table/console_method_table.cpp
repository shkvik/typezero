#include "compiler/program/library/console/console_method_table/console_method_table.h"
#include "llvm/Support/ErrorHandling.h"

namespace tz {

namespace {

const ConsoleMethodInfo kConsoleMethods[] = {
    {
        .name = "error",
        .method = ConsoleMethod::ERROR_MESSAGE,
        .isToStderr = true,
    },
    {
        .name = "warn",
        .method = ConsoleMethod::WARN_MESSAGE,
        .isToStderr = true,
    },
    {
        .name = "log",
        .method = ConsoleMethod::LOG_MESSAGE,
        .isToStderr = false,
    },
    {
        .name = "info",
        .method = ConsoleMethod::INFO_MESSAGE,
        .isToStderr = false,
    },
    {
        .name = "debug",
        .method = ConsoleMethod::DEBUG_MESSAGE,
        .isToStderr = false,
    },
};

}

std::optional<ConsoleMethod> ConsoleMethodTable::find(llvm::StringRef name) {
  for (const ConsoleMethodInfo &info : kConsoleMethods) {
    if (info.name == name) {
      return info.method;
    }
  }

  return std::nullopt;
}

bool ConsoleMethodTable::isToStderr(ConsoleMethod method) {
  for (const ConsoleMethodInfo &info : kConsoleMethods) {
    if (info.method == method) {
      return info.isToStderr;
    }
  }
  llvm_unreachable("console method missing from the table");
}

}
