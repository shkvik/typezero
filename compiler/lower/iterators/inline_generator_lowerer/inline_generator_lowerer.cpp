#include "compiler/lower/iterators/inline_generator_lowerer/inline_generator_lowerer.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/function_builder/function_builder.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/lower/stmt/stmt_lowerer/stmt_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/iterators/async_generator_lowerer/async_generator_lowerer.h"
#include "compiler/lower/objects/dict_lowerer/dict_lowerer.h"
#include "compiler/lower/iterators/coroutine_lowerer/coroutine_lowerer.h"
#include "compiler/lower/iterators/lazy_chain_lowerer/lazy_chain_lowerer.h"
#include "compiler/lower/errors/throw_lowerer/throw_lowerer.h"
#include "compiler/library/regex/regex_lowerer/regex_lowerer.h"
#include "compiler/lower/iterators/iter_lower_helpers/iter_lower_helpers.h"
#include "compiler/check/sema/sema.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/Support/ErrorHandling.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::BasicBlock;
using llvm::Value;

namespace tz {

namespace {

// A generator with more `yield`s than this is not emitted inline: the
// consumer is copied to each of them.
constexpr unsigned kMaxInlineYields = 8;

// Nor one emitted inline in so many others, or where the code would be
// copied more times than this: generators emitted inline in the consumers of
// others multiply their copies.
constexpr unsigned kMaxInlineDepth = 4;

constexpr unsigned kMaxInlineCopies = 16;

unsigned countYields(const FunctionDecl *fn) {
  unsigned count = 0;
  visitExprs(
      fn->body, [&](const Expr *e) { count += isa<YieldExpr>(e) ? 1 : 0; },
      /*intoFunctions=*/false);

  return count;
}

} // namespace

InlineGeneratorLowerer::InlineGeneratorLowerer(CodeGen &codegen) : codegen_(codegen) {}

bool InlineGeneratorLowerer::isInlinable(const FunctionDecl *decl) const {
  if (!decl->isGenerator || decl->isClosure() || !decl->sym || !decl->sym->ret ||
      !decl->sym->ret->isIterator()) {
    return false;
  }
  // A `catch` or `finally` around a `yield` would take what the consumer's
  // code throws there, or run when it jumps out.
  if (hasTry(decl->body)) {
    return false;
  }
  // Not inside itself: a recursive generator would be emitted inside itself
  // without end.
  unsigned depth = 0;
  for (const CodeGen::InlineGenerator *frame = codegen_.inline_; frame; frame = frame->consumerContext.inlined) {
    if (frame->decl == decl) {
      return false;
    }
    ++depth;
  }
  unsigned yields = countYields(decl);

  return depth < kMaxInlineDepth && yields <= kMaxInlineYields &&
         inlineCopies_ * std::max(yields, 1u) <= kMaxInlineCopies;
}

CodeGen::Producer InlineGeneratorLowerer::prepare(Expr *source, llvm::Value *returnSlot) {
  const Type *type = source->type;
  auto hold = [&](Expr *expr) {
    const Type *t = expr->type;
    llvm::Value *slot =
        t->isManaged() ? codegen_.functionFrame().createManagedSlot(t, "operand") : codegen_.functionFrame().createStackSlot(codegen_.moduleBuilder().lower(t), "operand");
    codegen_.builder_.CreateStore(codegen_.exprLowerer().emitExpr(expr), slot);

    return slot;
  };
  if (auto *call = dyn_cast<CallExpr>(source)) {
    switch (call->builtin) {
    case Builtin::IterFrom: // of an iterator, the iterator itself
      return prepare(call->args[0], returnSlot);
    case Builtin::StrMatchAll: {
      // The string, then the regex, then the check of its g flag, as when the
      // iterator is made.
      llvm::Value *text = hold(cast<MemberExpr>(call->callee)->object);
      llvm::Value *regex = hold(call->args[0]);
      if (!call->regex) {
        codegen_.builder_.CreateCall(
            codegen_.regexLowerer().regexFunction("tz_re_check_match_all", codegen_.builder_.getVoidTy(), {codegen_.builder_.getPtrTy()}),
            {codegen_.builder_.CreateLoad(codegen_.builder_.getPtrTy(), regex, "regex")});
        codegen_.throwLowerer().emitThrowCheck(); // a TypeError without the g flag
      }
      llvm::Value *lastSlot = codegen_.functionFrame().createStackSlot(codegen_.builder_.getInt32Ty(), "lastIndex");
      codegen_.builder_.CreateStore(
          codegen_.builder_.CreateCall(
              codegen_.regexLowerer().regexFunction("tz_re_match_all_begin", codegen_.builder_.getInt32Ty(), {codegen_.builder_.getPtrTy()}),
              {codegen_.builder_.CreateLoad(codegen_.builder_.getPtrTy(), regex, "regex")}),
          lastSlot);
      return codegen_.regexLowerer().matchAllProducer(text, regex, lastSlot, codegen_.regexLowerer().matchShape(type->yielded(), call->regex));
    }
    case Builtin::ArrKeys:
    case Builtin::ArrValues:
    case Builtin::ArrEntries: {
      Expr *array = cast<MemberExpr>(call->callee)->object;
      return codegen_.lazyChainLowerer().arrayProducer(hold(array), array->type, call->builtin);
    }
    case Builtin::MapKeys:
    case Builtin::MapValues:
    case Builtin::MapEntries:
    case Builtin::SetValues:
    case Builtin::SetEntries: {
      // Its entries stay where they are while the loop runs.
      Expr *dict = cast<MemberExpr>(call->callee)->object;
      llvm::Value *slot = hold(dict);
      codegen_.dictLowerer().beginDictIteration(slot);
      return codegen_.dictLowerer().dictProducer(slot, dict->type, call->builtin);
    }
    case Builtin::IterMap:
    case Builtin::IterFilter:
    case Builtin::IterTake:
    case Builtin::IterDrop:
    case Builtin::IterFlatMap: {
      // The chain before it first, then its own argument, as JavaScript
      // evaluates them.
      Expr *receiver = cast<MemberExpr>(call->callee)->object;
      CodeGen::Producer from = prepare(receiver);
      llvm::Value *operand = hold(call->args[0]);
      Builtin stage = call->builtin;
      const Type *callback = call->args[0]->type->isFunction() ? call->args[0]->type : nullptr;
      const Type *element = receiver->type->yielded(), *result = call->type->yielded();
      CodeGen::Producer producer;
      producer.run = [this, from, stage, callback, element, result, operand](const CodeGen::Consumer &down) {
        from.run(codegen_.lazyChainLowerer().stageConsumer(stage, callback, element, result, operand, down));
      };
      return producer;
    }
    case Builtin::None:
      if (call->func && isInlinable(call->func->decl)) {
        // The generator's code goes where it is consumed. Its arguments are
        // evaluated and its parameters bound now, as when it is called.
        FunctionDecl *decl = call->func->decl;
        auto snapshot = std::make_shared<std::tuple<decltype(codegen_.addresses_), decltype(codegen_.boxSlots_),
                                                    decltype(codegen_.envBoxes_), decltype(codegen_.groupEnvs_)>>(
            codegen_.addresses_, codegen_.boxSlots_, codegen_.envBoxes_, codegen_.groupEnvs_);
        std::vector<llvm::Value *> args;
        for (Expr *arg : call->args) {
          args.push_back(codegen_.exprLowerer().emitExpr(arg));
        }
        for (size_t i = args.size(); i < call->func->params.size(); ++i) {
          args.push_back(codegen_.optionalLowerer().undefinedValue(call->func->params[i]));
        }
        codegen_.functionBuilder().emitParams(decl, args);
        CodeGen::Producer producer;
        producer.run = [this, decl, returnSlot, snapshot](const CodeGen::Consumer &consumer) {
          emitInlineGenerator(decl, consumer, returnSlot);
          // The same generator may be emitted inline again inside the
          // consumer: its variables are where they were for the rest.
          std::tie(codegen_.addresses_, codegen_.boxSlots_, codegen_.envBoxes_, codegen_.groupEnvs_) = *snapshot;
        };
        return producer;
      }
      break;
    default:
      break;
    }
  }
  assert((type->isArray() || type->isString() || type->isIterator() || type->isTypedArray()) &&
         "Sema checks what is iterated");
  llvm::Value *slot = hold(source);
  // An iterator stopped before its end is closed (iterCloseFunction) when
  // the iteration's scope ends, however it ends, even if it lives on in a
  // variable.
  if (type->isIterator()) {
    codegen_.scopes_.back().iterators.push_back(slot);
  }

  return codegen_.lazyChainLowerer().valueProducer(slot, type, returnSlot);
}

// The body of a generator in place, in a context of its own; at each `yield`
// the consumer runs in the context the iteration started in.
void InlineGeneratorLowerer::emitInlineGenerator(FunctionDecl *decl, const CodeGen::Consumer &consumer,
                                  llvm::Value *returnSlot) {
  CodeGen::InlineGenerator frame{decl,       consumer,      codegen_.coroutineLowerer().saveContext(), codegen_.functionFrame().newBlock("gen.done"),
                        returnSlot, codegen_.scopes_.size()};
  codegen_.currentFn_ = decl;
  codegen_.envParam_ = nullptr;
  codegen_.returnType_ = decl->sym->ret;
  codegen_.tailGroup_ = nullptr;
  codegen_.targets_.clear();
  codegen_.inline_ = &frame;
  unsigned copies = inlineCopies_;
  inlineCopies_ *= std::max(countYields(decl), 1u);
  codegen_.stmtLowerer().emitStmt(decl->body);
  inlineCopies_ = copies;
  codegen_.functionFrame().branchTo(frame.exit);
  codegen_.functionFrame().switchTo(frame.exit);
  codegen_.coroutineLowerer().restoreContext(frame.consumerContext);
}

llvm::Value *InlineGeneratorLowerer::emitYieldValue(llvm::Value *value, const Type *resultType) {
  if (!codegen_.inline_) {
    return codegen_.coroutineLowerer().emitCoroutineYield(value, resultType);
  }
  CodeGen::InlineGenerator *frame = codegen_.inline_;
  CodeGen::EmitContext generator = codegen_.coroutineLowerer().saveContext();
  codegen_.coroutineLowerer().restoreContext(frame->consumerContext);
  llvm::Value *sent = frame->consumer.each(value);
  const Type *sentType = frame->consumer.sent;
  codegen_.coroutineLowerer().restoreContext(generator);
  // What the consumer gave back, as the `yield` wants it.
  if (!resultType || resultType->isUndefined()) {
    if (sent) {
      codegen_.refCounts().releaseValue(sent, sentType);
    }
    return resultType ? codegen_.builder_.getFalse() : nullptr;
  }
  if (!sent) {
    return codegen_.optionalLowerer().undefinedValue(resultType);
  }

  return codegen_.exprLowerer().emitConversion(sent, sentType, resultType);
}

llvm::Value *InlineGeneratorLowerer::emitYield(YieldExpr *yield) {
  if (codegen_.async_ && codegen_.async_->generator) {
    return codegen_.asyncGeneratorLowerer().emitAsyncYield(yield);
  }
  const Type *generator = codegen_.currentFn_->sym->ret;
  const Type *yielded = generator->yielded();
  if (!yield->delegate) {
    llvm::Value *value = yield->value ? codegen_.exprLowerer().emitExpr(yield->value) : codegen_.optionalLowerer().undefinedValue(yielded);
    return emitYieldValue(value, yield->type);
  }
  // `yield* xs`: each value of xs, and then what xs returns.
  const Type *iterable = yield->value->type;
  const Type *innerSent =
      iterable->isIterator() && !iterable->sent()->isVoid() ? iterable->sent() : nullptr;
  codegen_.functionFrame().pushScope();
  llvm::Value *returnSlot = nullptr;
  if (!yield->type->isUndefined()) {
    returnSlot = codegen_.functionFrame().createManagedSlot(yield->type, "yield.result");
  }
  CodeGen::Producer producer = prepare(yield->value, returnSlot);
  CodeGen::Consumer consumer;
  consumer.sent = innerSent;
  const Type *element = yield->element;
  consumer.each = [this, element, yielded, innerSent](llvm::Value *value) {
    return emitYieldValue(codegen_.exprLowerer().emitConversion(value, element, yielded), innerSent);
  };
  // `yield* xs.take(2)` ends when take does.
  size_t depth = codegen_.scopes_.size();
  llvm::BasicBlock *endBlock = codegen_.functionFrame().newBlock("yield.all.end");
  consumer.stop = [this, depth, endBlock] {
    codegen_.functionFrame().emitCleanups(depth);
    codegen_.builder_.CreateBr(endBlock);
  };
  producer.run(consumer);
  codegen_.functionFrame().emitBlock(endBlock);
  llvm::Value *result = codegen_.builder_.getFalse(); // undefined
  if (returnSlot) {
    // Moved out of the slot, which the scope would release.
    result = codegen_.builder_.CreateLoad(codegen_.moduleBuilder().lower(yield->type), returnSlot);
    codegen_.builder_.CreateStore(llvm::Constant::getNullValue(codegen_.moduleBuilder().lower(yield->type)), returnSlot);
  }
  codegen_.functionFrame().popScope();

  return result;
}

} // namespace tz
