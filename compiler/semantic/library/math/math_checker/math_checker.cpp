#include "compiler/semantic/library/math/math_checker/math_checker.h"
#include "compiler/program/library/builtin_names/global_constant_table/global_constant_table.h"
#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/program/library/math/bit_function_table/bit_function_table.h"
#include "compiler/program/library/math/bit_wrap_table/bit_wrap_table.h"
#include "compiler/program/library/math/math_function_table/math_function_table.h"
#include "compiler/semantic/bodies/expr/argument_checker/argument_checker.h"
#include "compiler/semantic/bodies/expr/binary_checker/binary_checker.h"
#include "compiler/semantic/bodies/expr/call_checker/call_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/bodies/iterators/generator_checker/generator_checker.h"
#include "compiler/semantic/bodies/iterators/iterator_methods/iterator_methods.h"
#include "compiler/semantic/bodies/objects/dict_checker/dict_checker.h"
#include "compiler/semantic/flow/flow_checker/flow_checker.h"
#include "compiler/semantic/flow/index_proofs/index_proofs.h"
#include "compiler/semantic/generics/generic_checker/generic_checker.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "llvm/ADT/STLExtras.h"
#include <cstring>

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

MathChecker::MathChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->callChecker = &all.callChecker;
  this->dictChecker = &all.dictChecker;
  this->exprChecker = &all.exprChecker;
  this->flowChecker = &all.flowChecker;
  this->indexProofs = &all.indexProofs;
  this->binaryChecker = &all.binaryChecker;
  this->typeRelations = &all.typeRelations;
  this->genericChecker = &all.genericChecker;
  this->argumentChecker = &all.argumentChecker;
  this->iteratorMethods = &all.iteratorMethods;
  this->generatorChecker = &all.generatorChecker;
}

MathChecker::ResultWrapBitsWidth MathChecker::checkWrapBitsWidth(llvm::StringRef name) const {
  auto bits = BitWrapTable::findBits(name);
  if (!bits.has_value()) {
    auto message = ErrorTextBit::noMember(name);

    return err(MathCheckerWrapBitsWidthError{
        .code = MathCheckerWrapBitsWidthErrorCode::NO_MEMBER,
        .message = message,
    });
  }

  return ok(bits.value());
}

MathChecker::ResultRoundedDouble MathChecker::checkRoundedDouble(llvm::StringRef name,
                                                                 const Type *type) const {
  if (type->isInteger()) {
    auto message = ErrorTextMath::rounding(name, type->name());

    return err(MathCheckerRoundedDoubleError{
        .code = MathCheckerRoundedDoubleErrorCode::NO_FRACTION,
        .message = message,
    });
  }

  return ok(type);
}

Expr *MathChecker::numericArg(Expr *arg, const Type *hint) {
  auto *checked = this->exprChecker->checkExpr(arg, hint);
  auto *apparent = this->genericChecker->apparent(checked);
  const Type *type = apparent->type;
  if (!type->isNumeric() && !type->isError()) {
    auto message = ErrorTextMathArg::notDouble(type);
    this->state.getDiagnostics().error(apparent->loc(), message);
  }

  return apparent;
}

Expr *MathChecker::checkMathCall(CallExpr *call, MemberExpr *member, const Type *expected) {
  const auto *function = MathFunctionTable::find(member->name);
  if (function == nullptr) {
    return this->reportMathMember(call, member);
  }
  bool isMinMax = function->form == MathFunctionForm::MIN_MAX;
  if (!isMinMax && this->callChecker->rejectSpread(call)) {
    return call;
  }
  auto arity = this->argumentChecker->checkArity(call, function->minArgs, function->maxArgs);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(member->loc(), arity.error().message);
    return call;
  }
  if (isMinMax && call->args.empty()) {
    auto message = ErrorTextMath::noValues(member->name);
    this->state.getDiagnostics().error(call->loc(), message);
    return call;
  }
  if (function->form == MathFunctionForm::POW) {
    auto *pow = this->state.getAst().make<BinaryExpr>(call->loc(), BinaryOp::Pow, call->args[0],
                                                      call->args[1]);
    return this->binaryChecker->checkBinary(pow, expected);
  }
  call->target = BuiltinCall{.builtin = function->builtin};
  const Type *wanted = expected != nullptr ? expected->nonOptional() : nullptr;
  const Type *hint = wanted != nullptr && wanted->isNumeric() ? wanted : nullptr;
  if (call->hasSpread()) {
    return this->checkMinMaxSpread(call, hint);
  }
  if (!this->prepareMathArgs(call, function->form, hint)) {
    return call;
  }

  return this->checkMathResult(call, member, *function);
}

Expr *MathChecker::reportMathMember(CallExpr *call, MemberExpr *member) {
  auto constant = GlobalConstantTable::find(GlobalName::MATH, member->name);
  bool isConstant = constant.has_value();
  std::string message =
      isConstant ? ErrorTextMath::constant(member->name) : ErrorTextMath::noMember(member->name);
  this->state.getDiagnostics().error(member->loc(), message);
  this->argumentChecker->walkArgsLoosely(call);

  return call;
}

bool MathChecker::prepareMathArgs(CallExpr *call, MathFunctionForm form, const Type *hint) {
  const auto *argHint = this->mathArgHint(form, hint);
  bool isNumeric = true;
  for (Expr *&arg : call->args) {
    arg = this->numericArg(arg, argHint);
    isNumeric &= arg->type->isNumeric();
  }

  return isNumeric;
}

const Type *MathChecker::mathArgHint(MathFunctionForm form, const Type *hint) const {
  if (form == MathFunctionForm::FLOAT || form == MathFunctionForm::ROUNDING) {
    return Type::doubleTy();
  }
  bool isHinted = form == MathFunctionForm::SAME || form == MathFunctionForm::MIN_MAX ||
                  form == MathFunctionForm::CLAMP;

  return isHinted ? hint : nullptr;
}

Expr *MathChecker::checkMathResult(CallExpr *call, MemberExpr *member,
                                   const MathFunctionInfo &function) {
  switch (function.form) {
    case MathFunctionForm::ROUNDING:
      return this->checkRounding(call, member);
    case MathFunctionForm::FLOAT:
      return this->checkFloatResult(call);
    case MathFunctionForm::SAME:
      call->type = call->args[0]->type;
      return call;
    case MathFunctionForm::MIN_MAX:
    case MathFunctionForm::CLAMP:
      return this->checkSharedResult(call);
    case MathFunctionForm::INT32:
      return this->checkInt32Math(call, member, function.builtin);
    case MathFunctionForm::POW:
      return call;
  }

  return call;
}

Expr *MathChecker::checkFloatResult(CallExpr *call) {
  for (Expr *&arg : call->args) {
    arg = this->typeRelations->coerce(arg, Type::doubleTy());
  }
  call->type = Type::doubleTy();

  return call;
}

Expr *MathChecker::checkRounding(CallExpr *call, MemberExpr *member) {
  auto rounded = this->checkRoundedDouble(member->name, call->args[0]->type);
  if (rounded.isErr()) {
    this->state.getDiagnostics().error(call->args[0]->loc(), rounded.error().message);
    return call;
  }
  call->type = Type::doubleTy();

  return call;
}

Expr *MathChecker::checkSharedResult(CallExpr *call) {
  const auto *common = this->sharedType(call->args, {});
  if (common == nullptr) {
    return call;
  }
  call->type = common;

  return call;
}

Expr *MathChecker::checkInt32Math(CallExpr *call, MemberExpr *member, Builtin builtin) {
  bool isImul = builtin == Builtin::MathImul;
  const auto *int32 = Type::signedInteger(32);
  auto mathName = GlobalNameTable::getName(GlobalName::MATH);
  std::string what = mathName.str() + "." + member->name.str();
  for (Expr *&arg : call->args) {
    arg = this->bitsArg(arg, 32, isImul ? int32 : nullptr, what);
    if (arg->type->isError()) {
      return call;
    }
    if (isImul) {
      arg = this->typeRelations->convert(arg, int32);
    }
  }
  call->type = isImul ? int32 : Type::intTy();

  return call;
}

const Type *MathChecker::sharedType(std::vector<Expr *> &args, const std::vector<bool> &spreads,
                                    const Type *common) {
  const auto *target = common != nullptr ? common : this->commonArgType(args, spreads);
  for (size_t index = 0; index < args.size(); ++index) {
    bool isFitting = this->isSpreadAt(spreads, index) || this->adaptSharedArg(args[index], target);
    if (!isFitting) {
      return nullptr;
    }
  }

  return target;
}

bool MathChecker::isSpreadAt(const std::vector<bool> &spreads, size_t index) const {
  return index < spreads.size() && spreads[index];
}

const Type *MathChecker::commonArgType(llvm::ArrayRef<Expr *> args,
                                       const std::vector<bool> &spreads) const {
  for (size_t index = 0; index < args.size(); ++index) {
    if (!this->isSpreadAt(spreads, index) && !this->typeRelations->isIntLiteralTree(args[index])) {
      return args[index]->type;
    }
  }

  return this->literalsType(args, spreads);
}

const Type *MathChecker::literalsType(llvm::ArrayRef<Expr *> args,
                                      const std::vector<bool> &spreads) const {
  for (size_t index = 0; index < args.size(); ++index) {
    if (!this->isSpreadAt(spreads, index) && args[index]->type->isFloat()) {
      return Type::doubleTy();
    }
  }

  return Type::intTy();
}

bool MathChecker::adaptSharedArg(Expr *&arg, const Type *common) {
  if (arg->type == common) {
    return true;
  }
  if (this->typeRelations->isLiteralAdapting(arg, common)) {
    arg = this->typeRelations->adaptLiteral(arg, common);
    return true;
  }
  auto hint = this->typeRelations->conversionHint(arg, common);
  auto message = ErrorTextMath::oneType(common->name(), arg->type->name(), hint);
  this->state.getDiagnostics().error(arg->loc(), message);

  return false;
}

Expr *MathChecker::bitsArg(Expr *arg, unsigned bits, const Type *literal, const std::string &what) {
  const auto *unsignedType = Type::unsignedInteger(bits);
  const auto *target = literal != nullptr ? literal : unsignedType;
  auto *checked = this->checkedBitsArg(arg, target);
  const Type *type = checked->type;
  if (type->isError()) {
    return checked;
  }
  if (type->isInteger() && type->bitWidth() == bits) {
    return checked;
  }
  if (this->typeRelations->isLiteralAdapting(checked, target)) {
    return this->typeRelations->adaptLiteral(checked, target);
  }
  if (literal != nullptr && this->typeRelations->isLiteralAdapting(checked, unsignedType)) {
    return this->typeRelations->adaptLiteral(checked, unsignedType);
  }
  auto hint = this->bitsHint(checked, unsignedType);
  auto message = ErrorTextMathArg::bits(what, bits, type->name(), hint);
  this->state.getDiagnostics().error(checked->loc(), message);
  checked->type = Type::error();

  return checked;
}

Expr *MathChecker::checkedBitsArg(Expr *arg, const Type *target) {
  if (arg->type != nullptr) {
    return arg;
  }
  bool isLiteral = this->typeRelations->isIntLiteralTree(arg) && !isa<NameExpr>(arg);
  const Type *hint = isLiteral ? target : nullptr;

  return this->exprChecker->checkExpr(arg, hint);
}

std::string MathChecker::bitsHint(const Expr *arg, const Type *unsignedType) {
  const Type *type = arg->type;
  if (type->isInteger()) {
    return this->typeRelations->conversionHint(arg, unsignedType);
  }
  if (type->isFloat()) {
    auto method = this->typeRelations->conversionMethod(unsignedType);
    return ErrorTextMathArg::trunc(method);
  }

  return std::string();
}

Expr *MathChecker::checkBitCall(CallExpr *call, MemberExpr *member) {
  call->type = Type::error();
  auto *inner = dyn_cast<MemberExpr>(member->object);
  if (inner != nullptr) {
    return this->checkWrapFrom(call, member, inner);
  }
  const auto *function = BitFunctionTable::find(member->name);
  if (function == nullptr) {
    auto message = ErrorTextBit::noMember(member->name);
    this->state.getDiagnostics().error(member->loc(), message);
    this->argumentChecker->walkArgsLoosely(call);
    return call;
  }
  bool isRejected = this->callChecker->rejectSpread(call);
  auto arity = isRejected ? ok(call)
                          : this->argumentChecker->checkArity(call, function->args, function->args);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(member->loc(), arity.error().message);
  }
  if (isRejected || arity.isErr()) {
    return call;
  }
  auto what = ErrorTextBit::label(member->name);
  call->target = BuiltinCall{.builtin = function->builtin};
  if (function->wide) {
    return this->checkWideBits(call, *function, what);
  }

  return this->checkPlainBits(call, *function, what);
}

Expr *MathChecker::checkWrapFrom(CallExpr *call, MemberExpr *member, MemberExpr *inner) {
  auto width = this->checkWrapBitsWidth(inner->name);
  if (width.isErr()) {
    this->state.getDiagnostics().error(inner->loc(), width.error().message);
    this->argumentChecker->walkArgsLoosely(call);
    return call;
  }
  unsigned bits = width.value();
  auto wrapMember = BitWrapTable::findMember(member->name);
  if (wrapMember != BitWrapMember::FROM) {
    auto message = ErrorTextBit::onlyFrom(inner->name);
    this->state.getDiagnostics().error(member->loc(), message);
    this->argumentChecker->walkArgsLoosely(call);
    return call;
  }
  auto arity = this->argumentChecker->checkArity(call, 1, 1);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(member->loc(), arity.error().message);
    return call;
  }
  call->args[0] = this->exprChecker->checkExpr(call->args[0]);
  const Expr *arg = call->args[0];
  bool isInteger = arg->type->isInteger();
  if (!isInteger && !arg->type->isError()) {
    auto message = ErrorTextBit::fromInt(inner->name, arg->type->name());
    this->state.getDiagnostics().error(arg->loc(), message);
  }
  if (!isInteger) {
    return call;
  }
  call->target = BuiltinCall{.builtin = Builtin::BitWrapFrom};
  call->type = Type::wrapping(bits);

  return call;
}

Expr *MathChecker::checkWideBits(CallExpr *call, const BitFunctionInfo &function,
                                 const std::string &what) {
  const auto *uint64 = Type::unsignedInteger(64);
  for (Expr *&arg : call->args) {
    arg = this->bitsArg(arg, 64, nullptr, what);
    if (arg->type->isError()) {
      return call;
    }
    if (arg->type != uint64 && !arg->type->isWrapping()) {
      auto hint = this->typeRelations->conversionHint(arg, uint64);
      auto message = ErrorTextBit::uint64(what, arg->type->name(), hint);
      this->state.getDiagnostics().error(arg->loc(), message);
      return call;
    }
    arg = this->typeRelations->convert(arg, uint64);
  }
  call->type =
      Type::object({{function.first.str(), uint64, false}, {function.second.str(), uint64, false}});

  return call;
}

Expr *MathChecker::checkPlainBits(CallExpr *call, const BitFunctionInfo &function,
                                  const std::string &what) {
  call->args[0] = this->bitsArg(call->args[0], function.bits, nullptr, what);
  if (call->args[0]->type->isError()) {
    return call;
  }
  if (function.args == 2 && !this->convertBitCount(call, what)) {
    return call;
  }
  call->type = function.counts ? Type::intTy() : call->args[0]->type;

  return call;
}

bool MathChecker::convertBitCount(CallExpr *call, const std::string &what) {
  call->args[1] = this->exprChecker->checkExpr(call->args[1]);
  const Type *count = call->args[1]->type;
  bool isInteger = count->isInteger();
  if (!isInteger && !count->isError()) {
    auto message = ErrorTextBit::count(what, count->name());
    this->state.getDiagnostics().error(call->args[1]->loc(), message);
  }
  if (!isInteger) {
    return false;
  }
  call->args[1] = this->typeRelations->convert(call->args[1], call->args[0]->type);

  return true;
}

Expr *MathChecker::checkMinMaxSpread(CallExpr *call, const Type *hint) {
  SpreadValues values{
      .common = nullptr,
      .hasSomeValue = false,
      .isNumeric = true,
  };
  for (size_t index = 0; index < call->args.size(); ++index) {
    if (!this->takeMinMaxArg(call, index, hint, values)) {
      return call;
    }
  }
  if (!values.isNumeric) {
    return call;
  }
  auto spreads = std::vector<bool>();
  for (size_t index = 0; index < call->args.size(); ++index) {
    spreads.push_back(call->spreads(index));
  }
  const auto *common = this->sharedType(call->args, spreads, values.common);
  if (common == nullptr) {
    return call;
  }
  call->type = values.hasSomeValue ? common : Type::optional(common);
  call->mayBeOutOfRange = !values.hasSomeValue;

  return call;
}

bool MathChecker::takeMinMaxArg(CallExpr *call, size_t index, const Type *hint,
                                SpreadValues &values) {
  const auto *type = this->minMaxArgType(call, index, hint, values);
  values.isNumeric &= type->isNumeric();
  if (!values.isNumeric || !call->spreads(index)) {
    return true;
  }
  if (values.common != nullptr && values.common != type) {
    bool isMin = call->builtin() == Builtin::MathMin;
    auto message = ErrorTextMath::spreadType(isMin, values.common->name(), type->name());
    this->state.getDiagnostics().error(call->args[index]->loc(), message);
    return false;
  }
  values.common = type;

  return true;
}

const Type *MathChecker::minMaxArgType(CallExpr *call, size_t index, const Type *hint,
                                       SpreadValues &values) {
  if (call->spreads(index)) {
    return this->spreadElementType(call, index, hint, values);
  }
  call->args[index] = this->numericArg(call->args[index], hint);
  values.hasSomeValue = true;

  return call->args[index]->type;
}

const Type *MathChecker::spreadElementType(CallExpr *call, size_t index, const Type *hint,
                                           SpreadValues &values) {
  this->iteratorMethods->markFused(call->args[index]);
  const Type *listHint = hint != nullptr ? Type::array(hint) : nullptr;
  auto *checked = this->exprChecker->checkExpr(call->args[index], listHint);
  call->args[index] = this->dictChecker->iterableOf(checked);
  if (call->args[index]->type->isIterator()) {
    call->args[index] = this->spreadIteratorValues(call->args[index]);
  }
  Expr *arg = call->args[index];
  const Type *type = arg->type->isArray() ? arg->type->inner() : arg->type;
  bool isNumericArray = arg->type->isArray() && type->isNumeric();
  if (!arg->type->isError() && !isNumericArray) {
    auto message = ErrorTextMath::spread(arg->type->name());
    this->state.getDiagnostics().error(arg->loc(), message);
    values.isNumeric = false;
  }
  values.hasSomeValue |= this->indexProofs->canProveNotEmpty(arg);

  return type;
}

Expr *MathChecker::spreadIteratorValues(Expr *arg) {
  auto nothing = this->generatorChecker->checkSendsNothing(arg->type, "a spread");
  if (nothing.isErr()) {
    this->state.getDiagnostics().error(arg->loc(), nothing.error().message);
    return arg;
  }
  auto *values = this->state.getAst().make<ArrayLitExpr>(arg->loc());
  values->elements.push_back({arg, true});
  values->type = Type::array(arg->type->yielded());
  this->flowChecker->calledFunction();

  return values;
}

}
