#include "compiler/program/library/http/headers_method_table/headers_method_table.h"

namespace tz {

namespace {

const HeadersMethodInfo kRows[] = {
    {
        .name = "append",
        .value = HeadersMethod::APPEND_HEADER,
    },
    {
        .name = "set",
        .value = HeadersMethod::SET_HEADER,
    },
    {
        .name = "get",
        .value = HeadersMethod::GET_HEADER,
    },
    {
        .name = "has",
        .value = HeadersMethod::HAS_HEADER,
    },
    {
        .name = "delete",
        .value = HeadersMethod::DELETE_HEADER,
    },
};

}

std::optional<HeadersMethod> HeadersMethodTable::find(llvm::StringRef name) {
  for (const HeadersMethodInfo &info : kRows) {
    if (info.name == name) {
      return info.value;
    }
  }

  return std::nullopt;
}

}
