#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/lower/stmt/stmt_lowerer/stmt_lowerer.h"
#include "compiler/lower/expr/unary_lowerer/unary_lowerer.h"
#include "compiler/lower/expr/binary_lowerer/binary_lowerer.h"
#include "compiler/lower/expr/truth_lowerer/truth_lowerer.h"
#include "compiler/lower/expr/assign_lowerer/assign_lowerer.h"
#include "compiler/lower/expr/call_lowerer/call_lowerer.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "compiler/lower/expr/member_lowerer/member_lowerer.h"
#include "compiler/lower/values/array_lowerer/array_lowerer.h"
#include "compiler/lower/values/array_method_lowerer/array_method_lowerer.h"
#include "compiler/lower/async/async_lowerer/async_lowerer.h"
#include "compiler/lower/classes/class_lowerer/class_lowerer.h"
#include "compiler/lower/functions/closure_lowerer/closure_lowerer.h"
#include "compiler/lower/functions/env_lowerer/env_lowerer.h"
#include "compiler/lower/classes/decorator_lowerer/decorator_lowerer.h"
#include "compiler/lower/iterators/inline_generator_lowerer/inline_generator_lowerer.h"
#include "compiler/lower/objects/object_lowerer/object_lowerer.h"
#include "compiler/lower/types/union_layout/union_layout.h"
#include "compiler/lower/types/union_lowerer/union_lowerer.h"
#include "compiler/lower/types/unknown_lowerer/unknown_lowerer.h"
#include "compiler/library/bytes/bytes_lowerer/bytes_lowerer.h"
#include "compiler/library/decimal/decimal_lowerer/decimal_lowerer.h"
#include "compiler/library/json/json_lowerer/json_lowerer.h"
#include "compiler/library/reflect/reflect_lowerer/reflect_lowerer.h"
#include "compiler/library/regex/regex_lowerer/regex_lowerer.h"
#include "compiler/library/result/result_lowerer/result_lowerer.h"
#include "compiler/library/tz_modules/tz_modules_lowerer/tz_modules_lowerer.h"
#include "compiler/library/regex/matcher_builder/matcher_builder.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/ErrorHandling.h"
#include <array>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::BasicBlock;
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

ExprLowerer::ExprLowerer(CodeGen &codegen) : codegen_(codegen) {}

// A built-in that fails in it throws from where the expression is (loc_).
llvm::Value *ExprLowerer::emitExpr(Expr *expr) {
  llvm::SMLoc outer = std::exchange(codegen_.loc_, expr->loc());
  codegen_.orderAt(expr->loc());
  llvm::Value *value = emitExprKind(expr);
  codegen_.loc_ = outer;

  return value;
}

llvm::Value *ExprLowerer::emitExprKind(Expr *expr) {
  switch (expr->kind()) {
  case Expr::Kind::IntLit: {
    auto *lit = cast<IntLitExpr>(expr);
    const Type *type = UnionLayout::storedType(lit->type); // `0 | 1` holds an int
    if (type->isFloat()) {
      return llvm::ConstantFP::get(codegen_.builder_.getDoubleTy(), static_cast<double>(lit->value));
    }
    if (type->isDecimal()) {
      return codegen_.decimalLowerer().emitDecimalLiteral(lit);
    }
    return llvm::ConstantInt::get(codegen_.moduleBuilder().lower(type), lit->value);
  }
  case Expr::Kind::FloatLit:
    if (expr->type->isDecimal()) {
      return codegen_.decimalLowerer().emitDecimalLiteral(expr);
    }
    return llvm::ConstantFP::get(codegen_.builder_.getDoubleTy(), cast<FloatLitExpr>(expr)->value);
  case Expr::Kind::BoolLit:
    return codegen_.builder_.getInt1(cast<BoolLitExpr>(expr)->value);
  case Expr::Kind::StringLit:
    if (expr->type->isStringLiteral()) { // a literal type: its atom
      return codegen_.builder_.getInt32(expr->type->atom());
    }
    return codegen_.stringLowerer().stringLiteral(cast<StringLitExpr>(expr)->value);
  case Expr::Kind::RegexLit:
    return codegen_.regexLowerer().emitRegexLiteral(cast<RegexLitExpr>(expr));
  case Expr::Kind::Undefined:
    return codegen_.builder_.getFalse(); // conversions and printing handle it
  case Expr::Kind::Template:
    return codegen_.stringLowerer().emitTemplate(cast<TemplateExpr>(expr));
  case Expr::Kind::ArrayLit:
    return codegen_.arrayLowerer().emitArrayLiteral(cast<ArrayLitExpr>(expr));
  case Expr::Kind::ObjectLit:
    return codegen_.objectLowerer().emitObjectLiteral(cast<ObjectLitExpr>(expr));
  case Expr::Kind::Name: {
    auto *name = cast<NameExpr>(expr);
    if (name->func) { // a top-level function as a value
      return codegen_.closureLowerer().makeFunctionValue(codegen_.closureLowerer().functionInfo(name->func->decl),
                               llvm::ConstantPointerNull::get(codegen_.builder_.getPtrTy()));
    }
    if (!name->var && name->type->isJson()) { // `null`, which only a Json holds
      return codegen_.jsonLowerer().emitJsonNull();
    }
    if (name->cls) { // a class as a value
      return codegen_.unknownLowerer().classObject(name->cls);
    }
    return codegen_.envLowerer().emitVarValue(name->var, /*borrow=*/false);
  }
  case Expr::Kind::Function:
    return codegen_.closureLowerer().emitFunctionExpr(cast<FunctionExpr>(expr));
  case Expr::Kind::Member:
    return codegen_.memberLowerer().emitMember(cast<MemberExpr>(expr));
  case Expr::Kind::Index: {
    auto *index = cast<IndexExpr>(expr);
    if (index->enumDecl) {
      return codegen_.unaryLowerer().emitEnumName(index);
    }
    if (index->object->type->nonOptional()->isJson()) {
      return codegen_.jsonLowerer().emitJsonIndex(index);
    }
    if (index->object->type->isString()) {
      return codegen_.stringLowerer().emitStringIndex(index);
    }
    if (index->keyed) { // `o[k]` with a key of literals
      CodeGen::Operand object = codegen_.refCounts().emitOperand(index->object, AssignLowerer::cannotAssign(index->index));
      llvm::Value *key = emitExpr(index->index);
      llvm::Value *value = codegen_.builder_.CreateLoad(
          codegen_.moduleBuilder().lower(index->type), codegen_.objectLowerer().keyedFieldAddress(object.value, object.type, key, index->index->type),
          "field");
      codegen_.refCounts().retainValue(value, index->type);
      codegen_.refCounts().dropOperand(object);
      return value;
    }
    if (index->object->type->isTypedArray()) {
      return codegen_.bytesLowerer().emitBytesIndex(index);
    }
    return codegen_.arrayLowerer().emitArrayIndex(index);
  }
  case Expr::Kind::Unary:
    return codegen_.unaryLowerer().emitUnary(cast<UnaryExpr>(expr));
  case Expr::Kind::Binary:
    return codegen_.binaryLowerer().emitBinary(cast<BinaryExpr>(expr));
  case Expr::Kind::Assign:
    return codegen_.assignLowerer().emitAssign(cast<AssignExpr>(expr), /*needResult=*/true);
  case Expr::Kind::Update:
    return codegen_.assignLowerer().emitUpdate(cast<UpdateExpr>(expr));
  case Expr::Kind::Call:
    return codegen_.callLowerer().emitCall(cast<CallExpr>(expr));
  case Expr::Kind::New: {
    auto *create = cast<NewExpr>(expr);
    if (create->reflect) {
      return codegen_.reflectLowerer().emitReflectNew(create);
    }
    if (create->cls) {
      return codegen_.classLowerer().emitNew(create);
    }
    return codegen_.arrayLowerer().emitNewArray(create);
  }
  case Expr::Kind::Class:
    return codegen_.decoratorLowerer().emitClassExpr(cast<ClassExpr>(expr));
  case Expr::Kind::Conditional:
    return emitConditional(cast<ConditionalExpr>(expr));
  case Expr::Kind::Cast: {
    auto *c = cast<CastExpr>(expr);
    if (c->shortCircuit) {
      return emitChainLink(c);
    }
    if (c->adaptsResult) {
      return codegen_.tzModulesLowerer().emitResultAdapter(c);
    }
    if (c->type->isFunction() && c->operand->type->isFunction()) {
      return codegen_.closureLowerer().emitFunctionAdapter(c);
    }
    // The text of a value of a union, whatever member it holds.
    if (c->text) {
      llvm::Value *value = emitExpr(c->operand);
      llvm::Value *text = codegen_.arrayMethodLowerer().emitValueToString(value, c->operand->type);
      codegen_.refCounts().releaseValue(value, c->operand->type);
      return text;
    }
    // A Json known to be a boolean is that boolean, not a truth to test.
    if (c->type->isBool() && !c->operand->type->isBool() &&
        !c->operand->type->nonOptional()->isJson() && !c->operand->type->isUnknown()) {
      return codegen_.truthLowerer().emitTruthOf(c->operand);
    }
    return emitConversion(emitExpr(c->operand), c->operand->type, c->type);
  }
  case Expr::Kind::Sequence: {
    auto *seq = cast<SequenceExpr>(expr);
    for (size_t i = 0; i + 1 < seq->exprs.size(); ++i) {
      codegen_.stmtLowerer().emitDiscarded(seq->exprs[i]);
    }
    return emitExpr(seq->exprs.back());
  }
  case Expr::Kind::OptionalChain:
    return emitOptionalChain(cast<OptionalChainExpr>(expr));
  case Expr::Kind::Yield:
    // In the generator of safeTry, which is no generator, `yield* r` takes
    // the value of the Result r or returns its Err.
    if (codegen_.currentFn_ && codegen_.currentFn_->safeTry) {
      return codegen_.resultLowerer().emitSafeTryYield(cast<YieldExpr>(expr));
    }
    return codegen_.inlineGeneratorLowerer().emitYield(cast<YieldExpr>(expr));
  case Expr::Kind::Await:
    return codegen_.asyncLowerer().emitAwait(cast<AwaitExpr>(expr));
  }
  llvm_unreachable("unknown expression kind");
}

// `a?.b.c`: each link whose object is undefined jumps to the end, where the
// chain is undefined.
llvm::Value *ExprLowerer::emitOptionalChain(OptionalChainExpr *chain) {
  if (!chain->mayShortCircuit) {
    return emitExpr(chain->chain);
  }
  llvm::BasicBlock *undefinedBlock = codegen_.functionFrame().newBlock("chain.undefined");
  llvm::BasicBlock *endBlock = codegen_.functionFrame().newBlock("chain.end");
  codegen_.chainEnds_.push_back(undefinedBlock);
  llvm::Value *value = emitExpr(chain->chain);
  codegen_.chainEnds_.pop_back();
  const Type *type = chain->chain->type;
  if (!type->isVoid() && !type->isOptional()) {
    value = codegen_.optionalLowerer().makeOptional(value, codegen_.builder_.getTrue(), chain->type);
  }
  llvm::BasicBlock *valueEnd = codegen_.builder_.GetInsertBlock();
  codegen_.builder_.CreateBr(endBlock);
  codegen_.functionFrame().switchTo(undefinedBlock);
  codegen_.builder_.CreateBr(endBlock);
  codegen_.functionFrame().switchTo(endBlock);
  if (type->isVoid()) {
    return nullptr;
  }
  llvm::PHINode *phi = codegen_.builder_.CreatePHI(codegen_.moduleBuilder().lower(chain->type), 2);
  phi->addIncoming(value, valueEnd);
  phi->addIncoming(codegen_.optionalLowerer().undefinedValue(chain->type), undefinedBlock);

  return phi;
}

llvm::Value *ExprLowerer::emitChainLink(CastExpr *link) {
  const Type *type = link->operand->type;
  if (type->nonOptional()->isJson()) {
    return codegen_.jsonLowerer().emitJsonChainLink(link);
  }
  llvm::Value *value = emitExpr(link->operand);
  llvm::BasicBlock *valueBlock = codegen_.functionFrame().newBlock("chain.link");
  // Undefined holds nothing to release.
  codegen_.builder_.CreateCondBr(codegen_.optionalLowerer().optionalHas(value, type), valueBlock, codegen_.chainEnds_.back());
  codegen_.functionFrame().switchTo(valueBlock);

  return codegen_.optionalLowerer().optionalValue(value, type);
}

llvm::Value *ExprLowerer::emitConditional(ConditionalExpr *cond) {
  llvm::Value *test = emitExpr(cond->cond);
  llvm::BasicBlock *thenBlock = codegen_.functionFrame().newBlock("cond.then");
  llvm::BasicBlock *elseBlock = codegen_.functionFrame().newBlock("cond.else");
  llvm::BasicBlock *endBlock = codegen_.functionFrame().newBlock("cond.end");
  codegen_.builder_.CreateCondBr(test, thenBlock, elseBlock);

  codegen_.functionFrame().switchTo(thenBlock);
  llvm::Value *thenValue = emitExpr(cond->thenExpr);
  llvm::BasicBlock *thenEnd = codegen_.builder_.GetInsertBlock();
  codegen_.builder_.CreateBr(endBlock);

  codegen_.functionFrame().switchTo(elseBlock);
  llvm::Value *elseValue = emitExpr(cond->elseExpr);
  llvm::BasicBlock *elseEnd = codegen_.builder_.GetInsertBlock();
  codegen_.builder_.CreateBr(endBlock);

  codegen_.functionFrame().switchTo(endBlock);
  if (cond->type->isVoid()) {
    return nullptr;
  }
  llvm::PHINode *phi = codegen_.builder_.CreatePHI(codegen_.moduleBuilder().lower(cond->type), 2);
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
    return llvm::Constant::getNullValue(codegen_.resultLowerer().lowerHalf(to));
  }
  if (from->isUnknown() || to->isUnknown()) {
    return codegen_.unknownLowerer().emitUnknownConversion(value, from, to);
  }
  // A variable narrowed to never (every member of its union ruled out), in
  // code that never runs.
  if (to->isNever()) {
    codegen_.refCounts().releaseValue(value, from);
    return llvm::Constant::getNullValue(codegen_.moduleBuilder().lower(to));
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
    return codegen_.unionLowerer().emitUnionConversion(value, from, to);
  }
  if ((from->nonOptional()->isJson() || to->nonOptional()->isJson()) &&
      from->nonOptional() != to->nonOptional()) {
    return codegen_.jsonLowerer().emitJsonConversion(value, from, to);
  }
  if (from->isResult() && to->isResult()) {
    return codegen_.resultLowerer().makeResult(to, codegen_.resultLowerer().resultIsOk(value, from),
                      codegen_.resultLowerer().convertHalf(codegen_.resultLowerer().resultHalf(value, from, false), from->okType(), to->okType()),
                      codegen_.resultLowerer().convertHalf(codegen_.resultLowerer().resultHalf(value, from, true), from->errType(), to->errType()));
  }

  if (to->isString()) {
    if (from->isBool()) {
      return codegen_.builder_.CreateSelect(value, codegen_.stringLowerer().stringLiteral(u"true"), codegen_.stringLowerer().stringLiteral(u"false"));
    }
    if (from->isInteger()) {
      return codegen_.stringLowerer().callReturningString(codegen_.rt_.strFromI64,
                                 {codegen_.builder_.CreateSExt(value, codegen_.builder_.getInt64Ty())});
    }
    if (from->isFloat()) {
      return codegen_.stringLowerer().callReturningString(codegen_.rt_.strFromF64, {value});
    }
    if (from->isUndefined()) {
      return codegen_.stringLowerer().stringLiteral(u"undefined");
    }
    if (from->isDecimal()) {
      llvm::Type *i64 = codegen_.builder_.getInt64Ty(), *ptr = codegen_.builder_.getPtrTy();
      llvm::Value *text = codegen_.stringLowerer().callReturningString(
          codegen_.decimalLowerer().decimalFunction("tz_dec_to_string", codegen_.builder_.getVoidTy(), {ptr, i64, i64}),
          {codegen_.decimalLowerer().decimalCoef(value), codegen_.decimalLowerer().decimalMeta(value)});
      codegen_.refCounts().releaseValue(value, from);
      return text;
    }
    // Arrays and optionals: the runtime knows how to show any value.
    llvm::Value *result = codegen_.arrayMethodLowerer().emitValueToString(value, from);
    codegen_.refCounts().releaseValue(value, from);
    return result;
  }

  if (to->isOptional()) {
    if (from->isUndefined()) {
      return codegen_.optionalLowerer().undefinedValue(to);
    }
    if (from->isOptional()) { // numeric widening, which keeps zero zero, or an object
      return codegen_.optionalLowerer().makeOptional(emitConversion(codegen_.optionalLowerer().optionalValue(value, from), from->inner(), to->inner()),
                          codegen_.optionalLowerer().optionalHas(value, from), to);
    }
    return codegen_.optionalLowerer().makeOptional(emitConversion(value, from, to->inner()), codegen_.builder_.getTrue(), to);
  }
  if (from->isOptional()) { // narrowed: known to hold a value
    return emitConversion(codegen_.optionalLowerer().optionalValue(value, from), from->inner(), to);
  }
  // An object seen through a type with read-only fields, or an iterator
  // through one that drops what it returns; an array of objects as one of a
  // type they are (`Dog[]` as `Animal[]`).
  if (isReference(from) || (from->isArray() && to->isArray())) {
    return value;
  }
  if (from->isInteger() && to->isDecimal()) {
    return codegen_.decimalLowerer().decimalFromInteger(value);
  }

  llvm::Type *target = codegen_.moduleBuilder().lower(to);
  if (from->isInteger() && to->isInteger()) {
    return from->bitWidth() < to->bitWidth() ? codegen_.builder_.CreateSExt(value, target)
                                             : codegen_.builder_.CreateTrunc(value, target);
  }
  if (from->isInteger() && to->isFloat()) {
    return codegen_.builder_.CreateSIToFP(value, target);
  }
  // An operand of a bitwise operator: JavaScript's ToInt32. A double becomes a
  // long only through Math functions (emitMathCall).
  if (from->isFloat() && to->isInt()) {
    return codegen_.builder_.CreateCall(codegen_.rt_.toInt32, {value});
  }
  llvm_unreachable("Sema rejects this conversion");
}

} // namespace tz
