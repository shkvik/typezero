#pragma once

#include "compiler/union/union/union.h"

namespace tz {

struct FunctionDecl;
struct VarSymbol;

enum class FieldKind {
  INSTANCE_FIELD,
  ARROW_METHOD_FIELD,
  STATIC_FIELD,
  PARAMETER_FIELD,
};

struct InstanceField {
  static constexpr FieldKind kind = FieldKind::INSTANCE_FIELD;
};

struct ArrowMethodField {
  static constexpr FieldKind kind = FieldKind::ARROW_METHOD_FIELD;
  FunctionDecl *method = nullptr;
};

struct StaticField {
  static constexpr FieldKind kind = FieldKind::STATIC_FIELD;
  VarSymbol *var = nullptr;
};

struct ParameterField {
  static constexpr FieldKind kind = FieldKind::PARAMETER_FIELD;
};

using FieldForm = Union<InstanceField, ArrowMethodField, StaticField, ParameterField>;

enum class FieldPresence {
  REQUIRED,
  OPTIONAL,
  DEFINITE,
};

}
