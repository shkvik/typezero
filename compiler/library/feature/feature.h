#pragma once

#include "compiler/program/ast/ast.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

namespace llvm {
class Value;
} // namespace llvm

namespace tz {

class CodeGen;
class Sema;
class Type;
struct FuncSymbol;
struct SourceFile;

// A topic of the library: what a program takes from a global name or a module (Math,
// node:crypto). The passes hand a topic, through these hooks, the code that is its own; a hook
// a topic does not have does nothing. Each hook is asked of every topic in the order of
// FeatureRegistry.
class Feature {
public:
  virtual ~Feature() = default;

  // The topic's name, as its folders are named: "math", "crypto".
  virtual llvm::StringRef name() const = 0;

  // declare: what the topic needs before the classes of the language are declared.
  virtual void prepare(Sema &sema, SourceFile &file) const {}
  // declare: the classes of the topic (SystemError, ExecSyncError).
  virtual void declare(Sema &sema) const {}

  // check: any call, before the language looks at it; null when it is not the topic's.
  virtual Expr *checkAnyCall(Sema &sema, CallExpr *call, const Type *expected) const {
    return nullptr;
  }
  // check: `G.f(...)` of a global of the topic (Math.max, JSON.parse); null when not its own.
  virtual Expr *checkStaticCall(Sema &sema, CallExpr *call, MemberExpr *member,
                                const Type *expected) const {
    return nullptr;
  }
  // The globals of the topic whose functions take no type arguments.
  virtual llvm::ArrayRef<llvm::StringLiteral> typeArgFreeGlobals() const { return {}; }
  // check: `x.f(...)` of a value of a type of the topic; null when not its own.
  virtual Expr *checkMethodCall(Sema &sema, CallExpr *call, MemberExpr *member,
                                const Type *expected) const {
    return nullptr;
  }
  // check: the value whose methods the methods of `object` are (the captures of a match);
  // null when the topic has none for it.
  virtual Expr *methodReceiver(Sema &sema, Expr *object) const { return nullptr; }
  // check: `x.f(...)` of an object of a class of the topic's modules written in tz.
  virtual Expr *checkClassMethodCall(Sema &sema, CallExpr *call, MemberExpr *member,
                                     const Type *expected) const {
    return nullptr;
  }
  // check: a method the topic gives the values of the language (s.localeCompare(t)).
  virtual Expr *checkValueMethodCall(Sema &sema, CallExpr *call, MemberExpr *member,
                                     const Type *expected) const {
    return nullptr;
  }
  // check: `f(...)` of a function of the topic (ok, embedJson); null when not its own.
  virtual Expr *checkFunctionCall(Sema &sema, CallExpr *call, NameExpr *name,
                                  const Type *expected) const {
    return nullptr;
  }
  // check: a call of `func`, a function of one of the topic's modules written in tz.
  virtual Expr *checkLibraryCall(Sema &sema, CallExpr *call, NameExpr *name,
                                 FuncSymbol *func) const {
    return nullptr;
  }

  // lower: whether the calls of `builtin` are the topic's, and their code.
  virtual bool lowers(Builtin builtin) const { return false; }
  virtual llvm::Value *lowerCall(CodeGen &codegen, CallExpr *call) const { return nullptr; }
};

} // namespace tz
