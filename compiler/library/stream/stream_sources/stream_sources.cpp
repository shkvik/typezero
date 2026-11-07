#include "compiler/library/stream/stream_sources/stream_sources.h"
#include "compiler/check/expr/argument_checker/argument_checker.h"
#include "compiler/check/expr/call_checker/call_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/iterators/iterable_checker/iterable_checker.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/declare/prelude/prelude.h"
#include "compiler/declare/type_declarer/type_declarer.h"
#include "compiler/library/events/event_target_checker/event_target_checker.h"
#include "compiler/library/stream/set_encoding_checker/set_encoding_checker.h"
#include "compiler/library/stream/stream_pipeline/stream_pipeline.h"
#include "compiler/library/tz_modules/library_calls/library_calls.h"
#include "llvm/ADT/STLExtras.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

StreamSources::StreamSources(Checker &owner)
    : checker(owner) {}

Expr *StreamSources::checkFsStreamFunction(CallExpr *call, FunctionDecl *decl, llvm::SMLoc loc) {
  llvm::StringRef fn = decl->name;
  bool reading = fn == "createReadStream";
  if (!reading && fn != "createWriteStream") {
    return nullptr;
  }
  if (this->checker.libraryCalls().inLibrary(call->loc())) {
    return nullptr;
  }
  call->type = Type::error();
  bool rejected = this->checker.callChecker().rejectSpread(call);
  ArgumentChecker::ResultArityMatches arity =
      rejected ? ok(call) : this->checker.argumentChecker().checkArity(call, 1, 2);
  if (arity.isErr()) {
    this->checker.diags_.error(loc, arity.error().message);
  }
  if (rejected || arity.isErr()) {
    return call;
  }
  TypeAliasDecl *alias =
      this->checker.libTypes_.lookup(reading ? "ReadStreamOptions" : "WriteStreamOptions");
  if (!alias) {
    return call;
  }
  const Type *options = Type::optional(this->checker.typeDeclarer().resolveAlias(alias));
  const Type *encodingType = Type::optional(this->checker.prelude().builtinType("BufferEncoding"));
  auto undefinedOf = [&](const Type *type) {
    return this->checker.typeRelations().convert(
        this->checker.exprChecker().checkExpr(this->checker.ctx_.make<UndefinedExpr>(loc)), type);
  };
  Expr *path = this->checker.argumentChecker().checkArg(call->args[0], Type::string());
  Expr *opts = nullptr;
  Expr *encoding = nullptr;
  bool text = false;
  bool fresh = false;
  if (call->args.size() == 2) {
    Expr *arg = call->args[1];
    if (isa<ObjectLitExpr>(arg)) {
      auto *lit = cast<ObjectLitExpr>(arg);
      for (const ObjectLitExpr::Property &prop : lit->props) {
        if (!prop.spread() && prop.name == "encoding" && !isa<UndefinedExpr>(prop.value)) {
          text = true;
        }
        auto *flags =
            prop.spread() || prop.name != "flags" ? nullptr : dyn_cast<StringLitExpr>(prop.value);
        if (flags && flags->value.find(u'x') != std::u16string::npos) {
          fresh = true;
        }
      }
      opts = this->checker.argumentChecker().checkArg(arg, options);
    } else if (isa<StringLitExpr>(arg)) {
      encoding = this->checker.argumentChecker().checkArg(arg, encodingType);
      text = true;
    } else {
      arg = this->checker.exprChecker().checkExpr(arg);
      if (arg->type->isSubtypeOf(Type::string())) {
        encoding = this->checker.typeRelations().coerce(arg, encodingType);
        text = true;
      } else {
        opts = this->checker.typeRelations().coerce(arg, options);
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
    return this->checker.eventTargetChecker().callLibraryFunction(
        call, fresh ? "streamWriteNewFile" : "streamWriteFile", {}, given, loc);
  }
  const Type *chunk = text ? Type::string() : Type::bytesType(Type::Bytes::Buffer);

  return this->checker.eventTargetChecker().callLibraryFunction(call, "streamReadFile", {chunk},
                                                                given, loc);
}

Expr *StreamSources::processStream(MemberExpr *member) {
  llvm::StringRef name = member->name;
  bool in = name == "stdin";
  llvm::StringRef fn = in ? "processStdin" : "processStdout";
  if (!this->checker.libFunctions_.lookup(fn)) {
    return nullptr;
  }
  llvm::SMLoc loc = member->loc();
  std::vector<Expr *> args;
  if (!in) {
    args.push_back(this->checker.exprChecker().checkExpr(
        this->checker.ctx_.make<IntLitExpr>(loc, name == "stdout" ? 1 : 2), Type::intTy()));
  }
  auto *callee = this->checker.ctx_.make<NameExpr>(loc, fn);
  auto *call = this->checker.ctx_.make<CallExpr>(loc, callee, std::move(args));
  call->type = Type::error();
  std::vector<bool> given(call->args.size(), true);
  Expr *stream = this->checker.eventTargetChecker().callLibraryFunction(call, fn, {}, given, loc);
  if (in && !this->checker.streamViews_.empty()) {
    Checker::FieldPath path;
    path.var = this->processStdinVar();
    if (const Type *view = this->checker.setEncodingChecker().streamViewOf(path, loc)) {
      return this->checker.typeRelations().convert(stream, view);
    }
  }

  return stream;
}

VarSymbol *StreamSources::processStdinVar() {
  if (!this->processStdin) {
    this->processStdin = this->checker.ctx_.newVar();
    this->processStdin->name = "process.stdin";
    this->processStdin->isConst = true;
  }

  return this->processStdin;
}

const Type *StreamSources::streamFromOptionsType() {
  TypeAliasDecl *alias = this->checker.libTypes_.lookup("ReadableFromOptions");

  return alias ? this->checker.typeDeclarer().resolveAlias(alias) : nullptr;
}

Expr *StreamSources::checkStreamFrom(CallExpr *call, MemberExpr *member) {
  call->type = Type::error();
  bool rejected = this->checker.callChecker().rejectSpread(call);
  ArgumentChecker::ResultArityMatches arity =
      rejected ? ok(call) : this->checker.argumentChecker().checkArity(call, 1, 2);
  if (arity.isErr()) {
    this->checker.diags_.error(member->loc(), arity.error().message);
  }
  if (rejected || arity.isErr()) {
    return call;
  }
  llvm::SMLoc loc = member->loc();
  Expr *source = this->checker.exprChecker().checkExpr(call->args[0]);
  const Type *type = source->type;
  if (type->isError()) {
    return call;
  }
  const Type *optionsType = this->streamFromOptionsType();
  if (!optionsType) {
    return call;
  }
  Expr *options =
      call->args.size() > 1
          ? this->checker.argumentChecker().checkArg(call->args[1], Type::optional(optionsType))
          : this->checker.typeRelations().convert(
                this->checker.exprChecker().checkExpr(this->checker.ctx_.make<UndefinedExpr>(loc)),
                Type::optional(optionsType));
  if (options->type->isError()) {
    return call;
  }
  std::vector<bool> given{true, true};
  if (type->isString() || type->isStringLiteral() || type->isBuffer()) {
    const Type *chunk = type->isBuffer() ? type : Type::string();
    call->args = {this->checker.typeRelations().coerce(source, chunk), options};
    return this->checker.eventTargetChecker().callLibraryFunction(call, "streamFromValue", {chunk},
                                                                  given, loc);
  }
  if (type->isAsyncIterator()) {
    call->args = {source, options};
    return this->checker.eventTargetChecker().callLibraryFunction(call, "streamFromAsyncIterator",
                                                                  {type->yielded()}, given, loc);
  }
  if (Expr *async = this->checker.iterableChecker().asyncUserIterator(source);
      async && async->type->isAsyncIterator()) {
    call->args = {async, options};
    return this->checker.eventTargetChecker().callLibraryFunction(
        call, "streamFromAsyncIterator", {async->type->yielded()}, given, loc);
  }
  Expr *iterator = this->checker.streamPipeline().valuesIteratorOf(source);
  if (!iterator) {
    this->checker.diags_.error(source->loc(),
                               "'Readable.from' takes an iterable, an async iterable, a string "
                               "or a Buffer, but this is '" +
                                   type->name() + "'");
    return call;
  }
  call->args = {iterator, options};

  return this->checker.eventTargetChecker().callLibraryFunction(
      call, "streamFromIterator", {iterator->type->yielded()}, given, loc);
}

}
