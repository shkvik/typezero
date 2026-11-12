#include "compiler/semantic/library/bytes/buffer_checker/buffer_checker.h"
#include "compiler/program/library/bytes/buffer_method_table/buffer_method_table.h"
#include "compiler/program/library/bytes/bytes_method_table/bytes_method_table.h"
#include "compiler/program/library/bytes/bytes_shape_field_table/bytes_shape_field_table.h"
#include "compiler/semantic/bodies/expr/argument_checker/argument_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/bodies/expr/member_checker/member_checker.h"
#include "compiler/semantic/declare/constant_folder/constant_folder.h"
#include "compiler/semantic/declare/prelude/prelude.h"
#include "compiler/semantic/flow/index_proofs/index_proofs.h"
#include "compiler/semantic/library/bytes/byte_proofs/byte_proofs.h"
#include "compiler/semantic/library/bytes/byte_proofs/byte_proofs_errors.h"
#include "compiler/semantic/library/bytes/bytes_check_helpers/bytes_check_helpers.h"
#include "compiler/semantic/library/bytes/bytes_checker/bytes_checker.h"
#include "compiler/semantic/library/decimal/decimal_checker/decimal_checker.h"
#include "compiler/semantic/library/decimal/decimal_checker/decimal_checker_errors.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "runtime/library/bytes/encodings/encodings.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/MathExtras.h"

using llvm::dyn_cast;

namespace tz {

BufferChecker::BufferChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->prelude = &all.prelude;
  this->byteProofs = &all.byteProofs;
  this->exprChecker = &all.exprChecker;
  this->indexProofs = &all.indexProofs;
  this->bytesChecker = &all.bytesChecker;
  this->memberChecker = &all.memberChecker;
  this->typeRelations = &all.typeRelations;
  this->constantFolder = &all.constantFolder;
  this->argumentChecker = &all.argumentChecker;
}

std::optional<std::u16string> BufferChecker::literalText(const Expr *expr) const {
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

BufferChecker::ResultCountOfBytes BufferChecker::checkCountOfBytes(std::optional<int64_t> count,
                                                                   llvm::StringRef name) const {
  if (!count || *count < 1 || *count > 6) {
    auto message = ErrorTextBuffer::byteCount(name);

    return err(BufferCheckerCountOfBytesError{
        .code = BufferCheckerCountOfBytesErrorCode::NOT_ONE_TO_SIX,
        .message = message,
    });
  }

  return ok(*count);
}

BufferChecker::ResultDecodedInput BufferChecker::checkDecodedInput(const Type *type) const {
  if (!type->isError() && !type->isUndefined() && !type->isByteView() &&
      type->bytes() != Bytes::ArrayBuffer) {
    auto message = ErrorTextBuffer::decode(type->name());

    return err(BufferCheckerDecodedInputError{
        .code = BufferCheckerDecodedInputErrorCode::NOT_BYTES,
        .message = message,
    });
  }

  return ok(type);
}

Expr *BufferChecker::checkBufferMethod(CallExpr *call, MemberExpr *member) {
  BufferCall buffer{
      .call = call,
      .member = member,
      .bufferType = member->object->type,
      .name = member->name,
  };
  auto method = BytesMethodTable::findMethod(member->name);
  const BufferMethodInfo *found = method.has_value() ? BufferMethodTable::find(*method) : nullptr;
  if (found == nullptr) {
    return this->checkNumberMethod(buffer);
  }

  const BufferMethodForm &form = found->form;
  switch (form.kind()) {
    case BufferMethodKind::SLICE:
      return this->reportSlice(buffer);
    case BufferMethodKind::TO_STRING:
      return this->checkToString(buffer);
    case BufferMethodKind::TO_JSON:
      return this->checkToJson(buffer);
    case BufferMethodKind::EQUALS:
      return this->checkEquals(buffer);
    case BufferMethodKind::COMPARE:
      return this->checkCompare(buffer);
    case BufferMethodKind::COPY:
      return this->checkCopy(buffer);
    case BufferMethodKind::WRITE:
      return this->checkWrite(buffer);
    case BufferMethodKind::FILL:
      return this->checkFill(buffer);
    case BufferMethodKind::SEARCH:
      return this->checkSearch(buffer, form.as<BufferSearchMethod>()->mode);
    case BufferMethodKind::SWAP:
      return this->checkSwap(buffer, form.as<BufferSwapMethod>()->width);
  }

  return call;
}

Expr *BufferChecker::toBuiltinCall(CallExpr *call, Builtin builtin, const Type *result) const {
  call->target = BuiltinCall{.builtin = builtin};
  call->type = result;

  return call;
}

Expr *BufferChecker::reportSlice(const BufferCall &buffer) {
  auto message = ErrorTextBuffer::slice();
  this->state.getDiagnostics().error(buffer.member->loc(), message);
  this->argumentChecker->walkArgsLoosely(buffer.call);

  return buffer.call;
}

Expr *BufferChecker::checkToString(const BufferCall &buffer) {
  CallExpr *call = buffer.call;
  auto arity = this->argumentChecker->checkArity(call, 0, 3);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(buffer.member->loc(), arity.error().message);
    return call;
  }
  Expr *encoding = call->args.empty() ? this->makeIntLiteral(call->loc(), Utf8)
                                      : this->bytesChecker->checkBufferEncoding(call->args[0]);
  std::vector<Expr *> args{encoding};
  for (size_t index = 1; index < call->args.size(); ++index) {
    args.push_back(this->memberChecker->checkPosition(call->args[index]));
  }
  call->args = std::move(args);

  return this->toBuiltinCall(call, Builtin::BufferToString, Type::string());
}

Expr *BufferChecker::checkToJson(const BufferCall &buffer) {
  CallExpr *call = buffer.call;
  auto arity = this->argumentChecker->checkArity(call, 0, 0);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(buffer.member->loc(), arity.error().message);
    return call;
  }
  auto typeName = BytesShapeFieldTable::getName(BytesShapeField::JSON_TYPE);
  auto dataName = BytesShapeFieldTable::getName(BytesShapeField::JSON_DATA);
  const auto *json = Type::object({{typeName.str(), Type::string(), false},
                                   {dataName.str(), Type::array(Type::intTy()), false}});

  return this->toBuiltinCall(call, Builtin::BufferToJSON, json);
}

Expr *BufferChecker::checkEquals(const BufferCall &buffer) {
  CallExpr *call = buffer.call;
  auto arity = this->argumentChecker->checkArity(call, 1, 1);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(buffer.member->loc(), arity.error().message);
    return call;
  }
  call->args[0] = this->checkBytesArg(call->args[0]);

  return this->toBuiltinCall(call, Builtin::BufferEquals, Type::boolTy());
}

Expr *BufferChecker::checkCompare(const BufferCall &buffer) {
  CallExpr *call = buffer.call;
  auto arity = this->argumentChecker->checkArity(call, 1, 5);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(buffer.member->loc(), arity.error().message);
    return call;
  }
  call->args[0] = this->checkBytesArg(call->args[0]);
  this->walkPositionArgs(call, 1);
  for (size_t index = 1; index < call->args.size(); ++index) {
    auto name = ErrorTextBufferLabel::comparePosition(index);
    auto what = ErrorTextBuffer::position(name);
    bool isEnd = index == 2 || index == 4;
    if (isEnd) {
      const Expr *array = index == 2 ? call->args[0] : buffer.member->object;
      auto upTo = this->indexProofs->requireUpTo(call->args[index], array, 0, what);
      this->reportUnprovenUpTo(upTo, call->args[index]->loc());
    } else {
      auto between = this->indexProofs->requireBetween(call->args[index], 0, std::nullopt, what);
      this->reportUnprovenBetween(between, call->args[index]->loc());
    }
  }

  return this->toBuiltinCall(call, Builtin::BufferCompareTo, Type::intTy());
}

Expr *BufferChecker::checkCopy(const BufferCall &buffer) {
  CallExpr *call = buffer.call;
  auto arity = this->argumentChecker->checkArity(call, 1, 4);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(buffer.member->loc(), arity.error().message);
    return call;
  }
  call->args[0] = this->checkBytesArg(call->args[0]);
  this->walkPositionArgs(call, 1);
  for (size_t index = 1; index < call->args.size(); ++index) {
    auto name = ErrorTextBufferLabel::copyPosition(index);
    auto what = ErrorTextBuffer::position(name);
    if (index == 2) {
      auto upTo = this->indexProofs->requireUpTo(call->args[index], buffer.member->object, 0, what);
      this->reportUnprovenUpTo(upTo, call->args[index]->loc());
    } else {
      auto between = this->indexProofs->requireBetween(call->args[index], 0, std::nullopt, what);
      this->reportUnprovenBetween(between, call->args[index]->loc());
    }
  }

  return this->toBuiltinCall(call, Builtin::BufferCopy, Type::intTy());
}

Expr *BufferChecker::checkWrite(const BufferCall &buffer) {
  CallExpr *call = buffer.call;
  auto arity = this->argumentChecker->checkArity(call, 1, 4);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(buffer.member->loc(), arity.error().message);
    return call;
  }
  auto *text = this->argumentChecker->checkArg(call->args[0], Type::string());
  auto rest = this->splitWriteArgs(buffer);
  if (!rest.positions.empty()) {
    auto offsetWhat = ErrorTextBufferLabel::offset();
    auto upTo =
        this->indexProofs->requireUpTo(rest.positions[0], buffer.member->object, 0, offsetWhat);
    this->reportUnprovenUpTo(upTo, rest.positions[0]->loc());
    if (upTo.isOk() && rest.positions.size() > 1) {
      this->walkWriteLength(buffer.member->object, rest.positions[0], rest.positions[1]);
    }
  }
  Expr *encoding =
      rest.encoding != nullptr ? rest.encoding : this->makeIntLiteral(call->loc(), Utf8);
  std::vector<Expr *> args{text, encoding};
  args.insert(args.end(), rest.positions.begin(), rest.positions.end());
  call->args = std::move(args);

  return this->toBuiltinCall(call, Builtin::BufferWrite, Type::intTy());
}

BufferChecker::EncodingAndPositions BufferChecker::splitWriteArgs(const BufferCall &buffer) {
  CallExpr *call = buffer.call;
  EncodingAndPositions rest;
  for (size_t index = 1; index < call->args.size(); ++index) {
    Expr *arg = call->args[index];
    bool isLast = index + 1 == call->args.size();
    bool isMayBeEncoding = this->literalText(arg).has_value() || isLast;
    if (!isMayBeEncoding) {
      rest.positions.push_back(this->memberChecker->checkPosition(arg));
      continue;
    }
    auto *checked = this->exprChecker->checkExpr(arg);
    if (checked->type->isString()) {
      rest.encoding = this->bytesChecker->checkBufferEncoding(checked);
      if (!isLast) {
        auto message = ErrorTextBuffer::encoding(buffer.name);
        this->state.getDiagnostics().error(call->args[index + 1]->loc(), message);
      }
      break;
    }
    rest.positions.push_back(this->memberChecker->checkPosition(checked));
  }

  return rest;
}

Expr *BufferChecker::checkFill(const BufferCall &buffer) {
  CallExpr *call = buffer.call;
  auto arity = this->argumentChecker->checkArity(call, 1, 4);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(buffer.member->loc(), arity.error().message);
    return call;
  }
  auto *value = this->checkValueArg(buffer, call->args[0]);
  if (value->type->isError()) {
    return call;
  }
  if (value->type->isInt()) {
    auto byte = ErrorTextBufferLabel::byte();
    auto fitting = this->byteProofs->checkValueFits(value, buffer.bufferType, byte);
    if (fitting.isErr()) {
      this->state.getDiagnostics().error(value->loc(), fitting.error().message);
    }
  }
  auto rest = this->splitValueArgs(buffer);
  this->reportEncodingWithoutText(value, rest.encoding);
  for (size_t index = 0; index < rest.positions.size(); ++index) {
    std::string what = index == 0 ? ErrorTextBufferLabel::offset() : ErrorTextBufferLabel::end();
    auto upTo =
        this->indexProofs->requireUpTo(rest.positions[index], buffer.member->object, 0, what);
    this->reportUnprovenUpTo(upTo, rest.positions[index]->loc());
  }
  Expr *encoding =
      rest.encoding != nullptr ? rest.encoding : this->makeIntLiteral(call->loc(), Utf8);
  std::vector<Expr *> args{value, encoding};
  args.insert(args.end(), rest.positions.begin(), rest.positions.end());
  call->args = std::move(args);

  return this->toBuiltinCall(call, Builtin::BufferFill, buffer.bufferType);
}

Expr *BufferChecker::checkSearch(const BufferCall &buffer, BytesSearchMode mode) {
  CallExpr *call = buffer.call;
  auto arity = this->argumentChecker->checkArity(call, 1, 3);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(buffer.member->loc(), arity.error().message);
    return call;
  }
  auto *value = this->checkValueArg(buffer, call->args[0]);
  if (value->type->isError()) {
    return call;
  }
  auto rest = this->splitValueArgs(buffer);
  this->reportEncodingWithoutText(value, rest.encoding);

  int runtimeMode = 0;
  const auto *answer = Type::intTy();
  switch (mode) {
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
  Expr *encoding =
      rest.encoding != nullptr ? rest.encoding : this->makeIntLiteral(call->loc(), Utf8);
  std::vector<Expr *> args{modeArg, value, encoding};
  args.insert(args.end(), rest.positions.begin(), rest.positions.end());
  call->args = std::move(args);

  return this->toBuiltinCall(call, Builtin::BufferIndexOf, answer);
}

BufferChecker::EncodingAndPositions BufferChecker::splitValueArgs(const BufferCall &buffer) {
  CallExpr *call = buffer.call;
  EncodingAndPositions rest;
  for (size_t index = 1; index < call->args.size(); ++index) {
    auto *checked = this->exprChecker->checkExpr(call->args[index]);
    if (checked->type->isString()) {
      rest.encoding = this->bytesChecker->checkBufferEncoding(checked);
      if (index + 1 != call->args.size()) {
        auto message = ErrorTextBuffer::encoding(buffer.name);
        this->state.getDiagnostics().error(call->args[index + 1]->loc(), message);
      }
      break;
    }
    rest.positions.push_back(this->memberChecker->checkPosition(checked));
  }

  return rest;
}

Expr *BufferChecker::checkValueArg(const BufferCall &buffer, Expr *arg) {
  auto *checked = this->exprChecker->checkExpr(arg, nullptr);
  const Type *type = checked->type;
  if (type->isError() || type->isString()) {
    return checked;
  }
  if (type->isInteger()) {
    return this->typeRelations->convert(checked, Type::intTy());
  }
  const auto *uint8 = Type::bytesType(Bytes::Uint8);
  if (type->isTypedArray() && TypeRelations::isImplicitlyConvertible(type, uint8)) {
    return this->typeRelations->convert(checked, uint8);
  }
  auto message = ErrorTextBuffer::valueArg(buffer.name, type->name());
  this->state.getDiagnostics().error(checked->loc(), message);
  checked->type = Type::error();

  return checked;
}

void BufferChecker::reportEncodingWithoutText(const Expr *value, const Expr *encoding) {
  if (encoding != nullptr && !value->type->isString()) {
    auto message = ErrorTextBuffer::withString();
    this->state.getDiagnostics().error(encoding->loc(), message);
  }
}

Expr *BufferChecker::checkSwap(const BufferCall &buffer, int width) {
  CallExpr *call = buffer.call;
  auto arity = this->argumentChecker->checkArity(call, 0, 0);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(buffer.member->loc(), arity.error().message);
    return call;
  }
  call->args = {this->makeIntLiteral(call->loc(), width)};

  return this->toBuiltinCall(call, Builtin::BufferSwap, buffer.bufferType);
}

Expr *BufferChecker::checkNumberMethod(const BufferCall &buffer) {
  auto parsed = NumberMethodTable::parse(buffer.name, false);
  if (!parsed.has_value()) {
    return nullptr;
  }
  BytesNumberMethod number = *parsed;
  if (number.bigint) {
    auto quoted = ErrorTextBigInt::quoted(buffer.name);
    auto bigint = ErrorTextBigInt::none(quoted);
    this->state.getDiagnostics().error(buffer.member->loc(), bigint);
    this->argumentChecker->walkArgsLoosely(buffer.call);
    return buffer.call;
  }
  if (number.variable) {
    return this->checkVariableNumber(buffer, number);
  }

  return this->checkFixedNumber(buffer, number);
}

Expr *BufferChecker::checkVariableNumber(const BufferCall &buffer,
                                         const BytesNumberMethod &number) {
  CallExpr *call = buffer.call;
  unsigned argCount = number.write ? 3 : 2;
  auto arity = this->argumentChecker->checkArity(call, argCount, argCount);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(buffer.member->loc(), arity.error().message);
    return call;
  }
  std::vector<Expr *> args{this->makeBoolLiteral(call->loc(), number.isSigned),
                           this->makeBoolLiteral(call->loc(), number.big)};
  size_t at = 0;
  if (number.write) {
    auto *value = this->argumentChecker->checkArg(call->args[0], Type::intTy());
    args.push_back(value);
    at = 1;
  }
  auto *offset = this->memberChecker->checkPosition(call->args[at]);
  auto *count = this->memberChecker->checkPosition(call->args[at + 1]);
  args.push_back(offset);
  args.push_back(count);
  auto written = this->constantFolder->intConstant(count);
  auto bytes = this->checkCountOfBytes(written, buffer.name);
  if (bytes.isErr()) {
    this->state.getDiagnostics().error(count->loc(), bytes.error().message);
    return call;
  }

  int64_t width = bytes.value();
  auto offsetWhat = ErrorTextBufferLabel::offsetOf(buffer.name);
  auto upTo = this->indexProofs->requireUpTo(offset, buffer.member->object, width, offsetWhat);
  this->reportUnprovenUpTo(upTo, offset->loc());
  call->mayBeOutOfRange = upTo.isErr();
  if (number.write) {
    auto fits = this->checkVarValueFits(args[2], width, number.isSigned);
    if (fits.isErr()) {
      this->state.getDiagnostics().error(args[2]->loc(), fits.error().message);
    }
  }
  call->args = std::move(args);
  Builtin builtin = number.write ? Builtin::BufferWriteVar : Builtin::BufferReadVar;

  return this->toBuiltinCall(call, builtin, Type::intTy());
}

BufferChecker::ResultVarValueFits BufferChecker::checkVarValueFits(const Expr *value, int64_t width,
                                                                   bool isSigned) {
  int64_t bits = width * 8;
  int64_t low = isSigned ? -(int64_t(1) << (bits - 1)) : 0;
  int64_t high = isSigned ? (int64_t(1) << (bits - 1)) - 1 : (int64_t(1) << bits) - 1;
  bool isUnproven = !value->type->isError() && !this->byteProofs->canProveFits(value, low, high);
  if (isUnproven) {
    auto range = ErrorTextBytes::range(low, high);
    auto message = ErrorTextBuffer::varFits(width, range);

    return err(BufferCheckerVarValueFitsError{
        .code = BufferCheckerVarValueFitsErrorCode::VALUE_MAY_NOT_FIT,
        .message = message,
    });
  }

  return ok(value);
}

Expr *BufferChecker::checkFixedNumber(const BufferCall &buffer, const BytesNumberMethod &number) {
  CallExpr *call = buffer.call;
  Bytes kind = number.kind;
  const auto *kindType = Type::bytesType(kind);
  const Type *element = kindType->accessType();
  int64_t size = kindType->elementSize();
  unsigned minArgs = number.write ? 1 : 0;
  unsigned maxArgs = number.write ? 2 : 1;
  auto arity = this->argumentChecker->checkArity(call, minArgs, maxArgs);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(buffer.member->loc(), arity.error().message);
    return call;
  }
  std::vector<Expr *> args{this->makeIntLiteral(call->loc(), int64_t(kind)),
                           this->makeBoolLiteral(call->loc(), number.big)};
  size_t at = 0;
  if (number.write) {
    auto *value = this->argumentChecker->checkArg(call->args[0], element);
    auto what = BytesCheckHelpers::numberWhat(kind);
    auto fitting = this->byteProofs->checkValueFits(value, kindType, what);
    if (fitting.isErr()) {
      this->state.getDiagnostics().error(value->loc(), fitting.error().message);
    }
    args.push_back(value);
    at = 1;
  }
  Expr *offset =
      call->args.size() > at ? this->memberChecker->checkPosition(call->args[at]) : nullptr;
  if (offset != nullptr) {
    args.push_back(offset);
  }
  auto offsetWhat = ErrorTextBufferLabel::offsetOf(buffer.name);
  auto upTo = this->indexProofs->requireUpTo(offset, buffer.member->object, size, offsetWhat);
  llvm::SMLoc place = offset != nullptr ? offset->loc() : buffer.member->object->loc();
  this->reportUnprovenUpTo(upTo, place);
  call->mayBeOutOfRange = upTo.isErr();
  call->args = std::move(args);

  if (number.write) {
    return this->toBuiltinCall(call, Builtin::BufferWriteNumber, Type::intTy());
  }
  if (!element->isFloat()) {
    return this->toBuiltinCall(call, Builtin::BufferRead, element);
  }
  const auto *numberError = this->prelude->builtinType(PreludeType::NUMBER_ERROR);
  const auto *read = Type::result(Type::doubleTy(), numberError);

  return this->toBuiltinCall(call, Builtin::BufferRead, read);
}

Expr *BufferChecker::checkBytesArg(Expr *arg) {
  const auto *uint8 = Type::bytesType(Bytes::Uint8);

  return this->argumentChecker->checkArg(arg, uint8);
}

void BufferChecker::walkPositionArgs(CallExpr *call, size_t from) {
  for (size_t index = from; index < call->args.size(); ++index) {
    call->args[index] = this->memberChecker->checkPosition(call->args[index]);
  }
}

void BufferChecker::reportUnprovenUpTo(const IndexProofs::ResultUpToWasProven &upTo,
                                       llvm::SMLoc at) {
  bool isShown = upTo.isErr() && upTo.error().code != IndexProofsRequireUpToErrorCode::EXEMPT;
  if (isShown) {
    this->state.getDiagnostics().error(at, upTo.error().message);
  }
}

void BufferChecker::reportUnprovenBetween(const IndexProofs::ResultBetweenProven &between,
                                          llvm::SMLoc at) {
  bool isShown =
      between.isErr() && between.error().code != IndexProofsRequireBetweenErrorCode::EXEMPT;
  if (isShown) {
    this->state.getDiagnostics().error(at, between.error().message);
  }
}

IntLitExpr *BufferChecker::makeIntLiteral(llvm::SMLoc loc, int64_t value) {
  auto *literal = this->state.getAst().make<IntLitExpr>(loc, uint64_t(value));
  literal->type = Type::intTy();

  return literal;
}

BoolLitExpr *BufferChecker::makeBoolLiteral(llvm::SMLoc loc, bool value) {
  auto *literal = this->state.getAst().make<BoolLitExpr>(loc, value);
  literal->type = Type::boolTy();

  return literal;
}

Expr *BufferChecker::checkTextMethod(CallExpr *call, MemberExpr *member) {
  const Type *type = member->object->type;
  auto method = BytesMethodTable::findMethod(member->name);
  bool isEncoder = type->bytes() == Bytes::TextEncoder;
  bool isDecoder = type->bytes() == Bytes::TextDecoder || type->bytes() == Bytes::FatalTextDecoder;
  if (isEncoder && method == BytesMethod::ENCODE) {
    return this->checkEncode(call, member);
  }
  if (isEncoder && method == BytesMethod::ENCODE_INTO) {
    return this->checkEncodeInto(call, member);
  }
  if (isDecoder && method == BytesMethod::DECODE) {
    return this->checkDecode(call, member);
  }
  this->argumentChecker->walkArgsLoosely(call);
  auto message = ErrorTextBuffer::noMember(member->name, type->name());
  this->state.getDiagnostics().error(member->loc(), message);

  return call;
}

Expr *BufferChecker::checkEncode(CallExpr *call, MemberExpr *member) {
  auto arity = this->argumentChecker->checkArity(call, 0, 1);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(member->loc(), arity.error().message);
    return call;
  }
  if (!call->args.empty()) {
    call->args[0] = this->argumentChecker->checkArg(call->args[0], Type::string());
  }
  const auto *uint8 = Type::bytesType(Bytes::Uint8);

  return this->toBuiltinCall(call, Builtin::TextEncode, uint8);
}

Expr *BufferChecker::checkEncodeInto(CallExpr *call, MemberExpr *member) {
  auto arity = this->argumentChecker->checkArity(call, 2, 2);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(member->loc(), arity.error().message);
    return call;
  }
  call->args[0] = this->argumentChecker->checkArg(call->args[0], Type::string());
  call->args[1] = this->checkBytesArg(call->args[1]);
  auto readName = BytesShapeFieldTable::getName(BytesShapeField::ENCODE_READ);
  auto writtenName = BytesShapeFieldTable::getName(BytesShapeField::ENCODE_WRITTEN);
  const auto *counts = Type::object(
      {{readName.str(), Type::intTy(), false}, {writtenName.str(), Type::intTy(), false}});

  return this->toBuiltinCall(call, Builtin::TextEncodeInto, counts);
}

Expr *BufferChecker::checkDecode(CallExpr *call, MemberExpr *member) {
  auto arity = this->argumentChecker->checkArity(call, 0, 2);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(member->loc(), arity.error().message);
    return call;
  }
  if (!call->args.empty()) {
    auto *input = this->exprChecker->checkExpr(call->args[0]);
    call->args[0] = input;
    auto decoded = this->checkDecodedInput(input->type);
    if (decoded.isErr()) {
      this->state.getDiagnostics().error(input->loc(), decoded.error().message);
      return call;
    }
  }
  if (call->args.size() == 2) {
    auto streamName = BytesShapeFieldTable::getName(BytesShapeField::DECODE_STREAM);
    const auto *options = Type::object({{streamName.str(), Type::optional(Type::boolTy()), false}});
    auto *given = this->exprChecker->checkExpr(call->args[1], options);
    auto what = ErrorTextBufferLabel::options();
    call->args[1] = this->typeRelations->coerce(given, options, what);
  }
  bool isFatal = member->object->type->bytes() == Bytes::FatalTextDecoder;
  if (!isFatal) {
    return this->toBuiltinCall(call, Builtin::TextDecode, Type::string());
  }
  const auto *decodeError = this->prelude->builtinType(PreludeType::DECODE_ERROR);
  const auto *decoded = Type::result(Type::string(), decodeError);

  return this->toBuiltinCall(call, Builtin::TextDecode, decoded);
}

void BufferChecker::walkWriteLength(const Expr *object, const Expr *offset, const Expr *length) {
  IndexProofs &proofs = *this->indexProofs;
  auto what = ErrorTextBufferLabel::length();
  auto between = proofs.requireBetween(length, 0, std::nullopt, what);
  if (between.isErr() && between.error().code != IndexProofsRequireBetweenErrorCode::EXEMPT) {
    this->state.getDiagnostics().error(length->loc(), between.error().message);
  }
  if (between.isErr() || this->indexProofs->isExempt(length->loc(), object)) {
    return;
  }
  if (!proofs.canProveSumUpTo(proofs.linear(offset), proofs.linear(length), object)) {
    auto message = ErrorTextBuffer::written(BytesCheckHelpers::valueText(offset),
                                            BytesCheckHelpers::valueText(length),
                                            BytesCheckHelpers::valueText(object));
    this->state.getDiagnostics().error(length->loc(), message);
  }
}

}
