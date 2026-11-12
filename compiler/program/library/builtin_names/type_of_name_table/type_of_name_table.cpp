#include "compiler/program/library/builtin_names/type_of_name_table/type_of_name_table.h"
#include "llvm/Support/ErrorHandling.h"

namespace tz {

namespace {

const TypeOfNameInfo kRows[] = {
    {
        .name = "undefined",
        .value = TypeOfName::UNDEFINED_TYPE,
    },
    {
        .name = "null",
        .value = TypeOfName::NULL_TYPE,
    },
    {
        .name = "boolean",
        .value = TypeOfName::BOOLEAN,
    },
    {
        .name = "string",
        .value = TypeOfName::STRING,
    },
    {
        .name = "double",
        .value = TypeOfName::DOUBLE,
    },
    {
        .name = "array",
        .value = TypeOfName::ARRAY,
    },
    {
        .name = "object",
        .value = TypeOfName::OBJECT,
    },
    {
        .name = "number",
        .value = TypeOfName::NUMBER,
    },
    {
        .name = "bigint",
        .value = TypeOfName::BIGINT,
    },
    {
        .name = "symbol",
        .value = TypeOfName::SYMBOL,
    },
};

}

std::optional<TypeOfName> TypeOfNameTable::find(llvm::StringRef name) {
  for (const TypeOfNameInfo &info : kRows) {
    if (info.name == name) {
      return info.value;
    }
  }

  return std::nullopt;
}

llvm::StringRef TypeOfNameTable::getName(TypeOfName value) {
  for (const TypeOfNameInfo &info : kRows) {
    if (info.value == value) {
      return info.name;
    }
  }
  llvm_unreachable("type of name missing from the table");
}

}
