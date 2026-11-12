#include "compiler/semantic/library/stream/stream_sources/stream_sources.h"
#include "compiler/program/library/builtin_modules/node_option_table/node_option_table.h"
#include "compiler/program/library/builtin_names/library_function_table/library_function_table.h"
#include "compiler/program/library/fs/fs_stream_function_table/fs_stream_function_table.h"
#include "compiler/program/library/process/process_function_table/process_function_table.h"
#include "compiler/program/library/process/process_member_table/process_member_table.h"
#include "compiler/program/library/process/process_qualified_name_table/process_qualified_name_table.h"
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

Expr *StreamSources::checkFsStreamFunction(CallExpr *call, FunctionDecl *functionDecl,
                                           llvm::SMLoc loc) {
  llvm::StringRef functionName = functionDecl->name;
  auto function = FsStreamFunctionTable::find(functionName);
  bool isReading = function == FsStreamFunction::CREATE_READ_STREAM;
  if (!function.has_value()) {
    return nullptr;
  }
  if (this->libraryCalls->isInLibrary(call->loc())) {
    return nullptr;
  }
  call->type = Type::error();
  if (!this->matchFsStreamArity(call, loc)) {
    return call;
  }
  auto options = isReading ? LibraryType::READ_STREAM_OPTIONS : LibraryType::WRITE_STREAM_OPTIONS;
  auto *alias = this->state.getLibraryDeclarations().findLibraryType(options);
  if (alias == nullptr) {
    return call;
  }
  FsStreamArgs args;
  args.options = Type::optional(this->typeDeclarer->resolveAlias(alias));
  args.encodingType = Type::optional(this->prelude->builtinType(PreludeType::BUFFER_ENCODING));
  auto *path = this->argumentChecker->checkArg(call->args[0], Type::string());
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

  return this->callFsStream(call, args, isReading, loc);
}

bool StreamSources::matchFsStreamArity(CallExpr *call, llvm::SMLoc loc) {
  bool isRejected = this->callChecker->rejectSpread(call);
  auto arity = isRejected ? ok(call) : this->argumentChecker->checkArity(call, 1, 2);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(loc, arity.error().message);
  }

  return !isRejected && arity.isOk();
}

void StreamSources::walkFsStreamOptions(Expr *arg, FsStreamArgs &args) {
  auto *literal = dyn_cast<ObjectLitExpr>(arg);
  if (literal != nullptr) {
    this->scanOptionsLiteral(literal, args);
    args.opts = this->argumentChecker->checkArg(arg, args.options);
    return;
  }
  if (isa<StringLitExpr>(arg)) {
    args.encoding = this->argumentChecker->checkArg(arg, args.encodingType);
    args.text = true;
    return;
  }
  auto *checked = this->exprChecker->checkExpr(arg);
  if (checked->type->isSubtypeOf(Type::string())) {
    args.encoding = this->typeRelations->coerce(checked, args.encodingType);
    args.text = true;
    return;
  }
  args.opts = this->typeRelations->coerce(checked, args.options);
}

void StreamSources::scanOptionsLiteral(const ObjectLitExpr *literal, FsStreamArgs &args) const {
  for (const ObjectLitExpr::Property &property : literal->props) {
    auto option = property.spread() ? std::nullopt : NodeOptionTable::find(property.name);
    if (option == NodeOption::ENCODING && !isa<UndefinedExpr>(property.value)) {
      args.text = true;
    }
    auto *flags = option == NodeOption::FLAGS ? dyn_cast<StringLitExpr>(property.value) : nullptr;
    if (flags != nullptr && flags->value.find(u'x') != std::u16string::npos) {
      args.fresh = true;
    }
  }
}

Expr *StreamSources::undefinedArg(const Type *type, llvm::SMLoc loc) {
  Expr *undefined = this->state.getAst().make<UndefinedExpr>(loc);
  auto *checked = this->exprChecker->checkExpr(undefined);

  return this->typeRelations->convert(checked, type);
}

Expr *StreamSources::callFsStream(CallExpr *call, const FsStreamArgs &args, bool reading,
                                  llvm::SMLoc loc) {
  std::vector<bool> given(3, true);
  if (!reading) {
    auto writer =
        args.fresh ? LibraryFunction::STREAM_WRITE_NEW_FILE : LibraryFunction::STREAM_WRITE_FILE;
    auto name = LibraryFunctionTable::getName(writer);

    return this->eventTargetChecker->callLibraryFunction(call, name, {}, given, loc);
  }
  const Type *chunk = args.text ? Type::string() : Type::bytesType(Type::Bytes::Buffer);

  auto reader = LibraryFunctionTable::getName(LibraryFunction::STREAM_READ_FILE);

  return this->eventTargetChecker->callLibraryFunction(call, reader, {chunk}, given, loc);
}

Expr *StreamSources::processStream(MemberExpr *member) {
  llvm::StringRef name = member->name;
  auto stream = ProcessMemberTable::find(name);
  bool isIn = stream == ProcessMember::STDIN;
  auto functionName =
      ProcessFunctionTable::getName(isIn ? ProcessFunction::STDIN : ProcessFunction::STDOUT);
  if (this->state.getLibraryDeclarations().findLibraryFunction(functionName) == nullptr) {
    return nullptr;
  }
  llvm::SMLoc loc = member->loc();
  auto args = std::vector<Expr *>();
  if (!isIn) {
    args.push_back(this->exprChecker->checkExpr(
        this->state.getAst().make<IntLitExpr>(loc, stream == ProcessMember::STDOUT ? 1 : 2),
        Type::intTy()));
  }
  auto *callee = this->state.getAst().make<NameExpr>(loc, functionName);
  auto *call = this->state.getAst().make<CallExpr>(loc, callee, std::move(args));
  call->type = Type::error();
  std::vector<bool> given(call->args.size(), true);
  auto *made = this->eventTargetChecker->callLibraryFunction(call, functionName, {}, given, loc);
  if (isIn && this->state.getStreamStore().hasStreamViews()) {
    FieldPath path;
    path.var = this->processStdinVar();
    const auto *view = this->state.getStreamStore().findStreamView(path, loc);
    if (view != nullptr) {
      return this->typeRelations->convert(made, view);
    }
  }

  return made;
}

VarSymbol *StreamSources::processStdinVar() {
  auto &streams = this->state.getStreamStore();
  if (streams.getProcessStdin() == nullptr) {
    auto *stdinVar = this->state.getAst().newVar();
    stdinVar->name = ProcessQualifiedNameTable::getQualifiedName(ProcessMember::STDIN);
    stdinVar->isConst = true;
    streams.rememberProcessStdin(stdinVar);
  }

  return streams.getProcessStdin();
}

const Type *StreamSources::streamFromOptionsType() {
  auto *alias =
      this->state.getLibraryDeclarations().findLibraryType(LibraryType::READABLE_FROM_OPTIONS);

  return alias != nullptr ? this->typeDeclarer->resolveAlias(alias) : nullptr;
}

StreamSources::ResultValuesIterator StreamSources::checkValuesIterator(Expr *source) {
  auto *iterator = this->streamPipeline->valuesIteratorOf(source);
  if (iterator == nullptr) {
    auto message = ErrorTextStreamFrom::source(source->type->name());

    return err(StreamSourcesValuesIteratorError{
        .code = StreamSourcesValuesIteratorErrorCode::NOT_ITERABLE,
        .message = message,
    });
  }

  return ok(iterator);
}

Expr *StreamSources::checkStreamFrom(CallExpr *call, MemberExpr *member) {
  call->type = Type::error();
  bool isRejected = this->callChecker->rejectSpread(call);
  auto arity = isRejected ? ok(call) : this->argumentChecker->checkArity(call, 1, 2);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(member->loc(), arity.error().message);
  }
  if (isRejected || arity.isErr()) {
    return call;
  }
  llvm::SMLoc loc = member->loc();
  auto *source = this->exprChecker->checkExpr(call->args[0]);
  const Type *type = source->type;
  if (type->isError()) {
    return call;
  }
  const auto *optionsType = this->streamFromOptionsType();
  if (optionsType == nullptr) {
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
    auto fromValue = LibraryFunctionTable::getName(LibraryFunction::STREAM_FROM_VALUE);
    return this->eventTargetChecker->callLibraryFunction(call, fromValue, {chunk}, given, loc);
  }
  if (type->isAsyncIterator()) {
    call->args = {source, options};
    auto fromAsync = LibraryFunctionTable::getName(LibraryFunction::STREAM_FROM_ASYNC_ITERATOR);
    return this->eventTargetChecker->callLibraryFunction(call, fromAsync, {type->yielded()}, given,
                                                         loc);
  }
  auto *async = this->iterableChecker->asyncUserIterator(source);
  if (async != nullptr && async->type->isAsyncIterator()) {
    call->args = {async, options};
    auto fromAsync = LibraryFunctionTable::getName(LibraryFunction::STREAM_FROM_ASYNC_ITERATOR);
    return this->eventTargetChecker->callLibraryFunction(call, fromAsync, {async->type->yielded()},
                                                         given, loc);
  }
  auto values = this->checkValuesIterator(source);
  if (values.isErr()) {
    this->state.getDiagnostics().error(source->loc(), values.error().message);
    return call;
  }
  Expr *iterator = values.value();
  call->args = {iterator, options};

  auto fromIterator = LibraryFunctionTable::getName(LibraryFunction::STREAM_FROM_ITERATOR);

  return this->eventTargetChecker->callLibraryFunction(call, fromIterator,
                                                       {iterator->type->yielded()}, given, loc);
}

}
