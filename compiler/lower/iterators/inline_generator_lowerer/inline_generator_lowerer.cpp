#include "compiler/lower/iterators/inline_generator_lowerer/inline_generator_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/function_builder/function_builder.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/iterators/async_generator_lowerer/async_generator_lowerer.h"
#include "compiler/lower/iterators/coroutine_lowerer/coroutine_lowerer.h"
#include "compiler/lower/iterators/iter_lower_helpers/iter_lower_helpers.h"
#include "compiler/lower/iterators/lazy_chain_lowerer/lazy_chain_lowerer.h"
#include "compiler/lower/library/regex/regex_lowerer/regex_lowerer.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/objects/dict_lowerer/dict_lowerer.h"
#include "compiler/lower/stmt/stmt_lowerer/stmt_lowerer.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/program/ast/ast.h"
#include "compiler/program/ast_walker/ast_walker.h"
#include "compiler/program/types/types.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/MDBuilder.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

namespace {

constexpr unsigned kMaxInlineYields = 8;

constexpr unsigned kMaxInlineDepth = 4;

constexpr unsigned kMaxInlineCopies = 16;

}

InlineGeneratorLowerer::InlineGeneratorLowerer(Lowering &owner)
    : lowering(owner) {}

unsigned InlineGeneratorLowerer::countYields(const FunctionDecl *fn) {
  unsigned count = 0;
  AstWalker::visitExprs(
      fn->body, [&](const Expr *e) { count += isa<YieldExpr>(e) ? 1 : 0; }, false);

  return count;
}

bool InlineGeneratorLowerer::isInlinable(const FunctionDecl *decl) const {
  if (!decl->isGenerator || decl->isClosure() || !decl->sym || !decl->sym->hasReturnType() ||
      !decl->sym->getKnownReturnType()->isIterator()) {
    return false;
  }
  unsigned depth = 0;
  for (const Lowering::InlineGenerator *frame = this->lowering.inline_; frame;
       frame = frame->consumerContext.inlined) {
    if (frame->decl == decl) {
      return false;
    }
    ++depth;
  }
  unsigned yields = InlineGeneratorLowerer::countYields(decl);

  return depth < kMaxInlineDepth && yields <= kMaxInlineYields &&
         this->inlineCopies * std::max(yields, 1u) <= kMaxInlineCopies;
}

Lowering::Producer InlineGeneratorLowerer::prepare(Expr *source, llvm::Value *returnSlot) {
  const Type *type = source->type;
  auto hold = [&](Expr *expr) {
    const Type *t = expr->type;
    llvm::Value *slot = t->isManaged()
                            ? this->lowering.functionFrame().createManagedSlot(t, "operand")
                            : this->lowering.functionFrame().createStackSlot(
                                  this->lowering.moduleBuilder().lower(t), "operand");
    this->lowering.builder.CreateStore(this->lowering.exprLowerer().emitExpr(expr), slot);

    return slot;
  };
  auto *call = dyn_cast<CallExpr>(source);
  if (call) {
    switch (call->builtin()) {
      case Builtin::IterFrom:
        return this->prepare(call->args[0], returnSlot);
      case Builtin::StrMatchAll: {
        llvm::Value *text = hold(cast<MemberExpr>(call->callee)->object);
        llvm::Value *regex = hold(call->args[0]);
        if (!call->regex) {
          this->lowering.builder.CreateCall(
              this->lowering.regexLowerer().regexFunction("tz_re_check_match_all",
                                                          this->lowering.builder.getVoidTy(),
                                                          {this->lowering.builder.getPtrTy()}),
              {this->lowering.builder.CreateLoad(this->lowering.builder.getPtrTy(), regex,
                                                 "regex")});
        }
        llvm::Value *lastSlot = this->lowering.functionFrame().createStackSlot(
            this->lowering.builder.getInt32Ty(), "lastIndex");
        this->lowering.builder.CreateStore(
            this->lowering.builder.CreateCall(
                this->lowering.regexLowerer().regexFunction("tz_re_match_all_begin",
                                                            this->lowering.builder.getInt32Ty(),
                                                            {this->lowering.builder.getPtrTy()}),
                {this->lowering.builder.CreateLoad(this->lowering.builder.getPtrTy(), regex,
                                                   "regex")}),
            lastSlot);
        return this->lowering.regexLowerer().matchAllProducer(
            text, regex, lastSlot,
            this->lowering.regexLowerer().matchShape(type->yielded(), call->regex));
      }
      case Builtin::ArrKeys:
      case Builtin::ArrValues:
      case Builtin::ArrEntries: {
        Expr *array = cast<MemberExpr>(call->callee)->object;
        return this->lowering.lazyChainLowerer().arrayProducer(hold(array), array->type,
                                                               call->builtin());
      }
      case Builtin::MapKeys:
      case Builtin::MapValues:
      case Builtin::MapEntries:
      case Builtin::SetValues:
      case Builtin::SetEntries: {
        Expr *dict = cast<MemberExpr>(call->callee)->object;
        llvm::Value *slot = hold(dict);
        this->lowering.dictLowerer().beginDictIteration(slot);
        return this->lowering.dictLowerer().dictProducer(slot, dict->type, call->builtin());
      }
      case Builtin::IterMap:
      case Builtin::IterFilter:
      case Builtin::IterTake:
      case Builtin::IterDrop:
      case Builtin::IterFlatMap: {
        Expr *receiver = cast<MemberExpr>(call->callee)->object;
        Lowering::Producer from = this->prepare(receiver);
        llvm::Value *operand = hold(call->args[0]);
        Builtin stage = call->builtin();
        const Type *callback = call->args[0]->type->isFunction() ? call->args[0]->type : nullptr;
        const Type *element = receiver->type->yielded();
        const Type *result = call->type->yielded();
        Lowering::Producer producer;
        producer.run = [this, from, stage, callback, element, result,
                        operand](const Lowering::Consumer &down) {
          from.run(this->lowering.lazyChainLowerer().stageConsumer(stage, callback, element, result,
                                                                   operand, down));
        };
        return producer;
      }
      case Builtin::None:
        if (call->func() && this->isInlinable(call->func()->decl)) {
          FunctionDecl *decl = call->func()->decl;
          auto snapshot = std::make_shared<
              std::tuple<decltype(this->lowering.addresses), decltype(this->lowering.boxSlots),
                         decltype(this->lowering.envBoxes), decltype(this->lowering.groupEnvs)>>(
              this->lowering.addresses, this->lowering.boxSlots, this->lowering.envBoxes,
              this->lowering.groupEnvs);
          std::vector<llvm::Value *> args;
          for (Expr *arg : call->args) {
            args.push_back(this->lowering.exprLowerer().emitExpr(arg));
          }
          for (size_t i = args.size(); i < call->func()->params.size(); ++i) {
            args.push_back(
                this->lowering.optionalLowerer().undefinedValue(call->func()->params[i]));
          }
          this->lowering.functionBuilder().emitParams(decl, args);
          Lowering::Producer producer;
          producer.run = [this, decl, returnSlot, snapshot](const Lowering::Consumer &consumer) {
            this->emitInlineGenerator(decl, consumer, returnSlot);
            std::tie(this->lowering.addresses, this->lowering.boxSlots, this->lowering.envBoxes,
                     this->lowering.groupEnvs) = *snapshot;
          };
          return producer;
        }
        break;
      default:
        break;
    }
  }
  assert((type->isArray() || type->isString() || type->isIterator() || type->isTypedArray()) &&
         "Checker checks what is iterated");
  llvm::Value *slot = hold(source);

  return this->lowering.lazyChainLowerer().valueProducer(slot, type, returnSlot);
}

void InlineGeneratorLowerer::emitInlineGenerator(FunctionDecl *decl,
                                                 const Lowering::Consumer &consumer,
                                                 llvm::Value *returnSlot) {
  Lowering::InlineGenerator frame{decl,
                                  consumer,
                                  this->lowering.coroutineLowerer().saveContext(),
                                  this->lowering.functionFrame().newBlock("gen.done"),
                                  returnSlot,
                                  this->lowering.scopes.size()};
  this->lowering.currentFn = decl;
  this->lowering.envParam = nullptr;
  this->lowering.returnType = decl->sym->getKnownReturnType();
  this->lowering.tailGroup = nullptr;
  this->lowering.targets.clear();
  this->lowering.inline_ = &frame;
  unsigned copies = this->inlineCopies;
  this->inlineCopies *= std::max(InlineGeneratorLowerer::countYields(decl), 1u);
  this->lowering.stmtLowerer().emitStmt(decl->body);
  this->inlineCopies = copies;
  this->lowering.functionFrame().branchTo(frame.exit);
  this->lowering.functionFrame().switchTo(frame.exit);
  this->lowering.coroutineLowerer().restoreContext(frame.consumerContext);
}

llvm::Value *InlineGeneratorLowerer::emitYieldValue(llvm::Value *value, const Type *resultType) {
  if (!this->lowering.inline_) {
    return this->lowering.coroutineLowerer().emitCoroutineYield(value, resultType);
  }
  Lowering::InlineGenerator *frame = this->lowering.inline_;
  Lowering::EmitContext generator = this->lowering.coroutineLowerer().saveContext();
  this->lowering.coroutineLowerer().restoreContext(frame->consumerContext);
  llvm::Value *sent = frame->consumer.each(value);
  const Type *sentType = frame->consumer.sent;
  this->lowering.coroutineLowerer().restoreContext(generator);
  if (!resultType || resultType->isUndefined()) {
    if (sent) {
      this->lowering.refCounts().releaseValue(sent, sentType);
    }
    return resultType ? this->lowering.builder.getFalse() : nullptr;
  }
  if (!sent) {
    return this->lowering.optionalLowerer().undefinedValue(resultType);
  }

  return this->lowering.exprLowerer().emitConversion(sent, sentType, resultType);
}

llvm::Value *InlineGeneratorLowerer::emitYield(YieldExpr *yield) {
  if (this->lowering.async && this->lowering.async->generator) {
    return this->lowering.asyncGeneratorLowerer().emitAsyncYield(yield);
  }
  const Type *generator = this->lowering.currentFn->sym->getKnownReturnType();
  const Type *yielded = generator->yielded();
  if (!yield->delegate) {
    llvm::Value *value = yield->value ? this->lowering.exprLowerer().emitExpr(yield->value)
                                      : this->lowering.optionalLowerer().undefinedValue(yielded);
    return this->emitYieldValue(value, yield->type);
  }
  const Type *iterable = yield->value->type;
  const Type *innerSent =
      iterable->isIterator() && !iterable->sent()->isVoid() ? iterable->sent() : nullptr;
  this->lowering.functionFrame().pushScope();
  llvm::Value *returnSlot = nullptr;
  if (!yield->type->isUndefined()) {
    returnSlot = this->lowering.functionFrame().createManagedSlot(yield->type, "yield.result");
  }
  Lowering::Producer producer = this->prepare(yield->value, returnSlot);
  Lowering::Consumer consumer;
  consumer.sent = innerSent;
  const Type *element = yield->element;
  consumer.each = [this, element, yielded, innerSent](llvm::Value *value) {
    return this->emitYieldValue(
        this->lowering.exprLowerer().emitConversion(value, element, yielded), innerSent);
  };
  size_t depth = this->lowering.scopes.size();
  llvm::BasicBlock *endBlock = this->lowering.functionFrame().newBlock("yield.all.end");
  consumer.stop = [this, depth, endBlock] {
    this->lowering.functionFrame().emitCleanups(depth);
    this->lowering.builder.CreateBr(endBlock);
  };
  producer.run(consumer);
  this->lowering.functionFrame().emitBlock(endBlock);
  llvm::Value *result = this->lowering.builder.getFalse();
  if (returnSlot) {
    result = this->lowering.builder.CreateLoad(this->lowering.moduleBuilder().lower(yield->type),
                                               returnSlot);
    this->lowering.builder.CreateStore(
        llvm::Constant::getNullValue(this->lowering.moduleBuilder().lower(yield->type)),
        returnSlot);
  }
  this->lowering.functionFrame().popScope();

  return result;
}

}
