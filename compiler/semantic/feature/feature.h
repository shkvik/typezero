#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/result/result/result.h"
#include "compiler/semantic/feature/feature_errors.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/SMLoc.h"
#include <optional>
#include <string>

namespace tz {

class SemanticState;
class Type;
class FuncSymbol;
struct SourceFile;
enum class NodeModule;

class Feature {
public:
  using ResultExportFound = Result<llvm::StringRef, FeatureExportFoundError>;

  virtual ~Feature() = default;

  virtual llvm::StringRef name() const = 0;

  virtual void prepare(SemanticState &, SourceFile &) const {}
  virtual void declare(SemanticState &) const {}

  virtual Expr *checkAnyCall(SemanticState &, CallExpr *, const Type *) const {
    return nullptr;
  }
  virtual Expr *checkStaticCall(SemanticState &, CallExpr *, MemberExpr *, const Type *) const {
    return nullptr;
  }
  virtual llvm::ArrayRef<llvm::StringLiteral> typeArgFreeGlobals() const { return {}; }
  virtual Expr *checkMethodCall(SemanticState &, CallExpr *, MemberExpr *, const Type *) const {
    return nullptr;
  }
  virtual Expr *methodReceiver(SemanticState &, Expr *) const {
    return nullptr;
  }
  virtual Expr *checkClassMethodCall(SemanticState &, CallExpr *, MemberExpr *,
                                     const Type *) const {
    return nullptr;
  }
  virtual Expr *checkValueMethodCall(SemanticState &, CallExpr *, MemberExpr *,
                                     const Type *) const {
    return nullptr;
  }
  virtual Expr *checkFunctionCall(SemanticState &, CallExpr *, NameExpr *, const Type *) const {
    return nullptr;
  }
  virtual Expr *checkLibraryCall(SemanticState &, CallExpr *, NameExpr *, FuncSymbol *) const {
    return nullptr;
  }

  virtual ResultExportFound exportFound(NodeModule, llvm::StringRef, llvm::StringRef) const {
    return err(FeatureExportFoundError{
        .code = FeatureExportFoundErrorCode::NOT_MINE,
        .message = "",
    });
  }
  virtual Expr *checkModuleCall(SemanticState &, CallExpr *, NodeModule, llvm::StringRef,
                                llvm::SMLoc, const Type *) const {
    return nullptr;
  }
  virtual Expr *checkModuleValue(SemanticState &, NodeModule, llvm::StringRef, Expr *) const {
    return nullptr;
  }
  virtual Expr *checkModuleMember(SemanticState &, MemberExpr *) const {
    return nullptr;
  }
};

}
