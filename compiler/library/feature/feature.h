#pragma once

#include "compiler/program/ast/ast.h"
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
class Checker;
class Type;
struct FuncSymbol;
struct SourceFile;
enum class NodeModule;

class Feature {
public:
  virtual ~Feature() = default;

  virtual llvm::StringRef name() const = 0;

  virtual void prepare(Checker &, SourceFile &) const {}
  virtual void declare(Checker &) const {}

  virtual Expr *checkAnyCall(Checker &, CallExpr *, const Type *) const { return nullptr; }
  virtual Expr *checkStaticCall(Checker &, CallExpr *, MemberExpr *, const Type *) const {
    return nullptr;
  }
  virtual llvm::ArrayRef<llvm::StringLiteral> typeArgFreeGlobals() const { return {}; }
  virtual Expr *checkMethodCall(Checker &, CallExpr *, MemberExpr *, const Type *) const {
    return nullptr;
  }
  virtual Expr *methodReceiver(Checker &, Expr *) const { return nullptr; }
  virtual Expr *checkClassMethodCall(Checker &, CallExpr *, MemberExpr *, const Type *) const {
    return nullptr;
  }
  virtual Expr *checkValueMethodCall(Checker &, CallExpr *, MemberExpr *, const Type *) const {
    return nullptr;
  }
  virtual Expr *checkFunctionCall(Checker &, CallExpr *, NameExpr *, const Type *) const {
    return nullptr;
  }
  virtual Expr *checkLibraryCall(Checker &, CallExpr *, NameExpr *, FuncSymbol *) const {
    return nullptr;
  }

  virtual std::optional<std::string> exportProblem(NodeModule, llvm::StringRef,
                                                   llvm::StringRef) const {
    return std::nullopt;
  }
  virtual Expr *checkModuleCall(Checker &, CallExpr *, NodeModule, llvm::StringRef, llvm::SMLoc,
                                const Type *) const {
    return nullptr;
  }
  virtual Expr *checkModuleValue(Checker &, NodeModule, llvm::StringRef, Expr *) const {
    return nullptr;
  }
  virtual Expr *checkModuleMember(Checker &, MemberExpr *) const { return nullptr; }

  virtual bool lowers(Builtin) const { return false; }
  virtual llvm::Value *lowerCall(Lowering &, CallExpr *) const { return nullptr; }
};

}
