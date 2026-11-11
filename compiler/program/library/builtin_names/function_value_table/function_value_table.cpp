#include "compiler/program/library/builtin_names/function_value_table/function_value_table.h"

namespace tz {

namespace {

const FunctionValueGlobalInfo kGlobalRows[] = {
    {
        .global = GlobalName::STRING,
        .maxArgs = 1,
    },
    {
        .global = GlobalName::NUMBER,
        .maxArgs = 1,
    },
    {
        .global = GlobalName::PARSE_INT,
        .maxArgs = 2,
    },
    {
        .global = GlobalName::PARSE_FLOAT,
        .maxArgs = 1,
    },
    {
        .global = GlobalName::BOOLEAN,
        .maxArgs = 1,
    },
    {
        .global = GlobalName::ENCODE_URI_COMPONENT,
        .maxArgs = 1,
    },
    {
        .global = GlobalName::ENCODE_URI,
        .maxArgs = 1,
    },
    {
        .global = GlobalName::DECODE_URI_COMPONENT,
        .maxArgs = 1,
    },
    {
        .global = GlobalName::DECODE_URI,
        .maxArgs = 1,
    },
};

const FunctionValueStaticInfo kStaticRows[] = {
    {
        .owner = GlobalName::MATH,
        .name = "pow",
        .maxArgs = 2,
        .returnsNothing = false,
    },
    {
        .owner = GlobalName::MATH,
        .name = "atan2",
        .maxArgs = 2,
        .returnsNothing = false,
    },
    {
        .owner = GlobalName::MATH,
        .name = "imul",
        .maxArgs = 2,
        .returnsNothing = false,
    },
    {
        .owner = GlobalName::MATH,
        .name = "min",
        .maxArgs = FunctionValueTable::kAnyArgs,
        .returnsNothing = false,
    },
    {
        .owner = GlobalName::MATH,
        .name = "max",
        .maxArgs = FunctionValueTable::kAnyArgs,
        .returnsNothing = false,
    },
    {
        .owner = GlobalName::MATH,
        .name = "hypot",
        .maxArgs = FunctionValueTable::kAnyArgs,
        .returnsNothing = false,
    },
    {
        .owner = GlobalName::MATH,
        .name = "clamp",
        .maxArgs = 3,
        .returnsNothing = false,
    },
    {
        .owner = GlobalName::MATH,
        .name = "random",
        .maxArgs = 0,
        .returnsNothing = false,
    },
    {
        .owner = GlobalName::MATH,
        .name = "floor",
        .maxArgs = 1,
        .returnsNothing = false,
    },
    {
        .owner = GlobalName::MATH,
        .name = "ceil",
        .maxArgs = 1,
        .returnsNothing = false,
    },
    {
        .owner = GlobalName::MATH,
        .name = "round",
        .maxArgs = 1,
        .returnsNothing = false,
    },
    {
        .owner = GlobalName::MATH,
        .name = "trunc",
        .maxArgs = 1,
        .returnsNothing = false,
    },
    {
        .owner = GlobalName::MATH,
        .name = "abs",
        .maxArgs = 1,
        .returnsNothing = false,
    },
    {
        .owner = GlobalName::MATH,
        .name = "sign",
        .maxArgs = 1,
        .returnsNothing = false,
    },
    {
        .owner = GlobalName::MATH,
        .name = "sqrt",
        .maxArgs = 1,
        .returnsNothing = false,
    },
    {
        .owner = GlobalName::MATH,
        .name = "cbrt",
        .maxArgs = 1,
        .returnsNothing = false,
    },
    {
        .owner = GlobalName::MATH,
        .name = "exp",
        .maxArgs = 1,
        .returnsNothing = false,
    },
    {
        .owner = GlobalName::MATH,
        .name = "expm1",
        .maxArgs = 1,
        .returnsNothing = false,
    },
    {
        .owner = GlobalName::MATH,
        .name = "log",
        .maxArgs = 1,
        .returnsNothing = false,
    },
    {
        .owner = GlobalName::MATH,
        .name = "log1p",
        .maxArgs = 1,
        .returnsNothing = false,
    },
    {
        .owner = GlobalName::MATH,
        .name = "log2",
        .maxArgs = 1,
        .returnsNothing = false,
    },
    {
        .owner = GlobalName::MATH,
        .name = "log10",
        .maxArgs = 1,
        .returnsNothing = false,
    },
    {
        .owner = GlobalName::MATH,
        .name = "sin",
        .maxArgs = 1,
        .returnsNothing = false,
    },
    {
        .owner = GlobalName::MATH,
        .name = "cos",
        .maxArgs = 1,
        .returnsNothing = false,
    },
    {
        .owner = GlobalName::MATH,
        .name = "tan",
        .maxArgs = 1,
        .returnsNothing = false,
    },
    {
        .owner = GlobalName::MATH,
        .name = "asin",
        .maxArgs = 1,
        .returnsNothing = false,
    },
    {
        .owner = GlobalName::MATH,
        .name = "acos",
        .maxArgs = 1,
        .returnsNothing = false,
    },
    {
        .owner = GlobalName::MATH,
        .name = "atan",
        .maxArgs = 1,
        .returnsNothing = false,
    },
    {
        .owner = GlobalName::MATH,
        .name = "sinh",
        .maxArgs = 1,
        .returnsNothing = false,
    },
    {
        .owner = GlobalName::MATH,
        .name = "cosh",
        .maxArgs = 1,
        .returnsNothing = false,
    },
    {
        .owner = GlobalName::MATH,
        .name = "tanh",
        .maxArgs = 1,
        .returnsNothing = false,
    },
    {
        .owner = GlobalName::MATH,
        .name = "asinh",
        .maxArgs = 1,
        .returnsNothing = false,
    },
    {
        .owner = GlobalName::MATH,
        .name = "acosh",
        .maxArgs = 1,
        .returnsNothing = false,
    },
    {
        .owner = GlobalName::MATH,
        .name = "atanh",
        .maxArgs = 1,
        .returnsNothing = false,
    },
    {
        .owner = GlobalName::MATH,
        .name = "fround",
        .maxArgs = 1,
        .returnsNothing = false,
    },
    {
        .owner = GlobalName::MATH,
        .name = "clz32",
        .maxArgs = 1,
        .returnsNothing = false,
    },
    {
        .owner = GlobalName::NUMBER,
        .name = "parseInt",
        .maxArgs = 2,
        .returnsNothing = false,
    },
    {
        .owner = GlobalName::NUMBER,
        .name = "parseFloat",
        .maxArgs = 1,
        .returnsNothing = false,
    },
    {
        .owner = GlobalName::NUMBER,
        .name = "isInteger",
        .maxArgs = 1,
        .returnsNothing = false,
    },
    {
        .owner = GlobalName::NUMBER,
        .name = "isSafeInteger",
        .maxArgs = 1,
        .returnsNothing = false,
    },
    {
        .owner = GlobalName::STRING,
        .name = "fromCharCode",
        .maxArgs = FunctionValueTable::kAnyArgs,
        .returnsNothing = false,
    },
    {
        .owner = GlobalName::STRING,
        .name = "fromCodePoint",
        .maxArgs = FunctionValueTable::kAnyArgs,
        .returnsNothing = false,
    },
    {
        .owner = GlobalName::CONSOLE,
        .name = "log",
        .maxArgs = FunctionValueTable::kAnyArgs,
        .returnsNothing = true,
    },
    {
        .owner = GlobalName::CONSOLE,
        .name = "info",
        .maxArgs = FunctionValueTable::kAnyArgs,
        .returnsNothing = true,
    },
    {
        .owner = GlobalName::CONSOLE,
        .name = "debug",
        .maxArgs = FunctionValueTable::kAnyArgs,
        .returnsNothing = true,
    },
    {
        .owner = GlobalName::CONSOLE,
        .name = "error",
        .maxArgs = FunctionValueTable::kAnyArgs,
        .returnsNothing = true,
    },
    {
        .owner = GlobalName::CONSOLE,
        .name = "warn",
        .maxArgs = FunctionValueTable::kAnyArgs,
        .returnsNothing = true,
    },
};

}

const FunctionValueGlobalInfo *FunctionValueTable::findGlobal(GlobalName global) {
  for (const FunctionValueGlobalInfo &info : kGlobalRows) {
    if (info.global == global) {
      return &info;
    }
  }

  return nullptr;
}

const FunctionValueStaticInfo *FunctionValueTable::findStatic(GlobalName owner,
                                                              llvm::StringRef name) {
  for (const FunctionValueStaticInfo &info : kStaticRows) {
    if (info.owner == owner && info.name == name) {
      return &info;
    }
  }

  return nullptr;
}

}
