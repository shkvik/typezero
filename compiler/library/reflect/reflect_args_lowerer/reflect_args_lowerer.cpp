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

using llvm::BasicBlock;
using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::Value;

namespace tz {

namespace {

// Whether a value of type `a` goes where one of `b` is without an explicit
// conversion, as a parameter of `a` is narrower than one of `b`: the rule a
// call that compiles prefers a variant by (docs/overloads.md, section 2).
bool narrower(const Type *a, const Type *b) {
  if (a == b || b->isUnknown()) {
    return true;
  }
  if (b->isOptional()) {
    return a->isUndefined() || narrower(a->isOptional() ? a->inner() : a, b->inner());
  }
  if (a->isOptional()) {
    return false;
  }
  if (b->isUnion()) {
    return llvm::any_of(b->members(), [&](const Type *m) { return narrower(a, m); });
  }
  if (a->isLiteral()) {
    return narrower(a->literalBase(), b);
  }
  if (a->isInt()) {
    return b->kind() == Type::Kind::Long || b->isFloat() || b->isDecimal();
  }
  if (a->kind() == Type::Kind::Long) {
    return b->isFloat() || b->isDecimal();
  }
  if (a->isObject() && b->isObject()) {
    return b->isAnyObject() || a->isSubtypeOf(b);
  }

  return false;
}

} // namespace

ReflectArgsLowerer::ReflectArgsLowerer(Lowering &lowering) : lowering_(lowering) {}

llvm::Value *ReflectArgsLowerer::emitChooseVariant(llvm::ArrayRef<const FunctionDecl *> variants,
                                                   llvm::Value *selfId, llvm::Value *args,
                                                   llvm::Value *count) {
  llvm::Type *ptr = lowering_.builder_.getPtrTy(), *i32 = lowering_.builder_.getInt32Ty();
  // args[i], or undefined past the end, read from a slot that holds it then.
  llvm::Value *missing = lowering_.functionFrame().createStackSlot(ptr, "variant.missing");
  lowering_.builder_.CreateStore(llvm::ConstantPointerNull::get(lowering_.builder_.getPtrTy()),
                                 missing);
  auto given = [&](size_t i) {
    return lowering_.builder_.CreateICmpULT(lowering_.builder_.getInt32(uint32_t(i)), count);
  };
  auto argAt = [&](size_t i) {
    llvm::Value *at = lowering_.builder_.CreateGEP(ptr, args, lowering_.builder_.getInt64(i));

    return lowering_.builder_.CreateLoad(
        ptr, lowering_.builder_.CreateSelect(given(i), at, missing), "arg");
  };
  struct Variant {
    const FuncSymbol *sym;
    size_t fixed;
    llvm::Value *ok;
    // How the arguments fit, up to the most parameters a variant has before
    // a rest: of a rest parameter, its element.
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
    llvm::Value *ok = lowering_.builder_.CreateICmpUGE(
        count, lowering_.builder_.getInt32(uint32_t(sym->minArgs)));
    if (!sym->hasRest) {
      ok = lowering_.builder_.CreateAnd(
          ok,
          lowering_.builder_.CreateICmpULE(count, lowering_.builder_.getInt32(uint32_t(v.fixed))));
    }
    if (selfId) {
      ok = lowering_.builder_.CreateAnd(
          ok, lowering_.unknownCastLowerer().classIdIn(selfId, {fn->cls}));
    }
    for (size_t i = 0; i < compared; ++i) {
      const Type *param = v.param(i);
      if (!param) {
        v.fits.push_back(nullptr);
        continue;
      }
      // Left out, a parameter that may be: no worse than given.
      llvm::Value *fit = lowering_.builder_.CreateSelect(
          given(i), lowering_.unknownCastLowerer().emitUnknownFit(argAt(i), param),
          lowering_.builder_.getInt32(3));
      if (i < v.fixed) {
        ok = lowering_.builder_.CreateAnd(
            ok, lowering_.builder_.CreateICmpNE(fit, lowering_.builder_.getInt32(0)));
      }
      v.fits.push_back(fit);
    }
    if (sym->hasRest) {
      // Each of the rest fits the element.
      const Type *element = sym->params.back()->inner();
      llvm::BasicBlock *entry = lowering_.builder_.GetInsertBlock();
      llvm::BasicBlock *loopBlock = lowering_.functionFrame().newBlock("variant.rest");
      llvm::BasicBlock *bodyBlock = lowering_.functionFrame().newBlock("variant.rest.body");
      llvm::BasicBlock *doneBlock = lowering_.functionFrame().newBlock("variant.rest.done");
      lowering_.builder_.CreateBr(loopBlock);
      lowering_.functionFrame().switchTo(loopBlock);
      llvm::PHINode *i = lowering_.builder_.CreatePHI(i32, 2, "i");
      llvm::PHINode *fits = lowering_.builder_.CreatePHI(lowering_.builder_.getInt1Ty(), 2, "fits");
      i->addIncoming(lowering_.builder_.getInt32(uint32_t(v.fixed)), entry);
      fits->addIncoming(lowering_.builder_.getTrue(), entry);
      lowering_.builder_.CreateCondBr(lowering_.builder_.CreateICmpULT(i, count), bodyBlock,
                                      doneBlock);
      lowering_.functionFrame().switchTo(bodyBlock);
      llvm::Value *at = lowering_.builder_.CreateZExt(i, lowering_.builder_.getInt64Ty());
      llvm::Value *u =
          lowering_.builder_.CreateLoad(ptr, lowering_.builder_.CreateInBoundsGEP(ptr, args, at));
      llvm::Value *fit = lowering_.unknownCastLowerer().emitUnknownFit(u, element);
      i->addIncoming(lowering_.builder_.CreateAdd(i, lowering_.builder_.getInt32(1)),
                     lowering_.builder_.GetInsertBlock());
      fits->addIncoming(
          lowering_.builder_.CreateAnd(
              fits, lowering_.builder_.CreateICmpNE(fit, lowering_.builder_.getInt32(0))),
          lowering_.builder_.GetInsertBlock());
      lowering_.builder_.CreateBr(loopBlock);
      lowering_.functionFrame().switchTo(doneBlock);
      ok = lowering_.builder_.CreateAnd(ok, fits);
    }
    v.ok = ok;
    all.push_back(std::move(v));
  }
  // Whether `a` fits better than `b`: each argument no worse, one better;
  // or the same, and `a` takes no rest where `b` does.
  auto better = [&](const Variant &a, const Variant &b) {
    llvm::Value *noWorse = lowering_.builder_.getTrue(), *anyBetter = lowering_.builder_.getFalse(),
                *anyWorse = lowering_.builder_.getFalse();
    for (size_t i = 0; i < compared; ++i) {
      const Type *pa = a.param(i), *pb = b.param(i);
      if (!pa || !pb) {
        continue; // one of them takes no argument there: it fits no such call
      }
      bool toB = narrower(pa, pb), toA = narrower(pb, pa);
      llvm::Value *fa = a.fits[i], *fb = b.fits[i];
      llvm::Value *same = lowering_.builder_.CreateICmpEQ(fa, fb);
      llvm::Value *higher = lowering_.builder_.CreateICmpUGT(fa, fb),
                  *lower = lowering_.builder_.CreateICmpULT(fa, fb);
      llvm::Value *ge = toB ? lowering_.builder_.CreateOr(higher, same) : higher;
      llvm::Value *gt = toB && !toA ? lowering_.builder_.CreateOr(higher, same) : higher;
      llvm::Value *lt = toA && !toB ? lowering_.builder_.CreateOr(lower, same) : lower;
      llvm::Value *in = given(i);
      noWorse = lowering_.builder_.CreateAnd(
          noWorse, lowering_.builder_.CreateOr(lowering_.builder_.CreateNot(in), ge));
      anyBetter = lowering_.builder_.CreateOr(anyBetter, lowering_.builder_.CreateAnd(in, gt));
      anyWorse = lowering_.builder_.CreateOr(anyWorse, lowering_.builder_.CreateAnd(in, lt));
    }
    llvm::Value *plainer = lowering_.builder_.getInt1(!a.sym->hasRest && b.sym->hasRest);

    return lowering_.builder_.CreateAnd(
        noWorse, lowering_.builder_.CreateOr(
                     anyBetter, lowering_.builder_.CreateAnd(lowering_.builder_.CreateNot(anyWorse),
                                                             plainer)));
  };
  llvm::Value *anyOk = lowering_.builder_.getFalse();
  for (const Variant &v : all) {
    anyOk = lowering_.builder_.CreateOr(anyOk, v.ok);
  }
  llvm::Value *chosen = lowering_.builder_.CreateSelect(
      anyOk, lowering_.builder_.getInt32(uint32_t(-2)), lowering_.builder_.getInt32(uint32_t(-1)));
  for (size_t a = all.size(); a-- > 0;) {
    llvm::Value *best = all[a].ok;
    for (size_t b = 0; b < all.size(); ++b) {
      if (b != a) {
        best = lowering_.builder_.CreateAnd(
            best, lowering_.builder_.CreateOr(lowering_.builder_.CreateNot(all[b].ok),
                                              better(all[a], all[b])));
      }
    }
    chosen = lowering_.builder_.CreateSelect(best, lowering_.builder_.getInt32(uint32_t(a)), chosen,
                                             "variant");
  }

  return chosen;
}

void ReflectArgsLowerer::emitReflectMismatch(llvm::StringRef what, llvm::Value *index,
                                             const Type *expected, llvm::Value *got) {
  llvm::Type *ptr = lowering_.builder_.getPtrTy(), *i32 = lowering_.builder_.getInt32Ty();
  llvm::StringRef name = expected->name();
  lowering_.builder_.CreateCall(
      lowering_.unknownLowerer().unknownFunction(
          "tz_reflect_mismatch", lowering_.builder_.getVoidTy(), {ptr, i32, i32, ptr, i32, ptr}),
      {lowering_.stringLowerer().utf8Constant(what),
       lowering_.builder_.getInt32(uint32_t(what.size())), index,
       lowering_.stringLowerer().utf8Constant(name),
       lowering_.builder_.getInt32(uint32_t(name.size())), got});
}

// The arguments `args[from..count)` as the parameters `params` take them,
// the rest parameter last (of `rest`, an array type, or null), in `values`;
// on a mismatch what was converted is released and control goes to the
// block `onFail` gives for the argument, with what it holds. `min` arguments
// are surely there.
void ReflectArgsLowerer::emitReflectArgs(
    llvm::Value *args, llvm::Value *count, size_t min, llvm::ArrayRef<const Type *> params,
    const Type *rest, std::vector<llvm::Value *> &values,
    llvm::function_ref<void(llvm::Value *, const Type *, llvm::Value *)> onFail) {
  llvm::Type *ptr = lowering_.builder_.getPtrTy(), *i64 = lowering_.builder_.getInt64Ty();
  size_t before = values.size();
  auto releaseMade = [&](size_t upTo) {
    for (size_t k = before; k < upTo; ++k) {
      lowering_.refCounts().releaseValue(values[k], params[k - before]);
    }
  };
  auto argAt = [&](llvm::Value *i) {
    return lowering_.builder_.CreateLoad(ptr, lowering_.builder_.CreateInBoundsGEP(ptr, args, i),
                                         "arg");
  };
  for (size_t i = 0; i < params.size(); ++i) {
    llvm::Value *u;
    if (i < min) {
      u = argAt(lowering_.builder_.getInt64(i));
    } else {
      // Left out: undefined.
      llvm::BasicBlock *entry = lowering_.builder_.GetInsertBlock();
      llvm::BasicBlock *givenBlock = lowering_.functionFrame().newBlock("reflect.given");
      llvm::BasicBlock *endBlock = lowering_.functionFrame().newBlock("reflect.arg");
      lowering_.builder_.CreateCondBr(
          lowering_.builder_.CreateICmpULT(lowering_.builder_.getInt32(uint32_t(i)), count),
          givenBlock, endBlock);
      lowering_.functionFrame().switchTo(givenBlock);
      llvm::Value *given = argAt(lowering_.builder_.getInt64(i));
      llvm::BasicBlock *givenEnd = lowering_.builder_.GetInsertBlock();
      lowering_.builder_.CreateBr(endBlock);
      lowering_.functionFrame().switchTo(endBlock);
      llvm::PHINode *phi = lowering_.builder_.CreatePHI(ptr, 2);
      phi->addIncoming(llvm::ConstantPointerNull::get(lowering_.builder_.getPtrTy()), entry);
      phi->addIncoming(given, givenEnd);
      u = phi;
    }
    llvm::BasicBlock *failBlock = lowering_.functionFrame().newBlock("reflect.mismatch");
    values.push_back(lowering_.unknownCastLowerer().emitUnknownAs(u, params[i], failBlock));
    llvm::IRBuilderBase::InsertPoint point = lowering_.builder_.saveIP();
    lowering_.functionFrame().switchTo(failBlock);
    releaseMade(before + i);
    onFail(lowering_.builder_.getInt32(uint32_t(i)), params[i], u);
    lowering_.builder_.restoreIP(point);
  }
  if (!rest) {
    return;
  }
  // The rest, as an array of its elements.
  const Type *element = rest->inner();
  llvm::Value *from = lowering_.builder_.getInt32(uint32_t(params.size()));
  llvm::Value *more = lowering_.builder_.CreateSelect(lowering_.builder_.CreateICmpUGT(count, from),
                                                      lowering_.builder_.CreateSub(count, from),
                                                      lowering_.builder_.getInt32(0));
  llvm::Value *array = lowering_.builder_.CreateCall(
      lowering_.rt_.arrNew,
      {lowering_.typeDescriptors().typeDescriptor(element), lowering_.builder_.getInt64(0),
       lowering_.builder_.CreateZExt(more, i64)},
      "rest");
  llvm::BasicBlock *entry = lowering_.builder_.GetInsertBlock();
  llvm::BasicBlock *loopBlock = lowering_.functionFrame().newBlock("reflect.rest");
  llvm::BasicBlock *bodyBlock = lowering_.functionFrame().newBlock("reflect.rest.body");
  llvm::BasicBlock *doneBlock = lowering_.functionFrame().newBlock("reflect.rest.done");
  lowering_.builder_.CreateBr(loopBlock);
  lowering_.functionFrame().switchTo(loopBlock);
  llvm::PHINode *i = lowering_.builder_.CreatePHI(lowering_.builder_.getInt32Ty(), 2, "i");
  i->addIncoming(from, entry);
  lowering_.builder_.CreateCondBr(lowering_.builder_.CreateICmpULT(i, count), bodyBlock, doneBlock);
  lowering_.functionFrame().switchTo(bodyBlock);
  llvm::Value *u = argAt(lowering_.builder_.CreateZExt(i, i64));
  llvm::BasicBlock *failBlock = lowering_.functionFrame().newBlock("reflect.mismatch");
  llvm::Value *value = lowering_.unknownCastLowerer().emitUnknownAs(u, element, failBlock);
  lowering_.arrayLowerer().emitPush(array, {value}, element);
  i->addIncoming(lowering_.builder_.CreateAdd(i, lowering_.builder_.getInt32(1)),
                 lowering_.builder_.GetInsertBlock());
  lowering_.builder_.CreateBr(loopBlock);
  llvm::IRBuilderBase::InsertPoint point = lowering_.builder_.saveIP();
  lowering_.functionFrame().switchTo(failBlock);
  lowering_.refCounts().releaseValue(array, rest);
  releaseMade(before + params.size());
  onFail(i, element, u);
  lowering_.builder_.restoreIP(point);
  lowering_.functionFrame().switchTo(doneBlock);
  values.push_back(array);
}

} // namespace tz
