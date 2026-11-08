#include "compiler/semantic/flow/index_proofs/index_proofs.h"
#include "compiler/program/ast/await_expr/await_expr.h"
#include "compiler/semantic/declare/constant_folder/constant_folder.h"
#include "compiler/semantic/generics/generic_checker/generic_checker.h"
#include "compiler/semantic/library/bytes/byte_proofs/byte_proofs.h"
#include "compiler/semantic/library/bytes/bytes_check_helpers/bytes_check_helpers.h"
#include "compiler/semantic/library/tz_modules/library_calls/library_calls.h"
#include "llvm/ADT/STLExtras.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;

namespace tz {

namespace {

constexpr uint64_t kMaxConstant = uint64_t(1) << 40;

}

const VarSymbol *IndexProofs::variableOf(const Expr *expr) {
  while (auto *conv = dyn_cast<CastExpr>(expr)) {
    if (!conv->isImplicit()) {
      return nullptr;
    }
    expr = conv->operand;
  }
  auto *name = dyn_cast<NameExpr>(expr);

  return name ? name->var() : nullptr;
}

IndexProofs::IndexProofs(SemanticState &owner)
    : state(owner) {}

std::optional<Linear> IndexProofs::linear(const Expr *expr) const {
  switch (expr->kind()) {
  case Expr::Kind::IntLit: {
    auto *lit = cast<IntLitExpr>(expr);
    if (!expr->type->isInteger() || lit->value > kMaxConstant) {
      return std::nullopt;
    }
    return Linear{Term::zero(), static_cast<int64_t>(lit->value)};
  }
  case Expr::Kind::Unary: {
    auto *unary = cast<UnaryExpr>(expr);
    auto *lit = dyn_cast<IntLitExpr>(unary->operand);
    if (unary->op != UnaryOp::Neg || !lit || !expr->type->isInteger() ||
        lit->value > kMaxConstant) {
      return std::nullopt;
    }
    return Linear{Term::zero(), -static_cast<int64_t>(lit->value)};
  }
  case Expr::Kind::Name: {
    const VarSymbol *var = cast<NameExpr>(expr)->var();
    if (var && var->type->isInt()) {
      return Linear{Term::of(var), 0};
    }
    return std::nullopt;
  }
  case Expr::Kind::Member: {
    auto *member = cast<MemberExpr>(expr);
    if (member->name == "length" && expr->type->isInt()) {
      return this->lengthOf(member->object);
    }
    if (member->name == "byteLength" && expr->type->isInt() && member->object->type &&
        (member->object->type->bytes() == Type::Bytes::ArrayBuffer ||
         member->object->type->bytes() == Type::Bytes::DataView)) {
      return this->lengthOf(member->object);
    }
    return std::nullopt;
  }
  case Expr::Kind::Cast: {
    auto *conv = cast<CastExpr>(expr);
    if (conv->isImplicit() && conv->operand->type->isInt() && conv->type->isInteger()) {
      return this->linear(conv->operand);
    }
    return std::nullopt;
  }
  case Expr::Kind::Call: {
    auto *call = cast<CallExpr>(expr);
    auto *member = dyn_cast<MemberExpr>(call->callee);
    if (call->builtin() == Builtin::BytesByteLength && member && member->object->type &&
        member->object->type->isTypedArray() && member->object->type->elementSize() == 1) {
      return this->lengthOf(member->object);
    }
    return std::nullopt;
  }
  case Expr::Kind::Binary: {
    auto *bin = cast<BinaryExpr>(expr);
    if ((bin->op != BinaryOp::Add && bin->op != BinaryOp::Sub) || !expr->type->isInteger()) {
      return std::nullopt;
    }
    std::optional<Linear> l = this->linear(bin->lhs);
    std::optional<Linear> r = this->linear(bin->rhs);
    if (!l || !r) {
      return std::nullopt;
    }
    Linear result;
    if (r->term.isZero()) {
      result = {l->term, bin->op == BinaryOp::Add ? l->offset + r->offset : l->offset - r->offset};
    } else if (bin->op == BinaryOp::Add && l->term.isZero()) {
      result = {r->term, l->offset + r->offset};
    } else if (std::optional<Term> both = Term::sum(l->term, r->term);
               both && bin->op == BinaryOp::Add) {
      result = {*both, l->offset + r->offset};
    } else {
      return std::nullopt;
    }
    if (result.offset > int64_t(kMaxConstant) || result.offset < -int64_t(kMaxConstant)) {
      return std::nullopt;
    }
    if (expr->type->isInt() && !result.term.isZero()) {
      const Facts &facts = this->state.getFlowState().getFacts();
      if ((result.offset > 0 &&
           !facts.proves(result.term, Term::zero(), kIntMax - (result.offset - 1))) ||
          (result.offset < 0 && !facts.proves(Term::zero(), result.term,
                                              Facts::saturatingSub(result.offset + 1, kIntMin)))) {
        return std::nullopt;
      }
    }
    return result;
  }
  default:
    return std::nullopt;
  }
}

std::optional<Linear> IndexProofs::lengthOf(const Expr *object) const {
  if (auto *lit = dyn_cast<StringLitExpr>(object)) {
    return Linear{Term::zero(), static_cast<int64_t>(lit->value.size())};
  }
  if (auto *lit = dyn_cast<ArrayLitExpr>(object)) {
    if (llvm::any_of(lit->elements, [](const ArrayLitExpr::Element &e) { return e.spread; })) {
      return std::nullopt;
    }
    return Linear{Term::zero(), static_cast<int64_t>(lit->elements.size())};
  }
  if (const VarSymbol *var = IndexProofs::variableOf(object)) {
    if (std::optional<int64_t> fixed = this->state.getFlowState().findFixedLength(var)) {
      return Linear{Term::zero(), *fixed};
    }
    const Type *type = GenericChecker::apparentType(var->type->nonOptional());
    if (type->isString() || type->isArray() || type->isByteView() ||
        type->bytes() == Type::Bytes::ArrayBuffer) {
      return Linear{Term::lengthOf(var), 0};
    }
  }
  if (auto *wait = dyn_cast<AwaitExpr>(object)) {
    if (auto *all = dyn_cast<CallExpr>(wait->value);
        all && all->builtin() == Builtin::PromiseAll && all->args.size() == 1) {
      return this->lengthOf(all->args[0]);
    }
  }
  if (auto *call = dyn_cast<CallExpr>(object); call && call->builtin() == Builtin::DecToFraction) {
    return Linear{Term::zero(), 2};
  }
  if (auto *call = dyn_cast<CallExpr>(object); call && call->builtin() == Builtin::BytesOf) {
    return Linear{Term::zero(), static_cast<int64_t>(call->args.size())};
  }
  if (auto *call = dyn_cast<CallExpr>(object); call && call->builtin() == Builtin::EmbedBytes) {
    return Linear{Term::zero(),
                  static_cast<int64_t>(cast<StringLitExpr>(call->args[0])->value.size())};
  }
  auto *call = dyn_cast<CallExpr>(object);
  if (!call) {
    return std::nullopt;
  }
  switch (call->builtin()) {
    case Builtin::BytesNew:
    case Builtin::ArrayBufferNew:
    case Builtin::BufferAllocFill:
      if (std::optional<Linear> n = this->linear(call->args[0]); n && n->term.isZero()) {
        return n;
      }
      return std::nullopt;
    case Builtin::BytesFromArray:
    case Builtin::BytesFromBytes:
      return this->lengthOf(call->args[0]);
    case Builtin::BufferFromString:
      return this->utf8Length(call);
    case Builtin::BytesBuffer:
      return this->freshBufferLength(call);
    default:
      return std::nullopt;
  }
}

std::optional<Linear> IndexProofs::freshBufferLength(const CallExpr *call) const {
  auto *member = dyn_cast<MemberExpr>(call->callee);
  auto *made = member ? dyn_cast<CallExpr>(member->object) : nullptr;
  if (!made || !made->type || !made->type->isTypedArray() || made->type->isBuffer()) {
    return std::nullopt;
  }
  switch (made->builtin()) {
    case Builtin::BytesNew:
    case Builtin::BytesFromArray:
    case Builtin::BytesFromBytes:
    case Builtin::BytesOf:
      break;
    default:
      return std::nullopt;
  }
  std::optional<Linear> count = this->lengthOf(made);
  if (!count || !count->term.isZero()) {
    return std::nullopt;
  }

  return Linear{Term::zero(), count->offset * int64_t(made->type->elementSize())};
}

std::optional<Linear> IndexProofs::utf8Length(const CallExpr *call) const {
  auto *text = dyn_cast<StringLitExpr>(call->args[0]);
  auto *encoding = call->args.size() > 1 ? dyn_cast_or_null<StringLitExpr>(call->args[1]) : nullptr;
  if (!text || (encoding && encoding->value != u"utf8" && encoding->value != u"utf-8")) {
    return std::nullopt;
  }
  int64_t n = 0;
  const std::u16string &s = text->value;
  for (size_t i = 0; i < s.size(); ++i) {
    char16_t c = s[i];
    if (c < 0x80) {
      n += 1;
    } else if (c < 0x800) {
      n += 2;
    } else if (c >= 0xD800 && c <= 0xDBFF && i + 1 < s.size() && s[i + 1] >= 0xDC00 &&
               s[i + 1] <= 0xDFFF) {
      n += 4;
      ++i;
    } else {
      n += 3;
    }
  }

  return Linear{Term::zero(), n};
}

bool IndexProofs::provesInBounds(const Expr *object, const Expr *index, bool allowEnd) const {
  std::optional<Linear> at = this->linear(index);
  std::optional<Linear> length = this->lengthOf(object);
  if (!length) {
    return false;
  }
  if (at && this->state.getFlowState().getFacts().proves(Term::zero(), at->term, at->offset + 1) &&
      this->state.getFlowState().getFacts().proves(
          at->term, length->term, length->offset - at->offset + (allowEnd ? 1 : 0))) {
    return true;
  }
  IntegerRange range = ByteProofs(this->state).valueRange(index);

  return range.lo && range.hi && *range.lo >= 0 &&
         this->state.getFlowState().getFacts().proves(
             Term::zero(), length->term, length->offset - *range.hi + (allowEnd ? 1 : 0));
}

bool IndexProofs::provesAtInBounds(const Expr *object, const Expr *index) const {
  std::optional<Linear> at = this->linear(index);
  std::optional<Linear> length = this->lengthOf(object);
  if (at && length && at->term.isZero() && at->offset < 0) {
    return this->state.getFlowState().getFacts().proves(Term::zero(), length->term,
                                                        length->offset + at->offset + 1);
  }

  return this->provesInBounds(object, index, false);
}

bool IndexProofs::provesNonNegative(const Expr *expr) const {
  std::optional<Linear> value = this->linear(expr);

  return value &&
         this->state.getFlowState().getFacts().proves(Term::zero(), value->term, value->offset + 1);
}

bool IndexProofs::provesNotEmpty(const Expr *object) const {
  std::optional<Linear> length = this->lengthOf(object);

  return length &&
         this->state.getFlowState().getFacts().proves(Term::zero(), length->term, length->offset);
}

bool IndexProofs::provesBetween(const Expr *expr, int64_t lo, std::optional<int64_t> hi) const {
  while (auto *conv = dyn_cast<CastExpr>(expr)) {
    const Type *from = conv->operand->type;
    if (!conv->isImplicit() || !from || !from->isInteger() ||
        !(conv->type->isFloat() ||
          (conv->type->isInteger() && conv->type->bitWidth() >= from->bitWidth()))) {
      break;
    }
    expr = conv->operand;
  }
  if (auto *cond = dyn_cast<ConditionalExpr>(expr)) {
    return this->provesBetween(cond->thenExpr, lo, hi) &&
           this->provesBetween(cond->elseExpr, lo, hi);
  }
  if (!expr->type || !expr->type->nonOptional()->isInteger()) {
    return false;
  }
  const Facts &facts = this->state.getFlowState().getFacts();
  bool lower = false;
  bool upper = !hi;
  if (std::optional<Linear> l = this->linear(expr)) {
    if (l->term.isZero()) {
      lower = l->offset >= lo;
      upper |= hi && l->offset <= *hi;
    } else {
      lower = facts.proves(Term::zero(), l->term, l->offset - lo + 1);
      upper |= hi && facts.proves(l->term, Term::zero(), *hi - l->offset + 1);
    }
  }
  IntegerRange range = ByteProofs(this->state).valueRange(expr);
  lower |= range.lo && *range.lo >= lo;
  upper |= hi && range.hi && *range.hi <= *hi;

  return lower && upper;
}

bool IndexProofs::exempt(llvm::SMLoc loc, const Expr *object) const {
  auto never = [](const Type *type) {
    return type && (type->isUndefined() || type->nonOptional()->isNever());
  };
  if (object && never(object->type)) {
    return true;
  }
  if (const VarSymbol *var = object ? IndexProofs::variableOf(object) : nullptr;
      var && never(var->type)) {
    return true;
  }

  return this->state.getFlowState().isUnreachable() || LibraryCalls(this->state).inLibrary(loc);
}

IndexProofs::ResultBetweenProven IndexProofs::requireBetween(const Expr *expr, int64_t lo,
                                                             std::optional<int64_t> hi,
                                                             llvm::StringRef what) {
  if (!expr->type || expr->type->isError() || this->provesBetween(expr, lo, hi)) {
    return ok(expr);
  }
  if (this->exempt(expr->loc())) {
    return err(IndexProofsRequireBetweenError{
        .code = IndexProofsRequireBetweenErrorCode::EXEMPT,
        .message = "",
    });
  }

  std::string range = ErrorTextBetween::range(lo, hi);
  std::optional<int64_t> constant = ConstantFolder(this->state).intConstant(expr);
  if (constant && !hi && lo == 0) {
    std::string message = ErrorTextBetween::negative(what, *constant);

    return err(IndexProofsRequireBetweenError{
        .code = IndexProofsRequireBetweenErrorCode::CONSTANT_NEGATIVE,
        .message = message,
    });
  }
  if (constant) {
    std::string message = ErrorTextBetween::outside(what, range, *constant);

    return err(IndexProofsRequireBetweenError{
        .code = IndexProofsRequireBetweenErrorCode::CONSTANT_OUT_OF_RANGE,
        .message = message,
    });
  }
  if (!expr->type->isInteger()) {
    std::string message = ErrorTextBetween::notWhole(what, range, expr->type->name());

    return err(IndexProofsRequireBetweenError{
        .code = IndexProofsRequireBetweenErrorCode::NOT_WHOLE_NUMBER,
        .message = message,
    });
  }

  std::string text = BytesCheckHelpers::valueText(expr);
  std::string test = ErrorTextBetween::test(text, lo, hi);
  std::string message = ErrorTextBetween::unproven(what, range, test);

  return err(IndexProofsRequireBetweenError{
      .code = IndexProofsRequireBetweenErrorCode::NOT_PROVEN,
      .message = message,
  });
}

bool IndexProofs::provesLess(const Expr *a, const Expr *b) const {
  std::optional<Linear> la = this->linear(a);
  std::optional<Linear> lb = this->linear(b);
  if (!la || !lb) {
    return false;
  }
  if (la->term.isZero() && lb->term.isZero()) {
    return la->offset < lb->offset;
  }

  return this->state.getFlowState().getFacts().proves(la->term, lb->term, lb->offset - la->offset);
}

IndexProofs::ResultLessWasProven IndexProofs::requireLess(const Expr *a, const Expr *b,
                                                          llvm::StringRef what) {
  if (!a->type || !b->type || a->type->isError() || b->type->isError() || this->provesLess(a, b) ||
      this->exempt(b->loc())) {
    return ok(b);
  }

  std::string left = BytesCheckHelpers::valueText(a);
  std::string right = BytesCheckHelpers::valueText(b);
  std::string message = ErrorTextLess::unproven(what, left, right);

  return err(IndexProofsRequireLessError{
      .code = IndexProofsRequireLessErrorCode::NOT_PROVEN,
      .message = message,
  });
}

bool IndexProofs::provesLinearUpTo(Linear value, const Expr *object, int64_t less) const {
  std::optional<Linear> length = this->lengthOf(object);
  const Expr *owner = object;
  if (auto *call = dyn_cast<CallExpr>(object); call && call->builtin() == Builtin::BytesBuffer) {
    owner = call->callee;
  }
  if (auto *member = dyn_cast<MemberExpr>(owner); !length && member && member->name == "buffer" &&
                                                  member->object->type &&
                                                  member->object->type->isByteView()) {
    int64_t size = member->object->type->elementSize();
    std::optional<Linear> view = this->lengthOf(member->object);
    if (view) {
      view = this->settled(*view);
    }
    if (view && size == 1) {
      length = view;
    } else if (view && view->term.isZero()) {
      length = Linear{Term::zero(), view->offset * size};
    }
  }
  if (!length) {
    return false;
  }

  return this->state.getFlowState().getFacts().proves(value.term, length->term,
                                                      length->offset - less - value.offset + 1);
}

bool IndexProofs::provesUpTo(const Expr *value, const Expr *object, int64_t less) const {
  if (!value) {
    return this->provesLinearUpTo(Linear{Term::zero(), 0}, object, less);
  }
  if (!this->provesBetween(value, 0, std::nullopt)) {
    return false;
  }
  if (std::optional<Linear> at = this->linear(value);
      at && this->provesLinearUpTo(*at, object, less)) {
    return true;
  }
  IntegerRange range = ByteProofs(this->state).valueRange(value);

  return range.hi && this->provesLinearUpTo(Linear{Term::zero(), *range.hi}, object, less);
}

IndexProofs::ResultUpToWasProven IndexProofs::requireUpTo(const Expr *value, const Expr *object,
                                                          int64_t less, llvm::StringRef what) {
  if ((value && (!value->type || value->type->isError())) ||
      this->provesUpTo(value, object, less)) {
    return ok(object);
  }
  if (this->exempt(object->loc(), object)) {
    return err(IndexProofsRequireUpToError{
        .code = IndexProofsRequireUpToErrorCode::EXEMPT,
        .message = "",
    });
  }

  std::optional<std::string> text =
      value ? std::optional<std::string>(BytesCheckHelpers::valueText(value)) : std::nullopt;
  bool bytes = object->type && (object->type->bytes() == Type::Bytes::ArrayBuffer ||
                                object->type->bytes() == Type::Bytes::DataView);
  std::string limit = ErrorTextUpTo::limit(BytesCheckHelpers::valueText(object), bytes, less);
  std::string message = ErrorTextUpTo::unproven(what, text, limit);

  return err(IndexProofsRequireUpToError{
      .code = IndexProofsRequireUpToErrorCode::NOT_PROVEN,
      .message = message,
  });
}

std::optional<int64_t> IndexProofs::exactLength(const Expr *object) const {
  std::optional<Linear> length = this->knownLength(object);
  if (!length) {
    return std::nullopt;
  }
  if (length->term.isZero()) {
    return length->offset;
  }
  const Facts &facts = this->state.getFlowState().getFacts();
  std::optional<int64_t> above = facts.bound(length->term, Term::zero());
  std::optional<int64_t> below = facts.bound(Term::zero(), length->term);
  if (!above || !below || *above - 1 != 1 - *below) {
    return std::nullopt;
  }

  return *above - 1 + length->offset;
}

std::optional<Linear> IndexProofs::viewLength(const CallExpr *call) const {
  int64_t size = call->type ? call->type->elementSize() : 1;
  if (call->args.size() > 2) {
    return this->linear(call->args[2]);
  }
  std::optional<Linear> whole = this->knownLength(call->args[0]);
  std::optional<Linear> from =
      call->args.size() > 1 ? this->linear(call->args[1]) : Linear{Term::zero(), 0};
  if (!whole || !from || !from->term.isZero()) {
    return std::nullopt;
  }
  if (size == 1) {
    return Linear{whole->term, whole->offset - from->offset};
  }
  std::optional<int64_t> exact = this->exactLength(call->args[0]);
  if (!exact || (*exact - from->offset) % size != 0) {
    return std::nullopt;
  }

  return Linear{Term::zero(), (*exact - from->offset) / size};
}

std::optional<Linear> IndexProofs::lengthAtMost(const Expr *expr) const {
  if (std::optional<Linear> exact = this->lengthOf(expr)) {
    return exact;
  }
  auto *call = dyn_cast<CallExpr>(expr);
  if (!call || call->args.size() != 2 ||
      (call->builtin() != Builtin::BytesSubarray && call->builtin() != Builtin::BytesSlice)) {
    return std::nullopt;
  }
  std::optional<int64_t> from = ConstantFolder(this->state).intConstant(call->args[0]);
  std::optional<int64_t> to = ConstantFolder(this->state).intConstant(call->args[1]);
  if (!from || !to || *from < 0 || *to < *from) {
    return std::nullopt;
  }

  return Linear{Term::zero(), *to - *from};
}

Linear IndexProofs::settled(Linear value) const {
  if (value.term.isZero()) {
    return value;
  }
  const Facts &facts = this->state.getFlowState().getFacts();
  std::optional<int64_t> above = facts.bound(value.term, Term::zero());
  std::optional<int64_t> below = facts.bound(Term::zero(), value.term);
  if (above && below && *above - 1 == 1 - *below) {
    return Linear{Term::zero(), *above - 1 + value.offset};
  }

  return value;
}

bool IndexProofs::provesSumUpTo(std::optional<Linear> a, std::optional<Linear> b,
                                const Expr *object) const {
  if (!a || !b) {
    return false;
  }
  Linear x = this->settled(*a);
  Linear y = this->settled(*b);
  Term term = x.term.isZero() ? y.term : x.term;
  if (!x.term.isZero() && !y.term.isZero()) {
    std::optional<Term> both = Term::sum(x.term, y.term);
    if (!both) {
      return false;
    }
    term = *both;
  }

  return this->provesLinearUpTo(Linear{term, x.offset + y.offset}, object, 0);
}

void IndexProofs::addIndexCallbackFacts(const FunctionDecl *fn) {
  CallbackFacts &callbacks = this->state.getCallbackFacts();
  if (const ElementCallback *element = callbacks.findElementCallback(fn)) {
    auto [param, lo, hi] = *element;
    const VarSymbol *value = param < fn->params.size() ? fn->params[param].sym : nullptr;
    if (value && value->type->isInt()) {
      this->state.getFlowState().getFacts().add(Term::zero(), Term::of(value),
                                                Facts::saturatingSub(1, lo));
      this->state.getFlowState().getFacts().add(Term::of(value), Term::zero(),
                                                Facts::saturatingAdd(hi, 1));
    }
  }
  std::optional<unsigned> index = callbacks.findIndexParam(fn);
  if (!index || *index >= fn->params.size()) {
    return;
  }
  const VarSymbol *var = fn->params[*index].sym;
  if (var && var->type->isInt()) {
    this->state.getFlowState().getFacts().add(Term::zero(), Term::of(var), 1);
    this->state.getFlowState().getFacts().add(Term::of(var), Term::zero(), Type::kMaxLength);
  }
}

IntegerRange IndexProofs::lengthRange(const Expr *value) const {
  auto *call = dyn_cast<CallExpr>(value);
  if (!call) {
    return {};
  }
  switch (call->builtin()) {
    case Builtin::BytesNew:
    case Builtin::ArrayBufferNew:
    case Builtin::BufferAllocFill:
      return call->args.empty() ? IntegerRange{0, 0}
                                : ByteProofs(this->state).valueRange(call->args[0]);
    case Builtin::DataViewNew:
    case Builtin::BytesView:
      if (call->args.size() == 1 && call->type && call->type->elementSize() == 1) {
        return this->lengthRange(call->args[0]);
      }
      return {};
    default:
      return {};
  }
}

std::optional<Linear> IndexProofs::knownLength(const Expr *value) const {
  if (auto *create = dyn_cast<NewExpr>(value)) {
    if (create->args.size() == 1) {
      return this->linear(create->args[0]);
    }
    return Linear{Term::zero(), 0};
  }
  if (auto *call = dyn_cast<CallExpr>(value)) {
    switch (call->builtin()) {
      case Builtin::BytesNew:
      case Builtin::ArrayBufferNew:
      case Builtin::BufferAllocFill:
        return call->args.empty() ? Linear{Term::zero(), 0} : this->linear(call->args[0]);
      case Builtin::BytesOf:
      case Builtin::EmbedBytes:
        return this->lengthOf(call);
      case Builtin::BytesFromArray:
      case Builtin::BytesFromBytes:
        return this->lengthOf(call->args[0]);
      case Builtin::DataViewNew:
      case Builtin::BytesView:
        return this->viewLength(call);
      default:
        break;
    }
  }

  return this->lengthOf(value);
}

}
