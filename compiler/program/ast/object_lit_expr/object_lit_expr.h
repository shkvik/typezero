#pragma once

#include "compiler/program/ast/expr/expr.h"

namespace tz {

struct ClassDecl;
class IndexExpr;

// `{ x: 1, y, ...rest, [key]: v }`: an object with these fields, in this
// order; `y` alone is `y: y`. A spread copies the fields of another object,
// and a later field replaces an earlier one of the same name.
class ObjectLitExpr : public Expr {
public:
  struct Property {
    llvm::StringRef name; // set by Sema for a key in brackets; empty for a spread
    llvm::SMLoc loc;
    Expr *value; // the object spread, for a spread
    bool spread = false;
    // `[key]: value`: a key that must be a constant (see Sema::constantKey).
    Expr *key = nullptr;
    // `{ x = 1 }`, which only a destructuring pattern may contain: value is
    // the default.
    bool defaulted = false;
    // A spread that leaves out some fields: the rest of an object pattern,
    // `{ a, [k]: b, ...rest }`. The fields named, and the reads by a key in
    // brackets, whose fields Sema finds (IndexExpr::field).
    std::vector<llvm::StringRef> omit;
    std::vector<const IndexExpr *> omitKeys;
    // Set by Sema in a literal with spreads: a later property replaces this
    // value, which is only evaluated; or of a spread, the fields that a later
    // property replaces, which are not copied.
    bool replaced = false;
    std::vector<llvm::StringRef> replacedFields;
    // `f() { ... }`: a method, whose value is the FunctionExpr of its body.
    bool method = false;
  };

  explicit ObjectLitExpr(llvm::SMLoc loc) : Expr(Kind::ObjectLit, loc) {}
  static bool classof(const Expr *e) { return e->kind() == Kind::ObjectLit; }

  std::vector<Property> props;
  // Set by Sema for a literal with methods: the class of its own that the
  // object is an instance of (see ClassDecl::literal).
  ClassDecl *cls = nullptr;
  bool hasSpread() const {
    return llvm::any_of(props, [](const Property &p) { return p.spread; });
  }
};

} // namespace tz
