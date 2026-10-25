#include "compiler/check/errors/throw_checker/throw_checker.h"
#include "compiler/check/scopes/scopes.h"
#include "compiler/check/types/type_resolver/type_resolver.h"
#include "compiler/check/stmt/stmt_checker/stmt_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/expr/argument_checker/argument_checker.h"
#include "compiler/check/flow/flow_checker/flow_checker.h"
#include "compiler/check/flow/loop_scanner/loop_scanner.h"
#include "compiler/check/flow/loop_bounds/loop_bounds.h"
#include "compiler/check/classes/class_bases/class_bases.h"
#include "compiler/library/process/process_checker/process_checker.h"
#include "compiler/whole_program/flows/flows.h"
#include "llvm/Support/ConvertUTF.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::dyn_cast_or_null;

namespace tz {

namespace {

std::string utf8(const std::u16string &text) {
  std::string out;
  llvm::convertUTF16ToUTF8String(
      llvm::ArrayRef<llvm::UTF16>(reinterpret_cast<const llvm::UTF16 *>(text.data()), text.size()),
      out);

  return out;
}

} // namespace

ThrowChecker::ThrowChecker(Sema &sema) : sema_(sema) {}

void ThrowChecker::declareErrorClass() {
  auto *cls = sema_.ctx_.make<ClassDecl>(llvm::SMLoc(), "Error");
  cls->builtin = true;
  cls->resolved = cls->declared = true;
  // Numbered with the program's classes, before those down its chain (see
  // linkClasses).
  sema_.classes_.push_back(cls);
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
  for (const ClassField &f : cls->fields) {
    fields.push_back({f.name.str(), f.resolved, false});
  }
  type->defineFields(std::move(fields));

  // constructor(message?: string, options?: { cause?: Error })
  auto *ctor = sema_.ctx_.make<FunctionDecl>(llvm::SMLoc(), "constructor");
  ctor->cls = cls;
  ctor->isConstructor = true;
  ctor->body = sema_.ctx_.make<BlockStmt>(llvm::SMLoc());
  ctor->checked = true;
  FuncSymbol *sym = sema_.ctx_.newFunc();
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
  sema_.errorClass_ = cls;

  // The kinds: `class TypeError extends Error`, with Error's fields and
  // constructor.
  sema_.errorKinds_ = {cls};
  for (size_t k = 1; k < std::size(kErrorKinds); ++k) {
    auto *kind = sema_.ctx_.make<ClassDecl>(llvm::SMLoc(), kErrorKinds[k]);
    kind->builtin = true;
    kind->resolved = kind->declared = true;
    kind->parent = cls;
    cls->children.push_back(kind);
    Type *kindType = Type::classType(kErrorKinds[k], kind);
    kind->type = kindType;
    kindType->defineFields(std::vector<Field>(type->fields().begin(), type->fields().end()));
    sema_.classBases().addBase(kindType, type);
    sema_.errorKinds_.push_back(kind);
  }

  // SystemError (docs/io.md, section 5), the errors of the system that the
  // functions of node:fs throw, with fields of its own after Error's: only
  // the runtime makes them (see runtime/library/node/fs/fs.c), so it has no
  // constructor.
  auto *system = sema_.ctx_.make<ClassDecl>(llvm::SMLoc(), "SystemError");
  system->builtin = true;
  system->resolved = system->declared = true;
  system->parent = cls;
  cls->children.push_back(system);
  Type *systemType = Type::classType("SystemError", system);
  system->type = systemType;
  const std::pair<llvm::StringRef, const Type *> own[] = {{"errno", Type::intTy()},
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
  sema_.classBases().addBase(systemType, type);
  sema_.systemErrorClass_ = system;
}

ClassDecl *ThrowChecker::errorKindClass(llvm::StringRef name) const {
  if (name == "SystemError") {
    return sema_.systemErrorClass_;
  }
  if (name == "ExecSyncError") {
    return sema_.execSyncErrorClass_;
  }
  int kind = errorKindIndex(name);

  return kind >= 0 && size_t(kind) < sema_.errorKinds_.size() ? sema_.errorKinds_[size_t(kind)] : nullptr;
}

bool ThrowChecker::isErrorType(const Type *type) const {
  return sema_.errorClass_ && type && derivesFromError(type->classDecl());
}

void ThrowChecker::checkThrow(ThrowStmt *stmt) {
  stmt->value = sema_.exprChecker().checkExpr(stmt->value, sema_.errorClass_->type);
  const Type *type = stmt->value->type;
  // An unknown, as `catch (e: unknown)` gives: what it holds is thrown if it
  // is an Error, and a TypeError otherwise (see tz_unknown_error).
  if (type->isError() || isErrorType(type) || type->isUnknown()) {
    return;
  }
  if (type->isOptional() && isErrorType(type->inner())) {
    sema_.diags_.error(stmt->value->loc(), "the error may be undefined; check it before throwing it");
    return;
  }
  // `throw "no file"`: the hint wraps the text in an Error.
  std::string text = "...";
  if (auto *lit = dyn_cast<StringLitExpr>(stmt->value)) {
    text = "\"" + utf8(lit->value) + "\"";
  } else if (auto *name = dyn_cast<NameExpr>(stmt->value); name && type->isString()) {
    text = name->name.str();
  } else if (!type->isString()) {
    text = "`...`";
  }
  sema_.diags_.error(stmt->value->loc(), "only an Error can be thrown, found '" + type->name() +
                                       "'; throw one with the text as its message, e.g. 'throw "
                                       "new Error(" +
                                       text + ")'");
}

Sema::Flow ThrowChecker::withoutChanges(const Sema::Flow &flow, const Sema::LoopScan &scan) {
  // A copy first: `flow` is often flow_ itself, which a move would empty.
  Sema::Flow saved = sema_.flow_;
  sema_.flow_ = flow;
  Sema::Flow result = sema_.loopBounds().loopHead(scan, nullptr, /*induction=*/false);
  sema_.flow_ = std::move(saved);
  result.unreachable = flow.unreachable;

  return result;
}

// An exception may leave the block of a `try` anywhere, so the `catch` starts
// with what holds before the block and stays true in all of it; so does the
// `finally` for the block and the `catch`, as it also runs when they throw or
// jump out.
void ThrowChecker::checkTry(TryStmt *stmt) {
  Sema::LoopScan blockScan = sema_.loopScanner().scanLoop({stmt->block}, {});
  Sema::Flow inBlock = withoutChanges(sema_.flow_, blockScan);
  std::vector<const Stmt *> guarded{stmt->block};
  if (stmt->handler) {
    guarded.push_back(stmt->handler);
  }
  Sema::LoopScan guardedScan = sema_.loopScanner().scanLoop(guarded, {});
  Sema::Flow inGuarded = withoutChanges(sema_.flow_, guardedScan);

  sema_.stmtChecker().checkStmt(stmt->block);
  Sema::Flow normal = sema_.flow_;
  if (stmt->handler) {
    sema_.flow_ = inBlock;
    sema_.pushScope();
    VarDeclarator &param = stmt->param;
    if (!param.name.empty()) {
      // `catch (e: unknown)`, as strict TypeScript asks for: an unknown, to be
      // checked with `instanceof` before use, though it is always an Error.
      const Type *caught = sema_.errorClass_->type;
      if (param.typeAnn) {
        const TypeRef &ref = *param.typeAnn;
        if (ref.kind == TypeRef::Kind::Name && ref.name == "unknown") {
          caught = Type::unknown();
        } else if (!isErrorType(sema_.typeResolver().resolveType(ref, /*allowVoid=*/false))) {
          sema_.diags_.error(ref.loc, "the caught value is always an Error; leave out the type, "
                                "'catch (" +
                                    param.name +
                                    ")', and tell kinds apart with "
                                    "'instanceof'");
        }
      }
      param.sym = sema_.scopes().declareVar(param.name, param.loc, caught, /*isConst=*/false);
    }
    sema_.stmtChecker().checkStmt(stmt->handler);
    sema_.popScope();
    normal = FlowChecker::join(normal, sema_.flow_);
  }
  if (!stmt->finalizer) {
    sema_.flow_ = normal;
    return;
  }
  sema_.flow_ = FlowChecker::join(normal, inGuarded);
  sema_.stmtChecker().checkStmt(stmt->finalizer);
  // After the `finally`, control goes on only where the block or the `catch`
  // ended normally; otherwise it goes on with their return, jump or throw.
  sema_.flow_.unreachable |= normal.unreachable;
  // A `break` out of the block went through the `finally`, which may have
  // changed what was known where it jumped.
  Sema::LoopScan finallyScan = sema_.loopScanner().scanLoop({stmt->finalizer}, {});
  for (Sema::JumpTarget &target : sema_.targets_) {
    target.breaks = withoutChanges(target.breaks, finallyScan);
  }
}

Expr *ThrowChecker::checkErrorStack(MemberExpr *member) {
  return sema_.processChecker().builtinCall(member->loc(), member, Builtin::ErrorStack, {}, Type::string());
}

Expr *ThrowChecker::checkErrorMethod(CallExpr *call, MemberExpr *member) {
  if (member->name != "toString") {
    return nullptr;
  }
  if (!sema_.argumentChecker().checkArity(call, member->loc(), 0, 0)) {
    return call;
  }
  call->builtin = Builtin::ErrorToString;
  call->type = Type::string();
  member->type = Type::function({}, 0, false, Type::string());

  return call;
}

} // namespace tz
