#pragma once

#include "compiler/program/warnings/warnings.h"
#include "compiler/union/union/union.h"

namespace tz {

enum class PassDoneKind {
  NO_WARNINGS,
  WITH_WARNINGS,
};

struct NoWarnings {
  static constexpr PassDoneKind kind = PassDoneKind::NO_WARNINGS;
};

struct WithWarnings {
  static constexpr PassDoneKind kind = PassDoneKind::WITH_WARNINGS;
  Warnings warnings;
};

using PassDone = Union<NoWarnings, WithWarnings>;

}
