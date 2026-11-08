#include "compiler/semantic/library/bytes/byte_proofs/byte_proofs.h"
#include "compiler/semantic/declare/constant_folder/constant_folder.h"
#include "compiler/semantic/flow/index_proofs/index_proofs.h"
#include "compiler/semantic/library/bytes/bytes_check_helpers/bytes_check_helpers.h"
#include "runtime/library/bytes/encodings/encodings.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/MathExtras.h"

using llvm::cast;
using llvm::dyn_cast;

namespace tz {

const VarSymbol *ByteProofs::viewOwner(const Expr *expr, llvm::StringRef field, const Type **type) {
  if (auto *call = dyn_cast<CallExpr>(expr);
      call &&
      (call->builtin() == Builtin::BytesBuffer || call->builtin() == Builtin::BytesByteOffset ||
       call->builtin() == Builtin::BytesByteLength)) {
    expr = call->callee;
  }
  auto *member = dyn_cast<MemberExpr>(expr);
  if (!member || member->name != field || !member->object->type ||
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
  const VarSymbol *owner = ByteProofs::viewOwner(buffer, "buffer", &ownerType);
  if (!owner || !offset ||
      ByteProofs::viewOwner(ByteProofs::withoutCasts(offset), "byteOffset") != owner) {
    return false;
  }
  if (!length) {
    return size == 1;
  }
  length = ByteProofs::withoutCasts(length);
  if (ByteProofs::viewOwner(length, "byteLength") == owner) {
    return size == 1;
  }

  return ByteProofs::viewOwner(length, "length") == owner &&
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

ByteProofs::ByteProofs(SemanticState &owner)
    : state(owner) {}

IntegerRange ByteProofs::valueRange(const Expr *e) const {
  Range r = this->computedRange(e);
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

IntegerRange ByteProofs::computedRange(const Expr *e) const {
  if (!e->type || !e->type->nonOptional()->isInteger()) {
    return {};
  }
  if (std::optional<int64_t> c = ConstantFolder(this->state).intConstant(e)) {
    return ByteProofs::exact(*c);
  }
  switch (e->kind()) {
  case Expr::Kind::Cast: {
    auto *conv = cast<CastExpr>(e);
    if (!conv->isImplicit() || !conv->operand->type || !conv->operand->type->isInteger()) {
      return {};
    }
    Range r = this->valueRange(conv->operand);
    const Type *to = conv->type->nonOptional();
    if (!to->holds(conv->operand->type->nonOptional()) &&
        !(ByteProofs::known(r) && *r.lo >= to->intMin() &&
          (*r.hi < 0 || uint64_t(*r.hi) <= to->intMax()))) {
      return {};
    }
    return r;
  }
  case Expr::Kind::Name: {
    const VarSymbol *var = cast<NameExpr>(e)->var();
    if (!var || !var->type->isInteger() || var->type->isWrapping()) {
      return {};
    }
    Range r;
    Term self = Term::of(var);
    if (std::optional<int64_t> above =
            this->state.getFlowState().getFacts().bound(self, Term::zero())) {
      r.hi = std::min(*above - 1, kIntMax);
    }
    if (std::optional<int64_t> below =
            this->state.getFlowState().getFacts().bound(Term::zero(), self)) {
      r.lo = std::max(1 - *below, kIntMin);
    }
    return r;
  }
  case Expr::Kind::Member: {
    auto *member = cast<MemberExpr>(e);
    if (member->name == "length" || member->name == "byteLength") {
      return {0, Type::kMaxLength};
    }
    return {};
  }
  case Expr::Kind::Index: {
    auto *index = cast<IndexExpr>(e);
    int64_t lo, hi;
    if (index->object->type && index->object->type->elementRange(lo, hi)) {
      return {lo, hi};
    }
    return {};
  }
  case Expr::Kind::Call: {
    auto *call = cast<CallExpr>(e);
    int64_t lo, hi;
    switch (call->builtin()) {
      case Builtin::StrCharCodeAt:
        return {0, 0xFFFF};
      case Builtin::StrCodePointAt:
        return {0, 0x10FFFF};
      case Builtin::BytesAt:
        if (cast<MemberExpr>(call->callee)->object->type->elementRange(lo, hi)) {
          return {lo, hi};
        }
        return {};
      case Builtin::NumConvert:
        return this->valueRange(cast<MemberExpr>(call->callee)->object);
      case Builtin::DataViewGet:
      case Builtin::BufferRead:
        if (Type::bytesType(Bytes(cast<IntLitExpr>(call->args[0])->value))->elementRange(lo, hi)) {
          return {lo, hi};
        }
        return {};
      case Builtin::MathMin:
      case Builtin::MathMax: {
        if (call->hasSpread() || call->args.empty()) {
          return {};
        }
        bool min = call->builtin() == Builtin::MathMin;
        Range r = this->valueRange(call->args[0]);
        for (size_t i = 1; i < call->args.size(); ++i) {
          Range next = this->valueRange(call->args[i]);
          Range both;
          if (min) {
            if (r.hi || next.hi) {
              both.hi = r.hi && next.hi ? std::min(*r.hi, *next.hi) : r.hi ? *r.hi : *next.hi;
            }
            if (r.lo && next.lo) {
              both.lo = std::min(*r.lo, *next.lo);
            }
          } else {
            if (r.lo || next.lo) {
              both.lo = r.lo && next.lo ? std::max(*r.lo, *next.lo) : r.lo ? *r.lo : *next.lo;
            }
            if (r.hi && next.hi) {
              both.hi = std::max(*r.hi, *next.hi);
            }
          }
          r = both;
        }
        return r;
      }
      default:
        return {};
    }
  }
  case Expr::Kind::Unary: {
    auto *unary = cast<UnaryExpr>(e);
    Range r = this->valueRange(unary->operand);
    if (unary->op == UnaryOp::Plus) {
      return r;
    }
    if (!ByteProofs::known(r)) {
      return {};
    }
    if (unary->op == UnaryOp::Neg) {
      return BytesCheckHelpers::operate(BinaryOp::Sub, ByteProofs::exact(0), r, false);
    }
    if (unary->op == UnaryOp::BitNot) {
      return {-*r.hi - 1, -*r.lo - 1};
    }
    return {};
  }
  case Expr::Kind::Binary: {
    auto *bin = cast<BinaryExpr>(e);
    if (bin->op == BinaryOp::Nullish) {
      return ByteProofs::unite(this->valueRange(bin->lhs), this->valueRange(bin->rhs));
    }
    return BytesCheckHelpers::operate(bin->op, this->valueRange(bin->lhs),
                                      this->valueRange(bin->rhs), false);
  }
  case Expr::Kind::Conditional: {
    auto *cond = cast<ConditionalExpr>(e);
    return ByteProofs::unite(this->valueRange(cond->thenExpr), this->valueRange(cond->elseExpr));
  }
  case Expr::Kind::Sequence:
    return this->valueRange(cast<SequenceExpr>(e)->exprs.back());
  default:
    return {};
  }
}

bool ByteProofs::provesFits(const Expr *expr, int64_t lo, int64_t hi) const {
  return IndexProofs(this->state).provesBetween(expr, lo, hi);
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
  if (std::optional<int64_t> c = ConstantFolder(this->state).intConstant(value)) {
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
      IndexProofs(this->state).exempt(buffer->loc(), buffer)) {
    return;
  }
  IndexProofs proofs(this->state);
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
      offset ? ConstantFolder(this->state).intConstant(offset) : std::optional<int64_t>(0);
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
  std::optional<Linear> at =
      offset ? IndexProofs(this->state).linear(offset) : Linear{Term::zero(), 0};
  std::optional<Linear> length = IndexProofs(this->state).lengthOf(object);
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
