#include "compiler/program/library/decimal/decimal_static_table/decimal_static_table.h"

namespace tz {

namespace {

const DecimalStaticInfo kDecimalStatics[] = {
    {
        .name = "max",
        .builtin = Builtin::DecMax,
    },
    {
        .name = "min",
        .builtin = Builtin::DecMin,
    },
    {
        .name = "sum",
        .builtin = Builtin::DecSum,
    },
};

}

const DecimalStaticInfo *DecimalStaticTable::find(llvm::StringRef name) {
  for (const DecimalStaticInfo &info : kDecimalStatics) {
    if (info.name == name) {
      return &info;
    }
  }

  return nullptr;
}

}
