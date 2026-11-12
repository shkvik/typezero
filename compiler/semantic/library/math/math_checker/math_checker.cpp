#include "compiler/semantic/library/math/math_checker/math_checker.h"
#include "compiler/program/library/builtin_names/global_constant_table/global_constant_table.h"
#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/program/library/math/bit_function_table/bit_function_table.h"
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

MathChecker::ResultWrapBitsWidth MathChecker::wrapBitsWidth(llvm::StringRef name) const {
  unsigned bits = name == "Wrap32" ? 32 : name == "Wrap64" ? 64 : 0;
  if (!bits) {
    auto message = ErrorTextBit::noMember(name);

    return err(MathCheckerWrapBitsWidthError{
        .code = MathCheckerWrapBitsWidthErrorCode::NO_MEMBER,
        .message = message,
    });
  }

  return ok(bits);
}

MathChecker::ResultRoundedDouble MathChecker::roundedDouble(llvm::StringRef name,
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
  arg = this->genericChecker->apparent(this->exprChecker->checkExpr(arg, hint));
  const Type *type = arg->type;
  if (!type->isNumeric() && !type->isError()) {
    auto message = ErrorTextMathArg::notDouble(type);
    this->state.getDiagnostics().error(arg->loc(), message);
  }

  return arg;
}

Expr *MathChecker::checkMathCall(CallExpr *call, MemberExpr *member, const Type *expected) {
  const auto *fn = MathFunctionTable::find(member->name);
  if (fn == nullptr) {
    return this->reportMathMember(call, member);
  }
  bool isMinMax = fn->form == MathFunctionForm::MIN_MAX;
  if (!isMinMax && this->callChecker->rejectSpread(call)) {
    return call;
  }
  auto arity = this->argumentChecker->checkArity(call, fn->minArgs, fn->maxArgs);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(member->loc(), arity.error().message);
    return call;
  }
  if (isMinMax && call->args.empty()) {
    auto message = ErrorTextMath::noValues(member->name);
    this->state.getDiagnostics().error(call->loc(), message);
    return call;
  }
  if (fn->form == MathFunctionForm::POW) {
    auto *pow = this->state.getAst().make<BinaryExpr>(call->loc(), BinaryOp::Pow, call->args[0],
                                                      call->args[1]);
    return this->binaryChecker->checkBinary(pow, expected);
  }
  call->target = BuiltinCall{.builtin = fn->builtin};
  const Type *wanted = expected != nullptr ? expected->nonOptional() : nullptr;
  const Type *hint = wanted != nullptr && wanted->isNumeric() ? wanted : nullptr;
  if (call->hasSpread()) {
    return this->checkMinMaxSpread(call, hint);
  }
  if (!this->mathArgsNumeric(call, fn->form, hint)) {
    return call;
  }

  return this->checkMathResult(call, member, *fn);
}

Expr *MathChecker::reportMathMember(CallExpr *call, MemberExpr *member) {
  auto constant = GlobalConstantTable::find(GlobalName::MATH, member->name);
  bool isConstant = constant.has_value();
  std::string message =
      isConstant ? ErrorTextMath::constant(member->name) : ErrorTextMath::noMember(member->name);
  this->state.getDiagnostics().error(member->loc(), message);
  for (Expr *&arg : call->args) {
    arg = this->exprChecker->checkExpr(arg);
  }

  return call;
}

bool MathChecker::mathArgsNumeric(CallExpr *call, MathFunctionForm form, const Type *hint) {
  bool isNumeric = true;
  for (Expr *&arg : call->args) {
    arg = this->numericArg(arg, this->mathArgHint(form, hint));
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

Expr *MathChecker::checkMathResult(CallExpr *call, MemberExpr *member, const MathFunctionInfo &fn) {
  switch (fn.form) {
    case MathFunctionForm::ROUNDING:
      return this->checkRounding(call, member);
    case MathFunctionForm::FLOAT:
      for (Expr *&arg : call->args) {
        arg = this->typeRelations->coerce(arg, Type::doubleTy());
      }
      call->type = Type::doubleTy();
      return call;
    case MathFunctionForm::SAME:
      call->type = call->args[0]->type;
      return call;
    case MathFunctionForm::MIN_MAX:
    case MathFunctionForm::CLAMP:
      return this->checkSharedResult(call);
    case MathFunctionForm::INT32:
      return this->checkInt32Math(call, member, fn.builtin);
    case MathFunctionForm::POW:
      return call;
  }

  return call;
}

Expr *MathChecker::checkRounding(CallExpr *call, MemberExpr *member) {
  auto rounded = this->roundedDouble(member->name, call->args[0]->type);
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
  std::string what = "Math." + member->name.str();
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
  auto spread = [&](size_t index) { return index < spreads.size() && spreads[index]; };
  for (size_t index = 0; index < args.size() && common == nullptr; ++index) {
    if (!spread(index) && !this->typeRelations->isIntLiteralTree(args[index])) {
      common = args[index]->type;
    }
  }
  if (common == nullptr) {
    common = Type::intTy();
    for (size_t index = 0; index < args.size(); ++index) {
      if (!spread(index) && args[index]->type->isFloat()) {
        common = Type::doubleTy();
      }
    }
  }
  for (size_t index = 0; index < args.size(); ++index) {
    if (spread(index) || args[index]->type == common) {
      continue;
    }
    if (this->typeRelations->isLiteralAdapting(args[index], common)) {
      args[index] = this->typeRelations->adaptLiteral(args[index], common);
      continue;
    }
    auto hint = this->typeRelations->conversionHint(args[index], common);
    auto message = ErrorTextMath::oneType(common->name(), args[index]->type->name(), hint);
    this->state.getDiagnostics().error(args[index]->loc(), message);
    return nullptr;
  }

  return common;
}

Expr *MathChecker::bitsArg(Expr *arg, unsigned bits, const Type *literal, const std::string &what) {
  if (arg->type == nullptr) {
    const Type *hint = literal != nullptr ? literal : Type::unsignedInteger(bits);
    arg = this->exprChecker->checkExpr(
        arg, this->typeRelations->isIntLiteralTree(arg) && !isa<NameExpr>(arg) ? hint : nullptr);
  }
  const Type *type = arg->type;
  if (type->isError()) {
    return arg;
  }
  if (type->isInteger() && type->bitWidth() == bits) {
    return arg;
  }
  const Type *target = literal != nullptr ? literal : Type::unsignedInteger(bits);
  if (this->typeRelations->isLiteralAdapting(arg, target)) {
    return this->typeRelations->adaptLiteral(arg, target);
  }
  if (literal != nullptr &&
      this->typeRelations->isLiteralAdapting(arg, Type::unsignedInteger(bits))) {
    return this->typeRelations->adaptLiteral(arg, Type::unsignedInteger(bits));
  }
  auto method = this->typeRelations->conversionMethod(Type::unsignedInteger(bits));
  std::string hint = type->isInteger()
                         ? this->typeRelations->conversionHint(arg, Type::unsignedInteger(bits))
                     : type->isFloat() ? ErrorTextMathArg::trunc(method)
                                       : std::string();
  auto message = ErrorTextMathArg::bits(what, bits, type->name(), hint);
  this->state.getDiagnostics().error(arg->loc(), message);
  arg->type = Type::error();

  return arg;
}

Expr *MathChecker::checkBitCall(CallExpr *call, MemberExpr *member) {
  call->type = Type::error();
  auto *inner = dyn_cast<MemberExpr>(member->object);
  if (inner != nullptr) {
    return this->checkWrapFrom(call, member, inner);
  }
  const auto *fn = BitFunctionTable::find(member->name);
  if (fn == nullptr) {
    auto message = ErrorTextBit::noMember(member->name);
    this->state.getDiagnostics().error(member->loc(), message);
    return this->checkArgsLoosely(call);
  }
  bool isRejected = this->callChecker->rejectSpread(call);
  auto arity = isRejected ? ok(call) : this->argumentChecker->checkArity(call, fn->args, fn->args);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(member->loc(), arity.error().message);
  }
  if (isRejected || arity.isErr()) {
    return call;
  }
  auto what = ErrorTextBit::label(member->name);
  call->target = BuiltinCall{.builtin = fn->builtin};
  if (fn->wide) {
    return this->checkWideBits(call, *fn, what);
  }

  return this->checkPlainBits(call, *fn, what);
}

Expr *MathChecker::checkArgsLoosely(CallExpr *call) {
  for (Expr *&arg : call->args) {
    arg = this->exprChecker->checkExpr(arg);
  }

  return call;
}

Expr *MathChecker::checkWrapFrom(CallExpr *call, MemberExpr *member, MemberExpr *inner) {
  auto width = this->wrapBitsWidth(inner->name);
  if (width.isErr()) {
    this->state.getDiagnostics().error(inner->loc(), width.error().message);
    return this->checkArgsLoosely(call);
  }
  unsigned bits = width.value();
  if (member->name != "from") {
    auto message = ErrorTextBit::onlyFrom(inner->name);
    this->state.getDiagnostics().error(member->loc(), message);
    return this->checkArgsLoosely(call);
  }
  auto arity = this->argumentChecker->checkArity(call, 1, 1);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(member->loc(), arity.error().message);
    return call;
  }
  call->args[0] = this->exprChecker->checkExpr(call->args[0]);
  const Expr *arg = call->args[0];
  if (!arg->type->isInteger()) {
    if (!arg->type->isError()) {
      auto message = ErrorTextBit::fromInt(inner->name, arg->type->name());
      this->state.getDiagnostics().error(arg->loc(), message);
    }
    return call;
  }
  call->target = BuiltinCall{.builtin = Builtin::BitWrapFrom};
  call->type = Type::wrapping(bits);

  return call;
}

Expr *MathChecker::checkWideBits(CallExpr *call, const BitFunctionInfo &fn,
                                 const std::string &what) {
  const auto *u64 = Type::unsignedInteger(64);
  for (Expr *&arg : call->args) {
    arg = this->bitsArg(arg, 64, nullptr, what);
    if (arg->type->isError()) {
      return call;
    }
    if (arg->type != u64 && !arg->type->isWrapping()) {
      auto hint = this->typeRelations->conversionHint(arg, u64);
      auto message = ErrorTextBit::uint64(what, arg->type->name(), hint);
      this->state.getDiagnostics().error(arg->loc(), message);
      return call;
    }
    arg = this->typeRelations->convert(arg, u64);
  }
  call->type = Type::object({{fn.first.str(), u64, false}, {fn.second.str(), u64, false}});

  return call;
}

Expr *MathChecker::checkPlainBits(CallExpr *call, const BitFunctionInfo &fn,
                                  const std::string &what) {
  call->args[0] = this->bitsArg(call->args[0], fn.bits, nullptr, what);
  if (call->args[0]->type->isError()) {
    return call;
  }
  if (fn.args == 2 && !this->bitCountFits(call, what)) {
    return call;
  }
  call->type = fn.counts ? Type::intTy() : call->args[0]->type;

  return call;
}

bool MathChecker::bitCountFits(CallExpr *call, const std::string &what) {
  call->args[1] = this->exprChecker->checkExpr(call->args[1]);
  const Type *count = call->args[1]->type;
  if (!count->isInteger()) {
    if (!count->isError()) {
      auto message = ErrorTextBit::count(what, count->name());
      this->state.getDiagnostics().error(call->args[1]->loc(), message);
    }
    return false;
  }
  call->args[1] = this->typeRelations->convert(call->args[1], call->args[0]->type);

  return true;
}

Expr *MathChecker::checkMinMaxSpread(CallExpr *call, const Type *hint) {
  SpreadValues values{
      .common = nullptr,
      .someValue = false,
      .numeric = true,
  };
  for (size_t index = 0; index < call->args.size(); ++index) {
    if (!this->minMaxArgFits(call, index, hint, values)) {
      return call;
    }
  }
  if (!values.numeric) {
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
  call->type = values.someValue ? common : Type::optional(common);
  call->mayBeOutOfRange = !values.someValue;

  return call;
}

bool MathChecker::minMaxArgFits(CallExpr *call, size_t index, const Type *hint,
                                SpreadValues &values) {
  bool isSpread = call->spreads(index);
  const Type *type = nullptr;
  if (isSpread) {
    type = this->spreadElementType(call, index, hint, values);
  } else {
    call->args[index] = this->numericArg(call->args[index], hint);
    type = call->args[index]->type;
    values.someValue = true;
  }
  values.numeric &= type->isNumeric();
  if (!values.numeric || !isSpread) {
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
    values.numeric = false;
  }
  values.someValue |= this->indexProofs->canProveNotEmpty(arg);

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
