#pragma once

#include "compiler/program/ast/expr/expr.h"
#include "compiler/union/union/union.h"

namespace tz {

struct EnumDecl;

enum class IndexKind {
  ELEMENT_INDEX,
  ENUM_NAME_INDEX,
  KEYED_FIELD_INDEX,
  NAMED_FIELD_INDEX,
};

struct ElementIndex {
  static constexpr IndexKind kind = IndexKind::ELEMENT_INDEX;
  bool mayBeOutOfRange = false;
  bool mayAppend = false;
};

struct EnumNameIndex {
  static constexpr IndexKind kind = IndexKind::ENUM_NAME_INDEX;
  EnumDecl *enumDecl = nullptr;
};

struct KeyedFieldIndex {
  static constexpr IndexKind kind = IndexKind::KEYED_FIELD_INDEX;
};

struct NamedFieldIndex {
  static constexpr IndexKind kind = IndexKind::NAMED_FIELD_INDEX;
  llvm::StringRef field;
};

using IndexForm = Union<ElementIndex, EnumNameIndex, KeyedFieldIndex, NamedFieldIndex>;

class IndexExpr : public Expr {
public:
  IndexExpr(llvm::SMLoc loc, Expr *object, Expr *index)
      : Expr(Kind::Index, loc), object(object), index(index) {}
  static bool classof(const Expr *e) { return e->kind() == Kind::Index; }

  Expr *object;
  Expr *index;
  bool optional = false;
  IndexForm form = ElementIndex{};

  bool mayBeOutOfRange() const;
  bool mayAppend() const;
  EnumDecl *enumDecl() const;
  bool keyed() const;
  llvm::StringRef field() const;
};

}
