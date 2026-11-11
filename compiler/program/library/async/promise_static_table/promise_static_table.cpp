#include "compiler/program/library/async/promise_static_table/promise_static_table.h"

namespace tz {

namespace {

const PromiseStaticInfo kRows[] = {
    {
        .name = "resolve",
        .value = PromiseStatic::RESOLVE,
    },
    {
        .name = "reject",
        .value = PromiseStatic::REJECT,
    },
    {
        .name = "allSettled",
        .value = PromiseStatic::ALL_SETTLED,
    },
    {
        .name = "any",
        .value = PromiseStatic::ANY,
    },
    {
        .name = "withResolvers",
        .value = PromiseStatic::WITH_RESOLVERS,
    },
    {
        .name = "all",
        .value = PromiseStatic::ALL,
    },
    {
        .name = "race",
        .value = PromiseStatic::RACE,
    },
};

}

std::optional<PromiseStatic> PromiseStaticTable::find(llvm::StringRef name) {
  for (const PromiseStaticInfo &info : kRows) {
    if (info.name == name) {
      return info.value;
    }
  }

  return std::nullopt;
}

}
