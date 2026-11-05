#pragma once

#include "compiler/union/union/union.h"

namespace tz {

struct FunctionDecl;

enum class VarPlaceKind {
  GLOBAL_VAR,
  TOP_LEVEL_LOCAL_VAR,
  FUNCTION_LOCAL_VAR,
};

struct LocalVarState {
  unsigned declSeq = 0;
  bool captured = false;
  bool capturedEarly = false;
  bool unshared = false;
};

struct GlobalVar {
  static constexpr VarPlaceKind kind = VarPlaceKind::GLOBAL_VAR;
  bool usedInFunction = false;
};

struct TopLevelLocalVar {
  static constexpr VarPlaceKind kind = VarPlaceKind::TOP_LEVEL_LOCAL_VAR;
  LocalVarState state;
};

struct FunctionLocalVar {
  static constexpr VarPlaceKind kind = VarPlaceKind::FUNCTION_LOCAL_VAR;
  FunctionDecl *owner = nullptr;
  LocalVarState state;
};

using VarPlace = Union<GlobalVar, TopLevelLocalVar, FunctionLocalVar>;

}
