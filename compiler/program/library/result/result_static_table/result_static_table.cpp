#include "compiler/program/library/result/result_static_table/result_static_table.h"

namespace tz {

namespace {

const ResultStaticInfo kRows[] = {
    {
        .name = "combine",
        .value = ResultStatic::COMBINE,
    },
    {
        .name = "combineWithAllErrors",
        .value = ResultStatic::COMBINE_WITH_ALL_ERRORS,
    },
};

}

std::optional<ResultStatic> ResultStaticTable::find(llvm::StringRef name) {
  for (const ResultStaticInfo &info : kRows) {
    if (info.name == name) {
      return info.value;
    }
  }

  return std::nullopt;
}

}
