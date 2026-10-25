#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// Which variant of an overloaded method a call means (docs/overloads.md).
class OverloadResolver {
public:
  explicit OverloadResolver(Sema &sema);

  // Overloads (docs/overloads.md). The variant of `variants` a call with
  // `args` chooses, or null after an error. With several variants that take
  // that many arguments, the arguments are checked here and `checked` is
  // set: the caller only converts them.
  // A generic variant (docs/overloads.md, plan step 3) is chosen with the
  // type arguments that the arguments give it, which go to `bound`; at equal
  // fit, a variant without type parameters wins, as in C#.
  FuncSymbol *chooseVariant(llvm::ArrayRef<FunctionDecl *> variants, std::vector<Expr *> &args,
                            const std::vector<bool> &spread, llvm::SMLoc loc, bool &checked,
                            std::vector<const Type *> *bound = nullptr);
  // A call of `sym`, a generic method (with `method`) or static method that
  // chooseVariant chose, whose arguments are checked, with type arguments
  // `bound`.
  Expr *callGenericVariant(CallExpr *call, FuncSymbol *sym, llvm::ArrayRef<const Type *> bound,
                           llvm::SMLoc loc, bool method);
  // A method as a value: the variant whose type `expected` asks for.
  FuncSymbol *variantForType(llvm::ArrayRef<FunctionDecl *> variants, const Type *expected,
                             const MemberExpr *member);
  // A method of an interface with several signatures (sema_inherit.cpp):
  // called, the signature its arguments choose; as a value, the one the
  // place it goes asks for. A variant of a method for each signature, which
  // the choice goes through.
  Expr *checkOverloadCall(CallExpr *call, MemberExpr *member);
  Expr *checkOverloadMember(MemberExpr *member, const Type *expected);
  std::vector<FunctionDecl *> signatureVariants(const Type *iface, const Overload &overload);

private:
  Sema &sema_;
  // See signatureVariants.
  std::map<std::pair<const Type *, std::string>, std::vector<FunctionDecl *>> signatureVariants_;
};

} // namespace tz
