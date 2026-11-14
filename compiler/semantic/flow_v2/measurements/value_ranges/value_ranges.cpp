#include "compiler/semantic/flow_v2/measurements/value_ranges/value_ranges.h"
#include "compiler/program/library/builtin_names/builtin_property_table/builtin_property_table.h"
#include "compiler/semantic/flow/children_first/children_first.h"
#include "compiler/semantic/library/bytes/bytes_check_helpers/bytes_check_helpers.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include "llvm/Support/MathExtras.h"

using llvm::cast;

namespace tz {

ValueRanges::ValueRanges(SemanticState &owner)
    : state(owner) {}

Range ValueRanges::exact(int64_t v) const {
  return {v, v};
}

bool ValueRanges::isKnownRange(const Range &r) const {
  return r.lo && r.hi;
}

Range ValueRanges::unite(const Range &a, const Range &b) const {
  Range r;
  if (a.lo && b.lo) {
    r.lo = std::min(*a.lo, *b.lo);
  }
  if (a.hi && b.hi) {
    r.hi = std::max(*a.hi, *b.hi);
  }

  return r;
}

IntegerRange ValueRanges::valueRange(const Expr *expr) const {
  auto answers = std::vector<Range>();
  for (const Expr *node : ChildrenFirst::order(expr, this, &ValueRanges::rangeParts)) {
    Range range = this->nodeRange(node, answers);
    answers.push_back(range);
  }

  return answers.back();
}

NodeChildren<const Expr *> ValueRanges::rangeParts(const Expr *expr) const {
  if (expr->type == nullptr || !expr->type->nonOptional()->isInteger()) {
    return {};
  }
  switch (expr->kind()) {
    case Expr::Kind::Cast: {
      auto *conversion = cast<CastExpr>(expr);
      bool isInteger =
          conversion->operand->type != nullptr && conversion->operand->type->isInteger();
      if (!conversion->isImplicit() || !isInteger) {
        return {};
      }
      return {conversion->operand};
    }
    case Expr::Kind::Call:
      return this->callRangeParts(cast<CallExpr>(expr));
    case Expr::Kind::Unary:
      return {cast<UnaryExpr>(expr)->operand};
    case Expr::Kind::Binary:
      return {cast<BinaryExpr>(expr)->lhs, cast<BinaryExpr>(expr)->rhs};
    case Expr::Kind::Conditional:
      return {cast<ConditionalExpr>(expr)->thenExpr, cast<ConditionalExpr>(expr)->elseExpr};
    case Expr::Kind::Sequence:
      return {cast<SequenceExpr>(expr)->exprs.back()};
    default:
      return {};
  }
}

NodeChildren<const Expr *> ValueRanges::callRangeParts(const CallExpr *call) const {
  switch (call->builtin()) {
    case Builtin::NumConvert:
      return {cast<MemberExpr>(call->callee)->object};
    case Builtin::MathMin:
    case Builtin::MathMax: {
      if (call->hasSpread() || call->args.empty()) {
        return {};
      }
      NodeChildren<const Expr *> parts(call->args.begin(), call->args.end());
      return parts;
    }
    default:
      return {};
  }
}

Range ValueRanges::nodeRange(const Expr *expr, std::vector<Range> &answers) const {
  size_t count = this->rangeParts(expr).size();
  std::vector<Range> parts = ChildrenFirst::takeAnswers(answers, count);
  Range computed = this->computedRange(expr, parts);

  return this->clampToType(expr, computed);
}

Range ValueRanges::clampToType(const Expr *expr, Range r) const {
  if (expr->type == nullptr || !expr->type->nonOptional()->isInteger()) {
    return r;
  }
  const Type *type = expr->type->nonOptional();
  if (type->bitWidth() < 64 || type->isUnsigned()) {
    r.lo = r.lo ? std::max(*r.lo, type->intMin()) : type->intMin();
  }
  if (type->bitWidth() < 64) {
    r.hi = r.hi ? std::min(*r.hi, int64_t(type->intMax())) : int64_t(type->intMax());
  }

  return r;
}

IntegerRange ValueRanges::computedRange(const Expr *expr, llvm::ArrayRef<Range> parts) const {
  if (expr->type == nullptr || !expr->type->nonOptional()->isInteger()) {
    return {};
  }
  auto constant = this->state.code.constants.intConstant(expr);
  if (constant.has_value()) {
    return this->exact(*constant);
  }
  switch (expr->kind()) {
    case Expr::Kind::Cast:
      return this->castRange(cast<CastExpr>(expr), parts[0]);
    case Expr::Kind::Name:
      return this->nameRange(cast<NameExpr>(expr));
    case Expr::Kind::Member:
      return this->memberRange(cast<MemberExpr>(expr));
    case Expr::Kind::Index:
      return this->elementRangeOf(cast<IndexExpr>(expr)->object->type);
    case Expr::Kind::Call:
      return this->callRange(cast<CallExpr>(expr), parts);
    case Expr::Kind::Unary:
      return this->unaryRange(cast<UnaryExpr>(expr), parts[0]);
    case Expr::Kind::Binary:
      return this->binaryRange(cast<BinaryExpr>(expr), parts);
    case Expr::Kind::Conditional:
      return this->unite(parts[0], parts[1]);
    case Expr::Kind::Sequence:
      return parts[0];
    case Expr::Kind::IntLit:
    case Expr::Kind::FloatLit:
    case Expr::Kind::BoolLit:
    case Expr::Kind::StringLit:
    case Expr::Kind::Undefined:
    case Expr::Kind::Template:
    case Expr::Kind::ArrayLit:
    case Expr::Kind::ObjectLit:
    case Expr::Kind::Assign:
    case Expr::Kind::Update:
    case Expr::Kind::New:
    case Expr::Kind::OptionalChain:
    case Expr::Kind::Function:
    case Expr::Kind::Yield:
    case Expr::Kind::RegexLit:
    case Expr::Kind::Await:
    case Expr::Kind::Class:
      return {};
  }

  return {};
}

IntegerRange ValueRanges::castRange(const CastExpr *conversion, const IntegerRange &operand) const {
  const Type *from = conversion->operand->type;
  if (!conversion->isImplicit() || from == nullptr || !from->isInteger()) {
    return {};
  }
  const Type *to = conversion->type->nonOptional();
  bool isInside = this->isKnownRange(operand) && *operand.lo >= to->intMin() &&
                  (*operand.hi < 0 || uint64_t(*operand.hi) <= to->intMax());
  if (!to->holds(from->nonOptional()) && !isInside) {
    return {};
  }

  return operand;
}

IntegerRange ValueRanges::nameRange(const NameExpr *name) const {
  const VarSymbol *variable = name->var();
  if (variable == nullptr || !variable->type->isInteger() || variable->type->isWrapping()) {
    return {};
  }
  IntegerRange r;
  auto self = Term::of(variable);
  auto above = this->state.walk.flow.getFacts().bound(self, Term::zero());
  if (above.has_value()) {
    r.hi = std::min(*above - 1, kIntMax);
  }
  auto below = this->state.walk.flow.getFacts().bound(Term::zero(), self);
  if (below.has_value()) {
    r.lo = std::max(1 - *below, kIntMin);
  }

  return r;
}

IntegerRange ValueRanges::memberRange(const MemberExpr *member) const {
  bool isLength = BuiltinPropertyTable::isName(member->name, BuiltinProperty::LENGTH) ||
                  BuiltinPropertyTable::isName(member->name, BuiltinProperty::BYTE_LENGTH);
  if (!isLength) {
    return {};
  }

  return {0, Type::kMaxLength};
}

IntegerRange ValueRanges::elementRangeOf(const Type *type) const {
  int64_t low = 0;
  int64_t high = 0;
  if (type == nullptr || !type->elementRange(low, high)) {
    return {};
  }

  return {low, high};
}

IntegerRange ValueRanges::callRange(const CallExpr *call,
                                    llvm::ArrayRef<IntegerRange> parts) const {
  Builtin builtin = call->builtin();
  if (builtin == Builtin::StrCharCodeAt) {
    return {0, 0xFFFF};
  }
  if (builtin == Builtin::StrCodePointAt) {
    return {0, 0x10FFFF};
  }
  if (builtin == Builtin::BytesAt) {
    return this->elementRangeOf(cast<MemberExpr>(call->callee)->object->type);
  }
  if (builtin == Builtin::NumConvert) {
    return parts[0];
  }
  if (builtin == Builtin::DataViewGet || builtin == Builtin::BufferRead) {
    auto *kind = cast<IntLitExpr>(call->args[0]);
    return this->elementRangeOf(Type::bytesType(Bytes(kind->value)));
  }
  if (builtin == Builtin::MathMin || builtin == Builtin::MathMax) {
    return this->minMaxRange(call, parts);
  }

  return {};
}

IntegerRange ValueRanges::minMaxRange(const CallExpr *call,
                                      llvm::ArrayRef<IntegerRange> parts) const {
  if (call->hasSpread() || call->args.empty()) {
    return {};
  }
  bool isMin = call->builtin() == Builtin::MathMin;
  IntegerRange r = parts[0];
  for (size_t index = 1; index < call->args.size(); ++index) {
    r = isMin ? this->lowerOf(r, parts[index]) : this->upperOf(r, parts[index]);
  }

  return r;
}

IntegerRange ValueRanges::lowerOf(const IntegerRange &a, const IntegerRange &b) const {
  IntegerRange both;
  if (a.hi.has_value() && b.hi.has_value()) {
    both.hi = std::min(*a.hi, *b.hi);
  } else if (a.hi.has_value() || b.hi.has_value()) {
    both.hi = a.hi.has_value() ? *a.hi : *b.hi;
  }
  if (a.lo.has_value() && b.lo.has_value()) {
    both.lo = std::min(*a.lo, *b.lo);
  }

  return both;
}

IntegerRange ValueRanges::upperOf(const IntegerRange &a, const IntegerRange &b) const {
  IntegerRange both;
  if (a.lo.has_value() && b.lo.has_value()) {
    both.lo = std::max(*a.lo, *b.lo);
  } else if (a.lo.has_value() || b.lo.has_value()) {
    both.lo = a.lo.has_value() ? *a.lo : *b.lo;
  }
  if (a.hi.has_value() && b.hi.has_value()) {
    both.hi = std::max(*a.hi, *b.hi);
  }

  return both;
}

IntegerRange ValueRanges::unaryRange(const UnaryExpr *unary, const IntegerRange &operand) const {
  if (unary->op == UnaryOp::Plus) {
    return operand;
  }
  if (!this->isKnownRange(operand)) {
    return {};
  }
  if (unary->op == UnaryOp::Neg) {
    return BytesCheckHelpers::operate(BinaryOp::Sub, this->exact(0), operand, false);
  }
  if (unary->op == UnaryOp::BitNot) {
    return {-*operand.hi - 1, -*operand.lo - 1};
  }

  return {};
}

IntegerRange ValueRanges::binaryRange(const BinaryExpr *binary,
                                      llvm::ArrayRef<IntegerRange> parts) const {
  if (binary->op == BinaryOp::Nullish) {
    return this->unite(parts[0], parts[1]);
  }

  return BytesCheckHelpers::operate(binary->op, parts[0], parts[1], false);
}

}
