#include "compiler/lower/classes/decorator_lowerer/decorator_lowerer.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/function_builder/function_builder.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/lower/stmt/stmt_lowerer/stmt_lowerer.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "compiler/lower/layouts/type_descriptors/type_descriptors.h"
#include "compiler/lower/async/async_lowerer/async_lowerer.h"
#include "compiler/lower/classes/class_descriptors/class_descriptors.h"
#include "compiler/lower/classes/class_lowerer/class_lowerer.h"
#include "compiler/lower/functions/closure_lowerer/closure_lowerer.h"
#include "compiler/lower/memory/group_lowerer/group_lowerer.h"
#include "compiler/lower/iterators/generator_lowerer/generator_lowerer.h"
#include "compiler/lower/errors/throw_lowerer/throw_lowerer.h"
#include "compiler/lower/types/unknown_lowerer/unknown_lowerer.h"
#include "compiler/library/regex/matcher_builder/matcher_builder.h"
#include "llvm/ADT/STLExtras.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::BasicBlock;
using llvm::Value;

namespace tz {

DecoratorLowerer::DecoratorLowerer(CodeGen &codegen) : codegen_(codegen) {}

void DecoratorLowerer::emitClassDeclaration(ClassDecl *cls) {
  // A base that is a value (`extends Mixin(Base)`) is evaluated first.
  if (cls->baseExpr) {
    codegen_.stmtLowerer().emitDiscarded(cls->baseExpr);
  }
  codegen_.classLowerer().emitStaticFields(cls);
  for (Stmt *stmt : cls->decorations) {
    codegen_.stmtLowerer().emitStmt(stmt);
  }
}

// The class of a class expression is one class, made once, however many
// times the expression runs (docs/decorators.md, section 5): its base is
// evaluated each time, and its static fields get their values the first.
llvm::Value *DecoratorLowerer::emitClassExpr(ClassExpr *expr) {
  ClassDecl *cls = expr->decl;
  if (cls->baseExpr) {
    codegen_.stmtLowerer().emitDiscarded(cls->baseExpr);
  }
  auto initialized = [](const ClassField &f) { return f.isStatic && f.var && f.init; };
  if (!cls->decorations.empty() || llvm::any_of(cls->fields, initialized)) {
    auto *done = new llvm::GlobalVariable(*codegen_.module_, codegen_.builder_.getInt1Ty(), /*isConstant=*/false,
                                          llvm::GlobalValue::InternalLinkage, codegen_.builder_.getFalse(),
                                          "tz.classinit");
    llvm::BasicBlock *initBlock = codegen_.functionFrame().newBlock("class.init");
    llvm::BasicBlock *doneBlock = codegen_.functionFrame().newBlock("class.ready");
    codegen_.builder_.CreateCondBr(codegen_.builder_.CreateLoad(codegen_.builder_.getInt1Ty(), done), doneBlock, initBlock);
    codegen_.functionFrame().switchTo(initBlock);
    codegen_.builder_.CreateStore(codegen_.builder_.getTrue(), done);
    codegen_.classLowerer().emitStaticFields(cls);
    for (Stmt *stmt : cls->decorations) {
      codegen_.stmtLowerer().emitStmt(stmt);
    }
    codegen_.builder_.CreateBr(doneBlock);
    codegen_.functionFrame().switchTo(doneBlock);
  }

  return codegen_.unknownLowerer().classObject(cls);
}

llvm::Constant *DecoratorLowerer::implAdapterInfo(FunctionDecl *fn) {
  if (llvm::Constant *found = adapterInfos_.lookup(fn)) {
    return found;
  }
  const Type *implType = fn->impl->type;
  llvm::Function *original = originalCode(fn);
  std::string name = codegen_.classDescriptors().methodCode(fn)->getName().str();
  auto *adapter =
      llvm::Function::Create(codegen_.closureLowerer().closureFnType(implType), llvm::GlobalValue::InternalLinkage,
                             name + ".adapter", codegen_.module_.get());
  codegen_.functionBuilder().setFunctionAttributes(adapter);
  llvm::Constant *info = llvm::ConstantStruct::get(
      codegen_.fnInfoTy_, {adapter, codegen_.stringLowerer().stringLiteral(std::u16string(fn->name.begin(), fn->name.end()))});
  auto *global = new llvm::GlobalVariable(*codegen_.module_, codegen_.fnInfoTy_, /*isConstant=*/true,
                                          llvm::GlobalValue::PrivateLinkage, info, name + ".info");
  adapterInfos_[fn] = global;

  // (env, this: unknown, the parameters) -> R: the `this` must be an instance
  // of the class (a static method takes any), then the body gets the rest.
  llvm::IRBuilderBase::InsertPointGuard guard(codegen_.builder_);
  codegen_.builder_.SetInsertPoint(llvm::BasicBlock::Create(codegen_.ctx_, "entry", adapter));
  llvm::Type *ptr = codegen_.builder_.getPtrTy();
  llvm::Value *self = adapter->getArg(1);
  const Type *ret = implType->ret();
  auto finish = [&](llvm::Value *result) {
    codegen_.builder_.CreateCall(codegen_.instanceReleaseFn_, {self});
    if (ret->isVoid()) {
      codegen_.builder_.CreateRetVoid();
    } else {
      codegen_.builder_.CreateRet(result);
    }
  };
  const ClassDecl *cls = fn->cls;
  if (!fn->isStatic) {
    auto *failBlock = llvm::BasicBlock::Create(codegen_.ctx_, "this.wrong", adapter);
    auto *testBlock = llvm::BasicBlock::Create(codegen_.ctx_, "this.test", adapter);
    auto *callBlock = llvm::BasicBlock::Create(codegen_.ctx_, "this.ok", adapter);
    codegen_.builder_.CreateCondBr(codegen_.builder_.CreateIsNotNull(self), testBlock, failBlock);
    codegen_.builder_.SetInsertPoint(testBlock);
    // The number of its class, which a box and a class do not have.
    llvm::Value *layout = codegen_.builder_.CreateLoad(
        ptr, codegen_.builder_.CreateConstInBoundsGEP1_64(codegen_.builder_.getInt8Ty(), self, 8), "layout");
    llvm::Value *id = codegen_.builder_.CreateLoad(
        codegen_.builder_.getInt32Ty(), codegen_.builder_.CreateStructGEP(codegen_.layoutDescTy_, layout, 6), "class");
    unsigned last = std::max(cls->id, cls->lastId);
    llvm::Value *matches = codegen_.builder_.CreateICmpULE(
        codegen_.builder_.CreateSub(id, codegen_.builder_.getInt32(cls->id)), codegen_.builder_.getInt32(last - cls->id));
    codegen_.builder_.CreateCondBr(matches, callBlock, failBlock);
    codegen_.builder_.SetInsertPoint(failBlock);
    std::string message = cls->name.str() + "." + fn->name.str() +
                          " was called with a 'this' that is not an instance of " + cls->name.str();
    llvm::Value *text = codegen_.stringLowerer().stringLiteral(std::u16string(message.begin(), message.end()));
    codegen_.builder_.CreateCall(codegen_.unknownLowerer().unknownFunction("tz_throw_str", codegen_.builder_.getVoidTy(),
                                        {codegen_.builder_.getInt32Ty(), ptr, codegen_.builder_.getInt64Ty()}),
                        {codegen_.builder_.getInt32(1), codegen_.stringLowerer().stringObj(text), codegen_.stringLowerer().stringMeta(text)});
    // The arguments go with the call that does not happen.
    for (size_t i = 1; i < implType->params().size(); ++i) {
      codegen_.refCounts().releaseValue(adapter->getArg(unsigned(i + 1)), implType->params()[i]);
    }
    finish(ret->isVoid() ? nullptr : llvm::Constant::getNullValue(codegen_.moduleBuilder().lower(ret)));
    codegen_.builder_.SetInsertPoint(callBlock);
  }
  std::vector<llvm::Value *> args;
  if (!fn->isStatic) {
    args.push_back(self); // borrowed, as a method takes its object
  }
  for (unsigned i = 2; i < adapter->arg_size(); ++i) {
    args.push_back(adapter->getArg(i));
  }
  llvm::CallInst *result = codegen_.builder_.CreateCall(original, args);
  finish(ret->isVoid() ? nullptr : result);

  return global;
}

llvm::Function *DecoratorLowerer::originalCode(FunctionDecl *fn) {
  llvm::Function *&original = originals_[fn];
  if (!original) {
    llvm::Function *stub = codegen_.classDescriptors().methodCode(fn);
    original = llvm::Function::Create(stub->getFunctionType(), llvm::GlobalValue::InternalLinkage,
                                      stub->getName() + ".original", codegen_.module_.get());
    codegen_.functionBuilder().setFunctionAttributes(original);
  }

  return original;
}

void DecoratorLowerer::emitDecoratedMethod(FunctionDecl *fn) {
  // The body, as any method's or static method's.
  llvm::Function *original = originalCode(fn);
  if (fn->isGenerator) {
    codegen_.generatorLowerer().emitGenerator(fn, original);
  } else if (fn->isAsync) { // a coroutine, whose promise the stub gives
    codegen_.asyncLowerer().emitAsyncFunction(fn, original);
  } else {
    codegen_.functionBuilder().beginFunction(original, fn);
    std::vector<llvm::Value *> args;
    unsigned first = 0;
    if (fn->takesThis()) {
      codegen_.envParam_ = original->getArg(0);
      codegen_.envParam_->setName("this");
      first = 1;
    }
    for (unsigned i = first; i < original->arg_size(); ++i) {
      args.push_back(original->getArg(i));
    }
    codegen_.functionBuilder().emitParamsAndBody(fn, args);
    codegen_.functionBuilder().finishFunction();
  }

  // The stub: a call of what the global holds, with the object (or the
  // class) as its `this`, and the arguments as they are.
  llvm::Function *stub = codegen_.classDescriptors().methodCode(fn);
  const Type *implType = fn->impl->type;
  llvm::IRBuilderBase::InsertPointGuard guard(codegen_.builder_);
  codegen_.builder_.SetInsertPoint(llvm::BasicBlock::Create(codegen_.ctx_, "entry", stub));
  unsigned first = fn->takesThis() ? 1 : 0;
  llvm::Value *self =
      fn->takesThis() ? static_cast<llvm::Value *>(stub->getArg(0)) : codegen_.unknownLowerer().classObject(fn->cls);
  codegen_.groupLowerer().retainObject(self); // the callee owns its `this`
  std::vector<llvm::Value *> args{self};
  for (unsigned i = first; i < stub->arg_size(); ++i) {
    args.push_back(stub->getArg(i));
  }
  // Kept alive during the call, which may replace it.
  llvm::Value *impl = codegen_.builder_.CreateLoad(codegen_.fnTy_, codegen_.functionFrame().addressOf(fn->impl), "impl");
  llvm::Value *env = codegen_.builder_.CreateExtractValue(impl, 1);
  codegen_.builder_.CreateCall(codegen_.objRetainFn_, {env});
  llvm::Value *result = codegen_.closureLowerer().callFunctionValue(impl, implType, args);
  codegen_.builder_.CreateCall(codegen_.objReleaseFn_, {env});
  if (result) {
    codegen_.builder_.CreateRet(result);
  } else {
    codegen_.builder_.CreateRetVoid();
  }
}

// Metadata (runtime/library/reflect/meta/meta.c): the key as an address of a
// constant of its own for an object key, or a string; values go in as unknowns,
// and records of an object key come out as the type Sema found for them.
llvm::Value *DecoratorLowerer::emitMetaCall(CallExpr *call) {
  usesMetadata_ = true;
  llvm::Type *ptr = codegen_.builder_.getPtrTy(), *i32 = codegen_.builder_.getInt32Ty(), *i64 = codegen_.builder_.getInt64Ty();
  bool define = call->builtin == Builtin::MetaDefine;
  std::vector<llvm::Value *> values = codegen_.throwLowerer().emitHeldValues(call->args);
  llvm::Value *key = values[0];
  llvm::Value *target = values[define ? 2 : 1];
  llvm::Value *property = values[define ? 3 : 2];
  const Type *propertyType = call->args[define ? 3 : 2]->type;
  llvm::Value *keyId = call->metaKey ? metaKeyConstant(call->metaKey)
                                     : llvm::ConstantPointerNull::get(codegen_.builder_.getPtrTy());
  llvm::Value *has = codegen_.optionalLowerer().optionalHas(property, propertyType);
  llvm::Value *text = codegen_.optionalLowerer().optionalValue(property, propertyType);
  std::vector<llvm::Value *> args{keyId, codegen_.stringLowerer().stringObj(key), codegen_.stringLowerer().stringMeta(key)};
  llvm::Value *result = nullptr;
  switch (call->builtin) {
  case Builtin::MetaDefine:
    args.insert(args.end(), {values[1], target, codegen_.stringLowerer().stringObj(text), codegen_.stringLowerer().stringMeta(text),
                             codegen_.builder_.CreateZExt(has, i32), codegen_.builder_.getInt32(call->designRecord)});
    codegen_.builder_.CreateCall(codegen_.unknownLowerer().unknownFunction("tz_meta_define", codegen_.builder_.getVoidTy(),
                                        {ptr, ptr, i64, ptr, ptr, ptr, i64, i32, i32}),
                        args);
    break;
  case Builtin::MetaDelete:
    args.insert(args.end(),
                {target, codegen_.stringLowerer().stringObj(text), codegen_.stringLowerer().stringMeta(text), codegen_.builder_.CreateZExt(has, i32)});
    result = codegen_.builder_.CreateICmpNE(
        codegen_.builder_.CreateCall(
            codegen_.unknownLowerer().unknownFunction("tz_meta_delete", i32, {ptr, ptr, i64, ptr, ptr, i64, i32}), args),
        codegen_.builder_.getInt32(0));
    break;
  default: {
    bool own = call->builtin == Builtin::MetaGetOwn || call->builtin == Builtin::MetaHasOwn;
    args.insert(args.end(), {target, codegen_.stringLowerer().stringObj(text), codegen_.stringLowerer().stringMeta(text),
                             codegen_.builder_.CreateZExt(has, i32), codegen_.builder_.getInt32(own)});
    bool get = call->builtin == Builtin::MetaGet || call->builtin == Builtin::MetaGetOwn;
    llvm::Value *found =
        codegen_.builder_.CreateCall(codegen_.unknownLowerer().unknownFunction(get ? "tz_meta_get" : "tz_meta_has", get ? ptr : i32,
                                            {ptr, ptr, i64, ptr, ptr, i64, i32, i32}),
                            args, "record");
    if (!get) {
      result = codegen_.builder_.CreateICmpNE(found, codegen_.builder_.getInt32(0));
      break;
    }
    if (call->type->isUnknown()) {
      result = found;
      break;
    }
    // A record of the type Sema knows it has, or undefined.
    const Type *type = call->type->nonOptional();
    llvm::BasicBlock *readBlock = codegen_.functionFrame().newBlock("record.read");
    llvm::BasicBlock *endBlock = codegen_.functionFrame().newBlock("record.end");
    llvm::BasicBlock *entry = codegen_.builder_.GetInsertBlock();
    codegen_.builder_.CreateCondBr(codegen_.builder_.CreateIsNotNull(found), readBlock, endBlock);
    codegen_.functionFrame().switchTo(readBlock);
    llvm::Value *slot = codegen_.functionFrame().createStackSlot(codegen_.moduleBuilder().lower(type), "record.value");
    codegen_.builder_.CreateCall(codegen_.unknownLowerer().unknownFunction("tz_unknown_unbox", codegen_.builder_.getVoidTy(), {ptr, ptr, ptr}),
                        {found, codegen_.typeDescriptors().typeDescriptor(type), slot});
    llvm::Value *value =
        codegen_.optionalLowerer().makeOptional(codegen_.builder_.CreateLoad(codegen_.moduleBuilder().lower(type), slot), codegen_.builder_.getTrue(), call->type);
    codegen_.builder_.CreateCall(codegen_.instanceReleaseFn_, {found});
    llvm::BasicBlock *readEnd = codegen_.builder_.GetInsertBlock();
    codegen_.builder_.CreateBr(endBlock);
    codegen_.functionFrame().switchTo(endBlock);
    llvm::PHINode *phi = codegen_.builder_.CreatePHI(codegen_.moduleBuilder().lower(call->type), 2);
    phi->addIncoming(codegen_.optionalLowerer().undefinedValue(call->type), entry);
    phi->addIncoming(value, readEnd);
    result = phi;
    break;
  }
  }
  codegen_.refCounts().releaseValue(key, Type::string());
  codegen_.refCounts().releaseValue(target, Type::anyObject());
  codegen_.refCounts().releaseValue(property, propertyType);
  if (define && !call->metaKey && !call->designRecord && !isa<StringLitExpr>(call->args[0])) {
    codegen_.throwLowerer().emitThrowCheck(); // a key `design:*` the program wrote
  }

  return result;
}

llvm::Constant *DecoratorLowerer::metaKeyConstant(const void *key) {
  llvm::Constant *&global = metaKeys_[key];
  if (!global) {
    global = new llvm::GlobalVariable(*codegen_.module_, codegen_.builder_.getInt8Ty(), /*isConstant=*/true,
                                      llvm::GlobalValue::PrivateLinkage, codegen_.builder_.getInt8(0),
                                      "tz.meta.key");
  }

  return global;
}

// With metadata, the runtime finds the class of an instance target by its
// number, and goes up from there: every class gets its object.
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
  std::vector<llvm::Constant *> table(count, llvm::ConstantPointerNull::get(codegen_.builder_.getPtrTy()));
  for (const ClassDecl *cls : classes) {
    if (cls->id) {
      table[cls->id] = codegen_.unknownLowerer().classObject(cls);
    }
  }
  auto *type = llvm::ArrayType::get(codegen_.builder_.getPtrTy(), count);
  auto *global = new llvm::GlobalVariable(*codegen_.module_, type, /*isConstant=*/true,
                                          llvm::GlobalValue::PrivateLinkage,
                                          llvm::ConstantArray::get(type, table), "tz.classes");
  llvm::IRBuilder<> b(init->getNextNode());
  b.CreateCall(codegen_.unknownLowerer().unknownFunction("tz_meta_init", codegen_.builder_.getVoidTy(),
                               {codegen_.builder_.getPtrTy(), codegen_.builder_.getInt32Ty()}),
               {global, b.getInt32(count)});
}

} // namespace tz
