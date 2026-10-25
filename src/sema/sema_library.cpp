// The built-in modules tz has written in tz (src/lib, docs/net.md): tz:http
// and the modules of the globals of the web platform, URL, fetch, Response
// and the others. Their code is checked as any file is; what it has that a
// program has not is checked here:
//
//   - Intrinsics: `function __tz_name(...): T` without a body is a call of
//     the runtime's C function tz_name (CodeGen::emitExternal). The types
//     that cross are few, those C sees plainly: numbers, booleans, strings,
//     and references (objects, arrays, Json, promises, optionals of
//     those). A Promise is made by the call and settled by the runtime.
//   - Accessors: `get x()` and `set x(v)` of a class are methods named x
//     that reading `o.x` and assigning `o.x = v` call, as the getters of
//     node's classes are (Response, URL...). A program sees properties.
//
// Which modules are those, their classes and where their code is
// (Module::builtin, prepareLibraries, inLibrary) is in sema_events.cpp, for
// node:events and tz:web too. The intrinsics written as calls, `__tz_as<C>`
// and the others that need the compiler, are checked there (checkLibCall);
// a declared one goes to C as is.

#include "sema/sema.h"

#include "llvm/ADT/STLExtras.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

namespace {

// A value C gets as a pointer: what lies in memory as a reference.
bool isExternalReference(const Type *type) {
  return type->isObject() || type->isArray() || type->isIterator() || type->isJson() ||
         type->isDict() || type->isPromise() || type->isUnknown();
}

// Whether a value of `type` can cross to the runtime: a number, a boolean,
// a string, or a reference, which may be undefined.
bool isExternalValue(const Type *type) {
  if (type->isInteger() || type->isFloat() || type->isBool() || type->isString())
    return true;
  if (type->isOptional())
    return isExternalReference(type->inner()) && !type->inner()->isArray();
  return isExternalReference(type);
}

} // namespace

void Sema::checkExternal(FunctionDecl *fn) {
  FuncSymbol *sym = fn->sym;
  resolveParams(sym);
  fn->checked = true;
  if (!fn->typeParams.empty() || sym->hasRest) {
    diags_.error(fn->loc(), "an intrinsic takes plain parameters: no type parameters, no rest");
    return;
  }
  for (size_t i = 0; i < fn->params.size(); ++i) {
    const Type *type = sym->params[i];
    if (fn->params[i].defaultValue || fn->params[i].optional)
      diags_.error(fn->params[i].loc, "an intrinsic has no optional parameters");
    else if (!type->isError() && !isExternalValue(type))
      diags_.error(fn->params[i].loc, "an intrinsic cannot take a value of type '" +
                                          type->name() + "'");
  }
  const Type *ret = sym->ret;
  if (!ret)
    return;
  if (ret->isPromise()) {
    file_->usesAsync = true;
    ret = ret->promised();
  }
  if (!ret->isVoid() && !ret->isError() && !isExternalValue(ret))
    diags_.error(fn->loc(), "an intrinsic cannot give a value of type '" + sym->ret->name() + "'");
}

Expr *Sema::adaptResult(Expr *expr, const Type *to) {
  const Type *from = expr->type;
  // The parameters the callers of `to` pass, then any number of optional
  // ones, which get undefined (hono's `app.fetch(request, env?, ctx?)`).
  llvm::ArrayRef<const Type *> mine = from->params(), given = to->params();
  if (mine.size() < given.size() || from->minArgs() > given.size() || from->hasRest() ||
      to->hasRest() || from->hasThis() != to->hasThis() ||
      mine.take_front(given.size()) != given ||
      (given.size() < to->minArgs()))
    return nullptr;
  bool fewer = mine.size() > given.size();
  const Type *got = from->ret(), *wanted = to->ret();
  if ((got == wanted && !fewer) || got->isVoid() || wanted->isVoid() || got->isError() ||
      wanted->isError() || (got != wanted && !isImplicitlyConvertible(got, wanted)))
    return nullptr;
  auto *cast = ctx_.make<CastExpr>(expr->loc(), expr, std::nullopt);
  cast->type = to;
  cast->adaptsResult = true;
  return cast;
}

//===----------------------------------------------------------------------===//
// Accessors
//===----------------------------------------------------------------------===//

FunctionDecl *Sema::accessorOf(ClassDecl *cls, llvm::StringRef name,
                               FunctionDecl::Accessor kind) {
  for (const ClassDecl *c = cls; c; c = c->parent)
    for (FunctionDecl *fn : c->methods)
      if (fn->name == name && !fn->isStatic && fn->accessor == kind)
        return fn;
  return nullptr;
}

Expr *Sema::checkAccessorRead(MemberExpr *member, ClassDecl *cls, const Type *expected) {
  if (!accessorOf(cls, member->name, FunctionDecl::Accessor::Getter)) {
    if (accessorOf(cls, member->name, FunctionDecl::Accessor::Setter)) {
      diags_.error(member->loc(), "'" + member->name + "' of '" + cls->name +
                                      "' can only be assigned");
      member->type = Type::error();
      return member;
    }
    return nullptr;
  }
  auto *call = ctx_.make<CallExpr>(member->loc(), member, std::vector<Expr *>());
  accessorCalls_.insert(call);
  return checkMethodCall(call, member, expected);
}

const Type *Sema::checkAccessorTarget(MemberExpr *member, ClassDecl *cls, bool compound) {
  FunctionDecl *setter = accessorOf(cls, member->name, FunctionDecl::Accessor::Setter);
  if (!setter) {
    if (!accessorOf(cls, member->name, FunctionDecl::Accessor::Getter))
      return nullptr;
    diags_.error(member->loc(), "cannot assign to '" + member->name + "' because it is a "
                                    "read-only property of '" + cls->name + "'");
    return Type::error();
  }
  if (compound) {
    diags_.error(member->loc(), "'" + member->name + "' of '" + cls->name +
                                    "' is assigned only with '=': write 'o." + member->name +
                                    " = o." + member->name + " + value'");
    return Type::error();
  }
  pendingSetter_ = member;
  resolveParams(setter->sym);
  return setter->sym->params.empty() ? Type::error() : setter->sym->params[0];
}

Expr *Sema::checkSetterCall(AssignExpr *assign) {
  MemberExpr *member = std::exchange(pendingSetter_, nullptr);
  auto *call = ctx_.make<CallExpr>(assign->loc(), member, std::vector<Expr *>{assign->value});
  accessorCalls_.insert(call);
  return checkMethodCall(call, member, nullptr);
}

bool Sema::rejectAccessorCall(CallExpr *call, MemberExpr *member, FuncSymbol *sym) {
  if (!sym || !sym->decl || sym->decl->accessor == FunctionDecl::Accessor::None ||
      accessorCalls_.contains(call))
    return false;
  diags_.error(member->loc(), "'" + member->name + "' is a property, not a method: " +
                                  (sym->decl->accessor == FunctionDecl::Accessor::Getter
                                       ? "read it without '()'"
                                       : "assign to it"));
  return true;
}

} // namespace tz
