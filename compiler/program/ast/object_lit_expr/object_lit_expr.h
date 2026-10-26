#pragma once

#include "compiler/program/ast/expr/expr.h"

namespace tz {

struct ClassDecl;
class IndexExpr;

class ObjectLitExpr : public Expr {
public:
  struct Property {
    llvm::StringRef name;
    llvm::SMLoc loc;
    Expr *value;
    bool spread = false;
    Expr *key = nullptr;
    bool defaulted = false;
    std::vector<llvm::StringRef> omit;
    std::vector<const IndexExpr *> omitKeys;
    bool replaced = false;
    std::vector<llvm::StringRef> replacedFields;
    bool method = false;
  };

  explicit ObjectLitExpr(llvm::SMLoc loc) : Expr(Kind::ObjectLit, loc) {}
  static bool classof(const Expr *e) { return e->kind() == Kind::ObjectLit; }

  std::vector<Property> props;
  ClassDecl *cls = nullptr;
  bool hasSpread() const {
    return llvm::any_of(props, [](const Property &p) { return p.spread; });
  }
};

}
