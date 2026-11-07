#pragma once

#include "compiler/library/feature/feature.h"

namespace tz {

class CryptoFeature : public Feature {
public:
  llvm::StringRef name() const override { return "crypto"; }
  void declare(CheckState &state) const override;

  Expr *checkStaticCall(CheckState &state, CallExpr *call, MemberExpr *member,
                        const Type *expected) const override;
  Expr *checkMethodCall(CheckState &state, CallExpr *call, MemberExpr *member,
                        const Type *expected) const override;
  Expr *checkClassMethodCall(CheckState &state, CallExpr *call, MemberExpr *member,
                             const Type *expected) const override;
  ResultExportFound exportFound(NodeModule module, llvm::StringRef exportName,
                                llvm::StringRef path) const override;
  Expr *checkModuleCall(CheckState &state, CallExpr *call, NodeModule module, llvm::StringRef fn,
                        llvm::SMLoc loc, const Type *expected) const override;
  Expr *checkModuleValue(CheckState &state, NodeModule module, llvm::StringRef exportName,
                         Expr *expr) const override;
  Expr *checkModuleMember(CheckState &state, MemberExpr *member) const override;
  bool lowers(Builtin builtin) const override;
  llvm::Value *lowerCall(Lowering &lowering, CallExpr *call) const override;
};

}
