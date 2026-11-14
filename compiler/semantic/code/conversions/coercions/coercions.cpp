#include "compiler/semantic/code/conversions/coercions/coercions.h"
#include "compiler/program/library/builtin_names/library_type_table/library_type_table.h"
#include <cctype>

using llvm::dyn_cast;

namespace tz {

namespace {

constexpr size_t kManyLiteralMembers = 64;

}

Coercions::Coercions(SemanticState &owner, ConversionRules &sharedConversionRules)
    : state(owner),
      conversionRules(sharedConversionRules) {}

void Coercions::coerce(const Coerce &task) {
  auto slot = CoerceSlot{.slot = nullptr, .to = task.to, .what = task.what};

  return this->coerceSlot(slot);
}

Expr *Coercions::coerced(Expr **slot) {
  if (slot != nullptr) {
    return *slot;
  }
  Expr *top = this->state.walk.tasks.takeChecked();
  this->state.walk.tasks.pushChecked(top);

  return top;
}

void Coercions::replaceCoerced(Expr **slot, Expr *value) {
  if (slot != nullptr) {
    *slot = value;
    return;
  }
  this->state.walk.tasks.takeChecked();

  return this->state.walk.tasks.pushChecked(value);
}

void Coercions::coerceSlot(const CoerceSlot &task) {
  Expr *expr = this->coerced(task.slot);
  const Type *from = expr->type;
  const Type *to = task.to;
  if (from == to || from->isError() || to->isError()) {
    return;
  }
  const auto &relations = this->state.types.relations;
  if (relations.isImplicitlyConvertible(from, to)) {
    auto *converted = this->conversionRules.convertExpr(expr, to);
    return this->replaceCoerced(task.slot, converted);
  }
  if (this->state.code.conversions.isLiteralAdapting(expr, to)) {
    auto *adapted = this->conversionRules.adaptLiteralExpr(expr, to);
    return this->replaceCoerced(task.slot, adapted);
  }
  if (relations.isFittingConditional(from, to)) {
    auto *converted = this->conversionRules.convertExpr(expr, to);
    return this->replaceCoerced(task.slot, converted);
  }
  if (to->nonOptional()->isIterator() && from->isObject()) {
    this->state.walk.tasks.pushTask(CoerceIteratorFound{
        .slot = task.slot,
        .to = to,
        .what = task.what,
    });
    return this->state.walk.tasks.pushTask(FindIterator{.expr = expr});
  }

  return this->coerceAsyncIterator(task.slot, to, task.what);
}

void Coercions::coerceIteratorFound(const CoerceIteratorFound &task) {
  Expr *iterator = this->state.walk.tasks.takeChecked();
  bool isFitting = iterator != nullptr && iterator->type->isIterator() &&
                   this->state.types.relations.isImplicitlyConvertible(iterator->type, task.to);
  if (isFitting) {
    auto *converted = this->conversionRules.convertExpr(iterator, task.to);
    return this->replaceCoerced(task.slot, converted);
  }

  return this->coerceAsyncIterator(task.slot, task.to, task.what);
}

void Coercions::coerceAsyncIterator(Expr **slot, const Type *to, const std::string &what) {
  Expr *expr = this->coerced(slot);
  const Type *from = expr->type;
  bool isAsyncTarget =
      to->nonOptional()->isAsyncIterator() && from->isObject() && !from->isAsyncIterator();
  if (isAsyncTarget) {
    this->state.walk.tasks.pushTask(CoerceAsyncIteratorFound{
        .slot = slot,
        .to = to,
        .what = what,
    });
    return this->state.walk.tasks.pushTask(AsyncUserIterator{.expr = expr});
  }

  return this->coerceFunction(slot, to, what);
}

void Coercions::coerceAsyncIteratorFound(const CoerceAsyncIteratorFound &task) {
  Expr *iterator = this->state.walk.tasks.takeChecked();
  bool isFitting = iterator != nullptr && iterator->type->isAsyncIterator() &&
                   this->state.types.relations.isImplicitlyConvertible(iterator->type, task.to);
  if (isFitting) {
    auto *converted = this->conversionRules.convertExpr(iterator, task.to);
    return this->replaceCoerced(task.slot, converted);
  }

  return this->coerceFunction(task.slot, task.to, task.what);
}

void Coercions::coerceFunction(Expr **slot, const Type *to, const std::string &what) {
  Expr *expr = this->coerced(slot);
  const Type *function = this->functionTarget(to);
  if (!expr->type->isFunction() || !function->isFunction()) {
    return this->coerceRange(slot, to, what);
  }
  auto *adapted = this->adaptFunction(expr, function);
  auto *result = adapted != nullptr ? adapted : this->adaptResult(expr, function);
  if (result == nullptr) {
    return this->coerceRange(slot, to, what);
  }
  auto *fitted = function == to ? result : this->conversionRules.convertExpr(result, to);

  return this->replaceCoerced(slot, fitted);
}

void Coercions::coerceRange(Expr **slot, const Type *to, const std::string &what) {
  Expr *expr = this->coerced(slot);
  auto intervals = this->targetIntervals(to);
  if (expr->type->isInt() && this->canProveSomeInterval(expr, intervals)) {
    auto *converted = this->conversionRules.convertExpr(expr, to);
    return this->replaceCoerced(slot, converted);
  }
  auto proven = this->rangeProven(expr, to, what, intervals);
  if (proven.isErr()) {
    return this->state.diagnostics.error(expr->loc(), proven.error().message);
  }

  return this->conversionRules.walkAssignable(expr, to, what);
}

Expr *Coercions::adaptResult(Expr *expr, const Type *to) {
  const Type *from = expr->type;
  llvm::ArrayRef<const Type *> mine = from->params();
  llvm::ArrayRef<const Type *> given = to->params();
  if (mine.size() < given.size() || from->minArgs() > given.size() || from->hasRest() ||
      to->hasRest() || from->hasThis() != to->hasThis() || (given.size() < to->minArgs())) {
    return nullptr;
  }
  bool isConverting = false;
  for (size_t index = 0; index < given.size(); ++index) {
    if (mine[index] == given[index]) {
      continue;
    }
    if (!mine[index]->nonOptional()->isUnknown() || given[index]->isVoid()) {
      return nullptr;
    }
    isConverting = true;
  }
  bool isFewer = mine.size() > given.size();
  const Type *got = from->ret();
  const Type *wanted = to->ret();
  bool isPlain = got == wanted && !isFewer && !isConverting;
  bool isUnfitting =
      got != wanted && !this->state.types.relations.isImplicitlyConvertible(got, wanted);
  if (isPlain || got->isVoid() || wanted->isVoid() || got->isError() || wanted->isError() ||
      isUnfitting) {
    return nullptr;
  }
  auto *castExpr = this->state.ast.make<CastExpr>(expr->loc(), expr, std::nullopt);
  castExpr->type = to;
  castExpr->adaptsResult = true;

  return castExpr;
}

FunctionDecl *Coercions::namedFunction(const Expr *expr) const {
  const auto *name = dyn_cast<NameExpr>(expr);
  if (name == nullptr) {
    return nullptr;
  }
  if (name->func() != nullptr) {
    return name->func()->decl;
  }
  const VarSymbol *variable = name->var();
  if (variable == nullptr) {
    return nullptr;
  }
  if (variable->function() != nullptr) {
    return variable->function();
  }

  return variable->isConst ? variable->lambda() : nullptr;
}

Expr *Coercions::adaptFunction(Expr *expr, const Type *to) {
  const Type *from = expr->type;
  if (this->namedFunction(expr) == nullptr || from->hasRest() || to->hasRest()) {
    return nullptr;
  }
  llvm::ArrayRef<const Type *> params = from->params();
  llvm::ArrayRef<const Type *> wanted = to->params();
  size_t shared = std::min(params.size(), wanted.size());
  for (size_t index = 0; index < shared; ++index) {
    if (!this->isParamFitting(params[index], wanted[index])) {
      return nullptr;
    }
  }
  if (wanted.size() < from->minArgs()) {
    return nullptr;
  }
  if (!to->ret()->isVoid() && from->ret() != to->ret()) {
    return nullptr;
  }
  auto *castExpr = this->state.ast.make<CastExpr>(expr->loc(), expr, std::nullopt);
  castExpr->type = to;

  return castExpr;
}

bool Coercions::isParamFitting(const Type *param, const Type *wanted) const {
  bool isOptionalWanted = param->isOptional() && param->inner() == wanted;
  bool isUnknownTaken = param->nonOptional()->isUnknown() && !wanted->isVoid();

  return param == wanted || isOptionalWanted || isUnknownTaken;
}

const Type *Coercions::functionTarget(const Type *to) const {
  if (to->isOptional() && to->inner()->isFunction()) {
    return to->inner();
  }

  return to;
}

std::vector<std::pair<int64_t, int64_t>> Coercions::targetIntervals(const Type *to) const {
  const Type *target = to->nonOptional();
  auto range = this->state.types.literals.intRange(target);
  if (range.has_value()) {
    return {*range};
  }
  if (target->isUnion() && target->members().size() >= kManyLiteralMembers) {
    return this->state.types.literals.intIntervals(target);
  }

  return {};
}

bool Coercions::canProveSomeInterval(const Expr *expr,
                                     llvm::ArrayRef<std::pair<int64_t, int64_t>> intervals) {
  for (const std::pair<int64_t, int64_t> &interval : intervals) {
    if (this->state.flow.proofs.canProveFits(expr, interval.first, interval.second)) {
      return true;
    }
  }

  return false;
}

Coercions::ResultRangeWasProven
Coercions::rangeProven(const Expr *expr, const Type *to, llvm::StringRef what,
                       llvm::ArrayRef<std::pair<int64_t, int64_t>> intervals) const {
  const Type *from = expr->type;
  bool isInteger = from->isInt() || (from->isLiteral() && from->literalBase()->isInt());
  bool isOutside = !intervals.empty() && isInteger;
  auto prefix = ErrorTextAssignable::notTo(from->name(), to->name(), what);
  auto holds = ErrorTextRange::holds(intervals);
  if (isOutside && !from->isInt()) {
    std::string message = prefix + holds;

    return err(CodeConversionsRegistryRangeProvenError{
        .code = CodeConversionsRegistryRangeProvenErrorCode::LITERAL_OUT_OF_RANGE,
        .message = message,
    });
  }
  if (isOutside) {
    auto check = this->rangeCheck(expr, to->nonOptional(), intervals);
    std::string message = prefix + holds + ErrorTextRange::outside(check);

    return err(CodeConversionsRegistryRangeProvenError{
        .code = CodeConversionsRegistryRangeProvenErrorCode::MAY_BE_OUT_OF_RANGE,
        .message = message,
    });
  }

  return ok(expr);
}

std::string Coercions::rangeCheck(const Expr *expr, const Type *target,
                                  llvm::ArrayRef<std::pair<int64_t, int64_t>> intervals) const {
  auto widest = this->widestInterval(intervals);
  auto low = std::to_string(widest.first);
  auto high = std::to_string(widest.second);
  auto *name = dyn_cast<NameExpr>(expr);
  auto text = name != nullptr ? name->name.str() : std::string("x");
  if (target->name() == LibraryTypeTable::getName(LibraryType::HTTP_STATUS)) {
    return ErrorTextRange::status(text);
  }

  return ErrorTextRange::between(text, low, high);
}

std::pair<int64_t, int64_t>
Coercions::widestInterval(llvm::ArrayRef<std::pair<int64_t, int64_t>> intervals) const {
  std::pair<int64_t, int64_t> widest = intervals.front();
  for (const std::pair<int64_t, int64_t> &interval : intervals.drop_front()) {
    bool isWider = widest.second - widest.first < interval.second - interval.first;
    if (isWider) {
      widest = interval;
    }
  }

  return widest;
}

}
