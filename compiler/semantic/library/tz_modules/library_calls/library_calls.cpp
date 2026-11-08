#include "compiler/semantic/library/tz_modules/library_calls/library_calls.h"
#include "compiler/semantic/bodies/expr/argument_checker/argument_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/library/crypto/web_crypto_checker/web_crypto_checker.h"
#include "compiler/semantic/library/stream/stream_checker/stream_checker.h"
#include "compiler/semantic/library/tz_modules/tz_modules_checker/tz_modules_checker_errors.h"
#include "compiler/semantic/names/scopes/scopes.h"
#include "compiler/semantic/types/type_resolver/type_resolver.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Path.h"

using llvm::dyn_cast;

namespace tz {

LibraryCalls::LibraryCalls(SemanticState &owner)
    : state(owner) {}

void LibraryCalls::prepareLibraries(SourceFile &program) {
  for (Module *module : program.modules) {
    if (!module->builtin) {
      continue;
    }
    this->state.getInventoryStore().markComparesFunctions();
    llvm::SourceMgr &sm = this->state.getDiagnostics().sourceMgr();
    for (unsigned id : module->buffers) {
      const llvm::MemoryBuffer *buffer = sm.getMemoryBuffer(id);
      this->state.getLibraryDeclarations().addLibraryRange(buffer->getBufferStart(),
                                                           buffer->getBufferEnd());
      this->state.getDiagnostics().quietRange(buffer->getBufferStart(), buffer->getBufferEnd());
    }
    bool intrinsics = module->name == "tz:intrinsics";
    for (Stmt *item : module->file.items) {
      if (auto *fn = dyn_cast<FunctionDecl>(item)) {
        if (intrinsics) {
          if (fn->external()) {
            fn->bodySource = FunctionBodySource::INTRINSIC;
          }
        } else {
          this->state.getLibraryDeclarations().rememberLibraryFunction(fn);
        }
        continue;
      }
      if (auto *alias = dyn_cast<TypeAliasDecl>(item)) {
        this->state.getLibraryDeclarations().rememberLibraryType(alias);
        continue;
      }
      auto *cls = dyn_cast<ClassDecl>(item);
      if (!cls) {
        continue;
      }
      cls->origin = ClassOrigin::LIBRARY;
      this->state.getLibraryDeclarations().rememberLibraryClass(cls);
    }
  }
}

bool LibraryCalls::inLibrary(llvm::SMLoc loc) const {
  return this->state.getLibraryDeclarations().isInsideLibrary(loc);
}

LibraryCalls::ResultIntrinsicShape LibraryCalls::intrinsicShape(const CallExpr *call,
                                                                const NameExpr *name,
                                                                llvm::StringRef intrinsicName,
                                                                bool known, unsigned args,
                                                                unsigned typeArgs) {
  bool wrong =
      !known || call->args.size() != args || name->typeArgs.size() != typeArgs || call->hasSpread();
  if (wrong) {
    std::string message = ErrorTextIntrinsicCall::wrong(intrinsicName);

    return err(LibraryCallsIntrinsicShapeError{
        .code = LibraryCallsIntrinsicShapeErrorCode::NOT_AN_INTRINSIC,
        .message = message,
    });
  }

  return ok(call);
}

Expr *LibraryCalls::checkLibCall(CallExpr *call, NameExpr *name) {
  ScopeEntry own = Scopes(this->state).lookup(name->name);
  const FunctionDecl *decl = own.func() ? own.func()->decl : nullptr;
  if (!decl || !decl->intrinsic()) {
    return nullptr;
  }
  llvm::StringRef intrinsicName = decl->name;
  call->type = Type::error();
  auto checkArgsLoosely = [&] {
    for (Expr *&arg : call->args) {
      arg = ExprChecker(this->state).checkExpr(arg);
    }
  };
  if (intrinsicName.starts_with("subtle")) {
    return WebCryptoChecker(this->state).checkSubtleIntrinsic(call, name, intrinsicName);
  }
  struct Intrinsic {
    llvm::StringLiteral name;
    Builtin builtin;
    unsigned args, typeArgs;
  };
  static const Intrinsic intrinsics[] = {{"asListener", Builtin::LibListener, 1, 1},
                                         {"asBoundListener", Builtin::LibBoundListener, 2, 1},
                                         {"asClass", Builtin::LibAs, 1, 1},
                                         {"chunkOf", Builtin::LibChunk, 1, 1},
                                         {"isChunk", Builtin::LibIsChunk, 1, 1},
                                         {"chunkKind", Builtin::LibChunkKind, 0, 1},
                                         {"typeName", Builtin::LibTypeName, 0, 1},
                                         {"overrides", Builtin::LibOverrides, 2, 0},
                                         {"fsOpen", Builtin::LibFsOpen, 3, 1},
                                         {"fsRead", Builtin::LibFsRead, 5, 1},
                                         {"fsWrite", Builtin::LibFsWrite, 5, 1},
                                         {"fsClose", Builtin::LibFsClose, 2, 1},
                                         {"stdinChunk", Builtin::LibStdinChunk, 1, 0},
                                         {"stdWrite", Builtin::LibStdWrite, 2, 0},
                                         {"stdinFailed", Builtin::LibStdinFailed, 0, 0},
                                         {"ahead", Builtin::LibAhead, 1, 0},
                                         {"order", Builtin::LibOrder, 0, 0},
                                         {"panic", Builtin::LibPanic, 1, 0},
                                         {"defer", Builtin::LibDefer, 1, 0},
                                         {"withContext", Builtin::LibWithContext, 2, 0},
                                         {"aheadMovable", Builtin::LibAheadMovable, 1, 0}};
  auto named = [&](const Intrinsic &i) { return i.name == intrinsicName; };
  const Intrinsic *intrinsic = llvm::find_if(intrinsics, named);
  bool known = intrinsic != std::end(intrinsics);
  ResultIntrinsicShape shape =
      LibraryCalls::intrinsicShape(call, name, intrinsicName, known, known ? intrinsic->args : 0,
                                   known ? intrinsic->typeArgs : 0);
  if (shape.isErr()) {
    this->state.getDiagnostics().error(name->loc(), shape.error().message);
    checkArgsLoosely();
    return call;
  }
  const Type *typeArg = intrinsic->typeArgs
                            ? TypeResolver(this->state).resolveType(name->typeArgs[0], false)
                            : nullptr;
  call->target = BuiltinCall{.builtin = intrinsic->builtin};
  switch (intrinsic->builtin) {
  case Builtin::LibListener:
  case Builtin::LibBoundListener: {
    const Type *fn = typeArg;
    std::vector<const Type *> flat;
    size_t minArgs = 0;
    bool hasRest = false;
    if (fn->isFunction() && ArgumentLists::flattenArguments(fn->params(), minArgs, hasRest, flat)) {
      fn = Type::function(flat, minArgs, hasRest, fn->ret(), fn->hasThis());
    }
    call->args[0] = ArgumentChecker(this->state).checkArg(call->args[0], Type::unknown());
    if (intrinsic->builtin == Builtin::LibBoundListener) {
      call->args[1] = ArgumentChecker(this->state).checkArg(call->args[1], Type::anyObject());
    }
    call->type = fn;
    if (!fn->isFunction() && !fn->isParam() && !fn->isError()) {
      std::string message = ErrorTextLibraryCall::listener();
      this->state.getDiagnostics().error(name->loc(), message);
    }
    break;
  }
  case Builtin::LibAs: {
    call->args[0] = ExprChecker(this->state).checkExpr(call->args[0]);
    if (!typeArg->isClass() && !typeArg->isParam() && !typeArg->isError()) {
      std::string message = ErrorTextLibraryCall::asClass();
      this->state.getDiagnostics().error(name->loc(), message);
    }
    call->type = Type::optional(typeArg);
    break;
  }
  case Builtin::LibAhead:
  case Builtin::LibAheadMovable: {
    call->args[0] = ExprChecker(this->state).checkExpr(call->args[0]);
    call->type = call->args[0]->type;
    auto promised = [](const Type *m) { return m->isPromise(); };
    bool awaitable = call->type->isError() || call->type->isPromise() ||
                     (call->type->isUnion() && llvm::any_of(call->type->members(), promised));
    if (!awaitable) {
      std::string message = ErrorTextLibraryCall::ahead(intrinsicName);
      this->state.getDiagnostics().error(name->loc(), message);
    }
    if (this->state.getFunctionStack().getCurrentFunction() &&
        this->state.getFunctionStack().getCurrentFunction()->decl) {
      this->state.getFunctionStack().getCurrentFunction()->decl->awaitsAhead = true;
    }
    break;
  }
  case Builtin::LibOrder:
    call->type = Type::voidTy();
    break;
  case Builtin::LibPanic:
    call->args[0] = ArgumentChecker(this->state).checkArg(call->args[0], Type::string());
    call->type = Type::voidTy();
    break;
  case Builtin::LibDefer:
    call->args[0] =
        ArgumentChecker(this->state)
            .checkArg(call->args[0], Type::function({}, 0, false, Type::voidTy(), false));
    call->type = Type::anyObject();
    break;
  case Builtin::LibWithContext: {
    call->args[0] = ArgumentChecker(this->state).checkArg(call->args[0], Type::anyObject());
    call->args[1] = ExprChecker(this->state).checkExpr(call->args[1]);
    call->type = call->args[1]->type->isFunction() && call->args[1]->type->params().empty()
                     ? call->args[1]->type->ret()
                     : Type::error();
    if (call->type->isError() && !call->args[1]->type->isError()) {
      std::string message = ErrorTextLibraryCall::context();
      this->state.getDiagnostics().error(name->loc(), message);
    }
    break;
  }
  case Builtin::LibChunk:
  case Builtin::LibIsChunk:
  case Builtin::LibChunkKind:
  case Builtin::LibTypeName:
  case Builtin::LibOverrides:
  case Builtin::LibFsOpen:
  case Builtin::LibFsRead:
  case Builtin::LibFsWrite:
  case Builtin::LibFsClose:
  case Builtin::LibStdinChunk:
  case Builtin::LibStdinFailed:
  case Builtin::LibStdWrite:
    return StreamChecker(this->state).checkStreamIntrinsic(call, name, typeArg);
  default:
    break;
  }

  return call;
}

}
