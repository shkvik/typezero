#pragma once

#include "compiler/program/ast/access/access.h"
#include "compiler/program/ast/block_stmt/block_stmt.h"
#include "compiler/program/ast/builtin/builtin.h"
#include "compiler/program/ast/decorator/decorator.h"
#include "compiler/program/ast/function_decl/function_role.h"
#include "compiler/program/ast/function_decl/function_syntax.h"
#include "compiler/program/ast/param/param.h"
#include "compiler/program/ast/stmt/stmt.h"
#include "compiler/program/ast/type_param_decl/type_param_decl.h"
#include "compiler/program/ast/type_ref/type_ref.h"
#include "compiler/program/symbols/func_symbol/func_symbol.h"
#include "compiler/program/symbols/var_symbol/var_symbol.h"

namespace tz {

struct ClassDecl;
struct NestedGroup;
class Type;

struct FunctionDecl : Stmt {
  FunctionDecl(llvm::SMLoc loc, llvm::StringRef name)
      : Stmt(Kind::Function, loc),
        name(name) {}
  static bool classof(const Stmt *s) {
    return s->kind() == Kind::Function;
  }

  llvm::StringRef name;
  int id = -1;
  std::vector<TypeParamDecl> typeParams;
  std::vector<const Type *> typeArgs;
  bool isInstance = false;
  std::vector<Param> params;
  std::optional<TypeRef> returnType;
  BlockStmt *body = nullptr;
  llvm::StringRef source;
  FunctionSyntax syntax = RegularFunction{};
  FunctionPlace place = FunctionPlace::DECLARED;
  FunctionBodySource bodySource = FunctionBodySource::WRITTEN;
  bool isGenerator = false;
  bool isAsync = false;
  bool awaitsAhead = false;
  FuncSymbol *sym = nullptr;
  bool checked = false;

  FunctionDecl *parent = nullptr;
  std::vector<VarSymbol *> captures;
  NestedGroup *group = nullptr;
  std::vector<FunctionDecl *> siblings;
  VarSymbol *var = nullptr;
  unsigned creationSeq = 0;
  bool nonEscaping = false;
  const VarSymbol *receiverEnv = nullptr;
  bool isClosure() const {
    return parent || isExpression() || group;
  }

  bool isArrow() const;
  bool concise() const;
  llvm::StringRef ownName() const;
  bool isExpression() const;
  bool lifted() const;
  bool external() const;
  bool intrinsic() const;

  Union<PlainFunction, ClassMember> role = PlainFunction{};
  ClassMember *member();
  const ClassMember *member() const;
  Overriding *overriding();
  const Overriding *overriding() const;
  ClassDecl *cls() const;
  bool isStatic() const;
  bool isConstructor() const;
  bool isAbstract() const;
  Access access() const;
  VarSymbol *thisVar() const;
  unsigned variant() const;
  bool isOverride() const;
  FunctionDecl *overrides() const;
  bool overridden() const;
  int slot() const;
  bool openToLate() const;
  const std::vector<Decorator> &decorators() const;
  VarSymbol *impl() const;
  bool takesThis() const;
  bool hasThisParam() const;
  const FunctionDecl *thisSource() const;

  FunctionDecl *generic = nullptr;
  const FunctionDecl *pristine = nullptr;
  llvm::StringRef listName;
  size_t listStart = 0;
  const Type *listType = nullptr;
};

}
