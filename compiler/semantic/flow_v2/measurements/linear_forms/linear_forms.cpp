#include "compiler/semantic/flow_v2/measurements/linear_forms/linear_forms.h"
#include "compiler/program/library/builtin_names/builtin_property_table/builtin_property_table.h"
#include "compiler/semantic/flow/children_first/children_first.h"
#include "compiler/semantic/library/bytes/bytes_check_helpers/bytes_check_helpers.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/MathExtras.h"

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

namespace {

constexpr uint64_t kMaxConstant = uint64_t(1) << 40;

}

LinearForms::LinearForms(SemanticState &owner, BuiltinLengths &sharedBuiltinLengths)
    : state(owner),
      builtinLengths(sharedBuiltinLengths) {}

const VarSymbol *LinearForms::variableOf(const Expr *expr) const {
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

std::optional<Linear> LinearForms::linear(const Expr *expr) const {
  return this->proofAnswer(ProofNode{.ask = ProofAsk::LINEAR, .expr = expr});
}

std::optional<Linear> LinearForms::lengthOf(const Expr *expr) const {
  return this->proofAnswer(ProofNode{.ask = ProofAsk::LENGTH, .expr = expr});
}

std::optional<Linear> LinearForms::proofAnswer(ProofNode root) const {
  auto answers = std::vector<Linear>();
  for (const ProofNode &node : ChildrenFirst::order(root, this, &LinearForms::proofParts)) {
    auto answer = this->nodeAnswer(node, answers);
    if (!answer.has_value()) {
      return std::nullopt;
    }
    answers.push_back(*answer);
  }

  return answers.back();
}

NodeChildren<ProofNode> LinearForms::proofParts(ProofNode node) const {
  if (node.ask == ProofAsk::LINEAR) {
    return this->linearParts(node.expr);
  }

  return this->lengthParts(node.expr);
}

std::optional<Linear> LinearForms::nodeAnswer(const ProofNode &node,
                                              std::vector<Linear> &answers) const {
  if (node.ask == ProofAsk::LINEAR) {
    return this->nodeLinear(node.expr, answers);
  }

  return this->nodeLength(node.expr, answers);
}

NodeChildren<ProofNode> LinearForms::linearParts(const Expr *expr) const {
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

NodeChildren<ProofNode> LinearForms::lengthParts(const Expr *expr) const {
  const auto *source = this->builtinLengths.sameLengthSource(expr);
  auto *call = dyn_cast<CallExpr>(source);
  if (call == nullptr) {
    return {};
  }
  if (this->builtinLengths.isSizedAllocation(call)) {
    return {ProofNode{.ask = ProofAsk::LINEAR, .expr = call->args[0]}};
  }
  const auto *made = this->builtinLengths.freshBufferSource(call);
  if (made == nullptr) {
    return {};
  }

  return {ProofNode{.ask = ProofAsk::LENGTH, .expr = made}};
}

bool LinearForms::isRawBuffer(const Type *type) const {
  return type != nullptr &&
         (type->bytes() == Type::Bytes::ArrayBuffer || type->bytes() == Type::Bytes::DataView);
}

const Expr *LinearForms::measuredObject(const Expr *expr) const {
  auto *member = dyn_cast<MemberExpr>(expr);
  auto *call = dyn_cast<CallExpr>(expr);
  if (member != nullptr) {
    return this->measuredMember(member);
  }
  if (call != nullptr) {
    return this->builtinLengths.measuredCall(call);
  }

  return nullptr;
}

const Expr *LinearForms::measuredMember(const MemberExpr *member) const {
  bool isBuffer = this->isRawBuffer(member->object->type);
  bool isLength = BuiltinPropertyTable::isName(member->name, BuiltinProperty::LENGTH);
  bool isByteLength =
      BuiltinPropertyTable::isName(member->name, BuiltinProperty::BYTE_LENGTH) && isBuffer;
  bool isMeasured = (isLength || isByteLength) && member->type->isInt();

  return isMeasured ? member->object : nullptr;
}

bool LinearForms::isIntWidening(const CastExpr *conversion) const {
  return conversion->isImplicit() && conversion->operand->type->isInt() &&
         conversion->type->isInteger();
}

bool LinearForms::isLinearStep(const BinaryExpr *binary) const {
  bool isAddOrSub = binary->op == BinaryOp::Add || binary->op == BinaryOp::Sub;

  return isAddOrSub && binary->type->isInteger();
}

std::optional<Linear> LinearForms::nodeLinear(const Expr *expr,
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

std::optional<Linear> LinearForms::literalLinear(const IntLitExpr *literal) const {
  if (!literal->type->isInteger() || literal->value > kMaxConstant) {
    return std::nullopt;
  }

  return Linear{Term::zero(), static_cast<int64_t>(literal->value)};
}

std::optional<Linear> LinearForms::negatedLinear(const UnaryExpr *unary) const {
  auto *literal = dyn_cast<IntLitExpr>(unary->operand);
  bool isNegatedLiteral = unary->op == UnaryOp::Neg && literal != nullptr;
  if (!isNegatedLiteral || !unary->type->isInteger() || literal->value > kMaxConstant) {
    return std::nullopt;
  }

  return Linear{Term::zero(), -static_cast<int64_t>(literal->value)};
}

std::optional<Linear> LinearForms::nameLinear(const NameExpr *name) const {
  const VarSymbol *variable = name->var();
  if (variable == nullptr || !variable->type->isInt()) {
    return std::nullopt;
  }

  return Linear{Term::of(variable), 0};
}

std::optional<Linear> LinearForms::measuredLinear(const Expr *expr,
                                                  std::vector<Linear> &answers) const {
  if (this->measuredObject(expr) == nullptr) {
    return std::nullopt;
  }

  return ChildrenFirst::takeAnswer(answers);
}

std::optional<Linear> LinearForms::wideningLinear(const CastExpr *conversion,
                                                  std::vector<Linear> &answers) const {
  if (!this->isIntWidening(conversion)) {
    return std::nullopt;
  }

  return ChildrenFirst::takeAnswer(answers);
}

std::optional<Linear> LinearForms::stepLinear(const BinaryExpr *binary,
                                              std::vector<Linear> &answers) const {
  if (!this->isLinearStep(binary)) {
    return std::nullopt;
  }
  Linear right = ChildrenFirst::takeAnswer(answers);
  Linear left = ChildrenFirst::takeAnswer(answers);

  return this->linearSum(binary, left, right);
}

std::optional<Linear> LinearForms::linearSum(const BinaryExpr *binary, const Linear &left,
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

std::optional<Linear> LinearForms::termSum(const BinaryExpr *binary, const Linear &left,
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

bool LinearForms::isFittingInt(const Linear &sum) const {
  const auto &facts = this->state.walk.flow.getFacts();
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

std::optional<Linear> LinearForms::nodeLength(const Expr *expr,
                                              std::vector<Linear> &answers) const {
  const auto *object = this->builtinLengths.sameLengthSource(expr);
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
    return this->builtinLengths.callLength(call, answers);
  }

  return std::nullopt;
}

std::optional<Linear> LinearForms::listLiteralLength(const ArrayLitExpr *list) const {
  if (this->hasSpreadElement(list)) {
    return std::nullopt;
  }

  return Linear{Term::zero(), static_cast<int64_t>(list->elements.size())};
}

bool LinearForms::hasSpreadElement(const ArrayLitExpr *list) const {
  for (const ArrayLitExpr::Element &element : list->elements) {
    if (element.spread) {
      return true;
    }
  }

  return false;
}

std::optional<Linear> LinearForms::variableLength(const VarSymbol *variable) const {
  auto fixed = this->state.flow.lengths.findFixedLength(variable);
  if (fixed.has_value()) {
    return Linear{Term::zero(), *fixed};
  }
  const auto *declared = variable->type->nonOptional();
  const auto *type = this->state.generics.apparent.apparentType(declared);
  bool isMeasured = type->isString() || type->isArray() || type->isByteView() ||
                    type->bytes() == Type::Bytes::ArrayBuffer;
  if (!isMeasured) {
    return std::nullopt;
  }

  return Linear{Term::lengthOf(variable), 0};
}

std::optional<int64_t> LinearForms::exactLength(const Expr *object) const {
  auto length = this->knownLength(object);

  return this->exactFrom(length);
}

std::optional<int64_t> LinearForms::exactFrom(std::optional<Linear> length) const {
  if (!length.has_value()) {
    return std::nullopt;
  }
  auto value = this->settled(*length);
  if (!value.term.isZero()) {
    return std::nullopt;
  }

  return value.offset;
}

std::optional<Linear> LinearForms::viewOver(const CallExpr *call,
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

std::optional<Linear> LinearForms::viewStart(const CallExpr *call) const {
  if (call->args.size() > 1) {
    return this->linear(call->args[1]);
  }

  return Linear{Term::zero(), 0};
}

std::optional<Linear> LinearForms::lengthAtMost(const Expr *expr) const {
  auto exact = this->lengthOf(expr);
  if (exact.has_value()) {
    return exact;
  }
  auto *call = dyn_cast<CallExpr>(expr);
  if (call == nullptr || !this->isTwoArgumentPart(call)) {
    return std::nullopt;
  }
  auto from = this->state.code.constants.intConstant(call->args[0]);
  auto to = this->state.code.constants.intConstant(call->args[1]);
  if (!from.has_value() || !to.has_value() || *from < 0 || *to < *from) {
    return std::nullopt;
  }

  return Linear{Term::zero(), *to - *from};
}

bool LinearForms::isTwoArgumentPart(const CallExpr *call) const {
  bool isPart = call->builtin() == Builtin::BytesSubarray || call->builtin() == Builtin::BytesSlice;

  return isPart && call->args.size() == 2;
}

Linear LinearForms::settled(Linear value) const {
  if (value.term.isZero()) {
    return value;
  }
  const auto &facts = this->state.walk.flow.getFacts();
  auto above = facts.bound(value.term, Term::zero());
  auto below = facts.bound(Term::zero(), value.term);
  bool isPinned = above.has_value() && below.has_value() && *above - 1 == 1 - *below;
  if (!isPinned) {
    return value;
  }

  return Linear{Term::zero(), *above - 1 + value.offset};
}

IntegerRange LinearForms::lengthRange(const Expr *value) const {
  const auto *source = this->viewedSource(value);
  auto *call = dyn_cast<CallExpr>(source);
  if (call == nullptr || !this->builtinLengths.isSizedAllocation(call)) {
    return {};
  }
  if (call->args.empty()) {
    return IntegerRange{.lo = 0, .hi = 0};
  }

  return this->state.flow.measurements.valueRange(call->args[0]);
}

const Expr *LinearForms::viewedSource(const Expr *value) const {
  const Expr *source = value;
  const auto *view = this->wholeByteView(source);
  while (view != nullptr) {
    source = view->args[0];
    view = this->wholeByteView(source);
  }

  return source;
}

const CallExpr *LinearForms::wholeByteView(const Expr *value) const {
  auto *call = dyn_cast<CallExpr>(value);
  if (call == nullptr) {
    return nullptr;
  }
  bool isView = call->builtin() == Builtin::DataViewNew || call->builtin() == Builtin::BytesView;
  bool isWhole = call->args.size() == 1 && call->type != nullptr && call->type->elementSize() == 1;

  return isView && isWhole ? call : nullptr;
}

std::optional<Linear> LinearForms::knownLength(const Expr *value) const {
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

const CallExpr *LinearForms::innerView(const Expr *value) const {
  auto *call = dyn_cast<CallExpr>(value);
  if (call == nullptr) {
    return nullptr;
  }
  bool isView = call->builtin() == Builtin::DataViewNew || call->builtin() == Builtin::BytesView;

  return isView && call->args.size() <= 2 ? call : nullptr;
}

std::optional<Linear> LinearForms::baseLength(const Expr *value) const {
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

std::optional<Linear> LinearForms::constructedLength(const NewExpr *create) const {
  if (create->args.size() == 1) {
    return this->linear(create->args[0]);
  }

  return Linear{Term::zero(), 0};
}

std::optional<Linear> LinearForms::callBaseLength(const CallExpr *call) const {
  Builtin builtin = call->builtin();
  bool isCopy = builtin == Builtin::BytesFromArray || builtin == Builtin::BytesFromBytes;
  bool isView = builtin == Builtin::DataViewNew || builtin == Builtin::BytesView;
  if (this->builtinLengths.isSizedAllocation(call)) {
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

std::optional<Linear> LinearForms::allocatedLength(const CallExpr *call) const {
  if (call->args.empty()) {
    return Linear{Term::zero(), 0};
  }

  return this->linear(call->args[0]);
}

}
