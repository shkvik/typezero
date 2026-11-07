#pragma once

#include "compiler/program/ast/expr/expr.h"
#include "compiler/program/ast/type_ref/type_ref.h"
#include "compiler/program/symbols/var_symbol/var_symbol.h"
#include "compiler/union/union/union.h"

namespace tz {

enum class BindingKind {
  PLAIN_BINDING,
  ARRAY_PATTERN,
  OBJECT_PATTERN,
};

struct PlainBinding {
  static constexpr BindingKind kind = BindingKind::PLAIN_BINDING;
};

struct ArrayPattern {
  static constexpr BindingKind kind = BindingKind::ARRAY_PATTERN;
  size_t length = 0;
  bool rest = false;
};

struct ObjectPattern {
  static constexpr BindingKind kind = BindingKind::OBJECT_PATTERN;
};

using BindingForm = Union<PlainBinding, ArrayPattern, ObjectPattern>;

struct VarDeclarator {
  llvm::StringRef name;
  llvm::SMLoc loc;
  std::optional<TypeRef> typeAnn;
  Expr *init = nullptr;
  VarSymbol *sym = nullptr;
  BindingForm form = PlainBinding{};
  bool alias = false;

  bool destructured() const;
  bool objectPattern() const;
  size_t patternLength() const;
  bool patternRest() const;
};

}
