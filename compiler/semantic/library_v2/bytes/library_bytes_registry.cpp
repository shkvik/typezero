#include "compiler/semantic/library_v2/bytes/library_bytes_registry.h"
#include "compiler/program/library/builtin_names/global_member_table/global_member_table.h"
#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/program/library/bytes/buffer_static_table/buffer_static_table.h"
#include "compiler/program/library/bytes/bytes_shape_field_table/bytes_shape_field_table.h"
#include "compiler/program/library/bytes/encoding_name_table/encoding_name_table.h"
#include "compiler/program/to_utf16/to_utf16.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "compiler/semantic/flow/expr_parts/expr_parts.h"
#include "compiler/semantic/flow_v2/function_facts/flow_function_facts_registry_errors.h"
#include "runtime/library/bytes/encodings/encodings.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include <cstring>
#include <functional>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

namespace {

constexpr llvm::StringLiteral kValuePlaceholder = "x";

}

LibraryBytesRegistry::LibraryBytesRegistry(SemanticState &owner)
    : state(owner) {}

void LibraryBytesRegistry::checkBytesIndex(const CheckBytesIndex &task) {
  auto &tasks = this->state.walk.tasks;
  tasks.pushTask(AfterBytesIndexPosition{.index = task.index});

  return tasks.pushTask(CheckPosition{.slot = &task.index->index});
}

void LibraryBytesRegistry::afterBytesIndexPosition(const AfterBytesIndexPosition &task) {
  IndexExpr *index = task.index;
  auto &tasks = this->state.walk.tasks;
  const Type *type = index->object->type;
  auto indexing = this->checkBytesIndexing(type);
  if (indexing.isErr()) {
    this->state.diagnostics.error(index->loc(), indexing.error().message);

    return tasks.pushChecked(index);
  }
  if (index->index->type->isError()) {
    return tasks.pushChecked(index);
  }
  bool isInRange = this->state.flow.proofs.canProveInBounds(index->object, index->index);
  ElementIndex *element = index->form.as<ElementIndex>();
  if (element != nullptr) {
    element->mayBeOutOfRange = !isInRange;
  }
  index->type = isInRange ? type->elementType() : Type::optional(type->elementType());

  return tasks.pushChecked(index);
}

void LibraryBytesRegistry::walkBytesStore(const WalkBytesStore &task) {
  IndexExpr *index = task.index;
  const Expr *value = task.value;
  const Type *target = index->object->type;
  if (index->index->type->isError() || (value != nullptr && value->type->isError())) {
    return;
  }
  if (this->state.flow.proofs.canProveInBounds(index->object, index->index)) {
    return;
  }
  auto object = BytesCheckHelpers::valueText(index->object);
  auto position = BytesCheckHelpers::valueText(index->index);
  auto at = this->state.code.constants.intConstant(index->index);
  auto message = ErrorTextBytesIndex::store(target->name(), object, position, at);
  this->state.diagnostics.error(index->loc(), message);
}

void LibraryBytesRegistry::checkBytesMember(const CheckBytesMember &task) {
  auto *checked = this->bytesMember(task.member);

  return this->state.walk.tasks.pushChecked(checked);
}

void LibraryBytesRegistry::checkBytesUnionMember(const CheckBytesUnionMember &task) {
  auto *checked = this->bytesUnionMember(task.member);

  return this->state.walk.tasks.pushChecked(checked);
}

void LibraryBytesRegistry::checkBytesStaticMember(const CheckBytesStaticMember &task) {
  auto *checked = this->bytesStaticMember(task.member);

  return this->state.walk.tasks.pushChecked(checked);
}

void LibraryBytesRegistry::bytesToString() {
  auto &tasks = this->state.walk.tasks;
  Expr *expr = tasks.takeChecked();
  auto *text = this->bytesText(expr);

  return tasks.pushChecked(text);
}

void LibraryBytesRegistry::bufferEncodingValue(const BufferEncodingValue &task) {
  Expr *arg = task.arg;
  auto &tasks = this->state.walk.tasks;
  auto text = this->literalText(arg);
  if (!text.has_value()) {
    tasks.pushTask(AfterBufferEncodingArg{});
    tasks.pushTask(Coerce{.to = Type::string(), .what = ""});

    return tasks.pushTask(CheckExpr{.expr = arg, .expected = Type::string()});
  }
  auto written = ToUtf8::toUtf8(*text);
  auto encoding = this->state.library.common.checkKnownEncoding(written);
  if (encoding.isErr()) {
    this->state.diagnostics.error(arg->loc(), encoding.error().message);
    arg->type = Type::error();

    return tasks.pushChecked(arg);
  }
  auto *literal = this->makeIntLiteral(arg->loc(), encoding.value());

  return tasks.pushChecked(literal);
}

void LibraryBytesRegistry::afterBufferEncodingArg() {
  auto &tasks = this->state.walk.tasks;
  Expr *arg = tasks.takeChecked();
  tasks.pushChecked(arg);
  if (arg->type->isError() || this->state.flow.proofs.isExempt(arg->loc())) {
    return;
  }
  auto names = this->state.library.common.knownTexts(arg);
  if (!names.has_value()) {
    auto message = ErrorTextBufferEncoding::notKnown();

    return this->state.diagnostics.error(arg->loc(), message);
  }
  for (const std::string &name : *names) {
    auto encoding = this->state.library.common.checkKnownEncoding(name);
    if (encoding.isErr()) {
      return this->state.diagnostics.error(arg->loc(), encoding.error().message);
    }
  }
}

void LibraryBytesRegistry::checkBytesMethod(const CheckBytesMethod &task) {
  CallExpr *call = task.call;
  MemberExpr *member = task.member;
  auto &tasks = this->state.walk.tasks;
  const Type *type = member->object->type;
  call->type = Type::error();
  if (this->state.library.common.hasSpread(call)) {
    tasks.pushChecked(call);

    return tasks.pushTask(RejectSpreadCall{.call = call});
  }
  switch (type->bytes()) {
    case Bytes::ArrayBuffer:
      return this->startArrayBufferMethod(call, member);
    case Bytes::DataView:
      return this->startDataViewMethod(call, member);
    case Bytes::TextEncoder:
    case Bytes::TextDecoder:
    case Bytes::FatalTextDecoder:
      return this->startTextMethod(call, member);
    case Bytes::None:
    case Bytes::Uint8:
    case Bytes::Int8:
    case Bytes::Uint8Clamped:
    case Bytes::Int16:
    case Bytes::Uint16:
    case Bytes::Int32:
    case Bytes::Uint32:
    case Bytes::Float16:
    case Bytes::Float32:
    case Bytes::Float64:
    case Bytes::Buffer:
      break;
  }
  if (type->isBuffer() && this->isBufferMethod(member->name)) {
    return this->startBufferMethod(call, member);
  }

  return this->startTypedArrayMethod(call, member, task.expected);
}

bool LibraryBytesRegistry::hasArity(CallExpr *call, unsigned min, unsigned max, llvm::SMLoc at) {
  auto arity = this->state.library.common.checkArity(call, min, max);
  if (arity.isErr()) {
    this->state.diagnostics.error(at, arity.error().message);
  }

  return arity.isOk();
}

void LibraryBytesRegistry::pushPositions(CallExpr *call, size_t from) {
  for (size_t index = call->args.size(); index > from; --index) {
    this->state.walk.tasks.pushTask(CheckPosition{.slot = &call->args[index - 1]});
  }
}

void LibraryBytesRegistry::pushLooseArgs(CallExpr *call) {
  for (Expr *&arg : llvm::reverse(call->args)) {
    this->state.walk.tasks.pushTask(CheckExprSlot{.slot = &arg, .expected = nullptr});
  }
}

void LibraryBytesRegistry::startArrayBufferMethod(CallExpr *call, MemberExpr *member) {
  auto &tasks = this->state.walk.tasks;
  const Type *type = member->object->type;
  llvm::StringRef name = member->name;
  auto method = BytesMethodTable::findMethod(name);
  tasks.pushChecked(call);
  if (method == BytesMethod::SLICE) {
    if (!this->hasArity(call, 0, 2, member->loc())) {
      return;
    }
    call->target = BuiltinCall{.builtin = Builtin::ArrayBufferSlice};
    call->type = type;

    return this->pushPositions(call, 0);
  }
  bool isNotYet = method == BytesMethod::RESIZE || method == BytesMethod::TRANSFER ||
                  method == BytesMethod::TRANSFER_TO_FIXED_LENGTH;
  std::string message =
      isNotYet ? ErrorTextArrayBuffer::notYet(name) : ErrorTextArrayBuffer::noMember(name);
  tasks.pushTask(ReportError{.loc = member->loc(), .message = message});

  return this->pushLooseArgs(call);
}

bool LibraryBytesRegistry::isBufferMethod(llvm::StringRef name) const {
  auto method = BytesMethodTable::findMethod(name);
  const BufferMethodInfo *found = method.has_value() ? BufferMethodTable::find(*method) : nullptr;

  return found != nullptr || NumberMethodTable::parse(name, false).has_value();
}

void LibraryBytesRegistry::startBufferMethod(CallExpr *call, MemberExpr *member) {
  this->state.walk.tasks.pushChecked(call);
  auto buffer = BufferCallDraft{
      .call = call,
      .member = member,
      .bufferType = member->object->type,
      .name = member->name,
  };
  auto method = BytesMethodTable::findMethod(member->name);
  const BufferMethodInfo *found = method.has_value() ? BufferMethodTable::find(*method) : nullptr;
  if (found == nullptr) {
    return this->startNumberMethod(buffer);
  }
  const BufferMethodForm &form = found->form;
  switch (form.kind()) {
    case BufferMethodKind::SLICE:
      return this->reportSlice(buffer);
    case BufferMethodKind::TO_STRING:
      return this->startToString(buffer);
    case BufferMethodKind::TO_JSON:
      return this->checkToJson(buffer);
    case BufferMethodKind::EQUALS:
      return this->startEquals(buffer);
    case BufferMethodKind::COMPARE:
    case BufferMethodKind::COPY:
      return this->startRangeMethod(buffer, form.kind());
    case BufferMethodKind::WRITE:
      return this->startWrite(buffer);
    case BufferMethodKind::FILL:
      return this->startValueMethod(buffer, BufferValueMethod::FILL, BytesSearchMode::INDEX_OF);
    case BufferMethodKind::SEARCH:
      return this->startValueMethod(buffer, BufferValueMethod::SEARCH,
                                    form.as<BufferSearchMethod>()->mode);
    case BufferMethodKind::SWAP:
      return this->checkSwap(buffer, form.as<BufferSwapMethod>()->width);
  }
}

void LibraryBytesRegistry::toBuiltinCall(CallExpr *call, Builtin builtin,
                                         const Type *result) const {
  call->target = BuiltinCall{.builtin = builtin};
  call->type = result;
}

void LibraryBytesRegistry::reportSlice(const BufferCallDraft &buffer) {
  auto message = ErrorTextBuffer::slice();
  this->state.diagnostics.error(buffer.member->loc(), message);

  return this->state.walk.tasks.pushTask(WalkArgsLoosely{.call = buffer.call});
}

void LibraryBytesRegistry::startToString(const BufferCallDraft &buffer) {
  CallExpr *call = buffer.call;
  auto &tasks = this->state.walk.tasks;
  if (!this->hasArity(call, 0, 3, buffer.member->loc())) {
    return;
  }
  tasks.pushTask(FinishBufferToString{.call = call});
  this->pushPositions(call, 1);
  if (call->args.empty()) {
    return;
  }

  return tasks.pushTask(BufferEncodingValue{.arg = call->args[0]});
}

void LibraryBytesRegistry::finishBufferToString(const FinishBufferToString &task) {
  CallExpr *call = task.call;
  Expr *encoding = call->args.empty() ? this->makeIntLiteral(call->loc(), Utf8)
                                      : this->state.walk.tasks.takeChecked();
  auto args = std::vector<Expr *>{encoding};
  for (size_t index = 1; index < call->args.size(); ++index) {
    args.push_back(call->args[index]);
  }
  call->args = std::move(args);

  return this->toBuiltinCall(call, Builtin::BufferToString, Type::string());
}

void LibraryBytesRegistry::checkToJson(const BufferCallDraft &buffer) {
  CallExpr *call = buffer.call;
  if (!this->hasArity(call, 0, 0, buffer.member->loc())) {
    return;
  }
  auto typeName = BytesShapeFieldTable::getName(BytesShapeField::JSON_TYPE);
  auto dataName = BytesShapeFieldTable::getName(BytesShapeField::JSON_DATA);
  const auto *json = Type::object({{typeName.str(), Type::string(), false},
                                   {dataName.str(), Type::array(Type::intTy()), false}});

  return this->toBuiltinCall(call, Builtin::BufferToJSON, json);
}

void LibraryBytesRegistry::startEquals(const BufferCallDraft &buffer) {
  CallExpr *call = buffer.call;
  if (!this->hasArity(call, 1, 1, buffer.member->loc())) {
    return;
  }
  this->toBuiltinCall(call, Builtin::BufferEquals, Type::boolTy());

  return this->state.walk.tasks.pushTask(
      CheckArgSlot{.slot = &call->args[0], .to = this->uint8Type()});
}

const Type *LibraryBytesRegistry::uint8Type() const {
  return Type::bytesType(Bytes::Uint8);
}

void LibraryBytesRegistry::startRangeMethod(const BufferCallDraft &buffer, BufferMethodKind kind) {
  CallExpr *call = buffer.call;
  auto &tasks = this->state.walk.tasks;
  bool isCompare = kind == BufferMethodKind::COMPARE;
  unsigned most = isCompare ? 5 : 4;
  if (!this->hasArity(call, 1, most, buffer.member->loc())) {
    return;
  }
  tasks.pushTask(FinishRangeMethod{.buffer = buffer, .isCompare = isCompare});
  this->pushPositions(call, 1);

  return tasks.pushTask(CheckArgSlot{.slot = &call->args[0], .to = this->uint8Type()});
}

void LibraryBytesRegistry::finishRangeMethod(const FinishRangeMethod &task) {
  const BufferCallDraft &buffer = task.buffer;
  CallExpr *call = buffer.call;
  auto &proofs = this->state.flow.proofs;
  for (size_t index = 1; index < call->args.size(); ++index) {
    auto name = task.isCompare ? ErrorTextBufferLabel::comparePosition(index)
                               : ErrorTextBufferLabel::copyPosition(index);
    auto what = ErrorTextBuffer::position(name);
    bool isEnd = task.isCompare ? (index == 2 || index == 4) : index == 2;
    const Expr *position = call->args[index];
    if (!isEnd) {
      auto between = proofs.requireBetween(position, 0, std::nullopt, what);
      this->reportUnprovenBetween(between, position->loc());
      continue;
    }
    bool isOther = task.isCompare && index == 2;
    const Expr *array = isOther ? call->args[0] : buffer.member->object;
    auto upTo = proofs.requireUpTo(position, array, 0, what);
    this->reportUnprovenUpTo(upTo, position->loc());
  }
  Builtin builtin = task.isCompare ? Builtin::BufferCompareTo : Builtin::BufferCopy;

  return this->toBuiltinCall(call, builtin, Type::intTy());
}

void LibraryBytesRegistry::startWrite(const BufferCallDraft &buffer) {
  CallExpr *call = buffer.call;
  auto &tasks = this->state.walk.tasks;
  if (!this->hasArity(call, 1, 4, buffer.member->loc())) {
    return;
  }
  tasks.pushTask(NextWriteArg{.buffer = buffer, .index = 1});

  return tasks.pushTask(CheckArgSlot{.slot = &call->args[0], .to = Type::string()});
}

void LibraryBytesRegistry::nextWriteArg(const NextWriteArg &task) {
  const BufferCallDraft &buffer = task.buffer;
  CallExpr *call = buffer.call;
  auto &tasks = this->state.walk.tasks;
  size_t index = task.index;
  if (index == call->args.size()) {
    return this->finishWrite(
        FinishWrite{.buffer = buffer, .positionsEnd = index, .hasEncoding = false});
  }
  Expr *arg = call->args[index];
  bool isLast = index + 1 == call->args.size();
  bool isMayBeEncoding = this->literalText(arg).has_value() || isLast;
  if (!isMayBeEncoding) {
    tasks.pushTask(NextWriteArg{.buffer = buffer, .index = index + 1});

    return tasks.pushTask(CheckPosition{.slot = &call->args[index]});
  }
  tasks.pushTask(AfterWriteArg{.buffer = buffer, .index = index});

  return tasks.pushTask(CheckExprSlot{.slot = &call->args[index], .expected = nullptr});
}

void LibraryBytesRegistry::afterWriteArg(const AfterWriteArg &task) {
  const BufferCallDraft &buffer = task.buffer;
  CallExpr *call = buffer.call;
  auto &tasks = this->state.walk.tasks;
  size_t index = task.index;
  Expr *checked = call->args[index];
  if (!checked->type->isString()) {
    tasks.pushTask(NextWriteArg{.buffer = buffer, .index = index + 1});

    return tasks.pushTask(CheckPosition{.slot = &call->args[index]});
  }
  tasks.pushTask(FinishWrite{.buffer = buffer, .positionsEnd = index, .hasEncoding = true});

  return this->pushEncodingAt(buffer, index);
}

void LibraryBytesRegistry::pushEncodingAt(const BufferCallDraft &buffer, size_t index) {
  CallExpr *call = buffer.call;
  auto &tasks = this->state.walk.tasks;
  bool isLast = index + 1 == call->args.size();
  if (!isLast) {
    auto message = ErrorTextBuffer::encoding(buffer.name);
    tasks.pushTask(ReportError{.loc = call->args[index + 1]->loc(), .message = message});
  }

  return tasks.pushTask(BufferEncodingValue{.arg = call->args[index]});
}

void LibraryBytesRegistry::finishWrite(const FinishWrite &task) {
  const BufferCallDraft &buffer = task.buffer;
  CallExpr *call = buffer.call;
  Expr *encoding = task.hasEncoding ? this->state.walk.tasks.takeChecked()
                                    : this->makeIntLiteral(call->loc(), Utf8);
  Expr *text = call->args[0];
  auto positions =
      std::vector<Expr *>(call->args.begin() + 1, call->args.begin() + int64_t(task.positionsEnd));
  if (!positions.empty()) {
    auto offsetWhat = ErrorTextBufferLabel::offset();
    auto upTo =
        this->state.flow.proofs.requireUpTo(positions[0], buffer.member->object, 0, offsetWhat);
    this->reportUnprovenUpTo(upTo, positions[0]->loc());
    if (upTo.isOk() && positions.size() > 1) {
      this->walkWriteLength(buffer.member->object, positions[0], positions[1]);
    }
  }
  auto args = std::vector<Expr *>{text, encoding};
  args.insert(args.end(), positions.begin(), positions.end());
  call->args = std::move(args);

  return this->toBuiltinCall(call, Builtin::BufferWrite, Type::intTy());
}

void LibraryBytesRegistry::startValueMethod(const BufferCallDraft &buffer, BufferValueMethod method,
                                            BytesSearchMode mode) {
  CallExpr *call = buffer.call;
  auto &tasks = this->state.walk.tasks;
  unsigned most = method == BufferValueMethod::FILL ? 4 : 3;
  if (!this->hasArity(call, 1, most, buffer.member->loc())) {
    return;
  }
  auto draft = BufferValueDraft{.buffer = buffer, .method = method, .mode = mode};
  tasks.pushTask(AfterBufferValue{.draft = draft});

  return tasks.pushTask(CheckExprSlot{.slot = &call->args[0], .expected = nullptr});
}

void LibraryBytesRegistry::afterBufferValue(const AfterBufferValue &task) {
  const BufferValueDraft &draft = task.draft;
  CallExpr *call = draft.buffer.call;
  auto &tasks = this->state.walk.tasks;
  Expr *checked = call->args[0];
  const Type *type = checked->type;
  if (type->isError()) {
    return;
  }
  tasks.pushTask(ContinueValueMethod{.draft = draft});
  if (type->isString()) {
    return;
  }
  if (type->isInteger()) {
    return tasks.pushTask(ConvertSlot{.slot = &call->args[0], .to = Type::intTy()});
  }
  const auto *uint8 = this->uint8Type();
  if (type->isTypedArray() && this->state.types.relations.isImplicitlyConvertible(type, uint8)) {
    return tasks.pushTask(ConvertSlot{.slot = &call->args[0], .to = uint8});
  }
  auto message = ErrorTextBuffer::valueArg(draft.buffer.name, type->name());
  this->state.diagnostics.error(checked->loc(), message);
  checked->type = Type::error();
}

void LibraryBytesRegistry::continueValueMethod(const ContinueValueMethod &task) {
  const BufferValueDraft &draft = task.draft;
  const Expr *value = draft.buffer.call->args[0];
  if (value->type->isError()) {
    return;
  }
  bool isFillingInt = draft.method == BufferValueMethod::FILL && value->type->isInt();
  if (isFillingInt) {
    auto byte = ErrorTextBufferLabel::byte();
    auto fitting = this->state.flow.proofs.checkValueFits(value, draft.buffer.bufferType, byte);
    if (fitting.isErr()) {
      this->state.diagnostics.error(value->loc(), fitting.error().message);
    }
  }

  return this->nextValueSplit(NextValueSplit{.draft = draft, .index = 1});
}

void LibraryBytesRegistry::nextValueSplit(const NextValueSplit &task) {
  const BufferValueDraft &draft = task.draft;
  CallExpr *call = draft.buffer.call;
  auto &tasks = this->state.walk.tasks;
  if (task.index == call->args.size()) {
    return this->finishValueMethod(
        FinishValueMethod{.draft = draft, .positionsEnd = task.index, .hasEncoding = false});
  }
  tasks.pushTask(AfterValueSplit{.draft = draft, .index = task.index});

  return tasks.pushTask(CheckExprSlot{.slot = &call->args[task.index], .expected = nullptr});
}

void LibraryBytesRegistry::afterValueSplit(const AfterValueSplit &task) {
  const BufferValueDraft &draft = task.draft;
  CallExpr *call = draft.buffer.call;
  auto &tasks = this->state.walk.tasks;
  size_t index = task.index;
  if (!call->args[index]->type->isString()) {
    tasks.pushTask(NextValueSplit{.draft = draft, .index = index + 1});

    return tasks.pushTask(CheckPosition{.slot = &call->args[index]});
  }
  tasks.pushTask(FinishValueMethod{.draft = draft, .positionsEnd = index, .hasEncoding = true});

  return this->pushEncodingAt(draft.buffer, index);
}

void LibraryBytesRegistry::finishValueMethod(const FinishValueMethod &task) {
  const BufferValueDraft &draft = task.draft;
  const BufferCallDraft &buffer = draft.buffer;
  CallExpr *call = buffer.call;
  Expr *encoding = task.hasEncoding ? this->state.walk.tasks.takeChecked() : nullptr;
  Expr *value = call->args[0];
  auto positions =
      std::vector<Expr *>(call->args.begin() + 1, call->args.begin() + int64_t(task.positionsEnd));
  this->reportEncodingWithoutText(value, encoding);
  Expr *givenEncoding = encoding != nullptr ? encoding : this->makeIntLiteral(call->loc(), Utf8);
  if (draft.method == BufferValueMethod::SEARCH) {
    return this->finishBufferSearch(draft, value, givenEncoding, positions);
  }
  for (size_t index = 0; index < positions.size(); ++index) {
    std::string what = index == 0 ? ErrorTextBufferLabel::offset() : ErrorTextBufferLabel::end();
    auto upTo =
        this->state.flow.proofs.requireUpTo(positions[index], buffer.member->object, 0, what);
    this->reportUnprovenUpTo(upTo, positions[index]->loc());
  }
  auto args = std::vector<Expr *>{value, givenEncoding};
  args.insert(args.end(), positions.begin(), positions.end());
  call->args = std::move(args);

  return this->toBuiltinCall(call, Builtin::BufferFill, buffer.bufferType);
}

void LibraryBytesRegistry::finishBufferSearch(const BufferValueDraft &draft, Expr *value,
                                              Expr *encoding,
                                              const std::vector<Expr *> &positions) {
  CallExpr *call = draft.buffer.call;
  int runtimeMode = 0;
  const auto *answer = Type::intTy();
  switch (draft.mode) {
    case BytesSearchMode::INDEX_OF:
      break;
    case BytesSearchMode::LAST_INDEX_OF:
      runtimeMode = 1;
      break;
    case BytesSearchMode::INCLUDES:
      runtimeMode = 2;
      answer = Type::boolTy();
      break;
  }
  auto *modeArg = this->makeIntLiteral(call->loc(), runtimeMode);
  auto args = std::vector<Expr *>{modeArg, value, encoding};
  args.insert(args.end(), positions.begin(), positions.end());
  call->args = std::move(args);

  return this->toBuiltinCall(call, Builtin::BufferIndexOf, answer);
}

void LibraryBytesRegistry::reportEncodingWithoutText(const Expr *value, const Expr *encoding) {
  if (encoding != nullptr && !value->type->isString()) {
    auto message = ErrorTextBuffer::withString();
    this->state.diagnostics.error(encoding->loc(), message);
  }
}

void LibraryBytesRegistry::checkSwap(const BufferCallDraft &buffer, int width) {
  CallExpr *call = buffer.call;
  if (!this->hasArity(call, 0, 0, buffer.member->loc())) {
    return;
  }
  auto *size = this->makeIntLiteral(call->loc(), width);
  call->args = {size};

  return this->toBuiltinCall(call, Builtin::BufferSwap, buffer.bufferType);
}

void LibraryBytesRegistry::startNumberMethod(const BufferCallDraft &buffer) {
  auto parsed = NumberMethodTable::parse(buffer.name, false);
  BytesNumberMethod number = *parsed;
  if (number.bigint) {
    auto quoted = ErrorTextBigInt::quoted(buffer.name);
    auto bigint = ErrorTextBigInt::none(quoted);
    this->state.diagnostics.error(buffer.member->loc(), bigint);

    return this->state.walk.tasks.pushTask(WalkArgsLoosely{.call = buffer.call});
  }
  if (number.variable) {
    return this->startVariableNumber(buffer, number);
  }

  return this->startFixedNumber(buffer, number);
}

void LibraryBytesRegistry::startVariableNumber(const BufferCallDraft &buffer,
                                               const BytesNumberMethod &number) {
  CallExpr *call = buffer.call;
  auto &tasks = this->state.walk.tasks;
  unsigned argCount = number.write ? 3 : 2;
  if (!this->hasArity(call, argCount, argCount, buffer.member->loc())) {
    return;
  }
  size_t at = number.write ? 1 : 0;
  tasks.pushTask(FinishVariableNumber{.buffer = buffer, .number = number});
  tasks.pushTask(CheckPosition{.slot = &call->args[at + 1]});
  tasks.pushTask(CheckPosition{.slot = &call->args[at]});
  if (!number.write) {
    return;
  }

  return tasks.pushTask(CheckArgSlot{.slot = &call->args[0], .to = Type::intTy()});
}

void LibraryBytesRegistry::finishVariableNumber(const FinishVariableNumber &task) {
  const BufferCallDraft &buffer = task.buffer;
  const BytesNumberMethod &number = task.number;
  CallExpr *call = buffer.call;
  size_t at = number.write ? 1 : 0;
  Expr *offset = call->args[at];
  Expr *count = call->args[at + 1];
  auto written = this->state.code.constants.intConstant(count);
  auto bytes = this->checkCountOfBytes(written, buffer.name);
  if (bytes.isErr()) {
    return this->state.diagnostics.error(count->loc(), bytes.error().message);
  }
  int64_t width = bytes.value();
  auto offsetWhat = ErrorTextBufferLabel::offsetOf(buffer.name);
  auto upTo = this->state.flow.proofs.requireUpTo(offset, buffer.member->object, width, offsetWhat);
  this->reportUnprovenUpTo(upTo, offset->loc());
  call->mayBeOutOfRange = upTo.isErr();
  auto *isSigned = this->makeBoolLiteral(call->loc(), number.isSigned);
  auto *isBig = this->makeBoolLiteral(call->loc(), number.big);
  auto args = std::vector<Expr *>{isSigned, isBig};
  if (number.write) {
    Expr *value = call->args[0];
    auto fits = this->checkVarValueFits(value, width, number.isSigned);
    if (fits.isErr()) {
      this->state.diagnostics.error(value->loc(), fits.error().message);
    }
    args.push_back(value);
  }
  args.push_back(offset);
  args.push_back(count);
  call->args = std::move(args);
  Builtin builtin = number.write ? Builtin::BufferWriteVar : Builtin::BufferReadVar;

  return this->toBuiltinCall(call, builtin, Type::intTy());
}

void LibraryBytesRegistry::startFixedNumber(const BufferCallDraft &buffer,
                                            const BytesNumberMethod &number) {
  CallExpr *call = buffer.call;
  auto &tasks = this->state.walk.tasks;
  const auto *kindType = Type::bytesType(number.kind);
  const Type *element = kindType->accessType();
  unsigned minArgs = number.write ? 1 : 0;
  unsigned maxArgs = number.write ? 2 : 1;
  if (!this->hasArity(call, minArgs, maxArgs, buffer.member->loc())) {
    return;
  }
  size_t at = number.write ? 1 : 0;
  tasks.pushTask(FinishFixedNumber{.buffer = buffer, .number = number});
  bool isFloatRead = !number.write && element->isFloat();
  if (isFloatRead) {
    tasks.pushTask(PreludeTypeOf{.type = PreludeType::NUMBER_ERROR});
  }
  if (call->args.size() > at) {
    tasks.pushTask(CheckPosition{.slot = &call->args[at]});
  }
  if (!number.write) {
    return;
  }
  tasks.pushTask(AfterFixedValue{.buffer = buffer, .number = number});

  return tasks.pushTask(CheckArgSlot{.slot = &call->args[0], .to = element});
}

void LibraryBytesRegistry::afterFixedValue(const AfterFixedValue &task) {
  const Expr *value = task.buffer.call->args[0];
  Bytes kind = task.number.kind;
  const auto *kindType = Type::bytesType(kind);
  auto what = BytesCheckHelpers::numberWhat(kind);
  auto fitting = this->state.flow.proofs.checkValueFits(value, kindType, what);
  if (fitting.isErr()) {
    this->state.diagnostics.error(value->loc(), fitting.error().message);
  }
}

void LibraryBytesRegistry::finishFixedNumber(const FinishFixedNumber &task) {
  const BufferCallDraft &buffer = task.buffer;
  const BytesNumberMethod &number = task.number;
  CallExpr *call = buffer.call;
  Bytes kind = number.kind;
  const auto *kindType = Type::bytesType(kind);
  const Type *element = kindType->accessType();
  int64_t size = kindType->elementSize();
  size_t at = number.write ? 1 : 0;
  Expr *offset = call->args.size() > at ? call->args[at] : nullptr;
  auto offsetWhat = ErrorTextBufferLabel::offsetOf(buffer.name);
  auto upTo = this->state.flow.proofs.requireUpTo(offset, buffer.member->object, size, offsetWhat);
  llvm::SMLoc place = offset != nullptr ? offset->loc() : buffer.member->object->loc();
  this->reportUnprovenUpTo(upTo, place);
  call->mayBeOutOfRange = upTo.isErr();
  auto *kindArg = this->makeIntLiteral(call->loc(), int64_t(kind));
  auto *isBig = this->makeBoolLiteral(call->loc(), number.big);
  auto args = std::vector<Expr *>{kindArg, isBig};
  if (number.write) {
    args.push_back(call->args[0]);
  }
  if (offset != nullptr) {
    args.push_back(offset);
  }
  call->args = std::move(args);
  if (number.write) {
    return this->toBuiltinCall(call, Builtin::BufferWriteNumber, Type::intTy());
  }
  if (!element->isFloat()) {
    return this->toBuiltinCall(call, Builtin::BufferRead, element);
  }
  const Type *numberError = this->state.walk.tasks.takeAnswer();
  const auto *read = Type::result(Type::doubleTy(), numberError);

  return this->toBuiltinCall(call, Builtin::BufferRead, read);
}

void LibraryBytesRegistry::reportUnprovenUpTo(const FlowProofs::ResultUpToWasProven &upTo,
                                              llvm::SMLoc at) {
  bool isShown = upTo.isErr() && upTo.error().code != FlowProofsRequireUpToErrorCode::EXEMPT;
  if (isShown) {
    this->state.diagnostics.error(at, upTo.error().message);
  }
}

void LibraryBytesRegistry::reportUnprovenBetween(const FlowProofs::ResultBetweenProven &between,
                                                 llvm::SMLoc at) {
  bool isShown =
      between.isErr() && between.error().code != FlowProofsRequireBetweenErrorCode::EXEMPT;
  if (isShown) {
    this->state.diagnostics.error(at, between.error().message);
  }
}

void LibraryBytesRegistry::startTextMethod(CallExpr *call, MemberExpr *member) {
  auto &tasks = this->state.walk.tasks;
  const Type *type = member->object->type;
  auto method = BytesMethodTable::findMethod(member->name);
  bool isEncoder = type->bytes() == Bytes::TextEncoder;
  bool isDecoder = type->bytes() == Bytes::TextDecoder || type->bytes() == Bytes::FatalTextDecoder;
  tasks.pushChecked(call);
  if (isEncoder && method == BytesMethod::ENCODE) {
    return this->startEncode(call, member);
  }
  if (isEncoder && method == BytesMethod::ENCODE_INTO) {
    return this->startEncodeInto(call, member);
  }
  if (isDecoder && method == BytesMethod::DECODE) {
    return this->startDecode(call, member);
  }
  auto message = ErrorTextBuffer::noMember(member->name, type->name());
  tasks.pushTask(ReportError{.loc = member->loc(), .message = message});

  return tasks.pushTask(WalkArgsLoosely{.call = call});
}

void LibraryBytesRegistry::startEncode(CallExpr *call, MemberExpr *member) {
  if (!this->hasArity(call, 0, 1, member->loc())) {
    return;
  }
  this->toBuiltinCall(call, Builtin::TextEncode, this->uint8Type());
  if (call->args.empty()) {
    return;
  }

  return this->state.walk.tasks.pushTask(
      CheckArgSlot{.slot = &call->args[0], .to = Type::string()});
}

void LibraryBytesRegistry::startEncodeInto(CallExpr *call, MemberExpr *member) {
  auto &tasks = this->state.walk.tasks;
  if (!this->hasArity(call, 2, 2, member->loc())) {
    return;
  }
  auto readName = BytesShapeFieldTable::getName(BytesShapeField::ENCODE_READ);
  auto writtenName = BytesShapeFieldTable::getName(BytesShapeField::ENCODE_WRITTEN);
  const auto *counts = Type::object(
      {{readName.str(), Type::intTy(), false}, {writtenName.str(), Type::intTy(), false}});
  this->toBuiltinCall(call, Builtin::TextEncodeInto, counts);
  tasks.pushTask(CheckArgSlot{.slot = &call->args[1], .to = this->uint8Type()});

  return tasks.pushTask(CheckArgSlot{.slot = &call->args[0], .to = Type::string()});
}

void LibraryBytesRegistry::startDecode(CallExpr *call, MemberExpr *member) {
  auto &tasks = this->state.walk.tasks;
  if (!this->hasArity(call, 0, 2, member->loc())) {
    return;
  }
  if (call->args.empty()) {
    return this->continueDecode(call, member);
  }
  tasks.pushTask(AfterDecodeInput{.call = call, .member = member});

  return tasks.pushTask(CheckExprSlot{.slot = &call->args[0], .expected = nullptr});
}

void LibraryBytesRegistry::afterDecodeInput(const AfterDecodeInput &task) {
  const Expr *input = task.call->args[0];
  auto decoded = this->checkDecodedInput(input->type);
  if (decoded.isErr()) {
    return this->state.diagnostics.error(input->loc(), decoded.error().message);
  }

  return this->continueDecode(task.call, task.member);
}

void LibraryBytesRegistry::continueDecode(CallExpr *call, MemberExpr *member) {
  auto &tasks = this->state.walk.tasks;
  bool isFatal = member->object->type->bytes() == Bytes::FatalTextDecoder;
  if (isFatal) {
    tasks.pushTask(FinishFatalDecode{.call = call});
    tasks.pushTask(PreludeTypeOf{.type = PreludeType::DECODE_ERROR});
  } else {
    this->toBuiltinCall(call, Builtin::TextDecode, Type::string());
  }
  if (call->args.size() != 2) {
    return;
  }
  auto streamName = BytesShapeFieldTable::getName(BytesShapeField::DECODE_STREAM);
  const auto *options = Type::object({{streamName.str(), Type::optional(Type::boolTy()), false}});
  auto what = ErrorTextBufferLabel::options();
  tasks.pushTask(CoerceSlot{.slot = &call->args[1], .to = options, .what = what});

  return tasks.pushTask(CheckExprSlot{.slot = &call->args[1], .expected = options});
}

void LibraryBytesRegistry::finishFatalDecode(const FinishFatalDecode &task) {
  const Type *decodeError = this->state.walk.tasks.takeAnswer();
  const auto *decoded = Type::result(Type::string(), decodeError);

  return this->toBuiltinCall(task.call, Builtin::TextDecode, decoded);
}

void LibraryBytesRegistry::walkWriteLength(const Expr *object, const Expr *offset,
                                           const Expr *length) {
  auto &proofs = this->state.flow.proofs;
  auto &measurements = this->state.flow.measurements;
  auto what = ErrorTextBufferLabel::length();
  auto between = proofs.requireBetween(length, 0, std::nullopt, what);
  if (between.isErr() && between.error().code != FlowProofsRequireBetweenErrorCode::EXEMPT) {
    this->state.diagnostics.error(length->loc(), between.error().message);
  }
  if (between.isErr() || proofs.isExempt(length->loc(), object)) {
    return;
  }
  auto from = measurements.linear(offset);
  auto count = measurements.linear(length);
  if (proofs.canProveSumUpTo(from, count, object)) {
    return;
  }
  auto offsetText = BytesCheckHelpers::valueText(offset);
  auto lengthText = BytesCheckHelpers::valueText(length);
  auto objectText = BytesCheckHelpers::valueText(object);
  auto message = ErrorTextBuffer::written(offsetText, lengthText, objectText);
  this->state.diagnostics.error(length->loc(), message);
}

void LibraryBytesRegistry::checkBytesStatic(const CheckBytesStatic &task) {
  CallExpr *call = task.call;
  MemberExpr *member = task.member;
  auto &tasks = this->state.walk.tasks;
  auto *object = dyn_cast<NameExpr>(member->object);
  bool isGlobal = object != nullptr && this->state.isUnshadowedGlobal(object);
  if (!isGlobal) {
    return tasks.pushChecked(nullptr);
  }
  auto globalName = GlobalNameTable::find(object->name);
  bool isBigint =
      globalName == GlobalName::BIG_INT64_ARRAY || globalName == GlobalName::BIG_UINT64_ARRAY;
  if (isBigint) {
    return this->reportBigIntArray(call, object);
  }
  const auto *type = this->state.library.bytes.bytesTypeNamed(object->name);
  if (type == nullptr) {
    return tasks.pushChecked(nullptr);
  }
  call->type = Type::error();
  if (this->state.library.common.hasSpread(call)) {
    tasks.pushChecked(call);

    return tasks.pushTask(RejectSpreadCall{.call = call});
  }
  auto method = BytesMethodTable::findMethod(member->name);
  if (type->bytes() == Bytes::ArrayBuffer && method == BytesMethod::IS_VIEW) {
    return this->startIsView(call, member);
  }
  if (type->isBuffer() && method != BytesMethod::OF) {
    return this->startBufferStatic(call, member);
  }
  if (type->bytes() == Bytes::TextDecoder && method == BytesMethod::PARSE) {
    return this->startTextDecoderParse(call, member);
  }
  if (!type->isTypedArray()) {
    auto message = ErrorTextBytesStatic::missing(object->name, member->name);

    return this->reportStatic(call, member, message);
  }
  if (method == BytesMethod::OF) {
    return this->startTypedArrayOf(call, type);
  }
  if (method != BytesMethod::FROM) {
    auto message = ErrorTextBytesStatic::notYet(object->name, member->name);

    return this->reportStatic(call, member, message);
  }

  return this->startTypedArrayFrom(call, member, object, type);
}

void LibraryBytesRegistry::reportBigIntArray(CallExpr *call, NameExpr *object) {
  auto &tasks = this->state.walk.tasks;
  auto quoted = ErrorTextBigInt::quoted(object->name);
  auto bigint = ErrorTextBigInt::none(quoted);
  this->state.diagnostics.error(object->loc(), bigint);
  call->type = Type::error();
  tasks.pushChecked(call);

  return tasks.pushTask(WalkArgsLoosely{.call = call});
}

void LibraryBytesRegistry::reportStatic(CallExpr *call, MemberExpr *member,
                                        const std::string &message) {
  auto &tasks = this->state.walk.tasks;
  tasks.pushChecked(call);
  tasks.pushTask(ReportError{.loc = member->loc(), .message = message});

  return tasks.pushTask(WalkArgsLoosely{.call = call});
}

void LibraryBytesRegistry::startIsView(CallExpr *call, MemberExpr *member) {
  this->state.walk.tasks.pushChecked(call);
  if (!this->hasArity(call, 1, 1, member->loc())) {
    return;
  }
  this->toBuiltinCall(call, Builtin::ArrayBufferIsView, Type::boolTy());

  return this->state.walk.tasks.pushTask(
      CheckExprSlot{.slot = &call->args[0], .expected = nullptr});
}

void LibraryBytesRegistry::startTypedArrayOf(CallExpr *call, const Type *type) {
  auto *literal = this->state.ast.make<ArrayLitExpr>(call->loc());
  for (Expr *arg : call->args) {
    literal->elements.push_back({arg, false});
  }

  return this->startBytesValues(call, literal, type);
}

void LibraryBytesRegistry::startTypedArrayFrom(CallExpr *call, MemberExpr *member, NameExpr *object,
                                               const Type *type) {
  auto &tasks = this->state.walk.tasks;
  if (!this->hasArity(call, 1, 2, member->loc())) {
    return tasks.pushChecked(call);
  }
  if (call->args.size() != 1) {
    auto what = ErrorTextBytesStatic::from(object->name);

    return this->startBytesFromWith(call, type, what);
  }
  auto *literal = dyn_cast<ArrayLitExpr>(call->args[0]);
  if (literal != nullptr) {
    return this->startBytesValues(call, literal, type);
  }
  tasks.pushChecked(call);
  tasks.pushTask(AfterBytesSource{.call = call, .type = type});

  return this->pushFusedSource(call);
}

void LibraryBytesRegistry::pushFusedSource(CallExpr *call) {
  auto &tasks = this->state.walk.tasks;
  tasks.pushTask(CheckExprSlot{.slot = &call->args[0], .expected = nullptr});

  return tasks.pushTask(MarkFused{.expr = call->args[0]});
}

void LibraryBytesRegistry::afterBytesSource(const AfterBytesSource &task) {
  CallExpr *call = task.call;
  Expr *source = call->args[0];
  if (!this->matchBytesSource(source, task.type)) {
    return;
  }
  Builtin builtin =
      source->type->isTypedArray() ? Builtin::BytesFromBytes : Builtin::BytesFromArray;

  return this->toBuiltinCall(call, builtin, task.type);
}

void LibraryBytesRegistry::startBytesFromWith(CallExpr *call, const Type *type,
                                              const std::string &what) {
  auto &tasks = this->state.walk.tasks;
  tasks.pushChecked(call);
  tasks.pushTask(AfterFromWithSource{.call = call, .type = type, .what = what});

  return this->pushFusedSource(call);
}

void LibraryBytesRegistry::afterFromWithSource(const AfterFromWithSource &task) {
  CallExpr *call = task.call;
  auto &tasks = this->state.walk.tasks;
  Expr *source = call->args[0];
  const Type *from = source->type;
  if (from->isError()) {
    return;
  }
  auto iterated = this->checkSourceElement(from, task.what);
  if (iterated.isErr()) {
    return this->state.diagnostics.error(source->loc(), iterated.error().message);
  }
  if (from->isIterator()) {
    auto nothing = this->state.code.iteration.checkSendsNothing(from, task.what);
    if (nothing.isErr()) {
      this->state.diagnostics.error(source->loc(), nothing.error().message);
    }
  }
  auto signature = this->mapperSignature(iterated.value(), task.type);
  this->rememberByteCallback(call->args[1], from, task.type);
  bool isArrow = isa<FunctionExpr>(call->args[1]);
  tasks.pushTask(AfterFromWithMapper{.call = call, .type = task.type, .isArrow = isArrow});

  return tasks.pushTask(CheckFunctionArgSlot{
      .slot = &call->args[1],
      .expected = signature,
      .primary = 1,
      .method = task.what,
  });
}

void LibraryBytesRegistry::afterFromWithMapper(const AfterFromWithMapper &task) {
  CallExpr *call = task.call;
  auto &tasks = this->state.walk.tasks;
  if (!call->args[1]->type->isFunction()) {
    return;
  }
  auto fits = this->checkMapResultFits(task.type, task.isArrow);
  if (fits.isErr()) {
    return this->state.diagnostics.error(call->args[1]->loc(), fits.error().message);
  }
  tasks.pushTask(FinishFromWith{.call = call, .type = task.type});

  return tasks.pushTask(CalledFunction{});
}

void LibraryBytesRegistry::finishFromWith(const FinishFromWith &task) {
  CallExpr *call = task.call;
  auto *arrayFrom = this->makeArrayFrom(call, call->args[0], task.type);
  call->args = {arrayFrom};

  return this->toBuiltinCall(call, Builtin::BytesFromArray, task.type);
}

void LibraryBytesRegistry::startBytesValues(CallExpr *call, ArrayLitExpr *literal,
                                            const Type *type) {
  auto &tasks = this->state.walk.tasks;
  call->type = Type::error();
  tasks.pushChecked(call);
  auto isSpread = std::bind_front(&LibraryBytesRegistry::isSpreadElement, this);
  bool isSpreading = llvm::any_of(literal->elements, isSpread);
  if (!isSpreading) {
    return this->nextBytesValue(
        NextBytesValue{.call = call, .literal = literal, .type = type, .index = 0});
  }
  const Type *element = this->valueElement(type);
  tasks.pushTask(AfterSpreadValues{.call = call, .literal = literal, .type = type});
  tasks.pushTask(Coerce{.to = Type::array(element), .what = ""});

  return tasks.pushTask(CheckExpr{.expr = literal, .expected = Type::array(element)});
}

const Type *LibraryBytesRegistry::valueElement(const Type *type) const {
  return type->bytes() == Type::Bytes::Uint8Clamped ? Type::intTy() : type->elementType();
}

void LibraryBytesRegistry::nextBytesValue(const NextBytesValue &task) {
  ArrayLitExpr *literal = task.literal;
  auto &tasks = this->state.walk.tasks;
  if (task.index == literal->elements.size()) {
    return this->finishBytesValues(task.call, literal, task.type);
  }
  ArrayLitExpr::Element &item = literal->elements[task.index];
  if (item.hole) {
    auto message = ErrorTextArrayHole::hole();

    return this->state.diagnostics.error(item.expr->loc(), message);
  }
  auto next = NextBytesValue{
      .call = task.call,
      .literal = literal,
      .type = task.type,
      .index = task.index + 1,
  };
  tasks.pushTask(std::move(next));
  tasks.pushTask(AfterBytesValue{.slot = &item.expr, .type = task.type});
  const Type *element = this->valueElement(task.type);

  return tasks.pushTask(CheckArgSlot{.slot = &item.expr, .to = element});
}

void LibraryBytesRegistry::afterBytesValue(const AfterBytesValue &task) {
  const Expr *value = *task.slot;
  auto what = ErrorTextByteView::element(task.type->name());
  auto fitting = this->state.flow.proofs.checkValueFits(value, task.type, what);
  if (fitting.isErr()) {
    this->state.diagnostics.error(value->loc(), fitting.error().message);
  }
}

void LibraryBytesRegistry::finishBytesValues(CallExpr *call, ArrayLitExpr *literal,
                                             const Type *type) {
  auto values = std::vector<Expr *>();
  for (const ArrayLitExpr::Element &item : literal->elements) {
    values.push_back(item.expr);
  }
  call->args = std::move(values);

  return this->toBuiltinCall(call, Builtin::BytesOf, type);
}

void LibraryBytesRegistry::afterSpreadValues(const AfterSpreadValues &task) {
  CallExpr *call = task.call;
  const Type *type = task.type;
  Expr *array = this->state.walk.tasks.takeChecked();
  if (array->type->isError()) {
    return;
  }
  auto what = ErrorTextByteView::element(type->name());
  bool isOk = true;
  for (const ArrayLitExpr::Element &item : task.literal->elements) {
    if (item.spread) {
      isOk &= this->matchBytesSource(item.expr, type);
      continue;
    }
    auto fitting = this->state.flow.proofs.checkValueFits(item.expr, type, what);
    if (fitting.isErr()) {
      this->state.diagnostics.error(item.expr->loc(), fitting.error().message);
    }
  }
  if (!isOk) {
    return;
  }
  call->args = {array};

  return this->toBuiltinCall(call, Builtin::BytesFromArray, type);
}

void LibraryBytesRegistry::startBufferStatic(CallExpr *call, MemberExpr *member) {
  auto method = BytesMethodTable::findMethod(member->name);
  const BufferStaticInfo *found = method.has_value() ? BufferStaticTable::find(*method) : nullptr;
  if (found == nullptr) {
    auto message = ErrorTextBufferStatic::unknown(member->name);

    return this->reportStatic(call, member, message);
  }
  switch (found->kind) {
    case BufferStaticKind::FROM:
      return this->startBufferFrom(call, member);
    case BufferStaticKind::ALLOC:
    case BufferStaticKind::ALLOC_UNSAFE:
      return this->startAlloc(call, member, found->kind == BufferStaticKind::ALLOC_UNSAFE);
    case BufferStaticKind::CONCAT:
      return this->startConcat(call, member);
    case BufferStaticKind::BYTE_LENGTH:
      return this->startByteLength(call, member);
    case BufferStaticKind::COMPARE:
      return this->startBufferCompare(call, member);
    case BufferStaticKind::IS_BUFFER:
      return this->startIsBuffer(call, member);
    case BufferStaticKind::IS_ENCODING:
      return this->checkIsEncoding(call, member);
  }
}

const Type *LibraryBytesRegistry::bufferType() const {
  return Type::bytesType(Bytes::Buffer);
}

void LibraryBytesRegistry::startBufferFrom(CallExpr *call, MemberExpr *member) {
  auto &tasks = this->state.walk.tasks;
  if (!this->hasArity(call, 1, 3, member->loc())) {
    return tasks.pushChecked(call);
  }
  auto *literal = dyn_cast<ArrayLitExpr>(call->args[0]);
  if (literal != nullptr && call->args.size() == 1) {
    return this->startBytesValues(call, literal, this->bufferType());
  }
  tasks.pushChecked(call);
  tasks.pushTask(AfterBufferFromSource{.call = call, .member = member});

  return this->pushFusedSource(call);
}

void LibraryBytesRegistry::afterBufferFromSource(const AfterBufferFromSource &task) {
  CallExpr *call = task.call;
  const Expr *source = call->args[0];
  const Type *from = source->type;
  if (from->isError()) {
    return;
  }
  if (from->isString()) {
    return this->startBufferFromString(call, task.member);
  }
  if (from->bytes() == Bytes::ArrayBuffer) {
    auto what = ErrorTextBufferStatic::buffer();
    this->toBuiltinCall(call, Builtin::BytesView, this->bufferType());

    return this->pushView(call, 1, what);
  }
  if (call->args.size() > 1) {
    auto message = ErrorTextBufferStatic::offsetOnly(from->name());

    return this->state.diagnostics.error(call->args[1]->loc(), message);
  }
  if (!this->matchBytesSource(call->args[0], this->bufferType())) {
    return;
  }
  Builtin builtin = from->isTypedArray() ? Builtin::BytesFromBytes : Builtin::BytesFromArray;

  return this->toBuiltinCall(call, builtin, this->bufferType());
}

void LibraryBytesRegistry::startBufferFromString(CallExpr *call, MemberExpr *member) {
  if (call->args.size() > 2) {
    auto encoded = this->state.library.common.checkArity(call, 1, 2);
    if (encoded.isErr()) {
      this->state.diagnostics.error(member->loc(), encoded.error().message);
    }

    return;
  }
  call->args.resize(2, nullptr);
  this->toBuiltinCall(call, Builtin::BufferFromString, this->bufferType());

  return this->pushEncodingOrUtf8(call, 1);
}

void LibraryBytesRegistry::pushEncodingOrUtf8(CallExpr *call, size_t index) {
  auto &tasks = this->state.walk.tasks;
  if (call->args[index] == nullptr) {
    call->args[index] = this->makeUtf8Literal(call->loc());

    return;
  }
  tasks.pushTask(WriteEncodingArg{.call = call, .index = index});

  return tasks.pushTask(BufferEncodingValue{.arg = call->args[index]});
}

void LibraryBytesRegistry::writeEncodingArg(const WriteEncodingArg &task) {
  task.call->args[task.index] = this->state.walk.tasks.takeChecked();
}

void LibraryBytesRegistry::pushView(CallExpr *call, int64_t elementSize, const std::string &what) {
  auto &tasks = this->state.walk.tasks;
  tasks.pushTask(FinishView{.call = call, .elementSize = elementSize, .what = what});

  return this->pushPositions(call, 1);
}

void LibraryBytesRegistry::finishView(const FinishView &task) {
  CallExpr *call = task.call;
  const Expr *offset = call->args.size() > 1 ? call->args[1] : nullptr;
  const Expr *length = call->args.size() > 2 ? call->args[2] : nullptr;

  return this->walkView(call->args[0], offset, length, task.elementSize, task.what);
}

void LibraryBytesRegistry::startAlloc(CallExpr *call, MemberExpr *member, bool isUnsafe) {
  auto &tasks = this->state.walk.tasks;
  tasks.pushChecked(call);
  unsigned most = isUnsafe ? 1 : 3;
  if (!this->hasArity(call, 1, most, member->loc())) {
    return;
  }
  tasks.pushTask(AfterAllocSize{.call = call, .isUnsafe = isUnsafe});

  return tasks.pushTask(CheckPosition{.slot = &call->args[0]});
}

void LibraryBytesRegistry::afterAllocSize(const AfterAllocSize &task) {
  CallExpr *call = task.call;
  auto &tasks = this->state.walk.tasks;
  auto size = ErrorTextBufferStatic::size();
  auto between = this->state.flow.proofs.requireBetween(call->args[0], 0, std::nullopt, size);
  this->reportUnprovenBetween(between, call->args[0]->loc());
  if (call->args.size() == 1) {
    auto *isZeroed = this->makeBoolLiteral(call->loc(), !task.isUnsafe);
    call->args.push_back(isZeroed);

    return this->toBuiltinCall(call, Builtin::BytesNew, this->bufferType());
  }
  tasks.pushTask(AfterAllocFill{.call = call});

  return tasks.pushTask(CheckExprSlot{.slot = &call->args[1], .expected = nullptr});
}

void LibraryBytesRegistry::afterAllocFill(const AfterAllocFill &task) {
  CallExpr *call = task.call;
  auto &tasks = this->state.walk.tasks;
  Expr *fill = call->args[1];
  const Type *fillType = fill->type;
  if (fillType->isError()) {
    return;
  }
  const auto *uint8 = this->uint8Type();
  bool isInteger = fillType->isInteger();
  bool isBytes = fillType->isTypedArray() &&
                 this->state.types.relations.isImplicitlyConvertible(fillType, uint8);
  if (!isInteger && !isBytes && !fillType->isString()) {
    auto message = ErrorTextBufferStatic::allocFill(fillType->name());

    return this->state.diagnostics.error(fill->loc(), message);
  }
  tasks.pushTask(FinishAllocFill{.call = call, .fillType = fillType});
  if (call->args.size() == 3) {
    tasks.pushTask(BufferEncodingValue{.arg = call->args[2]});
  }
  if (isInteger) {
    tasks.pushTask(AfterByteFill{.slot = &call->args[1]});

    return tasks.pushTask(ConvertSlot{.slot = &call->args[1], .to = Type::intTy()});
  }
  if (isBytes) {
    return tasks.pushTask(ConvertSlot{.slot = &call->args[1], .to = uint8});
  }
}

void LibraryBytesRegistry::afterByteFill(const AfterByteFill &task) {
  const Expr *converted = *task.slot;
  auto byte = ErrorTextBufferLabel::byte();
  auto fitting = this->state.flow.proofs.checkValueFits(converted, this->bufferType(), byte);
  if (fitting.isErr()) {
    this->state.diagnostics.error(converted->loc(), fitting.error().message);
  }
}

void LibraryBytesRegistry::finishAllocFill(const FinishAllocFill &task) {
  CallExpr *call = task.call;
  bool hasEncoding = call->args.size() == 3;
  Expr *encoding =
      hasEncoding ? this->state.walk.tasks.takeChecked() : this->makeUtf8Literal(call->loc());
  if (hasEncoding && !task.fillType->isString()) {
    auto message = ErrorTextBufferStatic::fillCoding();
    this->state.diagnostics.error(encoding->loc(), message);
  }
  Expr *size = call->args[0];
  Expr *fill = call->args[1];
  call->args = {size, fill, encoding};

  return this->toBuiltinCall(call, Builtin::BufferAllocFill, this->bufferType());
}

void LibraryBytesRegistry::startConcat(CallExpr *call, MemberExpr *member) {
  auto &tasks = this->state.walk.tasks;
  tasks.pushChecked(call);
  if (!this->hasArity(call, 1, 2, member->loc())) {
    return;
  }
  const Type *list = Type::array(this->uint8Type());
  tasks.pushTask(AfterConcatList{.call = call});

  return tasks.pushTask(CheckExprSlot{.slot = &call->args[0], .expected = list});
}

void LibraryBytesRegistry::afterConcatList(const AfterConcatList &task) {
  CallExpr *call = task.call;
  auto &tasks = this->state.walk.tasks;
  const Expr *list = call->args[0];
  if (list->type->isError()) {
    return;
  }
  auto sources = this->checkConcatSources(list->type, this->bufferType());
  if (sources.isErr()) {
    return this->state.diagnostics.error(list->loc(), sources.error().message);
  }
  this->toBuiltinCall(call, Builtin::BufferConcat, this->bufferType());
  if (call->args.size() != 2) {
    return;
  }
  tasks.pushTask(AfterConcatTotal{.call = call});

  return tasks.pushTask(CheckPosition{.slot = &call->args[1]});
}

void LibraryBytesRegistry::afterConcatTotal(const AfterConcatTotal &task) {
  const Expr *total = task.call->args[1];
  auto what = ErrorTextBufferStatic::total();
  auto between = this->state.flow.proofs.requireBetween(total, 0, std::nullopt, what);

  return this->reportUnprovenBetween(between, total->loc());
}

void LibraryBytesRegistry::startByteLength(CallExpr *call, MemberExpr *member) {
  auto &tasks = this->state.walk.tasks;
  tasks.pushChecked(call);
  if (!this->hasArity(call, 1, 2, member->loc())) {
    return;
  }
  tasks.pushTask(AfterByteLengthArg{.call = call});

  return tasks.pushTask(CheckExprSlot{.slot = &call->args[0], .expected = nullptr});
}

void LibraryBytesRegistry::afterByteLengthArg(const AfterByteLengthArg &task) {
  CallExpr *call = task.call;
  const Type *measured = call->args[0]->type;
  if (measured->isError()) {
    return;
  }
  bool hasBytes = measured->isByteView() || measured->bytes() == Bytes::ArrayBuffer;
  if (!measured->isString() && !hasBytes) {
    auto message = ErrorTextBufferStatic::byteLength(measured->name());

    return this->state.diagnostics.error(call->args[0]->loc(), message);
  }
  this->toBuiltinCall(call, Builtin::BufferByteLength, Type::intTy());
  if (!measured->isString()) {
    return;
  }
  call->args.resize(2, nullptr);

  return this->pushEncodingOrUtf8(call, 1);
}

void LibraryBytesRegistry::startBufferCompare(CallExpr *call, MemberExpr *member) {
  auto &tasks = this->state.walk.tasks;
  tasks.pushChecked(call);
  if (!this->hasArity(call, 2, 2, member->loc())) {
    return;
  }
  this->toBuiltinCall(call, Builtin::BufferCompare, Type::intTy());
  for (Expr *&arg : llvm::reverse(call->args)) {
    tasks.pushTask(CheckArgSlot{.slot = &arg, .to = this->uint8Type()});
  }
}

void LibraryBytesRegistry::startIsBuffer(CallExpr *call, MemberExpr *member) {
  auto &tasks = this->state.walk.tasks;
  if (!this->hasArity(call, 1, 1, member->loc())) {
    return tasks.pushChecked(call);
  }
  tasks.pushTask(AfterIsBufferArg{.call = call, .member = member});

  return tasks.pushTask(CheckExprSlot{.slot = &call->args[0], .expected = nullptr});
}

void LibraryBytesRegistry::afterIsBufferArg(const AfterIsBufferArg &task) {
  CallExpr *call = task.call;
  auto &tasks = this->state.walk.tasks;
  const Type *tested = call->args[0]->type;
  const Type *base = tested->nonOptional();
  llvm::ArrayRef<const Type *> members =
      base->isUnion() ? base->members() : llvm::ArrayRef<const Type *>(base);
  auto canBe = std::bind_front(&LibraryBytesRegistry::canBeBuffer, this);
  auto isBuffer = std::bind_front(&LibraryBytesRegistry::isBufferType, this);
  bool isUndecided = !tested->isError() && llvm::any_of(members, canBe) &&
                     (tested->isOptional() || !llvm::all_of(members, isBuffer));
  if (isUndecided) {
    auto *test = this->makeBufferTest(call, task.member);

    return tasks.pushChecked(test);
  }
  this->toBuiltinCall(call, Builtin::BufferIsBuffer, Type::boolTy());

  return tasks.pushChecked(call);
}

bool LibraryBytesRegistry::canBeBuffer(const Type *type) const {
  return type->isBuffer() || type->bytes() == Bytes::Uint8;
}

bool LibraryBytesRegistry::isBufferType(const Type *type) const {
  return type->isBuffer();
}

void LibraryBytesRegistry::checkIsEncoding(CallExpr *call, MemberExpr *member) {
  auto &tasks = this->state.walk.tasks;
  if (!this->hasArity(call, 1, 1, member->loc())) {
    return tasks.pushChecked(call);
  }
  auto text = this->literalText(call->args[0]);
  if (text.has_value()) {
    auto utf8 = ToUtf8::toUtf8(*text);
    bool isKnown = EncodingNameTable::find(utf8).has_value();
    auto *known = this->makeBoolLiteral(call->loc(), isKnown);

    return tasks.pushChecked(known);
  }
  this->toBuiltinCall(call, Builtin::BufferIsEncoding, Type::boolTy());
  tasks.pushChecked(call);

  return tasks.pushTask(CheckArgSlot{.slot = &call->args[0], .to = Type::string()});
}

void LibraryBytesRegistry::startTextDecoderParse(CallExpr *call, MemberExpr *member) {
  auto &tasks = this->state.walk.tasks;
  auto decoderName = Type::bytesType(Bytes::TextDecoder)->name();
  auto *expr =
      this->state.ast.make<NewExpr>(call->loc(), decoderName, std::vector<TypeRef>{}, call->args);
  this->state.library.decoder.enterDecoderParse();
  tasks.pushTask(AfterDecoderParse{.member = member});

  return tasks.pushTask(
      CheckBytesNew{.newExpr = expr, .bytes = Type::bytesType(Bytes::TextDecoder)});
}

void LibraryBytesRegistry::afterDecoderParse(const AfterDecoderParse &task) {
  auto &tasks = this->state.walk.tasks;
  this->state.library.decoder.leaveDecoderParse();
  Expr *made = tasks.takeChecked();
  auto *result = dyn_cast<CallExpr>(made);
  if (result == nullptr || result->builtin() != Builtin::TextDecoderNew) {
    return tasks.pushChecked(made);
  }
  tasks.pushTask(FinishDecoderParse{.result = result, .member = task.member});

  return tasks.pushTask(PreludeTypeOf{.type = PreludeType::ENCODING_ERROR});
}

void LibraryBytesRegistry::finishDecoderParse(const FinishDecoderParse &task) {
  CallExpr *result = task.result;
  auto &tasks = this->state.walk.tasks;
  const Type *encodingError = tasks.takeAnswer();
  result->callee = task.member;
  result->target = BuiltinCall{.builtin = Builtin::TextDecoderParse};
  result->type = Type::result(result->type, encodingError);

  return tasks.pushChecked(result);
}

void LibraryBytesRegistry::checkBytesNew(const CheckBytesNew &task) {
  NewExpr *expr = task.newExpr;
  const Type *type = task.bytes;
  llvm::SMLoc loc = expr->loc();
  auto *callee = this->state.ast.make<NameExpr>(loc, type->name());
  auto *call = this->state.ast.make<CallExpr>(loc, callee, expr->args);
  if (!expr->typeArgs.empty()) {
    auto message = ErrorTextBytesNew::typeArgs(type->name());
    this->state.diagnostics.error(loc, message);
  }
  call->type = Type::error();
  switch (type->bytes()) {
    case Bytes::Buffer:
      return this->reportBytesNew(expr, ErrorTextBytesNew::buffer());
    case Bytes::ArrayBuffer:
      return this->startArrayBufferNew(expr, call, type);
    case Bytes::DataView:
      return this->startDataViewNew(call, type);
    case Bytes::TextEncoder:
      return this->checkTextEncoderNew(call, type);
    case Bytes::TextDecoder:
      return this->startTextDecoderNew(call, type);
    case Bytes::FatalTextDecoder:
      return this->reportBytesNew(expr, ErrorTextBytesNew::fatal());
    case Bytes::None:
    case Bytes::Uint8:
    case Bytes::Int8:
    case Bytes::Uint8Clamped:
    case Bytes::Int16:
    case Bytes::Uint16:
    case Bytes::Int32:
    case Bytes::Uint32:
    case Bytes::Float16:
    case Bytes::Float32:
    case Bytes::Float64:
      return this->startTypedArrayNew(call, type);
  }
}

void LibraryBytesRegistry::reportBytesNew(NewExpr *expr, const std::string &message) {
  auto &tasks = this->state.walk.tasks;
  this->state.diagnostics.error(expr->loc(), message);
  tasks.pushChecked(expr);
  for (Expr *&arg : llvm::reverse(expr->args)) {
    tasks.pushTask(CheckExprSlot{.slot = &arg, .expected = nullptr});
  }
}

bool LibraryBytesRegistry::hasNewArity(CallExpr *call, unsigned min, unsigned max) {
  return this->hasArity(call, min, max, call->loc());
}

void LibraryBytesRegistry::startArrayBufferNew(NewExpr *expr, CallExpr *call, const Type *type) {
  auto &tasks = this->state.walk.tasks;
  if (expr->args.size() == 2) {
    auto message = ErrorTextBytesNew::resizable();
    this->state.diagnostics.error(expr->args[1]->loc(), message);
    tasks.pushChecked(expr);
    for (Expr *&arg : llvm::reverse(expr->args)) {
      tasks.pushTask(CheckExprSlot{.slot = &arg, .expected = nullptr});
    }

    return;
  }
  tasks.pushChecked(call);
  if (!this->hasNewArity(call, 1, 1)) {
    return;
  }
  this->toBuiltinCall(call, Builtin::ArrayBufferNew, type);
  tasks.pushTask(AfterNonNegative{.slot = &call->args[0], .what = ErrorTextBytesNew::bufferLen()});

  return tasks.pushTask(CheckPosition{.slot = &call->args[0]});
}

void LibraryBytesRegistry::afterNonNegative(const AfterNonNegative &task) {
  return this->walkNonNegative(*task.slot, task.what);
}

void LibraryBytesRegistry::walkNonNegative(const Expr *value, const std::string &what) {
  auto between = this->state.flow.proofs.requireBetween(value, 0, std::nullopt, what);

  return this->reportUnprovenBetween(between, value->loc());
}

void LibraryBytesRegistry::startDataViewNew(CallExpr *call, const Type *type) {
  auto &tasks = this->state.walk.tasks;
  tasks.pushChecked(call);
  if (!this->hasNewArity(call, 1, 3)) {
    return;
  }
  tasks.pushTask(AfterDataViewBuffer{.call = call, .type = type});

  return tasks.pushTask(CheckExprSlot{.slot = &call->args[0], .expected = nullptr});
}

void LibraryBytesRegistry::afterDataViewBuffer(const AfterDataViewBuffer &task) {
  CallExpr *call = task.call;
  auto viewed = this->checkViewedBuffer(call->args[0]->type);
  if (viewed.isErr()) {
    return this->state.diagnostics.error(call->args[0]->loc(), viewed.error().message);
  }
  this->toBuiltinCall(call, Builtin::DataViewNew, task.type);
  auto what = ErrorTextBytesNew::dataView();

  return this->pushView(call, 1, what);
}

void LibraryBytesRegistry::checkTextEncoderNew(CallExpr *call, const Type *type) {
  this->state.walk.tasks.pushChecked(call);
  if (!this->hasNewArity(call, 0, 0)) {
    return;
  }

  return this->toBuiltinCall(call, Builtin::TextEncoderNew, type);
}

void LibraryBytesRegistry::startTextDecoderNew(CallExpr *call, const Type *type) {
  auto &tasks = this->state.walk.tasks;
  tasks.pushChecked(call);
  if (!this->hasNewArity(call, 0, 2)) {
    return;
  }
  auto *resolved = this->makeIntLiteral(call->loc(), 1);
  auto decoder =
      DecoderNewDraft{.call = call, .type = type, .resolved = resolved, .label = nullptr};
  bool isGiven = !call->args.empty() && !isa<UndefinedExpr>(call->args[0]);
  if (!isGiven) {
    auto utf8 = ToUtf16::toUtf16(EncodingNameTable::getUtf8Label());
    decoder.label = this->decoderNameLiteral(call->loc(), utf8);
    if (call->args.empty()) {
      return this->continueDecoderNew(ContinueDecoderNew{.decoder = decoder});
    }
    tasks.pushTask(ContinueDecoderNew{.decoder = decoder});

    return tasks.pushTask(CheckExprSlot{.slot = &call->args[0], .expected = nullptr});
  }
  Expr *label = call->args[0];
  auto text = this->literalText(label);
  if (text.has_value()) {
    auto name = this->knownDecoderName(label, *text);
    if (!name.has_value()) {
      return;
    }
    decoder.label = this->decoderNameLiteral(call->loc(), *name);

    return this->continueDecoderNew(ContinueDecoderNew{.decoder = decoder});
  }
  resolved->value = 0;
  tasks.pushTask(AfterDecoderLabelArg{.decoder = decoder, .labelLoc = label->loc()});

  return tasks.pushTask(CheckArgSlot{.slot = &call->args[0], .to = Type::string()});
}

void LibraryBytesRegistry::afterDecoderLabelArg(const AfterDecoderLabelArg &task) {
  DecoderNewDraft decoder = task.decoder;
  Expr *label = decoder.call->args[0];
  if (!this->matchDecoderLabels(label, task.labelLoc)) {
    return;
  }
  decoder.label = label;

  return this->continueDecoderNew(ContinueDecoderNew{.decoder = decoder});
}

void LibraryBytesRegistry::continueDecoderNew(const ContinueDecoderNew &task) {
  const DecoderNewDraft &decoder = task.decoder;
  CallExpr *call = decoder.call;
  auto &tasks = this->state.walk.tasks;
  if (call->args.size() != 2) {
    return this->finishDecoderNew(FinishDecoderNew{.decoder = decoder, .isFatal = false});
  }
  const auto *options = this->decoderOptionsType();
  tasks.pushTask(AfterDecoderOptions{.decoder = decoder});

  return tasks.pushTask(CheckExprSlot{.slot = &call->args[1], .expected = options});
}

void LibraryBytesRegistry::afterDecoderOptions(const AfterDecoderOptions &task) {
  CallExpr *call = task.decoder.call;
  auto &tasks = this->state.walk.tasks;
  bool isFatal = this->canBeFatal(call->args[1]);
  const auto *options = this->decoderOptionsType();
  auto what = ErrorTextTextDecoder::options();
  tasks.pushTask(FinishDecoderNew{.decoder = task.decoder, .isFatal = isFatal});

  return tasks.pushTask(CoerceSlot{.slot = &call->args[1], .to = options, .what = what});
}

void LibraryBytesRegistry::finishDecoderNew(const FinishDecoderNew &task) {
  const DecoderNewDraft &decoder = task.decoder;
  CallExpr *call = decoder.call;
  auto checked = std::vector<Expr *>{decoder.resolved, decoder.label};
  if (call->args.size() == 2) {
    checked.push_back(call->args[1]);
  }
  call->args = std::move(checked);
  const Type *made = task.isFatal ? Type::bytesType(Bytes::FatalTextDecoder) : decoder.type;

  return this->toBuiltinCall(call, Builtin::TextDecoderNew, made);
}

void LibraryBytesRegistry::startTypedArrayNew(CallExpr *call, const Type *type) {
  auto &tasks = this->state.walk.tasks;
  if (call->args.empty()) {
    auto *zero = this->makeIntLiteral(call->loc(), 0);
    call->args = {zero};
    this->toBuiltinCall(call, Builtin::BytesNew, type);

    return tasks.pushChecked(call);
  }
  if (call->args.size() > 3) {
    this->hasNewArity(call, 0, 3);

    return tasks.pushChecked(call);
  }
  auto *values = dyn_cast<ArrayLitExpr>(call->args[0]);
  if (values != nullptr && call->args.size() == 1) {
    return this->startBytesValues(call, values, type);
  }
  tasks.pushChecked(call);
  tasks.pushTask(AfterTypedNewSource{.call = call, .type = type});

  return this->pushFusedSource(call);
}

void LibraryBytesRegistry::afterTypedNewSource(const AfterTypedNewSource &task) {
  CallExpr *call = task.call;
  const Type *type = task.type;
  auto &tasks = this->state.walk.tasks;
  const Expr *source = call->args[0];
  const Type *from = source->type;
  if (from->isError()) {
    return;
  }
  if (from->bytes() == Bytes::ArrayBuffer) {
    auto what = ErrorTextBytesNew::the(type->name());
    this->toBuiltinCall(call, Builtin::BytesView, type);

    return this->pushView(call, int64_t(type->elementSize()), what);
  }
  if (call->args.size() > 1) {
    auto message = ErrorTextBytesNew::offsetOnly(type->name(), from->name());

    return this->state.diagnostics.error(call->args[1]->loc(), message);
  }
  if (from->isInteger()) {
    auto what = ErrorTextBytesNew::lengthOf(type->name());
    tasks.pushTask(FinishLengthNew{.call = call, .type = type, .what = what});

    return tasks.pushTask(ConvertSlot{.slot = &call->args[0], .to = Type::intTy()});
  }
  if (from->isFloat()) {
    auto message = ErrorTextBytesNew::floatLen();

    return this->state.diagnostics.error(source->loc(), message);
  }
  if (!this->matchBytesSource(call->args[0], type)) {
    return;
  }
  Builtin builtin = from->isTypedArray() ? Builtin::BytesFromBytes : Builtin::BytesFromArray;

  return this->toBuiltinCall(call, builtin, type);
}

void LibraryBytesRegistry::finishLengthNew(const FinishLengthNew &task) {
  CallExpr *call = task.call;
  this->walkNonNegative(call->args[0], task.what);
  auto *zeroed = this->makeBoolLiteral(call->loc(), true);
  call->args.push_back(zeroed);

  return this->toBuiltinCall(call, Builtin::BytesNew, task.type);
}

void LibraryBytesRegistry::startTypedArrayMethod(CallExpr *call, MemberExpr *member,
                                                 const Type *expected) {
  const auto *found = BytesMethodTable::find(member->name);
  if (found == nullptr) {
    return this->reportMissingMethod(call, member, found);
  }
  auto typed = this->describeCall(call, member);
  const BytesMethodForm &form = found->form;
  switch (form.kind()) {
    case BytesMethodKind::TYPED_ARRAY_CALLBACK:
      return this->startCallbackCall(typed, found->method, expected);
    case BytesMethodKind::TYPED_ARRAY_SORT:
      return this->startSort(typed, *form.as<TypedArraySortMethod>(), expected);
    case BytesMethodKind::TYPED_ARRAY_REVERSE:
      return this->checkReverse(typed, *form.as<TypedArrayReverseMethod>());
    case BytesMethodKind::TYPED_ARRAY_AT:
      return this->startAt(typed);
    case BytesMethodKind::TYPED_ARRAY_SEARCH:
      return this->startTypedSearch(typed, *form.as<TypedArraySearchMethod>());
    case BytesMethodKind::TYPED_ARRAY_JOIN:
      return this->startJoin(typed, *form.as<TypedArrayJoinMethod>());
    case BytesMethodKind::TYPED_ARRAY_FILL:
      return this->startTypedFill(typed);
    case BytesMethodKind::TYPED_ARRAY_SET:
      return this->startTypedSet(typed);
    case BytesMethodKind::TYPED_ARRAY_POSITIONS:
      return this->startPositionsMethod(typed, *form.as<TypedArrayPositionsMethod>());
    case BytesMethodKind::TYPED_ARRAY_WITH:
      return this->startWith(typed);
    case BytesMethodKind::TYPED_ARRAY_ITERATOR:
      return this->startTypedIterator(typed);
    case BytesMethodKind::NOT_ON_TYPED_ARRAY:
      return this->reportMissingMethod(call, member, found);
  }
}

TypedArrayDraft LibraryBytesRegistry::describeCall(CallExpr *call, MemberExpr *member) const {
  const Type *arrayType = member->object->type;
  const Type *copyType = arrayType->isBuffer() ? this->uint8Type() : arrayType;
  auto what = ErrorTextByteView::element(arrayType->name());

  return TypedArrayDraft{
      .call = call,
      .member = member,
      .arrayType = arrayType,
      .element = arrayType->elementType(),
      .copyType = copyType,
      .what = what,
  };
}

void LibraryBytesRegistry::startCallbackCall(const TypedArrayDraft &typed, BytesMethod method,
                                             const Type *expected) {
  CallExpr *call = typed.call;
  auto &tasks = this->state.walk.tasks;
  bool isArrow = !call->args.empty() && isa<FunctionExpr>(call->args[0]);
  bool isMappingNamedFunction = method == BytesMethod::MAP && !call->args.empty() && !isArrow;
  if (isMappingNamedFunction) {
    tasks.pushTask(AfterMappedCallback{.typed = typed});
  }

  return tasks.pushTask(
      CheckCallbackMethod{.call = call, .member = typed.member, .expected = expected});
}

void LibraryBytesRegistry::afterMappedCallback(const AfterMappedCallback &task) {
  auto fits = this->checkTypedMapFits(task.typed);
  if (fits.isErr()) {
    this->state.diagnostics.error(task.typed.call->args[0]->loc(), fits.error().message);
  }
}

void LibraryBytesRegistry::startSort(const TypedArrayDraft &typed, const TypedArraySortMethod &sort,
                                     const Type *expected) {
  CallExpr *call = typed.call;
  auto &tasks = this->state.walk.tasks;
  bool isCopying = sort.place == ReorderPlace::NEW_COPY;
  if (call->args.empty()) {
    const Type *sorted = isCopying ? typed.copyType : typed.arrayType;
    this->toBuiltinCall(call, sort.builtin, sorted);

    return tasks.pushChecked(call);
  }
  if (isCopying) {
    tasks.pushTask(AfterCopySort{.call = call, .copyType = typed.copyType});
  }

  return tasks.pushTask(
      CheckCallbackMethod{.call = call, .member = typed.member, .expected = expected});
}

void LibraryBytesRegistry::afterCopySort(const AfterCopySort &task) {
  if (!task.call->type->isError()) {
    task.call->type = task.copyType;
  }
}

void LibraryBytesRegistry::checkReverse(const TypedArrayDraft &typed,
                                        const TypedArrayReverseMethod &reverse) {
  CallExpr *call = typed.call;
  this->state.walk.tasks.pushChecked(call);
  if (!this->hasArity(call, 0, 0, typed.member->loc())) {
    return;
  }
  bool isCopying = reverse.place == ReorderPlace::NEW_COPY;
  const Type *reversed = isCopying ? typed.copyType : typed.arrayType;

  return this->toBuiltinCall(call, reverse.builtin, reversed);
}

void LibraryBytesRegistry::startAt(const TypedArrayDraft &typed) {
  CallExpr *call = typed.call;
  auto &tasks = this->state.walk.tasks;
  tasks.pushChecked(call);
  if (!this->hasArity(call, 1, 1, typed.member->loc())) {
    return;
  }
  tasks.pushTask(FinishTypedAt{.typed = typed});

  return tasks.pushTask(CheckPosition{.slot = &call->args[0]});
}

void LibraryBytesRegistry::finishTypedAt(const FinishTypedAt &task) {
  const TypedArrayDraft &typed = task.typed;
  CallExpr *call = typed.call;
  bool isInRange = this->state.flow.proofs.canProveAtInBounds(typed.member->object, call->args[0]);
  const Type *answer = isInRange ? typed.element : Type::optional(typed.element);
  call->mayBeOutOfRange = !isInRange;

  return this->toBuiltinCall(call, Builtin::BytesAt, answer);
}

void LibraryBytesRegistry::startTypedSearch(const TypedArrayDraft &typed,
                                            const TypedArraySearchMethod &search) {
  CallExpr *call = typed.call;
  auto &tasks = this->state.walk.tasks;
  tasks.pushChecked(call);
  if (!this->hasArity(call, 1, 2, typed.member->loc())) {
    return;
  }
  tasks.pushTask(AfterTypedSearchValue{.typed = typed, .mode = search.mode});

  return tasks.pushTask(NumericArgSlot{.slot = &call->args[0], .hint = typed.element});
}

void LibraryBytesRegistry::afterTypedSearchValue(const AfterTypedSearchValue &task) {
  CallExpr *call = task.typed.call;
  auto &tasks = this->state.walk.tasks;
  if (!call->args[0]->type->isNumeric()) {
    return;
  }
  tasks.pushTask(FinishTypedSearch{.typed = task.typed, .mode = task.mode});
  if (call->args.size() == 2) {
    tasks.pushTask(CheckPosition{.slot = &call->args[1]});
  }

  return tasks.pushTask(ConvertSlot{.slot = &call->args[0], .to = Type::doubleTy()});
}

void LibraryBytesRegistry::finishTypedSearch(const FinishTypedSearch &task) {
  CallExpr *call = task.typed.call;
  int runtimeMode = 0;
  const auto *answer = Type::intTy();
  switch (task.mode) {
    case BytesSearchMode::INDEX_OF:
      break;
    case BytesSearchMode::LAST_INDEX_OF:
      runtimeMode = 1;
      break;
    case BytesSearchMode::INCLUDES:
      runtimeMode = 2;
      answer = Type::boolTy();
      break;
  }
  auto *mode = this->makeIntLiteral(call->loc(), runtimeMode);
  auto args = std::vector<Expr *>{mode, call->args[0]};
  if (call->args.size() == 2) {
    args.push_back(call->args[1]);
  }
  call->args = std::move(args);

  return this->toBuiltinCall(call, Builtin::BytesIndexOf, answer);
}

void LibraryBytesRegistry::startJoin(const TypedArrayDraft &typed,
                                     const TypedArrayJoinMethod &join) {
  CallExpr *call = typed.call;
  auto &tasks = this->state.walk.tasks;
  tasks.pushChecked(call);
  auto maxArgs = static_cast<unsigned>(join.maxArgs);
  if (!this->hasArity(call, 0, maxArgs, typed.member->loc())) {
    return;
  }
  this->toBuiltinCall(call, Builtin::BytesJoin, Type::string());
  if (call->args.empty()) {
    return;
  }

  return tasks.pushTask(CheckArgSlot{.slot = &call->args[0], .to = Type::string()});
}

void LibraryBytesRegistry::startTypedFill(const TypedArrayDraft &typed) {
  CallExpr *call = typed.call;
  auto &tasks = this->state.walk.tasks;
  tasks.pushChecked(call);
  if (!this->hasArity(call, 1, 3, typed.member->loc())) {
    return;
  }
  this->toBuiltinCall(call, Builtin::BytesFill, typed.arrayType);
  tasks.pushTask(AfterTypedValue{.typed = typed, .slot = &call->args[0], .positionsFrom = 1});

  return tasks.pushTask(CheckArgSlot{.slot = &call->args[0], .to = typed.element});
}

void LibraryBytesRegistry::afterTypedValue(const AfterTypedValue &task) {
  const TypedArrayDraft &typed = task.typed;
  auto &tasks = this->state.walk.tasks;
  const Expr *value = *task.slot;
  auto fitting = this->state.flow.proofs.checkValueFits(value, typed.arrayType, typed.what);
  if (fitting.isErr()) {
    this->state.diagnostics.error(value->loc(), fitting.error().message);
  }
  if (task.positionsFrom != 0) {
    this->pushPositions(typed.call, task.positionsFrom);
  }

  return tasks.pushTask(ConvertSlot{.slot = task.slot, .to = Type::doubleTy()});
}

void LibraryBytesRegistry::startTypedSet(const TypedArrayDraft &typed) {
  CallExpr *call = typed.call;
  auto &tasks = this->state.walk.tasks;
  tasks.pushChecked(call);
  if (!this->hasArity(call, 1, 2, typed.member->loc())) {
    return;
  }
  auto *literal = dyn_cast<ArrayLitExpr>(call->args[0]);
  if (literal != nullptr) {
    const auto *literalType = Type::array(typed.element);
    tasks.pushTask(AfterSetLiteral{.typed = typed, .literal = literal});

    return tasks.pushTask(CheckExprSlot{.slot = &call->args[0], .expected = literalType});
  }
  tasks.pushTask(AfterSetSource{.typed = typed});

  return tasks.pushTask(CheckExprSlot{.slot = &call->args[0], .expected = nullptr});
}

void LibraryBytesRegistry::afterSetLiteral(const AfterSetLiteral &task) {
  const TypedArrayDraft &typed = task.typed;
  if (typed.call->args[0]->type->isError()) {
    return;
  }
  for (const ArrayLitExpr::Element &item : task.literal->elements) {
    if (item.spread) {
      if (!this->matchBytesSource(item.expr, typed.arrayType)) {
        return;
      }
      continue;
    }
    auto fitting = this->state.flow.proofs.checkValueFits(item.expr, typed.arrayType, typed.what);
    if (fitting.isErr()) {
      this->state.diagnostics.error(item.expr->loc(), fitting.error().message);
    }
  }

  return this->continueTypedSet(typed);
}

void LibraryBytesRegistry::afterSetSource(const AfterSetSource &task) {
  const TypedArrayDraft &typed = task.typed;
  Expr *source = typed.call->args[0];
  auto set = this->checkSetSource(source);
  if (set.isErr()) {
    return this->state.diagnostics.error(source->loc(), set.error().message);
  }
  if (!this->matchBytesSource(source, typed.arrayType)) {
    return;
  }

  return this->continueTypedSet(typed);
}

void LibraryBytesRegistry::continueTypedSet(const TypedArrayDraft &typed) {
  this->state.walk.tasks.pushTask(FinishTypedSet{.typed = typed});

  return this->pushPositions(typed.call, 1);
}

void LibraryBytesRegistry::finishTypedSet(const FinishTypedSet &task) {
  const TypedArrayDraft &typed = task.typed;
  CallExpr *call = typed.call;
  const Expr *offset = call->args.size() > 1 ? call->args[1] : nullptr;
  this->walkSetRange(typed.member->object, call->args[0], offset);

  return this->toBuiltinCall(call, Builtin::BytesSet, Type::voidTy());
}

void LibraryBytesRegistry::startPositionsMethod(const TypedArrayDraft &typed,
                                                const TypedArrayPositionsMethod &positions) {
  CallExpr *call = typed.call;
  this->state.walk.tasks.pushChecked(call);
  auto minArgs = static_cast<unsigned>(positions.minArgs);
  auto maxArgs = static_cast<unsigned>(positions.maxArgs);
  if (!this->hasArity(call, minArgs, maxArgs, typed.member->loc())) {
    return;
  }
  this->toBuiltinCall(call, positions.builtin, typed.arrayType);

  return this->pushPositions(call, 0);
}

void LibraryBytesRegistry::startWith(const TypedArrayDraft &typed) {
  CallExpr *call = typed.call;
  auto &tasks = this->state.walk.tasks;
  tasks.pushChecked(call);
  if (!this->hasArity(call, 2, 2, typed.member->loc())) {
    return;
  }
  this->toBuiltinCall(call, Builtin::BytesWith, typed.copyType);
  tasks.pushTask(AfterWithIndex{.typed = typed});

  return tasks.pushTask(CheckPosition{.slot = &call->args[0]});
}

void LibraryBytesRegistry::afterWithIndex(const AfterWithIndex &task) {
  const TypedArrayDraft &typed = task.typed;
  CallExpr *call = typed.call;
  auto &tasks = this->state.walk.tasks;
  const Expr *index = call->args[0];
  auto proven = this->checkWithIndex(typed.member->object, index);
  if (proven.isErr()) {
    this->state.diagnostics.error(index->loc(), proven.error().message);
  }
  tasks.pushTask(AfterTypedValue{.typed = typed, .slot = &call->args[1], .positionsFrom = 0});

  return tasks.pushTask(CheckArgSlot{.slot = &call->args[1], .to = typed.element});
}

void LibraryBytesRegistry::startTypedIterator(const TypedArrayDraft &typed) {
  CallExpr *call = typed.call;
  MemberExpr *member = typed.member;
  if (!this->hasArity(call, 0, 0, member->loc())) {
    return this->state.walk.tasks.pushChecked(call);
  }
  auto *copied = this->makeTypedArrayFrom(typed);
  auto *iterated = this->state.ast.make<MemberExpr>(member->loc(), copied, member->name);
  auto *iterator = this->state.ast.make<CallExpr>(call->loc(), iterated, std::vector<Expr *>{});

  return this->state.walk.tasks.pushTask(CheckArrayIterator{.call = iterator, .member = iterated});
}

void LibraryBytesRegistry::reportMissingMethod(CallExpr *call, MemberExpr *member,
                                               const BytesMethodInfo *found) {
  auto &tasks = this->state.walk.tasks;
  tasks.pushChecked(call);
  bool isFlatMap = found != nullptr && found->method == BytesMethod::FLAT_MAP;
  if (isFlatMap) {
    auto message = ErrorTextTypedArray::flatMap();
    this->state.diagnostics.error(member->loc(), message);

    return tasks.pushTask(WalkArgsLoosely{.call = call});
  }
  bool isLocale = found != nullptr && found->method == BytesMethod::TO_LOCALE_STRING;
  std::string message =
      isLocale ? ErrorTextTypedArray::locale()
               : ErrorTextTypedArray::noMember(member->name, member->object->type->name());
  tasks.pushTask(ReportError{.loc = member->loc(), .message = message});

  return tasks.pushTask(WalkArgsLoosely{.call = call});
}

void LibraryBytesRegistry::startDataViewMethod(CallExpr *call, MemberExpr *member) {
  auto &tasks = this->state.walk.tasks;
  llvm::StringRef name = member->name;
  tasks.pushChecked(call);
  auto parsed = NumberMethodTable::parse(name, true);
  if (!parsed.has_value()) {
    auto viewName = Type::bytesType(Type::Bytes::DataView)->name();
    auto message = ErrorTextTypedArray::noMember(name, viewName);
    tasks.pushTask(ReportError{.loc = member->loc(), .message = message});

    return this->pushLooseArgs(call);
  }
  BytesNumberMethod number = *parsed;
  if (number.bigint) {
    auto quoted = ErrorTextBigInt::quoted(name);
    auto bigint = ErrorTextBigInt::none(quoted);
    this->state.diagnostics.error(member->loc(), bigint);

    return this->pushLooseArgs(call);
  }
  const auto *kindType = Type::bytesType(number.kind);
  const Type *element = kindType->accessType();
  bool isOneByte = kindType->elementSize() == 1;
  unsigned min = number.write ? 2 : 1;
  unsigned max = min + (isOneByte ? 0 : 1);
  if (!this->hasArity(call, min, max, member->loc())) {
    return;
  }
  tasks.pushTask(FinishDataView{.call = call, .member = member, .number = number});
  if (!number.write && element->isFloat()) {
    tasks.pushTask(PreludeTypeOf{.type = PreludeType::NUMBER_ERROR});
  }
  if (call->args.size() > min) {
    tasks.pushTask(CheckArgSlot{.slot = &call->args[min], .to = Type::boolTy()});
  }
  if (number.write) {
    tasks.pushTask(AfterDataViewValue{.call = call, .number = number});
    tasks.pushTask(CheckArgSlot{.slot = &call->args[1], .to = element});
  }

  return tasks.pushTask(CheckPosition{.slot = &call->args[0]});
}

void LibraryBytesRegistry::afterDataViewValue(const AfterDataViewValue &task) {
  const Expr *value = task.call->args[1];
  Bytes kind = task.number.kind;
  const auto *kindType = Type::bytesType(kind);
  auto what = BytesCheckHelpers::numberWhat(kind);
  auto fitting = this->state.flow.proofs.checkValueFits(value, kindType, what);
  if (fitting.isErr()) {
    this->state.diagnostics.error(value->loc(), fitting.error().message);
  }
}

void LibraryBytesRegistry::finishDataView(const FinishDataView &task) {
  CallExpr *call = task.call;
  MemberExpr *member = task.member;
  const BytesNumberMethod &number = task.number;
  const auto *kindType = Type::bytesType(number.kind);
  const Type *element = kindType->accessType();
  unsigned min = number.write ? 2 : 1;
  Expr *position = call->args[0];
  auto offsetWhat = ErrorTextTypedArray::offsetOf(member->name);
  auto size = int64_t(kindType->elementSize());
  auto upTo = this->state.flow.proofs.requireUpTo(position, member->object, size, offsetWhat);
  this->reportUnprovenUpTo(upTo, position->loc());
  call->mayBeOutOfRange = upTo.isErr();
  auto *kind = this->makeIntLiteral(call->loc(), int64_t(number.kind));
  auto args = std::vector<Expr *>{kind, position};
  if (number.write) {
    args.push_back(call->args[1]);
  }
  if (call->args.size() > min) {
    args.push_back(call->args[min]);
  }
  call->args = std::move(args);
  call->target = BuiltinCall{.builtin = number.write ? Builtin::DataViewSet : Builtin::DataViewGet};
  if (number.write) {
    call->type = Type::voidTy();

    return;
  }
  if (!element->isFloat()) {
    call->type = element;

    return;
  }
  const Type *numberError = this->state.walk.tasks.takeAnswer();
  call->type = Type::result(Type::doubleTy(), numberError);
}

void LibraryBytesRegistry::walkSetRange(const Expr *target, const Expr *source,
                                        const Expr *offset) {
  auto &proofs = this->state.flow.proofs;
  auto &measurements = this->state.flow.measurements;
  bool isOffsetUnproven = false;
  if (offset != nullptr) {
    auto offsetWhat = ErrorTextTypedArray::setOffset();
    auto between = proofs.requireBetween(offset, 0, std::nullopt, offsetWhat);
    this->reportUnprovenBetween(between, offset->loc());
    isOffsetUnproven = between.isErr();
  }
  if (isOffsetUnproven || proofs.isExempt(source->loc(), target)) {
    return;
  }
  std::optional<Linear> at =
      offset != nullptr ? measurements.linear(offset) : Linear{Term::zero(), 0};
  auto most = measurements.lengthAtMost(source);
  if (proofs.canProveSumUpTo(at, most, target)) {
    return;
  }
  std::string offsetText = offset != nullptr ? BytesCheckHelpers::valueText(offset) : std::string();
  auto sourceText = BytesCheckHelpers::valueText(source);
  auto targetText = BytesCheckHelpers::valueText(target);
  auto message = ErrorTextTypedArray::setFits(offsetText, sourceText, targetText);
  this->state.diagnostics.error(source->loc(), message);
}

void LibraryBytesRegistry::walkView(const Expr *buffer, const Expr *offset, const Expr *length,
                                    int64_t size, llvm::StringRef what) {
  auto &proofs = this->state.flow.proofs;
  auto &measurements = this->state.flow.measurements;
  if (this->isSameView(buffer, offset, length, size) || proofs.isExempt(buffer->loc(), buffer)) {
    return;
  }
  if (offset != nullptr) {
    auto offsetWhat = ErrorTextByteView::offsetOf(what);
    auto upTo = proofs.requireUpTo(offset, buffer, 0, offsetWhat);
    this->reportUnprovenUpTo(upTo, offset->loc());
    if (upTo.isErr()) {
      return;
    }
  }
  std::optional<int64_t> start = offset != nullptr ? this->state.code.constants.intConstant(offset)
                                                   : std::optional<int64_t>(0);
  auto aligned = this->checkOffsetAligned(start, size, what);
  if (aligned.isErr()) {
    return this->state.diagnostics.error(offset->loc(), aligned.error().message);
  }
  if (length == nullptr) {
    auto whole = measurements.exactLength(buffer);
    auto sized = this->checkBufferAligned(whole, size, what);
    if (sized.isErr()) {
      this->state.diagnostics.error(buffer->loc(), sized.error().message);
    }

    return;
  }
  auto lengthWhat = ErrorTextByteView::lengthOf(what);
  auto between = proofs.requireBetween(length, 0, std::nullopt, lengthWhat);
  this->reportUnprovenBetween(between, length->loc());
  if (between.isErr()) {
    return;
  }
  auto count = measurements.linear(length);
  std::optional<Linear> from =
      offset != nullptr ? measurements.linear(offset) : Linear{Term::zero(), 0};
  if (count && size > 1) {
    auto known = measurements.settled(*count);
    count = known.term.isZero() ? std::optional<Linear>(Linear{Term::zero(), known.offset * size})
                                : std::nullopt;
  }
  if (proofs.canProveSumUpTo(from, count, buffer)) {
    return;
  }
  std::string offsetText = offset != nullptr ? BytesCheckHelpers::valueText(offset) : std::string();
  auto lengthText = BytesCheckHelpers::valueText(length);
  auto end = ErrorTextByteView::end(offsetText, lengthText, size);
  auto bufferText = BytesCheckHelpers::valueText(buffer);
  auto message = ErrorTextByteView::notFits(what, end, bufferText);
  this->state.diagnostics.error(length->loc(), message);
}

void LibraryBytesRegistry::addByteCallbackFacts(const AddByteCallbackFacts &task) {
  const FunctionDecl *functionDecl = task.functionDecl;
  const auto *found = this->state.flow.callbacks.findByteCallback(functionDecl);
  if (found == nullptr || found->param >= functionDecl->params.size()) {
    return;
  }
  const ByteCallback &info = *found;
  const VarSymbol *variable = functionDecl->params[info.param].sym;
  bool isOpenRange = info.lo <= kIntMin && info.hi >= kIntMax;
  if (variable == nullptr || !variable->type->isInt() || isOpenRange) {
    return;
  }
  auto &facts = this->state.walk.flow.getFacts();
  facts.add(Term::zero(), Term::of(variable), Facts::saturatingSub(1, info.lo));
  facts.add(Term::of(variable), Term::zero(), Facts::saturatingAdd(info.hi, 1));
}

void LibraryBytesRegistry::checkByteCallbackResult(const CheckByteCallbackResult &task) {
  const auto *found = this->state.flow.callbacks.findByteCallback(task.functionDecl);
  if (found == nullptr || found->target == nullptr) {
    return;
  }
  auto what = ErrorTextByteView::element(found->target->name());
  auto fitting = this->state.flow.proofs.checkValueFits(task.value, found->target, what);
  if (fitting.isErr()) {
    this->state.diagnostics.error(task.value->loc(), fitting.error().message);
  }
}

LibraryBytesRegistry::ResultBytesIndexing
LibraryBytesRegistry::checkBytesIndexing(const Type *type) const {
  if (!type->isTypedArray()) {
    auto message = ErrorTextBytesIndex::notIndexed(type);

    return err(LibraryBytesRegistryBytesIndexingError{
        .code = LibraryBytesRegistryBytesIndexingErrorCode::NOT_INDEXED,
        .message = message,
    });
  }

  return ok(type);
}

std::optional<std::u16string> LibraryBytesRegistry::literalText(const Expr *expr) const {
  auto *conversion = dyn_cast<CastExpr>(expr);
  if (conversion != nullptr && conversion->isImplicit()) {
    expr = conversion->operand;
  }
  auto *literal = dyn_cast<StringLitExpr>(expr);
  if (literal != nullptr) {
    return literal->value;
  }
  auto *templateExpr = dyn_cast<TemplateExpr>(expr);
  if (templateExpr != nullptr && templateExpr->exprs.empty()) {
    return templateExpr->quasis[0];
  }

  return std::nullopt;
}

Expr *LibraryBytesRegistry::bytesMember(MemberExpr *member) {
  auto property = BuiltinPropertyTable::find(member->name);
  switch (member->object->type->bytes()) {
    case Bytes::ArrayBuffer:
      return this->checkArrayBufferMember(member, property);
    case Bytes::DataView:
      return this->checkDataViewMember(member, property);
    case Bytes::TextEncoder:
      return this->checkTextEncoderMember(member, property);
    case Bytes::TextDecoder:
    case Bytes::FatalTextDecoder:
      return this->checkTextDecoderMember(member, property);
    case Bytes::None:
    case Bytes::Uint8:
    case Bytes::Int8:
    case Bytes::Uint8Clamped:
    case Bytes::Int16:
    case Bytes::Uint16:
    case Bytes::Int32:
    case Bytes::Uint32:
    case Bytes::Float16:
    case Bytes::Float32:
    case Bytes::Float64:
    case Bytes::Buffer:
      return this->checkViewMember(member, property);
  }

  return this->reportNoMember(member);
}

Expr *LibraryBytesRegistry::checkArrayBufferMember(MemberExpr *member,
                                                   std::optional<BuiltinProperty> property) {
  if (property == BuiltinProperty::BYTE_LENGTH) {
    member->type = Type::intTy();
    return member;
  }
  if (property == BuiltinProperty::MAX_BYTE_LENGTH) {
    return this->readBuiltin(member, Builtin::BytesByteLength, Type::intTy());
  }
  if (property == BuiltinProperty::RESIZABLE || property == BuiltinProperty::DETACHED) {
    return this->keepObject(member, this->makeBoolLiteral(member->loc(), false));
  }

  return this->reportNoMember(member);
}

Expr *LibraryBytesRegistry::checkDataViewMember(MemberExpr *member,
                                                std::optional<BuiltinProperty> property) {
  if (property == BuiltinProperty::BYTE_LENGTH) {
    member->type = Type::intTy();
    return member;
  }
  if (property == BuiltinProperty::BYTE_OFFSET) {
    return this->readBuiltin(member, Builtin::BytesByteOffset, Type::intTy());
  }
  if (property == BuiltinProperty::BUFFER) {
    const auto *arrayBuffer = Type::bytesType(Bytes::ArrayBuffer);
    return this->readBuiltin(member, Builtin::BytesBuffer, arrayBuffer);
  }

  return this->reportNoMember(member);
}

Expr *LibraryBytesRegistry::checkTextEncoderMember(MemberExpr *member,
                                                   std::optional<BuiltinProperty> property) {
  if (property == BuiltinProperty::ENCODING) {
    auto label = ToUtf16::toUtf16(EncodingNameTable::getUtf8Label());
    auto *literal = this->state.ast.make<StringLitExpr>(member->loc(), label);
    literal->type = Type::string();
    return this->keepObject(member, literal);
  }

  return this->reportNoMember(member);
}

Expr *LibraryBytesRegistry::checkTextDecoderMember(MemberExpr *member,
                                                   std::optional<BuiltinProperty> property) {
  if (property == BuiltinProperty::ENCODING) {
    auto *which = this->makeIntLiteral(member->loc(), 0);
    return this->readBuiltin(member, Builtin::TextDecoderInfo, Type::string(), {which});
  }
  if (property == BuiltinProperty::FATAL || property == BuiltinProperty::IGNORE_BOM) {
    int64_t index = property == BuiltinProperty::FATAL ? 1 : 2;
    auto *which = this->makeIntLiteral(member->loc(), index);
    return this->readBuiltin(member, Builtin::TextDecoderInfo, Type::boolTy(), {which});
  }

  return this->reportNoMember(member);
}

Expr *LibraryBytesRegistry::checkViewMember(MemberExpr *member,
                                            std::optional<BuiltinProperty> property) {
  const Type *type = member->object->type;
  if (property == BuiltinProperty::LENGTH) {
    member->type = Type::intTy();
    return member;
  }
  if (property == BuiltinProperty::BYTE_LENGTH) {
    return this->readBuiltin(member, Builtin::BytesByteLength, Type::intTy());
  }
  if (property == BuiltinProperty::BYTE_OFFSET) {
    return this->readBuiltin(member, Builtin::BytesByteOffset, Type::intTy());
  }
  if (property == BuiltinProperty::BUFFER) {
    const auto *arrayBuffer = Type::bytesType(Bytes::ArrayBuffer);
    return this->readBuiltin(member, Builtin::BytesBuffer, arrayBuffer);
  }
  if (property == BuiltinProperty::BYTES_PER_ELEMENT) {
    auto *size = this->makeIntLiteral(member->loc(), type->elementSize());
    return this->keepObject(member, size);
  }
  bool isDeprecated = type->isBuffer() &&
                      (property == BuiltinProperty::PARENT || property == BuiltinProperty::OFFSET);
  if (isDeprecated) {
    auto message = ErrorTextBytesMember::deprecated(member->name);
    this->state.diagnostics.error(member->loc(), message);
    return member;
  }
  const auto *typedMethod = BytesMethodTable::find(member->name);
  bool isMethod = typedMethod != nullptr && !typedMethod->form.is<NotOnTypedArrayMethod>();
  if (isMethod) {
    auto message = ErrorTextBytesMember::notCalled(member->name, type->name());
    this->state.diagnostics.error(member->loc(), message);
    return member;
  }

  return this->reportNoMember(member);
}

Expr *LibraryBytesRegistry::reportNoMember(MemberExpr *member) {
  auto message = ErrorTextBytesMember::noMember(member->name, member->object->type->name());
  this->state.diagnostics.error(member->loc(), message);

  return member;
}

Expr *LibraryBytesRegistry::readBuiltin(MemberExpr *member, Builtin builtin, const Type *result,
                                        std::vector<Expr *> args) {
  return this->state.library.common.builtinCall(member->loc(), member, builtin, std::move(args),
                                                result);
}

Expr *LibraryBytesRegistry::keepObject(MemberExpr *member, Expr *value) {
  if (isa<NameExpr>(member->object)) {
    return value;
  }
  std::vector<Expr *> parts{member->object, value};
  auto *sequence = this->state.ast.make<SequenceExpr>(member->loc(), parts);
  sequence->type = value->type;

  return sequence;
}

IntLitExpr *LibraryBytesRegistry::makeIntLiteral(llvm::SMLoc loc, int64_t value) const {
  auto *literal = this->state.ast.make<IntLitExpr>(loc, uint64_t(value));
  literal->type = Type::intTy();

  return literal;
}

BoolLitExpr *LibraryBytesRegistry::makeBoolLiteral(llvm::SMLoc loc, bool value) const {
  auto *literal = this->state.ast.make<BoolLitExpr>(loc, value);
  literal->type = Type::boolTy();

  return literal;
}

Expr *LibraryBytesRegistry::bytesUnionMember(MemberExpr *member) {
  const Type *type = member->object->type;
  llvm::StringRef name = member->name;
  auto property = BuiltinPropertyTable::find(name);
  static const BuiltinProperty others[] = {
      BuiltinProperty::BYTES_PER_ELEMENT, BuiltinProperty::MAX_BYTE_LENGTH,
      BuiltinProperty::RESIZABLE,         BuiltinProperty::DETACHED,
      BuiltinProperty::ENCODING,          BuiltinProperty::FATAL,
      BuiltinProperty::IGNORE_BOM};
  if (property.has_value() && llvm::is_contained(others, *property)) {
    auto message = ErrorTextBytesMember::narrow(name, type->name());
    this->state.diagnostics.error(member->loc(), message);
    return member;
  }
  llvm::ArrayRef<const Type *> members = type->members();
  auto has = std::bind_front(&LibraryBytesRegistry::hasUnionProperty, this, property);
  const Type *const *lacking = llvm::find_if_not(members, has);
  if (lacking != members.end()) {
    const Type *const *with = llvm::find_if(members, has);
    llvm::StringRef withName = with != members.end() ? (*with)->name() : llvm::StringRef();
    auto message = ErrorTextBytesMember::lacking(name, (*lacking)->name(), type->name(), withName);
    this->state.diagnostics.error(member->loc(), message);
    return member;
  }
  if (property == BuiltinProperty::LENGTH) {
    member->type = Type::intTy();
    return member;
  }
  if (property == BuiltinProperty::BUFFER) {
    return this->state.library.common.builtinCall(member->loc(), member, Builtin::BytesBuffer, {},
                                                  Type::bytesType(Bytes::ArrayBuffer));
  }

  return this->state.library.common.builtinCall(member->loc(), member,
                                                property == BuiltinProperty::BYTE_LENGTH
                                                    ? Builtin::BytesByteLength
                                                    : Builtin::BytesByteOffset,
                                                {}, Type::intTy());
}

Expr *LibraryBytesRegistry::bytesStaticMember(MemberExpr *member) {
  auto *object = dyn_cast<NameExpr>(member->object);
  if (object == nullptr || !this->state.isUnshadowedGlobal(object)) {
    return nullptr;
  }
  const auto *type = this->state.library.bytes.bytesTypeNamed(object->name);
  if (type == nullptr) {
    return nullptr;
  }
  member->type = Type::error();
  auto property = BuiltinPropertyTable::find(member->name);
  if (type->isTypedArray() && property == BuiltinProperty::BYTES_PER_ELEMENT) {
    auto *literal = this->state.ast.make<IntLitExpr>(member->loc(), type->elementSize());
    literal->type = Type::intTy();
    return literal;
  }
  if (type->isBuffer() && property == BuiltinProperty::POOL_SIZE) {
    auto *literal = this->state.ast.make<IntLitExpr>(member->loc(), 8192);
    literal->type = Type::intTy();
    return literal;
  }
  auto message = ErrorTextBytesMember::callOnly(object->name, member->name);
  this->state.diagnostics.error(member->loc(), message);

  return member;
}

Expr *LibraryBytesRegistry::bytesText(Expr *expr) {
  const Type *type = expr->type;
  if (type->isTypedArray()) {
    auto toStringName = BytesMethodTable::getName(BytesMethod::TO_STRING);
    auto *callee = this->state.ast.make<MemberExpr>(expr->loc(), expr, toStringName);
    if (type->isBuffer()) {
      auto *encoding = this->state.ast.make<IntLitExpr>(expr->loc(), Utf8);
      encoding->type = Type::intTy();
      return this->state.library.common.builtinCall(expr->loc(), callee, Builtin::BufferToString,
                                                    {encoding}, Type::string());
    }
    return this->state.library.common.builtinCall(expr->loc(), callee, Builtin::BytesJoin, {},
                                                  Type::string());
  }
  auto message = ErrorTextBytesString::noText(type->name(), type->bytes() == Bytes::ArrayBuffer);
  this->state.diagnostics.error(expr->loc(), message);

  return expr;
}

bool LibraryBytesRegistry::hasUnionProperty(std::optional<BuiltinProperty> property,
                                            const Type *memberType) const {
  if (property == BuiltinProperty::BYTE_LENGTH) {
    return memberType->isByteView() || memberType->bytes() == Bytes::ArrayBuffer;
  }
  if (property == BuiltinProperty::BYTE_OFFSET || property == BuiltinProperty::BUFFER) {
    return memberType->isByteView();
  }

  return property == BuiltinProperty::LENGTH && memberType->isTypedArray();
}

LibraryBytesRegistry::ResultCountOfBytes
LibraryBytesRegistry::checkCountOfBytes(std::optional<int64_t> count, llvm::StringRef name) const {
  if (!count || *count < 1 || *count > 6) {
    auto message = ErrorTextBuffer::byteCount(name);

    return err(LibraryBytesRegistryCountOfBytesError{
        .code = LibraryBytesRegistryCountOfBytesErrorCode::NOT_ONE_TO_SIX,
        .message = message,
    });
  }

  return ok(*count);
}

LibraryBytesRegistry::ResultDecodedInput
LibraryBytesRegistry::checkDecodedInput(const Type *type) const {
  if (!type->isError() && !type->isUndefined() && !type->isByteView() &&
      type->bytes() != Bytes::ArrayBuffer) {
    auto message = ErrorTextBuffer::decode(type->name());

    return err(LibraryBytesRegistryDecodedInputError{
        .code = LibraryBytesRegistryDecodedInputErrorCode::NOT_BYTES,
        .message = message,
    });
  }

  return ok(type);
}

LibraryBytesRegistry::ResultVarValueFits
LibraryBytesRegistry::checkVarValueFits(const Expr *value, int64_t width, bool isSigned) {
  int64_t bits = width * 8;
  int64_t low = isSigned ? -(int64_t(1) << (bits - 1)) : 0;
  int64_t high = isSigned ? (int64_t(1) << (bits - 1)) - 1 : (int64_t(1) << bits) - 1;
  bool isUnproven =
      !value->type->isError() && !this->state.flow.proofs.canProveFits(value, low, high);
  if (isUnproven) {
    auto range = ErrorTextBytes::range(low, high);
    auto message = ErrorTextBuffer::varFits(width, range);

    return err(LibraryBytesRegistryVarValueFitsError{
        .code = LibraryBytesRegistryVarValueFitsErrorCode::VALUE_MAY_NOT_FIT,
        .message = message,
    });
  }

  return ok(value);
}

LibraryBytesRegistry::ResultSourceElement
LibraryBytesRegistry::checkSourceElement(const Type *from, llvm::StringRef what) const {
  const auto *element = this->state.code.iteration.iteratedType(from);
  if (element == nullptr || from->isString()) {
    auto message = ErrorTextBytesStatic::source(what, from->name());

    return err(LibraryBytesRegistrySourceElementError{
        .code = LibraryBytesRegistrySourceElementErrorCode::NOT_ITERABLE,
        .message = message,
    });
  }

  return ok(element);
}

LibraryBytesRegistry::ResultConcatSources
LibraryBytesRegistry::checkConcatSources(const Type *list, const Type *buffer) const {
  const auto *uint8 = Type::bytesType(Bytes::Uint8);
  if (!list->isArray() || (list->inner() != uint8 && list->inner() != buffer)) {
    auto message = ErrorTextBufferStatic::concat(list->name());

    return err(LibraryBytesRegistryConcatSourcesError{
        .code = LibraryBytesRegistryConcatSourcesErrorCode::NOT_BYTES_LIST,
        .message = message,
    });
  }

  return ok(list);
}

Signature LibraryBytesRegistry::mapperSignature(const Type *element, const Type *type) const {
  Signature signature;
  signature.pad = false;
  signature.params = {element, Type::intTy()};
  signature.ret = type->elementType();

  return signature;
}

void LibraryBytesRegistry::rememberByteCallback(const Expr *arg, const Type *from,
                                                const Type *type) {
  auto *function = dyn_cast<FunctionExpr>(arg);
  if (function != nullptr) {
    ByteCallback info;
    if (!from->elementRange(info.lo, info.hi)) {
      info.lo = kIntMin;
      info.hi = kIntMax;
    }
    info.target = type;
    this->state.flow.callbacks.rememberByteCallback(function->fn, info);
  }
}

LibraryBytesRegistry::ResultMapResultFits
LibraryBytesRegistry::checkMapResultFits(const Type *type, bool arrow) const {
  int64_t low = 0;
  int64_t high = 0;
  bool isBounded = !arrow && type->elementRange(low, high) && type->bytes() != Bytes::Uint8Clamped;
  if (isBounded) {
    auto elementWhat = ErrorTextByteView::element(type->name());
    auto range = ErrorTextBytes::range(low, high);
    auto hint = BytesCheckHelpers::wrapHint(type->bytes(), "f(x)");
    auto message = ErrorTextTypedArray::mapFits(elementWhat, range, hint);

    return err(LibraryBytesRegistryMapResultFitsError{
        .code = LibraryBytesRegistryMapResultFitsErrorCode::RESULT_MAY_NOT_FIT,
        .message = message,
    });
  }

  return ok(type);
}

CallExpr *LibraryBytesRegistry::makeArrayFrom(CallExpr *call, Expr *source, const Type *type) {
  auto arrayGlobal = GlobalNameTable::getName(GlobalName::ARRAY);
  auto fromName = GlobalMemberTable::getName(GlobalMember::ARRAY_FROM);
  auto *arrayName = this->state.ast.make<NameExpr>(call->loc(), arrayGlobal);
  auto *callee = this->state.ast.make<MemberExpr>(call->loc(), arrayName, fromName);
  std::vector<Expr *> args{source, call->args[1]};
  auto *arrayFrom = this->state.ast.make<CallExpr>(call->loc(), callee, args);
  arrayFrom->target = BuiltinCall{.builtin = Builtin::ArrayFrom};
  arrayFrom->type = Type::array(type->elementType());

  return arrayFrom;
}

Expr *LibraryBytesRegistry::makeBufferTest(CallExpr *call, MemberExpr *member) {
  auto bufferName = GlobalNameTable::getName(GlobalName::BUFFER);
  auto *rhs = this->state.ast.make<NameExpr>(member->loc(), bufferName);
  rhs->type = Type::bytesType(Bytes::Buffer);
  auto *test =
      this->state.ast.make<BinaryExpr>(call->loc(), BinaryOp::InstanceOf, call->args[0], rhs);
  test->type = Type::boolTy();

  return test;
}

IntLitExpr *LibraryBytesRegistry::makeUtf8Literal(llvm::SMLoc loc) const {
  auto *literal = this->state.ast.make<IntLitExpr>(loc, Utf8);
  literal->type = Type::intTy();

  return literal;
}

const VarSymbol *LibraryBytesRegistry::viewOwner(const Expr *expr, BuiltinProperty field,
                                                 const Type **type) const {
  auto *call = dyn_cast<CallExpr>(expr);
  if (call != nullptr &&
      (call->builtin() == Builtin::BytesBuffer || call->builtin() == Builtin::BytesByteOffset ||
       call->builtin() == Builtin::BytesByteLength)) {
    expr = call->callee;
  }
  auto *member = dyn_cast<MemberExpr>(expr);
  if (member == nullptr || !BuiltinPropertyTable::isName(member->name, field) ||
      member->object->type == nullptr || !member->object->type->isByteView()) {
    return nullptr;
  }
  const Expr *object = member->object;
  while (auto *conversion = dyn_cast<CastExpr>(object)) {
    if (!conversion->isImplicit()) {
      return nullptr;
    }
    object = conversion->operand;
  }
  auto *name = dyn_cast<NameExpr>(object);
  if (type != nullptr) {
    *type = member->object->type;
  }

  return name != nullptr ? name->var() : nullptr;
}

const Expr *LibraryBytesRegistry::withoutCasts(const Expr *expr) const {
  expr = ExprParts::withoutImplicitCasts(expr);

  return expr;
}

bool LibraryBytesRegistry::isSameView(const Expr *buffer, const Expr *offset, const Expr *length,
                                      int64_t size) const {
  const Type *ownerType = nullptr;
  const auto *owner = this->viewOwner(buffer, BuiltinProperty::BUFFER, &ownerType);
  if (owner == nullptr || offset == nullptr ||
      this->viewOwner(this->withoutCasts(offset), BuiltinProperty::BYTE_OFFSET) != owner) {
    return false;
  }
  if (length == nullptr) {
    return size == 1;
  }
  length = this->withoutCasts(length);
  if (this->viewOwner(length, BuiltinProperty::BYTE_LENGTH) == owner) {
    return size == 1;
  }

  return this->viewOwner(length, BuiltinProperty::LENGTH) == owner &&
         ownerType->elementSize() == uint64_t(size);
}

LibraryBytesRegistry::ResultOffsetAligned
LibraryBytesRegistry::checkOffsetAligned(std::optional<int64_t> start, int64_t size,
                                         llvm::StringRef what) const {
  if (size > 1 && (!start || *start % size != 0)) {
    auto message = ErrorTextByteView::offset(what, size);

    return err(LibraryBytesRegistryOffsetAlignedError{
        .code = LibraryBytesRegistryOffsetAlignedErrorCode::MISALIGNED_OFFSET,
        .message = message,
    });
  }

  return ok(start.value_or(0));
}

LibraryBytesRegistry::ResultBufferAligned
LibraryBytesRegistry::checkBufferAligned(std::optional<int64_t> whole, int64_t size,
                                         llvm::StringRef what) const {
  if (size > 1 && (!whole || *whole % size != 0)) {
    auto message = ErrorTextByteView::size(what, size);

    return err(LibraryBytesRegistryBufferAlignedError{
        .code = LibraryBytesRegistryBufferAlignedErrorCode::SIZE_NOT_PROVEN,
        .message = message,
    });
  }

  return ok(whole.value_or(0));
}

bool LibraryBytesRegistry::canBeFatal(const Expr *options) const {
  auto answers = std::vector<bool>();
  for (const Expr *node :
       ChildrenFirst::order(options, this, &LibraryBytesRegistry::spreadOptions)) {
    bool isFatal = this->canNodeBeFatal(node, answers);
    answers.push_back(isFatal);
  }

  return answers.back();
}

NodeChildren<const Expr *> LibraryBytesRegistry::spreadOptions(const Expr *options) const {
  auto *literal = dyn_cast<ObjectLitExpr>(options);
  if (options->type->isError() || literal == nullptr) {
    return {};
  }
  NodeChildren<const Expr *> spreads;
  for (const ObjectLitExpr::Property &property : literal->props) {
    if (property.spread()) {
      spreads.push_back(property.value);
    }
  }

  return spreads;
}

bool LibraryBytesRegistry::canNodeBeFatal(const Expr *options, std::vector<bool> &answers) const {
  if (options->type->isError()) {
    return false;
  }
  auto *literal = dyn_cast<ObjectLitExpr>(options);
  if (literal == nullptr) {
    auto fatal = BytesShapeFieldTable::getName(BytesShapeField::DECODER_FATAL);
    const auto *field = options->type->field(fatal);
    return field != nullptr && !field->type->isUndefined();
  }
  size_t spreadCount = this->spreadOptions(options).size();
  std::vector<bool> spreads = ChildrenFirst::takeAnswers(answers, spreadCount);
  size_t nextSpread = 0;
  bool isFatal = false;
  for (const ObjectLitExpr::Property &property : literal->props) {
    if (property.spread()) {
      isFatal = isFatal || spreads[nextSpread];
      nextSpread += 1;
    } else if (BytesShapeFieldTable::find(property.name) == BytesShapeField::DECODER_FATAL) {
      isFatal = this->isFatalFlag(property.value);
    }
  }

  return isFatal;
}

bool LibraryBytesRegistry::isFatalFlag(const Expr *value) const {
  const auto *plain = ExprParts::withoutImplicitCasts(value);
  const auto *flag = dyn_cast<BoolLitExpr>(plain);

  return !isa<UndefinedExpr>(plain) && !(flag != nullptr && !flag->value);
}

LibraryBytesRegistry::ResultDecoderLabel
LibraryBytesRegistry::checkDecoderLabel(llvm::StringRef label) const {
  std::string lower = label.trim(" \t\n\f\r").lower();
  const char *found = tz_encoding_name(lower.c_str());
  if (found == nullptr) {
    auto message = ErrorTextTextDecoder::unknown(label);

    return err(LibraryBytesRegistryDecoderLabelError{
        .code = LibraryBytesRegistryDecoderLabelErrorCode::UNKNOWN_LABEL,
        .message = message,
    });
  }

  return ok(found);
}

LibraryBytesRegistry::ResultViewedBuffer
LibraryBytesRegistry::checkViewedBuffer(const Type *buffer) const {
  if (buffer->bytes() != Bytes::ArrayBuffer && !buffer->isError()) {
    auto message = ErrorTextBytesNew::viewOver(buffer->name(), buffer->isTypedArray());

    return err(LibraryBytesRegistryViewedBufferError{
        .code = LibraryBytesRegistryViewedBufferErrorCode::NOT_A_BUFFER,
        .message = message,
    });
  }

  return ok(buffer);
}

const Type *LibraryBytesRegistry::decoderOptionsType() const {
  auto fatal = BytesShapeFieldTable::getName(BytesShapeField::DECODER_FATAL);
  auto ignoreBom = BytesShapeFieldTable::getName(BytesShapeField::DECODER_IGNORE_BOM);
  const auto *flag = Type::optional(Type::boolTy());

  return Type::object({{fatal.str(), flag, false}, {ignoreBom.str(), flag, false}});
}

std::optional<std::u16string> LibraryBytesRegistry::knownDecoderName(const Expr *label,
                                                                     const std::u16string &text) {
  auto written = ToUtf8::toUtf8(text);
  auto decoded = this->checkDecoderLabel(written);
  if (decoded.isErr()) {
    this->state.diagnostics.error(label->loc(), decoded.error().message);
    return std::nullopt;
  }
  const char *found = decoded.value();

  return std::u16string(found, found + strlen(found));
}

bool LibraryBytesRegistry::matchDecoderLabels(const Expr *checked, llvm::SMLoc at) {
  bool isSkipped = this->state.library.decoder.isInsideDecoderParse() || checked->type->isError() ||
                   this->state.flow.proofs.isExempt(at);
  if (isSkipped) {
    return true;
  }
  auto labels = this->state.library.common.knownTexts(checked);
  if (!labels.has_value()) {
    auto message = ErrorTextTextDecoder::notKnown();
    this->state.diagnostics.error(at, message);
    return false;
  }
  for (const std::string &each : *labels) {
    auto decoded = this->checkDecoderLabel(each);
    if (decoded.isErr()) {
      this->state.diagnostics.error(at, decoded.error().message);
      return false;
    }
  }

  return true;
}

Expr *LibraryBytesRegistry::decoderNameLiteral(llvm::SMLoc loc, const std::u16string &name) {
  auto *literal = this->state.ast.make<StringLitExpr>(loc, name);
  literal->type = Type::string();

  return literal;
}

bool LibraryBytesRegistry::matchBytesSource(Expr *source, const Type *target) {
  const Type *from = source->type;
  if (from->isError()) {
    return false;
  }
  const auto *element = this->sourceElement(from);
  if (element == nullptr) {
    auto message = ErrorTextBytesSource::notSource(target->name(), from->name(), from->isString());
    this->state.diagnostics.error(source->loc(), message);
    return false;
  }
  if (from->isIterator()) {
    auto what = ErrorTextBytesSource::quoted(target->name());
    auto nothing = this->state.code.iteration.checkSendsNothing(from, what);
    if (nothing.isErr()) {
      this->state.diagnostics.error(source->loc(), nothing.error().message);
    }
  }
  if (!element->isNumeric()) {
    auto message = ErrorTextBytesSource::notNumbers(from->name(), target->name());
    this->state.diagnostics.error(source->loc(), message);
    return false;
  }
  int64_t low = 0;
  int64_t high = 0;
  if (!target->elementRange(low, high)) {
    return true;
  }
  Bytes kind = target->bytes();
  auto text = BytesCheckHelpers::valueText(source);
  auto wrap = BytesCheckHelpers::wrapHint(kind, kValuePlaceholder.str());
  auto hint = ErrorTextBytesSource::hint(target->name(), text, wrap);
  if (element->isFloat()) {
    auto message = ErrorTextBytesSource::doubles(from->name(), target->name(), text);
    this->state.diagnostics.error(source->loc(), message);
    return false;
  }
  if (kind == Bytes::Uint8Clamped) {
    return true;
  }
  int64_t sourceLow = 0;
  int64_t sourceHigh = 0;
  if (from->isTypedArray() && from->elementRange(sourceLow, sourceHigh)) {
    if (sourceLow >= low && sourceHigh <= high) {
      return true;
    }
    auto fromRange = ErrorTextBytes::range(sourceLow, sourceHigh);
    auto range = ErrorTextBytes::range(low, high);
    auto message =
        ErrorTextBytesSource::mayNotFit(from->name(), fromRange, target->name(), range, hint);
    this->state.diagnostics.error(source->loc(), message);
    return false;
  }
  auto range = ErrorTextBytes::range(low, high);
  auto message = ErrorTextBytesSource::unproven(from->name(), target->name(), range, hint);
  this->state.diagnostics.error(source->loc(), message);

  return false;
}

const Type *LibraryBytesRegistry::sourceElement(const Type *from) const {
  if (from->isTypedArray()) {
    return from->elementType();
  }
  if (from->isArray()) {
    return from->inner();
  }
  if (from->isIterator()) {
    return from->yielded();
  }

  return nullptr;
}

bool LibraryBytesRegistry::isSpreadElement(const ArrayLitExpr::Element &element) const {
  return element.spread;
}

LibraryBytesRegistry::ResultSetSourceOk
LibraryBytesRegistry::checkSetSource(const Expr *source) const {
  if (source->type->isIterator()) {
    auto message = ErrorTextTypedArray::iterator();

    return err(LibraryBytesRegistrySetSourceError{
        .code = LibraryBytesRegistrySetSourceErrorCode::ITERATOR_SOURCE,
        .message = message,
    });
  }

  return ok(source);
}

LibraryBytesRegistry::ResultMapFitsType
LibraryBytesRegistry::checkTypedMapFits(const TypedArrayDraft &typed) const {
  const Expr *callback = typed.call->args[0];
  int64_t low = 0;
  int64_t high = 0;
  bool isBounded = callback->type->isFunction() && typed.arrayType->elementRange(low, high) &&
                   typed.arrayType->bytes() != Bytes::Uint8Clamped;
  if (isBounded) {
    auto range = ErrorTextBytes::range(low, high);
    auto hint = BytesCheckHelpers::wrapHint(typed.arrayType->bytes(), "f(x)");
    auto message = ErrorTextTypedArray::mapFits(typed.what, range, hint);

    return err(LibraryBytesRegistryTypedMapFitsError{
        .code = LibraryBytesRegistryTypedMapFitsErrorCode::RESULT_MAY_NOT_FIT,
        .message = message,
    });
  }

  return ok(callback);
}

LibraryBytesRegistry::ResultIndexProven
LibraryBytesRegistry::checkWithIndex(const Expr *array, const Expr *index) const {
  bool isUnproven = !index->type->isError() &&
                    !this->state.flow.proofs.canProveAtInBounds(array, index) &&
                    !this->state.flow.proofs.isExempt(index->loc(), array);
  if (isUnproven) {
    auto arrayText = BytesCheckHelpers::valueText(array);
    auto message = ErrorTextTypedArray::index(arrayText);

    return err(LibraryBytesRegistryWithIndexError{
        .code = LibraryBytesRegistryWithIndexErrorCode::INDEX_NOT_PROVEN,
        .message = message,
    });
  }

  return ok(index);
}

CallExpr *LibraryBytesRegistry::makeTypedArrayFrom(const TypedArrayDraft &typed) {
  MemberExpr *member = typed.member;
  auto arrayGlobal = GlobalNameTable::getName(GlobalName::ARRAY);
  auto fromName = GlobalMemberTable::getName(GlobalMember::ARRAY_FROM);
  auto *arrayName = this->state.ast.make<NameExpr>(member->loc(), arrayGlobal);
  auto *callee = this->state.ast.make<MemberExpr>(member->loc(), arrayName, fromName);
  auto *from =
      this->state.ast.make<CallExpr>(member->loc(), callee, std::vector<Expr *>{member->object});
  from->target = BuiltinCall{.builtin = Builtin::ArrayFrom};
  from->type = Type::array(typed.element);

  return from;
}

}
