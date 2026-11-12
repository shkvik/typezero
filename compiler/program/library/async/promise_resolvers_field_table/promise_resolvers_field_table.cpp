#include "compiler/program/library/async/promise_resolvers_field_table/promise_resolvers_field_table.h"
#include "llvm/Support/ErrorHandling.h"

namespace tz {

namespace {

const PromiseResolversFieldInfo kRows[] = {
    {
        .name = "promise",
        .value = PromiseResolversField::PROMISE,
    },
    {
        .name = "resolve",
        .value = PromiseResolversField::RESOLVE,
    },
};

}

std::optional<PromiseResolversField> PromiseResolversFieldTable::find(llvm::StringRef name) {
  for (const PromiseResolversFieldInfo &info : kRows) {
    if (info.name == name) {
      return info.value;
    }
  }

  return std::nullopt;
}

llvm::StringRef PromiseResolversFieldTable::getName(PromiseResolversField value) {
  for (const PromiseResolversFieldInfo &info : kRows) {
    if (info.value == value) {
      return info.name;
    }
  }
  llvm_unreachable("promise resolvers field missing from the table");
}

}
