#include "compiler/semantic/library/bytes/byte_proofs/byte_proofs.h"
#include "compiler/program/library/builtin_names/builtin_property_table/builtin_property_table.h"
#include "compiler/semantic/declare/constant_folder/constant_folder.h"
#include "compiler/semantic/flow/expr_parts/expr_parts.h"
#include "compiler/semantic/flow/index_proofs/index_proofs.h"
#include "compiler/semantic/library/bytes/bytes_check_helpers/bytes_check_helpers.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "runtime/library/bytes/encodings/encodings.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/MathExtras.h"

using llvm::cast;
using llvm::dyn_cast;

namespace tz {

ByteProofs::ByteProofs(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->indexProofs = &all.indexProofs;
  this->constantFolder = &all.constantFolder;
}

const VarSymbol *ByteProofs::viewOwner(const Expr *expr, BuiltinProperty field,
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

const Expr *ByteProofs::withoutCasts(const Expr *expr) const {
  expr = ExprParts::withoutImplicitCasts(expr);

  return expr;
}

bool ByteProofs::sameView(const Expr *buffer, const Expr *offset, const Expr *length,
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

int64_t ByteProofs::wrapped(Bytes kind, int64_t v) const {
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

Range ByteProofs::exact(int64_t v) const {
  return {v, v};
}

bool ByteProofs::known(const Range &r) const {
  return r.lo && r.hi;
}

Range ByteProofs::unite(const Range &a, const Range &b) const {
  Range r;
  if (a.lo && b.lo) {
    r.lo = std::min(*a.lo, *b.lo);
  }
  if (a.hi && b.hi) {
    r.hi = std::max(*a.hi, *b.hi);
  }

  return r;
}

int64_t ByteProofs::allOnes(int64_t v) const {
  int64_t m = 0;
  while (m < v) {
    m = m * 2 + 1;
  }

  return m;
}

int64_t ByteProofs::floorShift(int64_t v, int64_t n) const {
  return v >= 0 ? v >> n : -((-v + (int64_t(1) << n) - 1) >> n);
}

IntegerRange ByteProofs::valueRange(const Expr *expr) const {
  auto answers = std::vector<Range>();
  for (const Expr *node : ChildrenFirst::order(expr, this, &ByteProofs::rangeParts)) {
    Range range = this->nodeRange(node, answers);
    answers.push_back(range);
  }

  return answers.back();
}

NodeChildren<const Expr *> ByteProofs::rangeParts(const Expr *expr) const {
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

NodeChildren<const Expr *> ByteProofs::callRangeParts(const CallExpr *call) const {
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

Range ByteProofs::nodeRange(const Expr *expr, std::vector<Range> &answers) const {
  size_t count = this->rangeParts(expr).size();
  std::vector<Range> parts = ChildrenFirst::takeAnswers(answers, count);
  Range computed = this->computedRange(expr, parts);

  return this->clampToType(expr, computed);
}

Range ByteProofs::clampToType(const Expr *expr, Range r) const {
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

IntegerRange ByteProofs::computedRange(const Expr *expr, llvm::ArrayRef<Range> parts) const {
  if (expr->type == nullptr || !expr->type->nonOptional()->isInteger()) {
    return {};
  }
  auto constant = this->constantFolder->intConstant(expr);
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

IntegerRange ByteProofs::castRange(const CastExpr *conversion, const IntegerRange &operand) const {
  const Type *from = conversion->operand->type;
  if (!conversion->isImplicit() || from == nullptr || !from->isInteger()) {
    return {};
  }
  const Type *to = conversion->type->nonOptional();
  bool isInside = this->known(operand) && *operand.lo >= to->intMin() &&
                  (*operand.hi < 0 || uint64_t(*operand.hi) <= to->intMax());
  if (!to->holds(from->nonOptional()) && !isInside) {
    return {};
  }

  return operand;
}

IntegerRange ByteProofs::nameRange(const NameExpr *name) const {
  const VarSymbol *variable = name->var();
  if (variable == nullptr || !variable->type->isInteger() || variable->type->isWrapping()) {
    return {};
  }
  IntegerRange r;
  auto self = Term::of(variable);
  auto above = this->state.getFlowState().getFacts().bound(self, Term::zero());
  if (above.has_value()) {
    r.hi = std::min(*above - 1, kIntMax);
  }
  auto below = this->state.getFlowState().getFacts().bound(Term::zero(), self);
  if (below.has_value()) {
    r.lo = std::max(1 - *below, kIntMin);
  }

  return r;
}

IntegerRange ByteProofs::memberRange(const MemberExpr *member) const {
  bool isLength = BuiltinPropertyTable::isName(member->name, BuiltinProperty::LENGTH) ||
                  BuiltinPropertyTable::isName(member->name, BuiltinProperty::BYTE_LENGTH);
  if (!isLength) {
    return {};
  }

  return {0, Type::kMaxLength};
}

IntegerRange ByteProofs::elementRangeOf(const Type *type) const {
  int64_t low = 0;
  int64_t high = 0;
  if (type == nullptr || !type->elementRange(low, high)) {
    return {};
  }

  return {low, high};
}

IntegerRange ByteProofs::callRange(const CallExpr *call, llvm::ArrayRef<IntegerRange> parts) const {
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

IntegerRange ByteProofs::minMaxRange(const CallExpr *call,
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

IntegerRange ByteProofs::lowerOf(const IntegerRange &a, const IntegerRange &b) const {
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

IntegerRange ByteProofs::upperOf(const IntegerRange &a, const IntegerRange &b) const {
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

IntegerRange ByteProofs::unaryRange(const UnaryExpr *unary, const IntegerRange &operand) const {
  if (unary->op == UnaryOp::Plus) {
    return operand;
  }
  if (!this->known(operand)) {
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

IntegerRange ByteProofs::binaryRange(const BinaryExpr *binary,
                                     llvm::ArrayRef<IntegerRange> parts) const {
  if (binary->op == BinaryOp::Nullish) {
    return this->unite(parts[0], parts[1]);
  }

  return BytesCheckHelpers::operate(binary->op, parts[0], parts[1], false);
}

bool ByteProofs::provesFits(const Expr *expr, int64_t low, int64_t high) const {
  return this->indexProofs->canProveBetween(expr, low, high);
}

ByteProofs::ResultValueFitsType ByteProofs::valueFits(const Expr *value, const Type *target,
                                                      llvm::StringRef what) {
  int64_t low, hi;
  if (!value->type->isInteger() || !target->elementRange(low, hi)) {
    return ok(value);
  }
  Bytes kind = target->bytes();
  bool isFitting = kind == Bytes::Uint8Clamped || this->provesFits(value, low, hi) ||
                   (low >= value->type->intMin() && uint64_t(hi) >= value->type->intMax());
  if (isFitting) {
    return ok(value);
  }
  auto range = ErrorTextBytes::range(low, hi);
  if (std::optional<int64_t> c = this->constantFolder->intConstant(value)) {
    auto hint = BytesCheckHelpers::wrapHint(kind, std::to_string(*c));
    int64_t stored = this->wrapped(kind, *c);
    auto message = ErrorTextByteFit::constant(*c, what, range, stored, hint);

    return err(ByteProofsValueFitsError{
        .code = ByteProofsValueFitsErrorCode::CONSTANT_DOES_NOT_FIT,
        .message = message,
    });
  }
  auto text = BytesCheckHelpers::valueText(value);
  auto hint = BytesCheckHelpers::wrapHint(kind, text);
  auto message = ErrorTextByteFit::unproven(what, range, text, low, hi, hint);

  return err(ByteProofsValueFitsError{
      .code = ByteProofsValueFitsErrorCode::FIT_NOT_PROVEN,
      .message = message,
  });
}

ByteProofs::ResultOffsetAligned
ByteProofs::offsetAligned(std::optional<int64_t> start, int64_t size, llvm::StringRef what) const {
  if (size > 1 && (!start || *start % size != 0)) {
    auto message = ErrorTextByteView::offset(what, size);

    return err(ByteProofsOffsetAlignedError{
        .code = ByteProofsOffsetAlignedErrorCode::MISALIGNED_OFFSET,
        .message = message,
    });
  }

  return ok(start.value_or(0));
}

ByteProofs::ResultBufferAligned
ByteProofs::bufferAligned(std::optional<int64_t> whole, int64_t size, llvm::StringRef what) const {
  if (size > 1 && (!whole || *whole % size != 0)) {
    auto message = ErrorTextByteView::size(what, size);

    return err(ByteProofsBufferAlignedError{
        .code = ByteProofsBufferAlignedErrorCode::SIZE_NOT_PROVEN,
        .message = message,
    });
  }

  return ok(whole.value_or(0));
}

void ByteProofs::walkView(const Expr *buffer, const Expr *offset, const Expr *length, int64_t size,
                          llvm::StringRef what) {
  if (this->sameView(buffer, offset, length, size) ||
      this->indexProofs->isExempt(buffer->loc(), buffer)) {
    return;
  }
  IndexProofs &proofs = *this->indexProofs;
  if (offset != nullptr) {
    auto offsetWhat = ErrorTextByteView::offsetOf(what);
    auto upTo = proofs.requireUpTo(offset, buffer, 0, offsetWhat);
    if (upTo.isErr() && upTo.error().code != IndexProofsRequireUpToErrorCode::EXEMPT) {
      this->state.getDiagnostics().error(offset->loc(), upTo.error().message);
    }
    if (upTo.isErr()) {
      return;
    }
  }
  std::optional<int64_t> start =
      offset != nullptr ? this->constantFolder->intConstant(offset) : std::optional<int64_t>(0);
  auto aligned = this->offsetAligned(start, size, what);
  if (aligned.isErr()) {
    this->state.getDiagnostics().error(offset->loc(), aligned.error().message);
    return;
  }
  if (length == nullptr) {
    auto whole = proofs.exactLength(buffer);
    auto sized = this->bufferAligned(whole, size, what);
    if (sized.isErr()) {
      this->state.getDiagnostics().error(buffer->loc(), sized.error().message);
    }
    return;
  }
  auto lengthWhat = ErrorTextByteView::lengthOf(what);
  auto between = proofs.requireBetween(length, 0, std::nullopt, lengthWhat);
  if (between.isErr() && between.error().code != IndexProofsRequireBetweenErrorCode::EXEMPT) {
    this->state.getDiagnostics().error(length->loc(), between.error().message);
  }
  if (between.isErr()) {
    return;
  }
  auto count = proofs.linear(length);
  std::optional<Linear> from = offset != nullptr ? proofs.linear(offset) : Linear{Term::zero(), 0};
  if (count && size > 1) {
    auto known = proofs.settled(*count);
    count = known.term.isZero() ? std::optional<Linear>(Linear{Term::zero(), known.offset * size})
                                : std::nullopt;
  }
  if (!proofs.canProveSumUpTo(from, count, buffer)) {
    std::string offsetText =
        offset != nullptr ? BytesCheckHelpers::valueText(offset) : std::string();
    auto end = ErrorTextByteView::end(offsetText, BytesCheckHelpers::valueText(length), size);
    auto message = ErrorTextByteView::notFits(what, end, BytesCheckHelpers::valueText(buffer));
    this->state.getDiagnostics().error(length->loc(), message);
  }
}

bool ByteProofs::provesWithin(const Expr *object, const Expr *offset, int64_t size) const {
  std::optional<Linear> at =
      offset != nullptr ? this->indexProofs->linear(offset) : Linear{Term::zero(), 0};
  auto length = this->indexProofs->lengthOf(object);
  if (!at || !length) {
    return false;
  }

  return this->state.getFlowState().getFacts().canProve(Term::zero(), at->term, at->offset + 1) &&
         this->state.getFlowState().getFacts().canProve(at->term, length->term,
                                                        length->offset - size + 1 - at->offset);
}

void ByteProofs::addByteCallbackFacts(const FunctionDecl *functionDecl) {
  const auto *found = this->state.getCallbackFacts().findByteCallback(functionDecl);
  if (found == nullptr || found->param >= functionDecl->params.size()) {
    return;
  }
  const ByteCallback &info = *found;
  const VarSymbol *variable = functionDecl->params[info.param].sym;
  if (variable == nullptr || !variable->type->isInt() ||
      (info.lo <= kIntMin && info.hi >= kIntMax)) {
    return;
  }
  this->state.getFlowState().getFacts().add(Term::zero(), Term::of(variable),
                                            Facts::saturatingSub(1, info.lo));
  this->state.getFlowState().getFacts().add(Term::of(variable), Term::zero(),
                                            Facts::saturatingAdd(info.hi, 1));
}

ByteProofs::ResultByteCallbackChecked
ByteProofs::checkByteCallbackResult(const FunctionDecl *functionDecl, const Expr *value) {
  const auto *found = this->state.getCallbackFacts().findByteCallback(functionDecl);
  if (found == nullptr || found->target == nullptr) {
    return ok();
  }
  auto what = ErrorTextByteView::element(found->target->name());
  auto fitting = this->valueFits(value, found->target, what);
  if (fitting.isErr()) {
    return err(fitting.error());
  }

  return ok();
}

}
