#pragma once

#include "compiler/program/ast/builtin/builtin.h"
#include "compiler/union/union/union.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

namespace tz {

enum class ResultMethodKind {
  TEST,
  CALLBACK,
  DEFAULT_VALUE,
};

struct ResultTestMethod {
  static constexpr ResultMethodKind kind = ResultMethodKind::TEST;
};

struct ResultCallbackMethod {
  static constexpr ResultMethodKind kind = ResultMethodKind::CALLBACK;
  int callbacks;
};

struct ResultDefaultMethod {
  static constexpr ResultMethodKind kind = ResultMethodKind::DEFAULT_VALUE;
};

using ResultMethodForm = Union<ResultTestMethod, ResultCallbackMethod, ResultDefaultMethod>;

struct ResultMethodInfo {
  llvm::StringLiteral name;
  Builtin builtin;
  ResultMethodForm form;
};

class ResultMethodTable {
public:
  static const ResultMethodInfo *find(llvm::StringRef name);
  static int countArgs(const ResultMethodInfo &method);
  static int countCallbacks(const ResultMethodInfo &method);
  static llvm::ArrayRef<ResultMethodInfo> getMethods();
};

}
