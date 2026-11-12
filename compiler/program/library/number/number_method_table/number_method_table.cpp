#include "compiler/program/library/number/number_method_table/number_method_table.h"
#include <cctype>

namespace tz {

namespace {

constexpr llvm::StringLiteral kConversionPrefix = "to";

const NumberMethodInfo kRows[] = {
    {
        .name = "toInt",
        .method = NumberMethod::TO_INT,
        .convertsTo = "int",
    },
    {
        .name = "toInt8",
        .method = NumberMethod::TO_INT8,
        .convertsTo = "int8",
    },
    {
        .name = "toInt16",
        .method = NumberMethod::TO_INT16,
        .convertsTo = "int16",
    },
    {
        .name = "toInt32",
        .method = NumberMethod::TO_INT32,
        .convertsTo = "int32",
    },
    {
        .name = "toUint8",
        .method = NumberMethod::TO_UINT8,
        .convertsTo = "uint8",
    },
    {
        .name = "toUint16",
        .method = NumberMethod::TO_UINT16,
        .convertsTo = "uint16",
    },
    {
        .name = "toUint32",
        .method = NumberMethod::TO_UINT32,
        .convertsTo = "uint32",
    },
    {
        .name = "toUint64",
        .method = NumberMethod::TO_UINT64,
        .convertsTo = "uint64",
    },
    {
        .name = "toDouble",
        .method = NumberMethod::TO_DOUBLE,
        .convertsTo = "double",
    },
    {
        .name = "toDecimal",
        .method = NumberMethod::TO_DECIMAL,
        .convertsTo = "decimal",
    },
    {
        .name = "toString",
        .method = NumberMethod::TO_STRING,
        .convertsTo = "",
    },
    {
        .name = "toFixed",
        .method = NumberMethod::TO_FIXED,
        .convertsTo = "",
    },
};

}

const NumberMethodInfo *NumberMethodTable::find(llvm::StringRef name) {
  for (const NumberMethodInfo &info : kRows) {
    if (info.name == name) {
      return &info;
    }
  }

  return nullptr;
}

std::string NumberMethodTable::getConversionName(llvm::StringRef typeName) {
  for (const NumberMethodInfo &info : kRows) {
    if (!info.convertsTo.empty() && info.convertsTo == typeName) {
      return info.name.str();
    }
  }
  std::string name = typeName.str();
  name[0] = char(std::toupper(name[0]));

  return kConversionPrefix.str() + name;
}

}
