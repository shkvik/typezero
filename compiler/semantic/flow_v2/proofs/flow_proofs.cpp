#include "compiler/semantic/flow_v2/proofs/flow_proofs.h"
#include "compiler/program/library/builtin_names/builtin_property_table/builtin_property_table.h"
#include "compiler/semantic/library/bytes/bytes_check_helpers/bytes_check_helpers.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include "runtime/library/bytes/encodings/encodings.h"
#include "llvm/Support/MathExtras.h"

using llvm::dyn_cast;

namespace tz {

FlowProofs::FlowProofs(SemanticState &owner)
    : state(owner) {}

bool FlowProofs::canProveInBounds(const Expr *object, const Expr *index) const {
  return this->canProveIndexWithin(object, index, 0);
}

bool FlowProofs::canProveInBoundsOrEnd(const Expr *object, const Expr *index) const {
  return this->canProveIndexWithin(object, index, 1);
}

bool FlowProofs::canProveIndexWithin(const Expr *object, const Expr *index,
                                     int64_t endExtra) const {
  auto at = this->state.flow.measurements.linear(index);
  auto length = this->state.flow.measurements.lengthOf(object);
  if (!length.has_value()) {
    return false;
  }
  if (at.has_value() && this->canProveLinearIndex(*at, *length, endExtra)) {
    return true;
  }
  auto range = this->state.flow.measurements.valueRange(index);
  bool isNonNegative = range.lo.has_value() && range.hi.has_value() && *range.lo >= 0;
  if (!isNonNegative) {
    return false;
  }
  int64_t room = length->offset - *range.hi + endExtra;

  return this->state.walk.flow.getFacts().canProve(Term::zero(), length->term, room);
}

bool FlowProofs::canProveLinearIndex(const Linear &at, const Linear &length,
                                     int64_t endExtra) const {
  const auto &facts = this->state.walk.flow.getFacts();
  bool isNonNegative = facts.canProve(Term::zero(), at.term, at.offset + 1);
  int64_t room = length.offset - at.offset + endExtra;

  return isNonNegative && facts.canProve(at.term, length.term, room);
}

bool FlowProofs::canProveAtInBounds(const Expr *object, const Expr *index) const {
  auto at = this->state.flow.measurements.linear(index);
  auto length = this->state.flow.measurements.lengthOf(object);
  bool isFromEnd = at.has_value() && length.has_value() && at->term.isZero() && at->offset < 0;
  if (isFromEnd) {
    int64_t room = length->offset + at->offset + 1;

    return this->state.walk.flow.getFacts().canProve(Term::zero(), length->term, room);
  }

  return this->canProveInBounds(object, index);
}

bool FlowProofs::canProveNonNegative(const Expr *expr) const {
  auto value = this->state.flow.measurements.linear(expr);
  if (!value.has_value()) {
    return false;
  }

  return this->state.walk.flow.getFacts().canProve(Term::zero(), value->term, value->offset + 1);
}

bool FlowProofs::canProveNotEmpty(const Expr *object) const {
  auto length = this->state.flow.measurements.lengthOf(object);
  if (!length.has_value()) {
    return false;
  }

  return this->state.walk.flow.getFacts().canProve(Term::zero(), length->term, length->offset);
}

bool FlowProofs::canProveBetween(const Expr *expr, int64_t low, std::optional<int64_t> high) const {
  for (const Expr *branch : this->provedBranches(expr)) {
    if (!this->canProveValueBetween(branch, low, high)) {
      return false;
    }
  }

  return true;
}

std::vector<const Expr *> FlowProofs::provedBranches(const Expr *expr) const {
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

const Expr *FlowProofs::withoutWidening(const Expr *expr) const {
  const Expr *inner = expr;
  auto *conversion = dyn_cast<CastExpr>(inner);
  while (conversion != nullptr && this->isValueWidening(conversion)) {
    inner = conversion->operand;
    conversion = dyn_cast<CastExpr>(inner);
  }

  return inner;
}

bool FlowProofs::isValueWidening(const CastExpr *conversion) const {
  const Type *from = conversion->operand->type;
  if (!conversion->isImplicit() || from == nullptr || !from->isInteger()) {
    return false;
  }
  const Type *to = conversion->type;
  bool isWiderInteger = to->isInteger() && to->bitWidth() >= from->bitWidth();

  return to->isFloat() || isWiderInteger;
}

bool FlowProofs::canProveValueBetween(const Expr *expr, int64_t low,
                                      std::optional<int64_t> high) const {
  if (expr->type == nullptr || !expr->type->nonOptional()->isInteger()) {
    return false;
  }
  auto value = this->state.flow.measurements.linear(expr);
  auto range = this->state.flow.measurements.valueRange(expr);
  bool isRangeLow = range.lo.has_value() && *range.lo >= low;
  bool isLower = this->isLinearAtLeast(value, low) || isRangeLow;
  if (!high.has_value()) {
    return isLower;
  }
  bool isRangeHigh = range.hi.has_value() && *range.hi <= *high;
  bool isUpper = this->isLinearAtMost(value, *high) || isRangeHigh;

  return isLower && isUpper;
}

bool FlowProofs::isLinearAtLeast(const std::optional<Linear> &value, int64_t low) const {
  if (!value.has_value()) {
    return false;
  }
  if (value->term.isZero()) {
    return value->offset >= low;
  }

  return this->state.walk.flow.getFacts().canProve(Term::zero(), value->term,
                                                   value->offset - low + 1);
}

bool FlowProofs::isLinearAtMost(const std::optional<Linear> &value, int64_t high) const {
  if (!value.has_value()) {
    return false;
  }
  if (value->term.isZero()) {
    return value->offset <= high;
  }

  return this->state.walk.flow.getFacts().canProve(value->term, Term::zero(),
                                                   high - value->offset + 1);
}

bool FlowProofs::isExempt(llvm::SMLoc loc, const Expr *object) const {
  if (object != nullptr && this->isNeverObject(object)) {
    return true;
  }

  return this->state.walk.flow.isUnreachable() ||
         this->state.library.declarations.isInsideLibrary(loc);
}

bool FlowProofs::isNeverObject(const Expr *object) const {
  if (this->isNeverType(object->type)) {
    return true;
  }
  const VarSymbol *variable = this->state.flow.measurements.variableOf(object);

  return variable != nullptr && this->isNeverType(variable->type);
}

bool FlowProofs::isNeverType(const Type *type) const {
  return type != nullptr && (type->isUndefined() || type->nonOptional()->isNever());
}

FlowProofs::ResultBetweenProven FlowProofs::requireBetween(const Expr *expr, int64_t low,
                                                           std::optional<int64_t> high,
                                                           llvm::StringRef what) {
  if (expr->type == nullptr || expr->type->isError() || this->canProveBetween(expr, low, high)) {
    return ok(expr);
  }
  if (this->isExempt(expr->loc())) {
    return err(FlowProofsRequireBetweenError{
        .code = FlowProofsRequireBetweenErrorCode::EXEMPT,
        .message = "",
    });
  }

  auto range = ErrorTextBetween::range(low, high);
  auto constant = this->state.code.constants.intConstant(expr);
  if (constant.has_value() && !high.has_value() && low == 0) {
    auto message = ErrorTextBetween::negative(what, *constant);

    return err(FlowProofsRequireBetweenError{
        .code = FlowProofsRequireBetweenErrorCode::CONSTANT_NEGATIVE,
        .message = message,
    });
  }
  if (constant.has_value()) {
    auto message = ErrorTextBetween::outside(what, range, *constant);

    return err(FlowProofsRequireBetweenError{
        .code = FlowProofsRequireBetweenErrorCode::CONSTANT_OUT_OF_RANGE,
        .message = message,
    });
  }
  if (!expr->type->isInteger()) {
    auto message = ErrorTextBetween::notWhole(what, range, expr->type->name());

    return err(FlowProofsRequireBetweenError{
        .code = FlowProofsRequireBetweenErrorCode::NOT_WHOLE_NUMBER,
        .message = message,
    });
  }

  auto text = BytesCheckHelpers::valueText(expr);
  auto test = ErrorTextBetween::test(text, low, high);
  auto message = ErrorTextBetween::unproven(what, range, test);

  return err(FlowProofsRequireBetweenError{
      .code = FlowProofsRequireBetweenErrorCode::NOT_PROVEN,
      .message = message,
  });
}

bool FlowProofs::canProveLess(const Expr *left, const Expr *right) const {
  auto leftValue = this->state.flow.measurements.linear(left);
  auto rightValue = this->state.flow.measurements.linear(right);
  if (!leftValue.has_value() || !rightValue.has_value()) {
    return false;
  }
  if (leftValue->term.isZero() && rightValue->term.isZero()) {
    return leftValue->offset < rightValue->offset;
  }

  return this->state.walk.flow.getFacts().canProve(leftValue->term, rightValue->term,
                                                   rightValue->offset - leftValue->offset);
}

FlowProofs::ResultLessWasProven FlowProofs::requireLess(const Expr *left, const Expr *right,
                                                        llvm::StringRef what) {
  bool isUnchecked = left->type == nullptr || right->type == nullptr || left->type->isError() ||
                     right->type->isError();
  if (isUnchecked || this->canProveLess(left, right) || this->isExempt(right->loc())) {
    return ok(right);
  }

  auto leftText = BytesCheckHelpers::valueText(left);
  auto rightText = BytesCheckHelpers::valueText(right);
  auto message = ErrorTextLess::unproven(what, leftText, rightText);

  return err(FlowProofsRequireLessError{
      .code = FlowProofsRequireLessErrorCode::NOT_PROVEN,
      .message = message,
  });
}

bool FlowProofs::canProveLinearUpTo(Linear value, const Expr *object, int64_t less) const {
  auto length = this->lengthOrBufferLength(object);
  if (!length.has_value()) {
    return false;
  }
  int64_t room = length->offset - less - value.offset + 1;

  return this->state.walk.flow.getFacts().canProve(value.term, length->term, room);
}

std::optional<Linear> FlowProofs::lengthOrBufferLength(const Expr *object) const {
  auto length = this->state.flow.measurements.lengthOf(object);
  if (length.has_value()) {
    return length;
  }

  return this->viewBufferLength(object);
}

std::optional<Linear> FlowProofs::viewBufferLength(const Expr *object) const {
  const auto *member = this->viewBufferMember(object);
  if (member == nullptr) {
    return std::nullopt;
  }
  int64_t size = member->object->type->elementSize();
  auto view = this->state.flow.measurements.lengthOf(member->object);
  if (!view.has_value()) {
    return std::nullopt;
  }
  auto settledView = this->state.flow.measurements.settled(*view);
  if (size == 1) {
    return settledView;
  }
  if (settledView.term.isZero()) {
    return Linear{Term::zero(), settledView.offset * size};
  }

  return std::nullopt;
}

const MemberExpr *FlowProofs::viewBufferMember(const Expr *object) const {
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

bool FlowProofs::canProveUpTo(const Expr *value, const Expr *object, int64_t less) const {
  auto zero = Linear{Term::zero(), 0};
  if (value == nullptr) {
    return this->canProveLinearUpTo(zero, object, less);
  }
  if (!this->canProveBetween(value, 0, std::nullopt)) {
    return false;
  }
  auto at = this->state.flow.measurements.linear(value);
  if (at.has_value() && this->canProveLinearUpTo(*at, object, less)) {
    return true;
  }
  auto range = this->state.flow.measurements.valueRange(value);
  if (!range.hi.has_value()) {
    return false;
  }
  auto highest = Linear{Term::zero(), *range.hi};

  return this->canProveLinearUpTo(highest, object, less);
}

FlowProofs::ResultUpToWasProven FlowProofs::requireUpTo(const Expr *value, const Expr *object,
                                                        int64_t less, llvm::StringRef what) {
  bool isUnchecked = value != nullptr && (value->type == nullptr || value->type->isError());
  if (isUnchecked || this->canProveUpTo(value, object, less)) {
    return ok(object);
  }
  if (this->isExempt(object->loc(), object)) {
    return err(FlowProofsRequireUpToError{
        .code = FlowProofsRequireUpToErrorCode::EXEMPT,
        .message = "",
    });
  }

  auto text = this->shownValue(value);
  bool hasBytes = this->state.flow.measurements.isRawBuffer(object->type);
  auto objectText = BytesCheckHelpers::valueText(object);
  auto limit = ErrorTextUpTo::limit(objectText, hasBytes, less);
  auto message = ErrorTextUpTo::unproven(what, text, limit);

  return err(FlowProofsRequireUpToError{
      .code = FlowProofsRequireUpToErrorCode::NOT_PROVEN,
      .message = message,
  });
}

std::optional<std::string> FlowProofs::shownValue(const Expr *value) const {
  if (value == nullptr) {
    return std::nullopt;
  }

  return BytesCheckHelpers::valueText(value);
}

bool FlowProofs::canProveSumUpTo(std::optional<Linear> first, std::optional<Linear> second,
                                 const Expr *object) const {
  if (!first.has_value() || !second.has_value()) {
    return false;
  }
  auto settledFirst = this->state.flow.measurements.settled(*first);
  auto settledSecond = this->state.flow.measurements.settled(*second);
  auto term = this->sumTerm(settledFirst.term, settledSecond.term);
  if (!term.has_value()) {
    return false;
  }
  auto sum = Linear{*term, settledFirst.offset + settledSecond.offset};

  return this->canProveLinearUpTo(sum, object, 0);
}

std::optional<Term> FlowProofs::sumTerm(const Term &first, const Term &second) const {
  if (first.isZero()) {
    return second;
  }
  if (second.isZero()) {
    return first;
  }

  return Term::sum(first, second);
}

int64_t FlowProofs::wrapped(Bytes kind, int64_t v) const {
  switch (kind) {
    case Bytes::Int8:
      return int8_t(uint8_t(v));
    case Bytes::Int16:
      return int16_t(uint16_t(v));
    case Bytes::Uint16:
      return uint16_t(v);
    case Bytes::Uint32:
      return uint32_t(v);
    case Bytes::Int32:
      return int32_t(uint32_t(v));
    default:
      return uint8_t(v);
  }
}

bool FlowProofs::canProveFits(const Expr *expr, int64_t low, int64_t high) const {
  return this->canProveBetween(expr, low, high);
}

FlowProofs::ResultValueFitsType FlowProofs::checkValueFits(const Expr *value, const Type *target,
                                                           llvm::StringRef what) {
  int64_t low, hi;
  if (!value->type->isInteger() || !target->elementRange(low, hi)) {
    return ok(value);
  }
  Bytes kind = target->bytes();
  bool isFitting = kind == Bytes::Uint8Clamped || this->canProveFits(value, low, hi) ||
                   (low >= value->type->intMin() && uint64_t(hi) >= value->type->intMax());
  if (isFitting) {
    return ok(value);
  }
  auto range = ErrorTextBytes::range(low, hi);
  if (std::optional<int64_t> c = this->state.code.constants.intConstant(value)) {
    auto hint = BytesCheckHelpers::wrapHint(kind, std::to_string(*c));
    int64_t stored = this->wrapped(kind, *c);
    auto message = ErrorTextByteFit::constant(*c, what, range, stored, hint);

    return err(FlowProofsValueFitsError{
        .code = FlowProofsValueFitsErrorCode::CONSTANT_DOES_NOT_FIT,
        .message = message,
    });
  }
  auto text = BytesCheckHelpers::valueText(value);
  auto hint = BytesCheckHelpers::wrapHint(kind, text);
  auto message = ErrorTextByteFit::unproven(what, range, text, low, hi, hint);

  return err(FlowProofsValueFitsError{
      .code = FlowProofsValueFitsErrorCode::FIT_NOT_PROVEN,
      .message = message,
  });
}

}
