#include "compiler/library/stream/stream_sources/stream_sources.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/expr/call_checker/call_checker.h"
#include "compiler/check/expr/argument_checker/argument_checker.h"
#include "compiler/declare/type_declarer/type_declarer.h"
#include "compiler/check/iterators/iterable_checker/iterable_checker.h"
#include "compiler/library/tz_modules/library_calls/library_calls.h"
#include "compiler/library/events/event_target_checker/event_target_checker.h"
#include "compiler/library/stream/set_encoding_checker/set_encoding_checker.h"
#include "compiler/library/stream/stream_pipeline/stream_pipeline.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::dyn_cast_or_null;

namespace tz {

StreamSources::StreamSources(Sema &sema) : sema_(sema) {}

Expr *StreamSources::checkFsStreamFunction(CallExpr *call, FunctionDecl *decl, llvm::SMLoc loc) {
  llvm::StringRef fn = decl->name;
  bool reading = fn == "createReadStream";
  if (!reading && fn != "createWriteStream") {
    return nullptr;
  }
  if (sema_.libraryCalls().inLibrary(call->loc())) {
    return nullptr;
  }
  call->type = Type::error();
  if (sema_.callChecker().rejectSpread(call) || !sema_.argumentChecker().checkArity(call, loc, 1, 2)) {
    return call;
  }
  TypeAliasDecl *alias = sema_.libTypes_.lookup(reading ? "ReadStreamOptions" : "WriteStreamOptions");
  if (!alias) {
    return call;
  }
  const Type *options = Type::optional(sema_.typeDeclarer().resolveAlias(alias));
  const Type *encodingType = Type::optional(Type::string());
  auto undefinedOf = [&](const Type *type) {
    return sema_.typeRelations().convert(sema_.exprChecker().checkExpr(sema_.ctx_.make<UndefinedExpr>(loc)), type);
  };
  Expr *path = sema_.argumentChecker().checkArg(call->args[0], Type::string());
  Expr *opts = nullptr;
  Expr *encoding = nullptr;
  // The encoding gives strings: written in place of the options, or among
  // them, as a literal (docs/streams.md, section 2).
  bool text = false;
  if (call->args.size() == 2) {
    Expr *arg = call->args[1];
    if (isa<ObjectLitExpr>(arg)) {
      auto *lit = cast<ObjectLitExpr>(arg);
      for (const ObjectLitExpr::Property &prop : lit->props) {
        if (!prop.spread && prop.name == "encoding" && !isa<UndefinedExpr>(prop.value)) {
          text = true;
        }
      }
      opts = sema_.argumentChecker().checkArg(arg, options);
    } else {
      arg = sema_.exprChecker().checkExpr(arg);
      if (arg->type->isString() || arg->type->isStringLiteral()) {
        encoding = sema_.typeRelations().coerce(arg, encodingType);
        text = true;
      } else {
        opts = sema_.typeRelations().coerce(arg, options);
      }
    }
  }
  if (!opts) {
    opts = undefinedOf(options);
  }
  if (!encoding) {
    encoding = undefinedOf(encodingType);
  }
  if (path->type->isError() || opts->type->isError() || encoding->type->isError()) {
    return call;
  }
  call->args = {path, opts, encoding};
  std::vector<bool> given(3, true);
  if (!reading) {
    return sema_.eventTargetChecker().callLibraryFunction(call, "streamWriteFile", {}, given, loc);
  }
  const Type *chunk = text ? Type::string() : Type::bytesType(Type::Bytes::Buffer);

  return sema_.eventTargetChecker().callLibraryFunction(call, "streamReadFile", {chunk}, given, loc);
}

Expr *StreamSources::processStream(MemberExpr *member) {
  llvm::StringRef name = member->name;
  bool in = name == "stdin";
  llvm::StringRef fn = in ? "processStdin" : "processStdout";
  if (!sema_.libFunctions_.lookup(fn)) {
    return nullptr; // not used as a stream (see the loader)
  }
  llvm::SMLoc loc = member->loc();
  std::vector<Expr *> args;
  if (!in) {
    args.push_back(sema_.exprChecker().checkExpr(sema_.ctx_.make<IntLitExpr>(loc, name == "stdout" ? 1 : 2), Type::intTy()));
  }
  auto *callee = sema_.ctx_.make<NameExpr>(loc, fn);
  auto *call = sema_.ctx_.make<CallExpr>(loc, callee, std::move(args));
  call->type = Type::error();
  std::vector<bool> given(call->args.size(), true);
  Expr *stream = sema_.eventTargetChecker().callLibraryFunction(call, fn, {}, given, loc);
  // After process.stdin.setEncoding().
  if (in && !sema_.streamViews_.empty()) {
    Sema::FieldPath path;
    path.var = processStdinVar();
    if (const Type *view = sema_.setEncodingChecker().streamViewOf(path, loc)) {
      return sema_.typeRelations().convert(stream, view);
    }
  }

  return stream;
}

VarSymbol *StreamSources::processStdinVar() {
  if (!processStdin_) {
    processStdin_ = sema_.ctx_.newVar();
    processStdin_->name = "process.stdin";
    processStdin_->isConst = true;
  }

  return processStdin_;
}

const Type *StreamSources::streamFromOptionsType() {
  TypeAliasDecl *alias = sema_.libTypes_.lookup("ReadableFromOptions");

  return alias ? sema_.typeDeclarer().resolveAlias(alias) : nullptr;
}

Expr *StreamSources::checkStreamFrom(CallExpr *call, MemberExpr *member) {
  call->type = Type::error();
  if (sema_.callChecker().rejectSpread(call) || !sema_.argumentChecker().checkArity(call, member->loc(), 1, 2)) {
    return call;
  }
  llvm::SMLoc loc = member->loc();
  Expr *source = sema_.exprChecker().checkExpr(call->args[0]);
  const Type *type = source->type;
  if (type->isError()) {
    return call;
  }
  const Type *optionsType = streamFromOptionsType();
  if (!optionsType) {
    return call;
  }
  Expr *options = call->args.size() > 1 ? sema_.argumentChecker().checkArg(call->args[1], Type::optional(optionsType))
                                        : sema_.typeRelations().convert(sema_.exprChecker().checkExpr(sema_.ctx_.make<UndefinedExpr>(loc)),
                                                  Type::optional(optionsType));
  if (options->type->isError()) {
    return call;
  }
  std::vector<bool> given{true, true};
  // A string or a Buffer: one chunk.
  if (type->isString() || type->isStringLiteral() || type->isBuffer()) {
    const Type *chunk = type->isBuffer() ? type : Type::string();
    call->args = {sema_.typeRelations().coerce(source, chunk), options};
    return sema_.eventTargetChecker().callLibraryFunction(call, "streamFromValue", {chunk}, given, loc);
  }
  // An async iterable: an async generator, or an object with
  // `[Symbol.asyncIterator]()` (a stream among them).
  if (type->isAsyncIterator()) {
    call->args = {source, options};
    return sema_.eventTargetChecker().callLibraryFunction(call, "streamFromAsyncIterator", {type->yielded()}, given, loc);
  }
  if (Expr *async = sema_.iterableChecker().asyncUserIterator(source); async && async->type->isAsyncIterator()) {
    call->args = {async, options};
    return sema_.eventTargetChecker().callLibraryFunction(call, "streamFromAsyncIterator", {async->type->yielded()}, given,
                               loc);
  }
  // Any other iterable: an array, a Set, a generator...
  Expr *iterator = sema_.streamPipeline().valuesIteratorOf(source);
  if (!iterator) {
    sema_.diags_.error(source->loc(), "'Readable.from' takes an iterable, an async iterable, a string "
                                "or a Buffer, but this is '" +
                                    type->name() + "'");
    return call;
  }
  call->args = {iterator, options};

  return sema_.eventTargetChecker().callLibraryFunction(call, "streamFromIterator", {iterator->type->yielded()}, given, loc);
}

} // namespace tz
