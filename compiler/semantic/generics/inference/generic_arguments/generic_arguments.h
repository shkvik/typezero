#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/union/union/union.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/SMLoc.h"
#include <vector>

namespace tz {

enum class TypeArgUse {
  CALL,
  VALUE,
};

struct GenericCallBinding {
  CallExpr *call = nullptr;
  FuncSymbol *generic = nullptr;
  llvm::SMLoc loc;
  llvm::ArrayRef<const Type *> typeParams;
  const Type *result = nullptr;
  const Type *listRest = nullptr;
  size_t fixed = 0;
  unsigned errorsBefore = 0;
  std::vector<const Type *> &bound;
  std::vector<bool> checked;
  std::vector<size_t> literals;
  std::vector<size_t> contextual;
  bool inferred = true;
};

enum class GenericArgumentKind {
  LIST_REST,
  PLAIN,
  LITERAL_TEXT,
  CONTEXTUAL,
  LITERAL_SHAPE,
  CONTEXTUAL_LITERAL,
  INFERRED,
};

struct ListRestArgument {
  static constexpr GenericArgumentKind kind = GenericArgumentKind::LIST_REST;
};

struct PlainArgument {
  static constexpr GenericArgumentKind kind = GenericArgumentKind::PLAIN;
};

struct LiteralTextArgument {
  static constexpr GenericArgumentKind kind = GenericArgumentKind::LITERAL_TEXT;
  const StringLitExpr *text = nullptr;
};

struct ContextualArgument {
  static constexpr GenericArgumentKind kind = GenericArgumentKind::CONTEXTUAL;
};

struct LiteralShapeArgument {
  static constexpr GenericArgumentKind kind = GenericArgumentKind::LITERAL_SHAPE;
  const Type *shape = nullptr;
};

struct ContextualLiteralArgument {
  static constexpr GenericArgumentKind kind = GenericArgumentKind::CONTEXTUAL_LITERAL;
};

struct InferredArgument {
  static constexpr GenericArgumentKind kind = GenericArgumentKind::INFERRED;
};

using GenericArgumentForm =
    Union<ListRestArgument, PlainArgument, LiteralTextArgument, ContextualArgument,
          LiteralShapeArgument, ContextualLiteralArgument, InferredArgument>;

}
