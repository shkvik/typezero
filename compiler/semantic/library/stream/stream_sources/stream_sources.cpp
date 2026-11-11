#include "compiler/semantic/library/stream/stream_sources/stream_sources.h"
#include "compiler/semantic/bodies/expr/argument_checker/argument_checker.h"
#include "compiler/semantic/bodies/expr/call_checker/call_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/bodies/iterators/iterable_checker/iterable_checker.h"
#include "compiler/semantic/declare/prelude/prelude.h"
#include "compiler/semantic/declare/type_declarer/type_declarer.h"
#include "compiler/semantic/library/events/event_target_checker/event_target_checker.h"
#include "compiler/semantic/library/stream/set_encoding_checker/set_encoding_checker.h"
#include "compiler/semantic/library/stream/stream_pipeline/stream_pipeline.h"
#include "compiler/semantic/library/tz_modules/library_calls/library_calls.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "llvm/ADT/STLExtras.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

StreamSources::StreamSources(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->prelude = &all.prelude;
  this->callChecker = &all.callChecker;
  this->exprChecker = &all.exprChecker;
  this->libraryCalls = &all.libraryCalls;
  this->typeDeclarer = &all.typeDeclarer;
  this->typeRelations = &all.typeRelations;
  this->streamPipeline = &all.streamPipeline;
  this->argumentChecker = &all.argumentChecker;
  this->iterableChecker = &all.iterableChecker;
  this->eventTargetChecker = &all.eventTargetChecker;
}

Expr *StreamSources::checkFsStreamFunction(CallExpr *call, FunctionDecl *decl, llvm::SMLoc loc) {
  llvm::StringRef fn = decl->name;
  bool reading = fn == "createReadStream";
  if (!reading && fn != "createWriteStream") {
    return nullptr;
  }
  if (this->libraryCalls->inLibrary(call->loc())) {
    return nullptr;
  }
  call->type = Type::error();
  if (!this->fsStreamArityFits(call, loc)) {
    return call;
  }
  TypeAliasDecl *alias = this->state.getLibraryDeclarations().findLibraryType(
      reading ? "ReadStreamOptions" : "WriteStreamOptions");
  if (alias == nullptr) {
    return call;
  }
  FsStreamArgs args;
  args.options = Type::optional(this->typeDeclarer->resolveAlias(alias));
  args.encodingType = Type::optional(this->prelude->builtinType("BufferEncoding"));
  Expr *path = this->argumentChecker->checkArg(call->args[0], Type::string());
  if (call->args.size() == 2) {
    this->walkFsStreamOptions(call->args[1], args);
  }
  if (args.opts == nullptr) {
    args.opts = this->undefinedArg(args.options, loc);
  }
  if (args.encoding == nullptr) {
    args.encoding = this->undefinedArg(args.encodingType, loc);
  }
  if (path->type->isError() || args.opts->type->isError() || args.encoding->type->isError()) {
    return call;
  }
  call->args = {path, args.opts, args.encoding};

  return this->callFsStream(call, args, reading, loc);
}

bool StreamSources::fsStreamArityFits(CallExpr *call, llvm::SMLoc loc) {
  bool rejected = this->callChecker->rejectSpread(call);
  ArgumentChecker::ResultArityMatches arity =
      rejected ? ok(call) : this->argumentChecker->checkArity(call, 1, 2);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(loc, arity.error().message);
  }

  return !rejected && arity.isOk();
}

void StreamSources::walkFsStreamOptions(Expr *arg, FsStreamArgs &args) {
  if (isa<ObjectLitExpr>(arg)) {
    StreamSources::scanOptionsLiteral(cast<ObjectLitExpr>(arg), args);
    args.opts = this->argumentChecker->checkArg(arg, args.options);
    return;
  }
  if (isa<StringLitExpr>(arg)) {
    args.encoding = this->argumentChecker->checkArg(arg, args.encodingType);
    args.text = true;
    return;
  }
  Expr *checked = this->exprChecker->checkExpr(arg);
  if (checked->type->isSubtypeOf(Type::string())) {
    args.encoding = this->typeRelations->coerce(checked, args.encodingType);
    args.text = true;
    return;
  }
  args.opts = this->typeRelations->coerce(checked, args.options);
}

void StreamSources::scanOptionsLiteral(const ObjectLitExpr *lit, FsStreamArgs &args) {
  for (const ObjectLitExpr::Property &prop : lit->props) {
    if (!prop.spread() && prop.name == "encoding" && !isa<UndefinedExpr>(prop.value)) {
      args.text = true;
    }
    auto *flags =
        prop.spread() || prop.name != "flags" ? nullptr : dyn_cast<StringLitExpr>(prop.value);
    if (flags != nullptr && flags->value.find(u'x') != std::u16string::npos) {
      args.fresh = true;
    }
  }
}

Expr *StreamSources::undefinedArg(const Type *type, llvm::SMLoc loc) {
  Expr *undefined = this->state.getAst().make<UndefinedExpr>(loc);
  Expr *checked = this->exprChecker->checkExpr(undefined);

  return this->typeRelations->convert(checked, type);
}

Expr *StreamSources::callFsStream(CallExpr *call, const FsStreamArgs &args, bool reading,
                                  llvm::SMLoc loc) {
  std::vector<bool> given(3, true);
  if (!reading) {
    llvm::StringRef name = args.fresh ? "streamWriteNewFile" : "streamWriteFile";

    return this->eventTargetChecker->callLibraryFunction(call, name, {}, given, loc);
  }
  const Type *chunk = args.text ? Type::string() : Type::bytesType(Type::Bytes::Buffer);

  return this->eventTargetChecker->callLibraryFunction(call, "streamReadFile", {chunk}, given, loc);
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
    args.push_back(this->exprChecker->checkExpr(
        this->state.getAst().make<IntLitExpr>(loc, name == "stdout" ? 1 : 2), Type::intTy()));
  }
  auto *callee = this->state.getAst().make<NameExpr>(loc, fn);
  auto *call = this->state.getAst().make<CallExpr>(loc, callee, std::move(args));
  call->type = Type::error();
  std::vector<bool> given(call->args.size(), true);
  Expr *stream = this->eventTargetChecker->callLibraryFunction(call, fn, {}, given, loc);
  if (in && this->state.getStreamStore().hasStreamViews()) {
    FieldPath path;
    path.var = this->processStdinVar();
    if (const Type *view = this->state.getStreamStore().findStreamView(path, loc)) {
      return this->typeRelations->convert(stream, view);
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

  return alias ? this->typeDeclarer->resolveAlias(alias) : nullptr;
}

StreamSources::ResultValuesIterator StreamSources::valuesIterator(Expr *source) {
  Expr *iterator = this->streamPipeline->valuesIteratorOf(source);
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
  bool rejected = this->callChecker->rejectSpread(call);
  ArgumentChecker::ResultArityMatches arity =
      rejected ? ok(call) : this->argumentChecker->checkArity(call, 1, 2);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(member->loc(), arity.error().message);
  }
  if (rejected || arity.isErr()) {
    return call;
  }
  llvm::SMLoc loc = member->loc();
  Expr *source = this->exprChecker->checkExpr(call->args[0]);
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
          ? this->argumentChecker->checkArg(call->args[1], Type::optional(optionsType))
          : this->typeRelations->convert(
                this->exprChecker->checkExpr(this->state.getAst().make<UndefinedExpr>(loc)),
                Type::optional(optionsType));
  if (options->type->isError()) {
    return call;
  }
  std::vector<bool> given{true, true};
  if (type->isString() || type->isStringLiteral() || type->isBuffer()) {
    const Type *chunk = type->isBuffer() ? type : Type::string();
    call->args = {this->typeRelations->coerce(source, chunk), options};
    return this->eventTargetChecker->callLibraryFunction(call, "streamFromValue", {chunk}, given,
                                                         loc);
  }
  if (type->isAsyncIterator()) {
    call->args = {source, options};
    return this->eventTargetChecker->callLibraryFunction(call, "streamFromAsyncIterator",
                                                         {type->yielded()}, given, loc);
  }
  if (Expr *async = this->iterableChecker->asyncUserIterator(source);
      async && async->type->isAsyncIterator()) {
    call->args = {async, options};
    return this->eventTargetChecker->callLibraryFunction(call, "streamFromAsyncIterator",
                                                         {async->type->yielded()}, given, loc);
  }
  ResultValuesIterator values = this->valuesIterator(source);
  if (values.isErr()) {
    this->state.getDiagnostics().error(source->loc(), values.error().message);
    return call;
  }
  Expr *iterator = values.value();
  call->args = {iterator, options};

  return this->eventTargetChecker->callLibraryFunction(call, "streamFromIterator",
                                                       {iterator->type->yielded()}, given, loc);
}

}
