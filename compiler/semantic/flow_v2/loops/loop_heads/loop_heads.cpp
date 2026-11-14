#include "compiler/semantic/flow_v2/loops/loop_heads/loop_heads.h"
#include "compiler/program/library/builtin_names/builtin_property_table/builtin_property_table.h"
#include "compiler/semantic/flow/expr_parts/expr_parts.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include "llvm/ADT/STLExtras.h"
#include <functional>
#include <limits>

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

namespace {

constexpr int64_t kIntMin = std::numeric_limits<int64_t>::min();
constexpr int64_t kIntMax = std::numeric_limits<int64_t>::max();
constexpr uint64_t kMaxConstant = uint64_t(1) << 40;

}

LoopHeads::LoopHeads(SemanticState &owner)
    : state(owner) {}

const VarSymbol *LoopHeads::namedVar(const Expr *expr) const {
  auto *name = dyn_cast<NameExpr>(expr);
  if (name == nullptr) {
    return nullptr;
  }

  return this->state.walk.scopes.findName(name->name).var();
}

std::optional<int64_t> LoopHeads::varPlus(const Expr *expr, const VarSymbol *variable) const {
  if (isa<NameExpr>(expr)) {
    return this->namedVar(expr) == variable ? std::optional<int64_t>(0) : std::nullopt;
  }
  auto *binary = dyn_cast<BinaryExpr>(expr);
  if (binary == nullptr || binary->op != BinaryOp::Add) {
    return std::nullopt;
  }
  bool isNameFirst = isa<NameExpr>(binary->lhs);
  const Expr *named = isNameFirst ? binary->lhs : binary->rhs;
  const Expr *other = isNameFirst ? binary->rhs : binary->lhs;
  auto *literal = dyn_cast<IntLitExpr>(other);
  if (!isa<NameExpr>(named) || literal == nullptr || this->namedVar(named) != variable) {
    return std::nullopt;
  }
  if (literal->value > uint64_t(Type::kMaxLength)) {
    return std::nullopt;
  }

  return static_cast<int64_t>(literal->value);
}

bool LoopHeads::isArrayLength(const Term &term) const {
  return term.length && term.var->type->nonOptional()->isArray();
}

std::optional<LoopHeads::IntRange> LoopHeads::syntacticIntRange(const Expr *expr) const {
  auto *literal = dyn_cast<IntLitExpr>(expr);
  auto *unary = dyn_cast<UnaryExpr>(expr);
  auto *name = dyn_cast<NameExpr>(expr);
  auto *member = dyn_cast<MemberExpr>(expr);
  auto *binary = dyn_cast<BinaryExpr>(expr);
  if (literal != nullptr) {
    return this->literalRange(literal, 1);
  }
  if (unary != nullptr) {
    return this->negatedRange(unary);
  }
  if (name != nullptr) {
    return this->nameRange(name);
  }
  if (member != nullptr) {
    return this->memberRange(member);
  }
  if (binary != nullptr) {
    return this->lengthMinusRange(binary);
  }

  return std::nullopt;
}

std::optional<LoopHeads::IntRange> LoopHeads::literalRange(const IntLitExpr *literal,
                                                           int64_t sign) const {
  if (literal->value > uint64_t(kIntMax)) {
    return std::nullopt;
  }
  int64_t value = sign * static_cast<int64_t>(literal->value);

  return IntRange{.low = value, .high = value};
}

std::optional<LoopHeads::IntRange> LoopHeads::negatedRange(const UnaryExpr *unary) const {
  auto *literal = dyn_cast<IntLitExpr>(unary->operand);
  if (unary->op != UnaryOp::Neg || literal == nullptr) {
    return std::nullopt;
  }

  return this->literalRange(literal, -1);
}

std::optional<LoopHeads::IntRange> LoopHeads::nameRange(const NameExpr *name) const {
  const VarSymbol *variable = this->state.walk.scopes.findName(name->name).var();
  if (variable == nullptr || !variable->type->isInt()) {
    return std::nullopt;
  }

  return IntRange{.low = kIntMin, .high = kIntMax};
}

std::optional<LoopHeads::IntRange> LoopHeads::lengthMinusRange(const BinaryExpr *binary) const {
  auto *member = dyn_cast<MemberExpr>(binary->lhs);
  auto *literal = dyn_cast<IntLitExpr>(binary->rhs);
  if (binary->op != BinaryOp::Sub || member == nullptr || literal == nullptr) {
    return std::nullopt;
  }
  if (literal->value > kMaxConstant) {
    return std::nullopt;
  }
  auto length = this->memberRange(member);
  if (!length.has_value()) {
    return std::nullopt;
  }
  auto offset = static_cast<int64_t>(literal->value);

  return IntRange{.low = length->low - offset, .high = length->high - offset};
}

std::optional<LoopHeads::IntRange> LoopHeads::memberRange(const MemberExpr *member) const {
  const VarSymbol *variable = this->namedVar(member->object);
  auto anyLength = IntRange{.low = 0, .high = Type::kMaxLength};
  bool isByteLength = BuiltinPropertyTable::isName(member->name, BuiltinProperty::BYTE_LENGTH);
  if (isByteLength && variable != nullptr && variable->type->isBytes()) {
    return anyLength;
  }
  if (!BuiltinPropertyTable::isName(member->name, BuiltinProperty::LENGTH)) {
    return std::nullopt;
  }
  if (isa<StringLitExpr>(member->object) || isa<ArrayLitExpr>(member->object)) {
    return anyLength;
  }
  if (variable != nullptr && this->isSized(variable->type)) {
    return anyLength;
  }

  return std::nullopt;
}

bool LoopHeads::isSized(const Type *type) const {
  return type->isString() || type->isArray() || type->isTypedArray();
}

const VarSymbol *LoopHeads::unchangedInt(const Expr *expr, const LoopScan &scan) const {
  auto *name = dyn_cast<NameExpr>(expr);
  if (name == nullptr || scan.changes.contains(name->name)) {
    return nullptr;
  }
  const VarSymbol *variable = this->state.walk.scopes.findName(name->name).var();
  if (variable == nullptr || !variable->type->isInt()) {
    return nullptr;
  }

  return variable;
}

bool LoopHeads::isBoundedAbove(const Expr *expr, int64_t limit, const LoopScan &scan) const {
  auto range = this->syntacticIntRange(expr);
  if (range.has_value() && range->high <= limit) {
    return true;
  }
  const VarSymbol *variable = this->unchangedInt(expr, scan);
  if (variable == nullptr) {
    return false;
  }
  int64_t offset = Facts::saturatingAdd(limit, 1);

  return this->state.walk.flow.getFacts().canProve(Term::of(variable), Term::zero(), offset);
}

bool LoopHeads::isBoundedBelow(const Expr *expr, int64_t limit, const LoopScan &scan) const {
  auto range = this->syntacticIntRange(expr);
  if (range.has_value() && range->low >= limit) {
    return true;
  }
  const VarSymbol *variable = this->unchangedInt(expr, scan);
  if (variable == nullptr) {
    return false;
  }
  int64_t offset = Facts::saturatingSub(1, limit);

  return this->state.walk.flow.getFacts().canProve(Term::zero(), Term::of(variable), offset);
}

std::optional<LoopHeads::Comparison> LoopHeads::ordered(const Expr *part) const {
  auto *binary = llvm::dyn_cast_or_null<BinaryExpr>(part);
  if (binary == nullptr) {
    return std::nullopt;
  }
  if (binary->op == BinaryOp::Lt) {
    return Comparison{.small = binary->lhs, .large = binary->rhs, .isStrict = true};
  }
  if (binary->op == BinaryOp::Gt) {
    return Comparison{.small = binary->rhs, .large = binary->lhs, .isStrict = true};
  }
  if (binary->op == BinaryOp::Le) {
    return Comparison{.small = binary->lhs, .large = binary->rhs, .isStrict = false};
  }
  if (binary->op == BinaryOp::Ge) {
    return Comparison{.small = binary->rhs, .large = binary->lhs, .isStrict = false};
  }

  return std::nullopt;
}

std::optional<int64_t> LoopHeads::condBoundsAbove(const Expr *cond, const VarSymbol *variable,
                                                  const LoopScan &scan, int64_t step) const {
  for (const Expr *part : ExprParts::andParts(cond)) {
    auto offset = this->comparisonBoundsAbove(part, variable, scan, step);
    if (offset.has_value()) {
      return offset;
    }
  }

  return std::nullopt;
}

std::optional<int64_t> LoopHeads::comparisonBoundsAbove(const Expr *part, const VarSymbol *variable,
                                                        const LoopScan &scan, int64_t step) const {
  auto comparison = this->ordered(part);
  if (!comparison.has_value()) {
    return std::nullopt;
  }
  auto offset = this->varPlus(comparison->small, variable);
  if (!offset.has_value()) {
    return std::nullopt;
  }
  int64_t limit = comparison->isStrict ? kIntMax - (step - 1) : kIntMax - step;
  if (!this->isBoundedAbove(comparison->large, limit, scan)) {
    return std::nullopt;
  }

  return offset;
}

bool LoopHeads::isCondBoundingBelow(const Expr *cond, const VarSymbol *variable,
                                    const LoopScan &scan, int64_t step) const {
  for (const Expr *part : ExprParts::andParts(cond)) {
    if (this->isComparisonBoundingBelow(part, variable, scan, step)) {
      return true;
    }
  }

  return false;
}

bool LoopHeads::isComparisonBoundingBelow(const Expr *part, const VarSymbol *variable,
                                          const LoopScan &scan, int64_t step) const {
  auto comparison = this->ordered(part);
  if (!comparison.has_value() || this->namedVar(comparison->large) != variable) {
    return false;
  }
  int64_t limit = comparison->isStrict ? kIntMin + step - 1 : kIntMin + step;

  return this->isBoundedBelow(comparison->small, limit, scan);
}

Flow LoopHeads::loopHead(const LoopScan &scan) const {
  auto head = this->state.walk.flow.getFlow();
  head.unreachable = false;
  for (const auto &entry : scan.changes) {
    const VarSymbol *variable = this->state.walk.scopes.findName(entry.first()).var();
    this->forgetChanged(head, entry.first(), variable, KeptBounds{});
  }
  this->forgetLoopEffects(head, scan);

  return head;
}

Flow LoopHeads::countedLoopHead(const LoopScan &scan, const Expr *cond) const {
  auto head = this->state.walk.flow.getFlow();
  head.unreachable = false;
  for (const auto &entry : scan.changes) {
    const VarSymbol *variable = this->state.walk.scopes.findName(entry.first()).var();
    auto kept = this->keptBounds(scan, entry.second, variable, cond);
    this->forgetChanged(head, entry.first(), variable, kept);
  }
  this->forgetLoopEffects(head, scan);

  return head;
}

void LoopHeads::forgetLoopEffects(Flow &head, const LoopScan &scan) const {
  this->forgetWrittenFields(head, scan);
  if (scan.isCallingFunctions) {
    this->state.flow.changes.forgetCallEffects(head);
  }
  this->forgetArrayFacts(head, scan);
}

void LoopHeads::forgetChanged(Flow &head, llvm::StringRef name, const VarSymbol *variable,
                              const KeptBounds &kept) const {
  auto isStale = std::bind_front(&LoopHeads::isStaleFact, this, name, variable, kept);
  head.facts.forgetIf(isStale);
  if (kept.invariant.has_value()) {
    int64_t offset = Facts::saturatingAdd(kIntMax - *kept.invariant, 1);
    head.facts.add(Term::of(variable), Term::zero(), offset);
  }
  this->forgetNamed(head, name);
}

bool LoopHeads::isInvariantBelow(const VarSymbol *variable, int64_t offset) const {
  int64_t limit = Facts::saturatingAdd(kIntMax - offset, 1);

  return this->state.walk.flow.getFacts().canProve(Term::of(variable), Term::zero(), limit);
}

LoopHeads::KeptBounds LoopHeads::keptBounds(const LoopScan &scan, const LoopScan::Change &change,
                                            const VarSymbol *variable, const Expr *cond) const {
  bool isCounter =
      variable != nullptr && variable->type->isInt() && change.count == 1 && !change.isNested;
  if (!isCounter) {
    return KeptBounds{};
  }
  auto offset =
      change.delta > 0 ? this->condBoundsAbove(cond, variable, scan, change.delta) : std::nullopt;
  bool isZeroOffset = offset.has_value() && *offset == 0;
  bool isInvariant =
      offset.has_value() && !isZeroOffset && this->isInvariantBelow(variable, *offset);
  bool isUpperKept =
      change.delta < 0 && this->isCondBoundingBelow(cond, variable, scan, -change.delta);

  return KeptBounds{
      .isLowerKept = isZeroOffset || isInvariant,
      .isUpperKept = isUpperKept,
      .invariant = isInvariant ? offset : std::nullopt,
  };
}

bool LoopHeads::isStaleFact(llvm::StringRef name, const VarSymbol *variable, const KeptBounds &kept,
                            const Facts::Fact &fact) const {
  bool isLhsHit = this->isTermNaming(fact.lhs, name);
  bool isRhsHit = this->isTermNaming(fact.rhs, name);
  if (!isLhsHit && !isRhsHit) {
    return false;
  }
  if (kept.isLowerKept && !isLhsHit && fact.rhs == Term::of(variable)) {
    return false;
  }
  if (kept.isUpperKept && !isRhsHit && fact.lhs == Term::of(variable)) {
    return false;
  }

  return true;
}

bool LoopHeads::isTermNaming(const Term &term, llvm::StringRef name) const {
  bool isFirst = term.var != nullptr && term.var->name == name;
  bool isSecond = term.var2 != nullptr && term.var2->name == name;

  return isFirst || isSecond;
}

bool LoopHeads::isNarrowedNamed(llvm::StringRef name,
                                const std::pair<const VarSymbol *, const Type *> &narrowed) const {
  return narrowed.first->name == name;
}

bool LoopHeads::isPathNamed(llvm::StringRef name, const FieldPath &path) const {
  return path.var->name == name;
}

bool LoopHeads::isNarrowedPathNamed(llvm::StringRef name,
                                    const std::pair<FieldPath, const Type *> &narrowed) const {
  return narrowed.first.var->name == name;
}

void LoopHeads::forgetNamedIn(llvm::SmallPtrSetImpl<const VarSymbol *> &variables,
                              llvm::StringRef name) const {
  for (const VarSymbol *variable : llvm::to_vector(variables)) {
    if (variable->name == name) {
      variables.erase(variable);
    }
  }
}

void LoopHeads::forgetNamed(Flow &head, llvm::StringRef name) const {
  this->forgetNamedIn(head.defined, name);
  llvm::erase_if(head.narrowed, std::bind_front(&LoopHeads::isNarrowedNamed, this, name));
  this->forgetNamedIn(head.okResults, name);
  this->forgetNamedIn(head.errResults, name);
  llvm::erase_if(head.definedFields, std::bind_front(&LoopHeads::isPathNamed, this, name));
  llvm::erase_if(head.narrowedFields, std::bind_front(&LoopHeads::isNarrowedPathNamed, this, name));
}

bool LoopHeads::isNarrowedPathWritten(const LoopScan &scan,
                                      const std::pair<FieldPath, const Type *> &narrowed) const {
  return this->isWrittenPath(scan, narrowed.first);
}

void LoopHeads::forgetWrittenFields(Flow &head, const LoopScan &scan) const {
  auto isWritten = std::bind_front(&LoopHeads::isWrittenPath, this, std::cref(scan));
  auto isNarrowedWritten =
      std::bind_front(&LoopHeads::isNarrowedPathWritten, this, std::cref(scan));
  llvm::erase_if(head.definedFields, isWritten);
  llvm::erase_if(head.narrowedFields, isNarrowedWritten);
}

bool LoopHeads::isWrittenPath(const LoopScan &scan, const FieldPath &path) const {
  for (llvm::StringRef field : path.names) {
    if (scan.fieldWrites.contains(field)) {
      return true;
    }
  }

  return false;
}

bool LoopHeads::isResizedArrayFact(const LoopScan &scan, const Facts::Fact &fact) const {
  if (scan.isGrowingArrays && this->isArrayPart(fact.lhs)) {
    return true;
  }

  return scan.isShrinkingArrays && this->isArrayPart(fact.rhs);
}

void LoopHeads::forgetArrayFacts(Flow &head, const LoopScan &scan) const {
  auto isResized = std::bind_front(&LoopHeads::isResizedArrayFact, this, std::cref(scan));
  head.facts.forgetIf(isResized);
}

bool LoopHeads::isArrayPart(const Term &term) const {
  bool isFirstArray = term.length && this->isArrayLength(term.first());
  bool isSecondArray = term.length2 && this->isArrayLength(term.second());

  return isFirstArray || isSecondArray;
}

}
