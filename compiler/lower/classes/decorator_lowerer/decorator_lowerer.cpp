#include "compiler/lower/classes/decorator_lowerer/decorator_lowerer.h"
#include "compiler/library/regex/matcher_builder/matcher_builder.h"
#include "compiler/lower/async/async_lowerer/async_lowerer.h"
#include "compiler/lower/classes/class_descriptors/class_descriptors.h"
#include "compiler/lower/classes/class_lowerer/class_lowerer.h"
#include "compiler/lower/errors/throw_lowerer/throw_lowerer.h"
#include "compiler/lower/function_builder/function_builder.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/functions/closure_lowerer/closure_lowerer.h"
#include "compiler/lower/iterators/generator_lowerer/generator_lowerer.h"
#include "compiler/lower/layouts/type_descriptors/type_descriptors.h"
#include "compiler/lower/memory/group_lowerer/group_lowerer.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/stmt/stmt_lowerer/stmt_lowerer.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/lower/types/unknown_lowerer/unknown_lowerer.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "llvm/ADT/STLExtras.h"

using llvm::BasicBlock;
using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::Value;

namespace tz {

DecoratorLowerer::DecoratorLowerer(Lowering &lowering) : lowering_(lowering) {}

void DecoratorLowerer::emitClassDeclaration(ClassDecl *cls) {
  if (cls->baseExpr) {
    lowering_.stmtLowerer().emitDiscarded(cls->baseExpr);
  }
  lowering_.classLowerer().emitStaticFields(cls);
  for (Stmt *stmt : cls->decorations) {
    lowering_.stmtLowerer().emitStmt(stmt);
  }
}

llvm::Value *DecoratorLowerer::emitClassExpr(ClassExpr *expr) {
  ClassDecl *cls = expr->decl;
  if (cls->baseExpr) {
    lowering_.stmtLowerer().emitDiscarded(cls->baseExpr);
  }
  auto initialized = [](const ClassField &f) { return f.isStatic && f.var && f.init; };
  if (!cls->decorations.empty() || llvm::any_of(cls->fields, initialized)) {
    auto *done = new llvm::GlobalVariable(*lowering_.module_, lowering_.builder_.getInt1Ty(), false,
                                          llvm::GlobalValue::InternalLinkage,
                                          lowering_.builder_.getFalse(), "tz.classinit");
    llvm::BasicBlock *initBlock = lowering_.functionFrame().newBlock("class.init");
    llvm::BasicBlock *doneBlock = lowering_.functionFrame().newBlock("class.ready");
    lowering_.builder_.CreateCondBr(
        lowering_.builder_.CreateLoad(lowering_.builder_.getInt1Ty(), done), doneBlock, initBlock);
    lowering_.functionFrame().switchTo(initBlock);
    lowering_.builder_.CreateStore(lowering_.builder_.getTrue(), done);
    lowering_.classLowerer().emitStaticFields(cls);
    for (Stmt *stmt : cls->decorations) {
      lowering_.stmtLowerer().emitStmt(stmt);
    }
    lowering_.builder_.CreateBr(doneBlock);
    lowering_.functionFrame().switchTo(doneBlock);
  }

  return lowering_.unknownLowerer().classObject(cls);
}

llvm::Constant *DecoratorLowerer::implAdapterInfo(FunctionDecl *fn) {
  if (llvm::Constant *found = adapterInfos_.lookup(fn)) {
    return found;
  }
  const Type *implType = fn->impl->type;
  llvm::Function *original = originalCode(fn);
  std::string name = lowering_.classDescriptors().methodCode(fn)->getName().str();
  auto *adapter = llvm::Function::Create(lowering_.closureLowerer().closureFnType(implType),
                                         llvm::GlobalValue::InternalLinkage, name + ".adapter",
                                         lowering_.module_.get());
  lowering_.functionBuilder().setFunctionAttributes(adapter);
  llvm::Constant *info = llvm::ConstantStruct::get(
      lowering_.fnInfoTy_, {adapter, lowering_.stringLowerer().stringLiteral(
                                         std::u16string(fn->name.begin(), fn->name.end()))});
  auto *global = new llvm::GlobalVariable(*lowering_.module_, lowering_.fnInfoTy_, true,
                                          llvm::GlobalValue::PrivateLinkage, info, name + ".info");
  adapterInfos_[fn] = global;

  llvm::IRBuilderBase::InsertPointGuard guard(lowering_.builder_);
  lowering_.builder_.SetInsertPoint(llvm::BasicBlock::Create(lowering_.ctx_, "entry", adapter));
  llvm::Type *ptr = lowering_.builder_.getPtrTy();
  llvm::Value *self = adapter->getArg(1);
  const Type *ret = implType->ret();
  auto finish = [&](llvm::Value *result) {
    lowering_.builder_.CreateCall(lowering_.instanceReleaseFn_, {self});
    if (ret->isVoid()) {
      lowering_.builder_.CreateRetVoid();
    } else {
      lowering_.builder_.CreateRet(result);
    }
  };
  const ClassDecl *cls = fn->cls;
  if (!fn->isStatic) {
    auto *failBlock = llvm::BasicBlock::Create(lowering_.ctx_, "this.wrong", adapter);
    auto *testBlock = llvm::BasicBlock::Create(lowering_.ctx_, "this.test", adapter);
    auto *callBlock = llvm::BasicBlock::Create(lowering_.ctx_, "this.ok", adapter);
    lowering_.builder_.CreateCondBr(lowering_.builder_.CreateIsNotNull(self), testBlock, failBlock);
    lowering_.builder_.SetInsertPoint(testBlock);
    llvm::Value *layout = lowering_.builder_.CreateLoad(
        ptr, lowering_.builder_.CreateConstInBoundsGEP1_64(lowering_.builder_.getInt8Ty(), self, 8),
        "layout");
    llvm::Value *id = lowering_.builder_.CreateLoad(
        lowering_.builder_.getInt32Ty(),
        lowering_.builder_.CreateStructGEP(lowering_.layoutDescTy_, layout, 6), "class");
    unsigned last = std::max(cls->id, cls->lastId);
    llvm::Value *matches = lowering_.builder_.CreateICmpULE(
        lowering_.builder_.CreateSub(id, lowering_.builder_.getInt32(cls->id)),
        lowering_.builder_.getInt32(last - cls->id));
    lowering_.builder_.CreateCondBr(matches, callBlock, failBlock);
    lowering_.builder_.SetInsertPoint(failBlock);
    lowering_.throwLowerer().emitPanic("TypeError: " + cls->name.str() + "." + fn->name.str() +
                                       " was called with a 'this' that is not an instance of " +
                                       cls->name.str());
    lowering_.builder_.SetInsertPoint(callBlock);
  }
  std::vector<llvm::Value *> args;
  if (!fn->isStatic) {
    args.push_back(self);
  }
  for (unsigned i = 2; i < adapter->arg_size(); ++i) {
    args.push_back(adapter->getArg(i));
  }
  llvm::CallInst *result = lowering_.builder_.CreateCall(original, args);
  finish(ret->isVoid() ? nullptr : result);

  return global;
}

llvm::Function *DecoratorLowerer::originalCode(FunctionDecl *fn) {
  llvm::Function *&original = originals_[fn];
  if (!original) {
    llvm::Function *stub = lowering_.classDescriptors().methodCode(fn);
    original = llvm::Function::Create(stub->getFunctionType(), llvm::GlobalValue::InternalLinkage,
                                      stub->getName() + ".original", lowering_.module_.get());
    lowering_.functionBuilder().setFunctionAttributes(original);
  }

  return original;
}

void DecoratorLowerer::emitDecoratedMethod(FunctionDecl *fn) {
  llvm::Function *original = originalCode(fn);
  if (fn->isGenerator) {
    lowering_.generatorLowerer().emitGenerator(fn, original);
  } else if (fn->isAsync) {
    lowering_.asyncLowerer().emitAsyncFunction(fn, original);
  } else {
    lowering_.functionBuilder().beginFunction(original, fn);
    std::vector<llvm::Value *> args;
    unsigned first = 0;
    if (fn->takesThis()) {
      lowering_.envParam_ = original->getArg(0);
      lowering_.envParam_->setName("this");
      first = 1;
    }
    for (unsigned i = first; i < original->arg_size(); ++i) {
      args.push_back(original->getArg(i));
    }
    lowering_.functionBuilder().emitParamsAndBody(fn, args);
    lowering_.functionBuilder().finishFunction();
  }

  llvm::Function *stub = lowering_.classDescriptors().methodCode(fn);
  const Type *implType = fn->impl->type;
  llvm::IRBuilderBase::InsertPointGuard guard(lowering_.builder_);
  lowering_.builder_.SetInsertPoint(llvm::BasicBlock::Create(lowering_.ctx_, "entry", stub));
  unsigned first = fn->takesThis() ? 1 : 0;
  llvm::Value *self = fn->takesThis() ? static_cast<llvm::Value *>(stub->getArg(0))
                                      : lowering_.unknownLowerer().classObject(fn->cls);
  lowering_.groupLowerer().retainObject(self);
  std::vector<llvm::Value *> args{self};
  for (unsigned i = first; i < stub->arg_size(); ++i) {
    args.push_back(stub->getArg(i));
  }
  llvm::Value *impl = lowering_.builder_.CreateLoad(
      lowering_.fnTy_, lowering_.functionFrame().addressOf(fn->impl), "impl");
  llvm::Value *env = lowering_.builder_.CreateExtractValue(impl, 1);
  lowering_.builder_.CreateCall(lowering_.objRetainFn_, {env});
  llvm::Value *result = lowering_.closureLowerer().callFunctionValue(impl, implType, args);
  lowering_.builder_.CreateCall(lowering_.objReleaseFn_, {env});
  if (result) {
    lowering_.builder_.CreateRet(result);
  } else {
    lowering_.builder_.CreateRetVoid();
  }
}

llvm::Value *DecoratorLowerer::emitMetaCall(CallExpr *call) {
  usesMetadata_ = true;
  llvm::Type *ptr = lowering_.builder_.getPtrTy(), *i32 = lowering_.builder_.getInt32Ty(),
             *i64 = lowering_.builder_.getInt64Ty();
  bool define = call->builtin == Builtin::MetaDefine;
  std::vector<llvm::Value *> values = lowering_.exprLowerer().emitExprs(call->args);
  llvm::Value *key = values[0];
  llvm::Value *target = values[define ? 2 : 1];
  llvm::Value *property = values[define ? 3 : 2];
  const Type *propertyType = call->args[define ? 3 : 2]->type;
  llvm::Value *keyId = call->metaKey
                           ? metaKeyConstant(call->metaKey)
                           : llvm::ConstantPointerNull::get(lowering_.builder_.getPtrTy());
  llvm::Value *has = lowering_.optionalLowerer().optionalHas(property, propertyType);
  llvm::Value *text = lowering_.optionalLowerer().optionalValue(property, propertyType);
  std::vector<llvm::Value *> args{keyId, lowering_.stringLowerer().stringObj(key),
                                  lowering_.stringLowerer().stringMeta(key)};
  llvm::Value *result = nullptr;
  switch (call->builtin) {
  case Builtin::MetaDefine:
    args.insert(args.end(), {values[1], target, lowering_.stringLowerer().stringObj(text),
                             lowering_.stringLowerer().stringMeta(text),
                             lowering_.builder_.CreateZExt(has, i32),
                             lowering_.builder_.getInt32(call->designRecord)});
    lowering_.builder_.CreateCall(
        lowering_.unknownLowerer().unknownFunction("tz_meta_define", lowering_.builder_.getVoidTy(),
                                                   {ptr, ptr, i64, ptr, ptr, ptr, i64, i32, i32}),
        args);
    break;
  case Builtin::MetaDelete:
    args.insert(args.end(), {target, lowering_.stringLowerer().stringObj(text),
                             lowering_.stringLowerer().stringMeta(text),
                             lowering_.builder_.CreateZExt(has, i32)});
    result = lowering_.builder_.CreateICmpNE(
        lowering_.builder_.CreateCall(
            lowering_.unknownLowerer().unknownFunction("tz_meta_delete", i32,
                                                       {ptr, ptr, i64, ptr, ptr, i64, i32}),
            args),
        lowering_.builder_.getInt32(0));
    break;
  default: {
    bool own = call->builtin == Builtin::MetaGetOwn || call->builtin == Builtin::MetaHasOwn;
    args.insert(args.end(),
                {target, lowering_.stringLowerer().stringObj(text),
                 lowering_.stringLowerer().stringMeta(text),
                 lowering_.builder_.CreateZExt(has, i32), lowering_.builder_.getInt32(own)});
    bool get = call->builtin == Builtin::MetaGet || call->builtin == Builtin::MetaGetOwn;
    llvm::Value *found =
        lowering_.builder_.CreateCall(lowering_.unknownLowerer().unknownFunction(
                                          get ? "tz_meta_get" : "tz_meta_has", get ? ptr : i32,
                                          {ptr, ptr, i64, ptr, ptr, i64, i32, i32}),
                                      args, "record");
    if (!get) {
      result = lowering_.builder_.CreateICmpNE(found, lowering_.builder_.getInt32(0));
      break;
    }
    if (call->type->isUnknown()) {
      result = found;
      break;
    }
    const Type *type = call->type->nonOptional();
    llvm::BasicBlock *readBlock = lowering_.functionFrame().newBlock("record.read");
    llvm::BasicBlock *endBlock = lowering_.functionFrame().newBlock("record.end");
    llvm::BasicBlock *entry = lowering_.builder_.GetInsertBlock();
    lowering_.builder_.CreateCondBr(lowering_.builder_.CreateIsNotNull(found), readBlock, endBlock);
    lowering_.functionFrame().switchTo(readBlock);
    llvm::Value *slot = lowering_.functionFrame().createStackSlot(
        lowering_.moduleBuilder().lower(type), "record.value");
    lowering_.builder_.CreateCall(
        lowering_.unknownLowerer().unknownFunction("tz_unknown_unbox",
                                                   lowering_.builder_.getVoidTy(), {ptr, ptr, ptr}),
        {found, lowering_.typeDescriptors().typeDescriptor(type), slot});
    llvm::Value *value = lowering_.optionalLowerer().makeOptional(
        lowering_.builder_.CreateLoad(lowering_.moduleBuilder().lower(type), slot),
        lowering_.builder_.getTrue(), call->type);
    lowering_.builder_.CreateCall(lowering_.instanceReleaseFn_, {found});
    llvm::BasicBlock *readEnd = lowering_.builder_.GetInsertBlock();
    lowering_.builder_.CreateBr(endBlock);
    lowering_.functionFrame().switchTo(endBlock);
    llvm::PHINode *phi =
        lowering_.builder_.CreatePHI(lowering_.moduleBuilder().lower(call->type), 2);
    phi->addIncoming(lowering_.optionalLowerer().undefinedValue(call->type), entry);
    phi->addIncoming(value, readEnd);
    result = phi;
    break;
  }
  }
  lowering_.refCounts().releaseValue(key, Type::string());
  lowering_.refCounts().releaseValue(target, Type::anyObject());
  lowering_.refCounts().releaseValue(property, propertyType);
  if (define && !call->metaKey && !call->designRecord && !isa<StringLitExpr>(call->args[0])) {
  }

  return result;
}

llvm::Constant *DecoratorLowerer::metaKeyConstant(const void *key) {
  llvm::Constant *&global = metaKeys_[key];
  if (!global) {
    global = new llvm::GlobalVariable(*lowering_.module_, lowering_.builder_.getInt8Ty(), true,
                                      llvm::GlobalValue::PrivateLinkage,
                                      lowering_.builder_.getInt8(0), "tz.meta.key");
  }

  return global;
}

void DecoratorLowerer::finishMetadata(SourceFile &file, llvm::Instruction *init) {
  if (!usesMetadata_) {
    return;
  }
  std::vector<const ClassDecl *> classes;
  unsigned count = 1;
  for (Stmt *item : file.items) {
    if (auto *cls = dyn_cast<ClassDecl>(item); cls && !cls->literal) {
      classes.push_back(cls);
      count = std::max(count, cls->id + 1);
    }
  }
  std::vector<llvm::Constant *> table(
      count, llvm::ConstantPointerNull::get(lowering_.builder_.getPtrTy()));
  for (const ClassDecl *cls : classes) {
    if (cls->id) {
      table[cls->id] = lowering_.unknownLowerer().classObject(cls);
    }
  }
  auto *type = llvm::ArrayType::get(lowering_.builder_.getPtrTy(), count);
  auto *global =
      new llvm::GlobalVariable(*lowering_.module_, type, true, llvm::GlobalValue::PrivateLinkage,
                               llvm::ConstantArray::get(type, table), "tz.classes");
  llvm::IRBuilder<> b(init->getNextNode());
  b.CreateCall(lowering_.unknownLowerer().unknownFunction(
                   "tz_meta_init", lowering_.builder_.getVoidTy(),
                   {lowering_.builder_.getPtrTy(), lowering_.builder_.getInt32Ty()}),
               {global, b.getInt32(count)});
}

}
