#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// The utility types, getters of types and indices of generic types.
class UtilityTypes {
public:
  explicit UtilityTypes(Sema &sema);

  // What reading the getter `name` of the class gives; null without one.
  const Type *getterType(ClassDecl *cls, llvm::StringRef name);
  // `typeof x` of a name a file imports, before its imports are bound.
  const Type *importedValueType(Module *from, llvm::StringRef name);
  // Exclude, Extract, NonNullable, ReturnType and Parameters; null for any
  // other name.
  const Type *resolveUtilityType(const TypeRef &ref);
  // Whether a string literal given for type parameter `param` gives its
  // own type, as for `K extends keyof T` (`get(user, "name")`: K = "name").
  bool wantsLiteral(const Type *param) const;
  // `o[k]` in generic code, of an object of a type parameter or with a key
  // of one (`k: K` with `K extends keyof T`): of type `T[K]`. Null, with
  // nothing checked, for any other.
  Expr *checkGenericIndex(IndexExpr *index);

private:
  Sema &sema_;
};

} // namespace tz
