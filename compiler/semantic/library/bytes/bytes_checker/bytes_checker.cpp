#include "compiler/semantic/library/bytes/bytes_checker/bytes_checker.h"
#include "compiler/program/library/builtin_names/builtin_property_table/builtin_property_table.h"
#include "compiler/program/library/bytes/bytes_method_table/bytes_method_table.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "compiler/semantic/bodies/expr/argument_checker/argument_checker.h"
#include "compiler/semantic/bodies/expr/call_checker/call_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/bodies/expr/member_checker/member_checker.h"
#include "compiler/semantic/declare/constant_folder/constant_folder.h"
#include "compiler/semantic/flow/index_proofs/index_proofs.h"
#include "compiler/semantic/library/bytes/buffer_checker/buffer_checker.h"
#include "compiler/semantic/library/bytes/byte_proofs/byte_proofs.h"
#include "compiler/semantic/library/bytes/bytes_check_helpers/bytes_check_helpers.h"
#include "compiler/semantic/library/bytes/typed_array_checker/typed_array_checker.h"
#include "compiler/semantic/library/process/process_checker/process_checker.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/literal_types/literal_types.h"
#include "runtime/library/bytes/encodings/encodings.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/MathExtras.h"

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

namespace {

const std::pair<llvm::StringLiteral, Bytes> kNames[] = {
    {"Uint8Array", Bytes::Uint8},
    {"Int8Array", Bytes::Int8},
    {"Uint8ClampedArray", Bytes::Uint8Clamped},
    {"Int16Array", Bytes::Int16},
    {"Uint16Array", Bytes::Uint16},
    {"Int32Array", Bytes::Int32},
    {"Uint32Array", Bytes::Uint32},
    {"Float16Array", Bytes::Float16},
    {"Float32Array", Bytes::Float32},
    {"Float64Array", Bytes::Float64},
    {"Buffer", Bytes::Buffer},
    {"ArrayBuffer", Bytes::ArrayBuffer},
    {"DataView", Bytes::DataView},
    {"TextEncoder", Bytes::TextEncoder},
    {"TextDecoder", Bytes::TextDecoder},
    {"FatalTextDecoder", Bytes::FatalTextDecoder},
};

}

BytesChecker::BytesChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->callChecker = &all.callChecker;
  this->exprChecker = &all.exprChecker;
  this->indexProofs = &all.indexProofs;
  this->literalTypes = &all.literalTypes;
  this->bufferChecker = &all.bufferChecker;
  this->memberChecker = &all.memberChecker;
  this->constantFolder = &all.constantFolder;
  this->processChecker = &all.processChecker;
  this->argumentChecker = &all.argumentChecker;
  this->typedArrayChecker = &all.typedArrayChecker;
}

std::optional<std::u16string> BytesChecker::literalText(const Expr *e) {
  if (auto *conv = dyn_cast<CastExpr>(e); conv && conv->isImplicit()) {
    e = conv->operand;
  }
  if (auto *lit = dyn_cast<StringLitExpr>(e)) {
    return lit->value;
  }
  if (auto *tmpl = dyn_cast<TemplateExpr>(e); tmpl && tmpl->exprs.empty()) {
    return tmpl->quasis[0];
  }

  return std::nullopt;
}

BytesChecker::ResultKnownEncoding BytesChecker::knownEncoding(llvm::StringRef name) {
  std::optional<int> encoding = BytesCheckHelpers::encodingNamed(name);
  if (!encoding) {
    std::string message = ErrorTextBufferEncoding::unknown(name);

    return err(BytesCheckerKnownEncodingError{
        .code = BytesCheckerKnownEncodingErrorCode::UNKNOWN_ENCODING,
        .message = message,
    });
  }

  return ok(*encoding);
}

BytesChecker::ResultBytesIndexing BytesChecker::bytesIndexing(const Type *type) {
  if (!type->isTypedArray()) {
    std::string message = ErrorTextBytesIndex::notIndexed(type);

    return err(BytesCheckerBytesIndexingError{
        .code = BytesCheckerBytesIndexingErrorCode::NOT_INDEXED,
        .message = message,
    });
  }

  return ok(type);
}

const Type *BytesChecker::bytesTypeNamed(llvm::StringRef name) {
  for (const auto &[n, kind] : kNames) {
    if (n == name) {
      return Type::bytesType(kind);
    }
  }

  return nullptr;
}

Expr *BytesChecker::checkBytesIndex(IndexExpr *index) {
  const Type *type = index->object->type;
  index->index = this->memberChecker->checkPosition(index->index);
  ResultBytesIndexing indexing = BytesChecker::bytesIndexing(type);
  if (indexing.isErr()) {
    this->state.getDiagnostics().error(index->loc(), indexing.error().message);
    return index;
  }
  if (index->index->type->isError()) {
    return index;
  }
  bool isInRange = this->indexProofs->provesInBounds(index->object, index->index, false);
  if (ElementIndex *element = index->form.as<ElementIndex>()) {
    element->mayBeOutOfRange = !isInRange;
  }
  index->type = isInRange ? type->elementType() : Type::optional(type->elementType());

  return index;
}

void BytesChecker::walkBytesStore(IndexExpr *index, const Expr *value) {
  const Type *target = index->object->type;
  if (index->index->type->isError() || (value && value->type->isError())) {
    return;
  }
  std::string object = BytesCheckHelpers::valueText(index->object);
  std::string position = BytesCheckHelpers::valueText(index->index);
  if (!this->indexProofs->provesInBounds(index->object, index->index, false)) {
    std::optional<int64_t> at = this->constantFolder->intConstant(index->index);
    std::string message = ErrorTextBytesIndex::store(target->name(), object, position, at);
    this->state.getDiagnostics().error(index->loc(), message);
    return;
  }
}

Expr *BytesChecker::checkBytesMember(MemberExpr *member) {
  std::optional<BuiltinProperty> property = BuiltinPropertyTable::find(member->name);
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

Expr *BytesChecker::checkArrayBufferMember(MemberExpr *member,
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

Expr *BytesChecker::checkDataViewMember(MemberExpr *member,
                                        std::optional<BuiltinProperty> property) {
  if (property == BuiltinProperty::BYTE_LENGTH) {
    member->type = Type::intTy();
    return member;
  }
  if (property == BuiltinProperty::BYTE_OFFSET) {
    return this->readBuiltin(member, Builtin::BytesByteOffset, Type::intTy());
  }
  if (property == BuiltinProperty::BUFFER) {
    const Type *arrayBuffer = Type::bytesType(Bytes::ArrayBuffer);
    return this->readBuiltin(member, Builtin::BytesBuffer, arrayBuffer);
  }

  return this->reportNoMember(member);
}

Expr *BytesChecker::checkTextEncoderMember(MemberExpr *member,
                                           std::optional<BuiltinProperty> property) {
  if (property == BuiltinProperty::ENCODING) {
    auto *literal = this->state.getAst().make<StringLitExpr>(member->loc(), u"utf-8");
    literal->type = Type::string();
    return this->keepObject(member, literal);
  }

  return this->reportNoMember(member);
}

Expr *BytesChecker::checkTextDecoderMember(MemberExpr *member,
                                           std::optional<BuiltinProperty> property) {
  if (property == BuiltinProperty::ENCODING) {
    IntLitExpr *which = this->makeIntLiteral(member->loc(), 0);
    return this->readBuiltin(member, Builtin::TextDecoderInfo, Type::string(), {which});
  }
  if (property == BuiltinProperty::FATAL || property == BuiltinProperty::IGNORE_BOM) {
    int64_t index = property == BuiltinProperty::FATAL ? 1 : 2;
    IntLitExpr *which = this->makeIntLiteral(member->loc(), index);
    return this->readBuiltin(member, Builtin::TextDecoderInfo, Type::boolTy(), {which});
  }

  return this->reportNoMember(member);
}

Expr *BytesChecker::checkViewMember(MemberExpr *member, std::optional<BuiltinProperty> property) {
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
    const Type *arrayBuffer = Type::bytesType(Bytes::ArrayBuffer);
    return this->readBuiltin(member, Builtin::BytesBuffer, arrayBuffer);
  }
  if (property == BuiltinProperty::BYTES_PER_ELEMENT) {
    IntLitExpr *size = this->makeIntLiteral(member->loc(), type->elementSize());
    return this->keepObject(member, size);
  }
  bool isDeprecated = type->isBuffer() &&
                      (property == BuiltinProperty::PARENT || property == BuiltinProperty::OFFSET);
  if (isDeprecated) {
    std::string message = ErrorTextBytesMember::deprecated(member->name);
    this->state.getDiagnostics().error(member->loc(), message);
    return member;
  }
  const BytesMethodInfo *typedMethod = BytesMethodTable::find(member->name);
  bool isMethod = typedMethod != nullptr && !typedMethod->form.is<NotOnTypedArrayMethod>();
  if (isMethod) {
    std::string message = ErrorTextBytesMember::notCalled(member->name, type->name());
    this->state.getDiagnostics().error(member->loc(), message);
    return member;
  }

  return this->reportNoMember(member);
}

Expr *BytesChecker::reportNoMember(MemberExpr *member) {
  std::string message = ErrorTextBytesMember::noMember(member->name, member->object->type->name());
  this->state.getDiagnostics().error(member->loc(), message);

  return member;
}

Expr *BytesChecker::readBuiltin(MemberExpr *member, Builtin builtin, const Type *result,
                                std::vector<Expr *> args) {
  return this->processChecker->builtinCall(member->loc(), member, builtin, std::move(args), result);
}

Expr *BytesChecker::keepObject(MemberExpr *member, Expr *value) {
  if (isa<NameExpr>(member->object)) {
    return value;
  }
  std::vector<Expr *> parts{member->object, value};
  auto *sequence = this->state.getAst().make<SequenceExpr>(member->loc(), parts);
  sequence->type = value->type;

  return sequence;
}

IntLitExpr *BytesChecker::makeIntLiteral(llvm::SMLoc loc, int64_t value) {
  auto *literal = this->state.getAst().make<IntLitExpr>(loc, uint64_t(value));
  literal->type = Type::intTy();

  return literal;
}

BoolLitExpr *BytesChecker::makeBoolLiteral(llvm::SMLoc loc, bool value) {
  auto *literal = this->state.getAst().make<BoolLitExpr>(loc, value);
  literal->type = Type::boolTy();

  return literal;
}

Expr *BytesChecker::checkBytesUnionMember(MemberExpr *member) {
  const Type *type = member->object->type;
  llvm::StringRef name = member->name;
  std::optional<BuiltinProperty> property = BuiltinPropertyTable::find(name);
  auto has = [&](const Type *m) {
    if (property == BuiltinProperty::BYTE_LENGTH) {
      return m->isByteView() || m->bytes() == Bytes::ArrayBuffer;
    }
    if (property == BuiltinProperty::BYTE_OFFSET || property == BuiltinProperty::BUFFER) {
      return m->isByteView();
    }

    return property == BuiltinProperty::LENGTH && m->isTypedArray();
  };
  static const BuiltinProperty others[] = {
      BuiltinProperty::BYTES_PER_ELEMENT, BuiltinProperty::MAX_BYTE_LENGTH,
      BuiltinProperty::RESIZABLE,         BuiltinProperty::DETACHED,
      BuiltinProperty::ENCODING,          BuiltinProperty::FATAL,
      BuiltinProperty::IGNORE_BOM};
  if (property.has_value() && llvm::is_contained(others, *property)) {
    std::string message = ErrorTextBytesMember::narrow(name, type->name());
    this->state.getDiagnostics().error(member->loc(), message);
    return member;
  }
  llvm::ArrayRef<const Type *> members = type->members();
  if (const Type *const *lacking = llvm::find_if_not(members, has); lacking != members.end()) {
    const Type *const *with = llvm::find_if(members, has);
    llvm::StringRef withName = with != members.end() ? (*with)->name() : llvm::StringRef();
    std::string message =
        ErrorTextBytesMember::lacking(name, (*lacking)->name(), type->name(), withName);
    this->state.getDiagnostics().error(member->loc(), message);
    return member;
  }
  if (property == BuiltinProperty::LENGTH) {
    member->type = Type::intTy();
    return member;
  }
  if (property == BuiltinProperty::BUFFER) {
    return this->processChecker->builtinCall(member->loc(), member, Builtin::BytesBuffer, {},
                                             Type::bytesType(Bytes::ArrayBuffer));
  }

  return this->processChecker->builtinCall(member->loc(), member,
                                           property == BuiltinProperty::BYTE_LENGTH
                                               ? Builtin::BytesByteLength
                                               : Builtin::BytesByteOffset,
                                           {}, Type::intTy());
}

Expr *BytesChecker::checkBytesStaticMember(MemberExpr *member) {
  auto *object = dyn_cast<NameExpr>(member->object);
  if (!object || !this->callChecker->isUnshadowedGlobal(object)) {
    return nullptr;
  }
  const Type *type = BytesChecker::bytesTypeNamed(object->name);
  if (!type) {
    return nullptr;
  }
  member->type = Type::error();
  std::optional<BuiltinProperty> property = BuiltinPropertyTable::find(member->name);
  if (type->isTypedArray() && property == BuiltinProperty::BYTES_PER_ELEMENT) {
    auto *lit = this->state.getAst().make<IntLitExpr>(member->loc(), type->elementSize());
    lit->type = Type::intTy();
    return lit;
  }
  if (type->isBuffer() && property == BuiltinProperty::POOL_SIZE) {
    auto *lit = this->state.getAst().make<IntLitExpr>(member->loc(), 8192);
    lit->type = Type::intTy();
    return lit;
  }
  std::string message = ErrorTextBytesMember::callOnly(object->name, member->name);
  this->state.getDiagnostics().error(member->loc(), message);

  return member;
}

Expr *BytesChecker::bytesToString(Expr *expr) {
  const Type *type = expr->type;
  if (type->isTypedArray()) {
    llvm::StringRef toStringName = BytesMethodTable::getName(BytesMethod::TO_STRING);
    auto *callee = this->state.getAst().make<MemberExpr>(expr->loc(), expr, toStringName);
    if (type->isBuffer()) {
      auto *encoding = this->state.getAst().make<IntLitExpr>(expr->loc(), Utf8);
      encoding->type = Type::intTy();
      return this->processChecker->builtinCall(expr->loc(), callee, Builtin::BufferToString,
                                               {encoding}, Type::string());
    }
    return this->processChecker->builtinCall(expr->loc(), callee, Builtin::BytesJoin, {},
                                             Type::string());
  }
  std::string message =
      ErrorTextBytesString::noText(type->name(), type->bytes() == Bytes::ArrayBuffer);
  this->state.getDiagnostics().error(expr->loc(), message);

  return expr;
}

Expr *BytesChecker::checkBufferEncoding(Expr *arg) {
  if (std::optional<std::u16string> text = BytesChecker::literalText(arg)) {
    std::string written = ToUtf8::toUtf8(*text);
    ResultKnownEncoding encoding = BytesChecker::knownEncoding(written);
    if (encoding.isErr()) {
      this->state.getDiagnostics().error(arg->loc(), encoding.error().message);
      arg->type = Type::error();
      return arg;
    }
    auto *lit = this->state.getAst().make<IntLitExpr>(arg->loc(), uint64_t(encoding.value()));
    lit->type = Type::intTy();
    return lit;
  }
  arg = this->argumentChecker->checkArg(arg, Type::string());
  if (arg->type->isError() || this->indexProofs->exempt(arg->loc())) {
    return arg;
  }
  std::optional<std::vector<std::string>> names = this->literalTypes->knownTexts(arg);
  if (!names) {
    std::string message = ErrorTextBufferEncoding::notKnown();
    this->state.getDiagnostics().error(arg->loc(), message);
    return arg;
  }
  for (const std::string &name : *names) {
    ResultKnownEncoding encoding = BytesChecker::knownEncoding(name);
    if (encoding.isErr()) {
      this->state.getDiagnostics().error(arg->loc(), encoding.error().message);
      break;
    }
  }

  return arg;
}

Expr *BytesChecker::checkBytesMethod(CallExpr *call, MemberExpr *member, const Type *expected) {
  const Type *type = member->object->type;
  call->type = Type::error();
  if (this->callChecker->rejectSpread(call)) {
    return call;
  }
  switch (type->bytes()) {
  case Bytes::ArrayBuffer: {
    llvm::StringRef name = member->name;
    std::optional<BytesMethod> method = BytesMethodTable::findMethod(name);
    if (method == BytesMethod::SLICE) {
      ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 0, 2);
      if (arity.isErr()) {
        this->state.getDiagnostics().error(member->loc(), arity.error().message);
        return call;
      }
      for (Expr *&arg : call->args) {
        arg = this->memberChecker->checkPosition(arg);
      }
      call->target = BuiltinCall{.builtin = Builtin::ArrayBufferSlice};
      call->type = type;
      return call;
    }
    for (Expr *&arg : call->args) {
      arg = this->exprChecker->checkExpr(arg);
    }
    bool isNotYet = method == BytesMethod::RESIZE || method == BytesMethod::TRANSFER ||
                    method == BytesMethod::TRANSFER_TO_FIXED_LENGTH;
    std::string message =
        isNotYet ? ErrorTextArrayBuffer::notYet(name) : ErrorTextArrayBuffer::noMember(name);
    this->state.getDiagnostics().error(member->loc(), message);
    return call;
  }
  case Bytes::DataView:
    return this->typedArrayChecker->checkDataViewMethod(call, member);
  case Bytes::TextEncoder:
  case Bytes::TextDecoder:
  case Bytes::FatalTextDecoder:
    return this->bufferChecker->checkTextMethod(call, member);
  default:
    break;
  }
  if (type->isBuffer()) {
    if (Expr *result = this->bufferChecker->checkBufferMethod(call, member)) {
      return result;
    }
  }

  return this->typedArrayChecker->checkTypedArrayMethod(call, member, expected);
}

}
