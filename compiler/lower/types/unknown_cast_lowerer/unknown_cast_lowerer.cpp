#include "compiler/lower/types/unknown_cast_lowerer/unknown_cast_lowerer.h"
#include "compiler/library/regex/matcher_builder/matcher_builder.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/layouts/type_descriptors/type_descriptors.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/objects/dict_lowerer/dict_lowerer.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/lower/types/unknown_lowerer/unknown_lowerer.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/MDBuilder.h"

using llvm::BasicBlock;
using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::Value;

namespace tz {

namespace {

// TZ_LAYOUT_CLASS and TZ_LAYOUT_DATE of runtime/core/tzrt/tzrt.h.
constexpr uint32_t kLayoutClass = 2097152;

constexpr uint32_t kLayoutDate = 1024;

// TZ_T_* of runtime/core/tzrt/tzrt.h, and the kinds tz_unknown_kind gives.
enum : uint32_t { kBoxInt = 1, kBoxLong = 2, kBoxDouble = 3 };

enum : uint32_t { kKindString = 2, kKindBoolean = 3 };

} // namespace

UnknownCastLowerer::UnknownCastLowerer(Lowering &lowering) : lowering_(lowering) {}

llvm::Value *UnknownCastLowerer::classIdIn(llvm::Value *id,
                                           llvm::ArrayRef<const ClassDecl *> classes) {
  llvm::Value *any = lowering_.builder_.getFalse();
  for (const ClassDecl *cls : classes) {
    unsigned last = std::max(cls->id, cls->lastId);
    llvm::Value *from = lowering_.builder_.CreateSub(id, lowering_.builder_.getInt32(cls->id));
    any = lowering_.builder_.CreateOr(
        any, lowering_.builder_.CreateICmpULE(from, lowering_.builder_.getInt32(last - cls->id)));
  }

  return any;
}

std::vector<const ClassDecl *> UnknownCastLowerer::classesOfType(const Type *type) {
  std::vector<const ClassDecl *> classes;
  if (const ClassDecl *cls = type->classDecl()) {
    classes.push_back(cls);
    return classes;
  }
  for (const ClassDecl *cls : lowering_.emittedClasses_) {
    if (cls->type && !cls->literal && cls->type->isSubtypeOf(type)) {
      classes.push_back(cls);
    }
  }

  return classes;
}

llvm::Value *UnknownCastLowerer::emitUnknownAs(llvm::Value *u, const Type *to,
                                               llvm::BasicBlock *fail) {
  llvm::Type *ptr = lowering_.builder_.getPtrTy(), *i32 = lowering_.builder_.getInt32Ty();
  // Goes on where `ok` holds.
  auto test = [&](llvm::Value *ok) {
    llvm::BasicBlock *next = lowering_.functionFrame().newBlock("reflect.fits");
    lowering_.builder_.CreateCondBr(ok, next, fail);
    lowering_.functionFrame().switchTo(next);
  };
  // tz_unknown_to_X(u, &out): whether it holds one, and the value.
  auto through = [&](const char *name, llvm::Type *out) {
    llvm::Value *slot = lowering_.functionFrame().createStackSlot(out, "reflect.value");
    llvm::Value *ok = lowering_.builder_.CreateCall(
        lowering_.unknownLowerer().unknownFunction(name, i32, {ptr, ptr}), {u, slot});
    test(lowering_.builder_.CreateICmpNE(ok, lowering_.builder_.getInt32(0)));

    return lowering_.builder_.CreateLoad(out, slot);
  };
  if (to->isUnknown()) {
    lowering_.refCounts().retainValue(u, to);
    return u;
  }
  if (to->isOptional()) {
    // Undefined is undefined; anything else must be a T.
    llvm::BasicBlock *entry = lowering_.builder_.GetInsertBlock();
    llvm::BasicBlock *valueBlock = lowering_.functionFrame().newBlock("reflect.optional");
    llvm::BasicBlock *endBlock = lowering_.functionFrame().newBlock("reflect.optional.end");
    lowering_.builder_.CreateCondBr(lowering_.builder_.CreateIsNull(u), endBlock, valueBlock);
    lowering_.functionFrame().switchTo(valueBlock);
    llvm::Value *value = lowering_.optionalLowerer().makeOptional(
        emitUnknownAs(u, to->inner(), fail), lowering_.builder_.getTrue(), to);
    llvm::BasicBlock *valueEnd = lowering_.builder_.GetInsertBlock();
    lowering_.builder_.CreateBr(endBlock);
    lowering_.functionFrame().switchTo(endBlock);
    llvm::PHINode *phi = lowering_.builder_.CreatePHI(lowering_.moduleBuilder().lower(to), 2);
    phi->addIncoming(lowering_.optionalLowerer().undefinedValue(to), entry);
    phi->addIncoming(value, valueEnd);
    return phi;
  }
  if (to->isUnion()) {
    // The first member that it holds.
    llvm::BasicBlock *endBlock = lowering_.functionFrame().newBlock("reflect.union.end");
    std::vector<std::pair<llvm::Value *, llvm::BasicBlock *>> found;
    for (const Type *member : to->members()) {
      llvm::BasicBlock *next = lowering_.functionFrame().newBlock("reflect.union.next");
      llvm::Value *value =
          lowering_.exprLowerer().emitConversion(emitUnknownAs(u, member, next), member, to);
      found.push_back({value, lowering_.builder_.GetInsertBlock()});
      lowering_.builder_.CreateBr(endBlock);
      lowering_.functionFrame().switchTo(next);
    }
    lowering_.builder_.CreateBr(fail);
    lowering_.functionFrame().switchTo(endBlock);
    llvm::PHINode *phi =
        lowering_.builder_.CreatePHI(lowering_.moduleBuilder().lower(to), unsigned(found.size()));
    for (auto [value, block] : found) {
      phi->addIncoming(value, block);
    }
    return phi;
  }
  switch (to->kind()) {
  case Type::Kind::Int:
    return through("tz_unknown_to_int", i32);
  case Type::Kind::Long:
    return through("tz_unknown_to_long", lowering_.builder_.getInt64Ty());
  case Type::Kind::Double:
    return through("tz_unknown_to_double", lowering_.builder_.getDoubleTy());
  case Type::Kind::Bool:
    return lowering_.builder_.CreateICmpNE(through("tz_unknown_to_bool", i32),
                                           lowering_.builder_.getInt32(0));
  case Type::Kind::String:
    return through("tz_unknown_to_string", lowering_.strTy_);
  case Type::Kind::Array:
  case Type::Kind::Function:
  case Type::Kind::Decimal:
  case Type::Kind::Result:
  case Type::Kind::Json: {
    llvm::Value *slot = lowering_.functionFrame().createStackSlot(
        lowering_.moduleBuilder().lower(to), "reflect.value");
    llvm::Value *ok = lowering_.builder_.CreateCall(
        lowering_.unknownLowerer().unknownFunction("tz_unknown_to_box", i32, {ptr, ptr, ptr}),
        {u, lowering_.typeDescriptors().typeDescriptor(to), slot});
    test(lowering_.builder_.CreateICmpNE(ok, lowering_.builder_.getInt32(0)));
    return lowering_.builder_.CreateLoad(lowering_.moduleBuilder().lower(to), slot);
  }
  case Type::Kind::Object:
  case Type::Kind::Dict:
  case Type::Kind::ClassRef: {
    llvm::Value *layout = lowering_.builder_.CreateCall(
        lowering_.unknownLowerer().unknownFunction("tz_unknown_layout", ptr, {ptr}), {u}, "layout");
    test(lowering_.builder_.CreateIsNotNull(layout));
    auto loadLayout = [&](unsigned field) {
      return lowering_.builder_.CreateLoad(
          i32, lowering_.builder_.CreateStructGEP(lowering_.layoutDescTy_, layout, field));
    };
    llvm::Value *flags = loadLayout(3);
    llvm::Value *isClass = lowering_.builder_.CreateICmpNE(
        lowering_.builder_.CreateAnd(flags, lowering_.builder_.getInt32(kLayoutClass)),
        lowering_.builder_.getInt32(0));
    if (to->isClassRef()) {
      // A class itself: that one, or one whose objects are T.
      test(isClass);
      if (const ClassDecl *cls = to->classRef()) {
        test(lowering_.builder_.CreateICmpEQ(u, lowering_.unknownLowerer().classObject(cls)));
      } else if (!to->constructed()->isAnyObject()) {
        // TzClass: { rc, layout, parent, i32 id, ... }.
        llvm::Value *id = lowering_.builder_.CreateLoad(
            i32,
            lowering_.builder_.CreateConstInBoundsGEP1_64(lowering_.builder_.getInt8Ty(), u, 24),
            "class");
        test(classIdIn(id, classesOfType(to->constructed())));
      }
      return u; // never counted
    }
    if (to->isDict()) {
      test(lowering_.builder_.CreateICmpEQ(layout,
                                           lowering_.dictLowerer().dictLayout(to).descriptor));
    } else if (to->isDate()) {
      test(lowering_.builder_.CreateICmpNE(
          lowering_.builder_.CreateAnd(flags, lowering_.builder_.getInt32(kLayoutDate)),
          lowering_.builder_.getInt32(0)));
    } else if (!to->isAnyObject()) {
      std::vector<const ClassDecl *> classes = to->isClass() || to->isBase() || !to->bases().empty()
                                                   ? classesOfType(to)
                                                   : std::vector<const ClassDecl *>();
      if (classes.empty()) { // no check tells a literal type apart
        lowering_.builder_.CreateBr(fail);
        lowering_.functionFrame().startDeadBlock();
        return llvm::Constant::getNullValue(lowering_.moduleBuilder().lower(to));
      }
      test(lowering_.builder_.CreateNot(isClass));
      test(classIdIn(loadLayout(6), classes));
    }
    lowering_.refCounts().retainValue(u, to);
    return u;
  }
  default:
    lowering_.builder_.CreateBr(fail);
    lowering_.functionFrame().startDeadBlock();
    return llvm::Constant::getNullValue(lowering_.moduleBuilder().lower(to));
  }
}

llvm::Value *UnknownCastLowerer::emitUnknownFit(llvm::Value *u, const Type *to) {
  llvm::Type *ptr = lowering_.builder_.getPtrTy(), *i32 = lowering_.builder_.getInt32Ty();
  auto level = [&](uint32_t n) { return lowering_.builder_.getInt32(n); };
  auto pick = [&](llvm::Value *cond, uint32_t yes, llvm::Value *no) {
    return lowering_.builder_.CreateSelect(cond, level(yes), no);
  };
  if (to->isUnknown()) {
    return level(1);
  }
  if (to->isOptional()) {
    return lowering_.builder_.CreateSelect(lowering_.builder_.CreateIsNull(u), level(3),
                                           emitUnknownFit(u, to->inner()));
  }
  if (to->isUnion()) {
    // As well as the member it fits best.
    llvm::Value *best = level(0);
    for (const Type *member : to->members()) {
      llvm::Value *fit = emitUnknownFit(u, member);
      best =
          lowering_.builder_.CreateSelect(lowering_.builder_.CreateICmpUGT(fit, best), fit, best);
    }
    return best;
  }
  if (to->isLiteral()) {
    return level(0); // no check tells one value of a type apart
  }
  auto boxKind = [&] {
    return lowering_.builder_.CreateCall(
        lowering_.unknownLowerer().unknownFunction("tz_unknown_box_kind", i32, {ptr}), {u});
  };
  auto kind = [&] {
    return lowering_.builder_.CreateCall(
        lowering_.unknownLowerer().unknownFunction("tz_unknown_kind", i32, {ptr}), {u});
  };
  // Whether tz_unknown_to_X takes it, for a number.
  auto takes = [&](const char *name, llvm::Type *out) {
    llvm::Value *slot = lowering_.functionFrame().createStackSlot(out, "fit.value");
    llvm::Value *ok = lowering_.builder_.CreateCall(
        lowering_.unknownLowerer().unknownFunction(name, i32, {ptr, ptr}), {u, slot});

    return lowering_.builder_.CreateICmpNE(ok, lowering_.builder_.getInt32(0));
  };
  switch (to->kind()) {
  case Type::Kind::Int: {
    llvm::Value *box = boxKind();
    return pick(lowering_.builder_.CreateICmpEQ(box, level(kBoxInt)), 3,
                pick(takes("tz_unknown_to_int", i32), 1, level(0)));
  }
  case Type::Kind::Long: {
    llvm::Value *box = boxKind();
    return pick(
        lowering_.builder_.CreateICmpEQ(box, level(kBoxLong)), 3,
        pick(lowering_.builder_.CreateICmpEQ(box, level(kBoxInt)), 2,
             pick(takes("tz_unknown_to_long", lowering_.builder_.getInt64Ty()), 1, level(0))));
  }
  case Type::Kind::Double: {
    llvm::Value *box = boxKind();
    llvm::Value *integer =
        lowering_.builder_.CreateOr(lowering_.builder_.CreateICmpEQ(box, level(kBoxInt)),
                                    lowering_.builder_.CreateICmpEQ(box, level(kBoxLong)));
    return pick(
        lowering_.builder_.CreateICmpEQ(box, level(kBoxDouble)), 3,
        pick(integer, 2,
             pick(takes("tz_unknown_to_double", lowering_.builder_.getDoubleTy()), 1, level(0))));
  }
  case Type::Kind::Bool:
    return pick(lowering_.builder_.CreateICmpEQ(kind(), level(kKindBoolean)), 3, level(0));
  case Type::Kind::String:
    return pick(lowering_.builder_.CreateICmpEQ(kind(), level(kKindString)), 3, level(0));
  case Type::Kind::Array:
  case Type::Kind::Function:
  case Type::Kind::Decimal:
  case Type::Kind::Result:
  case Type::Kind::Json: {
    llvm::Value *holds = lowering_.builder_.CreateCall(
        lowering_.unknownLowerer().unknownFunction("tz_unknown_holds", i32, {ptr, ptr}),
        {u, lowering_.typeDescriptors().typeDescriptor(to)});
    return pick(lowering_.builder_.CreateICmpNE(holds, level(0)), 3, level(0));
  }
  case Type::Kind::Object:
  case Type::Kind::Dict:
  case Type::Kind::ClassRef:
    break;
  default:
    return level(0);
  }
  // An object of the language: its layout tells, as for emitUnknownAs.
  llvm::Value *layout = lowering_.builder_.CreateCall(
      lowering_.unknownLowerer().unknownFunction("tz_unknown_layout", ptr, {ptr}), {u}, "layout");
  llvm::BasicBlock *entry = lowering_.builder_.GetInsertBlock();
  llvm::BasicBlock *objectBlock = lowering_.functionFrame().newBlock("fit.object");
  llvm::BasicBlock *endBlock = lowering_.functionFrame().newBlock("fit.end");
  lowering_.builder_.CreateCondBr(lowering_.builder_.CreateIsNotNull(layout), objectBlock,
                                  endBlock);
  lowering_.functionFrame().switchTo(objectBlock);
  auto loadLayout = [&](unsigned field) {
    return lowering_.builder_.CreateLoad(
        i32, lowering_.builder_.CreateStructGEP(lowering_.layoutDescTy_, layout, field));
  };
  llvm::Value *flags = loadLayout(3);
  llvm::Value *isClass = lowering_.builder_.CreateICmpNE(
      lowering_.builder_.CreateAnd(flags, lowering_.builder_.getInt32(kLayoutClass)),
      lowering_.builder_.getInt32(0));
  llvm::Value *fit = level(0);
  if (to->isClassRef()) {
    if (const ClassDecl *cls = to->classRef()) {
      fit = pick(lowering_.builder_.CreateICmpEQ(u, lowering_.unknownLowerer().classObject(cls)), 3,
                 level(0));
    } else if (to->constructed()->isAnyObject()) {
      fit = pick(isClass, 2, level(0));
    } else {
      // TzClass: { rc, layout, parent, i32 id, ... }; read only of a class.
      llvm::Value *at = lowering_.builder_.CreateSelect(
          isClass, lowering_.builder_.CreateConstGEP1_64(lowering_.builder_.getInt8Ty(), u, 24),
          layout);
      llvm::Value *id = lowering_.builder_.CreateLoad(i32, at, "class");
      fit = pick(
          lowering_.builder_.CreateAnd(isClass, classIdIn(id, classesOfType(to->constructed()))), 2,
          level(0));
    }
  } else if (to->isDict()) {
    fit = pick(
        lowering_.builder_.CreateICmpEQ(layout, lowering_.dictLowerer().dictLayout(to).descriptor),
        3, level(0));
  } else if (to->isDate()) {
    fit = pick(lowering_.builder_.CreateICmpNE(
                   lowering_.builder_.CreateAnd(flags, lowering_.builder_.getInt32(kLayoutDate)),
                   lowering_.builder_.getInt32(0)),
               3, level(0));
  } else if (to->isAnyObject()) {
    fit = level(1);
  } else if (to->isClass() || to->isBase() || !to->bases().empty()) {
    std::vector<const ClassDecl *> classes = classesOfType(to);
    if (!classes.empty()) {
      llvm::Value *id = loadLayout(6);
      llvm::Value *in = lowering_.builder_.CreateAnd(lowering_.builder_.CreateNot(isClass),
                                                     classIdIn(id, classes));
      llvm::Value *exact = lowering_.builder_.getFalse();
      if (const ClassDecl *cls = to->classDecl()) {
        exact = lowering_.builder_.CreateICmpEQ(id, lowering_.builder_.getInt32(cls->id));
      }
      fit = pick(lowering_.builder_.CreateAnd(in, exact), 3, pick(in, 2, level(0)));
    }
  }
  llvm::BasicBlock *objectEnd = lowering_.builder_.GetInsertBlock();
  lowering_.builder_.CreateBr(endBlock);
  lowering_.functionFrame().switchTo(endBlock);
  llvm::PHINode *phi = lowering_.builder_.CreatePHI(i32, 2, "fit");
  phi->addIncoming(level(0), entry);
  phi->addIncoming(fit, objectEnd);

  return phi;
}

} // namespace tz
