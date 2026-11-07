#pragma once

#include "compiler/program/ast/expr/expr.h"
#include "compiler/union/union/union.h"
#include "llvm/ADT/ArrayRef.h"

namespace tz {

struct ClassDecl;
class IndexExpr;

enum class PropertyKind {
  VALUE_PROPERTY,
  METHOD_PROPERTY,
  DEFAULTED_PROPERTY,
  COMPUTED_PROPERTY,
  SPREAD_PROPERTY,
};

struct ValueProperty {
  static constexpr PropertyKind kind = PropertyKind::VALUE_PROPERTY;
};

struct MethodProperty {
  static constexpr PropertyKind kind = PropertyKind::METHOD_PROPERTY;
};

struct DefaultedProperty {
  static constexpr PropertyKind kind = PropertyKind::DEFAULTED_PROPERTY;
};

struct ComputedProperty {
  static constexpr PropertyKind kind = PropertyKind::COMPUTED_PROPERTY;
  Expr *key = nullptr;
  bool keyed = false;
};

struct SpreadProperty {
  static constexpr PropertyKind kind = PropertyKind::SPREAD_PROPERTY;
  std::vector<llvm::StringRef> omit;
  std::vector<const IndexExpr *> omitKeys;
  std::vector<llvm::StringRef> replacedFields;
};

using PropertyForm =
    Union<ValueProperty, MethodProperty, DefaultedProperty, ComputedProperty, SpreadProperty>;

class ObjectLitExpr : public Expr {
public:
  struct Property {
    llvm::StringRef name;
    llvm::SMLoc loc;
    Expr *value;
    PropertyForm form = ValueProperty{};
    bool replaced = false;

    bool spread() const;
    bool defaulted() const;
    bool method() const;
    bool keyed() const;
    Expr *key() const;
    llvm::ArrayRef<llvm::StringRef> omit() const;
    llvm::ArrayRef<const IndexExpr *> omitKeys() const;
    llvm::ArrayRef<llvm::StringRef> replacedFields() const;
  };

  explicit ObjectLitExpr(llvm::SMLoc loc) : Expr(Kind::ObjectLit, loc) {}
  static bool classof(const Expr *e) { return e->kind() == Kind::ObjectLit; }

  std::vector<Property> props;
  ClassDecl *cls = nullptr;
  bool hasSpread() const;
};

}
