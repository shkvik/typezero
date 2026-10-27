#pragma once

#include "compiler/program/ast/access/access.h"
#include "compiler/program/ast/block_stmt/block_stmt.h"
#include "compiler/program/ast/builtin/builtin.h"
#include "compiler/program/ast/decorator/decorator.h"
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
  FunctionDecl(llvm::SMLoc loc, llvm::StringRef name) : Stmt(Kind::Function, loc), name(name) {}
  static bool classof(const Stmt *s) { return s->kind() == Kind::Function; }

  llvm::StringRef name;
  std::vector<TypeParamDecl> typeParams;
  std::vector<const Type *> typeArgs;
  bool isInstance = false;
  std::vector<Param> params;
  std::optional<TypeRef> returnType;
  BlockStmt *body = nullptr;
  llvm::StringRef source;
  bool isExpression = false;
  bool isArrow = false;
  bool isGenerator = false;
  bool isAsync = false;
  bool awaitsAhead = false;
  bool lifted = false;
  llvm::StringRef ownName;
  bool concise = false;
  FuncSymbol *sym = nullptr;
  bool checked = false;

  FunctionDecl *parent = nullptr;
  std::vector<VarSymbol *> captures;
  NestedGroup *group = nullptr;
  std::vector<FunctionDecl *> siblings;
  VarSymbol *var = nullptr;
  unsigned creationSeq = 0;
  bool nonEscaping = false;
  bool isClosure() const { return parent || isExpression || group; }

  ClassDecl *cls = nullptr;
  bool isStatic = false;
  bool isConstructor = false;
  Access access = Access::Public;
  VarSymbol *thisVar = nullptr;
  unsigned variant = 0;
  bool takesThis() const { return cls && !isStatic; }
  bool isAbstract = false;
  bool isOverride = false;
  FunctionDecl *overrides = nullptr;
  bool overridden = false;
  int slot = -1;
  bool openToLate = false;
  std::vector<Decorator> decorators;
  VarSymbol *impl = nullptr;
  FunctionDecl *generic = nullptr;
  llvm::StringRef listName;
  size_t listStart = 0;
  const Type *listType = nullptr;
  bool external = false;
};

}
