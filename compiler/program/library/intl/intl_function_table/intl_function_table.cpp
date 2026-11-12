#include "compiler/program/library/intl/intl_function_table/intl_function_table.h"

namespace tz {

namespace {

const IntlFunctionInfo kRows[] = {
    {
        .name = "getCanonicalLocales",
        .value = IntlFunction::GET_CANONICAL_LOCALES,
    },
    {
        .name = "supportedValuesOf",
        .value = IntlFunction::SUPPORTED_VALUES_OF,
    },
    {
        .name = "supportedLocalesOf",
        .value = IntlFunction::SUPPORTED_LOCALES_OF,
    },
};

}

std::optional<IntlFunction> IntlFunctionTable::find(llvm::StringRef name) {
  for (const IntlFunctionInfo &info : kRows) {
    if (info.name == name) {
      return info.value;
    }
  }

  return std::nullopt;
}

}
