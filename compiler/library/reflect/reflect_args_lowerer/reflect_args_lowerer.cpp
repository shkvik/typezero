#include "compiler/library/reflect/reflect_args_lowerer/reflect_args_lowerer.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/layouts/type_descriptors/type_descriptors.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/types/unknown_cast_lowerer/unknown_cast_lowerer.h"
#include "compiler/lower/types/unknown_lowerer/unknown_lowerer.h"
#include "compiler/lower/values/array_lowerer/array_lowerer.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/MDBuilder.h"


namespace tz {

bool ReflectArgsLowerer::narrower(const Type *a, const Type *b) {
  if (a == b || b->isUnknown()) {
    return true;
  }
  if (b->isOptional()) {
    return a->isUndefined() ||
           ReflectArgsLowerer::narrower(a->isOptional() ? a->inner() : a, b->inner());
  }
  if (a->isOptional()) {
    return false;
  }
  if (b->isUnion()) {
    return llvm::any_of(b->members(),
                        [&](const Type *m) { return ReflectArgsLowerer::narrower(a, m); });
  }
  if (a->isLiteral()) {
    return ReflectArgsLowerer::narrower(a->literalBase(), b);
  }
  if (a->isInteger()) {
    return (b->isInteger() && b != a && b->holds(a)) || b->isFloat() || b->isDecimal();
  }
  if (a->isObject() && b->isObject()) {
    return b->isAnyObject() || a->isSubtypeOf(b);
  }

  return false;
}

ReflectArgsLowerer::ReflectArgsLowerer(Lowering &owner)
    : lowering(owner) {}

llvm::Value *ReflectArgsLowerer::emitChooseVariant(llvm::ArrayRef<const FunctionDecl *> variants,
                                                   llvm::Value *selfId, llvm::Value *args,
                                                   llvm::Value *count) {
  llvm::Type *ptr = this->lowering.builder_.getPtrTy();
  llvm::Type *i32 = this->lowering.builder_.getInt32Ty();
  llvm::Value *missing = this->lowering.functionFrame().createStackSlot(ptr, "variant.missing");
  this->lowering.builder_.CreateStore(
      llvm::ConstantPointerNull::get(this->lowering.builder_.getPtrTy()), missing);
  auto given = [&](size_t i) {
    return this->lowering.builder_.CreateICmpULT(this->lowering.builder_.getInt32(uint32_t(i)),
                                                 count);
  };
  auto argAt = [&](size_t i) {
    llvm::Value *at =
        this->lowering.builder_.CreateGEP(ptr, args, this->lowering.builder_.getInt64(i));

    return this->lowering.builder_.CreateLoad(
        ptr, this->lowering.builder_.CreateSelect(given(i), at, missing), "arg");
  };
  struct Variant {
    const FuncSymbol *sym;
    size_t fixed;
    llvm::Value *ok;
    std::vector<llvm::Value *> fits;
    const Type *param(size_t i) const {
      if (i < fixed) {
        return sym->params[i];
      }

      return sym->hasRest ? sym->params.back()->inner() : nullptr;
    }
  };
  size_t compared = 0;
  for (const FunctionDecl *fn : variants) {
    compared = std::max(compared, fn->sym->params.size() - (fn->sym->hasRest ? 1 : 0));
  }
  std::vector<Variant> all;
  for (const FunctionDecl *fn : variants) {
    const FuncSymbol *sym = fn->sym;
    Variant v{sym, sym->hasRest ? sym->params.size() - 1 : sym->params.size(), nullptr, {}};
    llvm::Value *ok = this->lowering.builder_.CreateICmpUGE(
        count, this->lowering.builder_.getInt32(uint32_t(sym->minArgs)));
    if (!sym->hasRest) {
      ok = this->lowering.builder_.CreateAnd(
          ok, this->lowering.builder_.CreateICmpULE(
                  count, this->lowering.builder_.getInt32(uint32_t(v.fixed))));
    }
    if (selfId) {
      ok = this->lowering.builder_.CreateAnd(
          ok, this->lowering.unknownCastLowerer().classIdIn(selfId, {fn->cls()}));
    }
    for (size_t i = 0; i < compared; ++i) {
      const Type *param = v.param(i);
      if (!param) {
        v.fits.push_back(nullptr);
        continue;
      }
      llvm::Value *fit = this->lowering.builder_.CreateSelect(
          given(i), this->lowering.unknownCastLowerer().emitUnknownFit(argAt(i), param),
          this->lowering.builder_.getInt32(3));
      if (i < v.fixed) {
        ok = this->lowering.builder_.CreateAnd(
            ok, this->lowering.builder_.CreateICmpNE(fit, this->lowering.builder_.getInt32(0)));
      }
      v.fits.push_back(fit);
    }
    if (sym->hasRest) {
      const Type *element = sym->params.back()->inner();
      llvm::BasicBlock *entry = this->lowering.builder_.GetInsertBlock();
      llvm::BasicBlock *loopBlock = this->lowering.functionFrame().newBlock("variant.rest");
      llvm::BasicBlock *bodyBlock = this->lowering.functionFrame().newBlock("variant.rest.body");
      llvm::BasicBlock *doneBlock = this->lowering.functionFrame().newBlock("variant.rest.done");
      this->lowering.builder_.CreateBr(loopBlock);
      this->lowering.functionFrame().switchTo(loopBlock);
      llvm::PHINode *i = this->lowering.builder_.CreatePHI(i32, 2, "i");
      llvm::PHINode *fits =
          this->lowering.builder_.CreatePHI(this->lowering.builder_.getInt1Ty(), 2, "fits");
      i->addIncoming(this->lowering.builder_.getInt32(uint32_t(v.fixed)), entry);
      fits->addIncoming(this->lowering.builder_.getTrue(), entry);
      this->lowering.builder_.CreateCondBr(this->lowering.builder_.CreateICmpULT(i, count),
                                           bodyBlock, doneBlock);
      this->lowering.functionFrame().switchTo(bodyBlock);
      llvm::Value *at = this->lowering.builder_.CreateZExt(i, this->lowering.builder_.getInt64Ty());
      llvm::Value *u = this->lowering.builder_.CreateLoad(
          ptr, this->lowering.builder_.CreateInBoundsGEP(ptr, args, at));
      llvm::Value *fit = this->lowering.unknownCastLowerer().emitUnknownFit(u, element);
      i->addIncoming(this->lowering.builder_.CreateAdd(i, this->lowering.builder_.getInt32(1)),
                     this->lowering.builder_.GetInsertBlock());
      fits->addIncoming(
          this->lowering.builder_.CreateAnd(
              fits, this->lowering.builder_.CreateICmpNE(fit, this->lowering.builder_.getInt32(0))),
          this->lowering.builder_.GetInsertBlock());
      this->lowering.builder_.CreateBr(loopBlock);
      this->lowering.functionFrame().switchTo(doneBlock);
      ok = this->lowering.builder_.CreateAnd(ok, fits);
    }
    v.ok = ok;
    all.push_back(std::move(v));
  }
  auto better = [&](const Variant &a, const Variant &b) {
    llvm::Value *noWorse = this->lowering.builder_.getTrue(),
                *anyBetter = this->lowering.builder_.getFalse(),
                *anyWorse = this->lowering.builder_.getFalse();
    for (size_t i = 0; i < compared; ++i) {
      const Type *pa = a.param(i);
      const Type *pb = b.param(i);
      if (!pa || !pb) {
        continue;
      }
      bool toB = ReflectArgsLowerer::narrower(pa, pb);
      bool toA = ReflectArgsLowerer::narrower(pb, pa);
      llvm::Value *fa = a.fits[i];
      llvm::Value *fb = b.fits[i];
      llvm::Value *same = this->lowering.builder_.CreateICmpEQ(fa, fb);
      llvm::Value *higher = this->lowering.builder_.CreateICmpUGT(fa, fb),
                  *lower = this->lowering.builder_.CreateICmpULT(fa, fb);
      llvm::Value *ge = toB ? this->lowering.builder_.CreateOr(higher, same) : higher;
      llvm::Value *gt = toB && !toA ? this->lowering.builder_.CreateOr(higher, same) : higher;
      llvm::Value *lt = toA && !toB ? this->lowering.builder_.CreateOr(lower, same) : lower;
      llvm::Value *in = given(i);
      noWorse = this->lowering.builder_.CreateAnd(
          noWorse, this->lowering.builder_.CreateOr(this->lowering.builder_.CreateNot(in), ge));
      anyBetter =
          this->lowering.builder_.CreateOr(anyBetter, this->lowering.builder_.CreateAnd(in, gt));
      anyWorse =
          this->lowering.builder_.CreateOr(anyWorse, this->lowering.builder_.CreateAnd(in, lt));
    }
    llvm::Value *plainer = this->lowering.builder_.getInt1(!a.sym->hasRest && b.sym->hasRest);

    return this->lowering.builder_.CreateAnd(
        noWorse, this->lowering.builder_.CreateOr(
                     anyBetter, this->lowering.builder_.CreateAnd(
                                    this->lowering.builder_.CreateNot(anyWorse), plainer)));
  };
  llvm::Value *anyOk = this->lowering.builder_.getFalse();
  for (const Variant &v : all) {
    anyOk = this->lowering.builder_.CreateOr(anyOk, v.ok);
  }
  llvm::Value *chosen =
      this->lowering.builder_.CreateSelect(anyOk, this->lowering.builder_.getInt32(uint32_t(-2)),
                                           this->lowering.builder_.getInt32(uint32_t(-1)));
  for (size_t a = all.size(); a-- > 0;) {
    llvm::Value *best = all[a].ok;
    for (size_t b = 0; b < all.size(); ++b) {
      if (b != a) {
        best = this->lowering.builder_.CreateAnd(
            best, this->lowering.builder_.CreateOr(this->lowering.builder_.CreateNot(all[b].ok),
                                                   better(all[a], all[b])));
      }
    }
    chosen = this->lowering.builder_.CreateSelect(
        best, this->lowering.builder_.getInt32(uint32_t(a)), chosen, "variant");
  }

  return chosen;
}

void ReflectArgsLowerer::emitReflectMismatch(llvm::StringRef what, llvm::Value *index,
                                             const Type *expected, llvm::Value *got) {
  llvm::Type *ptr = this->lowering.builder_.getPtrTy();
  llvm::Type *i32 = this->lowering.builder_.getInt32Ty();
  llvm::StringRef name = expected->name();
  this->lowering.builder_.CreateCall(
      this->lowering.unknownLowerer().unknownFunction("tz_reflect_mismatch",
                                                      this->lowering.builder_.getVoidTy(),
                                                      {ptr, i32, i32, ptr, i32, ptr}),
      {this->lowering.stringLowerer().utf8Constant(what),
       this->lowering.builder_.getInt32(uint32_t(what.size())), index,
       this->lowering.stringLowerer().utf8Constant(name),
       this->lowering.builder_.getInt32(uint32_t(name.size())), got});
}

void ReflectArgsLowerer::emitReflectFail(llvm::StringRef code, const std::string &message) {
  llvm::Type *ptr = this->lowering.builder_.getPtrTy();
  llvm::Type *i32 = this->lowering.builder_.getInt32Ty();
  llvm::Value *text =
      this->lowering.stringLowerer().stringLiteral(std::u16string(message.begin(), message.end()));
  this->lowering.builder_.CreateCall(this->lowering.unknownLowerer().unknownFunction(
                                         "tz_reflect_fail_str", this->lowering.builder_.getVoidTy(),
                                         {ptr, i32, ptr, this->lowering.builder_.getInt64Ty()}),
                                     {this->lowering.stringLowerer().utf8Constant(code),
                                      this->lowering.builder_.getInt32(uint32_t(code.size())),
                                      this->lowering.stringLowerer().stringObj(text),
                                      this->lowering.stringLowerer().stringMeta(text)});
}

void ReflectArgsLowerer::emitReflectArgs(
    llvm::Value *args, llvm::Value *count, size_t min, llvm::ArrayRef<const Type *> params,
    const Type *rest, std::vector<llvm::Value *> &values,
    llvm::function_ref<void(llvm::Value *, const Type *, llvm::Value *)> onFail) {
  llvm::Type *ptr = this->lowering.builder_.getPtrTy();
  llvm::Type *i64 = this->lowering.builder_.getInt64Ty();
  size_t before = values.size();
  auto releaseMade = [&](size_t upTo) {
    for (size_t k = before; k < upTo; ++k) {
      this->lowering.refCounts().releaseValue(values[k], params[k - before]);
    }
  };
  auto argAt = [&](llvm::Value *i) {
    return this->lowering.builder_.CreateLoad(
        ptr, this->lowering.builder_.CreateInBoundsGEP(ptr, args, i), "arg");
  };
  for (size_t i = 0; i < params.size(); ++i) {
    llvm::Value *u;
    if (i < min) {
      u = argAt(this->lowering.builder_.getInt64(i));
    } else {
      llvm::BasicBlock *entry = this->lowering.builder_.GetInsertBlock();
      llvm::BasicBlock *givenBlock = this->lowering.functionFrame().newBlock("reflect.given");
      llvm::BasicBlock *endBlock = this->lowering.functionFrame().newBlock("reflect.arg");
      this->lowering.builder_.CreateCondBr(
          this->lowering.builder_.CreateICmpULT(this->lowering.builder_.getInt32(uint32_t(i)),
                                                count),
          givenBlock, endBlock);
      this->lowering.functionFrame().switchTo(givenBlock);
      llvm::Value *given = argAt(this->lowering.builder_.getInt64(i));
      llvm::BasicBlock *givenEnd = this->lowering.builder_.GetInsertBlock();
      this->lowering.builder_.CreateBr(endBlock);
      this->lowering.functionFrame().switchTo(endBlock);
      llvm::PHINode *phi = this->lowering.builder_.CreatePHI(ptr, 2);
      phi->addIncoming(llvm::ConstantPointerNull::get(this->lowering.builder_.getPtrTy()), entry);
      phi->addIncoming(given, givenEnd);
      u = phi;
    }
    llvm::BasicBlock *failBlock = this->lowering.functionFrame().newBlock("reflect.mismatch");
    values.push_back(this->lowering.unknownCastLowerer().emitUnknownAs(u, params[i], failBlock));
    llvm::IRBuilderBase::InsertPoint point = this->lowering.builder_.saveIP();
    this->lowering.functionFrame().switchTo(failBlock);
    releaseMade(before + i);
    onFail(this->lowering.builder_.getInt32(uint32_t(i)), params[i], u);
    this->lowering.builder_.restoreIP(point);
  }
  if (!rest) {
    return;
  }
  const Type *element = rest->inner();
  llvm::Value *from = this->lowering.builder_.getInt32(uint32_t(params.size()));
  llvm::Value *more = this->lowering.builder_.CreateSelect(
      this->lowering.builder_.CreateICmpUGT(count, from),
      this->lowering.builder_.CreateSub(count, from), this->lowering.builder_.getInt32(0));
  llvm::Value *array = this->lowering.builder_.CreateCall(
      this->lowering.rt_.arrNew,
      {this->lowering.typeDescriptors().typeDescriptor(element),
       this->lowering.builder_.getInt64(0), this->lowering.builder_.CreateZExt(more, i64)},
      "rest");
  llvm::BasicBlock *entry = this->lowering.builder_.GetInsertBlock();
  llvm::BasicBlock *loopBlock = this->lowering.functionFrame().newBlock("reflect.rest");
  llvm::BasicBlock *bodyBlock = this->lowering.functionFrame().newBlock("reflect.rest.body");
  llvm::BasicBlock *doneBlock = this->lowering.functionFrame().newBlock("reflect.rest.done");
  this->lowering.builder_.CreateBr(loopBlock);
  this->lowering.functionFrame().switchTo(loopBlock);
  llvm::PHINode *i =
      this->lowering.builder_.CreatePHI(this->lowering.builder_.getInt32Ty(), 2, "i");
  i->addIncoming(from, entry);
  this->lowering.builder_.CreateCondBr(this->lowering.builder_.CreateICmpULT(i, count), bodyBlock,
                                       doneBlock);
  this->lowering.functionFrame().switchTo(bodyBlock);
  llvm::Value *u = argAt(this->lowering.builder_.CreateZExt(i, i64));
  llvm::BasicBlock *failBlock = this->lowering.functionFrame().newBlock("reflect.mismatch");
  llvm::Value *value = this->lowering.unknownCastLowerer().emitUnknownAs(u, element, failBlock);
  this->lowering.arrayLowerer().emitPush(array, {value}, element);
  i->addIncoming(this->lowering.builder_.CreateAdd(i, this->lowering.builder_.getInt32(1)),
                 this->lowering.builder_.GetInsertBlock());
  this->lowering.builder_.CreateBr(loopBlock);
  llvm::IRBuilderBase::InsertPoint point = this->lowering.builder_.saveIP();
  this->lowering.functionFrame().switchTo(failBlock);
  this->lowering.refCounts().releaseValue(array, rest);
  releaseMade(before + params.size());
  onFail(i, element, u);
  this->lowering.builder_.restoreIP(point);
  this->lowering.functionFrame().switchTo(doneBlock);
  values.push_back(array);
}

}
