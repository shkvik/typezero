#include "compiler/semantic/library/tz_modules/library_calls/library_calls.h"
#include "compiler/program/library/intrinsic/library_intrinsic_table/library_intrinsic_table.h"
#include "compiler/semantic/bodies/expr/argument_checker/argument_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/library/crypto/web_crypto_checker/web_crypto_checker.h"
#include "compiler/semantic/library/stream/stream_checker/stream_checker.h"
#include "compiler/semantic/library/tz_modules/tz_modules_checker/tz_modules_checker_errors.h"
#include "compiler/semantic/names/scopes/scopes.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/type_resolver/type_resolver.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Path.h"

using llvm::dyn_cast;

namespace tz {

namespace {

constexpr llvm::StringLiteral kIntrinsicsModule = "tz:intrinsics";

}

LibraryCalls::LibraryCalls(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->scopes = &all.scopes;
  this->exprChecker = &all.exprChecker;
  this->typeResolver = &all.typeResolver;
  this->streamChecker = &all.streamChecker;
  this->argumentChecker = &all.argumentChecker;
  this->webCryptoChecker = &all.webCryptoChecker;
}

void LibraryCalls::prepareLibraries(SourceFile &program) {
  for (Module *module : program.modules) {
    if (module->builtin) {
      this->prepareLibrary(module);
    }
  }
}

void LibraryCalls::prepareLibrary(Module *module) {
  this->state.getInventoryStore().markComparesFunctions();
  llvm::SourceMgr &sources = this->state.getDiagnostics().sourceMgr();
  for (unsigned id : module->buffers) {
    const llvm::MemoryBuffer *buffer = sources.getMemoryBuffer(id);
    this->state.getLibraryDeclarations().addLibraryRange(buffer->getBufferStart(),
                                                         buffer->getBufferEnd());
    this->state.getDiagnostics().quietRange(buffer->getBufferStart(), buffer->getBufferEnd());
  }
  bool hasIntrinsics = module->name == kIntrinsicsModule;
  for (Stmt *item : module->file.items) {
    this->rememberLibraryItem(item, hasIntrinsics);
  }
}

void LibraryCalls::rememberLibraryItem(Stmt *item, bool hasIntrinsics) {
  auto *functionDecl = dyn_cast<FunctionDecl>(item);
  if (functionDecl != nullptr) {
    return this->rememberLibraryFunction(functionDecl, hasIntrinsics);
  }
  auto *alias = dyn_cast<TypeAliasDecl>(item);
  if (alias != nullptr) {
    return this->state.getLibraryDeclarations().rememberLibraryType(alias);
  }
  auto *classDecl = dyn_cast<ClassDecl>(item);
  if (classDecl != nullptr) {
    classDecl->origin = ClassOrigin::LIBRARY;
    this->state.getLibraryDeclarations().rememberLibraryClass(classDecl);
  }
}

void LibraryCalls::rememberLibraryFunction(FunctionDecl *functionDecl, bool hasIntrinsics) {
  if (!hasIntrinsics) {
    return this->state.getLibraryDeclarations().rememberLibraryFunction(functionDecl);
  }
  if (functionDecl->external()) {
    functionDecl->bodySource = FunctionBodySource::INTRINSIC;
  }
}

bool LibraryCalls::isInLibrary(llvm::SMLoc loc) const {
  return this->state.getLibraryDeclarations().isInsideLibrary(loc);
}

LibraryCalls::ResultIntrinsicShape LibraryCalls::checkIntrinsicShape(const CallExpr *call,
                                                                     const NameExpr *name,
                                                                     llvm::StringRef intrinsicName,
                                                                     bool known, unsigned args,
                                                                     unsigned typeArgs) const {
  bool isWrong =
      !known || call->args.size() != args || name->typeArgs.size() != typeArgs || call->hasSpread();
  if (isWrong) {
    auto message = ErrorTextIntrinsicCall::wrong(intrinsicName);

    return err(LibraryCallsIntrinsicShapeError{
        .code = LibraryCallsIntrinsicShapeErrorCode::NOT_AN_INTRINSIC,
        .message = message,
    });
  }

  return ok(call);
}

Expr *LibraryCalls::checkLibCall(CallExpr *call, NameExpr *name) {
  auto own = this->scopes->lookup(name->name);
  const FunctionDecl *functionDecl = own.func() != nullptr ? own.func()->decl : nullptr;
  if (functionDecl == nullptr || !functionDecl->intrinsic()) {
    return nullptr;
  }
  llvm::StringRef intrinsicName = functionDecl->name;
  call->type = Type::error();
  if (intrinsicName.starts_with("subtle")) {
    return this->webCryptoChecker->checkSubtleIntrinsic(call, name, intrinsicName);
  }
  const auto *intrinsic = LibraryIntrinsicTable::find(intrinsicName);
  bool isKnown = intrinsic != nullptr;
  auto shape =
      this->checkIntrinsicShape(call, name, intrinsicName, isKnown, isKnown ? intrinsic->args : 0,
                                isKnown ? intrinsic->typeArgs : 0);
  if (shape.isErr()) {
    this->state.getDiagnostics().error(name->loc(), shape.error().message);
    for (Expr *&arg : call->args) {
      arg = this->exprChecker->checkExpr(arg);
    }
    return call;
  }
  const Type *typeArg =
      intrinsic->typeArgs != 0 ? this->typeResolver->resolveType(name->typeArgs[0]) : nullptr;
  call->target = BuiltinCall{.builtin = intrinsic->builtin};
  IntrinsicCall made{
      .call = call,
      .name = name,
      .intrinsicName = intrinsicName,
      .typeArg = typeArg,
  };

  return this->checkIntrinsic(made, intrinsic->intrinsic);
}

Expr *LibraryCalls::checkIntrinsic(const IntrinsicCall &made, LibraryIntrinsic intrinsic) {
  CallExpr *call = made.call;
  switch (intrinsic) {
    case LibraryIntrinsic::AS_LISTENER:
      return this->checkListenerIntrinsic(made, ListenerBinding::PLAIN);
    case LibraryIntrinsic::AS_BOUND_LISTENER:
      return this->checkListenerIntrinsic(made, ListenerBinding::BOUND);
    case LibraryIntrinsic::AS_CLASS:
      return this->checkAsClass(made);
    case LibraryIntrinsic::AHEAD:
    case LibraryIntrinsic::AHEAD_MOVABLE:
      return this->checkAhead(made);
    case LibraryIntrinsic::ORDER:
      call->type = Type::voidTy();
      return call;
    case LibraryIntrinsic::PANIC:
      call->args[0] = this->argumentChecker->checkArg(call->args[0], Type::string());
      call->type = Type::voidTy();
      return call;
    case LibraryIntrinsic::DEFER:
      call->args[0] = this->argumentChecker->checkArg(
          call->args[0], Type::function({}, 0, false, Type::voidTy(), false));
      call->type = Type::anyObject();
      return call;
    case LibraryIntrinsic::WITH_CONTEXT:
      return this->checkWithContext(made);
    case LibraryIntrinsic::CHUNK_OF:
    case LibraryIntrinsic::IS_CHUNK:
    case LibraryIntrinsic::CHUNK_KIND:
    case LibraryIntrinsic::TYPE_NAME:
    case LibraryIntrinsic::OVERRIDES:
    case LibraryIntrinsic::FS_OPEN:
    case LibraryIntrinsic::FS_READ:
    case LibraryIntrinsic::FS_WRITE:
    case LibraryIntrinsic::FS_CLOSE:
    case LibraryIntrinsic::STDIN_CHUNK:
    case LibraryIntrinsic::STDIN_FAILED:
    case LibraryIntrinsic::STD_WRITE:
      return this->streamChecker->checkStreamIntrinsic(call, made.name, made.typeArg);
  }

  return call;
}

Expr *LibraryCalls::checkListenerIntrinsic(const IntrinsicCall &made, ListenerBinding binding) {
  CallExpr *call = made.call;
  const auto *functionType = this->flatListenerType(made.typeArg);
  call->args[0] = this->argumentChecker->checkArg(call->args[0], Type::unknown());
  if (binding == ListenerBinding::BOUND) {
    call->args[1] = this->argumentChecker->checkArg(call->args[1], Type::anyObject());
  }
  call->type = functionType;
  if (!functionType->isFunction() && !functionType->isParam() && !functionType->isError()) {
    auto message = ErrorTextLibraryCall::listener();
    this->state.getDiagnostics().error(made.name->loc(), message);
  }

  return call;
}

const Type *LibraryCalls::flatListenerType(const Type *typeArg) const {
  auto flat = std::vector<const Type *>();
  size_t minArgs = 0;
  bool hasRest = false;
  bool isFlattened = typeArg->isFunction() &&
                     ArgumentLists::flattenArguments(typeArg->params(), minArgs, hasRest, flat);
  if (!isFlattened) {
    return typeArg;
  }

  return Type::function(flat, minArgs, hasRest, typeArg->ret(), typeArg->hasThis());
}

Expr *LibraryCalls::checkAsClass(const IntrinsicCall &made) {
  CallExpr *call = made.call;
  const Type *typeArg = made.typeArg;
  call->args[0] = this->exprChecker->checkExpr(call->args[0]);
  if (!typeArg->isClass() && !typeArg->isParam() && !typeArg->isError()) {
    auto message = ErrorTextLibraryCall::asClass();
    this->state.getDiagnostics().error(made.name->loc(), message);
  }
  call->type = Type::optional(typeArg);

  return call;
}

Expr *LibraryCalls::checkAhead(const IntrinsicCall &made) {
  CallExpr *call = made.call;
  call->args[0] = this->exprChecker->checkExpr(call->args[0]);
  call->type = call->args[0]->type;
  if (!this->isAwaitable(call->type)) {
    auto message = ErrorTextLibraryCall::ahead(made.intrinsicName);
    this->state.getDiagnostics().error(made.name->loc(), message);
  }
  auto *current = this->state.getFunctionStack().getCurrentFunction();
  if (current != nullptr && current->decl != nullptr) {
    current->decl->awaitsAhead = true;
  }

  return call;
}

bool LibraryCalls::isAwaitable(const Type *type) const {
  if (type->isError() || type->isPromise()) {
    return true;
  }
  if (!type->isUnion()) {
    return false;
  }
  for (const Type *member : type->members()) {
    if (member->isPromise()) {
      return true;
    }
  }

  return false;
}

Expr *LibraryCalls::checkWithContext(const IntrinsicCall &made) {
  CallExpr *call = made.call;
  call->args[0] = this->argumentChecker->checkArg(call->args[0], Type::anyObject());
  call->args[1] = this->exprChecker->checkExpr(call->args[1]);
  const Type *body = call->args[1]->type;
  call->type = body->isFunction() && body->params().empty() ? body->ret() : Type::error();
  if (call->type->isError() && !body->isError()) {
    auto message = ErrorTextLibraryCall::context();
    this->state.getDiagnostics().error(made.name->loc(), message);
  }

  return call;
}

}
