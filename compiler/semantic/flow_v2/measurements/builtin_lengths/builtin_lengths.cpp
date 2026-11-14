#include "compiler/semantic/flow_v2/measurements/builtin_lengths/builtin_lengths.h"
#include "compiler/program/ast/await_expr/await_expr.h"
#include "compiler/program/library/bytes/encoding_name_table/encoding_name_table.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "compiler/semantic/flow/children_first/children_first.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include "llvm/Support/MathExtras.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;

namespace tz {

namespace {

constexpr int64_t kFractionLength = 2;
constexpr char16_t kOneByteEnd = 0x80;
constexpr char16_t kTwoBytesEnd = 0x800;
constexpr char16_t kHighSurrogateFirst = 0xD800;
constexpr char16_t kHighSurrogateLast = 0xDBFF;
constexpr char16_t kLowSurrogateFirst = 0xDC00;
constexpr char16_t kLowSurrogateLast = 0xDFFF;
constexpr int64_t kPairBytes = 4;
constexpr int64_t kThreeBytes = 3;

}

BuiltinLengths::BuiltinLengths(SemanticState &owner)
    : state(owner) {}

bool BuiltinLengths::isSizedAllocation(const CallExpr *call) const {
  Builtin builtin = call->builtin();

  return builtin == Builtin::BytesNew || builtin == Builtin::ArrayBufferNew ||
         builtin == Builtin::BufferAllocFill;
}

const Expr *BuiltinLengths::measuredCall(const CallExpr *call) const {
  auto *callee = dyn_cast<MemberExpr>(call->callee);
  if (callee == nullptr) {
    return nullptr;
  }
  const Type *object = callee->object->type;
  bool isSingleBytes = object != nullptr && object->isTypedArray() && object->elementSize() == 1;
  bool isMeasured = isSingleBytes && call->builtin() == Builtin::BytesByteLength;

  return isMeasured ? callee->object : nullptr;
}

std::optional<Linear> BuiltinLengths::callLength(const CallExpr *call,
                                                 std::vector<Linear> &answers) const {
  Builtin builtin = call->builtin();
  if (builtin == Builtin::DecToFraction) {
    return Linear{Term::zero(), kFractionLength};
  }
  if (builtin == Builtin::BytesOf) {
    return Linear{Term::zero(), static_cast<int64_t>(call->args.size())};
  }
  if (builtin == Builtin::EmbedBytes) {
    return this->embeddedLength(call);
  }
  if (this->isSizedAllocation(call)) {
    return this->allocatedCount(answers);
  }
  if (builtin == Builtin::BufferFromString) {
    return this->utf8Length(call);
  }
  if (builtin == Builtin::BytesBuffer) {
    return this->freshBufferLength(call, answers);
  }

  return std::nullopt;
}

std::optional<Linear> BuiltinLengths::embeddedLength(const CallExpr *call) const {
  auto *content = cast<StringLitExpr>(call->args[0]);

  return Linear{Term::zero(), static_cast<int64_t>(content->value.size())};
}

std::optional<Linear> BuiltinLengths::allocatedCount(std::vector<Linear> &answers) const {
  Linear count = ChildrenFirst::takeAnswer(answers);
  if (!count.term.isZero()) {
    return std::nullopt;
  }

  return count;
}

std::optional<Linear> BuiltinLengths::freshBufferLength(const CallExpr *call,
                                                        std::vector<Linear> &answers) const {
  const auto *made = this->freshBufferSource(call);
  if (made == nullptr) {
    return std::nullopt;
  }
  Linear count = ChildrenFirst::takeAnswer(answers);
  if (!count.term.isZero()) {
    return std::nullopt;
  }
  auto elementSize = int64_t(made->type->elementSize());

  return Linear{Term::zero(), count.offset * elementSize};
}

const CallExpr *BuiltinLengths::freshBufferSource(const CallExpr *call) const {
  auto *member = dyn_cast<MemberExpr>(call->callee);
  if (call->builtin() != Builtin::BytesBuffer || member == nullptr) {
    return nullptr;
  }
  auto *made = dyn_cast<CallExpr>(member->object);
  bool isTyped = made != nullptr && made->type != nullptr && made->type->isTypedArray() &&
                 !made->type->isBuffer();
  if (!isTyped || !this->isFreshTypedArray(made)) {
    return nullptr;
  }

  return made;
}

bool BuiltinLengths::isFreshTypedArray(const CallExpr *made) const {
  Builtin builtin = made->builtin();

  return builtin == Builtin::BytesNew || builtin == Builtin::BytesFromArray ||
         builtin == Builtin::BytesFromBytes || builtin == Builtin::BytesOf;
}

std::optional<Linear> BuiltinLengths::utf8Length(const CallExpr *call) const {
  auto *text = dyn_cast<StringLitExpr>(call->args[0]);
  const Expr *encodingArgument = call->args.size() > 1 ? call->args[1] : nullptr;
  auto *encoding = dyn_cast_or_null<StringLitExpr>(encodingArgument);
  bool isUtf8 =
      encoding == nullptr || EncodingNameTable::isExactUtf8Name(ToUtf8::toUtf8(encoding->value));
  if (text == nullptr || !isUtf8) {
    return std::nullopt;
  }
  int64_t size = this->utf8Size(text->value);

  return Linear{Term::zero(), size};
}

int64_t BuiltinLengths::utf8Size(const std::u16string &text) const {
  int64_t size = 0;
  size_t index = 0;
  while (index < text.size()) {
    bool isPair = this->isSurrogatePair(text, index);
    size += isPair ? kPairBytes : this->utf8UnitSize(text[index]);
    index += isPair ? 2 : 1;
  }

  return size;
}

bool BuiltinLengths::isSurrogatePair(const std::u16string &text, size_t index) const {
  char16_t unit = text[index];
  bool isHigh = unit >= kHighSurrogateFirst && unit <= kHighSurrogateLast;
  if (!isHigh || index + 1 >= text.size()) {
    return false;
  }
  char16_t next = text[index + 1];

  return next >= kLowSurrogateFirst && next <= kLowSurrogateLast;
}

int64_t BuiltinLengths::utf8UnitSize(char16_t unit) const {
  if (unit < kOneByteEnd) {
    return 1;
  }
  if (unit < kTwoBytesEnd) {
    return 2;
  }

  return kThreeBytes;
}

const Expr *BuiltinLengths::sameLengthSource(const Expr *object) const {
  const Expr *source = object;
  const auto *inner = this->sameLengthInner(source);
  while (inner != nullptr) {
    source = inner;
    inner = this->sameLengthInner(source);
  }

  return source;
}

const Expr *BuiltinLengths::sameLengthInner(const Expr *object) const {
  auto *awaited = dyn_cast<AwaitExpr>(object);
  auto *call = dyn_cast<CallExpr>(object);
  if (awaited != nullptr) {
    return this->singleAllArgument(awaited);
  }
  if (call != nullptr) {
    return this->copiedSource(call);
  }

  return nullptr;
}

const Expr *BuiltinLengths::singleAllArgument(const AwaitExpr *awaited) const {
  auto *all = dyn_cast<CallExpr>(awaited->value);
  bool isSingle = all != nullptr && all->builtin() == Builtin::PromiseAll && all->args.size() == 1;

  return isSingle ? all->args[0] : nullptr;
}

const Expr *BuiltinLengths::copiedSource(const CallExpr *call) const {
  bool isFromArray = call->builtin() == Builtin::BytesFromArray;
  bool isFromBytes = call->builtin() == Builtin::BytesFromBytes;

  return isFromArray || isFromBytes ? call->args[0] : nullptr;
}

}
