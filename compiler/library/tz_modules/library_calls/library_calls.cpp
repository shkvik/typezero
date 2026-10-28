#include "compiler/library/tz_modules/library_calls/library_calls.h"
#include "compiler/check/expr/argument_checker/argument_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/scopes/scopes.h"
#include "compiler/check/types/type_resolver/type_resolver.h"
#include "compiler/library/crypto/web_crypto_checker/web_crypto_checker.h"
#include "compiler/library/stream/stream_checker/stream_checker.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/Path.h"
#include <cassert>

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

LibraryCalls::LibraryCalls(Checker &checker) : checker_(checker) {}

void LibraryCalls::prepareLibraries(SourceFile &program) {
  for (Module *module : program.modules) {
    if (!module->builtin) {
      continue;
    }
    program.comparesFunctions = true;
    llvm::SourceMgr &sm = checker_.diags_.sourceMgr();
    for (unsigned id : module->buffers) {
      const llvm::MemoryBuffer *buffer = sm.getMemoryBuffer(id);
      libraryRanges_.push_back({buffer->getBufferStart(), buffer->getBufferEnd()});
      checker_.diags_.quietRange(buffer->getBufferStart(), buffer->getBufferEnd());
    }
    bool intrinsics = module->name == "tz:intrinsics";
    for (Stmt *item : module->file.items) {
      if (auto *fn = dyn_cast<FunctionDecl>(item)) {
        if (intrinsics) {
          fn->intrinsic = fn->external;
        } else {
          checker_.libFunctions_[fn->name] = fn;
        }
        continue;
      }
      if (auto *alias = dyn_cast<TypeAliasDecl>(item)) {
        checker_.libTypes_[alias->name] = alias;
        continue;
      }
      auto *cls = dyn_cast<ClassDecl>(item);
      if (!cls) {
        continue;
      }
      cls->library = true;
      checker_.libClasses_[cls->name] = cls;
    }
  }
}

bool LibraryCalls::inLibrary(llvm::SMLoc loc) const {
  const char *at = loc.getPointer();

  return llvm::any_of(libraryRanges_, [&](const std::pair<const char *, const char *> &range) {
    return at >= range.first && at < range.second;
  });
}

Expr *LibraryCalls::checkLibCall(CallExpr *call, NameExpr *name) {
  Checker::ScopeEntry own = checker_.scopes().lookup(name->name);
  const FunctionDecl *decl = own.func ? own.func->decl : nullptr;
  if (!decl || !decl->intrinsic) {
    return nullptr;
  }
  llvm::StringRef intrinsicName = decl->name;
  call->type = Type::error();
  auto checkArgsLoosely = [&] {
    for (Expr *&arg : call->args) {
      arg = checker_.exprChecker().checkExpr(arg);
    }
  };
  if (intrinsicName.starts_with("subtle")) {
    return checker_.webCryptoChecker().checkSubtleIntrinsic(call, name, intrinsicName);
  }
  struct Intrinsic {
    llvm::StringLiteral name;
    Builtin builtin;
    unsigned args, typeArgs;
  };
  static const Intrinsic intrinsics[] = {
      {"asListener", Builtin::LibListener, 1, 1}, {"asClass", Builtin::LibAs, 1, 1},
      {"chunkOf", Builtin::LibChunk, 1, 1},       {"isChunk", Builtin::LibIsChunk, 1, 1},
      {"chunkKind", Builtin::LibChunkKind, 0, 1}, {"typeName", Builtin::LibTypeName, 0, 1},
      {"overrides", Builtin::LibOverrides, 2, 0}, {"fsOpen", Builtin::LibFsOpen, 3, 0},
      {"fsRead", Builtin::LibFsRead, 5, 0},       {"fsWrite", Builtin::LibFsWrite, 5, 0},
      {"fsClose", Builtin::LibFsClose, 2, 0},     {"stdinChunk", Builtin::LibStdinChunk, 1, 0},
      {"stdWrite", Builtin::LibStdWrite, 2, 0},   {"ahead", Builtin::LibAhead, 1, 0},
      {"order", Builtin::LibOrder, 0, 0},         {"panic", Builtin::LibPanic, 1, 0},
      {"defer", Builtin::LibDefer, 1, 0}};
  const Intrinsic *intrinsic =
      llvm::find_if(intrinsics, [&](const Intrinsic &i) { return i.name == intrinsicName; });
  if (intrinsic == std::end(intrinsics) || call->args.size() != intrinsic->args ||
      name->typeArgs.size() != intrinsic->typeArgs || call->hasSpread()) {
    checker_.diags_.error(name->loc(),
                          "'" + intrinsicName + "' is no intrinsic of tz, or is called wrongly");
    checkArgsLoosely();
    return call;
  }
  const Type *typeArg =
      intrinsic->typeArgs ? checker_.typeResolver().resolveType(name->typeArgs[0], false) : nullptr;
  call->builtin = intrinsic->builtin;
  switch (intrinsic->builtin) {
  case Builtin::LibListener: {
    const Type *fn = typeArg;
    std::vector<const Type *> flat;
    size_t minArgs = 0;
    bool hasRest = false;
    if (fn->isFunction() && flattenArguments(fn->params(), minArgs, hasRest, flat)) {
      fn = Type::function(flat, minArgs, hasRest, fn->ret(), fn->hasThis());
    }
    call->args[0] = checker_.argumentChecker().checkArg(call->args[0], Type::unknown());
    call->type = fn;
    if (!fn->isFunction() && !fn->isParam() && !fn->isError()) {
      checker_.diags_.error(name->loc(), "'asListener' gives a function");
    }
    break;
  }
  case Builtin::LibAs:
    call->args[0] = checker_.exprChecker().checkExpr(call->args[0]);
    if (!typeArg->isClass() && !typeArg->isParam() && !typeArg->isError()) {
      checker_.diags_.error(name->loc(), "'asClass' takes a class");
    }
    call->type = Type::optional(typeArg);
    break;
  case Builtin::LibAhead:
    call->args[0] = checker_.exprChecker().checkExpr(call->args[0]);
    call->type = call->args[0]->type;
    if (!call->type->isPromise() && !call->type->isError()) {
      checker_.diags_.error(name->loc(), "'ahead' takes the promise an await waits for");
    }
    if (checker_.currentFunction_ && checker_.currentFunction_->decl) {
      checker_.currentFunction_->decl->awaitsAhead = true;
    }
    break;
  case Builtin::LibOrder:
    call->type = Type::voidTy();
    break;
  case Builtin::LibPanic:
    call->args[0] = checker_.argumentChecker().checkArg(call->args[0], Type::string());
    call->type = Type::voidTy();
    break;
  case Builtin::LibDefer:
    call->args[0] = checker_.argumentChecker().checkArg(
        call->args[0], Type::function({}, 0, false, Type::voidTy(), false));
    call->type = Type::anyObject();
    break;
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
  case Builtin::LibStdWrite:
    return checker_.streamChecker().checkStreamIntrinsic(call, name, typeArg);
  default:
    break;
  }

  return call;
}

}
