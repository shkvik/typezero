#include "compiler/program/library/async/promise_method_table/promise_method_table.h"

namespace tz {

namespace {

const PromiseMethodInfo kRows[] = {
    {
        .name = "then",
        .value = PromiseMethod::THEN,
    },
    {
        .name = "catch",
        .value = PromiseMethod::CATCH,
    },
    {
        .name = "finally",
        .value = PromiseMethod::FINALLY,
    },
};

}

std::optional<PromiseMethod> PromiseMethodTable::find(llvm::StringRef name) {
  for (const PromiseMethodInfo &info : kRows) {
    if (info.name == name) {
      return info.value;
    }
  }

  return std::nullopt;
}

}
