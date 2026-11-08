#include "compiler/lower/library/reflect/class_info_lowerer/class_info_lowerer.h"
#include "compiler/lower/classes/class_descriptors/class_descriptors.h"
#include "compiler/lower/classes/class_lowerer/class_lowerer.h"
#include "compiler/lower/function_builder/function_builder.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/functions/closure_lowerer/closure_lowerer.h"
#include "compiler/lower/layouts/object_layouts/object_layouts.h"
#include "compiler/lower/layouts/type_descriptors/type_descriptors.h"
#include "compiler/lower/library/reflect/reflect_args_lowerer/reflect_args_lowerer.h"
#include "compiler/lower/library/regex/matcher_builder/matcher_builder.h"
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

namespace tz {

std::string ClassInfoLowerer::className(const ClassDecl *cls) {
  return cls->name.empty() && cls->type ? cls->type->name().str() : cls->name.str();
}

const std::vector<MakerFields> &ClassInfoLowerer::fillOrders() {
  static const std::vector<MakerFields> orders = {
      {nullptr, {"code", "details"}},
      {nullptr, {"message", "className", "field", "expected", "got", "value"}},
      {nullptr, {"message", "className", "field", "fields"}},
      {nullptr, {"message", "className"}},
  };

  return orders;
}

std::vector<FunctionDecl *> ClassInfoLowerer::constructorsOf(const ClassDecl *cls) {
  for (const ClassDecl *c = cls; c; c = c->parent) {
    if (!c->constructors.empty()) {
      return c->constructors;
    }
  }

  return {};
}

ClassInfoLowerer::ClassInfoLowerer(Lowering &owner)
    : lowering(owner) {}

llvm::StructType *ClassInfoLowerer::classInfoType() {
  if (!this->classInfoTy) {
    llvm::Type *ptr = this->lowering.builder.getPtrTy();
    this->classInfoTy = llvm::StructType::create(
        this->lowering.ctx,
        {ptr, ptr, this->lowering.builder.getInt32Ty(), ptr, this->lowering.builder.getInt32Ty()},
        "tz.classinfo");
  }

  return this->classInfoTy;
}

llvm::Constant *ClassInfoLowerer::classInfoGlobal(const ClassDecl *cls) {
  llvm::GlobalVariable *&global = this->lowering.classInfos[cls];
  if (!global) {
    global = new llvm::GlobalVariable(*this->lowering.module_, this->classInfoType(), true,
                                      llvm::GlobalValue::PrivateLinkage,
                                      llvm::Constant::getNullValue(this->classInfoType()),
                                      "tz.classinfo." + this->lowering.symbolPrefixes.lookup(cls) +
                                          ClassInfoLowerer::className(cls));
  }

  return global;
}

void ClassInfoLowerer::emitConstructThunk(const ClassDecl *cls, llvm::Function *fn) {
  llvm::Type *ptr = this->lowering.builder.getPtrTy();
  llvm::Type *i32 = this->lowering.builder.getInt32Ty();
  this->lowering.functionBuilder().beginFunction(fn, nullptr);
  this->lowering.loc_ = cls->loc();
  this->lowering.functionFrame().pushScope();
  llvm::Value *args = fn->getArg(0);
  llvm::Value *count = fn->getArg(1);
  std::string what = "new " + ClassInfoLowerer::className(cls);
  llvm::Value *null = llvm::ConstantPointerNull::get(this->lowering.builder.getPtrTy());
  auto fail = [&] {
    this->lowering.functionFrame().emitCleanups(0);
    this->lowering.builder.CreateRet(null);
  };
  std::vector<FunctionDecl *> ctors = ClassInfoLowerer::constructorsOf(cls);
  bool precise = ctors.size() <= 1;
  llvm::BasicBlock *none =
      precise ? nullptr : this->lowering.functionFrame().newBlock("reflect.none");
  llvm::BasicBlock *several =
      precise ? nullptr : this->lowering.functionFrame().newBlock("reflect.several");
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
        this->lowering.reflectArgsLowerer().emitChooseVariant(choices, nullptr, args, count);
    llvm::SwitchInst *which =
        this->lowering.builder.CreateSwitch(chosen, none, unsigned(ctors.size() + 1));
    which->addCase(this->lowering.builder.getInt32(uint32_t(-2)), several);
    for (size_t v = 0; v < ctors.size(); ++v) {
      starts.push_back(this->lowering.functionFrame().newBlock("reflect.variant"));
      which->addCase(this->lowering.builder.getInt32(uint32_t(v)), starts.back());
    }
  }
  for (size_t v = 0; v < variants.size(); ++v) {
    const FuncSymbol *sym = variants[v];
    if (!precise) {
      this->lowering.functionFrame().switchTo(starts[v]);
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
        this->lowering.builder.CreateICmpUGE(count, this->lowering.builder.getInt32(uint32_t(min)));
    if (!rest) {
      fits = this->lowering.builder.CreateAnd(
          fits, this->lowering.builder.CreateICmpULE(
                    count, this->lowering.builder.getInt32(uint32_t(params.size()))));
    }
    llvm::BasicBlock *arityOk = this->lowering.functionFrame().newBlock("reflect.arity");
    llvm::BasicBlock *arityWrong = this->lowering.functionFrame().newBlock("reflect.arity.wrong");
    this->lowering.builder.CreateCondBr(fits, arityOk, arityWrong);
    this->lowering.functionFrame().switchTo(arityWrong);
    if (precise) {
      this->lowering.builder.CreateCall(
          this->lowering.unknownLowerer().unknownFunction(
              "tz_reflect_arity", this->lowering.builder.getVoidTy(), {ptr, i32, i32, i32, i32}),
          {this->lowering.stringLowerer().utf8Constant(what),
           this->lowering.builder.getInt32(uint32_t(what.size())),
           this->lowering.builder.getInt32(uint32_t(min)),
           this->lowering.builder.getInt32(rest ? -1 : int32_t(params.size())), count});
      fail();
    } else {
      this->lowering.builder.CreateBr(next);
    }
    this->lowering.functionFrame().switchTo(arityOk);
    std::vector<llvm::Value *> values;
    this->lowering.reflectArgsLowerer().emitReflectArgs(
        args, count, min, params, restType, values,
        [&](llvm::Value *i, const Type *expected, llvm::Value *got) {
          if (!precise) {
            this->lowering.builder.CreateBr(next);
            return;
          }
          this->lowering.reflectArgsLowerer().emitReflectMismatch(what, i, expected, got);
          fail();
        });
    llvm::Value *object = this->lowering.classLowerer().allocInstance(cls);
    this->lowering.classLowerer().emitConstruct(object, cls, sym, values);
    llvm::Value *made = object;
    this->lowering.functionFrame().emitCleanups(0);
    this->lowering.builder.CreateRet(made);
  }
  if (none) {
    this->lowering.functionFrame().switchTo(none);
    this->lowering.reflectArgsLowerer().emitReflectFail(
        "ERR_NO_VARIANT", what + ": no constructor of " + ClassInfoLowerer::className(cls) +
                              " takes these arguments");
    fail();
    this->lowering.functionFrame().switchTo(several);
    this->lowering.reflectArgsLowerer().emitReflectFail(
        "ERR_SEVERAL_VARIANTS", what + ": the arguments fit several constructors of " +
                                    ClassInfoLowerer::className(cls) + " equally well");
    fail();
  }
  this->lowering.functionFrame().startDeadBlock();
  this->lowering.functionFrame().popScope();
  this->lowering.functionBuilder().finishFunction();
}

llvm::Function *ClassInfoLowerer::fillMaker(const Type *variant) {
  return this->lowering.objectMaker().maker(variant, ClassInfoLowerer::fillOrders());
}

llvm::Constant *ClassInfoLowerer::fillNames(llvm::ArrayRef<std::string> names,
                                            llvm::StringRef label) {
  llvm::Type *ptr = this->lowering.builder.getPtrTy();
  llvm::Type *i32 = this->lowering.builder.getInt32Ty();
  if (names.empty()) {
    return llvm::ConstantPointerNull::get(this->lowering.builder.getPtrTy());
  }
  auto *entryType = llvm::StructType::get(this->lowering.ctx, {ptr, i32});
  std::vector<llvm::Constant *> entries;
  for (const std::string &name : names) {
    entries.push_back(llvm::ConstantStruct::get(
        entryType, {this->lowering.stringLowerer().utf8Constant(name),
                    this->lowering.builder.getInt32(uint32_t(name.size()))}));
  }
  auto *arrayType = llvm::ArrayType::get(entryType, entries.size());
  auto *global = new llvm::GlobalVariable(*this->lowering.module_, arrayType, true,
                                          llvm::GlobalValue::PrivateLinkage,
                                          llvm::ConstantArray::get(arrayType, entries), label);
  global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);

  return global;
}

llvm::Function *ClassInfoLowerer::fillElement(const Type *element, const FillSite &site) {
  llvm::Function *&made = this->fillElements[element];
  if (made) {
    return made;
  }
  llvm::Type *ptr = this->lowering.builder.getPtrTy();
  llvm::Type *i32 = this->lowering.builder.getInt32Ty();
  made = llvm::Function::Create(
      llvm::FunctionType::get(i32, {ptr, ptr}, false), llvm::GlobalValue::InternalLinkage,
      "tz.fill.element." + element->name().str(), this->lowering.module_.get());
  this->lowering.functionBuilder().setFunctionAttributes(made);
  this->pendingElements.push_back({made, element});
  this->pendingSites.push_back(site);

  return made;
}

void ClassInfoLowerer::emitFillElements() {
  while (!this->pendingElements.empty()) {
    auto [fn, element] = this->pendingElements.back();
    FillSite site = this->pendingSites.back();
    this->pendingElements.pop_back();
    this->pendingSites.pop_back();
    this->lowering.functionBuilder().beginFunction(fn, nullptr);
    this->lowering.functionFrame().pushScope();
    llvm::BasicBlock *bad = this->lowering.functionFrame().newBlock("fill.element.bad");
    llvm::Value *value = this->fillValue(fn->getArg(0), element, site, bad, bad);
    this->lowering.builder.CreateStore(value, fn->getArg(1));
    this->lowering.functionFrame().emitCleanups(0);
    this->lowering.builder.CreateRet(this->lowering.builder.getInt32(1));
    this->lowering.functionFrame().switchTo(bad);
    this->lowering.functionFrame().emitCleanups(0);
    this->lowering.builder.CreateRet(this->lowering.builder.getInt32(0));
    this->lowering.functionFrame().startDeadBlock();
    this->lowering.functionFrame().popScope();
    this->lowering.functionBuilder().finishFunction();
  }
}

llvm::Value *ClassInfoLowerer::fillValue(llvm::Value *u, const Type *type, const FillSite &site,
                                         llvm::BasicBlock *mismatch, llvm::BasicBlock *failed) {
  llvm::Type *ptr = this->lowering.builder.getPtrTy();
  llvm::Type *i32 = this->lowering.builder.getInt32Ty();
  auto &b = this->lowering.builder;
  auto pass = [&](llvm::Value *ok, llvm::BasicBlock *otherwise) {
    llvm::BasicBlock *next = this->lowering.functionFrame().newBlock("fill.ok");
    b.CreateCondBr(ok, next, otherwise);
    this->lowering.functionFrame().switchTo(next);
  };
  if (type->isOptional()) {
    llvm::BasicBlock *none = this->lowering.functionFrame().newBlock("fill.none");
    llvm::BasicBlock *some = this->lowering.functionFrame().newBlock("fill.some");
    llvm::BasicBlock *end = this->lowering.functionFrame().newBlock("fill.optional");
    b.CreateCondBr(b.CreateIsNull(u), none, some);
    this->lowering.functionFrame().switchTo(none);
    llvm::Value *nothing = this->lowering.optionalLowerer().undefinedValue(type);
    llvm::BasicBlock *noneEnd = b.GetInsertBlock();
    b.CreateBr(end);
    this->lowering.functionFrame().switchTo(some);
    llvm::Value *inner = this->fillValue(u, type->inner(), site, mismatch, failed);
    llvm::Value *wrapped = this->lowering.optionalLowerer().makeOptional(inner, b.getTrue(), type);
    llvm::BasicBlock *someEnd = b.GetInsertBlock();
    b.CreateBr(end);
    this->lowering.functionFrame().switchTo(end);
    llvm::PHINode *phi = b.CreatePHI(this->lowering.moduleBuilder().lower(type), 2);
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
        this->lowering.unknownLowerer().unknownFunction("tz_reflect_fill_atom", i32,
                                                        {ptr, ptr, i32}),
        {u, this->fillNames(texts, "tz.fill.atoms"), b.getInt32(uint32_t(texts.size()))});
    pass(b.CreateICmpSGE(index, b.getInt32(0)), mismatch);
    auto *atomType = llvm::ArrayType::get(i32, atoms.size());
    auto *table = new llvm::GlobalVariable(
        *this->lowering.module_, atomType, true, llvm::GlobalValue::PrivateLinkage,
        llvm::ConstantArray::get(atomType, atoms), "tz.fill.atom");
    return b.CreateLoad(i32, b.CreateInBoundsGEP(atomType, table, {b.getInt32(0), index}));
  }
  bool literals = type->isLiteral() ||
                  (type->isUnion() && !type->isTagged() && !type->members().empty() &&
                   llvm::all_of(type->members(), [](const Type *m) { return m->isLiteral(); }));
  if (literals) {
    const Type *stored = UnionLayout::storedType(type);
    llvm::Value *value = this->fillValue(u, stored, site, mismatch, failed);
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
    llvm::Function *convert = this->fillElement(type->inner(), site);
    std::string element = type->inner()->name().str();
    llvm::Value *array = b.CreateCall(
        this->lowering.unknownLowerer().unknownFunction(
            "tz_reflect_fill_array", ptr, {ptr, ptr, ptr, ptr, i32, ptr, i32, ptr, i32, ptr}),
        {u, this->lowering.typeDescriptors().typeDescriptor(type), convert, site.cls,
         site.clsLength, site.field, site.fieldLength,
         this->lowering.stringLowerer().utf8Constant(element), b.getInt32(uint32_t(element.size())),
         site.maker});
    pass(b.CreateIsNotNull(array), failed);
    return array;
  }
  if (type->isInteger() || type->isFloat() || type->isString() || type->isBool() ||
      type->isDecimal()) {
    llvm::Value *exact = b.CreateCall(
        this->lowering.unknownLowerer().unknownFunction("tz_reflect_fill_exact", i32, {ptr, i32}),
        {u, b.getInt32(TypeDescriptors::descriptorKind(type))});
    pass(b.CreateICmpNE(exact, b.getInt32(0)), mismatch);
  }

  return this->lowering.unknownCastLowerer().emitUnknownAs(u, type, mismatch);
}

void ClassInfoLowerer::emitFillThunk(const ClassDecl *cls, llvm::Function *fn,
                                     const FillErrors &errors) {
  llvm::Type *ptr = this->lowering.builder.getPtrTy();
  llvm::Type *i32 = this->lowering.builder.getInt32Ty();
  auto &b = this->lowering.builder;
  this->lowering.functionBuilder().beginFunction(fn, nullptr);
  this->lowering.loc_ = cls->loc();
  this->lowering.functionFrame().pushScope();
  llvm::Value *values = fn->getArg(0);
  llvm::Value *null = llvm::ConstantPointerNull::get(this->lowering.builder.getPtrTy());
  std::string name = ClassInfoLowerer::className(cls);
  llvm::Value *clsText = this->lowering.stringLowerer().utf8Constant(name);
  llvm::Value *clsLength = b.getInt32(uint32_t(name.size()));
  std::vector<const ClassDecl *> chain;
  for (const ClassDecl *c = cls; c; c = c->parent) {
    chain.insert(chain.begin(), c);
  }
  std::vector<const ClassField *> fields;
  std::vector<std::string> names;
  for (const ClassDecl *c : chain) {
    for (const ClassField &f : c->fields) {
      if (!f.isStatic() && !f.method() && f.resolved && !f.resolved->isError()) {
        fields.push_back(&f);
        names.push_back(f.name.str());
      }
    }
  }
  llvm::Function *typeMaker = this->fillMaker(errors.type);
  llvm::Function *unknownMaker = this->fillMaker(errors.unknownField);
  llvm::Value *known =
      b.CreateCall(this->lowering.unknownLowerer().unknownFunction("tz_reflect_fill_keys", i32,
                                                                   {ptr, ptr, i32, ptr, i32, ptr}),
                   {values, this->fillNames(names, "tz.fill.names." + name),
                    b.getInt32(uint32_t(names.size())), clsText, clsLength, unknownMaker});
  llvm::BasicBlock *keysBad = this->lowering.functionFrame().newBlock("fill.keys.bad");
  llvm::BasicBlock *keysGood = this->lowering.functionFrame().newBlock("fill.keys");
  b.CreateCondBr(b.CreateICmpNE(known, b.getInt32(0)), keysGood, keysBad);
  this->lowering.functionFrame().switchTo(keysBad);
  this->lowering.functionFrame().emitCleanups(0);
  b.CreateRet(null);
  this->lowering.functionFrame().switchTo(keysGood);
  llvm::Value *object = this->lowering.classLowerer().allocInstance(cls);
  this->lowering.classLowerer().emitConstruct(object, cls, nullptr, {});
  for (const ClassField *f : fields) {
    llvm::Value *foundSlot = this->lowering.functionFrame().createStackSlot(i32, "fill.found");
    llvm::Value *fieldText = this->lowering.stringLowerer().utf8Constant(f->name.str());
    llvm::Value *fieldLength = b.getInt32(uint32_t(f->name.size()));
    llvm::Value *u = b.CreateCall(this->lowering.unknownLowerer().unknownFunction(
                                      "tz_reflect_fill_value", ptr, {ptr, ptr, i32, ptr}),
                                  {values, fieldText, fieldLength, foundSlot}, "fill.value");
    llvm::BasicBlock *set = this->lowering.functionFrame().newBlock("fill.set");
    llvm::BasicBlock *next = this->lowering.functionFrame().newBlock("fill.next");
    b.CreateCondBr(b.CreateICmpNE(b.CreateLoad(i32, foundSlot), b.getInt32(0)), set, next);
    this->lowering.functionFrame().switchTo(set);
    llvm::BasicBlock *mismatch = this->lowering.functionFrame().newBlock("fill.mismatch");
    llvm::BasicBlock *failed = this->lowering.functionFrame().newBlock("fill.failed");
    FillSite site{clsText, clsLength, fieldText, fieldLength, typeMaker};
    llvm::Value *value = this->fillValue(u, f->resolved, site, mismatch, failed);
    llvm::Value *address = this->lowering.objectLayouts().fieldAddress(object, cls->type, f->name);
    this->lowering.storeLowerer().storeIn(object, cls->type, address, value, f->resolved);
    b.CreateBr(next);
    this->lowering.functionFrame().switchTo(mismatch);
    std::string expected = f->resolved->name().str();
    b.CreateCall(this->lowering.unknownLowerer().unknownFunction(
                     "tz_reflect_fill_mismatch", b.getVoidTy(),
                     {ptr, i32, ptr, i32, i32, ptr, i32, ptr, ptr}),
                 {clsText, clsLength, fieldText, fieldLength, b.getInt32(uint32_t(-1)),
                  this->lowering.stringLowerer().utf8Constant(expected),
                  b.getInt32(uint32_t(expected.size())), u, typeMaker});
    b.CreateBr(failed);
    this->lowering.functionFrame().switchTo(failed);
    this->lowering.refCounts().releaseValue(object, cls->type);
    this->lowering.functionFrame().emitCleanups(0);
    b.CreateRet(null);
    this->lowering.functionFrame().switchTo(next);
  }
  this->lowering.functionFrame().emitCleanups(0);
  b.CreateRet(object);
  this->lowering.functionFrame().startDeadBlock();
  this->lowering.functionFrame().popScope();
  this->lowering.functionBuilder().finishFunction();
  this->emitFillElements();
}

llvm::Constant *ClassInfoLowerer::reflectAdapterInfo(const FunctionDecl *fn, const Type *&type) {
  const FuncSymbol *sym = fn->sym;
  std::vector<const Type *> params{Type::unknown()};
  params.insert(params.end(), sym->params.begin(), sym->params.end());
  type = Type::function(params, sym->minArgs + 1, sym->hasRest, sym->getKnownReturnType(), true);
  if (llvm::Constant *found = this->reflectAdapters.lookup(fn)) {
    return found;
  }
  llvm::Function *method = this->lowering.classDescriptors().methodCode(fn);
  std::string name = method->getName().str();
  auto *adapter = llvm::Function::Create(this->lowering.closureLowerer().closureFnType(type),
                                         llvm::GlobalValue::InternalLinkage, name + ".reflect",
                                         this->lowering.module_.get());
  this->lowering.functionBuilder().setFunctionAttributes(adapter);
  llvm::Constant *info = llvm::ConstantStruct::get(
      this->lowering.fnInfoTy, {adapter, this->lowering.stringLowerer().stringLiteral(
                                             std::u16string(fn->name.begin(), fn->name.end()))});
  auto *global =
      new llvm::GlobalVariable(*this->lowering.module_, this->lowering.fnInfoTy, true,
                               llvm::GlobalValue::PrivateLinkage, info, name + ".reflect.info");
  this->reflectAdapters[fn] = global;

  llvm::IRBuilderBase::InsertPointGuard guard(this->lowering.builder);
  llvm::Function *outer = std::exchange(this->lowering.function, adapter);
  this->lowering.builder.SetInsertPoint(
      llvm::BasicBlock::Create(this->lowering.ctx, "entry", adapter));
  llvm::Type *ptr = this->lowering.builder.getPtrTy();
  llvm::Value *self = adapter->getArg(1);
  const Type *ret = sym->getKnownReturnType();
  auto finish = [&](llvm::Value *result) {
    this->lowering.builder.CreateCall(this->lowering.instanceReleaseFn, {self});
    if (ret->isVoid()) {
      this->lowering.builder.CreateRetVoid();
    } else {
      this->lowering.builder.CreateRet(result);
    }
  };
  const ClassDecl *cls = fn->cls();
  llvm::BasicBlock *failBlock = llvm::BasicBlock::Create(this->lowering.ctx, "this.wrong", adapter);
  llvm::BasicBlock *testBlock = llvm::BasicBlock::Create(this->lowering.ctx, "this.test", adapter);
  llvm::BasicBlock *callBlock = llvm::BasicBlock::Create(this->lowering.ctx, "this.ok", adapter);
  llvm::Value *layout = this->lowering.builder.CreateCall(
      this->lowering.unknownLowerer().unknownFunction("tz_unknown_layout", ptr, {ptr}), {self},
      "layout");
  this->lowering.builder.CreateCondBr(this->lowering.builder.CreateIsNotNull(layout), testBlock,
                                      failBlock);
  this->lowering.builder.SetInsertPoint(testBlock);
  llvm::Value *id = this->lowering.builder.CreateLoad(
      this->lowering.builder.getInt32Ty(),
      this->lowering.builder.CreateStructGEP(this->lowering.layoutDescTy, layout, 6), "class");
  unsigned last = std::max(cls->id, cls->lastId);
  llvm::Value *matches = this->lowering.builder.CreateICmpULE(
      this->lowering.builder.CreateSub(id, this->lowering.builder.getInt32(cls->id)),
      this->lowering.builder.getInt32(last - cls->id));
  this->lowering.builder.CreateCondBr(matches, callBlock, failBlock);
  this->lowering.builder.SetInsertPoint(failBlock);
  this->lowering.reflectArgsLowerer().emitReflectFail(
      "ERR_INVALID_THIS", ClassInfoLowerer::className(cls) + "." + fn->name.str() +
                              " was called with a 'this' that is not an instance of " +
                              ClassInfoLowerer::className(cls));
  for (size_t i = 1; i < params.size(); ++i) {
    this->lowering.refCounts().releaseValue(adapter->getArg(unsigned(i + 1)), params[i]);
  }
  finish(ret->isVoid() ? nullptr
                       : llvm::Constant::getNullValue(this->lowering.moduleBuilder().lower(ret)));
  this->lowering.builder.SetInsertPoint(callBlock);
  std::vector<llvm::Value *> args{self};
  for (unsigned i = 2; i < adapter->arg_size(); ++i) {
    args.push_back(adapter->getArg(i));
  }
  llvm::CallInst *result = this->lowering.builder.CreateCall(method, args);
  finish(ret->isVoid() ? nullptr : result);
  this->lowering.function = outer;

  return global;
}

std::vector<const FunctionDecl *> ClassInfoLowerer::reflectVariants(const ClassDecl *cls,
                                                                    llvm::StringRef name) {
  auto overrides = [](const FunctionDecl *fn, const FunctionDecl *base) {
    for (const FunctionDecl *p = fn; p; p = p->overrides()) {
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
      if (f.method()) {
        own.push_back(f.method());
      }
    }
    for (const FunctionDecl *fn : own) {
      if (fn->name == name && !fn->isStatic() && !fn->isAbstract() && fn->typeParams.empty() &&
          fn->sym && fn->sym->hasReturnType() &&
          llvm::none_of(found, [&](const FunctionDecl *v) { return overrides(v, fn); })) {
        found.push_back(fn);
      }
    }
  }

  return found;
}

}
