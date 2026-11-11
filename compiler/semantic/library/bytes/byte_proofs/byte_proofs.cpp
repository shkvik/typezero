#include "compiler/semantic/library/bytes/byte_proofs/byte_proofs.h"
#include "compiler/program/library/builtin_names/builtin_property_table/builtin_property_table.h"
#include "compiler/semantic/declare/constant_folder/constant_folder.h"
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
  this->indexProofs = &all.getIndexProofs();
  this->constantFolder = &all.getConstantFolder();
}

const VarSymbol *ByteProofs::viewOwner(const Expr *expr, BuiltinProperty field, const Type **type) {
  if (auto *call = dyn_cast<CallExpr>(expr);
      call &&
      (call->builtin() == Builtin::BytesBuffer || call->builtin() == Builtin::BytesByteOffset ||
       call->builtin() == Builtin::BytesByteLength)) {
    expr = call->callee;
  }
  auto *member = dyn_cast<MemberExpr>(expr);
  if (!member || !BuiltinPropertyTable::isName(member->name, field) || !member->object->type ||
      !member->object->type->isByteView()) {
    return nullptr;
  }
  const Expr *object = member->object;
  while (auto *conv = dyn_cast<CastExpr>(object)) {
    if (!conv->isImplicit()) {
      return nullptr;
    }
    object = conv->operand;
  }
  auto *name = dyn_cast<NameExpr>(object);
  if (type) {
    *type = member->object->type;
  }

  return name ? name->var() : nullptr;
}

const Expr *ByteProofs::withoutCasts(const Expr *expr) {
  while (auto *conv = dyn_cast<CastExpr>(expr)) {
    if (!conv->isImplicit()) {
      break;
    }
    expr = conv->operand;
  }

  return expr;
}

bool ByteProofs::sameView(const Expr *buffer, const Expr *offset, const Expr *length,
                          int64_t size) {
  const Type *ownerType = nullptr;
  const VarSymbol *owner = ByteProofs::viewOwner(buffer, BuiltinProperty::BUFFER, &ownerType);
  if (!owner || !offset ||
      ByteProofs::viewOwner(ByteProofs::withoutCasts(offset), BuiltinProperty::BYTE_OFFSET) !=
          owner) {
    return false;
  }
  if (!length) {
    return size == 1;
  }
  length = ByteProofs::withoutCasts(length);
  if (ByteProofs::viewOwner(length, BuiltinProperty::BYTE_LENGTH) == owner) {
    return size == 1;
  }

  return ByteProofs::viewOwner(length, BuiltinProperty::LENGTH) == owner &&
         ownerType->elementSize() == uint64_t(size);
}

int64_t ByteProofs::wrapped(Bytes kind, int64_t v) {
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

Range ByteProofs::exact(int64_t v) {
  return {v, v};
}

bool ByteProofs::known(const Range &r) {
  return r.lo && r.hi;
}

Range ByteProofs::unite(const Range &a, const Range &b) {
  Range r;
  if (a.lo && b.lo) {
    r.lo = std::min(*a.lo, *b.lo);
  }
  if (a.hi && b.hi) {
    r.hi = std::max(*a.hi, *b.hi);
  }

  return r;
}

int64_t ByteProofs::allOnes(int64_t v) {
  int64_t m = 0;
  while (m < v) {
    m = m * 2 + 1;
  }

  return m;
}

int64_t ByteProofs::floorShift(int64_t v, int64_t n) {
  return v >= 0 ? v >> n : -((-v + (int64_t(1) << n) - 1) >> n);
}

IntegerRange ByteProofs::valueRange(const Expr *expr) const {
  std::vector<Range> answers;
  for (const Expr *node : ChildrenFirst::order(expr, ByteProofs::rangeParts)) {
    Range range = this->nodeRange(node, answers);
    answers.push_back(range);
  }

  return answers.back();
}

NodeChildren<const Expr *> ByteProofs::rangeParts(const Expr *e) {
  if (!e->type || !e->type->nonOptional()->isInteger()) {
    return {};
  }
  switch (e->kind()) {
    case Expr::Kind::Cast: {
      auto *conv = cast<CastExpr>(e);
      bool integer = conv->operand->type && conv->operand->type->isInteger();
      if (!conv->isImplicit() || !integer) {
        return {};
      }
      return {conv->operand};
    }
    case Expr::Kind::Call:
      return ByteProofs::callRangeParts(cast<CallExpr>(e));
    case Expr::Kind::Unary:
      return {cast<UnaryExpr>(e)->operand};
    case Expr::Kind::Binary:
      return {cast<BinaryExpr>(e)->lhs, cast<BinaryExpr>(e)->rhs};
    case Expr::Kind::Conditional:
      return {cast<ConditionalExpr>(e)->thenExpr, cast<ConditionalExpr>(e)->elseExpr};
    case Expr::Kind::Sequence:
      return {cast<SequenceExpr>(e)->exprs.back()};
    default:
      return {};
  }
}

NodeChildren<const Expr *> ByteProofs::callRangeParts(const CallExpr *call) {
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

Range ByteProofs::nodeRange(const Expr *e, std::vector<Range> &answers) const {
  size_t count = ByteProofs::rangeParts(e).size();
  std::vector<Range> parts = ChildrenFirst::takeAnswers(answers, count);
  Range computed = this->computedRange(e, parts);

  return ByteProofs::clampToType(e, computed);
}

Range ByteProofs::clampToType(const Expr *e, Range r) {
  if (!e->type || !e->type->nonOptional()->isInteger()) {
    return r;
  }
  const Type *type = e->type->nonOptional();
  if (type->bitWidth() < 64 || type->isUnsigned()) {
    r.lo = r.lo ? std::max(*r.lo, type->intMin()) : type->intMin();
  }
  if (type->bitWidth() < 64) {
    r.hi = r.hi ? std::min(*r.hi, int64_t(type->intMax())) : int64_t(type->intMax());
  }

  return r;
}

IntegerRange ByteProofs::computedRange(const Expr *e, llvm::ArrayRef<Range> parts) const {
  if (e->type == nullptr || !e->type->nonOptional()->isInteger()) {
    return {};
  }
  std::optional<int64_t> constant = this->constantFolder->intConstant(e);
  if (constant.has_value()) {
    return ByteProofs::exact(*constant);
  }
  switch (e->kind()) {
    case Expr::Kind::Cast:
      return ByteProofs::castRange(cast<CastExpr>(e), parts[0]);
    case Expr::Kind::Name:
      return this->nameRange(cast<NameExpr>(e));
    case Expr::Kind::Member:
      return ByteProofs::memberRange(cast<MemberExpr>(e));
    case Expr::Kind::Index:
      return ByteProofs::elementRangeOf(cast<IndexExpr>(e)->object->type);
    case Expr::Kind::Call:
      return ByteProofs::callRange(cast<CallExpr>(e), parts);
    case Expr::Kind::Unary:
      return ByteProofs::unaryRange(cast<UnaryExpr>(e), parts[0]);
    case Expr::Kind::Binary:
      return ByteProofs::binaryRange(cast<BinaryExpr>(e), parts);
    case Expr::Kind::Conditional:
      return ByteProofs::unite(parts[0], parts[1]);
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

IntegerRange ByteProofs::castRange(const CastExpr *conv, const IntegerRange &operand) {
  const Type *from = conv->operand->type;
  if (!conv->isImplicit() || from == nullptr || !from->isInteger()) {
    return {};
  }
  const Type *to = conv->type->nonOptional();
  bool inside = ByteProofs::known(operand) && *operand.lo >= to->intMin() &&
                (*operand.hi < 0 || uint64_t(*operand.hi) <= to->intMax());
  if (!to->holds(from->nonOptional()) && !inside) {
    return {};
  }

  return operand;
}

IntegerRange ByteProofs::nameRange(const NameExpr *name) const {
  const VarSymbol *var = name->var();
  if (var == nullptr || !var->type->isInteger() || var->type->isWrapping()) {
    return {};
  }
  IntegerRange r;
  Term self = Term::of(var);
  std::optional<int64_t> above = this->state.getFlowState().getFacts().bound(self, Term::zero());
  if (above.has_value()) {
    r.hi = std::min(*above - 1, kIntMax);
  }
  std::optional<int64_t> below = this->state.getFlowState().getFacts().bound(Term::zero(), self);
  if (below.has_value()) {
    r.lo = std::max(1 - *below, kIntMin);
  }

  return r;
}

IntegerRange ByteProofs::memberRange(const MemberExpr *member) {
  bool length = BuiltinPropertyTable::isName(member->name, BuiltinProperty::LENGTH) ||
                BuiltinPropertyTable::isName(member->name, BuiltinProperty::BYTE_LENGTH);
  if (!length) {
    return {};
  }

  return {0, Type::kMaxLength};
}

IntegerRange ByteProofs::elementRangeOf(const Type *type) {
  int64_t lo = 0;
  int64_t hi = 0;
  if (type == nullptr || !type->elementRange(lo, hi)) {
    return {};
  }

  return {lo, hi};
}

IntegerRange ByteProofs::callRange(const CallExpr *call, llvm::ArrayRef<IntegerRange> parts) {
  Builtin builtin = call->builtin();
  if (builtin == Builtin::StrCharCodeAt) {
    return {0, 0xFFFF};
  }
  if (builtin == Builtin::StrCodePointAt) {
    return {0, 0x10FFFF};
  }
  if (builtin == Builtin::BytesAt) {
    return ByteProofs::elementRangeOf(cast<MemberExpr>(call->callee)->object->type);
  }
  if (builtin == Builtin::NumConvert) {
    return parts[0];
  }
  if (builtin == Builtin::DataViewGet || builtin == Builtin::BufferRead) {
    auto *kind = cast<IntLitExpr>(call->args[0]);
    return ByteProofs::elementRangeOf(Type::bytesType(Bytes(kind->value)));
  }
  if (builtin == Builtin::MathMin || builtin == Builtin::MathMax) {
    return ByteProofs::minMaxRange(call, parts);
  }

  return {};
}

IntegerRange ByteProofs::minMaxRange(const CallExpr *call, llvm::ArrayRef<IntegerRange> parts) {
  if (call->hasSpread() || call->args.empty()) {
    return {};
  }
  bool min = call->builtin() == Builtin::MathMin;
  IntegerRange r = parts[0];
  for (size_t i = 1; i < call->args.size(); ++i) {
    r = min ? ByteProofs::lowerOf(r, parts[i]) : ByteProofs::upperOf(r, parts[i]);
  }

  return r;
}

IntegerRange ByteProofs::lowerOf(const IntegerRange &a, const IntegerRange &b) {
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

IntegerRange ByteProofs::upperOf(const IntegerRange &a, const IntegerRange &b) {
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

IntegerRange ByteProofs::unaryRange(const UnaryExpr *unary, const IntegerRange &operand) {
  if (unary->op == UnaryOp::Plus) {
    return operand;
  }
  if (!ByteProofs::known(operand)) {
    return {};
  }
  if (unary->op == UnaryOp::Neg) {
    return BytesCheckHelpers::operate(BinaryOp::Sub, ByteProofs::exact(0), operand, false);
  }
  if (unary->op == UnaryOp::BitNot) {
    return {-*operand.hi - 1, -*operand.lo - 1};
  }

  return {};
}

IntegerRange ByteProofs::binaryRange(const BinaryExpr *bin, llvm::ArrayRef<IntegerRange> parts) {
  if (bin->op == BinaryOp::Nullish) {
    return ByteProofs::unite(parts[0], parts[1]);
  }

  return BytesCheckHelpers::operate(bin->op, parts[0], parts[1], false);
}

bool ByteProofs::provesFits(const Expr *expr, int64_t lo, int64_t hi) const {
  return this->indexProofs->provesBetween(expr, lo, hi);
}

ByteProofs::ResultValueFitsType ByteProofs::valueFits(const Expr *value, const Type *target,
                                                      llvm::StringRef what) {
  int64_t lo, hi;
  if (!value->type->isInteger() || !target->elementRange(lo, hi)) {
    return ok(value);
  }
  Bytes kind = target->bytes();
  bool fits = kind == Bytes::Uint8Clamped || this->provesFits(value, lo, hi) ||
              (lo >= value->type->intMin() && uint64_t(hi) >= value->type->intMax());
  if (fits) {
    return ok(value);
  }
  std::string range = ErrorTextBytes::range(lo, hi);
  if (std::optional<int64_t> c = this->constantFolder->intConstant(value)) {
    std::string hint = BytesCheckHelpers::wrapHint(kind, std::to_string(*c));
    int64_t stored = ByteProofs::wrapped(kind, *c);
    std::string message = ErrorTextByteFit::constant(*c, what, range, stored, hint);

    return err(ByteProofsValueFitsError{
        .code = ByteProofsValueFitsErrorCode::CONSTANT_DOES_NOT_FIT,
        .message = message,
    });
  }
  std::string text = BytesCheckHelpers::valueText(value);
  std::string hint = BytesCheckHelpers::wrapHint(kind, text);
  std::string message = ErrorTextByteFit::unproven(what, range, text, lo, hi, hint);

  return err(ByteProofsValueFitsError{
      .code = ByteProofsValueFitsErrorCode::FIT_NOT_PROVEN,
      .message = message,
  });
}

ByteProofs::ResultOffsetAligned ByteProofs::offsetAligned(std::optional<int64_t> start,
                                                          int64_t size, llvm::StringRef what) {
  if (size > 1 && (!start || *start % size != 0)) {
    std::string message = ErrorTextByteView::offset(what, size);

    return err(ByteProofsOffsetAlignedError{
        .code = ByteProofsOffsetAlignedErrorCode::MISALIGNED_OFFSET,
        .message = message,
    });
  }

  return ok(start.value_or(0));
}

ByteProofs::ResultBufferAligned ByteProofs::bufferAligned(std::optional<int64_t> whole,
                                                          int64_t size, llvm::StringRef what) {
  if (size > 1 && (!whole || *whole % size != 0)) {
    std::string message = ErrorTextByteView::size(what, size);

    return err(ByteProofsBufferAlignedError{
        .code = ByteProofsBufferAlignedErrorCode::SIZE_NOT_PROVEN,
        .message = message,
    });
  }

  return ok(whole.value_or(0));
}

void ByteProofs::checkView(const Expr *buffer, const Expr *offset, const Expr *length, int64_t size,
                           llvm::StringRef what) {
  if (ByteProofs::sameView(buffer, offset, length, size) ||
      this->indexProofs->exempt(buffer->loc(), buffer)) {
    return;
  }
  IndexProofs &proofs = *this->indexProofs;
  if (offset) {
    std::string offsetWhat = ErrorTextByteView::offsetOf(what);
    IndexProofs::ResultUpToWasProven upTo = proofs.requireUpTo(offset, buffer, 0, offsetWhat);
    if (upTo.isErr() && upTo.error().code != IndexProofsRequireUpToErrorCode::EXEMPT) {
      this->state.getDiagnostics().error(offset->loc(), upTo.error().message);
    }
    if (upTo.isErr()) {
      return;
    }
  }
  std::optional<int64_t> start =
      offset ? this->constantFolder->intConstant(offset) : std::optional<int64_t>(0);
  ResultOffsetAligned aligned = ByteProofs::offsetAligned(start, size, what);
  if (aligned.isErr()) {
    this->state.getDiagnostics().error(offset->loc(), aligned.error().message);
    return;
  }
  if (!length) {
    std::optional<int64_t> whole = proofs.exactLength(buffer);
    ResultBufferAligned sized = ByteProofs::bufferAligned(whole, size, what);
    if (sized.isErr()) {
      this->state.getDiagnostics().error(buffer->loc(), sized.error().message);
    }
    return;
  }
  std::string lengthWhat = ErrorTextByteView::lengthOf(what);
  IndexProofs::ResultBetweenProven between =
      proofs.requireBetween(length, 0, std::nullopt, lengthWhat);
  if (between.isErr() && between.error().code != IndexProofsRequireBetweenErrorCode::EXEMPT) {
    this->state.getDiagnostics().error(length->loc(), between.error().message);
  }
  if (between.isErr()) {
    return;
  }
  std::optional<Linear> count = proofs.linear(length);
  std::optional<Linear> from = offset ? proofs.linear(offset) : Linear{Term::zero(), 0};
  if (count && size > 1) {
    Linear known = proofs.settled(*count);
    count = known.term.isZero() ? std::optional<Linear>(Linear{Term::zero(), known.offset * size})
                                : std::nullopt;
  }
  if (!proofs.provesSumUpTo(from, count, buffer)) {
    std::string offsetText = offset ? BytesCheckHelpers::valueText(offset) : std::string();
    std::string end =
        ErrorTextByteView::end(offsetText, BytesCheckHelpers::valueText(length), size);
    std::string message =
        ErrorTextByteView::notFits(what, end, BytesCheckHelpers::valueText(buffer));
    this->state.getDiagnostics().error(length->loc(), message);
  }
}

bool ByteProofs::provesWithin(const Expr *object, const Expr *offset, int64_t size) const {
  std::optional<Linear> at = offset ? this->indexProofs->linear(offset) : Linear{Term::zero(), 0};
  std::optional<Linear> length = this->indexProofs->lengthOf(object);
  if (!at || !length) {
    return false;
  }

  return this->state.getFlowState().getFacts().proves(Term::zero(), at->term, at->offset + 1) &&
         this->state.getFlowState().getFacts().proves(at->term, length->term,
                                                      length->offset - size + 1 - at->offset);
}

void ByteProofs::addByteCallbackFacts(const FunctionDecl *fn) {
  const ByteCallback *found = this->state.getCallbackFacts().findByteCallback(fn);
  if (!found || found->param >= fn->params.size()) {
    return;
  }
  const ByteCallback &info = *found;
  const VarSymbol *var = fn->params[info.param].sym;
  if (!var || !var->type->isInt() || (info.lo <= kIntMin && info.hi >= kIntMax)) {
    return;
  }
  this->state.getFlowState().getFacts().add(Term::zero(), Term::of(var),
                                            Facts::saturatingSub(1, info.lo));
  this->state.getFlowState().getFacts().add(Term::of(var), Term::zero(),
                                            Facts::saturatingAdd(info.hi, 1));
}

void ByteProofs::checkByteCallbackResult(const FunctionDecl *fn, const Expr *value) {
  const ByteCallback *found = this->state.getCallbackFacts().findByteCallback(fn);
  if (!found || !found->target) {
    return;
  }
  std::string what = ErrorTextByteView::element(found->target->name());
  ByteProofs::ResultValueFitsType fitting = this->valueFits(value, found->target, what);
  if (fitting.isErr()) {
    this->state.getDiagnostics().error(value->loc(), fitting.error().message);
  }
}

}
