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
#include "compiler/union/union/union.h"
#include "llvm/ADT/ArrayRef.h"

namespace tz {

class Type;
struct ClassDecl;

enum class ClassStage {
  UNRESOLVED,
  RESOLVING_BASE,
  RESOLVED,
};

enum class ClassOrigin {
  PROGRAM,
  LIBRARY,
  BUILTIN,
};

enum class ClassSyntaxKind {
  STATEMENT_CLASS,
  EXPRESSION_CLASS,
  LITERAL_CLASS,
};

struct StatementClass {
  static constexpr ClassSyntaxKind kind = ClassSyntaxKind::STATEMENT_CLASS;
};

struct ExpressionClass {
  static constexpr ClassSyntaxKind kind = ClassSyntaxKind::EXPRESSION_CLASS;
};

struct LiteralClass {
  static constexpr ClassSyntaxKind kind = ClassSyntaxKind::LITERAL_CLASS;
  ObjectLitExpr *literal = nullptr;
};

using ClassSyntax = Union<StatementClass, ExpressionClass, LiteralClass>;

enum class ClassGenericsKind {
  NOT_GENERIC,
  GENERIC_TEMPLATE,
  GENERIC_INSTANCE,
};

struct NotGeneric {
  static constexpr ClassGenericsKind kind = ClassGenericsKind::NOT_GENERIC;
};

struct GenericTemplate {
  static constexpr ClassGenericsKind kind = ClassGenericsKind::GENERIC_TEMPLATE;
  std::vector<ClassDecl *> instances;
};

struct GenericInstance {
  static constexpr ClassGenericsKind kind = ClassGenericsKind::GENERIC_INSTANCE;
  ClassDecl *generic = nullptr;
};

using ClassGenerics = Union<NotGeneric, GenericTemplate, GenericInstance>;

struct ClassDecl : Stmt {
  ClassDecl(llvm::SMLoc loc, llvm::StringRef name)
      : Stmt(Kind::Class, loc),
        name(name) {}
  static bool classof(const Stmt *s) {
    return s->kind() == Kind::Class;
  }

  const ClassField *field(llvm::StringRef member) const {
    for (const ClassField &f : fields) {
      if (f.name == member && !f.isStatic()) {
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
  std::vector<ClassField> fields;
  std::vector<FunctionDecl *> methods;
  std::vector<FunctionDecl *> constructors;
  const Type *type = nullptr;
  FunctionDecl *init = nullptr;
  unsigned id = 0;
  ClassStage stage = ClassStage::UNRESOLVED;
  bool declared = false;
  NestedGroup *group = nullptr;
  ClassOrigin origin = ClassOrigin::PROGRAM;
  ClassSyntax syntax = StatementClass{};
  std::vector<Decorator> decorators;
  std::vector<Stmt *> decorations;
  bool usedAsValue = false;
  const FunctionDecl *outer = nullptr;
  std::vector<llvm::StringRef> locals;
  ClassDecl *replacement = nullptr;
  bool reflectNew = false;
  bool reflectMethods = false;
  bool reflectFill = false;

  std::vector<TypeParamDecl> typeParams;
  llvm::StringRef source;
  std::vector<const Type *> typeArgs;
  ClassGenerics generics = NotGeneric{};
  bool checkOnly = false;
  std::vector<FunctionDecl *> methodInstances;
  const ClassDecl *pristine = nullptr;
  bool isGeneric() const;
  bool isEmitted() const;
  const ClassDecl *staticOwner() const;
  ClassDecl *staticOwner();
  ClassDecl *generic() const;
  llvm::ArrayRef<ClassDecl *> instances() const;
  bool library() const;
  bool builtin() const;
  bool expression() const;
  ObjectLitExpr *literal() const;

  bool hasMethodFields() const;


  std::vector<FunctionDecl *> findOwnMethods() const;
  std::vector<FunctionDecl *> findConstructors() const;
  std::vector<FunctionDecl *> collectMemberFunctions() const;
  std::vector<FunctionDecl *> findMethodsNamed(llvm::StringRef name) const;
  std::vector<FunctionDecl *> findStaticMethodsNamed(llvm::StringRef name) const;


  const ClassDecl *findFinalClass() const;
  const ClassDecl *findFieldOwner(llvm::StringRef name) const;
  const ClassDecl *findInheritedFieldOwner(llvm::StringRef name) const;


  FunctionDecl *findSoleImplementation(FunctionDecl *variant) const;


  const ClassField *findField(llvm::StringRef name) const;
  const ClassField *findInheritedField(llvm::StringRef name) const;

private:
  void addNotHidden(std::vector<FunctionDecl *> &found,
                    const std::vector<FunctionDecl *> &own) const;
  void addNotOverridden(std::vector<FunctionDecl *> &found,
                        const std::vector<FunctionDecl *> &own) const;


  bool hasOwnMethodField() const;
  bool isOverriding(const FunctionDecl *method, const FunctionDecl *base) const;
  bool hasSameSignature(const FuncSymbol *first, const FuncSymbol *second) const;
  bool isHiddenByAny(const std::vector<FunctionDecl *> &found, const FunctionDecl *method) const;
  bool isOverriddenByAny(const std::vector<FunctionDecl *> &found,
                         const FunctionDecl *method) const;


  std::vector<FunctionDecl *> ownMethodsNamed(llvm::StringRef name) const;
  std::vector<FunctionDecl *> ownStaticMethodsNamed(llvm::StringRef name) const;
};

}
