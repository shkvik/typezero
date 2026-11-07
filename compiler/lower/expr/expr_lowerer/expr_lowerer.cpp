#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/library/bytes/bytes_lowerer/bytes_lowerer.h"
#include "compiler/library/decimal/decimal_lowerer/decimal_lowerer.h"
#include "compiler/library/json/json_lowerer/json_lowerer.h"
#include "compiler/library/reflect/reflect_lowerer/reflect_lowerer.h"
#include "compiler/library/regex/matcher_builder/matcher_builder.h"
#include "compiler/library/regex/regex_lowerer/regex_lowerer.h"
#include "compiler/library/result/err_sites/err_sites.h"
#include "compiler/library/result/result_lowerer/result_lowerer.h"
#include "compiler/library/tz_modules/tz_modules_lowerer/tz_modules_lowerer.h"
#include "compiler/lower/async/async_lowerer/async_lowerer.h"
#include "compiler/lower/classes/class_lowerer/class_lowerer.h"
#include "compiler/lower/classes/decorator_lowerer/decorator_lowerer.h"
#include "compiler/lower/debug_info/debug_info.h"
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
#include "compiler/lower/objects/dict_lowerer/dict_lowerer.h"
#include "compiler/lower/objects/object_lowerer/object_lowerer.h"
#include "compiler/lower/stmt/stmt_lowerer/stmt_lowerer.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/lower/types/union_layout/union_layout.h"
#include "compiler/lower/types/union_lowerer/union_lowerer.h"
#include "compiler/lower/types/unknown_lowerer/unknown_lowerer.h"
#include "compiler/lower/values/array_lowerer/array_lowerer.h"
#include "compiler/lower/values/array_method_lowerer/array_method_lowerer.h"
#include "compiler/lower/values/number_lowerer/number_lowerer.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "compiler/pipeline/check_program/types/type_relations/type_relations.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/Support/ErrorHandling.h"

using llvm::cast;

namespace tz {

bool ExprLowerer::isReference(const Type *type) {
  return type->isObject() || type->isIterator() || type->isJson() || type->isDict() ||
         type->isPromise() || type->isUnknown() || type->isClassRef() ||
         (type->isUnion() && !type->isTagged() && type->slots().front() == UnionSlot::Object);
}

ExprLowerer::ExprLowerer(Lowering &owner)
    : lowering(owner) {}

llvm::Value *ExprLowerer::emitExpr(Expr *expr) {
  llvm::SMLoc outer = std::exchange(this->lowering.loc_, expr->loc());
  this->lowering.orderAt(expr->loc());
  llvm::DebugLoc outerDebug = this->lowering.debugInfo().here();
  this->lowering.debugInfo().at(expr->loc());
  llvm::Value *value = this->emitExprKind(expr);
  if (value && this->lowering.mismatch_.empty() && !expr->type->isVoid() &&
      !expr->type->isError() && !expr->type->isNever() &&
      this->lowering.builder_.GetInsertBlock()) {
    llvm::Type *want = this->lowering.moduleBuilder().lower(expr->type);
    if (value->getType() != want) {
      llvm::raw_string_ostream out(this->lowering.mismatch_);
      if (this->lowering.sourceMgr_) {
        auto [line, column] = this->lowering.sourceMgr_->getLineAndColumn(expr->loc());
        out << this->lowering.sourceMgr_
                   ->getMemoryBuffer(
                       this->lowering.sourceMgr_->FindBufferContainingLoc(expr->loc()))
                   ->getBufferIdentifier()
            << ":" << line << ":" << column << ": ";
      }
      out << "a value of type '" << expr->type->name() << "' is lowered as ";
      value->getType()->print(out);
      out << " instead of ";
      want->print(out);
      out << "\n";
    }
  }
  this->lowering.loc_ = outer;
  this->lowering.debugInfo().restore(outerDebug);

  return value;
}

llvm::Value *ExprLowerer::emitExprKind(Expr *expr) {
  switch (expr->kind()) {
  case Expr::Kind::IntLit: {
    auto *lit = cast<IntLitExpr>(expr);
    const Type *type = UnionLayout::storedType(lit->type);
    if (type->isFloat()) {
      return llvm::ConstantFP::get(this->lowering.builder_.getDoubleTy(),
                                   static_cast<double>(lit->value));
    }
    if (type->isDecimal()) {
      return this->lowering.decimalLowerer().emitDecimalLiteral(lit);
    }
    return llvm::ConstantInt::get(this->lowering.moduleBuilder().lower(type), lit->value);
  }
  case Expr::Kind::FloatLit:
    if (expr->type->isDecimal()) {
      return this->lowering.decimalLowerer().emitDecimalLiteral(expr);
    }
    return llvm::ConstantFP::get(this->lowering.builder_.getDoubleTy(),
                                 cast<FloatLitExpr>(expr)->value);
  case Expr::Kind::BoolLit:
    return this->lowering.builder_.getInt1(cast<BoolLitExpr>(expr)->value);
  case Expr::Kind::StringLit:
    if (expr->type->isStringLiteral()) {
      return this->lowering.builder_.getInt32(expr->type->atom());
    }
    return this->lowering.stringLowerer().stringLiteral(cast<StringLitExpr>(expr)->value);
  case Expr::Kind::RegexLit:
    return this->lowering.regexLowerer().emitRegexLiteral(cast<RegexLitExpr>(expr));
  case Expr::Kind::Undefined:
    return this->lowering.builder_.getFalse();
  case Expr::Kind::Template:
    return this->lowering.stringLowerer().emitTemplate(cast<TemplateExpr>(expr));
  case Expr::Kind::ArrayLit:
    return this->lowering.arrayLowerer().emitArrayLiteral(cast<ArrayLitExpr>(expr));
  case Expr::Kind::ObjectLit:
    return this->lowering.objectLowerer().emitObjectLiteral(cast<ObjectLitExpr>(expr));
  case Expr::Kind::Name: {
    auto *name = cast<NameExpr>(expr);
    if (name->func()) {
      return this->lowering.closureLowerer().makeFunctionValue(
          this->lowering.closureLowerer().functionInfo(name->func()->decl),
          llvm::ConstantPointerNull::get(this->lowering.builder_.getPtrTy()));
    }
    if (!name->var() && name->type->isJson()) {
      return this->lowering.jsonLowerer().emitJsonNull();
    }
    if (name->cls()) {
      return this->lowering.unknownLowerer().classObject(name->cls());
    }
    return this->lowering.envLowerer().emitVarValue(name->var(), false);
  }
  case Expr::Kind::Function:
    return this->lowering.closureLowerer().emitFunctionExpr(cast<FunctionExpr>(expr));
  case Expr::Kind::Member:
    return this->lowering.memberLowerer().emitMember(cast<MemberExpr>(expr));
  case Expr::Kind::Index: {
    auto *index = cast<IndexExpr>(expr);
    if (index->enumDecl()) {
      return this->lowering.unaryLowerer().emitEnumName(index);
    }
    if (index->object->type->nonOptional()->isJson()) {
      return this->lowering.jsonLowerer().emitJsonIndex(index);
    }
    if (index->object->type->isString()) {
      return this->lowering.stringLowerer().emitStringIndex(index);
    }
    if (index->keyed()) {
      Lowering::Operand object = this->lowering.refCounts().emitOperand(
          index->object, AssignLowerer::cannotAssign(index->index));
      llvm::Value *key = this->emitExpr(index->index);
      llvm::Value *value = this->lowering.builder_.CreateLoad(
          this->lowering.moduleBuilder().lower(index->type),
          this->lowering.objectLowerer().keyedFieldAddress(object.value, object.type, key,
                                                           index->index->type),
          "field");
      this->lowering.refCounts().retainValue(value, index->type);
      this->lowering.refCounts().dropOperand(object);
      return value;
    }
    if (index->object->type->isTypedArray()) {
      return this->lowering.bytesLowerer().emitBytesIndex(index);
    }
    return this->lowering.arrayLowerer().emitArrayIndex(index);
  }
  case Expr::Kind::Unary:
    return this->lowering.unaryLowerer().emitUnary(cast<UnaryExpr>(expr));
  case Expr::Kind::Binary:
    return this->lowering.binaryLowerer().emitBinary(cast<BinaryExpr>(expr));
  case Expr::Kind::Assign:
    return this->lowering.assignLowerer().emitAssign(cast<AssignExpr>(expr), true);
  case Expr::Kind::Update:
    return this->lowering.assignLowerer().emitUpdate(cast<UpdateExpr>(expr));
  case Expr::Kind::Call: {
    auto *call = cast<CallExpr>(expr);
    llvm::Value *value = this->lowering.callLowerer().emitCall(call);
    const FuncSymbol *callee = call->func() ? call->func() : call->method();
    if (value && call->type && call->type->isResult() && callee && callee->decl &&
        this->lowering.errSites().inLibrary(callee->decl->loc()) &&
        !this->lowering.errSites().inLibrary(call->loc())) {
      return this->lowering.errSites().stamp(value, call->type, call->loc());
    }
    return value;
  }
  case Expr::Kind::New: {
    auto *create = cast<NewExpr>(expr);
    if (create->reflect()) {
      return this->lowering.reflectLowerer().emitReflectNew(create);
    }
    if (create->cls()) {
      return this->lowering.classLowerer().emitNew(create);
    }
    return this->lowering.arrayLowerer().emitNewArray(create);
  }
  case Expr::Kind::Class:
    return this->lowering.decoratorLowerer().emitClassExpr(cast<ClassExpr>(expr));
  case Expr::Kind::Conditional:
    return this->emitConditional(cast<ConditionalExpr>(expr));
  case Expr::Kind::Cast: {
    auto *c = cast<CastExpr>(expr);
    if (c->shortCircuit) {
      return this->emitChainLink(c);
    }
    if (c->adaptsResult) {
      return this->lowering.tzModulesLowerer().emitResultAdapter(c);
    }
    if (c->type->isFunction() && c->operand->type->isFunction()) {
      return this->lowering.closureLowerer().emitFunctionAdapter(c);
    }
    if (c->text) {
      llvm::Value *value = this->emitExpr(c->operand);
      llvm::Value *text =
          this->lowering.arrayMethodLowerer().emitValueToString(value, c->operand->type);
      this->lowering.refCounts().releaseValue(value, c->operand->type);
      return text;
    }
    if (c->type->isBool() && !c->operand->type->isBool() &&
        !c->operand->type->nonOptional()->isJson() && !c->operand->type->isUnknown()) {
      return this->lowering.truthLowerer().emitTruthOf(c->operand);
    }
    return this->emitConversion(this->emitExpr(c->operand), c->operand->type, c->type);
  }
  case Expr::Kind::Sequence: {
    auto *seq = cast<SequenceExpr>(expr);
    for (size_t i = 0; i + 1 < seq->exprs.size(); ++i) {
      this->lowering.stmtLowerer().emitDiscarded(seq->exprs[i]);
    }
    return this->emitExpr(seq->exprs.back());
  }
  case Expr::Kind::OptionalChain:
    return this->emitOptionalChain(cast<OptionalChainExpr>(expr));
  case Expr::Kind::Yield:
    return this->lowering.inlineGeneratorLowerer().emitYield(cast<YieldExpr>(expr));
  case Expr::Kind::Await: {
    llvm::Value *value = this->lowering.asyncLowerer().emitAwait(cast<AwaitExpr>(expr));
    if (value && expr->type && expr->type->isResult() &&
        !this->lowering.errSites().inLibrary(expr->loc())) {
      return this->lowering.errSites().stamp(value, expr->type, expr->loc());
    }
    return value;
  }
  }
  llvm_unreachable("unknown expression kind");
}

llvm::Value *ExprLowerer::emitOptionalChain(OptionalChainExpr *chain) {
  if (!chain->mayShortCircuit) {
    return this->emitExpr(chain->chain);
  }
  llvm::BasicBlock *undefinedBlock = this->lowering.functionFrame().newBlock("chain.undefined");
  llvm::BasicBlock *endBlock = this->lowering.functionFrame().newBlock("chain.end");
  this->lowering.chainEnds_.push_back(undefinedBlock);
  llvm::Value *value = this->emitExpr(chain->chain);
  this->lowering.chainEnds_.pop_back();
  const Type *type = chain->chain->type;
  if (!type->isVoid() && !type->isOptional()) {
    value = this->lowering.optionalLowerer().makeOptional(value, this->lowering.builder_.getTrue(),
                                                          chain->type);
  }
  llvm::BasicBlock *valueEnd = this->lowering.builder_.GetInsertBlock();
  this->lowering.builder_.CreateBr(endBlock);
  this->lowering.functionFrame().switchTo(undefinedBlock);
  this->lowering.builder_.CreateBr(endBlock);
  this->lowering.functionFrame().switchTo(endBlock);
  if (type->isVoid()) {
    return nullptr;
  }
  llvm::PHINode *phi =
      this->lowering.builder_.CreatePHI(this->lowering.moduleBuilder().lower(chain->type), 2);
  phi->addIncoming(value, valueEnd);
  phi->addIncoming(this->lowering.optionalLowerer().undefinedValue(chain->type), undefinedBlock);

  return phi;
}

llvm::Value *ExprLowerer::emitChainLink(CastExpr *link) {
  const Type *type = link->operand->type;
  if (type->nonOptional()->isJson()) {
    return this->lowering.jsonLowerer().emitJsonChainLink(link);
  }
  llvm::Value *value = this->emitExpr(link->operand);
  llvm::BasicBlock *valueBlock = this->lowering.functionFrame().newBlock("chain.link");
  this->lowering.builder_.CreateCondBr(this->lowering.optionalLowerer().optionalHas(value, type),
                                       valueBlock, this->lowering.chainEnds_.back());
  this->lowering.functionFrame().switchTo(valueBlock);

  return this->lowering.optionalLowerer().optionalValue(value, type);
}

llvm::Value *ExprLowerer::emitConditional(ConditionalExpr *cond) {
  llvm::Value *test = this->emitExpr(cond->cond);
  llvm::BasicBlock *thenBlock = this->lowering.functionFrame().newBlock("cond.then");
  llvm::BasicBlock *elseBlock = this->lowering.functionFrame().newBlock("cond.else");
  llvm::BasicBlock *endBlock = this->lowering.functionFrame().newBlock("cond.end");
  this->lowering.builder_.CreateCondBr(test, thenBlock, elseBlock);

  this->lowering.functionFrame().switchTo(thenBlock);
  llvm::Value *thenValue = this->emitExpr(cond->thenExpr);
  llvm::BasicBlock *thenEnd = this->lowering.builder_.GetInsertBlock();
  this->lowering.builder_.CreateBr(endBlock);

  this->lowering.functionFrame().switchTo(elseBlock);
  llvm::Value *elseValue = this->emitExpr(cond->elseExpr);
  llvm::BasicBlock *elseEnd = this->lowering.builder_.GetInsertBlock();
  this->lowering.builder_.CreateBr(endBlock);

  this->lowering.functionFrame().switchTo(endBlock);
  if (cond->type->isVoid()) {
    return nullptr;
  }
  llvm::PHINode *phi =
      this->lowering.builder_.CreatePHI(this->lowering.moduleBuilder().lower(cond->type), 2);
  phi->addIncoming(thenValue, thenEnd);
  phi->addIncoming(elseValue, elseEnd);

  return phi;
}

std::vector<llvm::Value *> ExprLowerer::emitExprs(llvm::ArrayRef<Expr *> exprs) {
  std::vector<llvm::Value *> values;
  for (Expr *expr : exprs) {
    values.push_back(this->emitExpr(expr));
  }

  return values;
}

llvm::Value *ExprLowerer::emitConversion(llvm::Value *value, const Type *from, const Type *to) {
  if (from == to) {
    return value;
  }
  if (from->isString() && to->isString()) {
    return value;
  }
  if (from->isNever()) {
    return llvm::Constant::getNullValue(this->lowering.resultLowerer().lowerHalf(to));
  }
  if (from->isUnknown() || to->isUnknown()) {
    return this->lowering.unknownLowerer().emitUnknownConversion(value, from, to);
  }
  if (to->isNever()) {
    this->lowering.refCounts().releaseValue(value, from);
    return llvm::Constant::getNullValue(this->lowering.moduleBuilder().lower(to));
  }
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
    return this->lowering.unionLowerer().emitUnionConversion(value, from, to);
  }
  if ((from->nonOptional()->isJson() || to->nonOptional()->isJson()) &&
      from->nonOptional() != to->nonOptional()) {
    return this->lowering.jsonLowerer().emitJsonConversion(value, from, to);
  }
  if (TypeRelations::copiesIntoRecord(from, to)) {
    return this->lowering.dictLowerer().emitRecordFromObject(value, from, to);
  }
  if (from->isResult() && to->isResult()) {
    return this->lowering.resultLowerer().makeResult(
        to, this->lowering.resultLowerer().resultIsOk(value, from),
        this->lowering.resultLowerer().convertHalf(
            this->lowering.resultLowerer().resultHalf(value, from, false), from->okType(),
            to->okType()),
        this->lowering.resultLowerer().convertHalf(
            this->lowering.resultLowerer().resultHalf(value, from, true), from->errType(),
            to->errType()),
        this->lowering.resultLowerer().resultSite(value, from));
  }

  if (to->isString()) {
    if (from->isBool()) {
      return this->lowering.builder_.CreateSelect(
          value, this->lowering.stringLowerer().stringLiteral(u"true"),
          this->lowering.stringLowerer().stringLiteral(u"false"));
    }
    if (from->isInteger()) {
      return this->lowering.stringLowerer().callReturningString(
          from->isUnsigned() && from->bitWidth() == 64 ? this->lowering.rt_.strFromU64
                                                       : this->lowering.rt_.strFromI64,
          {this->lowering.numberLowerer().toI64(value, from)});
    }
    if (from->isFloat()) {
      return this->lowering.stringLowerer().callReturningString(this->lowering.rt_.strFromF64,
                                                                {value});
    }
    if (from->isUndefined()) {
      return this->lowering.stringLowerer().stringLiteral(u"undefined");
    }
    if (from->isDecimal()) {
      llvm::Type *i64 = this->lowering.builder_.getInt64Ty();
      llvm::Type *ptr = this->lowering.builder_.getPtrTy();
      llvm::Value *text = this->lowering.stringLowerer().callReturningString(
          this->lowering.decimalLowerer().decimalFunction(
              "tz_dec_to_string", this->lowering.builder_.getVoidTy(), {ptr, i64, i64}),
          {this->lowering.decimalLowerer().decimalCoef(value),
           this->lowering.decimalLowerer().decimalMeta(value)});
      this->lowering.refCounts().releaseValue(value, from);
      return text;
    }
    llvm::Value *result = this->lowering.arrayMethodLowerer().emitValueToString(value, from);
    this->lowering.refCounts().releaseValue(value, from);
    return result;
  }

  if (to->isOptional()) {
    if (from->isUndefined()) {
      return this->lowering.optionalLowerer().undefinedValue(to);
    }
    if (from->isOptional()) {
      return this->lowering.optionalLowerer().makeOptional(
          this->emitConversion(this->lowering.optionalLowerer().optionalValue(value, from),
                               from->inner(), to->inner()),
          this->lowering.optionalLowerer().optionalHas(value, from), to);
    }
    return this->lowering.optionalLowerer().makeOptional(
        this->emitConversion(value, from, to->inner()), this->lowering.builder_.getTrue(), to);
  }
  if (from->isOptional()) {
    return this->emitConversion(this->lowering.optionalLowerer().optionalValue(value, from),
                                from->inner(), to);
  }
  if (ExprLowerer::isReference(from) || (from->isArray() && to->isArray())) {
    return value;
  }
  if (from->isInteger() && to->isDecimal()) {
    return this->lowering.decimalLowerer().decimalFromInteger(value, from);
  }

  if (from->isInteger() && to->isInteger()) {
    return this->lowering.numberLowerer().intCast(value, from, to);
  }
  if (from->isInteger() && to->isFloat()) {
    return this->lowering.numberLowerer().toDouble(value, from);
  }
  if (from->isFloat() && to->isInteger()) {
    return this->lowering.numberLowerer().intCast(
        this->lowering.builder_.CreateCall(this->lowering.rt_.toInt32, {value}),
        Type::integer(32, false), to);
  }
  llvm_unreachable("Checker rejects this conversion");
}

}
