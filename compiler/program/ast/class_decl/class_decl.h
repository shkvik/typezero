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

// `class Name { members }` (docs/objects.md, section 6), only at the top level
// of a file; also the class Sema makes for an object literal with methods.
struct ClassDecl : Stmt {
  ClassDecl(llvm::SMLoc loc, llvm::StringRef name) : Stmt(Kind::Class, loc), name(name) {}
  static bool classof(const Stmt *s) { return s->kind() == Kind::Class; }

  // A field of the instances that this class declares.
  const ClassField *field(llvm::StringRef member) const {
    for (const ClassField &f : fields) {
      if (f.name == member && !f.isStatic) {
        return &f;
      }
    }

    return nullptr;
  }
  // Whether `ancestor` is this class or a class up its chain.
  bool derivesFrom(const ClassDecl *ancestor) const {
    for (const ClassDecl *c = this; c; c = c->parent) {
      if (c == ancestor) {
        return true;
      }
    }

    return false;
  }

  llvm::StringRef name; // empty for the class of a literal
  // `abstract class Name extends Base implements A, B`.
  bool isAbstract = false;
  std::optional<TypeRef> base;
  // `extends Timestamped(User)`: a base that is a value, an expression
  // whose type must be a class known when compiling (docs/decorators.md,
  // section 5). A class expression has its base here even when it is a
  // name, `class extends Base`, since that is usually a parameter.
  Expr *baseExpr = nullptr;
  std::vector<TypeRef> interfaces;
  // Set by Sema (sema_inherit.cpp): the class extended and the classes that
  // extend this one; the interfaces it implements, as types; the last number
  // among those of the classes down its chain, which follow its own; and
  // the methods its method table holds, by slot (null for an abstract one).
  ClassDecl *parent = nullptr;
  std::vector<ClassDecl *> children;
  std::vector<const Type *> implemented;
  unsigned lastId = 0;
  std::vector<FunctionDecl *> vtable;
  bool resolvingBase = false;
  // Parameter properties first, then the fields in the order declared: the
  // order node prints them in.
  std::vector<ClassField> fields;
  std::vector<FunctionDecl *> methods; // static or not, in the order declared
  std::vector<FunctionDecl *> constructors;
  // Set by Sema. The type of the instances, unique to the class; the method
  // that runs the initializers of the fields before any constructor, if a
  // field has one; the number of the class, for `instanceof`.
  const Type *type = nullptr;
  FunctionDecl *init = nullptr;
  unsigned id = 0;
  bool resolved = false;
  // Top-level code has run the declaration, which sets the static fields.
  bool declared = false;
  // The class of an object literal with methods. Its methods are closures,
  // whose captured variables the object itself holds; they share `group`.
  ObjectLitExpr *literal = nullptr;
  NestedGroup *group = nullptr;
  // A class of the language itself, which no file declares: Error and the
  // kinds derived from it (see Sema::declareErrorClass). CodeGen emits the
  // constructor of Error, which the kinds take.
  bool builtin = false;
  // Declared by a built-in module of tz written in tz (see Module::builtin),
  // such as EventEmitter of node:events, or Response of the globals of fetch.
  bool library = false;
  // `@Table("users") class User`, top to bottom (docs/decorators.md). Set by
  // Sema: the statements that apply the decorators of the class and of its
  // members, which run when the declaration does, after the static fields
  // get their values.
  std::vector<Decorator> decorators;
  std::vector<Stmt *> decorations;
  // Set by Sema: code uses the class as a value, whose object then holds the
  // static fields (they print with it, as in node).
  bool usedAsValue = false;
  // Classes as values (sema_classvalue.cpp). A class that a class
  // expression makes: Sema makes it while it checks code, and its static
  // fields get their values where the expression is evaluated. `outer` is
  // the copy of a generic function whose body has it, whose type parameters
  // it sees; `locals` the names of the variables of the functions around it,
  // which it cannot use.
  bool expression = false;
  const FunctionDecl *outer = nullptr;
  std::vector<llvm::StringRef> locals;
  // `@WithId class Order`: the class the decorator gave in its place, which
  // the name means from where the declaration runs on (`replaced`).
  ClassDecl *replacement = nullptr;
  bool replaced = false;
  // Reflection reaches the class (docs/decorators.md, section 6): its
  // description is in the exe, with the constructor when it becomes a
  // `Constructor<T>`, and its methods when its objects may be looked into
  // by name.
  bool reflectNew = false;
  bool reflectMethods = false;
  // fromJson(cls, j) may make its objects: from a Json, through its
  // constructor, then the fields named here (public, writable, of types a
  // Json gives) that the Json has. `reflectJson` when the class may be known
  // only when the program runs.
  bool reflectJson = false;
  std::vector<std::string> jsonParams, jsonFields; // the keys of the parameters, and the fields

  // `class Box<T> { ... }`: a generic class (docs/generics.md, section 5). It
  // is a pattern for a class for each list of type arguments, an instance
  // (`Box<int>`), which Sema makes from `source`, the text of the
  // declaration parsed again (see Sema::instantiateClass). Only its static
  // members, which no type parameter may appear in, belong to the generic
  // class itself.
  std::vector<TypeParamDecl> typeParams;
  llvm::StringRef source;
  // Set by Sema. What the type parameters stand for in the code of this
  // class: for the generic class itself, opaque type parameters
  // (Type::Kind::Param), with which its bodies are checked once; for an
  // instance, the types it is made for. An instance knows its generic
  // class, which knows its instances.
  std::vector<const Type *> typeArgs;
  ClassDecl *generic = nullptr;
  std::vector<ClassDecl *> instances;
  // An instance for type arguments that mention type parameters, which
  // generic code uses: it is checked with them, and not emitted.
  bool checkOnly = false;
  // The copies of its generic methods that calls need (see
  // Sema::instantiate), emitted with its other methods.
  std::vector<FunctionDecl *> methodInstances;
  // The generic class itself, rather than an instance of it.
  bool isGeneric() const { return !typeParams.empty() && !generic; }
  // A class whose code CodeGen emits: not a generic class (only its static
  // methods are) and not an instance that only generic code checks.
  bool isEmitted() const { return !isGeneric() && !checkOnly; }
  // The class the static members of this one belong to.
  const ClassDecl *staticOwner() const { return generic ? generic : this; }
  ClassDecl *staticOwner() { return generic ? generic : this; }
};

// Whether `cls` is Error or a class derived from it, built in or not.
inline bool derivesFromError(const ClassDecl *cls) {
  for (; cls; cls = cls->parent) {
    if (cls->builtin && cls->name == "Error") {
      return true;
    }
  }

  return false;
}

// The kinds of errors the language has built in (docs/errors.md): Error and
// the classes JavaScript derives from it, each a class that extends Error,
// in the order TZ_ERROR_* in runtime/core/tzrt/tzrt.h numbers them.
inline constexpr llvm::StringLiteral kErrorKinds[] = {"Error", "TypeError", "RangeError",
                                                      "SyntaxError", "URIError"};

// The position of `name` in kErrorKinds, or -1.
inline int errorKindIndex(llvm::StringRef name) {
  for (size_t i = 0; i < std::size(kErrorKinds); ++i) {
    if (kErrorKinds[i] == name) {
      return int(i);
    }
  }

  return -1;
}

} // namespace tz
