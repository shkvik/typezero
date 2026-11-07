#include "compiler/pipeline/check_program/topics/bytes/bytes_checker/bytes_checker.h"
#include "compiler/library/bytes/bytes_check_helpers/bytes_check_helpers.h"
#include "compiler/pipeline/check_program/declare/constant_folder/constant_folder.h"
#include "compiler/pipeline/check_program/flow/index_proofs/index_proofs.h"
#include "compiler/pipeline/check_program/topics/bytes/buffer_checker/buffer_checker.h"
#include "compiler/pipeline/check_program/topics/bytes/byte_proofs/byte_proofs.h"
#include "compiler/pipeline/check_program/topics/bytes/typed_array_checker/typed_array_checker.h"
#include "compiler/pipeline/check_program/topics/expr/argument_checker/argument_checker.h"
#include "compiler/pipeline/check_program/topics/expr/call_checker/call_checker.h"
#include "compiler/pipeline/check_program/topics/expr/expr_checker/expr_checker.h"
#include "compiler/pipeline/check_program/topics/expr/member_checker/member_checker.h"
#include "compiler/pipeline/check_program/topics/process/process_checker/process_checker.h"
#include "compiler/pipeline/check_program/types/literal_types/literal_types.h"
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

const llvm::StringLiteral kTypedMethods[] = {
    "at",          "copyWithin", "entries",  "every",         "fill",    "filter",
    "find",        "findIndex",  "findLast", "findLastIndex", "forEach", "includes",
    "indexOf",     "join",       "keys",     "lastIndexOf",   "map",     "reduce",
    "reduceRight", "reverse",    "set",      "slice",         "some",    "sort",
    "subarray",    "toReversed", "toSorted", "toString",      "values",  "with"};

}

std::string BytesChecker::utf8(const std::u16string &text) {
  std::string out;
  llvm::convertUTF16ToUTF8String(
      llvm::ArrayRef<llvm::UTF16>(reinterpret_cast<const llvm::UTF16 *>(text.data()), text.size()),
      out);

  return out;
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

std::string BytesChecker::rangeText(int64_t lo, int64_t hi) {
  return std::to_string(lo) + " to " + std::to_string(hi);
}

Range BytesChecker::exact(int64_t v) {
  return {v, v};
}

bool BytesChecker::known(const Range &r) {
  return r.lo && r.hi;
}

int64_t BytesChecker::allOnes(int64_t v) {
  int64_t m = 0;
  while (m < v) {
    m = m * 2 + 1;
  }

  return m;
}

int64_t BytesChecker::floorShift(int64_t v, int64_t n) {
  return v >= 0 ? v >> n : -((-v + (int64_t(1) << n) - 1) >> n);
}

BytesChecker::BytesChecker(CheckState &owner)
    : state(owner) {}

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
  index->index = MemberChecker(this->state).checkPosition(index->index);
  if (!type->isTypedArray()) {
    this->state.getDiagnostics().error(
        index->loc(),
        "type '" + type->name() + "' cannot be indexed; " +
            (type->bytes() == Bytes::DataView      ? "read its bytes with getUint8(i) and the like"
             : type->bytes() == Bytes::ArrayBuffer ? "read its bytes through a view, e.g. 'new "
                                                     "Uint8Array(buffer)[i]'"
                                                   : "it holds no elements"));
    return index;
  }
  if (index->index->type->isError()) {
    return index;
  }
  bool inRange = IndexProofs(this->state).provesInBounds(index->object, index->index, false);
  if (ElementIndex *element = index->form.as<ElementIndex>()) {
    element->mayBeOutOfRange = !inRange;
  }
  index->type = inRange ? type->elementType() : Type::optional(type->elementType());

  return index;
}

void BytesChecker::checkBytesStore(IndexExpr *index, const Expr *value) {
  const Type *target = index->object->type;
  if (index->index->type->isError() || (value && value->type->isError())) {
    return;
  }
  std::string object = valueText(index->object);
  std::string position = valueText(index->index);
  if (!IndexProofs(this->state).provesInBounds(index->object, index->index, false)) {
    std::optional<int64_t> at = ConstantFolder(this->state).intConstant(index->index);
    std::string check = at ? object + ".length > " + std::to_string(*at)
                           : position + " >= 0 && " + position + " < " + object + ".length";
    this->state.getDiagnostics().error(
        index->loc(), "cannot prove that the index is within the " + target->name() +
                          " (0 <= index < length): it never grows, and JavaScript would "
                          "drop the write; check the index first, e.g. 'if (" +
                          check + ")'");
    return;
  }
}

Expr *BytesChecker::checkBytesMember(MemberExpr *member) {
  const Type *type = member->object->type;
  llvm::StringRef name = member->name;
  auto read = [&](Builtin builtin, const Type *result, std::vector<Expr *> args = {}) {
    return ProcessChecker(this->state)
        .builtinCall(member->loc(), member, builtin, std::move(args), result);
  };
  auto constant = [&](Expr *value) -> Expr * {
    if (isa<NameExpr>(member->object)) {
      return value;
    }
    auto *seq = this->state.getAst().make<SequenceExpr>(member->loc(),
                                                        std::vector<Expr *>{member->object, value});
    seq->type = value->type;

    return seq;
  };
  auto intLit = [&](int64_t v) {
    auto *lit = this->state.getAst().make<IntLitExpr>(member->loc(), uint64_t(v));
    lit->type = Type::intTy();

    return lit;
  };
  auto boolLit = [&](bool v) {
    auto *lit = this->state.getAst().make<BoolLitExpr>(member->loc(), v);
    lit->type = Type::boolTy();

    return lit;
  };
  switch (type->bytes()) {
  case Bytes::ArrayBuffer:
    if (name == "byteLength") {
      member->type = Type::intTy();
      return member;
    }
    if (name == "maxByteLength") {
      return read(Builtin::BytesByteLength, Type::intTy());
    }
    if (name == "resizable" || name == "detached") {
      return constant(boolLit(false));
    }
    break;
  case Bytes::DataView:
    if (name == "byteLength") {
      member->type = Type::intTy();
      return member;
    }
    if (name == "byteOffset") {
      return read(Builtin::BytesByteOffset, Type::intTy());
    }
    if (name == "buffer") {
      return read(Builtin::BytesBuffer, Type::bytesType(Bytes::ArrayBuffer));
    }
    break;
  case Bytes::TextEncoder:
    if (name == "encoding") {
      auto *lit = this->state.getAst().make<StringLitExpr>(member->loc(), u"utf-8");
      lit->type = Type::string();
      return constant(lit);
    }
    break;
  case Bytes::TextDecoder:
  case Bytes::FatalTextDecoder:
    if (name == "encoding") {
      return read(Builtin::TextDecoderInfo, Type::string(), {intLit(0)});
    }
    if (name == "fatal" || name == "ignoreBOM") {
      return read(Builtin::TextDecoderInfo, Type::boolTy(), {intLit(name == "fatal" ? 1 : 2)});
    }
    break;
  default:
    if (name == "length") {
      member->type = Type::intTy();
      return member;
    }
    if (name == "byteLength") {
      return read(Builtin::BytesByteLength, Type::intTy());
    }
    if (name == "byteOffset") {
      return read(Builtin::BytesByteOffset, Type::intTy());
    }
    if (name == "buffer") {
      return read(Builtin::BytesBuffer, Type::bytesType(Bytes::ArrayBuffer));
    }
    if (name == "BYTES_PER_ELEMENT") {
      return constant(intLit(type->elementSize()));
    }
    if (type->isBuffer() && (name == "parent" || name == "offset")) {
      this->state.getDiagnostics().error(member->loc(),
                                         "'" + name +
                                             "' of a Buffer is deprecated in node and not "
                                             "supported; use '" +
                                             (name == "parent" ? "buffer" : "byteOffset") + "'");
      return member;
    }
    if (llvm::is_contained(kTypedMethods, name)) {
      this->state.getDiagnostics().error(member->loc(),
                                         "property '" + name + "' does not exist on type '" +
                                             type->name() + "'; methods must be called");
      return member;
    }
    break;
  }
  this->state.getDiagnostics().error(
      member->loc(), "property '" + name + "' does not exist on type '" + type->name() + "'");

  return member;
}

Expr *BytesChecker::checkBytesUnionMember(MemberExpr *member) {
  const Type *type = member->object->type;
  llvm::StringRef name = member->name;
  auto has = [&](const Type *m) {
    if (name == "byteLength") {
      return m->isByteView() || m->bytes() == Bytes::ArrayBuffer;
    }
    if (name == "byteOffset" || name == "buffer") {
      return m->isByteView();
    }

    return name == "length" && m->isTypedArray();
  };
  static const llvm::StringLiteral others[] = {"BYTES_PER_ELEMENT", "maxByteLength", "resizable",
                                               "detached",          "encoding",      "fatal",
                                               "ignoreBOM"};
  if (llvm::is_contained(others, name)) {
    this->state.getDiagnostics().error(member->loc(),
                                       "property '" + name + "' of '" + type->name() +
                                           "' is not supported yet; narrow it first");
    return member;
  }
  llvm::ArrayRef<const Type *> members = type->members();
  if (const Type *const *lacking = llvm::find_if_not(members, has); lacking != members.end()) {
    std::string message = "property '" + name.str() + "' does not exist on '" +
                          (*lacking)->name().str() + "', which '" + type->name().str() +
                          "' may be; narrow it first";
    if (const Type *const *with = llvm::find_if(members, has); with != members.end()) {
      message += ", e.g. 'if (x instanceof " + (*with)->name().str() + ")'";
    }
    this->state.getDiagnostics().error(member->loc(), message);
    return member;
  }
  if (name == "length") {
    member->type = Type::intTy();
    return member;
  }
  if (name == "buffer") {
    return ProcessChecker(this->state)
        .builtinCall(member->loc(), member, Builtin::BytesBuffer, {},
                     Type::bytesType(Bytes::ArrayBuffer));
  }

  return ProcessChecker(this->state)
      .builtinCall(member->loc(), member,
                   name == "byteLength" ? Builtin::BytesByteLength : Builtin::BytesByteOffset, {},
                   Type::intTy());
}

Expr *BytesChecker::checkBytesStaticMember(MemberExpr *member) {
  auto *object = dyn_cast<NameExpr>(member->object);
  if (!object || !CallChecker(this->state).isGlobalName(object, object->name)) {
    return nullptr;
  }
  const Type *type = BytesChecker::bytesTypeNamed(object->name);
  if (!type) {
    return nullptr;
  }
  member->type = Type::error();
  if (type->isTypedArray() && member->name == "BYTES_PER_ELEMENT") {
    auto *lit = this->state.getAst().make<IntLitExpr>(member->loc(), type->elementSize());
    lit->type = Type::intTy();
    return lit;
  }
  if (type->isBuffer() && member->name == "poolSize") {
    auto *lit = this->state.getAst().make<IntLitExpr>(member->loc(), 8192);
    lit->type = Type::intTy();
    return lit;
  }
  this->state.getDiagnostics().error(member->loc(), "'" + object->name + "." + member->name +
                                                        "' can be used only in a call, e.g. '" +
                                                        object->name + "." + member->name +
                                                        "(...)'");

  return member;
}

Expr *BytesChecker::bytesToString(Expr *expr) {
  const Type *type = expr->type;
  if (type->isTypedArray()) {
    auto *callee = this->state.getAst().make<MemberExpr>(expr->loc(), expr, "toString");
    if (type->isBuffer()) {
      auto *encoding = this->state.getAst().make<IntLitExpr>(expr->loc(), Utf8);
      encoding->type = Type::intTy();
      return ProcessChecker(this->state)
          .builtinCall(expr->loc(), callee, Builtin::BufferToString, {encoding}, Type::string());
    }
    return ProcessChecker(this->state)
        .builtinCall(expr->loc(), callee, Builtin::BytesJoin, {}, Type::string());
  }
  this->state.getDiagnostics().error(
      expr->loc(), "a value of type '" + type->name() +
                       "' cannot be converted to a string: JavaScript would give '[object " +
                       type->name() + "]'; print it with console.log" +
                       (type->bytes() == Bytes::ArrayBuffer
                            ? ", or the text of its bytes with 'new TextDecoder().decode(buffer)'"
                            : ""));

  return expr;
}

Expr *BytesChecker::checkBufferEncoding(Expr *arg) {
  if (std::optional<std::u16string> text = BytesChecker::literalText(arg)) {
    std::optional<int> encoding = encodingNamed(BytesChecker::utf8(*text));
    if (!encoding) {
      this->state.getDiagnostics().error(
          arg->loc(), "unknown encoding '" + BytesChecker::utf8(*text) +
                          "' (node throws a TypeError); Buffer knows utf8, utf16le, "
                          "latin1, ascii, base64, base64url and hex");
      arg->type = Type::error();
      return arg;
    }
    auto *lit = this->state.getAst().make<IntLitExpr>(arg->loc(), uint64_t(*encoding));
    lit->type = Type::intTy();
    return lit;
  }
  arg = ArgumentChecker(this->state).checkArg(arg, Type::string());
  if (arg->type->isError() || IndexProofs(this->state).exempt(arg->loc())) {
    return arg;
  }
  std::optional<std::vector<std::string>> names = LiteralTypes(this->state).knownTexts(arg);
  if (!names) {
    this->state.getDiagnostics().error(
        arg->loc(), "an encoding known only when the program runs may be one Buffer does "
                    "not know, which node would throw a TypeError for; check it first, "
                    "e.g. 'if (enc == \"hex\" || enc == \"base64\")', or give it the type "
                    "'BufferEncoding'");
    return arg;
  }
  for (const std::string &name : *names) {
    if (!encodingNamed(name)) {
      this->state.getDiagnostics().error(
          arg->loc(), "unknown encoding '" + name +
                          "' (node throws a TypeError); Buffer knows utf8, utf16le, "
                          "latin1, ascii, base64, base64url and hex");
      break;
    }
  }

  return arg;
}

Expr *BytesChecker::checkBytesMethod(CallExpr *call, MemberExpr *member, const Type *expected) {
  const Type *type = member->object->type;
  call->type = Type::error();
  if (CallChecker(this->state).rejectSpread(call)) {
    return call;
  }
  switch (type->bytes()) {
  case Bytes::ArrayBuffer: {
    llvm::StringRef name = member->name;
    if (name == "slice") {
      ArgumentChecker::ResultArityMatches arity =
          ArgumentChecker(this->state).checkArity(call, 0, 2);
      if (arity.isErr()) {
        this->state.getDiagnostics().error(member->loc(), arity.error().message);
        return call;
      }
      for (Expr *&arg : call->args) {
        arg = MemberChecker(this->state).checkPosition(arg);
      }
      call->target = BuiltinCall{.builtin = Builtin::ArrayBufferSlice};
      call->type = type;
      return call;
    }
    for (Expr *&arg : call->args) {
      arg = ExprChecker(this->state).checkExpr(arg);
    }
    if (name == "resize" || name == "transfer" || name == "transferToFixedLength") {
      this->state.getDiagnostics().error(member->loc(),
                                         "'" + name +
                                             "' is not supported yet: an ArrayBuffer keeps its "
                                             "size and its memory (docs/bytes.md, section 7)");
    } else {
      this->state.getDiagnostics().error(
          member->loc(), "property '" + name + "' does not exist on type 'ArrayBuffer'");
    }
    return call;
  }
  case Bytes::DataView:
    return TypedArrayChecker(this->state).checkDataViewMethod(call, member);
  case Bytes::TextEncoder:
  case Bytes::TextDecoder:
  case Bytes::FatalTextDecoder:
    return BufferChecker(this->state).checkTextMethod(call, member);
  default:
    break;
  }
  if (type->isBuffer()) {
    if (Expr *result = BufferChecker(this->state).checkBufferMethod(call, member)) {
      return result;
    }
  }

  return TypedArrayChecker(this->state).checkTypedArrayMethod(call, member, expected);
}

}
