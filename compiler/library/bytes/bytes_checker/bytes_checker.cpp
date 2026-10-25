#include "compiler/library/bytes/bytes_checker/bytes_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/expr/call_checker/call_checker.h"
#include "compiler/check/expr/argument_checker/argument_checker.h"
#include "compiler/check/expr/member_checker/member_checker.h"
#include "compiler/declare/constant_folder/constant_folder.h"
#include "compiler/check/flow/index_proofs/index_proofs.h"
#include "compiler/library/bytes/byte_proofs/byte_proofs.h"
#include "compiler/library/bytes/typed_array_checker/typed_array_checker.h"
#include "compiler/library/bytes/buffer_checker/buffer_checker.h"
#include "compiler/library/process/process_checker/process_checker.h"
#include "compiler/library/bytes/bytes_check_helpers/bytes_check_helpers.h"
#include "runtime/library/bytes/encodings/encodings.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/MathExtras.h"
#include <limits>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::dyn_cast_or_null;

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
};

std::string utf8(const std::u16string &text) {
  std::string out;
  llvm::convertUTF16ToUTF8String(
      llvm::ArrayRef<llvm::UTF16>(reinterpret_cast<const llvm::UTF16 *>(text.data()), text.size()),
      out);

  return out;
}

// Text known when compiling: a literal, or a template without values.
std::optional<std::u16string> literalText(const Expr *e) {
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

std::string rangeText(int64_t lo, int64_t hi) {
  return std::to_string(lo) + " to " + std::to_string(hi);
}

Range exact(int64_t v) { return {v, v}; }

bool known(const Range &r) { return r.lo && r.hi; }

// The smallest 2^n - 1 at or above v >= 0.
int64_t allOnes(int64_t v) {
  int64_t m = 0;
  while (m < v) {
    m = m * 2 + 1;
  }

  return m;
}

int64_t floorShift(int64_t v, int64_t n) {
  return v >= 0 ? v >> n : -((-v + (int64_t(1) << n) - 1) >> n);
}

const llvm::StringLiteral kTypedMethods[] = {
    "at",          "copyWithin", "entries",  "every",         "fill",    "filter",
    "find",        "findIndex",  "findLast", "findLastIndex", "forEach", "includes",
    "indexOf",     "join",       "keys",     "lastIndexOf",   "map",     "reduce",
    "reduceRight", "reverse",    "set",      "slice",         "some",    "sort",
    "subarray",    "toReversed", "toSorted", "toString",      "values",  "with"};

} // namespace

BytesChecker::BytesChecker(Sema &sema) : sema_(sema) {}

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
  index->index = sema_.memberChecker().checkPosition(index->index);
  if (!type->isTypedArray()) {
    sema_.diags_.error(index->loc(), "type '" + type->name() + "' cannot be indexed; " +
                                   (type->bytes() == Bytes::DataView
                                        ? "read its bytes with getUint8(i) and the like"
                                    : type->bytes() == Bytes::ArrayBuffer
                                        ? "read its bytes through a view, e.g. 'new "
                                          "Uint8Array(buffer)[i]'"
                                        : "it holds no elements"));
    return index;
  }
  if (index->index->type->isError()) {
    return index;
  }
  // Proved in range: a plain read. Otherwise JavaScript yields undefined.
  bool inRange = sema_.indexProofs().provesInBounds(index->object, index->index, /*allowEnd=*/false);
  index->mayBeOutOfRange = !inRange;
  index->type = inRange ? type->elementType() : Type::optional(type->elementType());

  return index;
}

void BytesChecker::checkBytesStore(IndexExpr *index, const Expr *value, std::optional<BinaryOp> op,
                           llvm::SMLoc loc) {
  const Type *target = index->object->type;
  if (index->index->type->isError() || (value && value->type->isError())) {
    return;
  }
  std::string object = valueText(index->object), position = valueText(index->index);
  if (!sema_.indexProofs().provesInBounds(index->object, index->index, /*allowEnd=*/false)) {
    std::optional<int64_t> at = sema_.constantFolder().intConstant(index->index);
    std::string check = at ? object + ".length > " + std::to_string(*at)
                           : position + " >= 0 && " + position + " < " + object + ".length";
    sema_.diags_.error(index->loc(), "cannot prove that the index is within the " + target->name() +
                                   " (0 <= index < length): it never grows, and JavaScript would "
                                   "drop the write; check the index first, e.g. 'if (" +
                                   check + ")'");
    return;
  }
  std::string what = "an element of '" + target->name().str() + "'";
  if (!op) {
    sema_.byteProofs().checkFits(value, target, what);
    return;
  }
  // `u[i] op= v` stores `u[i] op v`, and `u[i]++` stores u[i] + 1.
  int64_t lo, hi;
  if (!target->elementRange(lo, hi) || target->bytes() == Bytes::Uint8Clamped ||
      target->bytes() == Bytes::Int32) {
    return;
  }
  Range result = operate(*op, Range{lo, hi}, value ? sema_.byteProofs().valueRange(value) : exact(1),
                         target->elementType()->isInt());
  if (result.lo && result.hi && *result.lo >= lo && *result.hi <= hi) {
    return;
  }
  std::string element = object + "[" + position + "]";
  std::string written = value ? element + " " + opSpelling(*op) + " " + valueText(value)
                              : element + (*op == BinaryOp::Add ? " + 1" : " - 1");
  sema_.diags_.error(loc, "cannot prove that the result fits in " + what + " (" + rangeText(lo, hi) +
                        "), and JavaScript would cut it without a word; cut it explicitly: '" +
                        element + " = " + wrapHint(target->bytes(), "(" + written + ")") + "'");
}

Expr *BytesChecker::checkBytesMember(MemberExpr *member) {
  const Type *type = member->object->type;
  llvm::StringRef name = member->name;
  auto read = [&](Builtin builtin, const Type *result, std::vector<Expr *> args = {}) {
    return sema_.processChecker().builtinCall(member->loc(), member, builtin, std::move(args), result);
  };
  // A constant, after the object for what evaluating it does.
  auto constant = [&](Expr *value) -> Expr * {
    if (isa<NameExpr>(member->object)) {
      return value;
    }
    auto *seq = sema_.ctx_.make<SequenceExpr>(member->loc(), std::vector<Expr *>{member->object, value});
    seq->type = value->type;

    return seq;
  };
  auto intLit = [&](int64_t v) {
    auto *lit = sema_.ctx_.make<IntLitExpr>(member->loc(), uint64_t(v));
    lit->type = Type::intTy();

    return lit;
  };
  auto boolLit = [&](bool v) {
    auto *lit = sema_.ctx_.make<BoolLitExpr>(member->loc(), v);
    lit->type = Type::boolTy();

    return lit;
  };
  switch (type->bytes()) {
  case Bytes::ArrayBuffer:
    if (name == "byteLength") {
      member->type = Type::intTy(); // read by CodeGen, and a length for the proofs
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
      auto *lit = sema_.ctx_.make<StringLitExpr>(member->loc(), u"utf-8");
      lit->type = Type::string();
      return constant(lit);
    }
    break;
  case Bytes::TextDecoder:
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
      sema_.diags_.error(member->loc(), "'" + name +
                                      "' of a Buffer is deprecated in node and not "
                                      "supported; use '" +
                                      (name == "parent" ? "buffer" : "byteOffset") + "'");
      return member;
    }
    if (llvm::is_contained(kTypedMethods, name)) {
      sema_.diags_.error(member->loc(), "property '" + name + "' does not exist on type '" +
                                      type->name() + "'; methods must be called");
      return member;
    }
    break;
  }
  sema_.diags_.error(member->loc(),
               "property '" + name + "' does not exist on type '" + type->name() + "'");

  return member;
}

// The properties lie at the same place in every view (TzBytes), and the
// length of an ArrayBuffer where a view keeps its length: CodeGen reads them
// the same way whatever the member is, and finds the size of an element by
// the member's class number where the members' differ.
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
    sema_.diags_.error(member->loc(), "property '" + name + "' of '" + type->name() +
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
    sema_.diags_.error(member->loc(), message);
    return member;
  }
  if (name == "length") {
    member->type = Type::intTy(); // read by CodeGen, as of each member
    return member;
  }
  if (name == "buffer") {
    return sema_.processChecker().builtinCall(member->loc(), member, Builtin::BytesBuffer, {},
                       Type::bytesType(Bytes::ArrayBuffer));
  }

  return sema_.processChecker().builtinCall(member->loc(), member,
                     name == "byteLength" ? Builtin::BytesByteLength : Builtin::BytesByteOffset, {},
                     Type::intTy());
}

Expr *BytesChecker::checkBytesStaticMember(MemberExpr *member) {
  auto *object = dyn_cast<NameExpr>(member->object);
  if (!object || !sema_.callChecker().isGlobalName(object, object->name)) {
    return nullptr;
  }
  const Type *type = bytesTypeNamed(object->name);
  if (!type) {
    return nullptr;
  }
  member->type = Type::error();
  if (type->isTypedArray() && member->name == "BYTES_PER_ELEMENT") {
    auto *lit = sema_.ctx_.make<IntLitExpr>(member->loc(), type->elementSize());
    lit->type = Type::intTy();
    return lit;
  }
  // The size of the pool of Buffer.allocUnsafe in node, which tz has none of.
  if (type->isBuffer() && member->name == "poolSize") {
    auto *lit = sema_.ctx_.make<IntLitExpr>(member->loc(), 8192);
    lit->type = Type::intTy();
    return lit;
  }
  sema_.diags_.error(member->loc(), "'" + object->name + "." + member->name +
                                  "' can be used only in a call, e.g. '" + object->name + "." +
                                  member->name + "(...)'");

  return member;
}

Expr *BytesChecker::bytesToString(Expr *expr) {
  const Type *type = expr->type;
  if (type->isTypedArray()) {
    auto *callee = sema_.ctx_.make<MemberExpr>(expr->loc(), expr, "toString");
    if (type->isBuffer()) {
      auto *encoding = sema_.ctx_.make<IntLitExpr>(expr->loc(), Utf8);
      encoding->type = Type::intTy();
      return sema_.processChecker().builtinCall(expr->loc(), callee, Builtin::BufferToString, {encoding}, Type::string());
    }
    return sema_.processChecker().builtinCall(expr->loc(), callee, Builtin::BytesJoin, {}, Type::string());
  }
  sema_.diags_.error(expr->loc(),
               "a value of type '" + type->name() +
                   "' cannot be converted to a string: JavaScript would give '[object " +
                   type->name() + "]'; print it with console.log" +
                   (type->bytes() == Bytes::ArrayBuffer
                        ? ", or the text of its bytes with 'new TextDecoder().decode(buffer)'"
                        : ""));

  return expr;
}

Expr *BytesChecker::checkBufferEncoding(Expr *arg) {
  if (std::optional<std::u16string> text = literalText(arg)) {
    std::optional<int> encoding = encodingNamed(utf8(*text));
    if (!encoding) {
      sema_.diags_.error(arg->loc(), "unknown encoding '" + utf8(*text) +
                                   "' (node throws a TypeError); Buffer knows utf8, utf16le, "
                                   "latin1, ascii, base64, base64url and hex");
      arg->type = Type::error();
      return arg;
    }
    auto *lit = sema_.ctx_.make<IntLitExpr>(arg->loc(), uint64_t(*encoding));
    lit->type = Type::intTy();
    return lit;
  }

  // Checked when the program runs: an unknown one is a TypeError.
  return sema_.argumentChecker().checkArg(arg, Type::string());
}

Expr *BytesChecker::checkBytesMethod(CallExpr *call, MemberExpr *member, const Type *expected) {
  const Type *type = member->object->type;
  call->type = Type::error();
  if (sema_.callChecker().rejectSpread(call)) {
    return call;
  }
  switch (type->bytes()) {
  case Bytes::ArrayBuffer: {
    llvm::StringRef name = member->name;
    if (name == "slice") {
      if (!sema_.argumentChecker().checkArity(call, member->loc(), 0, 2)) {
        return call;
      }
      for (Expr *&arg : call->args) {
        arg = sema_.memberChecker().checkPosition(arg);
      }
      call->builtin = Builtin::ArrayBufferSlice;
      call->type = type;
      return call;
    }
    for (Expr *&arg : call->args) {
      arg = sema_.exprChecker().checkExpr(arg);
    }
    if (name == "resize" || name == "transfer" || name == "transferToFixedLength") {
      sema_.diags_.error(member->loc(), "'" + name +
                                      "' is not supported yet: an ArrayBuffer keeps its "
                                      "size and its memory (docs/bytes.md, section 7)");
    } else {
      sema_.diags_.error(member->loc(), "property '" + name + "' does not exist on type 'ArrayBuffer'");
    }
    return call;
  }
  case Bytes::DataView:
    return sema_.typedArrayChecker().checkDataViewMethod(call, member);
  case Bytes::TextEncoder:
  case Bytes::TextDecoder:
    return sema_.bufferChecker().checkTextMethod(call, member);
  default:
    break;
  }
  if (type->isBuffer()) {
    if (Expr *result = sema_.bufferChecker().checkBufferMethod(call, member)) {
      return result;
    }
  }

  return sema_.typedArrayChecker().checkTypedArrayMethod(call, member, expected);
}

} // namespace tz
