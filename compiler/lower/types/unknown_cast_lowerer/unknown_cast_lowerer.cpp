#include "compiler/lower/types/unknown_cast_lowerer/unknown_cast_lowerer.h"
#include "compiler/lower/async/promise_lowerer/promise_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/layouts/type_descriptors/type_descriptors.h"
#include "compiler/lower/library/regex/matcher_builder/matcher_builder.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/objects/dict_lowerer/dict_lowerer.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/lower/types/union_lowerer/union_lowerer.h"
#include "compiler/lower/types/unknown_lowerer/unknown_lowerer.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/MDBuilder.h"

namespace tz {

namespace {

constexpr uint32_t kLayoutClass = 2097152;

constexpr uint32_t kLayoutDate = 1024;

enum : uint32_t { kBoxDouble = 3 };

enum : uint32_t { kKindString = 2, kKindBoolean = 3 };

}

UnknownCastLowerer::UnknownCastLowerer(Lowering &owner)
    : lowering(owner) {}

bool UnknownCastLowerer::plainObject(const Type *type) {
  return type->isObject() && !type->isClass() && !type->isClassRef() && !type->isBase() &&
         type->bases().empty() && type->overloads().empty() && !type->isIncomplete() &&
         !type->isAnyObject() && !type->isDict() && !type->isDate() && !type->isRegExp() &&
         !type->isMatch() && !type->isIntl() && !type->isCrypto() && !type->isWeakRef();
}

llvm::Value *UnknownCastLowerer::classIdIn(llvm::Value *id,
                                           llvm::ArrayRef<const ClassDecl *> classes) {
  llvm::Value *any = this->lowering.builder.getFalse();
  for (const ClassDecl *cls : classes) {
    unsigned last = std::max(cls->id, cls->lastId);
    llvm::Value *from =
        this->lowering.builder.CreateSub(id, this->lowering.builder.getInt32(cls->id));
    any = this->lowering.builder.CreateOr(
        any, this->lowering.builder.CreateICmpULE(from,
                                                  this->lowering.builder.getInt32(last - cls->id)));
  }

  return any;
}

std::vector<const ClassDecl *> UnknownCastLowerer::classesOfType(const Type *type) {
  std::vector<const ClassDecl *> classes;
  const ClassDecl *cls = type->classDecl();
  if (cls) {
    classes.push_back(cls);
    return classes;
  }
  for (const ClassDecl *emitted : this->lowering.emittedClasses) {
    if (emitted->type && !emitted->literal() && emitted->type->isSubtypeOf(type)) {
      classes.push_back(emitted);
    }
  }

  return classes;
}

llvm::Value *UnknownCastLowerer::emitUnknownAs(llvm::Value *u, const Type *to,
                                               llvm::BasicBlock *fail) {
  llvm::Type *ptr = this->lowering.builder.getPtrTy();
  llvm::Type *i32 = this->lowering.builder.getInt32Ty();
  auto test = [&](llvm::Value *ok) {
    llvm::BasicBlock *next = this->lowering.functionFrame().newBlock("reflect.fits");
    this->lowering.builder.CreateCondBr(ok, next, fail);
    this->lowering.functionFrame().switchTo(next);
  };
  auto through = [&](const char *name, llvm::Type *out) {
    llvm::Value *slot = this->lowering.functionFrame().createStackSlot(out, "reflect.value");
    llvm::Value *ok = this->lowering.builder.CreateCall(
        this->lowering.unknownLowerer().unknownFunction(name, i32, {ptr, ptr}), {u, slot});
    test(this->lowering.builder.CreateICmpNE(ok, this->lowering.builder.getInt32(0)));

    return this->lowering.builder.CreateLoad(out, slot);
  };
  if (to->isUnknown()) {
    this->lowering.refCounts().retainValue(u, to);
    return u;
  }
  if (to->isOptional()) {
    llvm::BasicBlock *entry = this->lowering.builder.GetInsertBlock();
    llvm::BasicBlock *valueBlock = this->lowering.functionFrame().newBlock("reflect.optional");
    llvm::BasicBlock *endBlock = this->lowering.functionFrame().newBlock("reflect.optional.end");
    this->lowering.builder.CreateCondBr(this->lowering.builder.CreateIsNull(u), endBlock,
                                        valueBlock);
    this->lowering.functionFrame().switchTo(valueBlock);
    llvm::Value *value = this->lowering.optionalLowerer().makeOptional(
        this->emitUnknownAs(u, to->inner(), fail), this->lowering.builder.getTrue(), to);
    llvm::BasicBlock *valueEnd = this->lowering.builder.GetInsertBlock();
    this->lowering.builder.CreateBr(endBlock);
    this->lowering.functionFrame().switchTo(endBlock);
    llvm::PHINode *phi =
        this->lowering.builder.CreatePHI(this->lowering.moduleBuilder().lower(to), 2);
    phi->addIncoming(this->lowering.optionalLowerer().undefinedValue(to), entry);
    phi->addIncoming(value, valueEnd);
    return phi;
  }
  if (to->isUnion()) {
    llvm::BasicBlock *endBlock = this->lowering.functionFrame().newBlock("reflect.union.end");
    std::vector<std::pair<llvm::Value *, llvm::BasicBlock *>> found;
    for (const Type *member : to->members()) {
      llvm::BasicBlock *next = this->lowering.functionFrame().newBlock("reflect.union.next");
      llvm::Value *value = this->lowering.exprLowerer().emitConversion(
          this->emitUnknownAs(u, member, next), member, to);
      found.push_back({value, this->lowering.builder.GetInsertBlock()});
      this->lowering.builder.CreateBr(endBlock);
      this->lowering.functionFrame().switchTo(next);
    }
    this->lowering.builder.CreateBr(fail);
    this->lowering.functionFrame().switchTo(endBlock);
    llvm::PHINode *phi = this->lowering.builder.CreatePHI(this->lowering.moduleBuilder().lower(to),
                                                          unsigned(found.size()));
    for (auto [value, block] : found) {
      phi->addIncoming(value, block);
    }
    return phi;
  }
  switch (to->kind()) {
  case Type::Kind::Int: {
    llvm::Value *slot = this->lowering.functionFrame().createStackSlot(
        this->lowering.builder.getInt64Ty(), "reflect.value");
    llvm::Value *ok = this->lowering.builder.CreateCall(
        this->lowering.unknownLowerer().unknownFunction("tz_unknown_to_integer", i32,
                                                        {ptr, ptr, i32}),
        {u, slot, this->lowering.builder.getInt32(TypeDescriptors::descriptorKind(to))});
    test(this->lowering.builder.CreateICmpNE(ok, this->lowering.builder.getInt32(0)));
    return this->lowering.builder.CreateTrunc(
        this->lowering.builder.CreateLoad(this->lowering.builder.getInt64Ty(), slot),
        this->lowering.moduleBuilder().lower(to));
  }
  case Type::Kind::Double:
    return through("tz_unknown_to_double", this->lowering.builder.getDoubleTy());
  case Type::Kind::Bool:
    return this->lowering.builder.CreateICmpNE(through("tz_unknown_to_bool", i32),
                                               this->lowering.builder.getInt32(0));
  case Type::Kind::String:
    return through("tz_unknown_to_string", this->lowering.strTy);
  case Type::Kind::Array:
  case Type::Kind::Function:
  case Type::Kind::Decimal:
  case Type::Kind::Result:
  case Type::Kind::Json: {
    llvm::Value *slot = this->lowering.functionFrame().createStackSlot(
        this->lowering.moduleBuilder().lower(to), "reflect.value");
    llvm::Value *ok = this->lowering.builder.CreateCall(
        this->lowering.unknownLowerer().unknownFunction("tz_unknown_to_box", i32, {ptr, ptr, ptr}),
        {u, this->lowering.typeDescriptors().typeDescriptor(to), slot});
    test(this->lowering.builder.CreateICmpNE(ok, this->lowering.builder.getInt32(0)));
    return this->lowering.builder.CreateLoad(this->lowering.moduleBuilder().lower(to), slot);
  }
  case Type::Kind::Promise: {
    llvm::Value *layout = this->lowering.builder.CreateCall(
        this->lowering.unknownLowerer().unknownFunction("tz_unknown_layout", ptr, {ptr}), {u},
        "layout");
    test(this->lowering.builder.CreateICmpEQ(
        layout, this->lowering.promiseLowerer().promiseLayout(to->promised())));
    this->lowering.refCounts().retainValue(u, to);
    return u;
  }
  case Type::Kind::Object:
  case Type::Kind::Dict:
  case Type::Kind::ClassRef: {
    llvm::Value *layout = this->lowering.builder.CreateCall(
        this->lowering.unknownLowerer().unknownFunction("tz_unknown_layout", ptr, {ptr}), {u},
        "layout");
    test(this->lowering.builder.CreateIsNotNull(layout));
    auto loadLayout = [&](unsigned field) {
      return this->lowering.builder.CreateLoad(
          i32, this->lowering.builder.CreateStructGEP(this->lowering.layoutDescTy, layout, field));
    };
    llvm::Value *flags = loadLayout(3);
    llvm::Value *isClass = this->lowering.builder.CreateICmpNE(
        this->lowering.builder.CreateAnd(flags, this->lowering.builder.getInt32(kLayoutClass)),
        this->lowering.builder.getInt32(0));
    if (to->isClassRef()) {
      test(isClass);
      const ClassDecl *cls = to->classRef();
      if (cls) {
        test(this->lowering.builder.CreateICmpEQ(u,
                                                 this->lowering.unknownLowerer().classObject(cls)));
      } else if (!to->constructed()->isAnyObject()) {
        llvm::Value *id =
            this->lowering.builder.CreateLoad(i32,
                                              this->lowering.builder.CreateConstInBoundsGEP1_64(
                                                  this->lowering.builder.getInt8Ty(), u, 24),
                                              "class");
        test(this->classIdIn(id, this->classesOfType(to->constructed())));
      }
      return u;
    }
    if (to->isDict()) {
      test(this->lowering.builder.CreateICmpEQ(
          layout, this->lowering.dictLowerer().dictLayout(to).descriptor));
    } else if (to->isDate()) {
      test(this->lowering.builder.CreateICmpNE(
          this->lowering.builder.CreateAnd(flags, this->lowering.builder.getInt32(kLayoutDate)),
          this->lowering.builder.getInt32(0)));
    } else if (!to->isAnyObject()) {
      std::vector<const ClassDecl *> classes = to->isClass() || to->isBase() || !to->bases().empty()
                                                   ? this->classesOfType(to)
                                                   : std::vector<const ClassDecl *>();
      test(this->lowering.builder.CreateNot(isClass));
      if (!classes.empty()) {
        test(this->classIdIn(loadLayout(6), classes));
      } else if (UnknownCastLowerer::plainObject(to)) {
        test(this->lowering.builder.CreateICmpEQ(
            loadLayout(6),
            this->lowering.builder.getInt32(this->lowering.unionLowerer().plainTypeId(to))));
      } else {
        this->lowering.builder.CreateBr(fail);
        this->lowering.functionFrame().startDeadBlock();
        return llvm::Constant::getNullValue(this->lowering.moduleBuilder().lower(to));
      }
    }
    this->lowering.refCounts().retainValue(u, to);
    return u;
  }
  default:
    this->lowering.builder.CreateBr(fail);
    this->lowering.functionFrame().startDeadBlock();
    return llvm::Constant::getNullValue(this->lowering.moduleBuilder().lower(to));
  }
}

llvm::Value *UnknownCastLowerer::emitUnknownFit(llvm::Value *u, const Type *to) {
  llvm::Type *ptr = this->lowering.builder.getPtrTy();
  llvm::Type *i32 = this->lowering.builder.getInt32Ty();
  auto level = [&](uint32_t n) { return this->lowering.builder.getInt32(n); };
  auto pick = [&](llvm::Value *cond, uint32_t yes, llvm::Value *no) {
    return this->lowering.builder.CreateSelect(cond, level(yes), no);
  };
  if (to->isUnknown()) {
    return level(1);
  }
  if (to->isOptional()) {
    return this->lowering.builder.CreateSelect(this->lowering.builder.CreateIsNull(u), level(3),
                                               this->emitUnknownFit(u, to->inner()));
  }
  if (to->isUnion()) {
    llvm::Value *best = level(0);
    for (const Type *member : to->members()) {
      llvm::Value *fit = this->emitUnknownFit(u, member);
      best = this->lowering.builder.CreateSelect(this->lowering.builder.CreateICmpUGT(fit, best),
                                                 fit, best);
    }
    return best;
  }
  if (to->isLiteral()) {
    return level(0);
  }
  auto boxKind = [&] {
    return this->lowering.builder.CreateCall(
        this->lowering.unknownLowerer().unknownFunction("tz_unknown_box_kind", i32, {ptr}), {u});
  };
  auto kind = [&] {
    return this->lowering.builder.CreateCall(
        this->lowering.unknownLowerer().unknownFunction("tz_unknown_kind", i32, {ptr}), {u});
  };
  auto takes = [&](const char *name, llvm::Type *out) {
    llvm::Value *slot = this->lowering.functionFrame().createStackSlot(out, "fit.value");
    llvm::Value *ok = this->lowering.builder.CreateCall(
        this->lowering.unknownLowerer().unknownFunction(name, i32, {ptr, ptr}), {u, slot});

    return this->lowering.builder.CreateICmpNE(ok, this->lowering.builder.getInt32(0));
  };
  switch (to->kind()) {
  case Type::Kind::Int: {
    llvm::Value *box = boxKind();
    llvm::Value *slot = this->lowering.functionFrame().createStackSlot(
        this->lowering.builder.getInt64Ty(), "fit.value");
    llvm::Value *fits = this->lowering.builder.CreateICmpNE(
        this->lowering.builder.CreateCall(this->lowering.unknownLowerer().unknownFunction(
                                              "tz_unknown_to_integer", i32, {ptr, ptr, i32}),
                                          {u, slot, level(TypeDescriptors::descriptorKind(to))}),
        level(0));
    return pick(
        this->lowering.builder.CreateICmpEQ(box, level(TypeDescriptors::descriptorKind(to))), 3,
        pick(fits, 1, level(0)));
  }
  case Type::Kind::Double: {
    llvm::Value *box = boxKind();
    llvm::Value *integer = this->lowering.builder.CreateOr(
        this->lowering.builder.CreateOr(this->lowering.builder.CreateICmpEQ(box, level(1)),
                                        this->lowering.builder.CreateICmpEQ(box, level(2))),
        this->lowering.builder.CreateICmpULE(this->lowering.builder.CreateSub(box, level(18)),
                                             level(5)));
    return pick(this->lowering.builder.CreateICmpEQ(box, level(kBoxDouble)), 3,
                pick(integer, 2,
                     pick(takes("tz_unknown_to_double", this->lowering.builder.getDoubleTy()), 1,
                          level(0))));
  }
  case Type::Kind::Bool:
    return pick(this->lowering.builder.CreateICmpEQ(kind(), level(kKindBoolean)), 3, level(0));
  case Type::Kind::String:
    return pick(this->lowering.builder.CreateICmpEQ(kind(), level(kKindString)), 3, level(0));
  case Type::Kind::Array:
  case Type::Kind::Function:
  case Type::Kind::Decimal:
  case Type::Kind::Result:
  case Type::Kind::Json: {
    llvm::Value *holds = this->lowering.builder.CreateCall(
        this->lowering.unknownLowerer().unknownFunction("tz_unknown_holds", i32, {ptr, ptr}),
        {u, this->lowering.typeDescriptors().typeDescriptor(to)});
    return pick(this->lowering.builder.CreateICmpNE(holds, level(0)), 3, level(0));
  }
  case Type::Kind::Object:
  case Type::Kind::Dict:
  case Type::Kind::ClassRef:
    break;
  default:
    return level(0);
  }
  llvm::Value *layout = this->lowering.builder.CreateCall(
      this->lowering.unknownLowerer().unknownFunction("tz_unknown_layout", ptr, {ptr}), {u},
      "layout");
  llvm::BasicBlock *entry = this->lowering.builder.GetInsertBlock();
  llvm::BasicBlock *objectBlock = this->lowering.functionFrame().newBlock("fit.object");
  llvm::BasicBlock *endBlock = this->lowering.functionFrame().newBlock("fit.end");
  this->lowering.builder.CreateCondBr(this->lowering.builder.CreateIsNotNull(layout), objectBlock,
                                      endBlock);
  this->lowering.functionFrame().switchTo(objectBlock);
  auto loadLayout = [&](unsigned field) {
    return this->lowering.builder.CreateLoad(
        i32, this->lowering.builder.CreateStructGEP(this->lowering.layoutDescTy, layout, field));
  };
  llvm::Value *flags = loadLayout(3);
  llvm::Value *isClass = this->lowering.builder.CreateICmpNE(
      this->lowering.builder.CreateAnd(flags, this->lowering.builder.getInt32(kLayoutClass)),
      this->lowering.builder.getInt32(0));
  llvm::Value *fit = level(0);
  if (to->isClassRef()) {
    const ClassDecl *cls = to->classRef();
    if (cls) {
      fit = pick(
          this->lowering.builder.CreateICmpEQ(u, this->lowering.unknownLowerer().classObject(cls)),
          3, level(0));
    } else if (to->constructed()->isAnyObject()) {
      fit = pick(isClass, 2, level(0));
    } else {
      llvm::Value *at = this->lowering.builder.CreateSelect(
          isClass,
          this->lowering.builder.CreateConstGEP1_64(this->lowering.builder.getInt8Ty(), u, 24),
          layout);
      llvm::Value *id = this->lowering.builder.CreateLoad(i32, at, "class");
      fit = pick(this->lowering.builder.CreateAnd(
                     isClass, this->classIdIn(id, this->classesOfType(to->constructed()))),
                 2, level(0));
    }
  } else if (to->isDict()) {
    fit = pick(this->lowering.builder.CreateICmpEQ(
                   layout, this->lowering.dictLowerer().dictLayout(to).descriptor),
               3, level(0));
  } else if (to->isDate()) {
    fit = pick(
        this->lowering.builder.CreateICmpNE(
            this->lowering.builder.CreateAnd(flags, this->lowering.builder.getInt32(kLayoutDate)),
            this->lowering.builder.getInt32(0)),
        3, level(0));
  } else if (to->isAnyObject()) {
    fit = level(1);
  } else if (to->isClass() || to->isBase() || !to->bases().empty()) {
    std::vector<const ClassDecl *> classes = this->classesOfType(to);
    if (!classes.empty()) {
      llvm::Value *id = loadLayout(6);
      llvm::Value *in = this->lowering.builder.CreateAnd(this->lowering.builder.CreateNot(isClass),
                                                         this->classIdIn(id, classes));
      llvm::Value *exact = this->lowering.builder.getFalse();
      const ClassDecl *cls = to->classDecl();
      if (cls) {
        exact = this->lowering.builder.CreateICmpEQ(id, this->lowering.builder.getInt32(cls->id));
      }
      fit = pick(this->lowering.builder.CreateAnd(in, exact), 3, pick(in, 2, level(0)));
    }
  } else if (UnknownCastLowerer::plainObject(to)) {
    llvm::Value *same = this->lowering.builder.CreateICmpEQ(
        loadLayout(6),
        this->lowering.builder.getInt32(this->lowering.unionLowerer().plainTypeId(to)));
    fit = pick(this->lowering.builder.CreateAnd(this->lowering.builder.CreateNot(isClass), same), 3,
               level(0));
  }
  llvm::BasicBlock *objectEnd = this->lowering.builder.GetInsertBlock();
  this->lowering.builder.CreateBr(endBlock);
  this->lowering.functionFrame().switchTo(endBlock);
  llvm::PHINode *phi = this->lowering.builder.CreatePHI(i32, 2, "fit");
  phi->addIncoming(level(0), entry);
  phi->addIncoming(fit, objectEnd);

  return phi;
}

}
