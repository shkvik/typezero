// The built-in modules tz has written in tz (library/, docs/net.md): tz:http
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

#include "compiler/check/sema/sema.h"

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

//===----------------------------------------------------------------------===//
// TLS
//===----------------------------------------------------------------------===//

// TLS is the largest part a program can get, OpenSSL's libssl and all of its libcrypto (docs/net.md,
// section 4), so tz links it only into a program that may use https: one that fetches a URL not
// known to be http:// or data: (an https:// one, or one known only when it runs), or serves with
// options that may have `tls`. A program with neither gets no TLS; there, a call of the runtime's
// TLS (`__tz_tls_*`) is one that fails (CodeGen::emitExternal), which only a redirect from
// http:// to https:// reaches.

std::string Sema::knownPrefix(const Expr *expr) const {
  while (auto *cast = dyn_cast<CastExpr>(expr))
    expr = cast->operand;
  auto ascii = [](const std::u16string &text) {
    std::string out;
    for (char16_t c : text) {
      if (c >= 0x80)
        break;
      out += static_cast<char>(c);
    }
    return out;
  };
  if (auto *lit = dyn_cast<StringLitExpr>(expr))
    return ascii(lit->value);
  if (auto *tmpl = dyn_cast<TemplateExpr>(expr)) {
    if (tmpl->quasis[0].empty() && !tmpl->exprs.empty())
      return knownPrefix(tmpl->exprs[0]);
    return ascii(tmpl->quasis[0]);
  }
  if (auto *bin = dyn_cast<BinaryExpr>(expr); bin && bin->op == BinaryOp::Add)
    return knownPrefix(bin->lhs);
  if (auto *name = dyn_cast<NameExpr>(expr); name && name->var) {
    auto it = constPrefixes_.find(name->var);
    return it == constPrefixes_.end() ? std::string() : it->second;
  }
  // `new URL(url)` and `new Request(url, init)`: the address they are made of.
  if (auto *made = dyn_cast<NewExpr>(expr); made && made->cls && made->cls->library &&
                                            (made->cls->name == "URL" ||
                                             made->cls->name == "Request")) {
    const std::vector<Expr *> &args = made->ctor ? made->ctor->args : made->args;
    if (!args.empty() && (made->cls->name == "Request" || args.size() == 1))
      return knownPrefix(args[0]);
  }
  return std::string();
}

void Sema::noteHttpsCall(CallExpr *call, const FunctionDecl *fn) {
  if (!fn || file_->usesTls || call->args.empty() || inLibrary(call->loc()))
    return;
  if (fn == libFunctions_.lookup("fetch")) {
    std::string prefix = llvm::StringRef(knownPrefix(call->args[0])).lower();
    if (!llvm::StringRef(prefix).starts_with("http://") &&
        !llvm::StringRef(prefix).starts_with("data:"))
      file_->usesTls = true;
  } else if (fn == libFunctions_.lookup("serve")) {
    // Options written out have `tls` or not (a spread may bring it); others, whatever their type
    // allows.
    const Expr *options = call->args[0];
    while (auto *cast = dyn_cast<CastExpr>(options))
      options = cast->operand;
    if (auto *lit = dyn_cast<ObjectLitExpr>(options))
      file_->usesTls = llvm::any_of(lit->props, [](const ObjectLitExpr::Property &p) {
        return p.spread || p.name == "tls";
      });
    else
      file_->usesTls = !options->type || !options->type->isObject() ||
                       options->type->field("tls") != nullptr;
  }
}

void Sema::noteHttpsValue(const FunctionDecl *fn, llvm::SMLoc loc) {
  if (fn && !inLibrary(loc) &&
      (fn == libFunctions_.lookup("fetch") || fn == libFunctions_.lookup("serve")))
    file_->usesTls = true;
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
