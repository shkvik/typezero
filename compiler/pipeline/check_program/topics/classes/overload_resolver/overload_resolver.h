#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/topics/classes/overload_resolver/overload_resolver_errors.h"
#include "compiler/result/result/result.h"

namespace tz {

class OverloadResolver {
public:
  using ResultVariantChosen = Result<FuncSymbol *, OverloadResolverChooseVariantError>;
  using ResultVariantOfType = Result<FuncSymbol *, OverloadResolverVariantForTypeError>;

  explicit OverloadResolver(CheckState &owner);

  ResultVariantChosen chooseVariant(llvm::ArrayRef<FunctionDecl *> variants,
                                    std::vector<Expr *> &args, const std::vector<bool> &spread,
                                    bool &checked, std::vector<const Type *> *bound = nullptr);
  Expr *callGenericVariant(CallExpr *call, FuncSymbol *sym, llvm::ArrayRef<const Type *> bound,
                           llvm::SMLoc loc, bool method);
  ResultVariantOfType variantForType(llvm::ArrayRef<FunctionDecl *> variants, const Type *expected,
                                     const MemberExpr *member);
  Expr *checkOverloadCall(CallExpr *call, MemberExpr *member);
  Expr *checkOverloadMember(MemberExpr *member, const Type *expected);
  std::vector<FunctionDecl *> signatureVariants(const Type *iface, const Overload &overload);

private:
  static int conversionRank(const Type *from, const Type *to);
  static const Type *parameterAt(const FuncSymbol *sym, size_t i);
  static bool hasFloatLiteral(const Expr *expr);

  CheckState &state;
};

}
