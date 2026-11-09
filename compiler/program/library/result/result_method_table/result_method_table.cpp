#include "compiler/program/library/result/result_method_table/result_method_table.h"
#include "llvm/Support/ErrorHandling.h"

namespace tz {

namespace {

const ResultMethodInfo kResultMethods[] = {
    {
        .name = "isOk",
        .builtin = Builtin::ResultIsOk,
        .form = ResultTestMethod{},
    },
    {
        .name = "isErr",
        .builtin = Builtin::ResultIsErr,
        .form = ResultTestMethod{},
    },
    {
        .name = "map",
        .builtin = Builtin::ResultMap,
        .form = ResultCallbackMethod{.callbacks = 1},
    },
    {
        .name = "mapErr",
        .builtin = Builtin::ResultMapErr,
        .form = ResultCallbackMethod{.callbacks = 1},
    },
    {
        .name = "andThen",
        .builtin = Builtin::ResultAndThen,
        .form = ResultCallbackMethod{.callbacks = 1},
    },
    {
        .name = "orElse",
        .builtin = Builtin::ResultOrElse,
        .form = ResultCallbackMethod{.callbacks = 1},
    },
    {
        .name = "match",
        .builtin = Builtin::ResultMatch,
        .form = ResultCallbackMethod{.callbacks = 2},
    },
    {
        .name = "unwrapOr",
        .builtin = Builtin::ResultUnwrapOr,
        .form = ResultDefaultMethod{},
    },
    {
        .name = "andTee",
        .builtin = Builtin::ResultAndTee,
        .form = ResultCallbackMethod{.callbacks = 1},
    },
    {
        .name = "orTee",
        .builtin = Builtin::ResultOrTee,
        .form = ResultCallbackMethod{.callbacks = 1},
    },
    {
        .name = "andThrough",
        .builtin = Builtin::ResultAndThrough,
        .form = ResultCallbackMethod{.callbacks = 1},
    },
};

}

const ResultMethodInfo *ResultMethodTable::find(llvm::StringRef name) {
  for (const ResultMethodInfo &method : kResultMethods) {
    if (method.name == name) {
      return &method;
    }
  }

  return nullptr;
}

int ResultMethodTable::countArgs(const ResultMethodInfo &method) {
  switch (method.form.kind()) {
    case ResultMethodKind::TEST:
      return 0;
    case ResultMethodKind::CALLBACK:
      return method.form.as<ResultCallbackMethod>()->callbacks;
    case ResultMethodKind::DEFAULT_VALUE:
      return 1;
  }
  llvm_unreachable("unknown Result method kind");
}

int ResultMethodTable::countCallbacks(const ResultMethodInfo &method) {
  const ResultCallbackMethod *callback = method.form.as<ResultCallbackMethod>();
  if (callback == nullptr) {
    return 0;
  }

  return callback->callbacks;
}

llvm::ArrayRef<ResultMethodInfo> ResultMethodTable::getMethods() {
  return kResultMethods;
}

}
