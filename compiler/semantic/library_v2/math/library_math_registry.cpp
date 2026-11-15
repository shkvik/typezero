#include "compiler/semantic/library_v2/math/library_math_registry.h"
#include "compiler/program/library/builtin_names/global_constant_table/global_constant_table.h"
#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/program/library/math/bit_wrap_table/bit_wrap_table.h"
#include "llvm/ADT/STLExtras.h"

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

LibraryMathRegistry::LibraryMathRegistry(SemanticState &owner)
    : state(owner) {}

void LibraryMathRegistry::numericArgSlot(const NumericArgSlot &task) {
  auto &tasks = this->state.walk.tasks;
  tasks.pushTask(AfterNumericArg{.slot = task.slot});
  tasks.pushTask(ApparentSlot{.slot = task.slot});

  return tasks.pushTask(CheckExprSlot{.slot = task.slot, .expected = task.hint});
}

void LibraryMathRegistry::afterNumericArg(const AfterNumericArg &task) {
  const Expr *apparent = *task.slot;
  const Type *type = apparent->type;
  if (!type->isNumeric() && !type->isError()) {
    auto message = ErrorTextMathArg::notDouble(type);
    this->state.diagnostics.error(apparent->loc(), message);
  }
}

void LibraryMathRegistry::checkMathCall(const CheckMathCall &task) {
  CallExpr *call = task.call;
  MemberExpr *member = task.member;
  const Type *expected = task.expected;
  auto &tasks = this->state.walk.tasks;
  const auto *function = MathFunctionTable::find(member->name);
  if (function == nullptr) {
    return this->reportMathMember(call, member);
  }
  bool isMinMax = function->form == MathFunctionForm::MIN_MAX;
  if (!isMinMax && this->state.library.common.hasSpread(call)) {
    tasks.pushChecked(call);

    return tasks.pushTask(RejectSpreadCall{.call = call});
  }
  auto arity = this->state.library.common.checkArity(call, function->minArgs, function->maxArgs);
  if (arity.isErr()) {
    this->state.diagnostics.error(member->loc(), arity.error().message);

    return tasks.pushChecked(call);
  }
  if (isMinMax && call->args.empty()) {
    auto message = ErrorTextMath::noValues(member->name);
    this->state.diagnostics.error(call->loc(), message);

    return tasks.pushChecked(call);
  }
  if (function->form == MathFunctionForm::POW) {
    auto *pow = this->state.ast.make<BinaryExpr>(call->loc(), BinaryOp::Pow, call->args[0],
                                                 call->args[1]);

    return tasks.pushTask(CheckExpr{.expr = pow, .expected = expected});
  }
  call->target = BuiltinCall{.builtin = function->builtin};
  const Type *wanted = expected != nullptr ? expected->nonOptional() : nullptr;
  const Type *hint = wanted != nullptr && wanted->isNumeric() ? wanted : nullptr;
  tasks.pushChecked(call);
  if (call->hasSpread()) {
    auto draft = MinMaxDraft{
        .call = call,
        .hint = hint,
        .common = nullptr,
        .hasSomeValue = false,
        .isNumeric = true,
        .index = 0,
    };

    return this->nextMinMaxArg(NextMinMaxArg{.draft = std::move(draft)});
  }
  const auto *argHint = this->mathArgHint(function->form, hint);
  tasks.pushTask(AfterMathArgs{.call = call, .member = member, .function = function});
  for (Expr *&arg : llvm::reverse(call->args)) {
    tasks.pushTask(NumericArgSlot{.slot = &arg, .hint = argHint});
  }
}

void LibraryMathRegistry::reportMathMember(CallExpr *call, MemberExpr *member) {
  auto &tasks = this->state.walk.tasks;
  auto constant = GlobalConstantTable::find(GlobalName::MATH, member->name);
  bool isConstant = constant.has_value();
  std::string message =
      isConstant ? ErrorTextMath::constant(member->name) : ErrorTextMath::noMember(member->name);
  this->state.diagnostics.error(member->loc(), message);
  tasks.pushChecked(call);

  return tasks.pushTask(WalkArgsLoosely{.call = call});
}

const Type *LibraryMathRegistry::mathArgHint(MathFunctionForm form, const Type *hint) const {
  if (form == MathFunctionForm::FLOAT || form == MathFunctionForm::ROUNDING) {
    return Type::doubleTy();
  }
  bool isHinted = form == MathFunctionForm::SAME || form == MathFunctionForm::MIN_MAX ||
                  form == MathFunctionForm::CLAMP;

  return isHinted ? hint : nullptr;
}

void LibraryMathRegistry::afterMathArgs(const AfterMathArgs &task) {
  CallExpr *call = task.call;
  for (const Expr *arg : call->args) {
    if (!arg->type->isNumeric()) {
      return;
    }
  }

  return this->checkMathResult(call, task.member, *task.function);
}

void LibraryMathRegistry::checkMathResult(CallExpr *call, MemberExpr *member,
                                          const MathFunctionInfo &function) {
  switch (function.form) {
    case MathFunctionForm::ROUNDING:
      return this->checkRounding(call, member);
    case MathFunctionForm::FLOAT:
      return this->checkFloatResult(call);
    case MathFunctionForm::SAME:
      call->type = call->args[0]->type;
      return;
    case MathFunctionForm::MIN_MAX:
    case MathFunctionForm::CLAMP:
      return this->checkSharedResult(call);
    case MathFunctionForm::INT32:
      return this->startInt32Math(call, member, function.builtin);
    case MathFunctionForm::POW:
      return;
  }
}

void LibraryMathRegistry::checkFloatResult(CallExpr *call) {
  auto &tasks = this->state.walk.tasks;
  call->type = Type::doubleTy();
  for (Expr *&arg : llvm::reverse(call->args)) {
    tasks.pushTask(CoerceSlot{.slot = &arg, .to = Type::doubleTy(), .what = ""});
  }
}

void LibraryMathRegistry::checkRounding(CallExpr *call, MemberExpr *member) {
  auto rounded = this->checkRoundedDouble(member->name, call->args[0]->type);
  if (rounded.isErr()) {
    return this->state.diagnostics.error(call->args[0]->loc(), rounded.error().message);
  }
  call->type = Type::doubleTy();
}

void LibraryMathRegistry::checkSharedResult(CallExpr *call) {
  const auto *common = this->adaptSharedArgs(call->args, {}, nullptr);
  if (common != nullptr) {
    call->type = common;
  }
}

void LibraryMathRegistry::startInt32Math(CallExpr *call, MemberExpr *member, Builtin builtin) {
  bool isImul = builtin == Builtin::MathImul;
  auto mathName = GlobalNameTable::getName(GlobalName::MATH);
  std::string what = mathName.str() + "." + member->name.str();

  return this->nextInt32Arg(NextInt32Arg{.call = call, .isImul = isImul, .what = what, .index = 0});
}

void LibraryMathRegistry::nextInt32Arg(const NextInt32Arg &task) {
  CallExpr *call = task.call;
  auto &tasks = this->state.walk.tasks;
  const auto *int32 = Type::signedInteger(32);
  if (task.index == call->args.size()) {
    call->type = task.isImul ? int32 : Type::intTy();
    return;
  }
  tasks.pushTask(AfterInt32Arg{
      .call = call,
      .isImul = task.isImul,
      .what = task.what,
      .index = task.index,
  });

  return tasks.pushTask(BitsArgSlot{
      .slot = &call->args[task.index],
      .bits = 32,
      .literal = task.isImul ? int32 : nullptr,
      .what = task.what,
  });
}

void LibraryMathRegistry::afterInt32Arg(const AfterInt32Arg &task) {
  CallExpr *call = task.call;
  auto &tasks = this->state.walk.tasks;
  Expr **slot = &call->args[task.index];
  if ((*slot)->type->isError()) {
    return;
  }
  auto next = NextInt32Arg{
      .call = call,
      .isImul = task.isImul,
      .what = task.what,
      .index = task.index + 1,
  };
  tasks.pushTask(std::move(next));
  if (!task.isImul) {
    return;
  }

  return tasks.pushTask(ConvertSlot{.slot = slot, .to = Type::signedInteger(32)});
}

const Type *LibraryMathRegistry::adaptSharedArgs(std::vector<Expr *> &args,
                                                 const std::vector<bool> &spreads,
                                                 const Type *common) {
  auto &tasks = this->state.walk.tasks;
  const auto *target = common != nullptr ? common : this->commonArgType(args, spreads);
  auto adapted = std::vector<Expr **>();
  const Type *shared = target;
  for (size_t index = 0; index < args.size() && shared != nullptr; ++index) {
    bool isKept = this->isSpreadAt(spreads, index) || args[index]->type == target;
    bool isAdapting =
        !isKept && this->state.code.conversions.isLiteralAdapting(args[index], target);
    if (isAdapting) {
      adapted.push_back(&args[index]);
    }
    if (!isKept && !isAdapting) {
      this->reportSharedMismatch(args[index], target);
      shared = nullptr;
    }
  }
  for (Expr **slot : llvm::reverse(adapted)) {
    tasks.pushTask(AdaptLiteralSlot{.slot = slot, .to = target});
  }

  return shared;
}

void LibraryMathRegistry::reportSharedMismatch(const Expr *arg, const Type *target) {
  auto hint = this->state.code.conversions.conversionHint(arg, target);
  auto message = ErrorTextMath::oneType(target->name(), arg->type->name(), hint);
  this->state.diagnostics.error(arg->loc(), message);
}

bool LibraryMathRegistry::isSpreadAt(const std::vector<bool> &spreads, size_t index) const {
  return index < spreads.size() && spreads[index];
}

const Type *LibraryMathRegistry::commonArgType(llvm::ArrayRef<Expr *> args,
                                               const std::vector<bool> &spreads) const {
  for (size_t index = 0; index < args.size(); ++index) {
    bool isTyped = !this->isSpreadAt(spreads, index) &&
                   !this->state.code.conversions.isIntLiteralTree(args[index]);
    if (isTyped) {
      return args[index]->type;
    }
  }

  return this->literalsType(args, spreads);
}

const Type *LibraryMathRegistry::literalsType(llvm::ArrayRef<Expr *> args,
                                              const std::vector<bool> &spreads) const {
  for (size_t index = 0; index < args.size(); ++index) {
    if (!this->isSpreadAt(spreads, index) && args[index]->type->isFloat()) {
      return Type::doubleTy();
    }
  }

  return Type::intTy();
}

void LibraryMathRegistry::bitsArgSlot(const BitsArgSlot &task) {
  Expr *arg = *task.slot;
  auto &tasks = this->state.walk.tasks;
  if (arg->type != nullptr) {
    return this->finishBitsArg(FinishBitsArg{
        .slot = task.slot,
        .bits = task.bits,
        .literal = task.literal,
        .what = task.what,
    });
  }
  const auto *target = task.literal != nullptr ? task.literal : Type::unsignedInteger(task.bits);
  bool isLiteral = this->state.code.conversions.isIntLiteralTree(arg) && !isa<NameExpr>(arg);
  tasks.pushTask(FinishBitsArg{
      .slot = task.slot,
      .bits = task.bits,
      .literal = task.literal,
      .what = task.what,
  });

  return tasks.pushTask(CheckExprSlot{.slot = task.slot, .expected = isLiteral ? target : nullptr});
}

void LibraryMathRegistry::finishBitsArg(const FinishBitsArg &task) {
  Expr *checked = *task.slot;
  auto &tasks = this->state.walk.tasks;
  const auto *unsignedType = Type::unsignedInteger(task.bits);
  const auto *target = task.literal != nullptr ? task.literal : unsignedType;
  const Type *type = checked->type;
  if (type->isError() || (type->isInteger() && type->bitWidth() == task.bits)) {
    return;
  }
  auto &conversions = this->state.code.conversions;
  if (conversions.isLiteralAdapting(checked, target)) {
    return tasks.pushTask(AdaptLiteralSlot{.slot = task.slot, .to = target});
  }
  if (task.literal != nullptr && conversions.isLiteralAdapting(checked, unsignedType)) {
    return tasks.pushTask(AdaptLiteralSlot{.slot = task.slot, .to = unsignedType});
  }
  auto hint = this->bitsHint(checked, unsignedType);
  auto message = ErrorTextMathArg::bits(task.what, task.bits, type->name(), hint);
  this->state.diagnostics.error(checked->loc(), message);
  checked->type = Type::error();
}

std::string LibraryMathRegistry::bitsHint(const Expr *arg, const Type *unsignedType) const {
  const Type *type = arg->type;
  if (type->isInteger()) {
    return this->state.code.conversions.conversionHint(arg, unsignedType);
  }
  if (type->isFloat()) {
    auto method = this->state.code.conversions.conversionMethod(unsignedType);
    return ErrorTextMathArg::trunc(method);
  }

  return std::string();
}

void LibraryMathRegistry::checkBitCall(const CheckBitCall &task) {
  CallExpr *call = task.call;
  MemberExpr *member = task.member;
  auto &tasks = this->state.walk.tasks;
  call->type = Type::error();
  tasks.pushChecked(call);
  auto *inner = dyn_cast<MemberExpr>(member->object);
  if (inner != nullptr) {
    return this->checkWrapFrom(call, member, inner);
  }
  const auto *function = BitFunctionTable::find(member->name);
  if (function == nullptr) {
    auto message = ErrorTextBit::noMember(member->name);
    this->state.diagnostics.error(member->loc(), message);

    return tasks.pushTask(WalkArgsLoosely{.call = call});
  }
  if (this->state.library.common.hasSpread(call)) {
    return tasks.pushTask(RejectSpreadCall{.call = call});
  }
  auto arity = this->state.library.common.checkArity(call, function->args, function->args);
  if (arity.isErr()) {
    return this->state.diagnostics.error(member->loc(), arity.error().message);
  }
  auto what = ErrorTextBit::label(member->name);
  call->target = BuiltinCall{.builtin = function->builtin};
  if (function->wide) {
    return this->nextWideBitsArg(
        NextWideBitsArg{.call = call, .function = function, .what = what, .index = 0});
  }
  tasks.pushTask(AfterPlainBitsArg{.call = call, .function = function, .what = what});

  return tasks.pushTask(BitsArgSlot{
      .slot = &call->args[0],
      .bits = function->bits,
      .literal = nullptr,
      .what = what,
  });
}

void LibraryMathRegistry::checkWrapFrom(CallExpr *call, MemberExpr *member, MemberExpr *inner) {
  auto &tasks = this->state.walk.tasks;
  auto width = this->checkWrapBitsWidth(inner->name);
  if (width.isErr()) {
    this->state.diagnostics.error(inner->loc(), width.error().message);

    return tasks.pushTask(WalkArgsLoosely{.call = call});
  }
  auto wrapMember = BitWrapTable::findMember(member->name);
  if (wrapMember != BitWrapMember::FROM) {
    auto message = ErrorTextBit::onlyFrom(inner->name);
    this->state.diagnostics.error(member->loc(), message);

    return tasks.pushTask(WalkArgsLoosely{.call = call});
  }
  auto arity = this->state.library.common.checkArity(call, 1, 1);
  if (arity.isErr()) {
    return this->state.diagnostics.error(member->loc(), arity.error().message);
  }
  tasks.pushTask(AfterWrapFromArg{.call = call, .inner = inner, .bits = width.value()});

  return tasks.pushTask(CheckExprSlot{.slot = &call->args[0], .expected = nullptr});
}

void LibraryMathRegistry::afterWrapFromArg(const AfterWrapFromArg &task) {
  CallExpr *call = task.call;
  const Expr *arg = call->args[0];
  bool isInteger = arg->type->isInteger();
  if (!isInteger && !arg->type->isError()) {
    auto message = ErrorTextBit::fromInt(task.inner->name, arg->type->name());
    this->state.diagnostics.error(arg->loc(), message);
  }
  if (!isInteger) {
    return;
  }
  call->target = BuiltinCall{.builtin = Builtin::BitWrapFrom};
  call->type = Type::wrapping(task.bits);
}

void LibraryMathRegistry::nextWideBitsArg(const NextWideBitsArg &task) {
  CallExpr *call = task.call;
  auto &tasks = this->state.walk.tasks;
  if (task.index == call->args.size()) {
    const auto *uint64 = Type::unsignedInteger(64);
    const BitFunctionInfo &function = *task.function;
    call->type = Type::object(
        {{function.first.str(), uint64, false}, {function.second.str(), uint64, false}});
    return;
  }
  tasks.pushTask(AfterWideBitsArg{
      .call = call,
      .function = task.function,
      .what = task.what,
      .index = task.index,
  });

  return tasks.pushTask(BitsArgSlot{
      .slot = &call->args[task.index],
      .bits = 64,
      .literal = nullptr,
      .what = task.what,
  });
}

void LibraryMathRegistry::afterWideBitsArg(const AfterWideBitsArg &task) {
  CallExpr *call = task.call;
  auto &tasks = this->state.walk.tasks;
  const auto *uint64 = Type::unsignedInteger(64);
  Expr **slot = &call->args[task.index];
  const Expr *arg = *slot;
  if (arg->type->isError()) {
    return;
  }
  if (arg->type != uint64 && !arg->type->isWrapping()) {
    auto hint = this->state.code.conversions.conversionHint(arg, uint64);
    auto message = ErrorTextBit::uint64(task.what, arg->type->name(), hint);
    return this->state.diagnostics.error(arg->loc(), message);
  }
  tasks.pushTask(NextWideBitsArg{
      .call = call,
      .function = task.function,
      .what = task.what,
      .index = task.index + 1,
  });

  return tasks.pushTask(ConvertSlot{.slot = slot, .to = uint64});
}

void LibraryMathRegistry::afterPlainBitsArg(const AfterPlainBitsArg &task) {
  CallExpr *call = task.call;
  auto &tasks = this->state.walk.tasks;
  if (call->args[0]->type->isError()) {
    return;
  }
  if (task.function->args != 2) {
    return this->finishPlainBits(FinishPlainBits{.call = call, .function = task.function});
  }
  tasks.pushTask(AfterBitCount{.call = call, .function = task.function, .what = task.what});

  return tasks.pushTask(CheckExprSlot{.slot = &call->args[1], .expected = nullptr});
}

void LibraryMathRegistry::afterBitCount(const AfterBitCount &task) {
  CallExpr *call = task.call;
  auto &tasks = this->state.walk.tasks;
  const Type *count = call->args[1]->type;
  bool isInteger = count->isInteger();
  if (!isInteger && !count->isError()) {
    auto message = ErrorTextBit::count(task.what, count->name());
    this->state.diagnostics.error(call->args[1]->loc(), message);
  }
  if (!isInteger) {
    return;
  }
  tasks.pushTask(FinishPlainBits{.call = call, .function = task.function});

  return tasks.pushTask(ConvertSlot{.slot = &call->args[1], .to = call->args[0]->type});
}

void LibraryMathRegistry::finishPlainBits(const FinishPlainBits &task) {
  CallExpr *call = task.call;
  call->type = task.function->counts ? Type::intTy() : call->args[0]->type;
}

void LibraryMathRegistry::nextMinMaxArg(const NextMinMaxArg &task) {
  auto draft = task.draft;
  CallExpr *call = draft.call;
  auto &tasks = this->state.walk.tasks;
  if (draft.index == call->args.size()) {
    return this->finishMinMaxSpread(draft);
  }
  Expr **slot = &call->args[draft.index];
  if (!call->spreads(draft.index)) {
    draft.hasSomeValue = true;
    const Type *hint = draft.hint;
    tasks.pushTask(AfterMinMaxValue{.draft = std::move(draft)});

    return tasks.pushTask(NumericArgSlot{.slot = slot, .hint = hint});
  }
  const Type *listHint = draft.hint != nullptr ? Type::array(draft.hint) : nullptr;
  tasks.pushTask(AfterMinMaxSpread{.draft = std::move(draft)});
  tasks.pushTask(IterableOfSlot{.slot = slot});
  tasks.pushTask(CheckExprSlot{.slot = slot, .expected = listHint});

  return tasks.pushTask(MarkFused{.expr = *slot});
}

void LibraryMathRegistry::afterMinMaxValue(const AfterMinMaxValue &task) {
  auto draft = task.draft;
  const Type *type = draft.call->args[draft.index]->type;

  return this->takeMinMaxType(std::move(draft), type);
}

void LibraryMathRegistry::afterMinMaxSpread(const AfterMinMaxSpread &task) {
  auto draft = task.draft;
  CallExpr *call = draft.call;
  Expr *&arg = call->args[draft.index];
  Expr *spread = arg->type->isIterator() ? this->spreadIteratorValues(arg) : arg;
  bool isCalled = spread != arg;
  arg = spread;
  const Type *type = arg->type->isArray() ? arg->type->inner() : arg->type;
  bool isNumericArray = arg->type->isArray() && type->isNumeric();
  if (!arg->type->isError() && !isNumericArray) {
    auto message = ErrorTextMath::spread(arg->type->name());
    this->state.diagnostics.error(arg->loc(), message);
    draft.isNumeric = false;
  }
  draft.hasSomeValue |= this->state.flow.proofs.canProveNotEmpty(arg);
  this->takeMinMaxType(std::move(draft), type);
  if (!isCalled) {
    return;
  }

  return this->state.walk.tasks.pushTask(CalledFunction{});
}

Expr *LibraryMathRegistry::spreadIteratorValues(Expr *arg) {
  auto nothing = this->state.code.iteration.checkSendsNothing(arg->type, "a spread");
  if (nothing.isErr()) {
    this->state.diagnostics.error(arg->loc(), nothing.error().message);
    return arg;
  }
  auto *values = this->state.ast.make<ArrayLitExpr>(arg->loc());
  values->elements.push_back({arg, true});
  values->type = Type::array(arg->type->yielded());

  return values;
}

void LibraryMathRegistry::takeMinMaxType(MinMaxDraft draft, const Type *type) {
  CallExpr *call = draft.call;
  auto &tasks = this->state.walk.tasks;
  size_t index = draft.index;
  draft.index += 1;
  draft.isNumeric &= type->isNumeric();
  if (!draft.isNumeric || !call->spreads(index)) {
    return tasks.pushTask(NextMinMaxArg{.draft = std::move(draft)});
  }
  if (draft.common != nullptr && draft.common != type) {
    bool isMin = call->builtin() == Builtin::MathMin;
    auto message = ErrorTextMath::spreadType(isMin, draft.common->name(), type->name());
    return this->state.diagnostics.error(call->args[index]->loc(), message);
  }
  draft.common = type;

  return tasks.pushTask(NextMinMaxArg{.draft = std::move(draft)});
}

void LibraryMathRegistry::finishMinMaxSpread(const MinMaxDraft &draft) {
  CallExpr *call = draft.call;
  if (!draft.isNumeric) {
    return;
  }
  auto spreads = std::vector<bool>();
  for (size_t index = 0; index < call->args.size(); ++index) {
    spreads.push_back(call->spreads(index));
  }
  const auto *common = this->adaptSharedArgs(call->args, spreads, draft.common);
  if (common == nullptr) {
    return;
  }
  call->type = draft.hasSomeValue ? common : Type::optional(common);
  call->mayBeOutOfRange = !draft.hasSomeValue;
}

LibraryMathRegistry::ResultWrapBitsWidth
LibraryMathRegistry::checkWrapBitsWidth(llvm::StringRef name) const {
  auto bits = BitWrapTable::findBits(name);
  if (!bits.has_value()) {
    auto message = ErrorTextBit::noMember(name);

    return err(LibraryMathRegistryWrapBitsWidthError{
        .code = LibraryMathRegistryWrapBitsWidthErrorCode::NO_MEMBER,
        .message = message,
    });
  }

  return ok(bits.value());
}

LibraryMathRegistry::ResultRoundedDouble
LibraryMathRegistry::checkRoundedDouble(llvm::StringRef name, const Type *type) const {
  if (type->isInteger()) {
    auto message = ErrorTextMath::rounding(name, type->name());

    return err(LibraryMathRegistryRoundedDoubleError{
        .code = LibraryMathRegistryRoundedDoubleErrorCode::NO_FRACTION,
        .message = message,
    });
  }

  return ok(type);
}

}
