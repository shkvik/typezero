#pragma once

#include "compiler/program/ast/class_field/class_field.h"
#include "compiler/program/ast/decorator/decorator.h"
#include "compiler/program/ast/expr/expr.h"
#include "compiler/program/ast/function_decl/function_decl.h"
#include "compiler/program/ast/nested_group/nested_group.h"
#include "compiler/program/ast/object_lit_expr/object_lit_expr.h"
#include "compiler/program/ast/stmt/stmt.h"
#include "compiler/program/ast/type_param_decl/type_param_decl.h"
#include "compiler/program/ast/type_ref/type_ref.h"

namespace tz {

class Type;

struct ClassDecl : Stmt {
  ClassDecl(llvm::SMLoc loc, llvm::StringRef name) : Stmt(Kind::Class, loc), name(name) {}
  static bool classof(const Stmt *s) { return s->kind() == Kind::Class; }

  const ClassField *field(llvm::StringRef member) const {
    for (const ClassField &f : fields) {
      if (f.name == member && !f.isStatic) {
        return &f;
      }
    }

    return nullptr;
  }
  bool derivesFrom(const ClassDecl *ancestor) const {
    for (const ClassDecl *c = this; c; c = c->parent) {
      if (c == ancestor) {
        return true;
      }
    }

    return false;
  }

  llvm::StringRef name;
  bool isAbstract = false;
  std::optional<TypeRef> base;
  Expr *baseExpr = nullptr;
  std::vector<TypeRef> interfaces;
  ClassDecl *parent = nullptr;
  std::vector<ClassDecl *> children;
  std::vector<const Type *> implemented;
  unsigned lastId = 0;
  std::vector<FunctionDecl *> vtable;
  bool resolvingBase = false;
  std::vector<ClassField> fields;
  std::vector<FunctionDecl *> methods;
  std::vector<FunctionDecl *> constructors;
  const Type *type = nullptr;
  FunctionDecl *init = nullptr;
  unsigned id = 0;
  bool resolved = false;
  bool declared = false;
  ObjectLitExpr *literal = nullptr;
  NestedGroup *group = nullptr;
  bool builtin = false;
  bool library = false;
  std::vector<Decorator> decorators;
  std::vector<Stmt *> decorations;
  bool usedAsValue = false;
  bool expression = false;
  const FunctionDecl *outer = nullptr;
  std::vector<llvm::StringRef> locals;
  ClassDecl *replacement = nullptr;
  bool replaced = false;
  bool reflectNew = false;
  bool reflectMethods = false;
  bool reflectJson = false;
  std::vector<std::string> jsonParams, jsonFields;

  std::vector<TypeParamDecl> typeParams;
  llvm::StringRef source;
  std::vector<const Type *> typeArgs;
  ClassDecl *generic = nullptr;
  std::vector<ClassDecl *> instances;
  bool checkOnly = false;
  std::vector<FunctionDecl *> methodInstances;
  bool isGeneric() const { return !typeParams.empty() && !generic; }
  bool isEmitted() const { return !isGeneric() && !checkOnly; }
  const ClassDecl *staticOwner() const { return generic ? generic : this; }
  ClassDecl *staticOwner() { return generic ? generic : this; }
};

inline bool derivesFromError(const ClassDecl *cls) {
  for (; cls; cls = cls->parent) {
    if (cls->builtin && cls->name == "Error") {
      return true;
    }
  }

  return false;
}

inline constexpr llvm::StringLiteral kErrorKinds[] = {"Error", "TypeError", "RangeError",
                                                      "SyntaxError", "URIError"};

inline int errorKindIndex(llvm::StringRef name) {
  for (size_t i = 0; i < std::size(kErrorKinds); ++i) {
    if (kErrorKinds[i] == name) {
      return int(i);
    }
  }

  return -1;
}

}
