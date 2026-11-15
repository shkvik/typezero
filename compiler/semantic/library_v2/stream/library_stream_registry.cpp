#include "compiler/semantic/library_v2/stream/library_stream_registry.h"
#include "compiler/program/ast_walker/ast_walker.h"
#include "compiler/program/library/array/array_method_table/array_method_table.h"
#include "compiler/program/library/builtin_names/library_class_table/library_class_table.h"
#include "compiler/program/library/builtin_names/library_function_table/library_function_table.h"
#include "compiler/program/library/builtin_names/library_method_table/library_method_table.h"
#include "compiler/program/library/builtin_names/library_type_table/library_type_table.h"
#include "compiler/program/library/events/emitter_member_table/emitter_member_table.h"
#include "compiler/program/library/fs/fs_stream_function_table/fs_stream_function_table.h"
#include "compiler/program/library/library_module_names/library_module_names.h"
#include "compiler/program/library/process/process_function_table/process_function_table.h"
#include "compiler/program/library/process/process_qualified_name_table/process_qualified_name_table.h"
#include "compiler/program/library/stream/stream_hook_table/stream_hook_table.h"
#include "compiler/program/library/stream/stream_kind_table/stream_kind_table.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include <functional>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

LibraryStreamRegistry::LibraryStreamRegistry(SemanticState &owner)
    : state(owner) {}

void LibraryStreamRegistry::addStreamBases(const AddStreamBases &task) {
  ClassDecl *instance = task.instance;
  ClassDecl *generic = instance->generic();
  auto *writable = this->state.library.declarations.findLibraryClass(LibraryClass::WRITABLE);
  bool isDuplex =
      generic != nullptr && LibraryClassTable::isName(generic->name, LibraryClass::DUPLEX);
  if (!isDuplex || !generic->library() || writable == nullptr || instance->typeArgs.size() != 3) {
    return;
  }
  auto &tasks = this->state.walk.tasks;
  auto args = std::vector<const Type *>{instance->typeArgs[0], instance->typeArgs[2]};
  tasks.pushTask(AfterStreamSide{.instance = instance});

  return tasks.pushTask(
      InstantiateClassType{.classDecl = writable, .args = std::move(args), .loc = task.loc});
}

void LibraryStreamRegistry::afterStreamSide(const AfterStreamSide &task) {
  ClassDecl *instance = task.instance;
  const Type *side = this->state.walk.tasks.takeAnswer();
  if (side->isError()) {
    return;
  }
  auto &subtypes = this->state.inventory.subtypes;
  bool isNew = !llvm::is_contained(subtypes, instance->type);
  if (instance->isEmitted() && isNew) {
    subtypes.push_back(instance->type);
  }

  return this->state.walk.tasks.pushTask(AddBase{.type = instance->type, .base = side});
}

void LibraryStreamRegistry::findStreamHookParams(const FindStreamHookParams &task) {
  const FunctionDecl *functionDecl = task.functionDecl;
  if (!this->isStreamHook(functionDecl)) {
    return;
  }
  const auto *lib = this->libraryBase(functionDecl->cls()->parent);
  if (lib == nullptr) {
    return;
  }
  auto &tasks = this->state.walk.tasks;
  tasks.pushTask(AfterHookError{.functionDecl = functionDecl, .lib = lib});

  return tasks.pushTask(PreludeTypeOf{.type = PreludeType::STREAM_ERROR});
}

void LibraryStreamRegistry::afterHookError(const AfterHookError &task) {
  const FunctionDecl *functionDecl = task.functionDecl;
  auto &tasks = this->state.walk.tasks;
  const Type *streamError = tasks.takeAnswer();
  auto sides = this->streamSides(task.lib, streamError);
  if (!sides.has_value()) {
    return;
  }
  llvm::SMLoc loc = functionDecl->loc();
  tasks.pushTask(AfterHookCallbacks{.functionDecl = functionDecl, .sides = *sides});
  tasks.pushTask(LibraryAliasOf{
      .libraryType = LibraryType::TRANSFORM_HOOK_CALLBACK,
      .arg = sides->error,
      .loc = loc,
  });

  return tasks.pushTask(LibraryAliasOf{
      .libraryType = LibraryType::HOOK_CALLBACK,
      .arg = sides->error,
      .loc = loc,
  });
}

void LibraryStreamRegistry::libraryAliasOf(const LibraryAliasOf &task) {
  auto &tasks = this->state.walk.tasks;
  auto *aliasDecl = this->state.library.declarations.findLibraryType(task.libraryType);
  if (aliasDecl == nullptr) {
    return tasks.pushAnswer(nullptr);
  }

  return tasks.pushTask(InstantiateAlias{.alias = aliasDecl, .args = {task.arg}, .loc = task.loc});
}

void LibraryStreamRegistry::afterHookCallbacks(const AfterHookCallbacks &task) {
  const FunctionDecl *functionDecl = task.functionDecl;
  const StreamSidesDraft &sides = task.sides;
  auto &tasks = this->state.walk.tasks;
  auto callbacks = tasks.takeAnswers(2);
  const Type *callback = callbacks[0];
  const Type *transformCallback = callbacks[1];
  auto hook = StreamHookTable::find(functionDecl->name);
  if (!hook.has_value()) {
    return;
  }
  switch (*hook) {
    case StreamHook::ON_READ:
      return this->rememberHook(functionDecl, sides.readable != nullptr,
                                std::vector<const Type *>{Type::intTy()});
    case StreamHook::ON_FINAL:
    case StreamHook::ON_CONSTRUCT:
      return this->rememberHook(functionDecl, true, std::vector<const Type *>{callback});
    case StreamHook::ON_DESTROY:
      tasks.pushTask(FinishDestroyHook{.functionDecl = functionDecl});
      return tasks.pushTask(PreludeTypeOf{.type = PreludeType::STREAM_ERROR});
    case StreamHook::ON_FLUSH:
      return this->rememberHook(functionDecl, sides.isTransform,
                                std::vector<const Type *>{transformCallback});
    case StreamHook::ON_TRANSFORM:
      return this->rememberHook(
          functionDecl, sides.isTransform,
          std::vector<const Type *>{sides.writable, Type::string(), transformCallback});
    case StreamHook::ON_WRITE:
      return this->rememberHook(
          functionDecl, sides.writable != nullptr,
          std::vector<const Type *>{sides.writable, Type::string(), callback});
    case StreamHook::ON_WRITEV:
      return this->startWritevHook(functionDecl, sides, callback);
  }
}

void LibraryStreamRegistry::startWritevHook(const FunctionDecl *functionDecl,
                                            const StreamSidesDraft &sides, const Type *callback) {
  if (sides.writable == nullptr) {
    return;
  }
  auto &tasks = this->state.walk.tasks;
  tasks.pushTask(FinishWritevHook{.functionDecl = functionDecl, .callback = callback});

  return tasks.pushTask(LibraryAliasOf{
      .libraryType = LibraryType::WRITE_CHUNK,
      .arg = sides.writable,
      .loc = functionDecl->loc(),
  });
}

void LibraryStreamRegistry::finishWritevHook(const FinishWritevHook &task) {
  const Type *chunk = this->state.walk.tasks.takeAnswer();
  if (chunk == nullptr) {
    return;
  }
  const Type *chunks = Type::array(chunk);

  return this->rememberHook(task.functionDecl, true,
                            std::vector<const Type *>{chunks, task.callback});
}

void LibraryStreamRegistry::finishDestroyHook(const FinishDestroyHook &task) {
  const Type *error = this->state.walk.tasks.takeAnswer();
  auto *aliasDecl = this->state.library.declarations.findLibraryType(LibraryType::STREAM_CALLBACK);
  const Type *callback = aliasDecl != nullptr ? aliasDecl->resolved : nullptr;
  const Type *failure = Type::optional(error);

  return this->rememberHook(task.functionDecl, true, std::vector<const Type *>{failure, callback});
}

void LibraryStreamRegistry::rememberHook(const FunctionDecl *functionDecl, bool isHooked,
                                         std::vector<const Type *> params) {
  if (!isHooked || params.empty() || llvm::is_contained(params, nullptr)) {
    return;
  }
  this->state.library.streams.rememberHookParams(functionDecl, std::move(params));
}

void LibraryStreamRegistry::walkHookParams(const WalkHookParams &task) {
  auto params = this->checkHookParams(task.functionDecl, task.inherited);
  bool isOther = params.isErr() &&
                 params.error().code == LibraryStreamRegistryHookParamsErrorCode::OTHER_PARAMETERS;
  if (isOther) {
    this->state.diagnostics.error(task.functionDecl->loc(), params.error().message);
  }
}

void LibraryStreamRegistry::checkStreamCall(const CheckStreamCall &task) {
  CallExpr *call = task.call;
  MemberExpr *member = task.member;
  ClassDecl *classDecl = task.classDecl;
  auto &tasks = this->state.walk.tasks;
  auto method = StreamMethodTable::find(member->name);
  if (!method.has_value() || !this->isStreamClass(classDecl)) {
    return tasks.pushChecked(nullptr);
  }
  auto methods = classDecl->findMethodsNamed(member->name);
  if (!methods.empty() && !methods[0]->cls()->library()) {
    return tasks.pushChecked(nullptr);
  }
  call->type = Type::error();
  member->type = Type::error();
  if (this->state.library.common.hasSpread(call)) {
    tasks.pushChecked(call);

    return tasks.pushTask(RejectSpreadCall{.call = call});
  }
  switch (*method) {
    case StreamMethod::SET_ENCODING:
      return this->startSetEncoding(call, member, classDecl);
    case StreamMethod::PIPE:
      return this->reportPipe(call, member);
    case StreamMethod::PUSH:
    case StreamMethod::UNSHIFT:
    case StreamMethod::WRITE:
    case StreamMethod::END:
      return this->startStreamData(call, member, classDecl, *method);
  }
}

void LibraryStreamRegistry::reportPipe(CallExpr *call, MemberExpr *member) {
  auto &tasks = this->state.walk.tasks;
  auto message = ErrorTextStreamCall::pipe();
  this->state.diagnostics.error(member->loc(), message);
  tasks.pushChecked(call);

  return tasks.pushTask(MarkCallFailed{.call = call});
}

void LibraryStreamRegistry::startSetEncoding(CallExpr *call, MemberExpr *member,
                                             ClassDecl *classDecl) {
  auto &tasks = this->state.walk.tasks;
  const Type *receiver = member->object->type;
  auto chunks = this->streamChunksOf(classDecl);
  llvm::SMLoc loc = member->loc();
  auto decoded = this->checkDecodedChunks(chunks.readable, receiver);
  if (decoded.isErr()) {
    this->state.diagnostics.error(loc, decoded.error().message);

    return tasks.pushChecked(call);
  }
  if (chunks.readable->isString()) {
    return this->pushPlainSetEncoding(call, member, classDecl, receiver);
  }
  const auto *object = this->withoutImplicit(member->object);
  bool isStdinStream = this->isStdinCall(object);
  if (isStdinStream) {
    auto placed = this->checkStdinPlace();
    if (placed.isErr()) {
      this->state.diagnostics.error(loc, placed.error().message);

      return tasks.pushChecked(call);
    }
  }
  auto path = this->encodingPath(object, isStdinStream);
  auto why = this->encodingPathProblem(path, isStdinStream);
  if (why.has_value()) {
    auto message = ErrorTextSetEncoding::changes(*why);
    this->state.diagnostics.error(loc, message);

    return tasks.pushChecked(call);
  }
  EncodingRun run{
      .member = member,
      .stdinStream = isStdinStream,
      .path = *path,
      .here = nullptr,
      .start = nullptr,
      .escapes = {},
      .safe = {},
  };
  this->markEscapeWindow(run);
  this->collectEscapes(run);
  if (!run.escapes.empty()) {
    this->reportFirstEscape(run, loc);

    return tasks.pushChecked(call);
  }
  tasks.pushTask(FinishEncodedView{.call = call, .path = run.path, .here = run.here});
  this->pushPlainSetEncoding(call, member, classDecl, receiver);

  return this->pushEncodedStreamType(classDecl, loc);
}

void LibraryStreamRegistry::pushPlainSetEncoding(CallExpr *call, MemberExpr *member,
                                                 ClassDecl *classDecl, const Type *receiver) {
  auto &tasks = this->state.walk.tasks;
  auto setEncoding = LibraryMethodTable::getName(LibraryMethod::SET_ENCODING);
  tasks.pushTask(AfterPlainEncoding{.call = call, .receiver = receiver});

  return tasks.pushTask(CallLibraryMethod{
      .call = call,
      .member = member,
      .classDecl = classDecl,
      .name = setEncoding,
      .bound = {},
      .given = {},
      .receiver = CallReceiver::OWN,
  });
}

void LibraryStreamRegistry::afterPlainEncoding(const AfterPlainEncoding &task) {
  auto &tasks = this->state.walk.tasks;
  Expr *checked = tasks.takeChecked();
  if (!task.call->type->isError()) {
    task.call->type = task.receiver;
  }

  return tasks.pushChecked(checked);
}

void LibraryStreamRegistry::pushEncodedStreamType(ClassDecl *classDecl, llvm::SMLoc loc) {
  auto &tasks = this->state.walk.tasks;
  for (ClassDecl *current = classDecl; current != nullptr; current = current->parent) {
    auto args = this->encodedArgs(current);
    if (!args.empty()) {
      return tasks.pushTask(InstantiateClassType{
          .classDecl = current->generic(),
          .args = std::move(args),
          .loc = loc,
      });
    }
  }

  return tasks.pushAnswer(nullptr);
}

void LibraryStreamRegistry::finishEncodedView(const FinishEncodedView &task) {
  CallExpr *call = task.call;
  const Type *view = this->state.walk.tasks.takeAnswer();
  bool isKnown = view != nullptr && !view->isError();
  if (!isKnown || call->type->isError()) {
    return;
  }
  call->type = view;
  llvm::SMLoc here = llvm::SMLoc::getFromPointer(task.here);
  this->state.library.streams.rememberStreamView(task.path, here, view);
}

void LibraryStreamRegistry::startStreamData(CallExpr *call, MemberExpr *member,
                                            ClassDecl *classDecl, StreamMethod method) {
  auto &tasks = this->state.walk.tasks;
  auto chunks = this->streamChunksOf(classDecl);
  bool isReading = method == StreamMethod::PUSH || method == StreamMethod::UNSHIFT;
  const Type *chunk = isReading ? chunks.readable : chunks.writable;
  const Type *receiver = member->object->type;
  auto side = this->checkStreamSide(chunk, receiver, member->name, isReading);
  if (side.isErr()) {
    this->state.diagnostics.error(member->loc(), side.error().message);
    tasks.pushChecked(call);

    return tasks.pushTask(WalkArgsLoosely{.call = call});
  }
  auto stream = StreamDataDraft{
      .call = call,
      .member = member,
      .classDecl = classDecl,
      .method = method,
      .chunk = chunk,
  };
  if (isReading) {
    return this->startStreamRead(stream);
  }
  if (method == StreamMethod::WRITE) {
    return this->startStreamWrite(stream);
  }

  return this->startStreamEnd(stream);
}

bool LibraryStreamRegistry::hasStreamArity(const StreamDataDraft &stream, unsigned min,
                                           unsigned max) {
  auto arity = this->state.library.common.checkArity(stream.call, min, max);
  if (arity.isErr()) {
    this->state.diagnostics.error(stream.member->loc(), arity.error().message);
    this->state.walk.tasks.pushChecked(stream.call);
  }

  return arity.isOk();
}

void LibraryStreamRegistry::startStreamRead(const StreamDataDraft &stream) {
  CallExpr *call = stream.call;
  auto &tasks = this->state.walk.tasks;
  if (!this->hasStreamArity(stream, 1, 2)) {
    return;
  }
  LibraryMethod typed = stream.method == StreamMethod::PUSH ? LibraryMethod::PUSH_TYPED
                                                            : LibraryMethod::UNSHIFT_TYPED;
  tasks.pushTask(FinishStreamData{.stream = stream, .count = 2, .implementation = typed});
  if (call->args.size() > 1) {
    this->pushEncodingValue(call->args[1], false);
  } else {
    this->pushUndefinedValue(call->loc(), this->optionalString());
  }

  return this->pushChunkValue(stream, call->args[0], StreamChunkSide::PUSHED, false);
}

void LibraryStreamRegistry::startStreamWrite(const StreamDataDraft &stream) {
  CallExpr *call = stream.call;
  auto &tasks = this->state.walk.tasks;
  if (!this->hasStreamArity(stream, 1, 3)) {
    return;
  }
  tasks.pushTask(AfterWriteChunk{.stream = stream});

  return this->pushChunkValue(stream, call->args[0], StreamChunkSide::WRITTEN, false);
}

void LibraryStreamRegistry::afterWriteChunk(const AfterWriteChunk &task) {
  const StreamDataDraft &stream = task.stream;
  CallExpr *call = stream.call;
  auto &tasks = this->state.walk.tasks;
  size_t count = call->args.size();
  if (count == 3) {
    auto parts = WriteParts{
        .encoding = call->args[1],
        .isEncodingChecked = false,
        .callback = call->args[2],
        .isCallbackChecked = false,
    };

    return this->continueStreamWrite(stream, parts);
  }
  if (count == 1) {
    return this->continueStreamWrite(stream, WriteParts{});
  }
  Expr *second = call->args[1];
  if (isa<FunctionExpr>(second)) {
    return this->continueStreamWrite(stream, WriteParts{.callback = second});
  }
  if (isa<StringLitExpr>(second)) {
    return this->continueStreamWrite(stream, WriteParts{.encoding = second});
  }
  tasks.pushTask(AfterWriteSecond{.stream = stream});

  return tasks.pushTask(CheckExprSlot{.slot = &call->args[1], .expected = nullptr});
}

void LibraryStreamRegistry::afterWriteSecond(const AfterWriteSecond &task) {
  Expr *second = task.stream.call->args[1];
  if (second->type->nonOptional()->isFunction()) {
    return this->continueStreamWrite(task.stream,
                                     WriteParts{.callback = second, .isCallbackChecked = true});
  }

  return this->continueStreamWrite(task.stream,
                                   WriteParts{.encoding = second, .isEncodingChecked = true});
}

void LibraryStreamRegistry::continueStreamWrite(const StreamDataDraft &stream,
                                                const WriteParts &parts) {
  auto &tasks = this->state.walk.tasks;
  tasks.pushTask(AfterWrittenType{
      .stream = stream,
      .callback = parts.callback,
      .isCallbackChecked = parts.isCallbackChecked,
  });
  tasks.pushTask(WrittenCallbackType{.classDecl = stream.classDecl});
  if (parts.encoding != nullptr) {
    return this->pushEncodingValue(parts.encoding, parts.isEncodingChecked);
  }

  return this->pushUndefinedValue(stream.call->loc(), this->optionalString());
}

void LibraryStreamRegistry::writtenCallbackType(const WrittenCallbackType &task) {
  auto &tasks = this->state.walk.tasks;
  auto writeTyped = LibraryMethodTable::getName(LibraryMethod::WRITE_TYPED);
  for (FunctionDecl *typed : task.classDecl->findMethodsNamed(writeTyped)) {
    if (typed->sym != nullptr && typed->params.size() == 3) {
      return tasks.pushAnswer(typed->sym->params[2]);
    }
  }
  tasks.pushTask(AnswerStreamCallback{.isOptional = true});

  return tasks.pushTask(PreludeTypeOf{.type = PreludeType::STREAM_ERROR});
}

void LibraryStreamRegistry::answerStreamCallback(const AnswerStreamCallback &task) {
  auto &tasks = this->state.walk.tasks;
  const Type *error = tasks.takeAnswer();
  const Type *failure = Type::optional(error);
  const Type *callback = Type::function({failure}, 0, false, Type::voidTy());
  const Type *answer = task.isOptional ? Type::optional(callback) : callback;

  return tasks.pushAnswer(answer);
}

void LibraryStreamRegistry::afterWrittenType(const AfterWrittenType &task) {
  const StreamDataDraft &stream = task.stream;
  auto &tasks = this->state.walk.tasks;
  const Type *written = tasks.takeAnswer();
  auto writeTyped = LibraryMethod::WRITE_TYPED;
  tasks.pushTask(FinishStreamData{.stream = stream, .count = 3, .implementation = writeTyped});
  if (task.callback == nullptr) {
    return this->pushUndefinedValue(stream.call->loc(), written);
  }

  return this->pushCheckedAs(task.callback, task.isCallbackChecked, written);
}

void LibraryStreamRegistry::startStreamEnd(const StreamDataDraft &stream) {
  if (!this->hasStreamArity(stream, 0, 3)) {
    return;
  }

  return this->nextEndArg(NextEndArg{.stream = stream, .index = 0, .checked = 0});
}

void LibraryStreamRegistry::nextEndArg(const NextEndArg &task) {
  const StreamDataDraft &stream = task.stream;
  CallExpr *call = stream.call;
  auto &tasks = this->state.walk.tasks;
  if (task.index == call->args.size()) {
    return this->continueStreamEnd(stream, task.checked);
  }
  Expr *arg = call->args[task.index];
  if (isa<FunctionExpr>(arg)) {
    return this->failEndCallback(stream, arg);
  }
  if (isa<StringLitExpr>(arg)) {
    return this->nextEndArg(
        NextEndArg{.stream = stream, .index = task.index + 1, .checked = task.checked});
  }
  tasks.pushTask(AfterEndArg{.stream = stream, .index = task.index, .checked = task.checked});

  return tasks.pushTask(CheckExprSlot{.slot = &call->args[task.index], .expected = nullptr});
}

void LibraryStreamRegistry::afterEndArg(const AfterEndArg &task) {
  Expr *arg = task.stream.call->args[task.index];
  if (arg->type->nonOptional()->isFunction()) {
    return this->failEndCallback(task.stream, arg);
  }
  unsigned checked = task.checked | (1U << task.index);

  return this->nextEndArg(
      NextEndArg{.stream = task.stream, .index = task.index + 1, .checked = checked});
}

void LibraryStreamRegistry::failEndCallback(const StreamDataDraft &stream, const Expr *arg) {
  auto &tasks = this->state.walk.tasks;
  auto message = ErrorTextStreamCall::endWithFn();
  this->state.diagnostics.error(arg->loc(), message);
  tasks.pushChecked(stream.call);

  return tasks.pushTask(MarkCallFailed{.call = stream.call});
}

void LibraryStreamRegistry::continueStreamEnd(const StreamDataDraft &stream, unsigned checked) {
  CallExpr *call = stream.call;
  auto &tasks = this->state.walk.tasks;
  size_t count = call->args.size();
  if (count == 3) {
    auto message = ErrorTextStreamCall::endArgs();
    this->state.diagnostics.error(call->args[2]->loc(), message);
    tasks.pushChecked(call);

    return tasks.pushTask(CheckExprSlot{.slot = &call->args[2], .expected = nullptr});
  }
  llvm::SMLoc loc = call->loc();
  auto endTyped = LibraryMethod::END_TYPED;
  tasks.pushTask(FinishStreamData{.stream = stream, .count = 3, .implementation = endTyped});
  if (count >= 2) {
    bool isEncodingChecked = (checked & 2U) != 0;
    this->pushEncodingValue(call->args[1], isEncodingChecked);
  } else {
    this->pushUndefinedValue(loc, this->optionalString());
  }
  if (count >= 1) {
    bool isChunkChecked = (checked & 1U) != 0;
    this->pushChunkValue(stream, call->args[0], StreamChunkSide::WRITTEN, isChunkChecked);
  } else {
    this->pushUndefinedValue(loc, Type::unknown());
  }
  auto *given = this->state.ast.make<BoolLitExpr>(loc, count >= 1);

  return tasks.pushTask(CheckExpr{.expr = given, .expected = nullptr});
}

void LibraryStreamRegistry::finishStreamData(const FinishStreamData &task) {
  const StreamDataDraft &stream = task.stream;
  CallExpr *call = stream.call;
  auto &tasks = this->state.walk.tasks;
  auto args = this->takeCheckedValues(task.count);
  for (Expr *arg : args) {
    if (arg->type->isError()) {
      return tasks.pushChecked(call);
    }
  }
  auto implementation = LibraryMethodTable::getName(task.implementation);
  call->args = std::move(args);
  stream.member->name = implementation;
  auto given = std::vector<bool>(call->args.size(), true);

  return tasks.pushTask(CallLibraryMethod{
      .call = call,
      .member = stream.member,
      .classDecl = stream.classDecl,
      .name = implementation,
      .bound = {},
      .given = std::move(given),
      .receiver = CallReceiver::OWN,
  });
}

std::vector<Expr *> LibraryStreamRegistry::takeCheckedValues(size_t count) {
  auto values = std::vector<Expr *>(count, nullptr);
  for (size_t index = count; index > 0; --index) {
    values[index - 1] = this->state.walk.tasks.takeChecked();
  }

  return values;
}

void LibraryStreamRegistry::pushChunkValue(const StreamDataDraft &stream, Expr *arg,
                                           StreamChunkSide side, bool isChecked) {
  auto &tasks = this->state.walk.tasks;
  const Type *chunk = stream.chunk;
  llvm::StringRef method = stream.member->name;
  auto given = this->checkChunkNotUndefined(arg, side, method);
  if (given.isErr()) {
    this->state.diagnostics.error(arg->loc(), given.error().message);
    arg->type = Type::error();

    return tasks.pushTask(PushCheckedValue{.expr = arg});
  }
  if (!this->isByteChunk(chunk)) {
    tasks.pushTask(Convert{.to = Type::unknown()});

    return this->pushCheckedAs(arg, isChecked, chunk);
  }
  tasks.pushTask(AfterByteChunk{.chunk = chunk, .side = side, .method = method});
  if (isChecked) {
    return tasks.pushTask(PushCheckedValue{.expr = arg});
  }

  return tasks.pushTask(CheckExpr{.expr = arg, .expected = nullptr});
}

void LibraryStreamRegistry::afterByteChunk(const AfterByteChunk &task) {
  auto &tasks = this->state.walk.tasks;
  Expr *arg = tasks.takeChecked();
  tasks.pushChecked(arg);
  const Type *type = arg->type;
  if (type->isError()) {
    return;
  }
  bool isOnlyStrings = task.side == StreamChunkSide::WRITTEN && task.chunk->isString();
  auto fits = std::bind_front(&LibraryStreamRegistry::isChunkValue, this, isOnlyStrings);
  bool isOk = type->isUnion() ? llvm::all_of(type->members(), fits) : fits(type);
  if (!isOk) {
    auto message =
        ErrorTextStreamCall::chunk(task.method, task.chunk->name(), isOnlyStrings, type->name());

    return this->state.diagnostics.error(arg->loc(), message);
  }

  return tasks.pushTask(Convert{.to = Type::unknown()});
}

void LibraryStreamRegistry::pushCheckedAs(Expr *arg, bool isChecked, const Type *type) {
  auto &tasks = this->state.walk.tasks;
  tasks.pushTask(Coerce{.to = type, .what = ""});
  if (isChecked) {
    return tasks.pushTask(PushCheckedValue{.expr = arg});
  }
  const Type *expected = type->isError() ? nullptr : type;

  return tasks.pushTask(CheckExpr{.expr = arg, .expected = expected});
}

void LibraryStreamRegistry::pushEncodingValue(Expr *arg, bool isChecked) {
  auto &tasks = this->state.walk.tasks;
  tasks.pushTask(AfterEncodingType{.arg = arg, .isChecked = isChecked});

  return tasks.pushTask(PreludeTypeOf{.type = PreludeType::BUFFER_ENCODING});
}

void LibraryStreamRegistry::afterEncodingType(const AfterEncodingType &task) {
  auto &tasks = this->state.walk.tasks;
  const Type *encoding = tasks.takeAnswer();
  const Type *bufferEncoding = Type::optional(encoding);
  tasks.pushTask(AfterEncodingValue{});

  return this->pushCheckedAs(task.arg, task.isChecked, bufferEncoding);
}

void LibraryStreamRegistry::afterEncodingValue() {
  auto &tasks = this->state.walk.tasks;
  Expr *encoding = tasks.takeChecked();
  tasks.pushChecked(encoding);
  if (encoding->type->isError()) {
    return;
  }

  return tasks.pushTask(Convert{.to = this->optionalString()});
}

void LibraryStreamRegistry::pushUndefinedValue(llvm::SMLoc loc, const Type *type) {
  auto &tasks = this->state.walk.tasks;
  auto *nothing = this->state.ast.make<UndefinedExpr>(loc);
  tasks.pushTask(Convert{.to = type});

  return tasks.pushTask(CheckExpr{.expr = nothing, .expected = nullptr});
}

void LibraryStreamRegistry::pushArgValue(Expr *arg, const Type *type) {
  return this->pushCheckedAs(arg, false, type);
}

const Type *LibraryStreamRegistry::optionalString() const {
  return Type::optional(Type::string());
}

void LibraryStreamRegistry::checkStreamIntrinsic(const CheckStreamIntrinsic &task) {
  CallExpr *call = task.call;
  const Type *typeArg = task.typeArg;
  auto &tasks = this->state.walk.tasks;
  Builtin builtin = call->builtin();
  if (builtin == Builtin::LibChunk || builtin == Builtin::LibIsChunk) {
    return this->checkChunkIntrinsic(call, typeArg);
  }
  if (builtin == Builtin::LibChunkKind) {
    unsigned kind = this->chunkKind(typeArg);
    auto *literal = this->state.ast.make<IntLitExpr>(call->loc(), uint64_t(kind));

    return tasks.pushTask(CheckExpr{.expr = literal, .expected = Type::intTy()});
  }
  if (builtin == Builtin::LibTypeName) {
    auto *literal = this->typeNameLiteral(call, typeArg);

    return tasks.pushTask(CheckExpr{.expr = literal, .expected = Type::string()});
  }
  if (builtin == Builtin::LibOverrides) {
    return this->startOverridesIntrinsic(call, task.name);
  }
  if (builtin == Builtin::LibStdinFailed) {
    call->type = Type::boolTy();

    return tasks.pushChecked(call);
  }
  if (!this->isNativeIo(builtin)) {
    return tasks.pushChecked(call);
  }
  if (typeArg != nullptr) {
    return this->checkNativeIo(call, typeArg);
  }
  tasks.pushTask(AfterNativeIoFailure{.call = call});

  return tasks.pushTask(NativeErrorType{});
}

void LibraryStreamRegistry::checkChunkIntrinsic(CallExpr *call, const Type *typeArg) {
  auto &tasks = this->state.walk.tasks;
  bool isChunk = call->builtin() == Builtin::LibChunk;
  call->type = isChunk ? typeArg : Type::boolTy();
  if (!isChunk) {
    Expr *witness = this->state.ast.make<UndefinedExpr>(call->loc());
    witness->type = typeArg;
    call->args.push_back(witness);
  }
  tasks.pushChecked(call);

  return tasks.pushTask(CheckArgSlot{.slot = &call->args[0], .to = Type::unknown()});
}

StringLitExpr *LibraryStreamRegistry::typeNameLiteral(CallExpr *call, const Type *typeArg) {
  auto units = llvm::SmallVector<llvm::UTF16, 32>();
  llvm::convertUTF8ToUTF16String(typeArg->name(), units);
  std::u16string text(units.begin(), units.end());

  return this->state.ast.make<StringLitExpr>(call->loc(), text);
}

void LibraryStreamRegistry::startOverridesIntrinsic(CallExpr *call, NameExpr *name) {
  auto &tasks = this->state.walk.tasks;
  call->type = Type::boolTy();
  tasks.pushChecked(call);
  tasks.pushTask(AfterOverridesClass{.call = call, .name = name});

  return tasks.pushTask(CheckExprSlot{.slot = &call->args[0], .expected = nullptr});
}

void LibraryStreamRegistry::afterOverridesClass(const AfterOverridesClass &task) {
  CallExpr *call = task.call;
  auto *classDecl = call->args[0]->type->classDecl();
  auto *literal = dyn_cast<StringLitExpr>(call->args[1]);
  auto args = this->checkOverridesArgs(classDecl, literal);
  if (args.isErr()) {
    return this->state.diagnostics.error(task.name->loc(), args.error().message);
  }
  auto method = ToUtf8::toUtf8(literal->value);
  auto methods = classDecl->findMethodsNamed(method);
  auto there = this->checkOverriddenMethod(classDecl, methods, method);
  if (there.isErr()) {
    return this->state.diagnostics.error(literal->loc(), there.error().message);
  }
  call->target = BuiltinCall{.builtin = Builtin::LibOverrides, .method = methods[0]->sym};

  return this->state.walk.tasks.pushTask(
      CheckExprSlot{.slot = &call->args[1], .expected = nullptr});
}

void LibraryStreamRegistry::afterNativeIoFailure(const AfterNativeIoFailure &task) {
  const Type *failure = this->state.walk.tasks.takeAnswer();

  return this->checkNativeIo(task.call, failure);
}

void LibraryStreamRegistry::checkNativeIo(CallExpr *call, const Type *failure) {
  auto &tasks = this->state.walk.tasks;
  auto signature = this->nativeIoSignature(call->builtin(), failure);
  call->type = signature.result;
  this->state.inventory.usesAsync = true;
  tasks.pushChecked(call);
  tasks.pushTask(CalledFunction{});
  for (size_t index = signature.params.size(); index > 0; --index) {
    const Type *param = signature.params[index - 1];
    tasks.pushTask(CheckArgSlot{.slot = &call->args[index - 1], .to = param});
  }
}

void LibraryStreamRegistry::checkStreamLibraryCall(const CheckStreamLibraryCall &task) {
  CallExpr *call = task.call;
  FuncSymbol *function = task.function;
  auto &tasks = this->state.walk.tasks;
  auto *functionDecl = function->decl != nullptr ? dyn_cast<FunctionDecl>(function->decl) : nullptr;
  if (functionDecl == nullptr) {
    return tasks.pushChecked(nullptr);
  }
  llvm::SMLoc loc = task.name->loc();
  auto place = this->state.library.common.placeOf(functionDecl->loc());
  if (this->isInModule(place, LibraryModuleNames::kFsStreamsPath)) {
    return this->startFsStreamFunction(call, functionDecl, loc);
  }
  bool hasPromises = this->isInModule(place, LibraryModuleNames::kStreamPromisesPath);
  if (!hasPromises && !this->isInModule(place, LibraryModuleNames::kStreamPath)) {
    return tasks.pushChecked(nullptr);
  }
  auto streamFunction = StreamFunctionTable::find(functionDecl->name);
  if (!streamFunction.has_value()) {
    return tasks.pushChecked(nullptr);
  }
  call->type = Type::error();
  if (this->state.library.common.hasSpread(call)) {
    tasks.pushChecked(call);

    return tasks.pushTask(RejectSpreadCall{.call = call});
  }
  auto *streamClass = this->state.library.declarations.findLibraryClass(LibraryClass::STREAM);
  if (streamClass == nullptr) {
    return tasks.pushChecked(nullptr);
  }
  auto stream = StreamFunctionDraft{
      .call = call,
      .loc = loc,
      .streamClass = streamClass,
      .streamType = streamClass->type,
      .callback = nullptr,
      .isPromises = hasPromises,
  };
  tasks.pushTask(AfterStreamCallback{.stream = stream, .function = *streamFunction});
  tasks.pushTask(AnswerStreamCallback{.isOptional = false});

  return tasks.pushTask(PreludeTypeOf{.type = PreludeType::STREAM_ERROR});
}

void LibraryStreamRegistry::afterStreamCallback(const AfterStreamCallback &task) {
  StreamFunctionDraft stream = task.stream;
  stream.callback = this->state.walk.tasks.takeAnswer();
  switch (task.function) {
    case StreamFunction::FINISHED:
      return this->startFinished(stream);
    case StreamFunction::COMPOSE:
      return this->startCompose(stream);
    case StreamFunction::PIPELINE:
      return this->startPipeline(stream);
  }
}

void LibraryStreamRegistry::startFinished(const StreamFunctionDraft &stream) {
  CallExpr *call = stream.call;
  auto &tasks = this->state.walk.tasks;
  unsigned most = stream.isPromises ? 2 : 3;
  auto arity = this->state.library.common.checkArity(call, most - 1, most);
  if (arity.isErr()) {
    this->state.diagnostics.error(stream.loc, arity.error().message);

    return tasks.pushChecked(call);
  }
  auto *alias = this->state.library.declarations.findLibraryType(LibraryType::FINISHED_OPTIONS);
  if (alias == nullptr) {
    return this->continueFinished(stream, Type::error());
  }
  tasks.pushTask(AfterFinishedOptions{.stream = stream});
  tasks.pushTask(AnswerAliasType{.alias = alias});

  return tasks.pushTask(ResolveAlias{.alias = alias});
}

void LibraryStreamRegistry::afterFinishedOptions(const AfterFinishedOptions &task) {
  const Type *resolved = this->state.walk.tasks.takeAnswer();
  const Type *options = Type::optional(resolved);

  return this->continueFinished(task.stream, options);
}

void LibraryStreamRegistry::continueFinished(const StreamFunctionDraft &stream,
                                             const Type *options) {
  CallExpr *call = stream.call;
  auto &tasks = this->state.walk.tasks;
  unsigned most = stream.isPromises ? 2 : 3;
  tasks.pushTask(FinishFinished{.stream = stream});
  if (!stream.isPromises) {
    this->pushArgValue(call->args.back(), stream.callback);
  }
  if (call->args.size() == most) {
    this->pushArgValue(call->args[1], options);
  } else {
    this->pushUndefinedValue(stream.loc, options);
  }
  tasks.pushTask(NoteTargetHandled{});

  return this->pushArgValue(call->args[0], stream.streamType);
}

void LibraryStreamRegistry::noteTargetHandled() {
  auto &tasks = this->state.walk.tasks;
  Expr *target = tasks.takeChecked();
  tasks.pushChecked(target);

  return tasks.pushTask(NoteErrorsHandled{.expr = target});
}

void LibraryStreamRegistry::finishFinished(const FinishFinished &task) {
  const StreamFunctionDraft &stream = task.stream;
  CallExpr *call = stream.call;
  size_t count = stream.isPromises ? 2 : 3;
  call->args = this->takeCheckedValues(count);
  LibraryMethod method =
      stream.isPromises ? LibraryMethod::FINISHED_RESULT : LibraryMethod::EOS_FOR;
  auto name = LibraryMethodTable::getName(method);
  auto given = std::vector<bool>(count, true);

  return this->state.walk.tasks.pushTask(CallLibraryStatic{
      .call = call,
      .classDecl = stream.streamClass,
      .name = name,
      .bound = {},
      .given = std::move(given),
      .loc = stream.loc,
  });
}

void LibraryStreamRegistry::startCompose(const StreamFunctionDraft &stream) {
  CallExpr *call = stream.call;
  if (call->args.empty()) {
    auto message = ErrorTextCompose::streams();
    this->state.diagnostics.error(stream.loc, message);

    return this->state.walk.tasks.pushChecked(call);
  }

  return this->nextComposedPart(NextComposedPart{.draft = ComposeDraft{
                                                     .stream = stream,
                                                     .index = 0,
                                                     .chunk = nullptr,
                                                     .headWritable = nullptr,
                                                     .tailReadable = nullptr,
                                                 }});
}

void LibraryStreamRegistry::nextComposedPart(const NextComposedPart &task) {
  const ComposeDraft &draft = task.draft;
  CallExpr *call = draft.stream.call;
  auto &tasks = this->state.walk.tasks;
  if (draft.index == call->args.size()) {
    return this->finishCompose(draft);
  }
  tasks.pushTask(AfterComposedPart{.draft = draft});

  return tasks.pushTask(CheckExprSlot{.slot = &call->args[draft.index], .expected = nullptr});
}

void LibraryStreamRegistry::afterComposedPart(const AfterComposedPart &task) {
  ComposeDraft draft = task.draft;
  CallExpr *call = draft.stream.call;
  auto &tasks = this->state.walk.tasks;
  size_t index = draft.index;
  Expr *arg = call->args[index];
  if (arg->type->isError()) {
    return tasks.pushChecked(call);
  }
  auto *classDecl = arg->type->classDecl();
  bool isStreamed =
      classDecl != nullptr && this->isStreamClass(classDecl) && !arg->type->isOptional();
  if (!isStreamed) {
    auto message = ErrorTextCompose::notStream(arg->type->name());
    this->state.diagnostics.error(arg->loc(), message);
    tasks.pushChecked(call);

    return this->pushLooseChecks(call, index + 1);
  }
  auto chunks = this->streamChunksOf(classDecl);
  bool isFitting = index == 0 || draft.chunk == nullptr || chunks.writable == nullptr ||
                   this->isStreamTaking(chunks.writable, draft.chunk);
  if (!isFitting) {
    auto compose = StreamFunctionTable::getName(StreamFunction::COMPOSE);
    auto message = ErrorTextPipeline::chunkKind(compose, draft.chunk->name(), arg->type->name(),
                                                chunks.writable->name());
    this->state.diagnostics.error(arg->loc(), message);

    return tasks.pushChecked(call);
  }
  if (index == 0) {
    draft.headWritable = chunks.writable;
  }
  draft.tailReadable = chunks.readable;
  draft.chunk = chunks.readable;
  draft.index = index + 1;
  tasks.pushTask(NextComposedPart{.draft = draft});

  return tasks.pushTask(ConvertSlot{.slot = &call->args[index], .to = draft.stream.streamType});
}

void LibraryStreamRegistry::finishCompose(const ComposeDraft &draft) {
  const StreamFunctionDraft &stream = draft.stream;
  CallExpr *call = stream.call;
  auto *list = this->state.ast.make<ArrayLitExpr>(stream.loc);
  for (Expr *part : call->args) {
    list->elements.push_back({part, false});
  }
  list->type = Type::array(stream.streamType);
  const auto *buffer = Type::bytesType(Type::Bytes::Buffer);
  const Type *writable = draft.headWritable != nullptr ? draft.headWritable : buffer;
  const Type *readable = draft.tailReadable != nullptr ? draft.tailReadable : buffer;
  call->args = {list};
  auto composeAll = LibraryMethodTable::getName(LibraryMethod::COMPOSE_ALL);

  return this->state.walk.tasks.pushTask(CallLibraryStatic{
      .call = call,
      .classDecl = stream.streamClass,
      .name = composeAll,
      .bound = {writable, readable},
      .given = {true},
      .loc = stream.loc,
  });
}

void LibraryStreamRegistry::pushLooseChecks(CallExpr *call, size_t from) {
  for (size_t index = call->args.size(); index > from; --index) {
    this->state.walk.tasks.pushTask(
        CheckExprSlot{.slot = &call->args[index - 1], .expected = nullptr});
  }
}

void LibraryStreamRegistry::startPipeline(const StreamFunctionDraft &stream) {
  CallExpr *call = stream.call;
  auto &tasks = this->state.walk.tasks;
  auto draft = PipelineDraft{
      .stream = stream,
      .count = call->args.size(),
      .callback = nullptr,
      .options = nullptr,
      .index = 0,
      .chunk = nullptr,
      .isLastStream = false,
      .sinkValue = nullptr,
  };
  if (!stream.isPromises && draft.count < 3) {
    auto message = ErrorTextPipeline::callback();
    this->state.diagnostics.error(stream.loc, message);
    tasks.pushChecked(call);

    return tasks.pushTask(WalkArgsLoosely{.call = call});
  }
  if (!stream.isPromises) {
    draft.callback = call->args.back();
    --draft.count;
  } else if (draft.count > 0 && isa<ObjectLitExpr>(call->args.back())) {
    draft.options = call->args.back();
    --draft.count;
  }
  if (draft.count < 2) {
    auto message = ErrorTextPipeline::streams();
    this->state.diagnostics.error(stream.loc, message);
    tasks.pushChecked(call);

    return tasks.pushTask(WalkArgsLoosely{.call = call});
  }

  return this->nextPipelineStage(NextPipelineStage{.draft = draft});
}

void LibraryStreamRegistry::nextPipelineStage(const NextPipelineStage &task) {
  const PipelineDraft &draft = task.draft;
  CallExpr *call = draft.stream.call;
  auto &tasks = this->state.walk.tasks;
  if (draft.index == draft.count) {
    return this->finishPipeline(draft);
  }
  Expr *arg = call->args[draft.index];
  auto *stageClass =
      this->state.library.declarations.findLibraryClass(LibraryClass::PIPELINE_STAGE);
  auto shape = StageShapeDraft{
      .loc = arg->loc(),
      .stageType = stageClass != nullptr ? stageClass->type : Type::error(),
      .isFirst = draft.index == 0,
      .isLast = draft.index + 1 == draft.count,
  };
  tasks.pushTask(AfterStageValue{.draft = draft, .shape = shape});
  auto *functionExpr = dyn_cast<FunctionExpr>(arg);
  if (functionExpr == nullptr) {
    return tasks.pushTask(CheckExpr{.expr = arg, .expected = nullptr});
  }
  Signature signature;
  if (!shape.isFirst) {
    signature.params = {Type::asyncIterator(draft.chunk)};
    signature.minArgs = 1;
  }

  return tasks.pushTask(CheckFunctionExpr{.functionExpr = functionExpr, .expected = signature});
}

void LibraryStreamRegistry::afterStageValue(const AfterStageValue &task) {
  PipelineDraft draft = task.draft;
  const StageShapeDraft &shape = task.shape;
  CallExpr *call = draft.stream.call;
  auto &tasks = this->state.walk.tasks;
  Expr *arg = tasks.takeChecked();
  call->args[draft.index] = arg;
  const Type *type = arg->type;
  if (type->isError()) {
    return this->failPipelineStage(draft);
  }
  auto *classDecl = type->classDecl();
  bool isStream = classDecl != nullptr && this->isStreamClass(classDecl) && !type->isOptional();
  if (isStream) {
    return this->streamClassStage(draft, shape, arg, classDecl);
  }
  if (type->isFunction()) {
    return this->functionStage(draft, shape, arg);
  }
  if (shape.isFirst) {
    return this->iterableSourceStage(draft, shape, arg);
  }

  return this->failStageKind(draft, shape, arg);
}

void LibraryStreamRegistry::failStageKind(const PipelineDraft &draft, const StageShapeDraft &shape,
                                          Expr *arg) {
  auto message = ErrorTextPipeline::stage(arg->type->name());
  this->state.diagnostics.error(shape.loc, message);
  arg->type = Type::error();

  return this->failPipelineStage(draft);
}

void LibraryStreamRegistry::failPipelineStage(const PipelineDraft &draft) {
  CallExpr *call = draft.stream.call;
  this->state.walk.tasks.pushChecked(call);

  return this->pushLooseChecks(call, draft.index + 1);
}

void LibraryStreamRegistry::streamClassStage(PipelineDraft draft, const StageShapeDraft &shape,
                                             Expr *arg, ClassDecl *classDecl) {
  auto &tasks = this->state.walk.tasks;
  const Type *type = arg->type;
  auto chunks = this->streamChunksOf(classDecl);
  bool isReported = false;
  if (!shape.isFirst) {
    isReported = this->reportWritableEnd(draft, shape, chunks, type);
  }
  if (!isReported && !shape.isLast) {
    auto readable = this->checkReadableEnd(chunks, type);
    if (readable.isErr()) {
      this->state.diagnostics.error(shape.loc, readable.error().message);
    } else {
      draft.chunk = chunks.readable;
    }
    isReported = readable.isErr();
  }
  tasks.pushTask(AfterStageMade{.draft = draft, .isStream = true});
  tasks.pushChecked(arg);
  if (isReported) {
    return;
  }
  const Type *stage = this->pipelineStageType(shape);

  return tasks.pushTask(Convert{.to = stage});
}

bool LibraryStreamRegistry::reportWritableEnd(const PipelineDraft &draft,
                                              const StageShapeDraft &shape,
                                              const StreamChunks &chunks, const Type *type) {
  auto writable = this->checkWritableEnd(chunks, type);
  if (writable.isErr()) {
    this->state.diagnostics.error(shape.loc, writable.error().message);

    return true;
  }
  if (draft.chunk == nullptr || this->isStreamTaking(chunks.writable, draft.chunk)) {
    return false;
  }
  auto pipeline = StreamFunctionTable::getName(StreamFunction::PIPELINE);
  auto message = ErrorTextPipeline::chunkKind(pipeline, draft.chunk->name(), type->name(),
                                              chunks.writable->name());
  this->state.diagnostics.error(shape.loc, message);

  return true;
}

const Type *LibraryStreamRegistry::pipelineStageType(const StageShapeDraft &shape) const {
  const Type *streamType =
      this->state.library.declarations.findLibraryClass(LibraryClass::STREAM)->type;

  return Type::unionOf({streamType, shape.stageType});
}


void LibraryStreamRegistry::functionStage(const PipelineDraft &draft, const StageShapeDraft &shape,
                                          Expr *arg) {
  auto &tasks = this->state.walk.tasks;
  const Type *returnType = arg->type->ret();
  if (shape.isFirst) {
    return this->sourceFunctionStage(draft, shape, arg);
  }
  const auto *source = Type::asyncIterator(draft.chunk);
  const auto *functionType = Type::function({source}, 1, false, returnType);
  if (returnType->isAsyncIterator()) {
    auto bound = std::vector<const Type *>{draft.chunk, returnType->yielded()};
    PipelineDraft next = draft;
    next.chunk = returnType->yielded();
    tasks.pushTask(AfterStageMade{.draft = next, .isStream = false});

    return this->pushStageCall(shape, LibraryFunction::STREAM_TRANSFORM_STAGE, std::move(bound),
                               arg, functionType);
  }
  if (returnType->isPromise() && shape.isLast) {
    return this->sinkFunctionStage(draft, shape, arg, functionType);
  }
  auto message = ErrorTextPipeline::function(shape.isLast);
  this->state.diagnostics.error(shape.loc, message);
  tasks.pushTask(AfterStageMade{.draft = draft, .isStream = false});

  return tasks.pushChecked(arg);
}

void LibraryStreamRegistry::sourceFunctionStage(const PipelineDraft &draft,
                                                const StageShapeDraft &shape, Expr *arg) {
  auto &tasks = this->state.walk.tasks;
  const Type *returnType = arg->type->ret();
  if (!returnType->isAsyncIterator()) {
    auto message = ErrorTextPipeline::source();
    this->state.diagnostics.error(shape.loc, message);
    tasks.pushTask(AfterStageMade{.draft = draft, .isStream = false});

    return tasks.pushChecked(arg);
  }
  PipelineDraft next = draft;
  next.chunk = returnType->yielded();
  const Type *functionType = Type::function({}, 0, false, returnType);
  tasks.pushTask(AfterStageMade{.draft = next, .isStream = false});

  return this->pushStageCall(shape, LibraryFunction::STREAM_SOURCE_STAGE, {next.chunk}, arg,
                             functionType);
}

void LibraryStreamRegistry::sinkFunctionStage(const PipelineDraft &draft,
                                              const StageShapeDraft &shape, Expr *arg,
                                              const Type *functionType) {
  auto &tasks = this->state.walk.tasks;
  const Type *value = arg->type->ret()->promised();
  PipelineDraft next = draft;
  next.sinkValue = value;
  tasks.pushTask(AfterStageMade{.draft = next, .isStream = false});
  if (value->isVoid()) {
    return this->pushStageCall(shape, LibraryFunction::STREAM_VOID_SINK_STAGE, {draft.chunk}, arg,
                               functionType);
  }

  return this->pushStageCall(shape, LibraryFunction::STREAM_SINK_STAGE, {draft.chunk, value}, arg,
                             functionType);
}

void LibraryStreamRegistry::iterableSourceStage(const PipelineDraft &draft,
                                                const StageShapeDraft &shape, Expr *arg) {
  auto &tasks = this->state.walk.tasks;
  const Type *type = arg->type;
  if (type->isAsyncIterator()) {
    PipelineDraft next = draft;
    next.chunk = type->yielded();
    tasks.pushTask(AfterStageMade{.draft = next, .isStream = false});

    return this->pushStageCall(shape, LibraryFunction::STREAM_ASYNC_ITERABLE_STAGE, {next.chunk},
                               arg, nullptr);
  }
  if (type->isIterator() || type->isArray()) {
    auto *iterator = this->valuesIteratorOf(arg);

    return this->iterableStage(draft, shape, iterator);
  }
  Expr **slot = &draft.stream.call->args[draft.index];
  tasks.pushTask(AfterStageIterable{.draft = draft, .shape = shape, .collection = arg});

  return tasks.pushTask(IterableOfSlot{.slot = slot});
}

void LibraryStreamRegistry::afterStageIterable(const AfterStageIterable &task) {
  const PipelineDraft &draft = task.draft;
  Expr *iterator = draft.stream.call->args[draft.index];
  bool isIterator = iterator != task.collection && iterator->type->isIterator();
  if (!isIterator) {
    return this->failStageKind(draft, task.shape, task.collection);
  }

  return this->iterableStage(draft, task.shape, iterator);
}

void LibraryStreamRegistry::iterableStage(const PipelineDraft &draft, const StageShapeDraft &shape,
                                          Expr *iterator) {
  PipelineDraft next = draft;
  next.chunk = iterator->type->yielded();
  this->state.walk.tasks.pushTask(AfterStageMade{.draft = next, .isStream = false});

  return this->pushStageCall(shape, LibraryFunction::STREAM_ITERABLE_STAGE, {next.chunk}, iterator,
                             nullptr);
}

Expr *LibraryStreamRegistry::valuesIteratorOf(Expr *collection) {
  const Type *type = collection->type;
  if (type->isIterator()) {
    return collection;
  }
  llvm::SMLoc loc = collection->loc();
  const auto *iterator = Type::iterator(type->inner());
  auto values = ArrayMethodTable::getName(ArrayMethod::VALUES);
  auto *member = this->state.ast.make<MemberExpr>(loc, collection, values);
  member->type = Type::function({}, 0, false, iterator);

  return this->state.library.common.builtinCall(loc, member, Builtin::ArrValues, {}, iterator);
}

void LibraryStreamRegistry::pushStageCall(const StageShapeDraft &shape, LibraryFunction function,
                                          std::vector<const Type *> bound, Expr *value,
                                          const Type *coerceTo) {
  auto &tasks = this->state.walk.tasks;
  tasks.pushTask(AfterStageCall{.shape = shape});
  tasks.pushTask(StartStageCall{.shape = shape, .function = function, .bound = std::move(bound)});
  if (coerceTo != nullptr) {
    tasks.pushTask(Coerce{.to = coerceTo, .what = ""});
  }

  return tasks.pushChecked(value);
}

void LibraryStreamRegistry::startStageCall(const StartStageCall &task) {
  auto &tasks = this->state.walk.tasks;
  Expr *value = tasks.takeChecked();
  llvm::SMLoc loc = task.shape.loc;
  auto functionName = LibraryFunctionTable::getName(task.function);
  auto *callee = this->state.ast.make<NameExpr>(loc, functionName);
  auto *call = this->state.ast.make<CallExpr>(loc, callee, std::vector<Expr *>{value});
  call->type = Type::error();

  return tasks.pushTask(CallLibraryFunction{
      .call = call,
      .name = functionName,
      .bound = task.bound,
      .given = {true},
      .loc = loc,
  });
}

void LibraryStreamRegistry::afterStageCall(const AfterStageCall &task) {
  auto &tasks = this->state.walk.tasks;
  Expr *made = tasks.takeChecked();
  tasks.pushChecked(made);
  if (made->type->isError()) {
    return;
  }
  const Type *stage = this->pipelineStageType(task.shape);
  tasks.pushTask(Convert{.to = stage});

  return tasks.pushTask(Convert{.to = task.shape.stageType});
}


void LibraryStreamRegistry::afterStageMade(const AfterStageMade &task) {
  PipelineDraft draft = task.draft;
  CallExpr *call = draft.stream.call;
  auto &tasks = this->state.walk.tasks;
  Expr *stage = tasks.takeChecked();
  call->args[draft.index] = stage;
  if (stage->type->isError()) {
    return this->failPipelineStage(draft);
  }
  if (draft.index + 1 == draft.count) {
    draft.isLastStream = task.isStream;
  }
  draft.index = draft.index + 1;
  tasks.pushTask(NextPipelineStage{.draft = draft});
  if (!task.isStream) {
    return;
  }

  return tasks.pushTask(NoteErrorsHandled{.expr = stage});
}


void LibraryStreamRegistry::finishPipeline(const PipelineDraft &draft) {
  const StreamFunctionDraft &stream = draft.stream;
  auto &tasks = this->state.walk.tasks;
  if (!stream.isPromises) {
    tasks.pushTask(FinishCallbackPipeline{.draft = draft});

    return this->pushArgValue(draft.callback, stream.callback);
  }
  const auto *optionsType = Type::optional(Type::object({Field{
      NodeOptionTable::getName(NodeOption::END).str(), Type::optional(Type::boolTy()), false}}));
  tasks.pushTask(FinishPromisePipeline{.draft = draft});
  if (draft.options != nullptr) {
    return this->pushArgValue(draft.options, optionsType);
  }

  return this->pushUndefinedValue(stream.loc, optionsType);
}

ArrayLitExpr *LibraryStreamRegistry::stageList(const PipelineDraft &draft, Expr *&lastStream) {
  const StreamFunctionDraft &stream = draft.stream;
  CallExpr *call = stream.call;
  auto *stageClass =
      this->state.library.declarations.findLibraryClass(LibraryClass::PIPELINE_STAGE);
  const auto *element = Type::unionOf({stream.streamType, stageClass->type});
  auto *list = this->state.ast.make<ArrayLitExpr>(stream.loc);
  for (size_t index = 0; index < draft.count; ++index) {
    Expr *stage = call->args[index];
    bool isTail = index + 1 == draft.count && draft.isLastStream && !stream.isPromises;
    if (isTail) {
      auto *conversion = dyn_cast<CastExpr>(stage);
      lastStream = conversion != nullptr ? conversion->operand : stage;
    } else {
      list->elements.push_back({stage, false});
    }
  }
  list->type = Type::array(element);

  return list;
}

void LibraryStreamRegistry::finishCallbackPipeline(const FinishCallbackPipeline &task) {
  const PipelineDraft &draft = task.draft;
  const StreamFunctionDraft &stream = draft.stream;
  CallExpr *call = stream.call;
  auto &tasks = this->state.walk.tasks;
  Expr *callback = tasks.takeChecked();
  if (callback->type->isError()) {
    return tasks.pushChecked(call);
  }
  Expr *lastStream = nullptr;
  auto *list = this->stageList(draft, lastStream);
  if (lastStream != nullptr) {
    call->args = {list, lastStream, callback};
    auto pipelineTo = LibraryMethodTable::getName(LibraryMethod::PIPELINE_TO);

    return tasks.pushTask(CallLibraryStatic{
        .call = call,
        .classDecl = stream.streamClass,
        .name = pipelineTo,
        .bound = {lastStream->type},
        .given = std::vector<bool>(3, true),
        .loc = stream.loc,
    });
  }
  call->args = {list, callback};
  auto pipelineOut = LibraryMethodTable::getName(LibraryMethod::PIPELINE_OUT);

  return tasks.pushTask(CallLibraryStatic{
      .call = call,
      .classDecl = stream.streamClass,
      .name = pipelineOut,
      .bound = {},
      .given = std::vector<bool>(2, true),
      .loc = stream.loc,
  });
}

void LibraryStreamRegistry::finishPromisePipeline(const FinishPromisePipeline &task) {
  const PipelineDraft &draft = task.draft;
  const StreamFunctionDraft &stream = draft.stream;
  CallExpr *call = stream.call;
  auto &tasks = this->state.walk.tasks;
  Expr *options = tasks.takeChecked();
  if (options->type->isError()) {
    return tasks.pushChecked(call);
  }
  Expr *lastStream = nullptr;
  auto *list = this->stageList(draft, lastStream);
  call->args = {list, options};
  const Type *sinkValue = draft.sinkValue;
  bool hasValue = sinkValue != nullptr && !sinkValue->isVoid();
  LibraryMethod method = hasValue ? LibraryMethod::PIPELINE_VALUE : LibraryMethod::PIPELINE_RESULT;
  auto name = LibraryMethodTable::getName(method);
  auto bound = hasValue ? std::vector<const Type *>{sinkValue} : std::vector<const Type *>{};

  return tasks.pushTask(CallLibraryStatic{
      .call = call,
      .classDecl = stream.streamClass,
      .name = name,
      .bound = std::move(bound),
      .given = std::vector<bool>(2, true),
      .loc = stream.loc,
  });
}

void LibraryStreamRegistry::startFsStreamFunction(CallExpr *call, FunctionDecl *functionDecl,
                                                  llvm::SMLoc loc) {
  auto &tasks = this->state.walk.tasks;
  auto function = FsStreamFunctionTable::find(functionDecl->name);
  if (!function.has_value() || this->state.library.declarations.isInsideLibrary(call->loc())) {
    return tasks.pushChecked(nullptr);
  }
  bool isReading = function == FsStreamFunction::CREATE_READ_STREAM;
  call->type = Type::error();
  if (this->state.library.common.hasSpread(call)) {
    tasks.pushChecked(call);

    return tasks.pushTask(RejectSpreadCall{.call = call});
  }
  auto arity = this->state.library.common.checkArity(call, 1, 2);
  if (arity.isErr()) {
    this->state.diagnostics.error(loc, arity.error().message);

    return tasks.pushChecked(call);
  }
  auto options = isReading ? LibraryType::READ_STREAM_OPTIONS : LibraryType::WRITE_STREAM_OPTIONS;
  auto *alias = this->state.library.declarations.findLibraryType(options);
  if (alias == nullptr) {
    return tasks.pushChecked(call);
  }
  tasks.pushTask(AfterFsStreamTypes{.call = call, .loc = loc, .isReading = isReading});
  tasks.pushTask(PreludeTypeOf{.type = PreludeType::BUFFER_ENCODING});
  tasks.pushTask(AnswerAliasType{.alias = alias});

  return tasks.pushTask(ResolveAlias{.alias = alias});
}


void LibraryStreamRegistry::afterFsStreamTypes(const AfterFsStreamTypes &task) {
  CallExpr *call = task.call;
  auto &tasks = this->state.walk.tasks;
  auto types = tasks.takeAnswers(2);
  auto draft = FsStreamDraft{
      .call = call,
      .loc = task.loc,
      .isReading = task.isReading,
      .options = Type::optional(types[0]),
      .encodingType = Type::optional(types[1]),
      .isText = false,
      .isFresh = false,
      .option = FsStreamOption::NONE,
  };
  if (call->args.size() < 2) {
    tasks.pushTask(FinishFsStream{.draft = draft});

    return tasks.pushTask(CheckArgSlot{.slot = &call->args[0], .to = Type::string()});
  }
  Expr **slot = &call->args[1];
  auto *literal = dyn_cast<ObjectLitExpr>(*slot);
  if (literal != nullptr) {
    this->scanOptionsLiteral(literal, draft);
    draft.option = FsStreamOption::OPTIONS;
    tasks.pushTask(FinishFsStream{.draft = draft});
    tasks.pushTask(CheckArgSlot{.slot = slot, .to = draft.options});
  } else if (isa<StringLitExpr>(*slot)) {
    draft.isText = true;
    draft.option = FsStreamOption::ENCODING;
    tasks.pushTask(FinishFsStream{.draft = draft});
    tasks.pushTask(CheckArgSlot{.slot = slot, .to = draft.encodingType});
  } else {
    tasks.pushTask(AfterFsStreamOption{.draft = draft});
    tasks.pushTask(CheckExprSlot{.slot = slot, .expected = nullptr});
  }

  return tasks.pushTask(CheckArgSlot{.slot = &call->args[0], .to = Type::string()});
}

void LibraryStreamRegistry::afterFsStreamOption(const AfterFsStreamOption &task) {
  FsStreamDraft draft = task.draft;
  auto &tasks = this->state.walk.tasks;
  Expr **slot = &draft.call->args[1];
  bool isText = (*slot)->type->isSubtypeOf(Type::string());
  draft.isText = draft.isText || isText;
  draft.option = isText ? FsStreamOption::ENCODING : FsStreamOption::OPTIONS;
  const Type *to = isText ? draft.encodingType : draft.options;
  tasks.pushTask(FinishFsStream{.draft = draft});

  return tasks.pushTask(CoerceSlot{.slot = slot, .to = to, .what = ""});
}

void LibraryStreamRegistry::finishFsStream(const FinishFsStream &task) {
  const FsStreamDraft &draft = task.draft;
  auto &tasks = this->state.walk.tasks;
  tasks.pushTask(FinalFsStream{.draft = draft});
  if (draft.option != FsStreamOption::ENCODING) {
    this->pushUndefinedValue(draft.loc, draft.encodingType);
  }
  if (draft.option != FsStreamOption::OPTIONS) {
    this->pushUndefinedValue(draft.loc, draft.options);
  }
}

void LibraryStreamRegistry::finalFsStream(const FinalFsStream &task) {
  const FsStreamDraft &draft = task.draft;
  CallExpr *call = draft.call;
  auto &tasks = this->state.walk.tasks;
  Expr *given = call->args.size() > 1 ? call->args[1] : nullptr;
  Expr *encoding = draft.option == FsStreamOption::ENCODING ? given : tasks.takeChecked();
  Expr *opts = draft.option == FsStreamOption::OPTIONS ? given : tasks.takeChecked();
  Expr *path = call->args[0];
  if (path->type->isError() || opts->type->isError() || encoding->type->isError()) {
    return tasks.pushChecked(call);
  }
  call->args = {path, opts, encoding};
  auto given3 = std::vector<bool>(3, true);
  if (!draft.isReading) {
    auto writer =
        draft.isFresh ? LibraryFunction::STREAM_WRITE_NEW_FILE : LibraryFunction::STREAM_WRITE_FILE;
    auto name = LibraryFunctionTable::getName(writer);

    return tasks.pushTask(CallLibraryFunction{
        .call = call,
        .name = name,
        .bound = {},
        .given = std::move(given3),
        .loc = draft.loc,
    });
  }
  const Type *chunk = draft.isText ? Type::string() : Type::bytesType(Type::Bytes::Buffer);
  auto reader = LibraryFunctionTable::getName(LibraryFunction::STREAM_READ_FILE);

  return tasks.pushTask(CallLibraryFunction{
      .call = call,
      .name = reader,
      .bound = {chunk},
      .given = std::move(given3),
      .loc = draft.loc,
  });
}

void LibraryStreamRegistry::processStream(const ProcessStream &task) {
  MemberExpr *member = task.member;
  auto &tasks = this->state.walk.tasks;
  auto stream = ProcessMemberTable::find(member->name);
  bool isIn = stream == ProcessMember::STDIN;
  auto functionName =
      ProcessFunctionTable::getName(isIn ? ProcessFunction::STDIN : ProcessFunction::STDOUT);
  if (this->state.library.declarations.findLibraryFunction(functionName) == nullptr) {
    return tasks.pushChecked(nullptr);
  }
  llvm::SMLoc loc = member->loc();
  auto args = std::vector<Expr *>();
  if (!isIn) {
    uint64_t descriptor = stream == ProcessMember::STDOUT ? 1 : 2;
    auto *number = this->state.ast.make<IntLitExpr>(loc, descriptor);
    args.push_back(number);
  }
  auto *callee = this->state.ast.make<NameExpr>(loc, functionName);
  auto *call = this->state.ast.make<CallExpr>(loc, callee, std::move(args));
  call->type = Type::error();
  auto given = std::vector<bool>(call->args.size(), true);
  tasks.pushTask(AfterProcessStreamCall{.loc = loc, .isIn = isIn});
  tasks.pushTask(CallLibraryFunction{
      .call = call,
      .name = functionName,
      .bound = {},
      .given = std::move(given),
      .loc = loc,
  });
  if (isIn) {
    return;
  }

  return tasks.pushTask(CheckExprSlot{.slot = &call->args[0], .expected = Type::intTy()});
}

void LibraryStreamRegistry::afterProcessStreamCall(const AfterProcessStreamCall &task) {
  auto &tasks = this->state.walk.tasks;
  Expr *made = tasks.takeChecked();
  tasks.pushChecked(made);
  auto &streams = this->state.library.streams;
  if (!task.isIn || !streams.hasStreamViews()) {
    return;
  }
  FieldPath path;
  path.var = this->processStdinVar();
  const auto *view = streams.findStreamView(path, task.loc);
  if (view == nullptr) {
    return;
  }

  return tasks.pushTask(Convert{.to = view});
}

void LibraryStreamRegistry::checkStreamNew(const CheckStreamNew &task) {
  NewExpr *expr = task.newExpr;
  ClassDecl *generic = task.classDecl;
  auto &tasks = this->state.walk.tasks;
  llvm::StringRef name = generic->name;
  bool isReadable = LibraryClassTable::isName(name, LibraryClass::READABLE);
  bool isWritable = LibraryClassTable::isName(name, LibraryClass::WRITABLE);
  bool isDuplex = LibraryClassTable::isName(name, LibraryClass::DUPLEX) ||
                  LibraryClassTable::isName(name, LibraryClass::TRANSFORM) ||
                  LibraryClassTable::isName(name, LibraryClass::PASS_THROUGH);
  bool isLibraryNew = this->state.library.declarations.isInsideLibrary(expr->loc());
  if ((!isReadable && !isWritable && !isDuplex) || isLibraryNew) {
    return tasks.pushChecked(nullptr);
  }
  auto concrete = this->checkConcreteStream(name, isReadable, isWritable);
  if (concrete.isErr()) {
    return this->failStreamNew(expr, concrete.error().message);
  }
  auto given = this->checkChunksGiven(expr, generic, task.expected);
  if (given.isErr()) {
    return this->failStreamNew(expr, given.error().message);
  }

  return tasks.pushChecked(nullptr);
}

void LibraryStreamRegistry::failStreamNew(NewExpr *expr, const std::string &message) {
  auto &tasks = this->state.walk.tasks;
  this->state.diagnostics.error(expr->loc(), message);
  expr->type = Type::error();
  tasks.pushChecked(expr);
  for (Expr *&arg : llvm::reverse(expr->args)) {
    tasks.pushTask(CheckExprSlot{.slot = &arg, .expected = nullptr});
  }
}

void LibraryStreamRegistry::checkStreamFrom(const CheckStreamFrom &task) {
  CallExpr *call = task.call;
  MemberExpr *member = task.member;
  auto &tasks = this->state.walk.tasks;
  call->type = Type::error();
  if (this->state.library.common.hasSpread(call)) {
    tasks.pushChecked(call);

    return tasks.pushTask(RejectSpreadCall{.call = call});
  }
  auto arity = this->state.library.common.checkArity(call, 1, 2);
  if (arity.isErr()) {
    this->state.diagnostics.error(member->loc(), arity.error().message);

    return tasks.pushChecked(call);
  }
  tasks.pushTask(AfterFromSource{.call = call, .member = member});

  return tasks.pushTask(CheckExprSlot{.slot = &call->args[0], .expected = nullptr});
}


void LibraryStreamRegistry::afterFromSource(const AfterFromSource &task) {
  auto &tasks = this->state.walk.tasks;
  auto *alias =
      this->state.library.declarations.findLibraryType(LibraryType::READABLE_FROM_OPTIONS);
  if (task.call->args[0]->type->isError() || alias == nullptr) {
    return tasks.pushChecked(task.call);
  }
  tasks.pushTask(AfterFromOptionsType{.call = task.call, .member = task.member});
  tasks.pushTask(AnswerAliasType{.alias = alias});

  return tasks.pushTask(ResolveAlias{.alias = alias});
}


void LibraryStreamRegistry::afterFromOptionsType(const AfterFromOptionsType &task) {
  CallExpr *call = task.call;
  auto &tasks = this->state.walk.tasks;
  const Type *resolved = tasks.takeAnswer();
  const Type *options = Type::optional(resolved);
  tasks.pushTask(AfterFromOptions{.call = call, .member = task.member});
  if (call->args.size() > 1) {
    return tasks.pushTask(CheckArgSlot{.slot = &call->args[1], .to = options});
  }
  auto *nothing = this->state.ast.make<UndefinedExpr>(task.member->loc());
  call->args.push_back(nothing);
  tasks.pushTask(ConvertSlot{.slot = &call->args[1], .to = options});

  return tasks.pushTask(CheckExprSlot{.slot = &call->args[1], .expected = nullptr});
}

void LibraryStreamRegistry::afterFromOptions(const AfterFromOptions &task) {
  CallExpr *call = task.call;
  auto &tasks = this->state.walk.tasks;
  if (call->args[1]->type->isError()) {
    return tasks.pushChecked(call);
  }
  llvm::SMLoc loc = task.member->loc();
  Expr *source = call->args[0];
  const Type *type = source->type;
  if (type->isString() || type->isStringLiteral() || type->isBuffer()) {
    const Type *chunk = type->isBuffer() ? type : Type::string();
    this->pushFromFunction(call, LibraryFunction::STREAM_FROM_VALUE, chunk, loc);

    return tasks.pushTask(CoerceSlot{.slot = &call->args[0], .to = chunk, .what = ""});
  }
  if (type->isAsyncIterator()) {
    return this->pushFromFunction(call, LibraryFunction::STREAM_FROM_ASYNC_ITERATOR,
                                  type->yielded(), loc);
  }
  tasks.pushTask(AfterFromAsync{.call = call, .member = task.member});

  return tasks.pushTask(AsyncUserIterator{.expr = source});
}

void LibraryStreamRegistry::afterFromAsync(const AfterFromAsync &task) {
  CallExpr *call = task.call;
  auto &tasks = this->state.walk.tasks;
  llvm::SMLoc loc = task.member->loc();
  Expr *async = tasks.takeChecked();
  if (async != nullptr && async->type->isAsyncIterator()) {
    call->args[0] = async;

    return this->pushFromFunction(call, LibraryFunction::STREAM_FROM_ASYNC_ITERATOR,
                                  async->type->yielded(), loc);
  }
  Expr *source = call->args[0];
  const Type *type = source->type;
  if (type->isIterator() || type->isArray()) {
    call->args[0] = this->valuesIteratorOf(source);

    return this->finishFromIterator(call, loc);
  }
  tasks.pushTask(AfterFromIterable{.call = call, .member = task.member, .source = source});

  return tasks.pushTask(IterableOfSlot{.slot = &call->args[0]});
}

void LibraryStreamRegistry::afterFromIterable(const AfterFromIterable &task) {
  CallExpr *call = task.call;
  Expr *iterator = call->args[0];
  bool isIterator = iterator != task.source && iterator->type->isIterator();
  if (!isIterator) {
    auto message = ErrorTextStreamFrom::source(task.source->type->name());
    this->state.diagnostics.error(task.source->loc(), message);

    return this->state.walk.tasks.pushChecked(call);
  }

  return this->finishFromIterator(call, task.member->loc());
}

void LibraryStreamRegistry::finishFromIterator(CallExpr *call, llvm::SMLoc loc) {
  const Type *yielded = call->args[0]->type->yielded();

  return this->pushFromFunction(call, LibraryFunction::STREAM_FROM_ITERATOR, yielded, loc);
}

void LibraryStreamRegistry::pushFromFunction(CallExpr *call, LibraryFunction function,
                                             const Type *chunk, llvm::SMLoc loc) {
  auto name = LibraryFunctionTable::getName(function);

  return this->state.walk.tasks.pushTask(CallLibraryFunction{
      .call = call,
      .name = name,
      .bound = {chunk},
      .given = {true, true},
      .loc = loc,
  });
}

bool LibraryStreamRegistry::isStreamClass(const ClassDecl *classDecl) const {
  auto *stream = this->state.library.declarations.findLibraryClass(LibraryClass::STREAM);

  return classDecl != nullptr && stream != nullptr && classDecl->derivesFrom(stream);
}

LibraryStreamRegistry::StreamChunks
LibraryStreamRegistry::streamChunksOf(const ClassDecl *classDecl) const {
  StreamChunks chunks;
  for (const ClassDecl *current = classDecl; current != nullptr; current = current->parent) {
    if (this->isTypedLibraryStream(current)) {
      this->addStreamChunk(chunks, current);
    }
  }

  return chunks;
}

bool LibraryStreamRegistry::isTypedLibraryStream(const ClassDecl *classDecl) const {
  return classDecl->library() && classDecl->generic() != nullptr && !classDecl->typeArgs.empty();
}

void LibraryStreamRegistry::addStreamChunk(StreamChunks &chunks, const ClassDecl *classDecl) const {
  llvm::StringRef name = classDecl->generic()->name;
  bool isReadable = LibraryClassTable::isName(name, LibraryClass::READABLE);
  bool isWritable = LibraryClassTable::isName(name, LibraryClass::WRITABLE);
  bool isDuplex = LibraryClassTable::isName(name, LibraryClass::DUPLEX);
  if (isReadable && chunks.readable == nullptr) {
    chunks.readable = classDecl->typeArgs[0];
  } else if (isWritable && chunks.writable == nullptr) {
    chunks.writable = classDecl->typeArgs[0];
  } else if (isDuplex && classDecl->typeArgs.size() >= 2 && chunks.writable == nullptr) {
    chunks.writable = classDecl->typeArgs[0];
  }
}

const ClassDecl *LibraryStreamRegistry::libraryBase(const ClassDecl *classDecl) const {
  const ClassDecl *lib = classDecl;
  while (lib != nullptr && !lib->library()) {
    lib = lib->parent;
  }
  if (lib == nullptr || lib->generic() == nullptr) {
    return nullptr;
  }

  return lib;
}

std::optional<StreamSidesDraft> LibraryStreamRegistry::streamSides(const ClassDecl *lib,
                                                                   const Type *streamError) const {
  auto kind = StreamKindTable::find(lib->generic()->name);
  if (!kind.has_value()) {
    return std::nullopt;
  }
  const std::vector<const Type *> &args = lib->typeArgs;
  switch (*kind) {
    case StreamKind::READABLE:
      if (args.size() != 2) {
        return std::nullopt;
      }
      return StreamSidesDraft{
          .readable = args[0],
          .writable = nullptr,
          .error = args[1],
          .isTransform = false,
      };
    case StreamKind::WRITABLE:
      if (args.size() != 2) {
        return std::nullopt;
      }
      return StreamSidesDraft{
          .readable = nullptr,
          .writable = args[0],
          .error = args[1],
          .isTransform = false,
      };
    case StreamKind::DUPLEX:
    case StreamKind::TRANSFORM:
      if (args.size() != 3) {
        return std::nullopt;
      }
      return StreamSidesDraft{
          .readable = args[1],
          .writable = args[0],
          .error = args[2],
          .isTransform = *kind == StreamKind::TRANSFORM,
      };
    case StreamKind::PASS_THROUGH:
      if (args.size() != 2) {
        return std::nullopt;
      }
      return StreamSidesDraft{
          .readable = args[1],
          .writable = args[0],
          .error = streamError,
          .isTransform = true,
      };
  }

  return std::nullopt;
}

const FuncSymbol *
LibraryStreamRegistry::libraryHook(llvm::ArrayRef<FunctionDecl *> inherited) const {
  for (const FunctionDecl *method : llvm::reverse(inherited)) {
    if (method->sym != nullptr && method->cls() != nullptr && method->cls()->library()) {
      return method->sym;
    }
  }

  return nullptr;
}

bool LibraryStreamRegistry::isUntypedParam(const Type *type) const {
  return type == nullptr || type->isError();
}

bool LibraryStreamRegistry::isStreamHook(const FunctionDecl *functionDecl) const {
  const ClassDecl *classDecl = functionDecl->cls();
  bool isUserMethod = classDecl != nullptr && !functionDecl->isStatic() && !classDecl->library() &&
                      classDecl->literal() == nullptr && classDecl->parent != nullptr;
  auto hook = StreamHookTable::find(functionDecl->name);

  return isUserMethod && hook.has_value();
}

LibraryStreamRegistry::ResultHookParamsMatch
LibraryStreamRegistry::checkHookParams(const FunctionDecl *functionDecl,
                                       llvm::ArrayRef<FunctionDecl *> inherited) const {
  const auto *hook = this->libraryHook(inherited);
  if (hook == nullptr) {
    return err(LibraryStreamRegistryHookParamsError{
        .code = LibraryStreamRegistryHookParamsErrorCode::NO_LIBRARY_HOOK,
        .message = "",
    });
  }
  auto isUntyped = std::bind_front(&LibraryStreamRegistry::isUntypedParam, this);
  if (llvm::any_of(functionDecl->sym->params, isUntyped)) {
    return err(LibraryStreamRegistryHookParamsError{
        .code = LibraryStreamRegistryHookParamsErrorCode::PARAMS_NOT_TYPED,
        .message = "",
    });
  }
  auto named = StreamHookTable::find(functionDecl->name);
  bool hasChunks = named == StreamHook::ON_WRITE || named == StreamHook::ON_WRITEV ||
                   named == StreamHook::ON_TRANSFORM;
  auto params = ErrorTextStreamHook::params(hook);
  auto message = ErrorTextStreamHook::other(functionDecl->name, params, hasChunks);

  return err(LibraryStreamRegistryHookParamsError{
      .code = LibraryStreamRegistryHookParamsErrorCode::OTHER_PARAMETERS,
      .message = message,
  });
}

std::vector<const Type *> LibraryStreamRegistry::encodedArgs(const ClassDecl *classDecl) const {
  if (!classDecl->library() || classDecl->generic() == nullptr || classDecl->typeArgs.empty()) {
    return {};
  }
  llvm::StringRef name = classDecl->generic()->name;
  if (LibraryClassTable::isName(name, LibraryClass::READABLE)) {
    return {Type::string()};
  }
  bool isDuplex = LibraryClassTable::isName(name, LibraryClass::DUPLEX) ||
                  LibraryClassTable::isName(name, LibraryClass::TRANSFORM) ||
                  LibraryClassTable::isName(name, LibraryClass::PASS_THROUGH);
  if (isDuplex) {
    return {classDecl->typeArgs[0], Type::string()};
  }

  return {};
}

LibraryStreamRegistry::ResultDecodedChunks
LibraryStreamRegistry::checkDecodedChunks(const Type *readable, const Type *receiver) const {
  if (readable == nullptr) {
    auto message = ErrorTextSetEncoding::unreadable(receiver->name());

    return err(LibraryStreamRegistryDecodedChunksError{
        .code = LibraryStreamRegistryDecodedChunksErrorCode::NOT_READABLE,
        .message = message,
    });
  }
  if (!readable->isString() && !readable->isBuffer()) {
    auto message = ErrorTextSetEncoding::objectMode(readable->name());

    return err(LibraryStreamRegistryDecodedChunksError{
        .code = LibraryStreamRegistryDecodedChunksErrorCode::OBJECT_MODE,
        .message = message,
    });
  }

  return ok(readable);
}

LibraryStreamRegistry::ResultStdinIsPlaced LibraryStreamRegistry::checkStdinPlace() const {
  const auto *module = this->state.declaration.modules.getCurrentModule();
  bool isTopLevel = module != nullptr && module->isEntry &&
                    !this->state.walk.functions.isInsideFunction() &&
                    this->state.walk.scopes.isTopLevel();
  if (!isTopLevel) {
    auto message = ErrorTextSetEncoding::stdinPlace();

    return err(LibraryStreamRegistryStdinPlaceError{
        .code = LibraryStreamRegistryStdinPlaceErrorCode::NOT_TOP_LEVEL,
        .message = message,
    });
  }

  return ok(module);
}

const Expr *LibraryStreamRegistry::withoutImplicit(const Expr *expr) const {
  const Expr *bare = expr;
  auto *conversion = dyn_cast<CastExpr>(bare);
  while (conversion != nullptr && conversion->isImplicit()) {
    bare = conversion->operand;
    conversion = dyn_cast<CastExpr>(bare);
  }

  return bare;
}

bool LibraryStreamRegistry::isStdinCall(const Expr *expr) const {
  auto *call = dyn_cast<CallExpr>(expr);

  auto stdinName = ProcessFunctionTable::getName(ProcessFunction::STDIN);

  return call != nullptr && call->func() != nullptr && call->func()->name == stdinName;
}

std::optional<FieldPath> LibraryStreamRegistry::encodingPath(const Expr *object, bool stdinStream) {
  if (stdinStream) {
    FieldPath stdinPath;
    stdinPath.var = this->processStdinVar();
    return stdinPath;
  }
  auto *name = dyn_cast<NameExpr>(object);
  if (name != nullptr && name->var() != nullptr) {
    FieldPath namePath;
    namePath.var = name->var();
    return namePath;
  }

  return this->state.flow.tracked.fieldPath(object);
}

std::optional<std::string>
LibraryStreamRegistry::encodingPathProblem(const std::optional<FieldPath> &path,
                                           bool stdinStream) const {
  if (!path.has_value() || path->var == nullptr) {
    return ErrorTextEncodingWhy::noConstant();
  }
  const VarSymbol *root = path->var;
  if (!root->isConst) {
    return ErrorTextEncodingWhy::notConst(root->name);
  }
  auto field = this->writableField(*path);
  if (field.has_value()) {
    return ErrorTextEncodingWhy::fieldSet(*field);
  }
  if (!this->isOwnBlock(root, stdinStream)) {
    return ErrorTextEncodingWhy::ownBlock(root->name);
  }

  return std::nullopt;
}

std::optional<llvm::StringRef> LibraryStreamRegistry::writableField(const FieldPath &path) const {
  const Type *current = path.var->type;
  for (llvm::StringRef field : path.names) {
    const Field *found = current != nullptr ? current->nonOptional()->field(field) : nullptr;
    if (found == nullptr || !found->readonly) {
      return field;
    }
    current = found->type;
  }

  return std::nullopt;
}

bool LibraryStreamRegistry::isOwnBlock(const VarSymbol *root, bool stdinStream) const {
  auto &scopes = this->state.walk.scopes;
  size_t declared = stdinStream ? 0 : scopes.getScopeDepth();
  if (!stdinStream) {
    auto found = scopes.findDeclaringScope(root);
    if (found.has_value()) {
      declared = *found;
    }
  }
  bool isBranched = stdinStream ? scopes.isInsideBranch() : scopes.isBranchedSince(declared);

  return declared + 1 == scopes.getScopeDepth() && !isBranched;
}

void LibraryStreamRegistry::markEscapeWindow(EncodingRun &run) const {
  const Expr *base = run.member->object;
  for (;;) {
    auto *conversion = dyn_cast<CastExpr>(base);
    auto *member = dyn_cast<MemberExpr>(base);
    if (conversion != nullptr) {
      base = conversion->operand;
    } else if (member != nullptr) {
      base = member->object;
    } else {
      break;
    }
  }
  run.here = base->loc().getPointer();
  run.start = run.stdinStream ? nullptr : run.path.var->loc.getPointer();
}

bool LibraryStreamRegistry::isBefore(const EncodingRun &run, const Expr *expr) const {
  const char *at = expr->loc().getPointer();

  return at > run.start && at < run.here;
}

bool LibraryStreamRegistry::isEncodingPath(const EncodingRun &run, const Expr *expr) const {
  const auto *bare = this->withoutImplicit(expr);
  if (run.stdinStream) {
    if (isa<CallExpr>(bare)) {
      return this->isStdinCall(bare);
    }
    return this->state.library.common.isProcessMember(bare, ProcessMember::STDIN);
  }
  const VarSymbol *root = run.path.var;
  if (run.path.names.empty()) {
    auto *name = dyn_cast<NameExpr>(bare);
    return name != nullptr &&
           (name->var() == root || (name->var() == nullptr && name->name == root->name));
  }
  auto path = this->state.flow.tracked.fieldPath(bare);

  return path.has_value() && *path == run.path;
}

void LibraryStreamRegistry::collectEscapes(EncodingRun &run) {
  auto body = this->encodingBody(run);
  for (const Stmt *stmt : body) {
    for (const Expr *expr : AstWalker::allExprsIn(stmt)) {
      this->classifyUse(run, expr);
    }
  }
  auto direct = llvm::SmallPtrSet<const Expr *, 32>();
  for (const Stmt *stmt : body) {
    auto exprs = AstWalker::ownExprsIn(stmt);
    direct.insert(exprs.begin(), exprs.end());
  }
  for (const Stmt *stmt : body) {
    for (const Expr *expr : AstWalker::allExprsIn(stmt)) {
      this->noteEscape(run, expr, direct);
    }
  }
}

std::vector<const Stmt *> LibraryStreamRegistry::encodingBody(const EncodingRun &run) const {
  auto body = std::vector<const Stmt *>();
  FunctionDecl *owner = run.stdinStream ? nullptr : run.path.var->owner();
  if (owner != nullptr && owner->body != nullptr) {
    body.push_back(owner->body);
    return body;
  }
  auto *module = this->state.declaration.modules.getCurrentModule();
  if (module != nullptr) {
    for (const Stmt *item : module->file.items) {
      body.push_back(item);
    }
  }

  return body;
}

void LibraryStreamRegistry::classifyUse(EncodingRun &run, const Expr *expr) const {
  if (!this->isBefore(run, expr)) {
    return;
  }
  auto *call = dyn_cast<CallExpr>(expr);
  if (call != nullptr) {
    auto *member = dyn_cast<MemberExpr>(call->callee);
    if (member == nullptr || !this->isEncodingPath(run, member->object)) {
      return;
    }
    if (this->isListeningMethod(member->name)) {
      run.escapes.push_back({member->loc(), ErrorTextEncodingUse::listeners(member->name)});
    } else {
      run.safe.insert(this->withoutImplicit(member->object));
    }
    return;
  }
  auto *member = dyn_cast<MemberExpr>(expr);
  if (member != nullptr && this->isEncodingPath(run, member->object)) {
    run.safe.insert(this->withoutImplicit(member->object));
  }
}

void LibraryStreamRegistry::noteEscape(EncodingRun &run, const Expr *expr,
                                       const llvm::SmallPtrSetImpl<const Expr *> &direct) const {
  bool isCandidate =
      this->isBefore(run, expr) && !isa<CastExpr>(expr) && this->isEncodingPath(run, expr);
  if (!isCandidate) {
    return;
  }
  if (!direct.contains(expr)) {
    run.escapes.push_back({expr->loc(), ErrorTextEncodingUse::function()});
  } else if (!run.safe.contains(expr)) {
    run.escapes.push_back({expr->loc(), ErrorTextEncodingUse::elsewhere()});
  }
}

void LibraryStreamRegistry::reportFirstEscape(const EncodingRun &run, llvm::SMLoc loc) {
  const StreamEscape *first = &run.escapes[0];
  for (const StreamEscape &escape : run.escapes) {
    if (escape.loc.getPointer() < first->loc.getPointer()) {
      first = &escape;
    }
  }
  auto message = ErrorTextSetEncoding::usedBefore(first->how);
  auto usedHere = ErrorTextSetEncoding::usedHere();
  this->state.diagnostics.error(loc, message);
  this->state.diagnostics.note(first->loc, usedHere);
}

bool LibraryStreamRegistry::isListeningMethod(llvm::StringRef name) const {
  auto member = EmitterMemberTable::find(name);
  bool isListen = member == EmitterMember::ON || member == EmitterMember::ONCE ||
                  member == EmitterMember::ADD_LISTENER ||
                  member == EmitterMember::PREPEND_LISTENER ||
                  member == EmitterMember::PREPEND_ONCE_LISTENER;

  return isListen || StreamMethodTable::find(name) == StreamMethod::PIPE;
}

bool LibraryStreamRegistry::isByteChunk(const Type *type) const {
  return type->isBuffer() || type->isString();
}

bool LibraryStreamRegistry::isNativeIo(Builtin builtin) const {
  return builtin == Builtin::LibFsOpen || builtin == Builtin::LibFsRead ||
         builtin == Builtin::LibFsWrite || builtin == Builtin::LibFsClose ||
         builtin == Builtin::LibStdinChunk || builtin == Builtin::LibStdWrite;
}

LibraryStreamRegistry::NativeIoSignature
LibraryStreamRegistry::nativeIoSignature(Builtin builtin, const Type *failure) const {
  const auto *buffer = Type::bytesType(Type::Bytes::Buffer);
  const auto *counted = Type::promise(Type::result(Type::intTy(), failure));
  if (builtin == Builtin::LibFsOpen) {
    return NativeIoSignature{
        .params = {Type::string(), Type::string(), Type::intTy()},
        .result = counted,
    };
  }
  if (builtin == Builtin::LibFsRead || builtin == Builtin::LibFsWrite) {
    return NativeIoSignature{
        .params = {Type::intTy(), buffer, Type::intTy(), Type::intTy(), Type::doubleTy()},
        .result = counted,
    };
  }
  if (builtin == Builtin::LibFsClose) {
    return NativeIoSignature{
        .params = {Type::intTy(), Type::boolTy()},
        .result = counted,
    };
  }
  if (builtin == Builtin::LibStdinChunk) {
    return NativeIoSignature{
        .params = {Type::intTy()},
        .result = Type::promise(Type::optional(buffer)),
    };
  }

  return NativeIoSignature{
      .params = {Type::intTy(), buffer},
      .result = Type::intTy(),
  };
}

LibraryStreamRegistry::ResultOverrideArgs
LibraryStreamRegistry::checkOverridesArgs(const ClassDecl *classDecl,
                                          const StringLitExpr *literal) const {
  if (classDecl == nullptr || literal == nullptr) {
    auto message = ErrorTextStreamCall::overrides();

    return err(LibraryStreamRegistryOverridesArgsError{
        .code = LibraryStreamRegistryOverridesArgsErrorCode::NOT_CLASS_AND_LITERAL,
        .message = message,
    });
  }

  return ok(classDecl);
}

LibraryStreamRegistry::ResultMethodExists
LibraryStreamRegistry::checkOverriddenMethod(const ClassDecl *classDecl,
                                             llvm::ArrayRef<FunctionDecl *> methods,
                                             llvm::StringRef method) const {
  if (methods.empty() || methods[0]->sym == nullptr) {
    auto message = ErrorTextStreamCall::noMethod(classDecl->name, method);

    return err(LibraryStreamRegistryOverriddenMethodError{
        .code = LibraryStreamRegistryOverriddenMethodErrorCode::NO_SUCH_METHOD,
        .message = message,
    });
  }

  return ok(methods[0]);
}

LibraryStreamRegistry::ResultChunkIsGiven
LibraryStreamRegistry::checkChunkNotUndefined(const Expr *arg, StreamChunkSide side,
                                              llvm::StringRef method) const {
  if (side == StreamChunkSide::PUSHED && isa<UndefinedExpr>(arg)) {
    auto message = ErrorTextStreamCall::undefined(method);

    return err(LibraryStreamRegistryChunkNotUndefinedError{
        .code = LibraryStreamRegistryChunkNotUndefinedErrorCode::UNDEFINED_CHUNK,
        .message = message,
    });
  }

  return ok(arg);
}

LibraryStreamRegistry::ResultStreamSideOk
LibraryStreamRegistry::checkStreamSide(const Type *chunk, const Type *receiver,
                                       llvm::StringRef name, bool reading) const {
  if (chunk == nullptr && reading) {
    auto message = ErrorTextStreamCall::noSide(receiver->name(), name, reading);

    return err(LibraryStreamRegistryStreamSideError{
        .code = LibraryStreamRegistryStreamSideErrorCode::NOT_READABLE,
        .message = message,
    });
  }
  if (chunk == nullptr) {
    auto message = ErrorTextStreamCall::noSide(receiver->name(), name, reading);

    return err(LibraryStreamRegistryStreamSideError{
        .code = LibraryStreamRegistryStreamSideErrorCode::NOT_WRITABLE,
        .message = message,
    });
  }

  return ok(chunk);
}

bool LibraryStreamRegistry::isChunkValue(bool isOnlyStrings, const Type *candidate) const {
  const Type *base = candidate->isLiteral() ? candidate->literalBase() : candidate;
  if (base->isString()) {
    return true;
  }

  return !isOnlyStrings && (base->isBuffer() || base->bytes() == Type::Bytes::Uint8);
}

unsigned LibraryStreamRegistry::chunkKind(const Type *typeArg) const {
  if (typeArg->isBuffer()) {
    return 0;
  }
  if (typeArg->isString()) {
    return 1;
  }

  return 2;
}

LibraryStreamRegistry::ResultConcreteStream
LibraryStreamRegistry::checkConcreteStream(llvm::StringRef name, bool readable,
                                           bool writable) const {
  if (!LibraryClassTable::isName(name, LibraryClass::PASS_THROUGH)) {
    auto message = ErrorTextStreamNew::abstract(name, readable, writable);

    return err(LibraryStreamRegistryConcreteStreamError{
        .code = LibraryStreamRegistryConcreteStreamErrorCode::ABSTRACT_STREAM,
        .message = message,
    });
  }

  return ok(name);
}

LibraryStreamRegistry::ResultChunkTypeGiven
LibraryStreamRegistry::checkChunksGiven(const NewExpr *expr, const ClassDecl *generic,
                                        const Type *expected) const {
  const Type *want = expected != nullptr ? expected->nonOptional() : nullptr;
  const ClassDecl *wanted = want != nullptr ? want->classDecl() : nullptr;
  bool isFromExpected = wanted != nullptr && wanted->generic() == generic;
  if (expr->typeArgs.empty() && !isFromExpected) {
    auto message = ErrorTextStreamNew::untyped(generic->name);

    return err(LibraryStreamRegistryChunksGivenError{
        .code = LibraryStreamRegistryChunksGivenErrorCode::CHUNK_TYPE_MISSING,
        .message = message,
    });
  }

  return ok(expr);
}

bool LibraryStreamRegistry::isInModule(const std::string &place, llvm::StringRef module) const {
  llvm::StringRef at(place);

  return at.consume_front(module) && at.consume_front(":");
}

LibraryStreamRegistry::ResultWritableEnd
LibraryStreamRegistry::checkWritableEnd(const StreamChunks &chunks, const Type *type) const {
  if (chunks.writable == nullptr) {
    auto message = ErrorTextPipeline::notWritten(type->name());

    return err(LibraryStreamRegistryWritableEndError{
        .code = LibraryStreamRegistryWritableEndErrorCode::NOT_WRITABLE,
        .message = message,
    });
  }

  return ok(chunks.writable);
}

LibraryStreamRegistry::ResultReadableEnd
LibraryStreamRegistry::checkReadableEnd(const StreamChunks &chunks, const Type *type) const {
  if (chunks.readable == nullptr) {
    auto message = ErrorTextPipeline::notRead(type->name());

    return err(LibraryStreamRegistryReadableEndError{
        .code = LibraryStreamRegistryReadableEndErrorCode::NOT_READABLE,
        .message = message,
    });
  }

  return ok(chunks.readable);
}

bool LibraryStreamRegistry::isStreamTaking(const Type *writable, const Type *chunk) {
  if (writable == chunk) {
    return true;
  }
  if (writable->isBuffer() && chunk->isString()) {
    return true;
  }

  return this->state.types.relations.isImplicitlyConvertible(chunk, writable);
}

void LibraryStreamRegistry::scanOptionsLiteral(const ObjectLitExpr *literal,
                                               FsStreamDraft &args) const {
  for (const ObjectLitExpr::Property &property : literal->props) {
    auto option = property.spread() ? std::nullopt : NodeOptionTable::find(property.name);
    if (option == NodeOption::ENCODING && !isa<UndefinedExpr>(property.value)) {
      args.isText = true;
    }
    auto *flags = option == NodeOption::FLAGS ? dyn_cast<StringLitExpr>(property.value) : nullptr;
    if (flags != nullptr && flags->value.find(u'x') != std::u16string::npos) {
      args.isFresh = true;
    }
  }
}

VarSymbol *LibraryStreamRegistry::processStdinVar() {
  auto &streams = this->state.library.streams;
  if (streams.getProcessStdin() == nullptr) {
    auto *stdinVar = this->state.ast.newVar();
    stdinVar->name = ProcessQualifiedNameTable::getQualifiedName(ProcessMember::STDIN);
    stdinVar->isConst = true;
    streams.rememberProcessStdin(stdinVar);
  }

  return streams.getProcessStdin();
}

}
