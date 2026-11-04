#include "compiler/library/reflect/class_info_lowerer/class_info_lowerer.h"
#include "compiler/library/reflect/reflect_args_lowerer/reflect_args_lowerer.h"
#include "compiler/library/regex/matcher_builder/matcher_builder.h"
#include "compiler/lower/classes/class_descriptors/class_descriptors.h"
#include "compiler/lower/classes/class_lowerer/class_lowerer.h"
#include "compiler/lower/function_builder/function_builder.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/functions/closure_lowerer/closure_lowerer.h"
#include "compiler/lower/layouts/object_layouts/object_layouts.h"
#include "compiler/lower/layouts/type_descriptors/type_descriptors.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/memory/store_lowerer/store_lowerer.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/objects/object_maker/object_maker.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/lower/types/union_layout/union_layout.h"
#include "compiler/lower/types/unknown_cast_lowerer/unknown_cast_lowerer.h"
#include "compiler/lower/types/unknown_lowerer/unknown_lowerer.h"
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

std::string className(const ClassDecl *cls) {
  return cls->name.empty() && cls->type ? cls->type->name().str() : cls->name.str();
}

const std::vector<MakerFields> &fillOrders() {
  static const std::vector<MakerFields> orders = {
      {nullptr, {"code", "details"}},
      {nullptr, {"message", "className", "field", "expected", "got", "value"}},
      {nullptr, {"message", "className", "field", "fields"}},
      {nullptr, {"message", "className"}},
  };

  return orders;
}

std::vector<FunctionDecl *> constructorsOf(const ClassDecl *cls) {
  for (const ClassDecl *c = cls; c; c = c->parent) {
    if (!c->constructors.empty()) {
      return c->constructors;
    }
  }

  return {};
}

}

ClassInfoLowerer::ClassInfoLowerer(Lowering &lowering) : lowering_(lowering) {}

llvm::StructType *ClassInfoLowerer::classInfoType() {
  if (!classInfoTy_) {
    llvm::Type *ptr = lowering_.builder_.getPtrTy();
    classInfoTy_ = llvm::StructType::create(
        lowering_.ctx_,
        {ptr, ptr, lowering_.builder_.getInt32Ty(), ptr, lowering_.builder_.getInt32Ty()},
        "tz.classinfo");
  }

  return classInfoTy_;
}

llvm::Constant *ClassInfoLowerer::classInfoGlobal(const ClassDecl *cls) {
  llvm::GlobalVariable *&global = lowering_.classInfos_[cls];
  if (!global) {
    global = new llvm::GlobalVariable(
        *lowering_.module_, classInfoType(), true, llvm::GlobalValue::PrivateLinkage,
        llvm::Constant::getNullValue(classInfoType()),
        "tz.classinfo." + lowering_.symbolPrefixes_.lookup(cls) + className(cls));
  }

  return global;
}

void ClassInfoLowerer::emitConstructThunk(const ClassDecl *cls, llvm::Function *fn) {
  llvm::Type *ptr = lowering_.builder_.getPtrTy(), *i32 = lowering_.builder_.getInt32Ty();
  lowering_.functionBuilder().beginFunction(fn, nullptr);
  lowering_.loc_ = cls->loc();
  lowering_.functionFrame().pushScope();
  llvm::Value *args = fn->getArg(0), *count = fn->getArg(1);
  std::string what = "new " + className(cls);
  llvm::Value *null = llvm::ConstantPointerNull::get(lowering_.builder_.getPtrTy());
  auto fail = [&] {
    lowering_.functionFrame().emitCleanups(0);
    lowering_.builder_.CreateRet(null);
  };
  std::vector<FunctionDecl *> ctors = constructorsOf(cls);
  bool precise = ctors.size() <= 1;
  llvm::BasicBlock *none = precise ? nullptr : lowering_.functionFrame().newBlock("reflect.none");
  llvm::BasicBlock *several =
      precise ? nullptr : lowering_.functionFrame().newBlock("reflect.several");
  std::vector<const FuncSymbol *> variants;
  for (FunctionDecl *ctor : ctors) {
    variants.push_back(ctor->sym);
  }
  if (variants.empty()) {
    variants.push_back(nullptr);
  }
  std::vector<llvm::BasicBlock *> starts;
  if (!precise) {
    std::vector<const FunctionDecl *> choices(ctors.begin(), ctors.end());
    llvm::Value *chosen =
        lowering_.reflectArgsLowerer().emitChooseVariant(choices, nullptr, args, count);
    llvm::SwitchInst *which =
        lowering_.builder_.CreateSwitch(chosen, none, unsigned(ctors.size() + 1));
    which->addCase(lowering_.builder_.getInt32(uint32_t(-2)), several);
    for (size_t v = 0; v < ctors.size(); ++v) {
      starts.push_back(lowering_.functionFrame().newBlock("reflect.variant"));
      which->addCase(lowering_.builder_.getInt32(uint32_t(v)), starts.back());
    }
  }
  for (size_t v = 0; v < variants.size(); ++v) {
    const FuncSymbol *sym = variants[v];
    if (!precise) {
      lowering_.functionFrame().switchTo(starts[v]);
    }
    llvm::BasicBlock *next = none;
    size_t min = sym ? sym->minArgs : 0;
    bool rest = sym && sym->hasRest;
    std::vector<const Type *> params = sym ? sym->params : std::vector<const Type *>();
    const Type *restType = rest ? params.back() : nullptr;
    if (rest) {
      params.pop_back();
    }
    llvm::Value *fits =
        lowering_.builder_.CreateICmpUGE(count, lowering_.builder_.getInt32(uint32_t(min)));
    if (!rest) {
      fits = lowering_.builder_.CreateAnd(
          fits, lowering_.builder_.CreateICmpULE(
                    count, lowering_.builder_.getInt32(uint32_t(params.size()))));
    }
    llvm::BasicBlock *arityOk = lowering_.functionFrame().newBlock("reflect.arity");
    llvm::BasicBlock *arityWrong = lowering_.functionFrame().newBlock("reflect.arity.wrong");
    lowering_.builder_.CreateCondBr(fits, arityOk, arityWrong);
    lowering_.functionFrame().switchTo(arityWrong);
    if (precise) {
      lowering_.builder_.CreateCall(
          lowering_.unknownLowerer().unknownFunction(
              "tz_reflect_arity", lowering_.builder_.getVoidTy(), {ptr, i32, i32, i32, i32}),
          {lowering_.stringLowerer().utf8Constant(what),
           lowering_.builder_.getInt32(uint32_t(what.size())),
           lowering_.builder_.getInt32(uint32_t(min)),
           lowering_.builder_.getInt32(rest ? -1 : int32_t(params.size())), count});
      fail();
    } else {
      lowering_.builder_.CreateBr(next);
    }
    lowering_.functionFrame().switchTo(arityOk);
    std::vector<llvm::Value *> values;
    lowering_.reflectArgsLowerer().emitReflectArgs(
        args, count, min, params, restType, values,
        [&](llvm::Value *i, const Type *expected, llvm::Value *got) {
          if (!precise) {
            lowering_.builder_.CreateBr(next);
            return;
          }
          lowering_.reflectArgsLowerer().emitReflectMismatch(what, i, expected, got);
          fail();
        });
    llvm::Value *object = lowering_.classLowerer().allocInstance(cls);
    lowering_.classLowerer().emitConstruct(object, cls, sym, values);
    llvm::Value *made = object;
    lowering_.functionFrame().emitCleanups(0);
    lowering_.builder_.CreateRet(made);
  }
  if (none) {
    lowering_.functionFrame().switchTo(none);
    lowering_.reflectArgsLowerer().emitReflectFail("ERR_NO_VARIANT", what + ": no constructor of " +
                                                                         className(cls) +
                                                                         " takes these arguments");
    fail();
    lowering_.functionFrame().switchTo(several);
    lowering_.reflectArgsLowerer().emitReflectFail(
        "ERR_SEVERAL_VARIANTS",
        what + ": the arguments fit several constructors of " + className(cls) + " equally well");
    fail();
  }
  lowering_.functionFrame().startDeadBlock();
  lowering_.functionFrame().popScope();
  lowering_.functionBuilder().finishFunction();
}

llvm::Function *ClassInfoLowerer::fillMaker(const Type *variant) {
  return lowering_.objectMaker().maker(variant, fillOrders());
}

llvm::Constant *ClassInfoLowerer::fillNames(llvm::ArrayRef<std::string> names,
                                            llvm::StringRef label) {
  llvm::Type *ptr = lowering_.builder_.getPtrTy(), *i32 = lowering_.builder_.getInt32Ty();
  if (names.empty()) {
    return llvm::ConstantPointerNull::get(lowering_.builder_.getPtrTy());
  }
  auto *entryType = llvm::StructType::get(lowering_.ctx_, {ptr, i32});
  std::vector<llvm::Constant *> entries;
  for (const std::string &name : names) {
    entries.push_back(
        llvm::ConstantStruct::get(entryType, {lowering_.stringLowerer().utf8Constant(name),
                                              lowering_.builder_.getInt32(uint32_t(name.size()))}));
  }
  auto *arrayType = llvm::ArrayType::get(entryType, entries.size());
  auto *global = new llvm::GlobalVariable(*lowering_.module_, arrayType, true,
                                          llvm::GlobalValue::PrivateLinkage,
                                          llvm::ConstantArray::get(arrayType, entries), label);
  global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);

  return global;
}

llvm::Function *ClassInfoLowerer::fillElement(const Type *element, const FillSite &site) {
  llvm::Function *&made = fillElements_[element];
  if (made) {
    return made;
  }
  llvm::Type *ptr = lowering_.builder_.getPtrTy(), *i32 = lowering_.builder_.getInt32Ty();
  made = llvm::Function::Create(
      llvm::FunctionType::get(i32, {ptr, ptr}, false), llvm::GlobalValue::InternalLinkage,
      "tz.fill.element." + element->name().str(), lowering_.module_.get());
  lowering_.functionBuilder().setFunctionAttributes(made);
  pendingElements_.push_back({made, element});
  pendingSites_.push_back(site);

  return made;
}

void ClassInfoLowerer::emitFillElements() {
  while (!pendingElements_.empty()) {
    auto [fn, element] = pendingElements_.back();
    FillSite site = pendingSites_.back();
    pendingElements_.pop_back();
    pendingSites_.pop_back();
    lowering_.functionBuilder().beginFunction(fn, nullptr);
    lowering_.functionFrame().pushScope();
    llvm::BasicBlock *bad = lowering_.functionFrame().newBlock("fill.element.bad");
    llvm::Value *value = fillValue(fn->getArg(0), element, site, bad, bad);
    lowering_.builder_.CreateStore(value, fn->getArg(1));
    lowering_.functionFrame().emitCleanups(0);
    lowering_.builder_.CreateRet(lowering_.builder_.getInt32(1));
    lowering_.functionFrame().switchTo(bad);
    lowering_.functionFrame().emitCleanups(0);
    lowering_.builder_.CreateRet(lowering_.builder_.getInt32(0));
    lowering_.functionFrame().startDeadBlock();
    lowering_.functionFrame().popScope();
    lowering_.functionBuilder().finishFunction();
  }
}

llvm::Value *ClassInfoLowerer::fillValue(llvm::Value *u, const Type *type, const FillSite &site,
                                         llvm::BasicBlock *mismatch, llvm::BasicBlock *failed) {
  llvm::Type *ptr = lowering_.builder_.getPtrTy(), *i32 = lowering_.builder_.getInt32Ty();
  auto &b = lowering_.builder_;
  auto pass = [&](llvm::Value *ok, llvm::BasicBlock *otherwise) {
    llvm::BasicBlock *next = lowering_.functionFrame().newBlock("fill.ok");
    b.CreateCondBr(ok, next, otherwise);
    lowering_.functionFrame().switchTo(next);
  };
  if (type->isOptional()) {
    llvm::BasicBlock *none = lowering_.functionFrame().newBlock("fill.none");
    llvm::BasicBlock *some = lowering_.functionFrame().newBlock("fill.some");
    llvm::BasicBlock *end = lowering_.functionFrame().newBlock("fill.optional");
    b.CreateCondBr(b.CreateIsNull(u), none, some);
    lowering_.functionFrame().switchTo(none);
    llvm::Value *nothing = lowering_.optionalLowerer().undefinedValue(type);
    llvm::BasicBlock *noneEnd = b.GetInsertBlock();
    b.CreateBr(end);
    lowering_.functionFrame().switchTo(some);
    llvm::Value *inner = fillValue(u, type->inner(), site, mismatch, failed);
    llvm::Value *wrapped = lowering_.optionalLowerer().makeOptional(inner, b.getTrue(), type);
    llvm::BasicBlock *someEnd = b.GetInsertBlock();
    b.CreateBr(end);
    lowering_.functionFrame().switchTo(end);
    llvm::PHINode *phi = b.CreatePHI(lowering_.moduleBuilder().lower(type), 2);
    phi->addIncoming(nothing, noneEnd);
    phi->addIncoming(wrapped, someEnd);
    return phi;
  }
  if (UnionLayout::isAtomType(type)) {
    std::vector<const Type *> literals;
    if (type->isStringLiteral()) {
      literals.push_back(type);
    } else {
      literals.assign(type->members().begin(), type->members().end());
    }
    std::vector<std::string> texts;
    std::vector<llvm::Constant *> atoms;
    for (const Type *literal : literals) {
      std::string text;
      for (char16_t unit : literal->literalText()) {
        if (unit < 0x80) {
          text.push_back(char(unit));
        } else if (unit < 0x800) {
          text.push_back(char(0xC0 | (unit >> 6)));
          text.push_back(char(0x80 | (unit & 0x3F)));
        } else {
          text.push_back(char(0xE0 | (unit >> 12)));
          text.push_back(char(0x80 | ((unit >> 6) & 0x3F)));
          text.push_back(char(0x80 | (unit & 0x3F)));
        }
      }
      texts.push_back(text);
      atoms.push_back(b.getInt32(literal->atom()));
    }
    llvm::Value *index = b.CreateCall(
        lowering_.unknownLowerer().unknownFunction("tz_reflect_fill_atom", i32, {ptr, ptr, i32}),
        {u, fillNames(texts, "tz.fill.atoms"), b.getInt32(uint32_t(texts.size()))});
    pass(b.CreateICmpSGE(index, b.getInt32(0)), mismatch);
    auto *atomType = llvm::ArrayType::get(i32, atoms.size());
    auto *table = new llvm::GlobalVariable(
        *lowering_.module_, atomType, true, llvm::GlobalValue::PrivateLinkage,
        llvm::ConstantArray::get(atomType, atoms), "tz.fill.atom");
    return b.CreateLoad(i32, b.CreateInBoundsGEP(atomType, table, {b.getInt32(0), index}));
  }
  bool literals = type->isLiteral() ||
                  (type->isUnion() && !type->isTagged() && !type->members().empty() &&
                   llvm::all_of(type->members(), [](const Type *m) { return m->isLiteral(); }));
  if (literals) {
    const Type *stored = UnionLayout::storedType(type);
    llvm::Value *value = fillValue(u, stored, site, mismatch, failed);
    std::vector<const Type *> members;
    if (type->isLiteral()) {
      members.push_back(type);
    } else {
      members.assign(type->members().begin(), type->members().end());
    }
    llvm::Value *any = b.getFalse();
    for (const Type *m : members) {
      llvm::Value *same =
          stored->isFloat()
              ? b.CreateFCmpOEQ(value, llvm::ConstantFP::get(b.getDoubleTy(), m->literalNumber()))
          : stored->isBool()
              ? b.CreateICmpEQ(value, b.getInt1(m->literalNumber() != 0))
              : b.CreateICmpEQ(value, llvm::ConstantInt::getSigned(value->getType(),
                                                                   int64_t(m->literalNumber())));
      any = b.CreateOr(any, same);
    }
    pass(any, mismatch);
    return value;
  }
  if (type->isArray() && !type->isTuple() && !type->inner()->isUnknown()) {
    llvm::Function *convert = fillElement(type->inner(), site);
    std::string element = type->inner()->name().str();
    llvm::Value *array = b.CreateCall(
        lowering_.unknownLowerer().unknownFunction(
            "tz_reflect_fill_array", ptr, {ptr, ptr, ptr, ptr, i32, ptr, i32, ptr, i32, ptr}),
        {u, lowering_.typeDescriptors().typeDescriptor(type), convert, site.cls, site.clsLength,
         site.field, site.fieldLength, lowering_.stringLowerer().utf8Constant(element),
         b.getInt32(uint32_t(element.size())), site.maker});
    pass(b.CreateIsNotNull(array), failed);
    return array;
  }
  if (type->isInteger() || type->isFloat() || type->isString() || type->isBool() ||
      type->isDecimal()) {
    llvm::Value *exact = b.CreateCall(
        lowering_.unknownLowerer().unknownFunction("tz_reflect_fill_exact", i32, {ptr, i32}),
        {u, b.getInt32(TypeDescriptors::descriptorKind(type))});
    pass(b.CreateICmpNE(exact, b.getInt32(0)), mismatch);
  }

  return lowering_.unknownCastLowerer().emitUnknownAs(u, type, mismatch);
}

void ClassInfoLowerer::emitFillThunk(const ClassDecl *cls, llvm::Function *fn,
                                     const FillErrors &errors) {
  llvm::Type *ptr = lowering_.builder_.getPtrTy(), *i32 = lowering_.builder_.getInt32Ty();
  auto &b = lowering_.builder_;
  lowering_.functionBuilder().beginFunction(fn, nullptr);
  lowering_.loc_ = cls->loc();
  lowering_.functionFrame().pushScope();
  llvm::Value *values = fn->getArg(0);
  llvm::Value *null = llvm::ConstantPointerNull::get(lowering_.builder_.getPtrTy());
  std::string name = className(cls);
  llvm::Value *clsText = lowering_.stringLowerer().utf8Constant(name);
  llvm::Value *clsLength = b.getInt32(uint32_t(name.size()));
  std::vector<const ClassDecl *> chain;
  for (const ClassDecl *c = cls; c; c = c->parent) {
    chain.insert(chain.begin(), c);
  }
  std::vector<const ClassField *> fields;
  std::vector<std::string> names;
  for (const ClassDecl *c : chain) {
    for (const ClassField &f : c->fields) {
      if (!f.isStatic && !f.method && f.resolved && !f.resolved->isError()) {
        fields.push_back(&f);
        names.push_back(f.name.str());
      }
    }
  }
  llvm::Function *typeMaker = fillMaker(errors.type);
  llvm::Function *unknownMaker = fillMaker(errors.unknownField);
  llvm::Value *known =
      b.CreateCall(lowering_.unknownLowerer().unknownFunction("tz_reflect_fill_keys", i32,
                                                              {ptr, ptr, i32, ptr, i32, ptr}),
                   {values, fillNames(names, "tz.fill.names." + name),
                    b.getInt32(uint32_t(names.size())), clsText, clsLength, unknownMaker});
  llvm::BasicBlock *keysBad = lowering_.functionFrame().newBlock("fill.keys.bad");
  llvm::BasicBlock *keysGood = lowering_.functionFrame().newBlock("fill.keys");
  b.CreateCondBr(b.CreateICmpNE(known, b.getInt32(0)), keysGood, keysBad);
  lowering_.functionFrame().switchTo(keysBad);
  lowering_.functionFrame().emitCleanups(0);
  b.CreateRet(null);
  lowering_.functionFrame().switchTo(keysGood);
  llvm::Value *object = lowering_.classLowerer().allocInstance(cls);
  lowering_.classLowerer().emitConstruct(object, cls, nullptr, {});
  for (const ClassField *f : fields) {
    llvm::Value *foundSlot = lowering_.functionFrame().createStackSlot(i32, "fill.found");
    llvm::Value *fieldText = lowering_.stringLowerer().utf8Constant(f->name.str());
    llvm::Value *fieldLength = b.getInt32(uint32_t(f->name.size()));
    llvm::Value *u = b.CreateCall(lowering_.unknownLowerer().unknownFunction(
                                      "tz_reflect_fill_value", ptr, {ptr, ptr, i32, ptr}),
                                  {values, fieldText, fieldLength, foundSlot}, "fill.value");
    llvm::BasicBlock *set = lowering_.functionFrame().newBlock("fill.set");
    llvm::BasicBlock *next = lowering_.functionFrame().newBlock("fill.next");
    b.CreateCondBr(b.CreateICmpNE(b.CreateLoad(i32, foundSlot), b.getInt32(0)), set, next);
    lowering_.functionFrame().switchTo(set);
    llvm::BasicBlock *mismatch = lowering_.functionFrame().newBlock("fill.mismatch");
    llvm::BasicBlock *failed = lowering_.functionFrame().newBlock("fill.failed");
    FillSite site{clsText, clsLength, fieldText, fieldLength, typeMaker};
    llvm::Value *value = fillValue(u, f->resolved, site, mismatch, failed);
    llvm::Value *address = lowering_.objectLayouts().fieldAddress(object, cls->type, f->name);
    lowering_.storeLowerer().storeIn(object, cls->type, address, value, f->resolved);
    b.CreateBr(next);
    lowering_.functionFrame().switchTo(mismatch);
    std::string expected = f->resolved->name().str();
    b.CreateCall(
        lowering_.unknownLowerer().unknownFunction("tz_reflect_fill_mismatch", b.getVoidTy(),
                                                   {ptr, i32, ptr, i32, i32, ptr, i32, ptr, ptr}),
        {clsText, clsLength, fieldText, fieldLength, b.getInt32(uint32_t(-1)),
         lowering_.stringLowerer().utf8Constant(expected), b.getInt32(uint32_t(expected.size())), u,
         typeMaker});
    b.CreateBr(failed);
    lowering_.functionFrame().switchTo(failed);
    lowering_.refCounts().releaseValue(object, cls->type);
    lowering_.functionFrame().emitCleanups(0);
    b.CreateRet(null);
    lowering_.functionFrame().switchTo(next);
  }
  lowering_.functionFrame().emitCleanups(0);
  b.CreateRet(object);
  lowering_.functionFrame().startDeadBlock();
  lowering_.functionFrame().popScope();
  lowering_.functionBuilder().finishFunction();
  emitFillElements();
}

llvm::Constant *ClassInfoLowerer::reflectAdapterInfo(const FunctionDecl *fn, const Type *&type) {
  const FuncSymbol *sym = fn->sym;
  std::vector<const Type *> params{Type::unknown()};
  params.insert(params.end(), sym->params.begin(), sym->params.end());
  type = Type::function(params, sym->minArgs + 1, sym->hasRest, sym->ret, true);
  if (llvm::Constant *found = reflectAdapters_.lookup(fn)) {
    return found;
  }
  llvm::Function *method = lowering_.classDescriptors().methodCode(fn);
  std::string name = method->getName().str();
  auto *adapter = llvm::Function::Create(lowering_.closureLowerer().closureFnType(type),
                                         llvm::GlobalValue::InternalLinkage, name + ".reflect",
                                         lowering_.module_.get());
  lowering_.functionBuilder().setFunctionAttributes(adapter);
  llvm::Constant *info = llvm::ConstantStruct::get(
      lowering_.fnInfoTy_, {adapter, lowering_.stringLowerer().stringLiteral(
                                         std::u16string(fn->name.begin(), fn->name.end()))});
  auto *global =
      new llvm::GlobalVariable(*lowering_.module_, lowering_.fnInfoTy_, true,
                               llvm::GlobalValue::PrivateLinkage, info, name + ".reflect.info");
  reflectAdapters_[fn] = global;

  llvm::IRBuilderBase::InsertPointGuard guard(lowering_.builder_);
  llvm::Function *outer = std::exchange(lowering_.function_, adapter);
  lowering_.builder_.SetInsertPoint(llvm::BasicBlock::Create(lowering_.ctx_, "entry", adapter));
  llvm::Type *ptr = lowering_.builder_.getPtrTy();
  llvm::Value *self = adapter->getArg(1);
  const Type *ret = sym->ret;
  auto finish = [&](llvm::Value *result) {
    lowering_.builder_.CreateCall(lowering_.instanceReleaseFn_, {self});
    if (ret->isVoid()) {
      lowering_.builder_.CreateRetVoid();
    } else {
      lowering_.builder_.CreateRet(result);
    }
  };
  const ClassDecl *cls = fn->cls;
  llvm::BasicBlock *failBlock = llvm::BasicBlock::Create(lowering_.ctx_, "this.wrong", adapter);
  llvm::BasicBlock *testBlock = llvm::BasicBlock::Create(lowering_.ctx_, "this.test", adapter);
  llvm::BasicBlock *callBlock = llvm::BasicBlock::Create(lowering_.ctx_, "this.ok", adapter);
  llvm::Value *layout = lowering_.builder_.CreateCall(
      lowering_.unknownLowerer().unknownFunction("tz_unknown_layout", ptr, {ptr}), {self},
      "layout");
  lowering_.builder_.CreateCondBr(lowering_.builder_.CreateIsNotNull(layout), testBlock, failBlock);
  lowering_.builder_.SetInsertPoint(testBlock);
  llvm::Value *id = lowering_.builder_.CreateLoad(
      lowering_.builder_.getInt32Ty(),
      lowering_.builder_.CreateStructGEP(lowering_.layoutDescTy_, layout, 6), "class");
  unsigned last = std::max(cls->id, cls->lastId);
  llvm::Value *matches = lowering_.builder_.CreateICmpULE(
      lowering_.builder_.CreateSub(id, lowering_.builder_.getInt32(cls->id)),
      lowering_.builder_.getInt32(last - cls->id));
  lowering_.builder_.CreateCondBr(matches, callBlock, failBlock);
  lowering_.builder_.SetInsertPoint(failBlock);
  lowering_.reflectArgsLowerer().emitReflectFail(
      "ERR_INVALID_THIS", className(cls) + "." + fn->name.str() +
                              " was called with a 'this' that is not an instance of " +
                              className(cls));
  for (size_t i = 1; i < params.size(); ++i) {
    lowering_.refCounts().releaseValue(adapter->getArg(unsigned(i + 1)), params[i]);
  }
  finish(ret->isVoid() ? nullptr
                       : llvm::Constant::getNullValue(lowering_.moduleBuilder().lower(ret)));
  lowering_.builder_.SetInsertPoint(callBlock);
  std::vector<llvm::Value *> args{self};
  for (unsigned i = 2; i < adapter->arg_size(); ++i) {
    args.push_back(adapter->getArg(i));
  }
  llvm::CallInst *result = lowering_.builder_.CreateCall(method, args);
  finish(ret->isVoid() ? nullptr : result);
  lowering_.function_ = outer;

  return global;
}

std::vector<const FunctionDecl *> ClassInfoLowerer::reflectVariants(const ClassDecl *cls,
                                                                    llvm::StringRef name) {
  auto overrides = [](const FunctionDecl *fn, const FunctionDecl *base) {
    for (const FunctionDecl *p = fn; p; p = p->overrides) {
      if (p == base) {
        return true;
      }
    }

    return false;
  };
  std::vector<const FunctionDecl *> found;
  for (const ClassDecl *c = cls; c; c = c->parent) {
    std::vector<const FunctionDecl *> own(c->methods.begin(), c->methods.end());
    for (const ClassField &f : c->fields) {
      if (f.method) {
        own.push_back(f.method);
      }
    }
    for (const FunctionDecl *fn : own) {
      if (fn->name == name && !fn->isStatic && !fn->isAbstract && fn->typeParams.empty() &&
          fn->sym && fn->sym->ret &&
          llvm::none_of(found, [&](const FunctionDecl *v) { return overrides(v, fn); })) {
        found.push_back(fn);
      }
    }
  }

  return found;
}

}
