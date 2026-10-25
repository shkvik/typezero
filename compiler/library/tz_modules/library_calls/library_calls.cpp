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
    // Their code finds a listener by what it is: every evaluation of a
    // function expression must give a function of its own.
    program.comparesFunctions = true;
    llvm::SourceMgr &sm = checker_.diags_.sourceMgr();
    for (unsigned id : module->buffers) {
      const llvm::MemoryBuffer *buffer = sm.getMemoryBuffer(id);
      libraryRanges_.push_back({buffer->getBufferStart(), buffer->getBufferEnd()});
      // Its warnings are not the program's author's to fix.
      checker_.diags_.quietRange(buffer->getBufferStart(), buffer->getBufferEnd());
    }
    for (Stmt *item : module->file.items) {
      if (auto *fn = dyn_cast<FunctionDecl>(item)) {
        checker_.libFunctions_[fn->name] = fn;
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
      // `@__tz_hidden x`: a field that is no key.
      for (ClassField &f : cls->fields) {
        llvm::erase_if(f.decorators, [&](const Decorator &d) {
          auto *name = dyn_cast<NameExpr>(d.expr);
          if (!name || name->name != "__tz_hidden") {
            return false;
          }
          f.hidden = true;

          return true;
        });
      }
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
  if (!name->name.starts_with("__tz_")) {
    return nullptr;
  }
  call->type = Type::error();
  auto checkArgsLoosely = [&] {
    for (Expr *&arg : call->args) {
      arg = checker_.exprChecker().checkExpr(arg);
    }
  };
  // A name of the program's own is that; anywhere else but the built-in
  // modules, these are no names.
  if (Checker::ScopeEntry own = checker_.scopes().lookup(name->name);
      own.var || own.func || own.cls) {
    return nullptr;
  }
  if (!inLibrary(call->loc())) {
    checker_.diags_.error(name->loc(), "cannot find name '" + name->name + "'");
    checkArgsLoosely();
    return call;
  }
  if (name->name.starts_with("__tz_subtle_")) {
    return checker_.webCryptoChecker().checkSubtleIntrinsic(call, name);
  }
  struct Intrinsic {
    llvm::StringLiteral name;
    Builtin builtin;
    unsigned args, typeArgs;
  };
  // Those that take a type; the others are declared by the code that
  // calls them (sema_library.cpp).
  static const Intrinsic intrinsics[] = {{"__tz_listener", Builtin::LibListener, 1, 1},
                                         {"__tz_as", Builtin::LibAs, 1, 1},
                                         {"__tz_chunk", Builtin::LibChunk, 1, 1},
                                         {"__tz_isChunk", Builtin::LibIsChunk, 1, 1},
                                         {"__tz_chunkKind", Builtin::LibChunkKind, 0, 1},
                                         {"__tz_typeName", Builtin::LibTypeName, 0, 1},
                                         {"__tz_overrides", Builtin::LibOverrides, 2, 0},
                                         {"__tz_fsOpen", Builtin::LibFsOpen, 3, 0},
                                         {"__tz_fsRead", Builtin::LibFsRead, 5, 0},
                                         {"__tz_fsWrite", Builtin::LibFsWrite, 5, 0},
                                         {"__tz_fsClose", Builtin::LibFsClose, 2, 0},
                                         {"__tz_stdinChunk", Builtin::LibStdinChunk, 1, 0},
                                         {"__tz_stdWrite", Builtin::LibStdWrite, 2, 0},
                                         {"__tz_ahead", Builtin::LibAhead, 1, 0},
                                         {"__tz_order", Builtin::LibOrder, 0, 0}};
  const Intrinsic *intrinsic =
      llvm::find_if(intrinsics, [&](const Intrinsic &i) { return i.name == name->name; });
  if (intrinsic == std::end(intrinsics) || call->args.size() != intrinsic->args ||
      name->typeArgs.size() != intrinsic->typeArgs || call->hasSpread()) {
    checker_.diags_.error(name->loc(),
                          "'" + name->name + "' is no intrinsic of tz, or is called wrongly");
    checkArgsLoosely();
    return call;
  }
  const Type *typeArg =
      intrinsic->typeArgs
          ? checker_.typeResolver().resolveType(name->typeArgs[0], /*allowVoid=*/false)
          : nullptr;
  call->builtin = intrinsic->builtin;
  switch (intrinsic->builtin) {
  case Builtin::LibListener: {
    // A function type whose last parameter is a tuple of arguments, `(...args:
    // A) => void` with A a tuple in a copy, is the function of those
    // parameters: the one a listener was converted from.
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
      checker_.diags_.error(name->loc(), "'__tz_listener' gives a function");
    }
    break;
  }
  case Builtin::LibAs:
    call->args[0] = checker_.exprChecker().checkExpr(call->args[0]);
    if (!typeArg->isClass() && !typeArg->isParam() && !typeArg->isError()) {
      checker_.diags_.error(name->loc(), "'__tz_as' takes a class");
    }
    call->type = Type::optional(typeArg);
    break;
  case Builtin::LibAhead:
    call->args[0] = checker_.exprChecker().checkExpr(call->args[0]);
    call->type = call->args[0]->type;
    if (!call->type->isPromise() && !call->type->isError()) {
      checker_.diags_.error(name->loc(), "'__tz_ahead' takes the promise an await waits for");
    }
    if (checker_.currentFunction_ && checker_.currentFunction_->decl) {
      checker_.currentFunction_->decl->awaitsAhead = true;
    }
    break;
  case Builtin::LibOrder:
    call->type = Type::voidTy();
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

} // namespace tz
