#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

// Conditional types, infer, intersections and the utility types of functions.
class ConditionalTypes {
public:
  explicit ConditionalTypes(Checker &checker);

  // Conditional types, `infer`, template literal types and the intrinsic
  // string types (sema_conditional.cpp, docs/generics.md section 10).
  //
  // `T extends U ? A : B` of known types gives the branch at once, and
  // resolves only that one, so that a type alias can refer to itself in a
  // branch. Over type parameters it waits, as a computation (Type::typeOp)
  // whose operands are the types the names it uses stand for; a copy that
  // knows them resolves it again (evalConditional).
  const Type *resolveConditional(const TypeRef &ref);
  const Type *deferConditional(const TypeRef &ref);
  const Type *evalConditional(llvm::ArrayRef<const Type *> operands);
  // The branches of a conditional type that waits, with its `infer` types
  // unknown; generic code may give a value of either where the type is
  // expected (fitsConditional), and each copy checks the one it chooses.
  std::vector<const Type *> conditionalBranches(const Type *conditional);
  bool fitsConditional(const Type *from, const Type *to, unsigned depth = 0);
  // Binds `name` to `type` in the innermost scope of types, as a type
  // parameter is bound.
  void bindType(llvm::StringRef name, const Type *type, llvm::SMLoc loc);
  const Type *resolveIntersection(const TypeRef &ref);
  // A type argument of a type alias or a utility type, where `undefined` and
  // `void` alone are types too (`Exclude<T, undefined>`); where the result is
  // used says whether it may be such a type.
  const Type *resolveTypeArgument(const TypeRef &ref);
  // `typeof f` of a generic function or an overloaded method, for
  // ReturnType and Parameters: as TypeScript does, the last signature, with
  // each type parameter its constraint (or unknown); null for other types.
  const Type *signatureForUtility(const TypeRef &ref);

private:
  Checker &checker_;
  std::map<const TypeRef *, Checker::ConditionalInfo> conditionals_;
  llvm::DenseMap<const Type *, const Checker::ConditionalInfo *> conditionalTags_;
};

} // namespace tz
