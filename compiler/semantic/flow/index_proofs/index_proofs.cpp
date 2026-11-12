#include "compiler/semantic/flow/index_proofs/index_proofs.h"
#include "compiler/program/ast/await_expr/await_expr.h"
#include "compiler/program/library/builtin_names/builtin_property_table/builtin_property_table.h"
#include "compiler/semantic/declare/constant_folder/constant_folder.h"
#include "compiler/semantic/flow/children_first/children_first.h"
#include "compiler/semantic/generics/generic_checker/generic_checker.h"
#include "compiler/semantic/library/bytes/byte_proofs/byte_proofs.h"
#include "compiler/semantic/library/bytes/bytes_check_helpers/bytes_check_helpers.h"
#include "compiler/semantic/library/tz_modules/library_calls/library_calls.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "llvm/ADT/STLExtras.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

namespace {

constexpr uint64_t kMaxConstant = uint64_t(1) << 40;
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

IndexProofs::IndexProofs(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->byteProofs = &all.byteProofs;
  this->libraryCalls = &all.libraryCalls;
  this->constantFolder = &all.constantFolder;
  this->genericChecker = &all.genericChecker;
}

const VarSymbol *IndexProofs::variableOf(const Expr *expr) const {
  const Expr *inner = expr;
  auto *conversion = dyn_cast<CastExpr>(inner);
  while (conversion != nullptr) {
    if (!conversion->isImplicit()) {
      return nullptr;
    }
    inner = conversion->operand;
    conversion = dyn_cast<CastExpr>(inner);
  }
  auto *name = dyn_cast<NameExpr>(inner);

  return name != nullptr ? name->var() : nullptr;
}

std::optional<Linear> IndexProofs::linear(const Expr *expr) const {
  return this->proofAnswer(ProofNode{.ask = ProofAsk::LINEAR, .expr = expr});
}

std::optional<Linear> IndexProofs::lengthOf(const Expr *expr) const {
  return this->proofAnswer(ProofNode{.ask = ProofAsk::LENGTH, .expr = expr});
}

std::optional<Linear> IndexProofs::proofAnswer(ProofNode root) const {
  auto answers = std::vector<Linear>();
  for (const ProofNode &node : ChildrenFirst::order(root, this, &IndexProofs::proofParts)) {
    auto answer = this->nodeAnswer(node, answers);
    if (!answer.has_value()) {
      return std::nullopt;
    }
    answers.push_back(*answer);
  }

  return answers.back();
}

NodeChildren<ProofNode> IndexProofs::proofParts(ProofNode node) const {
  if (node.ask == ProofAsk::LINEAR) {
    return this->linearParts(node.expr);
  }

  return this->lengthParts(node.expr);
}

std::optional<Linear> IndexProofs::nodeAnswer(const ProofNode &node,
                                              std::vector<Linear> &answers) const {
  if (node.ask == ProofAsk::LINEAR) {
    return this->nodeLinear(node.expr, answers);
  }

  return this->nodeLength(node.expr, answers);
}

NodeChildren<ProofNode> IndexProofs::linearParts(const Expr *expr) const {
  auto *conversion = dyn_cast<CastExpr>(expr);
  auto *binary = dyn_cast<BinaryExpr>(expr);
  const auto *measured = this->measuredObject(expr);
  if (conversion != nullptr && this->isIntWidening(conversion)) {
    return {ProofNode{.ask = ProofAsk::LINEAR, .expr = conversion->operand}};
  }
  if (binary != nullptr && this->isLinearStep(binary)) {
    return {
        ProofNode{.ask = ProofAsk::LINEAR, .expr = binary->lhs},
        ProofNode{.ask = ProofAsk::LINEAR, .expr = binary->rhs},
    };
  }
  if (measured != nullptr) {
    return {ProofNode{.ask = ProofAsk::LENGTH, .expr = measured}};
  }

  return {};
}

NodeChildren<ProofNode> IndexProofs::lengthParts(const Expr *expr) const {
  const auto *source = this->sameLengthSource(expr);
  auto *call = dyn_cast<CallExpr>(source);
  if (call == nullptr) {
    return {};
  }
  if (this->isSizedAllocation(call)) {
    return {ProofNode{.ask = ProofAsk::LINEAR, .expr = call->args[0]}};
  }
  const auto *made = this->freshBufferSource(call);
  if (made == nullptr) {
    return {};
  }

  return {ProofNode{.ask = ProofAsk::LENGTH, .expr = made}};
}

bool IndexProofs::isSizedAllocation(const CallExpr *call) const {
  Builtin builtin = call->builtin();

  return builtin == Builtin::BytesNew || builtin == Builtin::ArrayBufferNew ||
         builtin == Builtin::BufferAllocFill;
}

bool IndexProofs::isRawBuffer(const Type *type) const {
  return type != nullptr &&
         (type->bytes() == Type::Bytes::ArrayBuffer || type->bytes() == Type::Bytes::DataView);
}

const Expr *IndexProofs::measuredObject(const Expr *expr) const {
  auto *member = dyn_cast<MemberExpr>(expr);
  auto *call = dyn_cast<CallExpr>(expr);
  if (member != nullptr) {
    return this->measuredMember(member);
  }
  if (call != nullptr) {
    return this->measuredCall(call);
  }

  return nullptr;
}

const Expr *IndexProofs::measuredMember(const MemberExpr *member) const {
  bool isBuffer = this->isRawBuffer(member->object->type);
  bool isLength = BuiltinPropertyTable::isName(member->name, BuiltinProperty::LENGTH);
  bool isByteLength =
      BuiltinPropertyTable::isName(member->name, BuiltinProperty::BYTE_LENGTH) && isBuffer;
  bool isMeasured = (isLength || isByteLength) && member->type->isInt();

  return isMeasured ? member->object : nullptr;
}

const Expr *IndexProofs::measuredCall(const CallExpr *call) const {
  auto *callee = dyn_cast<MemberExpr>(call->callee);
  if (callee == nullptr) {
    return nullptr;
  }
  const Type *object = callee->object->type;
  bool isSingleBytes = object != nullptr && object->isTypedArray() && object->elementSize() == 1;
  bool isMeasured = isSingleBytes && call->builtin() == Builtin::BytesByteLength;

  return isMeasured ? callee->object : nullptr;
}

bool IndexProofs::isIntWidening(const CastExpr *conversion) const {
  return conversion->isImplicit() && conversion->operand->type->isInt() &&
         conversion->type->isInteger();
}

bool IndexProofs::isLinearStep(const BinaryExpr *binary) const {
  bool isAddOrSub = binary->op == BinaryOp::Add || binary->op == BinaryOp::Sub;

  return isAddOrSub && binary->type->isInteger();
}

std::optional<Linear> IndexProofs::nodeLinear(const Expr *expr,
                                              std::vector<Linear> &answers) const {
  auto *literal = dyn_cast<IntLitExpr>(expr);
  auto *unary = dyn_cast<UnaryExpr>(expr);
  auto *name = dyn_cast<NameExpr>(expr);
  auto *conversion = dyn_cast<CastExpr>(expr);
  auto *binary = dyn_cast<BinaryExpr>(expr);
  bool isMeasuring = isa<MemberExpr>(expr) || isa<CallExpr>(expr);
  if (literal != nullptr) {
    return this->literalLinear(literal);
  }
  if (unary != nullptr) {
    return this->negatedLinear(unary);
  }
  if (name != nullptr) {
    return this->nameLinear(name);
  }
  if (isMeasuring) {
    return this->measuredLinear(expr, answers);
  }
  if (conversion != nullptr) {
    return this->wideningLinear(conversion, answers);
  }
  if (binary != nullptr) {
    return this->stepLinear(binary, answers);
  }

  return std::nullopt;
}

std::optional<Linear> IndexProofs::literalLinear(const IntLitExpr *literal) const {
  if (!literal->type->isInteger() || literal->value > kMaxConstant) {
    return std::nullopt;
  }

  return Linear{Term::zero(), static_cast<int64_t>(literal->value)};
}

std::optional<Linear> IndexProofs::negatedLinear(const UnaryExpr *unary) const {
  auto *literal = dyn_cast<IntLitExpr>(unary->operand);
  bool isNegatedLiteral = unary->op == UnaryOp::Neg && literal != nullptr;
  if (!isNegatedLiteral || !unary->type->isInteger() || literal->value > kMaxConstant) {
    return std::nullopt;
  }

  return Linear{Term::zero(), -static_cast<int64_t>(literal->value)};
}

std::optional<Linear> IndexProofs::nameLinear(const NameExpr *name) const {
  const VarSymbol *variable = name->var();
  if (variable == nullptr || !variable->type->isInt()) {
    return std::nullopt;
  }

  return Linear{Term::of(variable), 0};
}

std::optional<Linear> IndexProofs::measuredLinear(const Expr *expr,
                                                  std::vector<Linear> &answers) const {
  if (this->measuredObject(expr) == nullptr) {
    return std::nullopt;
  }

  return ChildrenFirst::takeAnswer(answers);
}

std::optional<Linear> IndexProofs::wideningLinear(const CastExpr *conversion,
                                                  std::vector<Linear> &answers) const {
  if (!this->isIntWidening(conversion)) {
    return std::nullopt;
  }

  return ChildrenFirst::takeAnswer(answers);
}

std::optional<Linear> IndexProofs::stepLinear(const BinaryExpr *binary,
                                              std::vector<Linear> &answers) const {
  if (!this->isLinearStep(binary)) {
    return std::nullopt;
  }
  Linear right = ChildrenFirst::takeAnswer(answers);
  Linear left = ChildrenFirst::takeAnswer(answers);

  return this->linearSum(binary, left, right);
}

std::optional<Linear> IndexProofs::linearSum(const BinaryExpr *binary, const Linear &left,
                                             const Linear &right) const {
  auto sum = this->termSum(binary, left, right);
  if (!sum.has_value()) {
    return std::nullopt;
  }
  bool isSmall = sum->offset <= int64_t(kMaxConstant) && sum->offset >= -int64_t(kMaxConstant);
  if (!isSmall) {
    return std::nullopt;
  }
  if (binary->type->isInt() && !this->isFittingInt(*sum)) {
    return std::nullopt;
  }

  return sum;
}

std::optional<Linear> IndexProofs::termSum(const BinaryExpr *binary, const Linear &left,
                                           const Linear &right) const {
  bool isAdd = binary->op == BinaryOp::Add;
  if (right.term.isZero()) {
    int64_t offset = isAdd ? left.offset + right.offset : left.offset - right.offset;

    return Linear{left.term, offset};
  }
  if (isAdd && left.term.isZero()) {
    return Linear{right.term, left.offset + right.offset};
  }
  auto both = Term::sum(left.term, right.term);
  if (isAdd && both.has_value()) {
    return Linear{*both, left.offset + right.offset};
  }

  return std::nullopt;
}

bool IndexProofs::isFittingInt(const Linear &sum) const {
  const auto &facts = this->state.getFlowState().getFacts();
  if (sum.term.isZero()) {
    return true;
  }
  if (sum.offset > 0) {
    int64_t limit = kIntMax - (sum.offset - 1);

    return facts.canProve(sum.term, Term::zero(), limit);
  }
  if (sum.offset < 0) {
    int64_t limit = Facts::saturatingSub(sum.offset + 1, kIntMin);

    return facts.canProve(Term::zero(), sum.term, limit);
  }

  return true;
}

std::optional<Linear> IndexProofs::nodeLength(const Expr *expr,
                                              std::vector<Linear> &answers) const {
  const auto *object = this->sameLengthSource(expr);
  auto *text = dyn_cast<StringLitExpr>(object);
  auto *list = dyn_cast<ArrayLitExpr>(object);
  const auto *variable = this->variableOf(object);
  auto *call = dyn_cast<CallExpr>(object);
  if (text != nullptr) {
    return Linear{Term::zero(), static_cast<int64_t>(text->value.size())};
  }
  if (list != nullptr) {
    return this->listLiteralLength(list);
  }
  if (variable != nullptr) {
    return this->variableLength(variable);
  }
  if (call != nullptr) {
    return this->callLength(call, answers);
  }

  return std::nullopt;
}

std::optional<Linear> IndexProofs::listLiteralLength(const ArrayLitExpr *list) const {
  if (this->hasSpreadElement(list)) {
    return std::nullopt;
  }

  return Linear{Term::zero(), static_cast<int64_t>(list->elements.size())};
}

bool IndexProofs::hasSpreadElement(const ArrayLitExpr *list) const {
  for (const ArrayLitExpr::Element &element : list->elements) {
    if (element.spread) {
      return true;
    }
  }

  return false;
}

std::optional<Linear> IndexProofs::variableLength(const VarSymbol *variable) const {
  auto fixed = this->state.getFlowState().findFixedLength(variable);
  if (fixed.has_value()) {
    return Linear{Term::zero(), *fixed};
  }
  const auto *declared = variable->type->nonOptional();
  const auto *type = this->genericChecker->apparentType(declared);
  bool isMeasured = type->isString() || type->isArray() || type->isByteView() ||
                    type->bytes() == Type::Bytes::ArrayBuffer;
  if (!isMeasured) {
    return std::nullopt;
  }

  return Linear{Term::lengthOf(variable), 0};
}

std::optional<Linear> IndexProofs::callLength(const CallExpr *call,
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

std::optional<Linear> IndexProofs::embeddedLength(const CallExpr *call) const {
  auto *content = cast<StringLitExpr>(call->args[0]);

  return Linear{Term::zero(), static_cast<int64_t>(content->value.size())};
}

std::optional<Linear> IndexProofs::allocatedCount(std::vector<Linear> &answers) const {
  Linear count = ChildrenFirst::takeAnswer(answers);
  if (!count.term.isZero()) {
    return std::nullopt;
  }

  return count;
}

std::optional<Linear> IndexProofs::freshBufferLength(const CallExpr *call,
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

const CallExpr *IndexProofs::freshBufferSource(const CallExpr *call) const {
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

bool IndexProofs::isFreshTypedArray(const CallExpr *made) const {
  Builtin builtin = made->builtin();

  return builtin == Builtin::BytesNew || builtin == Builtin::BytesFromArray ||
         builtin == Builtin::BytesFromBytes || builtin == Builtin::BytesOf;
}

std::optional<Linear> IndexProofs::utf8Length(const CallExpr *call) const {
  auto *text = dyn_cast<StringLitExpr>(call->args[0]);
  const Expr *encodingArgument = call->args.size() > 1 ? call->args[1] : nullptr;
  auto *encoding = dyn_cast_or_null<StringLitExpr>(encodingArgument);
  bool isUtf8 = encoding == nullptr || encoding->value == u"utf8" || encoding->value == u"utf-8";
  if (text == nullptr || !isUtf8) {
    return std::nullopt;
  }
  int64_t size = this->utf8Size(text->value);

  return Linear{Term::zero(), size};
}

int64_t IndexProofs::utf8Size(const std::u16string &text) const {
  int64_t size = 0;
  size_t index = 0;
  while (index < text.size()) {
    bool isPair = this->isSurrogatePair(text, index);
    size += isPair ? kPairBytes : this->utf8UnitSize(text[index]);
    index += isPair ? 2 : 1;
  }

  return size;
}

bool IndexProofs::isSurrogatePair(const std::u16string &text, size_t index) const {
  char16_t unit = text[index];
  bool isHigh = unit >= kHighSurrogateFirst && unit <= kHighSurrogateLast;
  if (!isHigh || index + 1 >= text.size()) {
    return false;
  }
  char16_t next = text[index + 1];

  return next >= kLowSurrogateFirst && next <= kLowSurrogateLast;
}

int64_t IndexProofs::utf8UnitSize(char16_t unit) const {
  if (unit < kOneByteEnd) {
    return 1;
  }
  if (unit < kTwoBytesEnd) {
    return 2;
  }

  return kThreeBytes;
}

bool IndexProofs::canProveInBounds(const Expr *object, const Expr *index) const {
  return this->canProveIndexWithin(object, index, 0);
}

bool IndexProofs::canProveInBoundsOrEnd(const Expr *object, const Expr *index) const {
  return this->canProveIndexWithin(object, index, 1);
}

bool IndexProofs::canProveIndexWithin(const Expr *object, const Expr *index,
                                      int64_t endExtra) const {
  auto at = this->linear(index);
  auto length = this->lengthOf(object);
  if (!length.has_value()) {
    return false;
  }
  if (at.has_value() && this->canProveLinearIndex(*at, *length, endExtra)) {
    return true;
  }
  auto range = this->byteProofs->valueRange(index);
  bool isNonNegative = range.lo.has_value() && range.hi.has_value() && *range.lo >= 0;
  if (!isNonNegative) {
    return false;
  }
  int64_t room = length->offset - *range.hi + endExtra;

  return this->state.getFlowState().getFacts().canProve(Term::zero(), length->term, room);
}

bool IndexProofs::canProveLinearIndex(const Linear &at, const Linear &length,
                                      int64_t endExtra) const {
  const auto &facts = this->state.getFlowState().getFacts();
  bool isNonNegative = facts.canProve(Term::zero(), at.term, at.offset + 1);
  int64_t room = length.offset - at.offset + endExtra;

  return isNonNegative && facts.canProve(at.term, length.term, room);
}

bool IndexProofs::canProveAtInBounds(const Expr *object, const Expr *index) const {
  auto at = this->linear(index);
  auto length = this->lengthOf(object);
  bool isFromEnd = at.has_value() && length.has_value() && at->term.isZero() && at->offset < 0;
  if (isFromEnd) {
    int64_t room = length->offset + at->offset + 1;

    return this->state.getFlowState().getFacts().canProve(Term::zero(), length->term, room);
  }

  return this->canProveInBounds(object, index);
}

bool IndexProofs::canProveNonNegative(const Expr *expr) const {
  auto value = this->linear(expr);
  if (!value.has_value()) {
    return false;
  }

  return this->state.getFlowState().getFacts().canProve(Term::zero(), value->term,
                                                        value->offset + 1);
}

bool IndexProofs::canProveNotEmpty(const Expr *object) const {
  auto length = this->lengthOf(object);
  if (!length.has_value()) {
    return false;
  }

  return this->state.getFlowState().getFacts().canProve(Term::zero(), length->term, length->offset);
}

bool IndexProofs::canProveBetween(const Expr *expr, int64_t low,
                                  std::optional<int64_t> high) const {
  for (const Expr *branch : this->provedBranches(expr)) {
    if (!this->canProveValueBetween(branch, low, high)) {
      return false;
    }
  }

  return true;
}

std::vector<const Expr *> IndexProofs::provedBranches(const Expr *expr) const {
  auto branches = std::vector<const Expr *>();
  auto stack = std::vector<const Expr *>{expr};
  while (!stack.empty()) {
    const auto *next = this->withoutWidening(stack.back());
    stack.pop_back();
    auto *choice = dyn_cast<ConditionalExpr>(next);
    if (choice != nullptr) {
      stack.push_back(choice->elseExpr);
      stack.push_back(choice->thenExpr);
    } else {
      branches.push_back(next);
    }
  }

  return branches;
}

const Expr *IndexProofs::withoutWidening(const Expr *expr) const {
  const Expr *inner = expr;
  auto *conversion = dyn_cast<CastExpr>(inner);
  while (conversion != nullptr && this->isValueWidening(conversion)) {
    inner = conversion->operand;
    conversion = dyn_cast<CastExpr>(inner);
  }

  return inner;
}

bool IndexProofs::isValueWidening(const CastExpr *conversion) const {
  const Type *from = conversion->operand->type;
  if (!conversion->isImplicit() || from == nullptr || !from->isInteger()) {
    return false;
  }
  const Type *to = conversion->type;
  bool isWiderInteger = to->isInteger() && to->bitWidth() >= from->bitWidth();

  return to->isFloat() || isWiderInteger;
}

bool IndexProofs::canProveValueBetween(const Expr *expr, int64_t low,
                                       std::optional<int64_t> high) const {
  if (expr->type == nullptr || !expr->type->nonOptional()->isInteger()) {
    return false;
  }
  auto value = this->linear(expr);
  auto range = this->byteProofs->valueRange(expr);
  bool isRangeLow = range.lo.has_value() && *range.lo >= low;
  bool isLower = this->isLinearAtLeast(value, low) || isRangeLow;
  if (!high.has_value()) {
    return isLower;
  }
  bool isRangeHigh = range.hi.has_value() && *range.hi <= *high;
  bool isUpper = this->isLinearAtMost(value, *high) || isRangeHigh;

  return isLower && isUpper;
}

bool IndexProofs::isLinearAtLeast(const std::optional<Linear> &value, int64_t low) const {
  if (!value.has_value()) {
    return false;
  }
  if (value->term.isZero()) {
    return value->offset >= low;
  }

  return this->state.getFlowState().getFacts().canProve(Term::zero(), value->term,
                                                        value->offset - low + 1);
}

bool IndexProofs::isLinearAtMost(const std::optional<Linear> &value, int64_t high) const {
  if (!value.has_value()) {
    return false;
  }
  if (value->term.isZero()) {
    return value->offset <= high;
  }

  return this->state.getFlowState().getFacts().canProve(value->term, Term::zero(),
                                                        high - value->offset + 1);
}

bool IndexProofs::isExempt(llvm::SMLoc loc, const Expr *object) const {
  if (object != nullptr && this->isNeverObject(object)) {
    return true;
  }

  return this->state.getFlowState().isUnreachable() || this->libraryCalls->isInLibrary(loc);
}

bool IndexProofs::isNeverObject(const Expr *object) const {
  if (this->isNeverType(object->type)) {
    return true;
  }
  const VarSymbol *variable = this->variableOf(object);

  return variable != nullptr && this->isNeverType(variable->type);
}

bool IndexProofs::isNeverType(const Type *type) const {
  return type != nullptr && (type->isUndefined() || type->nonOptional()->isNever());
}

IndexProofs::ResultBetweenProven IndexProofs::requireBetween(const Expr *expr, int64_t low,
                                                             std::optional<int64_t> high,
                                                             llvm::StringRef what) {
  if (expr->type == nullptr || expr->type->isError() || this->canProveBetween(expr, low, high)) {
    return ok(expr);
  }
  if (this->isExempt(expr->loc())) {
    return err(IndexProofsRequireBetweenError{
        .code = IndexProofsRequireBetweenErrorCode::EXEMPT,
        .message = "",
    });
  }

  auto range = ErrorTextBetween::range(low, high);
  auto constant = this->constantFolder->intConstant(expr);
  if (constant.has_value() && !high.has_value() && low == 0) {
    auto message = ErrorTextBetween::negative(what, *constant);

    return err(IndexProofsRequireBetweenError{
        .code = IndexProofsRequireBetweenErrorCode::CONSTANT_NEGATIVE,
        .message = message,
    });
  }
  if (constant.has_value()) {
    auto message = ErrorTextBetween::outside(what, range, *constant);

    return err(IndexProofsRequireBetweenError{
        .code = IndexProofsRequireBetweenErrorCode::CONSTANT_OUT_OF_RANGE,
        .message = message,
    });
  }
  if (!expr->type->isInteger()) {
    auto message = ErrorTextBetween::notWhole(what, range, expr->type->name());

    return err(IndexProofsRequireBetweenError{
        .code = IndexProofsRequireBetweenErrorCode::NOT_WHOLE_NUMBER,
        .message = message,
    });
  }

  auto text = BytesCheckHelpers::valueText(expr);
  auto test = ErrorTextBetween::test(text, low, high);
  auto message = ErrorTextBetween::unproven(what, range, test);

  return err(IndexProofsRequireBetweenError{
      .code = IndexProofsRequireBetweenErrorCode::NOT_PROVEN,
      .message = message,
  });
}

bool IndexProofs::canProveLess(const Expr *left, const Expr *right) const {
  auto leftValue = this->linear(left);
  auto rightValue = this->linear(right);
  if (!leftValue.has_value() || !rightValue.has_value()) {
    return false;
  }
  if (leftValue->term.isZero() && rightValue->term.isZero()) {
    return leftValue->offset < rightValue->offset;
  }

  return this->state.getFlowState().getFacts().canProve(leftValue->term, rightValue->term,
                                                        rightValue->offset - leftValue->offset);
}

IndexProofs::ResultLessWasProven IndexProofs::requireLess(const Expr *left, const Expr *right,
                                                          llvm::StringRef what) {
  bool isUnchecked = left->type == nullptr || right->type == nullptr || left->type->isError() ||
                     right->type->isError();
  if (isUnchecked || this->canProveLess(left, right) || this->isExempt(right->loc())) {
    return ok(right);
  }

  auto leftText = BytesCheckHelpers::valueText(left);
  auto rightText = BytesCheckHelpers::valueText(right);
  auto message = ErrorTextLess::unproven(what, leftText, rightText);

  return err(IndexProofsRequireLessError{
      .code = IndexProofsRequireLessErrorCode::NOT_PROVEN,
      .message = message,
  });
}

bool IndexProofs::canProveLinearUpTo(Linear value, const Expr *object, int64_t less) const {
  auto length = this->lengthOrBufferLength(object);
  if (!length.has_value()) {
    return false;
  }
  int64_t room = length->offset - less - value.offset + 1;

  return this->state.getFlowState().getFacts().canProve(value.term, length->term, room);
}

std::optional<Linear> IndexProofs::lengthOrBufferLength(const Expr *object) const {
  auto length = this->lengthOf(object);
  if (length.has_value()) {
    return length;
  }

  return this->viewBufferLength(object);
}

std::optional<Linear> IndexProofs::viewBufferLength(const Expr *object) const {
  const auto *member = this->viewBufferMember(object);
  if (member == nullptr) {
    return std::nullopt;
  }
  int64_t size = member->object->type->elementSize();
  auto view = this->lengthOf(member->object);
  if (!view.has_value()) {
    return std::nullopt;
  }
  auto settledView = this->settled(*view);
  if (size == 1) {
    return settledView;
  }
  if (settledView.term.isZero()) {
    return Linear{Term::zero(), settledView.offset * size};
  }

  return std::nullopt;
}

const MemberExpr *IndexProofs::viewBufferMember(const Expr *object) const {
  auto *call = dyn_cast<CallExpr>(object);
  bool isBufferCall = call != nullptr && call->builtin() == Builtin::BytesBuffer;
  const Expr *owner = isBufferCall ? call->callee : object;
  auto *member = dyn_cast<MemberExpr>(owner);
  if (member == nullptr || !BuiltinPropertyTable::isName(member->name, BuiltinProperty::BUFFER)) {
    return nullptr;
  }
  const Type *view = member->object->type;
  bool isByteView = view != nullptr && view->isByteView();

  return isByteView ? member : nullptr;
}

bool IndexProofs::canProveUpTo(const Expr *value, const Expr *object, int64_t less) const {
  auto zero = Linear{Term::zero(), 0};
  if (value == nullptr) {
    return this->canProveLinearUpTo(zero, object, less);
  }
  if (!this->canProveBetween(value, 0, std::nullopt)) {
    return false;
  }
  auto at = this->linear(value);
  if (at.has_value() && this->canProveLinearUpTo(*at, object, less)) {
    return true;
  }
  auto range = this->byteProofs->valueRange(value);
  if (!range.hi.has_value()) {
    return false;
  }
  auto highest = Linear{Term::zero(), *range.hi};

  return this->canProveLinearUpTo(highest, object, less);
}

IndexProofs::ResultUpToWasProven IndexProofs::requireUpTo(const Expr *value, const Expr *object,
                                                          int64_t less, llvm::StringRef what) {
  bool isUnchecked = value != nullptr && (value->type == nullptr || value->type->isError());
  if (isUnchecked || this->canProveUpTo(value, object, less)) {
    return ok(object);
  }
  if (this->isExempt(object->loc(), object)) {
    return err(IndexProofsRequireUpToError{
        .code = IndexProofsRequireUpToErrorCode::EXEMPT,
        .message = "",
    });
  }

  auto text = this->shownValue(value);
  bool hasBytes = this->isRawBuffer(object->type);
  auto objectText = BytesCheckHelpers::valueText(object);
  auto limit = ErrorTextUpTo::limit(objectText, hasBytes, less);
  auto message = ErrorTextUpTo::unproven(what, text, limit);

  return err(IndexProofsRequireUpToError{
      .code = IndexProofsRequireUpToErrorCode::NOT_PROVEN,
      .message = message,
  });
}

std::optional<std::string> IndexProofs::shownValue(const Expr *value) const {
  if (value == nullptr) {
    return std::nullopt;
  }

  return BytesCheckHelpers::valueText(value);
}

std::optional<int64_t> IndexProofs::exactLength(const Expr *object) const {
  auto length = this->knownLength(object);

  return this->exactFrom(length);
}

std::optional<int64_t> IndexProofs::exactFrom(std::optional<Linear> length) const {
  if (!length.has_value()) {
    return std::nullopt;
  }
  auto value = this->settled(*length);
  if (!value.term.isZero()) {
    return std::nullopt;
  }

  return value.offset;
}

std::optional<Linear> IndexProofs::viewOver(const CallExpr *call,
                                            std::optional<Linear> whole) const {
  int64_t size = call->type != nullptr ? call->type->elementSize() : 1;
  auto from = this->viewStart(call);
  if (!whole.has_value() || !from.has_value() || !from->term.isZero()) {
    return std::nullopt;
  }
  if (size == 1) {
    return Linear{whole->term, whole->offset - from->offset};
  }
  auto exact = this->exactFrom(whole);
  if (!exact.has_value()) {
    return std::nullopt;
  }
  int64_t bytes = *exact - from->offset;
  if (bytes % size != 0) {
    return std::nullopt;
  }

  return Linear{Term::zero(), bytes / size};
}

std::optional<Linear> IndexProofs::viewStart(const CallExpr *call) const {
  if (call->args.size() > 1) {
    return this->linear(call->args[1]);
  }

  return Linear{Term::zero(), 0};
}

std::optional<Linear> IndexProofs::lengthAtMost(const Expr *expr) const {
  auto exact = this->lengthOf(expr);
  if (exact.has_value()) {
    return exact;
  }
  auto *call = dyn_cast<CallExpr>(expr);
  if (call == nullptr || !this->isTwoArgumentPart(call)) {
    return std::nullopt;
  }
  auto from = this->constantFolder->intConstant(call->args[0]);
  auto to = this->constantFolder->intConstant(call->args[1]);
  if (!from.has_value() || !to.has_value() || *from < 0 || *to < *from) {
    return std::nullopt;
  }

  return Linear{Term::zero(), *to - *from};
}

bool IndexProofs::isTwoArgumentPart(const CallExpr *call) const {
  bool isPart = call->builtin() == Builtin::BytesSubarray || call->builtin() == Builtin::BytesSlice;

  return isPart && call->args.size() == 2;
}

Linear IndexProofs::settled(Linear value) const {
  if (value.term.isZero()) {
    return value;
  }
  const auto &facts = this->state.getFlowState().getFacts();
  auto above = facts.bound(value.term, Term::zero());
  auto below = facts.bound(Term::zero(), value.term);
  bool isPinned = above.has_value() && below.has_value() && *above - 1 == 1 - *below;
  if (!isPinned) {
    return value;
  }

  return Linear{Term::zero(), *above - 1 + value.offset};
}

bool IndexProofs::canProveSumUpTo(std::optional<Linear> first, std::optional<Linear> second,
                                  const Expr *object) const {
  if (!first.has_value() || !second.has_value()) {
    return false;
  }
  auto settledFirst = this->settled(*first);
  auto settledSecond = this->settled(*second);
  auto term = this->sumTerm(settledFirst.term, settledSecond.term);
  if (!term.has_value()) {
    return false;
  }
  auto sum = Linear{*term, settledFirst.offset + settledSecond.offset};

  return this->canProveLinearUpTo(sum, object, 0);
}

std::optional<Term> IndexProofs::sumTerm(const Term &first, const Term &second) const {
  if (first.isZero()) {
    return second;
  }
  if (second.isZero()) {
    return first;
  }

  return Term::sum(first, second);
}

void IndexProofs::addIndexCallbackFacts(const FunctionDecl *functionDecl) {
  this->addElementFacts(functionDecl);
  this->addIndexParamFacts(functionDecl);
}

void IndexProofs::addElementFacts(const FunctionDecl *functionDecl) {
  const auto *element = this->state.getCallbackFacts().findElementCallback(functionDecl);
  if (element == nullptr) {
    return;
  }
  const VarSymbol *value = this->paramVariable(functionDecl, element->param);
  if (value == nullptr || !value->type->isInt()) {
    return;
  }
  auto &facts = this->state.getFlowState().getFacts();
  int64_t below = Facts::saturatingSub(1, element->lo);
  int64_t above = Facts::saturatingAdd(element->hi, 1);
  facts.add(Term::zero(), Term::of(value), below);
  facts.add(Term::of(value), Term::zero(), above);
}

void IndexProofs::addIndexParamFacts(const FunctionDecl *functionDecl) {
  auto index = this->state.getCallbackFacts().findIndexParam(functionDecl);
  if (!index.has_value()) {
    return;
  }
  const VarSymbol *variable = this->paramVariable(functionDecl, *index);
  if (variable == nullptr || !variable->type->isInt()) {
    return;
  }
  auto &facts = this->state.getFlowState().getFacts();
  facts.add(Term::zero(), Term::of(variable), 1);
  facts.add(Term::of(variable), Term::zero(), Type::kMaxLength);
}

const VarSymbol *IndexProofs::paramVariable(const FunctionDecl *functionDecl, size_t index) const {
  if (index >= functionDecl->params.size()) {
    return nullptr;
  }

  return functionDecl->params[index].sym;
}

IntegerRange IndexProofs::lengthRange(const Expr *value) const {
  const auto *source = this->viewedSource(value);
  auto *call = dyn_cast<CallExpr>(source);
  if (call == nullptr || !this->isSizedAllocation(call)) {
    return {};
  }
  if (call->args.empty()) {
    return IntegerRange{.lo = 0, .hi = 0};
  }

  return this->byteProofs->valueRange(call->args[0]);
}

const Expr *IndexProofs::sameLengthSource(const Expr *object) const {
  const Expr *source = object;
  const auto *inner = this->sameLengthInner(source);
  while (inner != nullptr) {
    source = inner;
    inner = this->sameLengthInner(source);
  }

  return source;
}

const Expr *IndexProofs::sameLengthInner(const Expr *object) const {
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

const Expr *IndexProofs::singleAllArgument(const AwaitExpr *awaited) const {
  auto *all = dyn_cast<CallExpr>(awaited->value);
  bool isSingle = all != nullptr && all->builtin() == Builtin::PromiseAll && all->args.size() == 1;

  return isSingle ? all->args[0] : nullptr;
}

const Expr *IndexProofs::copiedSource(const CallExpr *call) const {
  bool isFromArray = call->builtin() == Builtin::BytesFromArray;
  bool isFromBytes = call->builtin() == Builtin::BytesFromBytes;

  return isFromArray || isFromBytes ? call->args[0] : nullptr;
}

const Expr *IndexProofs::viewedSource(const Expr *value) const {
  const Expr *source = value;
  const auto *view = this->wholeByteView(source);
  while (view != nullptr) {
    source = view->args[0];
    view = this->wholeByteView(source);
  }

  return source;
}

const CallExpr *IndexProofs::wholeByteView(const Expr *value) const {
  auto *call = dyn_cast<CallExpr>(value);
  if (call == nullptr) {
    return nullptr;
  }
  bool isView = call->builtin() == Builtin::DataViewNew || call->builtin() == Builtin::BytesView;
  bool isWhole = call->args.size() == 1 && call->type != nullptr && call->type->elementSize() == 1;

  return isView && isWhole ? call : nullptr;
}

std::optional<Linear> IndexProofs::knownLength(const Expr *value) const {
  auto views = std::vector<const CallExpr *>();
  const Expr *base = value;
  const auto *view = this->innerView(base);
  while (view != nullptr) {
    views.push_back(view);
    base = view->args[0];
    view = this->innerView(base);
  }
  auto length = this->baseLength(base);
  for (const CallExpr *outer : llvm::reverse(views)) {
    length = this->viewOver(outer, length);
  }

  return length;
}

const CallExpr *IndexProofs::innerView(const Expr *value) const {
  auto *call = dyn_cast<CallExpr>(value);
  if (call == nullptr) {
    return nullptr;
  }
  bool isView = call->builtin() == Builtin::DataViewNew || call->builtin() == Builtin::BytesView;

  return isView && call->args.size() <= 2 ? call : nullptr;
}

std::optional<Linear> IndexProofs::baseLength(const Expr *value) const {
  auto *create = dyn_cast<NewExpr>(value);
  auto *call = dyn_cast<CallExpr>(value);
  if (create != nullptr) {
    return this->constructedLength(create);
  }
  if (call != nullptr) {
    return this->callBaseLength(call);
  }

  return this->lengthOf(value);
}

std::optional<Linear> IndexProofs::constructedLength(const NewExpr *create) const {
  if (create->args.size() == 1) {
    return this->linear(create->args[0]);
  }

  return Linear{Term::zero(), 0};
}

std::optional<Linear> IndexProofs::callBaseLength(const CallExpr *call) const {
  Builtin builtin = call->builtin();
  bool isCopy = builtin == Builtin::BytesFromArray || builtin == Builtin::BytesFromBytes;
  bool isView = builtin == Builtin::DataViewNew || builtin == Builtin::BytesView;
  if (this->isSizedAllocation(call)) {
    return this->allocatedLength(call);
  }
  if (isCopy) {
    return this->lengthOf(call->args[0]);
  }
  if (isView) {
    return this->linear(call->args[2]);
  }

  return this->lengthOf(call);
}

std::optional<Linear> IndexProofs::allocatedLength(const CallExpr *call) const {
  if (call->args.empty()) {
    return Linear{Term::zero(), 0};
  }

  return this->linear(call->args[0]);
}

}
