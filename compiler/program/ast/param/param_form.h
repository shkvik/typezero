#pragma once

#include "compiler/program/ast/access/access.h"
#include "compiler/union/union/union.h"

namespace tz {

class Expr;

enum class ParamKind {
  REQUIRED_PARAM,
  OPTIONAL_PARAM,
  DEFAULTED_PARAM,
  REST_PARAM,
};

struct RequiredParam {
  static constexpr ParamKind kind = ParamKind::REQUIRED_PARAM;
};

struct OptionalParam {
  static constexpr ParamKind kind = ParamKind::OPTIONAL_PARAM;
};

struct DefaultedParam {
  static constexpr ParamKind kind = ParamKind::DEFAULTED_PARAM;
  Expr *defaultValue = nullptr;
};

struct RestParam {
  static constexpr ParamKind kind = ParamKind::REST_PARAM;
};

using ParamForm = Union<RequiredParam, OptionalParam, DefaultedParam, RestParam>;

struct ParamProperty {
  bool readonly = false;
  Access access = Access::Public;
};

}
