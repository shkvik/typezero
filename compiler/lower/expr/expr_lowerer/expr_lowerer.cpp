#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/library/bytes/bytes_lowerer/bytes_lowerer.h"
#include "compiler/library/decimal/decimal_lowerer/decimal_lowerer.h"
#include "compiler/library/json/json_lowerer/json_lowerer.h"
#include "compiler/library/reflect/reflect_lowerer/reflect_lowerer.h"
#include "compiler/library/regex/matcher_builder/matcher_builder.h"
#include "compiler/library/regex/regex_lowerer/regex_lowerer.h"
#include "compiler/library/result/result_lowerer/result_lowerer.h"
#include "compiler/library/tz_modules/tz_modules_lowerer/tz_modules_lowerer.h"
#include "compiler/lower/async/async_lowerer/async_lowerer.h"
#include "compiler/lower/classes/class_lowerer/class_lowerer.h"
#include "compiler/lower/classes/decorator_lowerer/decorator_lowerer.h"
#include "compiler/lower/expr/assign_lowerer/assign_lowerer.h"
#include "compiler/lower/expr/binary_lowerer/binary_lowerer.h"
#include "compiler/lower/expr/call_lowerer/call_lowerer.h"
#include "compiler/lower/expr/member_lowerer/member_lowerer.h"
#include "compiler/lower/expr/truth_lowerer/truth_lowerer.h"
#include "compiler/lower/expr/unary_lowerer/unary_lowerer.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/functions/closure_lowerer/closure_lowerer.h"
#include "compiler/lower/functions/env_lowerer/env_lowerer.h"
#include "compiler/lower/iterators/inline_generator_lowerer/inline_generator_lowerer.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/objects/object_lowerer/object_lowerer.h"
#include "compiler/lower/stmt/stmt_lowerer/stmt_lowerer.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/lower/types/union_layout/union_layout.h"
#include "compiler/lower/types/union_lowerer/union_lowerer.h"
#include "compiler/lower/types/unknown_lowerer/unknown_lowerer.h"
#include "compiler/lower/values/array_lowerer/array_lowerer.h"
#include "compiler/lower/values/array_method_lowerer/array_method_lowerer.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/ErrorHandling.h"
#include <array>

using llvm::BasicBlock;
using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::Value;

namespace tz {

namespace {

// A value held as a pointer, null for undefined in `T | undefined`: an
// object, an iterator, a Json, a dictionary or a Promise.
bool isReference(const Type *type) {
  return type->isObject() || type->isIterator() || type->isJson() || type->isDict() ||
         type->isPromise() || type->isUnknown() || type->isClassRef() ||
         (type->isUnion() && !type->isTagged() && type->slots().front() == UnionSlot::Object);
}

} // namespace

ExprLowerer::ExprLowerer(Lowering &lowering) : lowering_(lowering) {}

// A built-in that fails in it throws from where the expression is (loc_).
llvm::Value *ExprLowerer::emitExpr(Expr *expr) {
  llvm::SMLoc outer = std::exchange(lowering_.loc_, expr->loc());
  lowering_.orderAt(expr->loc());
  llvm::Value *value = emitExprKind(expr);
  lowering_.loc_ = outer;

  return value;
}

llvm::Value *ExprLowerer::emitExprKind(Expr *expr) {
  switch (expr->kind()) {
  case Expr::Kind::IntLit: {
    auto *lit = cast<IntLitExpr>(expr);
    const Type *type = UnionLayout::storedType(lit->type); // `0 | 1` holds an int
    if (type->isFloat()) {
      return llvm::ConstantFP::get(lowering_.builder_.getDoubleTy(),
                                   static_cast<double>(lit->value));
    }
    if (type->isDecimal()) {
      return lowering_.decimalLowerer().emitDecimalLiteral(lit);
    }
    return llvm::ConstantInt::get(lowering_.moduleBuilder().lower(type), lit->value);
  }
  case Expr::Kind::FloatLit:
    if (expr->type->isDecimal()) {
      return lowering_.decimalLowerer().emitDecimalLiteral(expr);
    }
    return llvm::ConstantFP::get(lowering_.builder_.getDoubleTy(), cast<FloatLitExpr>(expr)->value);
  case Expr::Kind::BoolLit:
    return lowering_.builder_.getInt1(cast<BoolLitExpr>(expr)->value);
  case Expr::Kind::StringLit:
    if (expr->type->isStringLiteral()) { // a literal type: its atom
      return lowering_.builder_.getInt32(expr->type->atom());
    }
    return lowering_.stringLowerer().stringLiteral(cast<StringLitExpr>(expr)->value);
  case Expr::Kind::RegexLit:
    return lowering_.regexLowerer().emitRegexLiteral(cast<RegexLitExpr>(expr));
  case Expr::Kind::Undefined:
    return lowering_.builder_.getFalse(); // conversions and printing handle it
  case Expr::Kind::Template:
    return lowering_.stringLowerer().emitTemplate(cast<TemplateExpr>(expr));
  case Expr::Kind::ArrayLit:
    return lowering_.arrayLowerer().emitArrayLiteral(cast<ArrayLitExpr>(expr));
  case Expr::Kind::ObjectLit:
    return lowering_.objectLowerer().emitObjectLiteral(cast<ObjectLitExpr>(expr));
  case Expr::Kind::Name: {
    auto *name = cast<NameExpr>(expr);
    if (name->func) { // a top-level function as a value
      return lowering_.closureLowerer().makeFunctionValue(
          lowering_.closureLowerer().functionInfo(name->func->decl),
          llvm::ConstantPointerNull::get(lowering_.builder_.getPtrTy()));
    }
    if (!name->var && name->type->isJson()) { // `null`, which only a Json holds
      return lowering_.jsonLowerer().emitJsonNull();
    }
    if (name->cls) { // a class as a value
      return lowering_.unknownLowerer().classObject(name->cls);
    }
    return lowering_.envLowerer().emitVarValue(name->var, /*borrow=*/false);
  }
  case Expr::Kind::Function:
    return lowering_.closureLowerer().emitFunctionExpr(cast<FunctionExpr>(expr));
  case Expr::Kind::Member:
    return lowering_.memberLowerer().emitMember(cast<MemberExpr>(expr));
  case Expr::Kind::Index: {
    auto *index = cast<IndexExpr>(expr);
    if (index->enumDecl) {
      return lowering_.unaryLowerer().emitEnumName(index);
    }
    if (index->object->type->nonOptional()->isJson()) {
      return lowering_.jsonLowerer().emitJsonIndex(index);
    }
    if (index->object->type->isString()) {
      return lowering_.stringLowerer().emitStringIndex(index);
    }
    if (index->keyed) { // `o[k]` with a key of literals
      Lowering::Operand object = lowering_.refCounts().emitOperand(
          index->object, AssignLowerer::cannotAssign(index->index));
      llvm::Value *key = emitExpr(index->index);
      llvm::Value *value =
          lowering_.builder_.CreateLoad(lowering_.moduleBuilder().lower(index->type),
                                        lowering_.objectLowerer().keyedFieldAddress(
                                            object.value, object.type, key, index->index->type),
                                        "field");
      lowering_.refCounts().retainValue(value, index->type);
      lowering_.refCounts().dropOperand(object);
      return value;
    }
    if (index->object->type->isTypedArray()) {
      return lowering_.bytesLowerer().emitBytesIndex(index);
    }
    return lowering_.arrayLowerer().emitArrayIndex(index);
  }
  case Expr::Kind::Unary:
    return lowering_.unaryLowerer().emitUnary(cast<UnaryExpr>(expr));
  case Expr::Kind::Binary:
    return lowering_.binaryLowerer().emitBinary(cast<BinaryExpr>(expr));
  case Expr::Kind::Assign:
    return lowering_.assignLowerer().emitAssign(cast<AssignExpr>(expr), /*needResult=*/true);
  case Expr::Kind::Update:
    return lowering_.assignLowerer().emitUpdate(cast<UpdateExpr>(expr));
  case Expr::Kind::Call:
    return lowering_.callLowerer().emitCall(cast<CallExpr>(expr));
  case Expr::Kind::New: {
    auto *create = cast<NewExpr>(expr);
    if (create->reflect) {
      return lowering_.reflectLowerer().emitReflectNew(create);
    }
    if (create->cls) {
      return lowering_.classLowerer().emitNew(create);
    }
    return lowering_.arrayLowerer().emitNewArray(create);
  }
  case Expr::Kind::Class:
    return lowering_.decoratorLowerer().emitClassExpr(cast<ClassExpr>(expr));
  case Expr::Kind::Conditional:
    return emitConditional(cast<ConditionalExpr>(expr));
  case Expr::Kind::Cast: {
    auto *c = cast<CastExpr>(expr);
    if (c->shortCircuit) {
      return emitChainLink(c);
    }
    if (c->adaptsResult) {
      return lowering_.tzModulesLowerer().emitResultAdapter(c);
    }
    if (c->type->isFunction() && c->operand->type->isFunction()) {
      return lowering_.closureLowerer().emitFunctionAdapter(c);
    }
    // The text of a value of a union, whatever member it holds.
    if (c->text) {
      llvm::Value *value = emitExpr(c->operand);
      llvm::Value *text = lowering_.arrayMethodLowerer().emitValueToString(value, c->operand->type);
      lowering_.refCounts().releaseValue(value, c->operand->type);
      return text;
    }
    // A Json known to be a boolean is that boolean, not a truth to test.
    if (c->type->isBool() && !c->operand->type->isBool() &&
        !c->operand->type->nonOptional()->isJson() && !c->operand->type->isUnknown()) {
      return lowering_.truthLowerer().emitTruthOf(c->operand);
    }
    return emitConversion(emitExpr(c->operand), c->operand->type, c->type);
  }
  case Expr::Kind::Sequence: {
    auto *seq = cast<SequenceExpr>(expr);
    for (size_t i = 0; i + 1 < seq->exprs.size(); ++i) {
      lowering_.stmtLowerer().emitDiscarded(seq->exprs[i]);
    }
    return emitExpr(seq->exprs.back());
  }
  case Expr::Kind::OptionalChain:
    return emitOptionalChain(cast<OptionalChainExpr>(expr));
  case Expr::Kind::Yield:
    // In the generator of safeTry, which is no generator, `yield* r` takes
    // the value of the Result r or returns its Err.
    if (lowering_.currentFn_ && lowering_.currentFn_->safeTry) {
      return lowering_.resultLowerer().emitSafeTryYield(cast<YieldExpr>(expr));
    }
    return lowering_.inlineGeneratorLowerer().emitYield(cast<YieldExpr>(expr));
  case Expr::Kind::Await:
    return lowering_.asyncLowerer().emitAwait(cast<AwaitExpr>(expr));
  }
  llvm_unreachable("unknown expression kind");
}

// `a?.b.c`: each link whose object is undefined jumps to the end, where the
// chain is undefined.
llvm::Value *ExprLowerer::emitOptionalChain(OptionalChainExpr *chain) {
  if (!chain->mayShortCircuit) {
    return emitExpr(chain->chain);
  }
  llvm::BasicBlock *undefinedBlock = lowering_.functionFrame().newBlock("chain.undefined");
  llvm::BasicBlock *endBlock = lowering_.functionFrame().newBlock("chain.end");
  lowering_.chainEnds_.push_back(undefinedBlock);
  llvm::Value *value = emitExpr(chain->chain);
  lowering_.chainEnds_.pop_back();
  const Type *type = chain->chain->type;
  if (!type->isVoid() && !type->isOptional()) {
    value =
        lowering_.optionalLowerer().makeOptional(value, lowering_.builder_.getTrue(), chain->type);
  }
  llvm::BasicBlock *valueEnd = lowering_.builder_.GetInsertBlock();
  lowering_.builder_.CreateBr(endBlock);
  lowering_.functionFrame().switchTo(undefinedBlock);
  lowering_.builder_.CreateBr(endBlock);
  lowering_.functionFrame().switchTo(endBlock);
  if (type->isVoid()) {
    return nullptr;
  }
  llvm::PHINode *phi =
      lowering_.builder_.CreatePHI(lowering_.moduleBuilder().lower(chain->type), 2);
  phi->addIncoming(value, valueEnd);
  phi->addIncoming(lowering_.optionalLowerer().undefinedValue(chain->type), undefinedBlock);

  return phi;
}

llvm::Value *ExprLowerer::emitChainLink(CastExpr *link) {
  const Type *type = link->operand->type;
  if (type->nonOptional()->isJson()) {
    return lowering_.jsonLowerer().emitJsonChainLink(link);
  }
  llvm::Value *value = emitExpr(link->operand);
  llvm::BasicBlock *valueBlock = lowering_.functionFrame().newBlock("chain.link");
  // Undefined holds nothing to release.
  lowering_.builder_.CreateCondBr(lowering_.optionalLowerer().optionalHas(value, type), valueBlock,
                                  lowering_.chainEnds_.back());
  lowering_.functionFrame().switchTo(valueBlock);

  return lowering_.optionalLowerer().optionalValue(value, type);
}

llvm::Value *ExprLowerer::emitConditional(ConditionalExpr *cond) {
  llvm::Value *test = emitExpr(cond->cond);
  llvm::BasicBlock *thenBlock = lowering_.functionFrame().newBlock("cond.then");
  llvm::BasicBlock *elseBlock = lowering_.functionFrame().newBlock("cond.else");
  llvm::BasicBlock *endBlock = lowering_.functionFrame().newBlock("cond.end");
  lowering_.builder_.CreateCondBr(test, thenBlock, elseBlock);

  lowering_.functionFrame().switchTo(thenBlock);
  llvm::Value *thenValue = emitExpr(cond->thenExpr);
  llvm::BasicBlock *thenEnd = lowering_.builder_.GetInsertBlock();
  lowering_.builder_.CreateBr(endBlock);

  lowering_.functionFrame().switchTo(elseBlock);
  llvm::Value *elseValue = emitExpr(cond->elseExpr);
  llvm::BasicBlock *elseEnd = lowering_.builder_.GetInsertBlock();
  lowering_.builder_.CreateBr(endBlock);

  lowering_.functionFrame().switchTo(endBlock);
  if (cond->type->isVoid()) {
    return nullptr;
  }
  llvm::PHINode *phi = lowering_.builder_.CreatePHI(lowering_.moduleBuilder().lower(cond->type), 2);
  phi->addIncoming(thenValue, thenEnd);
  phi->addIncoming(elseValue, elseEnd);

  return phi;
}

llvm::Value *ExprLowerer::emitConversion(llvm::Value *value, const Type *from, const Type *to) {
  if (from == to) {
    return value;
  }
  // A string of a pattern (Type::stringPattern) is a string as it is.
  if (from->isString() && to->isString()) {
    return value;
  }
  // A value that cannot exist, in code that never runs: the callback of
  // mapErr on a Result that cannot be an Err.
  if (from->isNever()) {
    return llvm::Constant::getNullValue(lowering_.resultLowerer().lowerHalf(to));
  }
  if (from->isUnknown() || to->isUnknown()) {
    return lowering_.unknownLowerer().emitUnknownConversion(value, from, to);
  }
  // A variable narrowed to never (every member of its union ruled out), in
  // code that never runs.
  if (to->isNever()) {
    lowering_.refCounts().releaseValue(value, from);
    return llvm::Constant::getNullValue(lowering_.moduleBuilder().lower(to));
  }
  // A number or boolean literal type is a value of its base.
  from = UnionLayout::storedType(from);
  to = UnionLayout::storedType(to);
  if (from == to) {
    return value;
  }
  auto unionLike = [](const Type *t) {
    t = t->nonOptional();

    return t->isUnion() || t->isStringLiteral();
  };
  if (unionLike(from) || unionLike(to)) {
    return lowering_.unionLowerer().emitUnionConversion(value, from, to);
  }
  if ((from->nonOptional()->isJson() || to->nonOptional()->isJson()) &&
      from->nonOptional() != to->nonOptional()) {
    return lowering_.jsonLowerer().emitJsonConversion(value, from, to);
  }
  if (from->isResult() && to->isResult()) {
    return lowering_.resultLowerer().makeResult(
        to, lowering_.resultLowerer().resultIsOk(value, from),
        lowering_.resultLowerer().convertHalf(
            lowering_.resultLowerer().resultHalf(value, from, false), from->okType(), to->okType()),
        lowering_.resultLowerer().convertHalf(
            lowering_.resultLowerer().resultHalf(value, from, true), from->errType(),
            to->errType()));
  }

  if (to->isString()) {
    if (from->isBool()) {
      return lowering_.builder_.CreateSelect(value,
                                             lowering_.stringLowerer().stringLiteral(u"true"),
                                             lowering_.stringLowerer().stringLiteral(u"false"));
    }
    if (from->isInteger()) {
      return lowering_.stringLowerer().callReturningString(
          lowering_.rt_.strFromI64,
          {lowering_.builder_.CreateSExt(value, lowering_.builder_.getInt64Ty())});
    }
    if (from->isFloat()) {
      return lowering_.stringLowerer().callReturningString(lowering_.rt_.strFromF64, {value});
    }
    if (from->isUndefined()) {
      return lowering_.stringLowerer().stringLiteral(u"undefined");
    }
    if (from->isDecimal()) {
      llvm::Type *i64 = lowering_.builder_.getInt64Ty(), *ptr = lowering_.builder_.getPtrTy();
      llvm::Value *text = lowering_.stringLowerer().callReturningString(
          lowering_.decimalLowerer().decimalFunction(
              "tz_dec_to_string", lowering_.builder_.getVoidTy(), {ptr, i64, i64}),
          {lowering_.decimalLowerer().decimalCoef(value),
           lowering_.decimalLowerer().decimalMeta(value)});
      lowering_.refCounts().releaseValue(value, from);
      return text;
    }
    // Arrays and optionals: the runtime knows how to show any value.
    llvm::Value *result = lowering_.arrayMethodLowerer().emitValueToString(value, from);
    lowering_.refCounts().releaseValue(value, from);
    return result;
  }

  if (to->isOptional()) {
    if (from->isUndefined()) {
      return lowering_.optionalLowerer().undefinedValue(to);
    }
    if (from->isOptional()) { // numeric widening, which keeps zero zero, or an object
      return lowering_.optionalLowerer().makeOptional(
          emitConversion(lowering_.optionalLowerer().optionalValue(value, from), from->inner(),
                         to->inner()),
          lowering_.optionalLowerer().optionalHas(value, from), to);
    }
    return lowering_.optionalLowerer().makeOptional(emitConversion(value, from, to->inner()),
                                                    lowering_.builder_.getTrue(), to);
  }
  if (from->isOptional()) { // narrowed: known to hold a value
    return emitConversion(lowering_.optionalLowerer().optionalValue(value, from), from->inner(),
                          to);
  }
  // An object seen through a type with read-only fields, or an iterator
  // through one that drops what it returns; an array of objects as one of a
  // type they are (`Dog[]` as `Animal[]`).
  if (isReference(from) || (from->isArray() && to->isArray())) {
    return value;
  }
  if (from->isInteger() && to->isDecimal()) {
    return lowering_.decimalLowerer().decimalFromInteger(value);
  }

  llvm::Type *target = lowering_.moduleBuilder().lower(to);
  if (from->isInteger() && to->isInteger()) {
    return from->bitWidth() < to->bitWidth() ? lowering_.builder_.CreateSExt(value, target)
                                             : lowering_.builder_.CreateTrunc(value, target);
  }
  if (from->isInteger() && to->isFloat()) {
    return lowering_.builder_.CreateSIToFP(value, target);
  }
  // An operand of a bitwise operator: JavaScript's ToInt32. A double becomes a
  // long only through Math functions (emitMathCall).
  if (from->isFloat() && to->isInt()) {
    return lowering_.builder_.CreateCall(lowering_.rt_.toInt32, {value});
  }
  llvm_unreachable("Checker rejects this conversion");
}

} // namespace tz
