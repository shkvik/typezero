#include "compiler/lower/classes/decorator_lowerer/decorator_lowerer.h"
#include "compiler/library/process/process_lowerer/process_lowerer.h"
#include "compiler/library/regex/matcher_builder/matcher_builder.h"
#include "compiler/lower/async/async_lowerer/async_lowerer.h"
#include "compiler/lower/classes/class_descriptors/class_descriptors.h"
#include "compiler/lower/classes/class_lowerer/class_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
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

using llvm::dyn_cast;

namespace tz {

DecoratorLowerer::DecoratorLowerer(Lowering &owner)
    : lowering(owner) {}

void DecoratorLowerer::emitClassDeclaration(ClassDecl *cls) {
  if (cls->baseExpr) {
    this->lowering.stmtLowerer().emitDiscarded(cls->baseExpr);
  }
  this->lowering.classLowerer().emitStaticFields(cls);
  for (Stmt *stmt : cls->decorations) {
    this->lowering.stmtLowerer().emitStmt(stmt);
  }
}

llvm::Value *DecoratorLowerer::emitClassExpr(ClassExpr *expr) {
  ClassDecl *cls = expr->decl;
  if (cls->baseExpr) {
    this->lowering.stmtLowerer().emitDiscarded(cls->baseExpr);
  }
  auto initialized = [](const ClassField &f) { return f.isStatic() && f.var() && f.init; };
  if (!cls->decorations.empty() || llvm::any_of(cls->fields, initialized)) {
    auto *done = new llvm::GlobalVariable(
        *this->lowering.module_, this->lowering.builder_.getInt1Ty(), false,
        llvm::GlobalValue::InternalLinkage, this->lowering.builder_.getFalse(), "tz.classinit");
    llvm::BasicBlock *initBlock = this->lowering.functionFrame().newBlock("class.init");
    llvm::BasicBlock *doneBlock = this->lowering.functionFrame().newBlock("class.ready");
    this->lowering.builder_.CreateCondBr(
        this->lowering.builder_.CreateLoad(this->lowering.builder_.getInt1Ty(), done), doneBlock,
        initBlock);
    this->lowering.functionFrame().switchTo(initBlock);
    this->lowering.builder_.CreateStore(this->lowering.builder_.getTrue(), done);
    this->lowering.classLowerer().emitStaticFields(cls);
    for (Stmt *stmt : cls->decorations) {
      this->lowering.stmtLowerer().emitStmt(stmt);
    }
    this->lowering.builder_.CreateBr(doneBlock);
    this->lowering.functionFrame().switchTo(doneBlock);
  }

  return this->lowering.unknownLowerer().classObject(cls);
}

llvm::Constant *DecoratorLowerer::implAdapterInfo(FunctionDecl *fn) {
  llvm::Constant *found = this->adapterInfos.lookup(fn);
  if (found) {
    return found;
  }
  const Type *implType = fn->impl()->type;
  llvm::Function *original = this->originalCode(fn);
  std::string name = this->lowering.classDescriptors().methodCode(fn)->getName().str();
  auto *adapter = llvm::Function::Create(this->lowering.closureLowerer().closureFnType(implType),
                                         llvm::GlobalValue::InternalLinkage, name + ".adapter",
                                         this->lowering.module_.get());
  this->lowering.functionBuilder().setFunctionAttributes(adapter);
  llvm::Constant *info = llvm::ConstantStruct::get(
      this->lowering.fnInfoTy_, {adapter, this->lowering.stringLowerer().stringLiteral(
                                              std::u16string(fn->name.begin(), fn->name.end()))});
  auto *global = new llvm::GlobalVariable(*this->lowering.module_, this->lowering.fnInfoTy_, true,
                                          llvm::GlobalValue::PrivateLinkage, info, name + ".info");
  this->adapterInfos[fn] = global;

  llvm::IRBuilderBase::InsertPointGuard guard(this->lowering.builder_);
  this->lowering.builder_.SetInsertPoint(
      llvm::BasicBlock::Create(this->lowering.ctx_, "entry", adapter));
  llvm::Type *ptr = this->lowering.builder_.getPtrTy();
  llvm::Value *self = adapter->getArg(1);
  const Type *ret = implType->ret();
  auto finish = [&](llvm::Value *result) {
    this->lowering.builder_.CreateCall(this->lowering.instanceReleaseFn_, {self});
    if (ret->isVoid()) {
      this->lowering.builder_.CreateRetVoid();
    } else {
      this->lowering.builder_.CreateRet(result);
    }
  };
  const ClassDecl *cls = fn->cls();
  if (!fn->isStatic()) {
    auto *failBlock = llvm::BasicBlock::Create(this->lowering.ctx_, "this.wrong", adapter);
    auto *testBlock = llvm::BasicBlock::Create(this->lowering.ctx_, "this.test", adapter);
    auto *callBlock = llvm::BasicBlock::Create(this->lowering.ctx_, "this.ok", adapter);
    this->lowering.builder_.CreateCondBr(this->lowering.builder_.CreateIsNotNull(self), testBlock,
                                         failBlock);
    this->lowering.builder_.SetInsertPoint(testBlock);
    llvm::Value *layout =
        this->lowering.builder_.CreateLoad(ptr,
                                           this->lowering.builder_.CreateConstInBoundsGEP1_64(
                                               this->lowering.builder_.getInt8Ty(), self, 8),
                                           "layout");
    llvm::Value *id = this->lowering.builder_.CreateLoad(
        this->lowering.builder_.getInt32Ty(),
        this->lowering.builder_.CreateStructGEP(this->lowering.layoutDescTy_, layout, 6), "class");
    unsigned last = std::max(cls->id, cls->lastId);
    llvm::Value *matches = this->lowering.builder_.CreateICmpULE(
        this->lowering.builder_.CreateSub(id, this->lowering.builder_.getInt32(cls->id)),
        this->lowering.builder_.getInt32(last - cls->id));
    this->lowering.builder_.CreateCondBr(matches, callBlock, failBlock);
    this->lowering.builder_.SetInsertPoint(failBlock);
    this->lowering.processLowerer().emitPanic(
        "TypeError: " + cls->name.str() + "." + fn->name.str() +
        " was called with a 'this' that is not an instance of " + cls->name.str());
    this->lowering.builder_.SetInsertPoint(callBlock);
  }
  std::vector<llvm::Value *> args;
  if (!fn->isStatic()) {
    args.push_back(self);
  }
  for (unsigned i = 2; i < adapter->arg_size(); ++i) {
    args.push_back(adapter->getArg(i));
  }
  llvm::CallInst *result = this->lowering.builder_.CreateCall(original, args);
  finish(ret->isVoid() ? nullptr : result);

  return global;
}

llvm::Function *DecoratorLowerer::originalCode(FunctionDecl *fn) {
  llvm::Function *&original = this->originals[fn];
  if (!original) {
    llvm::Function *stub = this->lowering.classDescriptors().methodCode(fn);
    original = llvm::Function::Create(stub->getFunctionType(), llvm::GlobalValue::InternalLinkage,
                                      stub->getName() + ".original", this->lowering.module_.get());
    this->lowering.functionBuilder().setFunctionAttributes(original);
  }

  return original;
}

void DecoratorLowerer::emitDecoratedMethod(FunctionDecl *fn) {
  llvm::Function *original = this->originalCode(fn);
  if (fn->isGenerator) {
    this->lowering.generatorLowerer().emitGenerator(fn, original);
  } else if (fn->isAsync) {
    this->lowering.asyncLowerer().emitAsyncFunction(fn, original);
  } else {
    this->lowering.functionBuilder().beginFunction(original, fn);
    std::vector<llvm::Value *> args;
    unsigned first = 0;
    if (fn->takesThis()) {
      this->lowering.envParam_ = original->getArg(0);
      this->lowering.envParam_->setName("this");
      first = 1;
    }
    for (unsigned i = first; i < original->arg_size(); ++i) {
      args.push_back(original->getArg(i));
    }
    this->lowering.functionBuilder().emitParamsAndBody(fn, args);
    this->lowering.functionBuilder().finishFunction();
  }

  llvm::Function *stub = this->lowering.classDescriptors().methodCode(fn);
  const Type *implType = fn->impl()->type;
  llvm::IRBuilderBase::InsertPointGuard guard(this->lowering.builder_);
  this->lowering.builder_.SetInsertPoint(
      llvm::BasicBlock::Create(this->lowering.ctx_, "entry", stub));
  unsigned first = fn->takesThis() ? 1 : 0;
  llvm::Value *self = fn->takesThis() ? static_cast<llvm::Value *>(stub->getArg(0))
                                      : this->lowering.unknownLowerer().classObject(fn->cls());
  this->lowering.groupLowerer().retainObject(self);
  std::vector<llvm::Value *> args{self};
  for (unsigned i = first; i < stub->arg_size(); ++i) {
    args.push_back(stub->getArg(i));
  }
  llvm::Value *impl = this->lowering.builder_.CreateLoad(
      this->lowering.fnTy_, this->lowering.functionFrame().addressOf(fn->impl()), "impl");
  llvm::Value *env = this->lowering.builder_.CreateExtractValue(impl, 1);
  this->lowering.builder_.CreateCall(this->lowering.objRetainFn_, {env});
  llvm::Value *result = this->lowering.closureLowerer().callFunctionValue(impl, implType, args);
  this->lowering.builder_.CreateCall(this->lowering.objReleaseFn_, {env});
  if (result) {
    this->lowering.builder_.CreateRet(result);
  } else {
    this->lowering.builder_.CreateRetVoid();
  }
}

llvm::Value *DecoratorLowerer::emitMetaCall(CallExpr *call) {
  this->usesMetadata = true;
  llvm::Type *ptr = this->lowering.builder_.getPtrTy(), *i32 = this->lowering.builder_.getInt32Ty(),
             *i64 = this->lowering.builder_.getInt64Ty();
  bool define = call->builtin() == Builtin::MetaDefine;
  std::vector<llvm::Value *> values = this->lowering.exprLowerer().emitExprs(call->args);
  llvm::Value *key = values[0];
  llvm::Value *target = values[define ? 2 : 1];
  llvm::Value *property = values[define ? 3 : 2];
  const Type *propertyType = call->args[define ? 3 : 2]->type;
  llvm::Value *keyId = call->metaKey
                           ? this->metaKeyConstant(call->metaKey)
                           : llvm::ConstantPointerNull::get(this->lowering.builder_.getPtrTy());
  llvm::Value *has = this->lowering.optionalLowerer().optionalHas(property, propertyType);
  llvm::Value *text = this->lowering.optionalLowerer().optionalValue(property, propertyType);
  std::vector<llvm::Value *> args{keyId, this->lowering.stringLowerer().stringObj(key),
                                  this->lowering.stringLowerer().stringMeta(key)};
  llvm::Value *result = nullptr;
  switch (call->builtin()) {
    case Builtin::MetaDefine:
      args.insert(args.end(), {values[1], target, this->lowering.stringLowerer().stringObj(text),
                               this->lowering.stringLowerer().stringMeta(text),
                               this->lowering.builder_.CreateZExt(has, i32),
                               this->lowering.builder_.getInt32(call->designRecord)});
      this->lowering.builder_.CreateCall(this->lowering.unknownLowerer().unknownFunction(
                                             "tz_meta_define", this->lowering.builder_.getVoidTy(),
                                             {ptr, ptr, i64, ptr, ptr, ptr, i64, i32, i32}),
                                         args);
      break;
    case Builtin::MetaDelete:
      args.insert(args.end(), {target, this->lowering.stringLowerer().stringObj(text),
                               this->lowering.stringLowerer().stringMeta(text),
                               this->lowering.builder_.CreateZExt(has, i32)});
      result = this->lowering.builder_.CreateICmpNE(
          this->lowering.builder_.CreateCall(
              this->lowering.unknownLowerer().unknownFunction("tz_meta_delete", i32,
                                                              {ptr, ptr, i64, ptr, ptr, i64, i32}),
              args),
          this->lowering.builder_.getInt32(0));
      break;
    default: {
      bool own = call->builtin() == Builtin::MetaGetOwn || call->builtin() == Builtin::MetaHasOwn;
      args.insert(args.end(), {target, this->lowering.stringLowerer().stringObj(text),
                               this->lowering.stringLowerer().stringMeta(text),
                               this->lowering.builder_.CreateZExt(has, i32),
                               this->lowering.builder_.getInt32(own)});
      bool get = call->builtin() == Builtin::MetaGet || call->builtin() == Builtin::MetaGetOwn;
      llvm::Value *found = this->lowering.builder_.CreateCall(
          this->lowering.unknownLowerer().unknownFunction(get ? "tz_meta_get" : "tz_meta_has",
                                                          get ? ptr : i32,
                                                          {ptr, ptr, i64, ptr, ptr, i64, i32, i32}),
          args, "record");
      if (!get) {
        result = this->lowering.builder_.CreateICmpNE(found, this->lowering.builder_.getInt32(0));
        break;
      }
      if (call->type->isUnknown()) {
        result = found;
        break;
      }
      const Type *type = call->type->nonOptional();
      llvm::BasicBlock *readBlock = this->lowering.functionFrame().newBlock("record.read");
      llvm::BasicBlock *endBlock = this->lowering.functionFrame().newBlock("record.end");
      llvm::BasicBlock *entry = this->lowering.builder_.GetInsertBlock();
      this->lowering.builder_.CreateCondBr(this->lowering.builder_.CreateIsNotNull(found),
                                           readBlock, endBlock);
      this->lowering.functionFrame().switchTo(readBlock);
      llvm::Value *slot = this->lowering.functionFrame().createStackSlot(
          this->lowering.moduleBuilder().lower(type), "record.value");
      this->lowering.builder_.CreateCall(
          this->lowering.unknownLowerer().unknownFunction(
              "tz_unknown_unbox", this->lowering.builder_.getVoidTy(), {ptr, ptr, ptr}),
          {found, this->lowering.typeDescriptors().typeDescriptor(type), slot});
      llvm::Value *value = this->lowering.optionalLowerer().makeOptional(
          this->lowering.builder_.CreateLoad(this->lowering.moduleBuilder().lower(type), slot),
          this->lowering.builder_.getTrue(), call->type);
      this->lowering.builder_.CreateCall(this->lowering.instanceReleaseFn_, {found});
      llvm::BasicBlock *readEnd = this->lowering.builder_.GetInsertBlock();
      this->lowering.builder_.CreateBr(endBlock);
      this->lowering.functionFrame().switchTo(endBlock);
      llvm::PHINode *phi =
          this->lowering.builder_.CreatePHI(this->lowering.moduleBuilder().lower(call->type), 2);
      phi->addIncoming(this->lowering.optionalLowerer().undefinedValue(call->type), entry);
      phi->addIncoming(value, readEnd);
      result = phi;
      break;
    }
  }
  this->lowering.refCounts().releaseValue(key, Type::string());
  this->lowering.refCounts().releaseValue(target, Type::anyObject());
  this->lowering.refCounts().releaseValue(property, propertyType);

  return result;
}

llvm::Constant *DecoratorLowerer::metaKeyConstant(const void *key) {
  llvm::Constant *&global = this->metaKeys[key];
  if (!global) {
    global = new llvm::GlobalVariable(*this->lowering.module_, this->lowering.builder_.getInt8Ty(),
                                      true, llvm::GlobalValue::PrivateLinkage,
                                      this->lowering.builder_.getInt8(0), "tz.meta.key");
  }

  return global;
}

void DecoratorLowerer::finishMetadata(SourceFile &file, llvm::Instruction *init) {
  if (!this->usesMetadata) {
    return;
  }
  std::vector<const ClassDecl *> classes;
  unsigned count = 1;
  for (Stmt *item : file.items) {
    auto *cls = dyn_cast<ClassDecl>(item);
    if (cls && !cls->literal()) {
      classes.push_back(cls);
      count = std::max(count, cls->id + 1);
    }
  }
  std::vector<llvm::Constant *> table(
      count, llvm::ConstantPointerNull::get(this->lowering.builder_.getPtrTy()));
  for (const ClassDecl *cls : classes) {
    if (cls->id) {
      table[cls->id] = this->lowering.unknownLowerer().classObject(cls);
    }
  }
  auto *type = llvm::ArrayType::get(this->lowering.builder_.getPtrTy(), count);
  auto *global = new llvm::GlobalVariable(*this->lowering.module_, type, true,
                                          llvm::GlobalValue::PrivateLinkage,
                                          llvm::ConstantArray::get(type, table), "tz.classes");
  llvm::IRBuilder<> b(init->getNextNode());
  b.CreateCall(this->lowering.unknownLowerer().unknownFunction(
                   "tz_meta_init", this->lowering.builder_.getVoidTy(),
                   {this->lowering.builder_.getPtrTy(), this->lowering.builder_.getInt32Ty()}),
               {global, b.getInt32(count)});
}

}
