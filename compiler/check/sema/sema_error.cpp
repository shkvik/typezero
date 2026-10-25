// Exceptions (docs/errors.md): the class Error, `throw`, and `try` with
// `catch` and `finally`.
//
// Only an Error can be thrown, so a caught value always has a message and a
// stack. Error is a class of the language: an ordinary class with the fields
// `message`, `name` and `cause` and a constructor `(message?, options?)`,
// which no file declares and every file sees (Sema::lookup finds it after
// the program's own names). Its stack is no field: an error collects the
// places it passes on its way up (see codegen_throw.cpp), and `e.stack` makes
// the text from them. TypeError, RangeError, SyntaxError and URIError are
// classes that extend Error, built in too: they take its constructor, and
// their `name` is their own.
//
// `class ParseError extends Error` takes errorClass_ as its base like any
// other class (sema_inherit.cpp). Its constructor's `super(message, options)`
// calls the constructor of Error, whose body CodeGen emits
// (CodeGen::emitErrorConstructor), and its instances keep the fields of Error
// at the same offsets, as a subclass keeps those of its parent first; so the
// runtime finds message, name, cause and the places in any of them.

#include "compiler/check/sema/sema.h"

#include "llvm/Support/ConvertUTF.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

void Sema::declareErrorClass() {
  auto *cls = ctx_.make<ClassDecl>(llvm::SMLoc(), "Error");
  cls->builtin = true;
  cls->resolved = cls->declared = true;
  // Numbered with the program's classes, before those down its chain (see
  // linkClasses).
  classes_.push_back(cls);
  Type *type = Type::classType("Error", cls);
  cls->type = type;
  const Type *optionalError = Type::optional(type);
  auto field = [&](llvm::StringRef name, const Type *fieldType) {
    ClassField f;
    f.name = name;
    f.resolved = fieldType;
    f.optional = fieldType->isOptional();
    f.access = name.starts_with("#") ? Access::Private : Access::Public;
    cls->fields.push_back(f);
  };
  field("message", Type::string());
  field("name", Type::string());
  field("cause", optionalError);
  // The places the error passed on its way up, as the addresses of constant
  // descriptors (see tz_error_frame in runtime/core/error/error.c); private, as
  // `#` makes it.
  field("#frames", Type::array(Type::longTy()));
  std::vector<Field> fields;
  for (const ClassField &f : cls->fields)
    fields.push_back({f.name.str(), f.resolved, false});
  type->defineFields(std::move(fields));

  // constructor(message?: string, options?: { cause?: Error })
  auto *ctor = ctx_.make<FunctionDecl>(llvm::SMLoc(), "constructor");
  ctor->cls = cls;
  ctor->isConstructor = true;
  ctor->body = ctx_.make<BlockStmt>(llvm::SMLoc());
  ctor->checked = true;
  FuncSymbol *sym = ctx_.newFunc();
  sym->name = "constructor";
  sym->decl = ctor;
  sym->ret = Type::voidTy();
  const Type *options = Type::object({{"cause", optionalError, false}});
  const std::pair<llvm::StringRef, const Type *> params[] = {{"message", Type::string()},
                                                             {"options", options}};
  for (const auto &[name, paramType] : params) {
    Param param;
    param.name = name;
    param.optional = true;
    param.declaredType = Type::optional(paramType);
    ctor->params.push_back(param);
    sym->params.push_back(Type::optional(paramType));
  }
  ctor->sym = sym;
  cls->constructors.push_back(ctor);
  errorClass_ = cls;

  // The kinds: `class TypeError extends Error`, with Error's fields and
  // constructor.
  errorKinds_ = {cls};
  for (size_t k = 1; k < std::size(kErrorKinds); ++k) {
    auto *kind = ctx_.make<ClassDecl>(llvm::SMLoc(), kErrorKinds[k]);
    kind->builtin = true;
    kind->resolved = kind->declared = true;
    kind->parent = cls;
    cls->children.push_back(kind);
    Type *kindType = Type::classType(kErrorKinds[k], kind);
    kind->type = kindType;
    kindType->defineFields(std::vector<Field>(type->fields().begin(), type->fields().end()));
    addBase(kindType, type);
    errorKinds_.push_back(kind);
  }

  // SystemError (docs/io.md, section 5), the errors of the system that the
  // functions of node:fs throw, with fields of its own after Error's: only
  // the runtime makes them (see runtime/library/node/fs/fs.c), so it has no
  // constructor.
  auto *system = ctx_.make<ClassDecl>(llvm::SMLoc(), "SystemError");
  system->builtin = true;
  system->resolved = system->declared = true;
  system->parent = cls;
  cls->children.push_back(system);
  Type *systemType = Type::classType("SystemError", system);
  system->type = systemType;
  const std::pair<llvm::StringRef, const Type *> own[] = {
      {"errno", Type::intTy()},
      {"code", Type::string()},
      {"syscall", Type::string()},
      {"path", Type::optional(Type::string())},
      {"dest", Type::optional(Type::string())}};
  std::vector<Field> systemFields(type->fields().begin(), type->fields().end());
  for (const auto &[name, fieldType] : own) {
    ClassField f;
    f.name = name;
    f.resolved = fieldType;
    f.optional = fieldType->isOptional();
    system->fields.push_back(f);
    systemFields.push_back({name.str(), fieldType, false});
  }
  systemType->defineFields(std::move(systemFields));
  addBase(systemType, type);
  systemErrorClass_ = system;
}

ClassDecl *Sema::errorKindClass(llvm::StringRef name) const {
  if (name == "SystemError")
    return systemErrorClass_;
  if (name == "ExecSyncError")
    return execSyncErrorClass_;
  int kind = errorKindIndex(name);
  return kind >= 0 && size_t(kind) < errorKinds_.size() ? errorKinds_[size_t(kind)] : nullptr;
}

bool Sema::isErrorType(const Type *type) const {
  return errorClass_ && type && derivesFromError(type->classDecl());
}

namespace {

std::string utf8(const std::u16string &text) {
  std::string out;
  llvm::convertUTF16ToUTF8String(llvm::ArrayRef<llvm::UTF16>(
                                     reinterpret_cast<const llvm::UTF16 *>(text.data()), text.size()),
                                 out);
  return out;
}

} // namespace

void Sema::checkThrow(ThrowStmt *stmt) {
  stmt->value = checkExpr(stmt->value, errorClass_->type);
  const Type *type = stmt->value->type;
  // An unknown, as `catch (e: unknown)` gives: what it holds is thrown if it
  // is an Error, and a TypeError otherwise (see tz_unknown_error).
  if (type->isError() || isErrorType(type) || type->isUnknown())
    return;
  if (type->isOptional() && isErrorType(type->inner())) {
    diags_.error(stmt->value->loc(), "the error may be undefined; check it before throwing it");
    return;
  }
  // `throw "no file"`: the hint wraps the text in an Error.
  std::string text = "...";
  if (auto *lit = dyn_cast<StringLitExpr>(stmt->value))
    text = "\"" + utf8(lit->value) + "\"";
  else if (auto *name = dyn_cast<NameExpr>(stmt->value); name && type->isString())
    text = name->name.str();
  else if (!type->isString())
    text = "`...`";
  diags_.error(stmt->value->loc(), "only an Error can be thrown, found '" + type->name() +
                                       "'; throw one with the text as its message, e.g. 'throw "
                                       "new Error(" + text + ")'");
}

Sema::Flow Sema::withoutChanges(const Flow &flow, const LoopScan &scan) {
  // A copy first: `flow` is often flow_ itself, which a move would empty.
  Flow saved = flow_;
  flow_ = flow;
  Flow result = loopHead(scan, nullptr, /*induction=*/false);
  flow_ = std::move(saved);
  result.unreachable = flow.unreachable;
  return result;
}

// An exception may leave the block of a `try` anywhere, so the `catch` starts
// with what holds before the block and stays true in all of it; so does the
// `finally` for the block and the `catch`, as it also runs when they throw or
// jump out.
void Sema::checkTry(TryStmt *stmt) {
  LoopScan blockScan = scanLoop({stmt->block}, {});
  Flow inBlock = withoutChanges(flow_, blockScan);
  std::vector<const Stmt *> guarded{stmt->block};
  if (stmt->handler)
    guarded.push_back(stmt->handler);
  LoopScan guardedScan = scanLoop(guarded, {});
  Flow inGuarded = withoutChanges(flow_, guardedScan);

  checkStmt(stmt->block);
  Flow normal = flow_;
  if (stmt->handler) {
    flow_ = inBlock;
    pushScope();
    VarDeclarator &param = stmt->param;
    if (!param.name.empty()) {
      // `catch (e: unknown)`, as strict TypeScript asks for: an unknown, to be
      // checked with `instanceof` before use, though it is always an Error.
      const Type *caught = errorClass_->type;
      if (param.typeAnn) {
        const TypeRef &ref = *param.typeAnn;
        if (ref.kind == TypeRef::Kind::Name && ref.name == "unknown")
          caught = Type::unknown();
        else if (!isErrorType(resolveType(ref, /*allowVoid=*/false)))
          diags_.error(ref.loc, "the caught value is always an Error; leave out the type, "
                                "'catch (" + param.name + ")', and tell kinds apart with "
                                "'instanceof'");
      }
      param.sym = declareVar(param.name, param.loc, caught, /*isConst=*/false);
    }
    checkStmt(stmt->handler);
    popScope();
    normal = join(normal, flow_);
  }
  if (!stmt->finalizer) {
    flow_ = normal;
    return;
  }
  flow_ = join(normal, inGuarded);
  checkStmt(stmt->finalizer);
  // After the `finally`, control goes on only where the block or the `catch`
  // ended normally; otherwise it goes on with their return, jump or throw.
  flow_.unreachable |= normal.unreachable;
  // A `break` out of the block went through the `finally`, which may have
  // changed what was known where it jumped.
  LoopScan finallyScan = scanLoop({stmt->finalizer}, {});
  for (JumpTarget &target : targets_)
    target.breaks = withoutChanges(target.breaks, finallyScan);
}

Expr *Sema::checkErrorStack(MemberExpr *member) {
  return builtinCall(member->loc(), member, Builtin::ErrorStack, {}, Type::string());
}

Expr *Sema::checkErrorMethod(CallExpr *call, MemberExpr *member) {
  if (member->name != "toString")
    return nullptr;
  if (!checkArity(call, member->loc(), 0, 0))
    return call;
  call->builtin = Builtin::ErrorToString;
  call->type = Type::string();
  member->type = Type::function({}, 0, false, Type::string());
  return call;
}

} // namespace tz
