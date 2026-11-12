#include "compiler/program/library/function/function_method_table/function_method_table.h"

namespace tz {

namespace {

const FunctionMethodInfo kFunctionMethods[] = {
    {
        .name = "call",
        .method = FunctionMethod::CALL,
    },
    {
        .name = "apply",
        .method = FunctionMethod::APPLY,
    },
    {
        .name = "bind",
        .method = FunctionMethod::BIND,
    },
};

}

std::optional<FunctionMethod> FunctionMethodTable::find(llvm::StringRef name) {
  for (const FunctionMethodInfo &info : kFunctionMethods) {
    if (info.name == name) {
      return info.method;
    }
  }

  return std::nullopt;
}

llvm::StringRef FunctionMethodTable::getName(FunctionMethod method) {
  for (const FunctionMethodInfo &info : kFunctionMethods) {
    if (info.method == method) {
      return info.name;
    }
  }

  return "";
}

}
