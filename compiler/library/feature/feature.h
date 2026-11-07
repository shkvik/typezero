#pragma once

#include "compiler/library/feature/feature_errors.h"
#include "compiler/program/ast/ast.h"
#include "compiler/result/result/result.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/SMLoc.h"
#include <optional>
#include <string>

namespace llvm {
class Value;
}

namespace tz {

class Lowering;
class CheckState;
class Type;
struct FuncSymbol;
struct SourceFile;
enum class NodeModule;

class Feature {
public:
  using ResultExportFound = Result<llvm::StringRef, FeatureExportFoundError>;

  virtual ~Feature() = default;

  virtual llvm::StringRef name() const = 0;

  virtual void prepare(CheckState &, SourceFile &) const {}
  virtual void declare(CheckState &) const {}

  virtual Expr *checkAnyCall(CheckState &, CallExpr *, const Type *) const {
    return nullptr;
  }
  virtual Expr *checkStaticCall(CheckState &, CallExpr *, MemberExpr *, const Type *) const {
    return nullptr;
  }
  virtual llvm::ArrayRef<llvm::StringLiteral> typeArgFreeGlobals() const { return {}; }
  virtual Expr *checkMethodCall(CheckState &, CallExpr *, MemberExpr *, const Type *) const {
    return nullptr;
  }
  virtual Expr *methodReceiver(CheckState &, Expr *) const {
    return nullptr;
  }
  virtual Expr *checkClassMethodCall(CheckState &, CallExpr *, MemberExpr *, const Type *) const {
    return nullptr;
  }
  virtual Expr *checkValueMethodCall(CheckState &, CallExpr *, MemberExpr *, const Type *) const {
    return nullptr;
  }
  virtual Expr *checkFunctionCall(CheckState &, CallExpr *, NameExpr *, const Type *) const {
    return nullptr;
  }
  virtual Expr *checkLibraryCall(CheckState &, CallExpr *, NameExpr *, FuncSymbol *) const {
    return nullptr;
  }

  virtual ResultExportFound exportFound(NodeModule, llvm::StringRef, llvm::StringRef) const {
    return err(FeatureExportFoundError{
        .code = FeatureExportFoundErrorCode::NOT_MINE,
        .message = "",
    });
  }
  virtual Expr *checkModuleCall(CheckState &, CallExpr *, NodeModule, llvm::StringRef, llvm::SMLoc,
                                const Type *) const {
    return nullptr;
  }
  virtual Expr *checkModuleValue(CheckState &, NodeModule, llvm::StringRef, Expr *) const {
    return nullptr;
  }
  virtual Expr *checkModuleMember(CheckState &, MemberExpr *) const {
    return nullptr;
  }

  virtual bool lowers(Builtin) const { return false; }
  virtual llvm::Value *lowerCall(Lowering &, CallExpr *) const { return nullptr; }
};

}
