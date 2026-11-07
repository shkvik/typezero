#include "compiler/pipeline/check_program/topics/stream/stream_sources/stream_sources.h"
#include "compiler/pipeline/check_program/declare/prelude/prelude.h"
#include "compiler/pipeline/check_program/declare/type_declarer/type_declarer.h"
#include "compiler/pipeline/check_program/topics/events/event_target_checker/event_target_checker.h"
#include "compiler/pipeline/check_program/topics/expr/argument_checker/argument_checker.h"
#include "compiler/pipeline/check_program/topics/expr/call_checker/call_checker.h"
#include "compiler/pipeline/check_program/topics/expr/expr_checker/expr_checker.h"
#include "compiler/pipeline/check_program/topics/iterators/iterable_checker/iterable_checker.h"
#include "compiler/pipeline/check_program/topics/stream/set_encoding_checker/set_encoding_checker.h"
#include "compiler/pipeline/check_program/topics/stream/stream_pipeline/stream_pipeline.h"
#include "compiler/pipeline/check_program/topics/tz_modules/library_calls/library_calls.h"
#include "compiler/pipeline/check_program/types/type_relations/type_relations.h"
#include "llvm/ADT/STLExtras.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

StreamSources::StreamSources(CheckState &owner)
    : state(owner) {}

Expr *StreamSources::checkFsStreamFunction(CallExpr *call, FunctionDecl *decl, llvm::SMLoc loc) {
  llvm::StringRef fn = decl->name;
  bool reading = fn == "createReadStream";
  if (!reading && fn != "createWriteStream") {
    return nullptr;
  }
  if (LibraryCalls(this->state).inLibrary(call->loc())) {
    return nullptr;
  }
  call->type = Type::error();
  bool rejected = CallChecker(this->state).rejectSpread(call);
  ArgumentChecker::ResultArityMatches arity =
      rejected ? ok(call) : ArgumentChecker(this->state).checkArity(call, 1, 2);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(loc, arity.error().message);
  }
  if (rejected || arity.isErr()) {
    return call;
  }
  TypeAliasDecl *alias = this->state.getLibraryDeclarations().findLibraryType(
      reading ? "ReadStreamOptions" : "WriteStreamOptions");
  if (!alias) {
    return call;
  }
  const Type *options = Type::optional(TypeDeclarer(this->state).resolveAlias(alias));
  const Type *encodingType = Type::optional(Prelude(this->state).builtinType("BufferEncoding"));
  auto undefinedOf = [&](const Type *type) {
    return TypeRelations(this->state)
        .convert(ExprChecker(this->state).checkExpr(this->state.getAst().make<UndefinedExpr>(loc)),
                 type);
  };
  Expr *path = ArgumentChecker(this->state).checkArg(call->args[0], Type::string());
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
      opts = ArgumentChecker(this->state).checkArg(arg, options);
    } else if (isa<StringLitExpr>(arg)) {
      encoding = ArgumentChecker(this->state).checkArg(arg, encodingType);
      text = true;
    } else {
      arg = ExprChecker(this->state).checkExpr(arg);
      if (arg->type->isSubtypeOf(Type::string())) {
        encoding = TypeRelations(this->state).coerce(arg, encodingType);
        text = true;
      } else {
        opts = TypeRelations(this->state).coerce(arg, options);
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
    return EventTargetChecker(this->state)
        .callLibraryFunction(call, fresh ? "streamWriteNewFile" : "streamWriteFile", {}, given,
                             loc);
  }
  const Type *chunk = text ? Type::string() : Type::bytesType(Type::Bytes::Buffer);

  return EventTargetChecker(this->state)
      .callLibraryFunction(call, "streamReadFile", {chunk}, given, loc);
}

Expr *StreamSources::processStream(MemberExpr *member) {
  llvm::StringRef name = member->name;
  bool in = name == "stdin";
  llvm::StringRef fn = in ? "processStdin" : "processStdout";
  if (!this->state.getLibraryDeclarations().findLibraryFunction(fn)) {
    return nullptr;
  }
  llvm::SMLoc loc = member->loc();
  std::vector<Expr *> args;
  if (!in) {
    args.push_back(
        ExprChecker(this->state)
            .checkExpr(this->state.getAst().make<IntLitExpr>(loc, name == "stdout" ? 1 : 2),
                       Type::intTy()));
  }
  auto *callee = this->state.getAst().make<NameExpr>(loc, fn);
  auto *call = this->state.getAst().make<CallExpr>(loc, callee, std::move(args));
  call->type = Type::error();
  std::vector<bool> given(call->args.size(), true);
  Expr *stream = EventTargetChecker(this->state).callLibraryFunction(call, fn, {}, given, loc);
  if (in && this->state.getStreamStore().hasStreamViews()) {
    FieldPath path;
    path.var = this->processStdinVar();
    if (const Type *view = this->state.getStreamStore().findStreamView(path, loc)) {
      return TypeRelations(this->state).convert(stream, view);
    }
  }

  return stream;
}

VarSymbol *StreamSources::processStdinVar() {
  StreamStore &streams = this->state.getStreamStore();
  if (!streams.getProcessStdin()) {
    VarSymbol *stdinVar = this->state.getAst().newVar();
    stdinVar->name = "process.stdin";
    stdinVar->isConst = true;
    streams.rememberProcessStdin(stdinVar);
  }

  return streams.getProcessStdin();
}

const Type *StreamSources::streamFromOptionsType() {
  TypeAliasDecl *alias =
      this->state.getLibraryDeclarations().findLibraryType("ReadableFromOptions");

  return alias ? TypeDeclarer(this->state).resolveAlias(alias) : nullptr;
}

StreamSources::ResultValuesIterator StreamSources::valuesIterator(Expr *source) {
  Expr *iterator = StreamPipeline(this->state).valuesIteratorOf(source);
  if (!iterator) {
    std::string message = ErrorTextStreamFrom::source(source->type->name());

    return err(StreamSourcesValuesIteratorError{
        .code = StreamSourcesValuesIteratorErrorCode::NOT_ITERABLE,
        .message = message,
    });
  }

  return ok(iterator);
}

Expr *StreamSources::checkStreamFrom(CallExpr *call, MemberExpr *member) {
  call->type = Type::error();
  bool rejected = CallChecker(this->state).rejectSpread(call);
  ArgumentChecker::ResultArityMatches arity =
      rejected ? ok(call) : ArgumentChecker(this->state).checkArity(call, 1, 2);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(member->loc(), arity.error().message);
  }
  if (rejected || arity.isErr()) {
    return call;
  }
  llvm::SMLoc loc = member->loc();
  Expr *source = ExprChecker(this->state).checkExpr(call->args[0]);
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
          ? ArgumentChecker(this->state).checkArg(call->args[1], Type::optional(optionsType))
          : TypeRelations(this->state)
                .convert(ExprChecker(this->state)
                             .checkExpr(this->state.getAst().make<UndefinedExpr>(loc)),
                         Type::optional(optionsType));
  if (options->type->isError()) {
    return call;
  }
  std::vector<bool> given{true, true};
  if (type->isString() || type->isStringLiteral() || type->isBuffer()) {
    const Type *chunk = type->isBuffer() ? type : Type::string();
    call->args = {TypeRelations(this->state).coerce(source, chunk), options};
    return EventTargetChecker(this->state)
        .callLibraryFunction(call, "streamFromValue", {chunk}, given, loc);
  }
  if (type->isAsyncIterator()) {
    call->args = {source, options};
    return EventTargetChecker(this->state)
        .callLibraryFunction(call, "streamFromAsyncIterator", {type->yielded()}, given, loc);
  }
  if (Expr *async = IterableChecker(this->state).asyncUserIterator(source);
      async && async->type->isAsyncIterator()) {
    call->args = {async, options};
    return EventTargetChecker(this->state)
        .callLibraryFunction(call, "streamFromAsyncIterator", {async->type->yielded()}, given, loc);
  }
  ResultValuesIterator values = this->valuesIterator(source);
  if (values.isErr()) {
    this->state.getDiagnostics().error(source->loc(), values.error().message);
    return call;
  }
  Expr *iterator = values.value();
  call->args = {iterator, options};

  return EventTargetChecker(this->state)
      .callLibraryFunction(call, "streamFromIterator", {iterator->type->yielded()}, given, loc);
}

}
