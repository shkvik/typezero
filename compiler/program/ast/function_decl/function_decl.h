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

// A function: a declaration, or the function of a FunctionExpr. A function
// nested in another one (or in main's blocks) is a closure: its value carries
// an environment with the variables of the functions around it that it uses.
struct FunctionDecl : Stmt {
  FunctionDecl(llvm::SMLoc loc, llvm::StringRef name) : Stmt(Kind::Function, loc), name(name) {}
  static bool classof(const Stmt *s) { return s->kind() == Kind::Function; }

  llvm::StringRef name;
  // `function f<T, U>(...)`: a generic function.
  std::vector<TypeParamDecl> typeParams;
  // Set by Checker: what the type parameters stand for while the body is
  // checked. For the generic function itself they are opaque type parameters
  // (Type::Kind::Param); for a copy of it made for some types (an instance,
  // see Checker::instantiate), those types.
  std::vector<const Type *> typeArgs;
  bool isInstance = false;
  std::vector<Param> params;
  std::optional<TypeRef> returnType;
  BlockStmt *body = nullptr;
  // The source text of the declaration, for Checker to parse a fresh copy.
  llvm::StringRef source;
  bool isExpression = false; // an arrow or a function expression
  bool isArrow = false;
  // `function*`: a call creates a generator, an iterator that runs the body
  // up to each `yield` as it is asked for the next value.
  bool isGenerator = false;
  // `async function`, `async () => ...`, `async m() {}`: a call runs the body
  // up to its first `await` and gives a Promise of what it returns.
  bool isAsync = false;
  // Its code awaits `__tz_ahead(p)` (Builtin::LibAhead): a piece of it may
  // run ahead of its turn.
  bool awaitsAhead = false;
  // Set by Checker for the `function*` given to safeTry, which is no generator:
  // it runs once, at the call, and `yield* r` in it takes the value of the
  // Result r or returns its Err (see Checker::checkSafeTry). Checker clears
  // isGenerator then.
  bool safeTry = false;
  // A generic arrow or function expression that a top-level constant names
  // (`const id = <T>(x: T) => x`): Checker makes it a top-level function, like a
  // declaration; it is no longer an expression.
  bool lifted = false;
  // A function expression's own name (`function f() {}`), which its body
  // sees.
  llvm::StringRef ownName;
  // An arrow whose body is an expression: the parser makes it `{ return e }`.
  bool concise = false;
  FuncSymbol *sym = nullptr; // set by Checker
  bool checked = false;      // set by Checker once the body is checked

  // Set by Checker for closures. `parent` is the function around this one (null
  // for main). `captures` are the variables of the functions around it that
  // the body uses, in the order of the environment's fields; the functions
  // declared in one block share an environment (`group`).
  FunctionDecl *parent = nullptr;
  std::vector<VarSymbol *> captures;
  NestedGroup *group = nullptr;
  // The other functions of its group that it uses.
  std::vector<FunctionDecl *> siblings;
  VarSymbol *var = nullptr; // a nested declaration's variable
  // When the closure is created, in the order of declarations (see VarSymbol).
  unsigned creationSeq = 0;
  // Passed straight to a built-in that only calls it during the call, such as
  // map: the environment can live on the stack.
  bool nonEscaping = false;
  bool isClosure() const { return parent || isExpression || group; }

  // Methods (sema_class.cpp): the class, or the object literal (see
  // ClassDecl::literal), the method belongs to. A method that is not static
  // gets the object as `this`, a variable Checker declares in its body.
  ClassDecl *cls = nullptr;
  bool isStatic = false;
  bool isConstructor = false;
  Access access = Access::Public;
  VarSymbol *thisVar = nullptr;
  // Overloads: the position of this variant among the methods of its name.
  unsigned variant = 0;
  bool takesThis() const { return cls && !isStatic; }
  // Inheritance (sema_inherit.cpp). `abstract m(): T` has no body; `override`
  // says that the method replaces one of the class extended.
  bool isAbstract = false;
  bool isOverride = false;
  // Set by Checker: the variant of a class up the chain that this one replaces
  // (the same name and parameters); whether a class down the chain replaces
  // this one; and the slot of the method table that a call through the class
  // goes through, -1 for a method that nothing replaces, which every call
  // calls directly.
  FunctionDecl *overrides = nullptr;
  bool overridden = false;
  int slot = -1;
  // A class that a class expression makes may override it, when the program
  // is checked: every call goes through the method table (see
  // Checker::markLateOverrides).
  bool openToLate = false;
  // A method's decorators (docs/decorators.md), top to bottom. Set by Checker
  // when one of them takes the method's descriptor and may replace what the
  // method does: the hidden global that holds that, a function of the type
  // of the descriptor's `value`, which every call of the method goes through
  // (see codegen_decorate.cpp).
  std::vector<Decorator> decorators;
  VarSymbol *impl = nullptr;
  // A method of an instance of a generic class (see ClassDecl::generic):
  // the method of the generic class it is a copy of, whose signature, with
  // the instance's types, is its own.
  FunctionDecl *generic = nullptr;
  // A rest parameter `...args: L` of an argument list L (a tuple, what `A
  // extends unknown[]` stands for in a copy) is the parameters of L, from
  // `listStart` on, with hidden names; `listName` names them together in
  // the body (see Checker::expandArgumentList). Empty without one.
  llvm::StringRef listName;
  size_t listStart = 0;
  const Type *listType = nullptr;
  // In a built-in module of tz written in tz (Module::builtin): `function
  // __tz_name(...): T` without a body is a call of the runtime's tz_name,
  // which the parser gives an empty body (see Checker::checkExternal and
  // Lowering::emitExternal). `get x()` and `set x(v)` of a class there are
  // methods named x that reading and assigning `o.x` call, as JavaScript's
  // accessors (see Checker::checkAccessorRead).
  bool external = false;
  enum class Accessor { None, Getter, Setter };
  Accessor accessor = Accessor::None;
  // `@__tz_hidden get x()`: a getter that does not print, as node's classes
  // do not show every property they have.
  bool hiddenGetter = false;
};

} // namespace tz
