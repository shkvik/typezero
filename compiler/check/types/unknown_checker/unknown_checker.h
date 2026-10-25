#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// unknown and object, and what narrows them.
class UnknownChecker {
public:
  explicit UnknownChecker(Sema &sema);

  // unknown and object (sema_unknown.cpp, docs/decorators.md section 4).
  // An object of the language (an instance, a literal, a date, a
  // dictionary, an iterator, a Promise, a class itself, a union of objects):
  // a reference with a layout.
  static bool isObjectReference(const Type *type);
  // An unknown, or an optional or an array of them.
  static bool holdsUnknown(const Type *type);
  // "; check what it holds first, e.g. ...": the end of a message about an
  // unknown or an object `expr` used as a `to`.
  std::string unknownHint(const Expr *expr, const Type *to);
  // Reports a use of `what` ("property 'x'"; empty for indexing) of an
  // unknown or an object, which needs a check first.
  void reportUnknownUse(const Expr *object, const std::string &what);
  // `x.name` and `x.m(...)` of an unknown, an object or a class as a value,
  // whose object is checked; checkUnknownMethod gives null for other
  // receivers.
  Expr *checkUnknownMember(MemberExpr *member);
  Expr *checkUnknownMethod(CallExpr *call, MemberExpr *member);
  // What `typeof x == "string"`, `x instanceof C` and `Array.isArray(x)` say
  // about an unknown or object variable x.
  void addUnknownFacts(const Expr *cond, Sema::Flow &whenTrue, Sema::Flow &whenFalse) const;

private:
  Sema &sema_;
};

} // namespace tz
