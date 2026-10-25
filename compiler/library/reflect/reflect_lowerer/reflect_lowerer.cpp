#include "compiler/library/reflect/reflect_lowerer/reflect_lowerer.h"
#include "compiler/lower/function_builder/function_builder.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/stmt/stmt_lowerer/stmt_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "compiler/lower/layouts/type_descriptors/type_descriptors.h"
#include "compiler/lower/classes/class_lowerer/class_lowerer.h"
#include "compiler/lower/memory/store_lowerer/store_lowerer.h"
#include "compiler/lower/layouts/object_layouts/object_layouts.h"
#include "compiler/lower/errors/throw_lowerer/throw_lowerer.h"
#include "compiler/lower/types/unknown_lowerer/unknown_lowerer.h"
#include "compiler/library/json/json_lowerer/json_lowerer.h"
#include "compiler/library/json/from_json_lowerer/from_json_lowerer.h"
#include "compiler/library/reflect/class_info_lowerer/class_info_lowerer.h"
#include "compiler/library/reflect/reflect_dispatch_lowerer/reflect_dispatch_lowerer.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/MDBuilder.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::BasicBlock;
using llvm::Value;

namespace tz {

namespace {

// A count no program reaches, as literals have (see codegen.cpp).
constexpr int64_t kStaticRefCount = int64_t(1) << 62;

// What a class is called in messages: its name, or what it extends.
std::string className(const ClassDecl *cls) {
  return cls->name.empty() && cls->type ? cls->type->name().str() : cls->name.str();
}

// The constructors `new` of a class chooses from: its own, or those of the
// nearest class up its chain that has any.
std::vector<FunctionDecl *> constructorsOf(const ClassDecl *cls) {
  for (const ClassDecl *c = cls; c; c = c->parent) {
    if (!c->constructors.empty()) {
      return c->constructors;
    }
  }

  return {};
}

} // namespace

ReflectLowerer::ReflectLowerer(CodeGen &codegen) : codegen_(codegen) {}

llvm::Function *ReflectLowerer::classFromJsonFunction(const ClassDecl *cls) {
  llvm::Function *&fn = classConverters_[cls];
  if (!fn) {
    llvm::Type *ptr = codegen_.builder_.getPtrTy();
    fn = llvm::Function::Create(
        llvm::FunctionType::get(ptr, {ptr, ptr}, false), llvm::GlobalValue::InternalLinkage,
        "tz.fromJson.class." + codegen_.symbolPrefixes_.lookup(cls) + className(cls), codegen_.module_.get());
    codegen_.functionBuilder().setFunctionAttributes(fn);
    pendingConverters_.push_back(cls); // its body comes once the program is emitted
  }

  return fn;
}

// (ptr json, ptr path) -> ptr: the object the constructor makes from the
// keys of the Json named as its parameters, each checked as fromJson<T>
// checks a field; then the public fields it may write that the Json has.
void ReflectLowerer::emitClassFromJson(const ClassDecl *cls, llvm::Function *fn) {
  llvm::Type *ptr = codegen_.builder_.getPtrTy(), *i64 = codegen_.builder_.getInt64Ty();
  codegen_.functionBuilder().beginFunction(fn, nullptr);
  codegen_.loc_ = cls->loc();
  codegen_.functionFrame().pushScope();
  if (!codegen_.jsonPathTy_) { // runtime/library/json/json/json.c: TzJsonPath
    codegen_.jsonPathTy_ = llvm::StructType::create(codegen_.ctx_, {ptr, ptr, i64, i64}, "tz.jsonpath");
  }
  llvm::Value *json = fn->getArg(0), *path = fn->getArg(1);
  llvm::Value *null = llvm::ConstantPointerNull::get(codegen_.builder_.getPtrTy());
  std::string name = className(cls);
  llvm::Value *link = codegen_.functionFrame().createStackSlot(codegen_.jsonPathTy_, "path");
  codegen_.builder_.CreateStore(path, codegen_.builder_.CreateStructGEP(codegen_.jsonPathTy_, link, 0));
  llvm::Value *keyObj = codegen_.builder_.CreateStructGEP(codegen_.jsonPathTy_, link, 1);
  llvm::Value *keyMeta = codegen_.builder_.CreateStructGEP(codegen_.jsonPathTy_, link, 2);
  codegen_.builder_.CreateStore(llvm::ConstantInt::getSigned(i64, -1),
                       codegen_.builder_.CreateStructGEP(codegen_.jsonPathTy_, link, 3));
  auto failIfThrown = [&](llvm::function_ref<void()> release) {
    llvm::BasicBlock *failBlock = codegen_.functionFrame().newBlock("json.failed");
    llvm::BasicBlock *goBlock = codegen_.functionFrame().newBlock("json.fits");
    codegen_.builder_.CreateCondBr(codegen_.builder_.CreateIsNotNull(codegen_.builder_.CreateLoad(ptr, codegen_.throwLowerer().thrownSlot())),
                          failBlock, goBlock, llvm::MDBuilder(codegen_.ctx_).createUnlikelyBranchWeights());
    codegen_.functionFrame().switchTo(failBlock);
    release();
    codegen_.functionFrame().emitCleanups(0);
    codegen_.builder_.CreateRet(null);
    codegen_.functionFrame().switchTo(goBlock);
  };
  codegen_.builder_.CreateCall(
      codegen_.jsonLowerer().jsonFunction("tz_json_expect_object", codegen_.builder_.getVoidTy(), {ptr, ptr, ptr, i64}),
      {json, path, codegen_.stringLowerer().utf8Constant(name), codegen_.builder_.getInt64(name.size())});
  failIfThrown([] {});
  auto keyed = [&](llvm::StringRef key) {
    llvm::Value *text = codegen_.stringLowerer().stringLiteral(std::u16string(key.begin(), key.end()));
    codegen_.builder_.CreateStore(codegen_.stringLowerer().stringObj(text), keyObj);
    codegen_.builder_.CreateStore(codegen_.stringLowerer().stringMeta(text), keyMeta);

    return codegen_.builder_.CreateCall(codegen_.jsonLowerer().jsonFunction("tz_json_field", ptr, {ptr, ptr, i64}),
                               {json, codegen_.stringLowerer().stringObj(text), codegen_.stringLowerer().stringMeta(text)}, key);
  };
  // The arguments: the keys named as the parameters of the constructor.
  std::vector<FunctionDecl *> ctors = constructorsOf(cls);
  const FunctionDecl *ctor = ctors.empty() ? nullptr : ctors.front();
  const FuncSymbol *sym = ctor ? ctor->sym : nullptr;
  std::vector<llvm::Value *> values;
  llvm::StringSet<> given;
  for (size_t k = 0; sym && k < sym->params.size() && k < cls->jsonParams.size(); ++k) {
    llvm::StringRef param = cls->jsonParams[k];
    given.insert(param);
    llvm::Value *item = keyed(param);
    values.push_back(codegen_.fromJsonLowerer().emitFromJson(sym->params[k], item, link));
    failIfThrown([&] {
      for (size_t made = 0; made < k; ++made) {
        codegen_.refCounts().releaseValue(values[made], sym->params[made]);
      }
    });
  }
  llvm::Value *object = codegen_.classLowerer().allocInstance(cls);
  CodeGen::Held held = codegen_.throwLowerer().hold(object, cls->type, true);
  codegen_.classLowerer().emitConstruct(object, cls, sym, values);
  object = codegen_.throwLowerer().take(held);
  // Then the other fields that code may write, if the Json has them.
  for (const Field &f : cls->type->fields()) {
    if (given.contains(f.name) || !llvm::is_contained(cls->jsonFields, f.name)) {
      continue;
    }
    llvm::Value *item = keyed(f.name);
    llvm::BasicBlock *setBlock = codegen_.functionFrame().newBlock("json.field");
    llvm::BasicBlock *nextBlock = codegen_.functionFrame().newBlock("json.field.next");
    codegen_.builder_.CreateCondBr(codegen_.builder_.CreateIsNotNull(item), setBlock, nextBlock);
    codegen_.functionFrame().switchTo(setBlock);
    llvm::Value *value = codegen_.fromJsonLowerer().emitFromJson(f.type, item, link);
    failIfThrown([&] { codegen_.refCounts().releaseValue(object, cls->type); });
    codegen_.storeLowerer().storeBarrier(object, value, f.type);
    codegen_.stmtLowerer().storeReplacing(codegen_.objectLayouts().fieldAddress(object, cls->type, f.name), value, f.type);
    codegen_.builder_.CreateBr(nextBlock);
    codegen_.functionFrame().switchTo(nextBlock);
  }
  codegen_.functionFrame().emitCleanups(0);
  codegen_.builder_.CreateRet(object);
  codegen_.functionFrame().startDeadBlock();
  codegen_.functionFrame().popScope();
  codegen_.functionBuilder().finishFunction();
}

llvm::Function *ReflectLowerer::constructFunction(const ClassDecl *cls) {
  if (llvm::Function *found = constructFunctions_.lookup(cls)) {
    return found;
  }
  llvm::Type *ptr = codegen_.builder_.getPtrTy();
  auto *fn = llvm::Function::Create(
      llvm::FunctionType::get(ptr, {ptr, codegen_.builder_.getInt32Ty()}, false),
      llvm::GlobalValue::InternalLinkage,
      "tz.construct." + codegen_.symbolPrefixes_.lookup(cls) + className(cls), codegen_.module_.get());
  codegen_.functionBuilder().setFunctionAttributes(fn);
  constructFunctions_[cls] = fn; // its body comes once the program is emitted

  return fn;
}

void ReflectLowerer::finishReflection(SourceFile &file, llvm::Instruction *init) {
  // The bodies of the functions that make objects of classes from Json, which
  // may need those of more classes.
  for (size_t i = 0; i < pendingConverters_.size(); ++i) {
    emitClassFromJson(pendingConverters_[i], classConverters_.lookup(pendingConverters_[i]));
  }
  // A class is described only if the program looks up what the description
  // holds: when every `new` and Reflect call is of classes known when
  // compiling, none is.
  auto described = [&](const ClassDecl *cls) {
    return cls->isEmitted() && !cls->literal && !cls->builtin &&
           ((codegen_.reflectsNew_ && cls->reflectNew) || (codegen_.reflectsGet_ && cls->reflectMethods) ||
            (codegen_.reflectsJson_ && cls->reflectJson));
  };
  llvm::Type *ptr = codegen_.builder_.getPtrTy(), *i32 = codegen_.builder_.getInt32Ty();
  llvm::Constant *null = llvm::ConstantPointerNull::get(codegen_.builder_.getPtrTy());
  std::vector<const ClassDecl *> classes;
  unsigned count = 1;
  for (Stmt *item : file.items) {
    auto *cls = dyn_cast<ClassDecl>(item);
    if (!cls || !described(cls)) {
      continue;
    }
    classes.push_back(cls);
    count = std::max(count, cls->id + 1);
  }
  // The description of each class.
  std::vector<const Type *> invoked;
  for (const ClassDecl *cls : classes) {
    llvm::Constant *construct = null;
    if (codegen_.reflectsNew_ && cls->reflectNew && !cls->isAbstract) {
      construct = constructFunction(cls);
    }
    llvm::Constant *fromJson = null;
    if (codegen_.reflectsJson_ && cls->reflectJson && !cls->isAbstract) {
      fromJson = classFromJsonFunction(cls);
    }
    // Its methods, and those up its chain it does not override, by name: of
    // several variants, the function that chooses one by the arguments
    // (docs/overloads.md, section 6).
    std::vector<llvm::Constant *> methods;
    llvm::StringSet<> named;
    for (const ClassDecl *c = cls; c && codegen_.reflectsGet_ && cls->reflectMethods; c = c->parent) {
      std::vector<FunctionDecl *> own;
      for (FunctionDecl *fn : c->methods) {
        own.push_back(fn);
      }
      for (const ClassField &f : c->fields) {
        if (f.method) {
          own.push_back(f.method);
        }
      }
      for (FunctionDecl *fn : own) {
        if (fn->isStatic || named.contains(fn->name)) {
          continue;
        }
        std::vector<const FunctionDecl *> variants = codegen_.classInfoLowerer().reflectVariants(cls, fn->name);
        if (variants.empty()) {
          continue;
        }
        named.insert(fn->name);
        const Type *type = nullptr;
        llvm::Constant *info = variants.size() == 1 ? codegen_.classInfoLowerer().reflectAdapterInfo(variants[0], type)
                                                    : codegen_.reflectDispatchLowerer().reflectDispatchInfo(cls, variants, type);
        invoked.push_back(type);
        methods.push_back(llvm::ConstantStruct::get(reflectMethodTy(),
                                                    {codegen_.stringLowerer().utf8Constant(fn->name),
                                                     codegen_.builder_.getInt32(uint32_t(fn->name.size())),
                                                     codegen_.typeDescriptors().typeDescriptor(type), info}));
      }
    }
    llvm::Constant *table = null;
    if (!methods.empty()) {
      auto *type = llvm::ArrayType::get(reflectMethodTy(), methods.size());
      auto *global = new llvm::GlobalVariable(
          *codegen_.module_, type, /*isConstant=*/true, llvm::GlobalValue::PrivateLinkage,
          llvm::ConstantArray::get(type, methods), "tz.methods." + className(cls));
      global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
      table = global;
    }
    auto *info = cast<llvm::GlobalVariable>(codegen_.classInfoLowerer().classInfoGlobal(cls));
    info->setInitializer(
        llvm::ConstantStruct::get(codegen_.classInfoLowerer().classInfoType(), {construct, fromJson, table,
                                                    codegen_.builder_.getInt32(uint32_t(methods.size()))}));
  }
  // The bodies of the functions that make objects from unknowns, which the
  // descriptions and Reflect.construct of classes known when compiling call.
  for (size_t i = 0; i < constructFunctions_.size(); ++i) {
    auto [cls, fn] = *(constructFunctions_.begin() + i);
    if (fn->empty()) {
      codegen_.classInfoLowerer().emitConstructThunk(cls, fn);
    }
  }
  for (size_t i = 0; i < pendingConverters_.size(); ++i) {
    if (classConverters_.lookup(pendingConverters_[i])->empty()) {
      emitClassFromJson(pendingConverters_[i], classConverters_.lookup(pendingConverters_[i]));
    }
  }
  // The objects of the other classes point to no description.
  for (auto &[cls, global] : codegen_.classInfos_) {
    if (global && !llvm::is_contained(classes, cls)) {
      global->replaceAllUsesWith(null);
      global->eraseFromParent();
      global = nullptr;
    }
  }
  if (!codegen_.reflectsNew_ && !codegen_.reflectsGet_ && !codegen_.reflectsJson_ && !usesApply_) {
    return;
  }

  // The objects of the classes by number, and the invokers of the types of
  // the functions that may reach Reflect.apply.
  std::vector<llvm::Constant *> byId(count, null);
  for (const ClassDecl *cls : classes) {
    byId[cls->id] = codegen_.unknownLowerer().classObject(cls);
  }
  auto *classesType = llvm::ArrayType::get(ptr, count);
  auto *classTable = new llvm::GlobalVariable(
      *codegen_.module_, classesType, /*isConstant=*/true, llvm::GlobalValue::PrivateLinkage,
      llvm::ConstantArray::get(classesType, byId), "tz.reflect.classes");
  std::vector<llvm::Constant *> entries;
  if (usesApply_) {
    invoked.insert(invoked.end(), codegen_.unknownFunctionTypes_.begin(), codegen_.unknownFunctionTypes_.end());
    llvm::SmallPtrSet<const Type *, 8> seen;
    auto *entryType = llvm::StructType::get(codegen_.ctx_, {ptr, ptr});
    for (const Type *type : invoked) {
      if (seen.insert(type).second) {
        entries.push_back(
            llvm::ConstantStruct::get(entryType, {codegen_.typeDescriptors().typeDescriptor(type), codegen_.reflectDispatchLowerer().invokerFor(type)}));
      }
    }
  }
  llvm::Constant *invokers = null;
  if (!entries.empty()) {
    auto *type = llvm::ArrayType::get(entries.front()->getType(), entries.size());
    invokers = new llvm::GlobalVariable(*codegen_.module_, type, /*isConstant=*/true,
                                        llvm::GlobalValue::PrivateLinkage,
                                        llvm::ConstantArray::get(type, entries), "tz.invokers");
  }
  llvm::IRBuilder<> b(init->getNextNode());
  b.CreateCall(codegen_.unknownLowerer().unknownFunction("tz_reflect_init", codegen_.builder_.getVoidTy(), {ptr, i32, ptr, i32}),
               {classTable, b.getInt32(count), invokers, b.getInt32(uint32_t(entries.size()))});
}

llvm::StructType *ReflectLowerer::reflectMethodTy() {
  // TzReflectMethod: { ptr name, i32 length, ptr type, ptr info }.
  if (!reflectMethodTy_) {
    llvm::Type *ptr = codegen_.builder_.getPtrTy();
    reflectMethodTy_ =
        llvm::StructType::create(codegen_.ctx_, {ptr, codegen_.builder_.getInt32Ty(), ptr, ptr}, "tz.reflectmethod");
  }

  return reflectMethodTy_;
}

// The class first, then the arguments as unknowns, to the description of
// the class the value is.
llvm::Value *ReflectLowerer::emitReflectNew(NewExpr *expr) {
  llvm::Type *ptr = codegen_.builder_.getPtrTy(), *i32 = codegen_.builder_.getInt32Ty();
  codegen_.reflectsNew_ = true;
  llvm::Value *cls = codegen_.exprLowerer().emitExpr(expr->callee); // a class is never counted
  std::vector<llvm::Value *> values = codegen_.throwLowerer().emitHeldValues(expr->args);
  auto *arrayType = llvm::ArrayType::get(ptr, std::max<size_t>(values.size(), 1));
  llvm::Value *args = codegen_.functionFrame().createStackSlot(arrayType, "reflect.args");
  for (size_t i = 0; i < values.size(); ++i) {
    codegen_.builder_.CreateStore(values[i],
                         codegen_.builder_.CreateConstInBoundsGEP2_32(arrayType, args, 0, unsigned(i)));
  }
  llvm::Value *object =
      codegen_.builder_.CreateCall(codegen_.unknownLowerer().unknownFunction("tz_reflect_new", ptr, {ptr, ptr, i32}),
                          {cls, args, codegen_.builder_.getInt32(uint32_t(values.size()))}, "object");
  for (llvm::Value *value : values) {
    codegen_.refCounts().releaseValue(value, Type::unknown());
  }
  codegen_.throwLowerer().emitThrowCheck();

  return object;
}

llvm::Value *ReflectLowerer::emitReflectCall(CallExpr *call) {
  llvm::Type *ptr = codegen_.builder_.getPtrTy(), *i64 = codegen_.builder_.getInt64Ty();
  switch (call->builtin) {
  case Builtin::ClassName: {
    llvm::Value *cls = codegen_.exprLowerer().emitExpr(call->args[0]);
    return codegen_.stringLowerer().callReturningString(codegen_.unknownLowerer().unknownFunction("tz_class_name", codegen_.builder_.getVoidTy(), {ptr, ptr}),
                               {cls});
  }
  case Builtin::ReflectGet: {
    codegen_.reflectsGet_ = true;
    std::vector<llvm::Value *> values = codegen_.throwLowerer().emitHeldValues(call->args);
    llvm::Value *result =
        codegen_.builder_.CreateCall(codegen_.unknownLowerer().unknownFunction("tz_reflect_get", ptr, {ptr, ptr, i64}),
                            {values[0], codegen_.stringLowerer().stringObj(values[1]), codegen_.stringLowerer().stringMeta(values[1])}, "got");
    codegen_.refCounts().releaseValue(values[0], call->args[0]->type);
    codegen_.refCounts().releaseValue(values[1], Type::string());
    codegen_.throwLowerer().emitThrowCheck();
    return result;
  }
  case Builtin::ReflectApply: {
    usesApply_ = true;
    std::vector<llvm::Value *> values = codegen_.throwLowerer().emitHeldValues(call->args);
    llvm::Value *result =
        codegen_.builder_.CreateCall(codegen_.unknownLowerer().unknownFunction("tz_reflect_apply", ptr, {ptr, ptr, ptr}),
                            {values[0], values[1], values[2]}, "result");
    for (size_t i = 0; i < values.size(); ++i) {
      codegen_.refCounts().releaseValue(values[i], call->args[i]->type);
    }
    codegen_.throwLowerer().emitThrowCheck();
    return result;
  }
  case Builtin::ReflectConstruct: {
    std::vector<llvm::Value *> values = codegen_.throwLowerer().emitHeldValues(call->args);
    llvm::Value *object;
    if (const ClassDecl *known = call->args[0]->type->classRef()) {
      // A class known when compiling: its function, without its description.
      object = codegen_.builder_.CreateCall(codegen_.unknownLowerer().unknownFunction("tz_reflect_make", ptr, {ptr, ptr}),
                                   {constructFunction(known), values[1]}, "object");
    } else {
      codegen_.reflectsNew_ = true;
      object = codegen_.builder_.CreateCall(codegen_.unknownLowerer().unknownFunction("tz_reflect_construct", ptr, {ptr, ptr}),
                                   {values[0], values[1]}, "object");
    }
    codegen_.refCounts().releaseValue(values[1], call->args[1]->type);
    codegen_.throwLowerer().emitThrowCheck();
    return object;
  }
  case Builtin::FromJsonClass: {
    llvm::Value *cls = codegen_.exprLowerer().emitExpr(call->args[0]);
    CodeGen::Operand json = codegen_.refCounts().emitOperand(call->args[1], /*canBorrow=*/true);
    llvm::Value *object;
    if (const ClassDecl *known = call->args[0]->type->classRef()) {
      object = codegen_.builder_.CreateCall(
          classFromJsonFunction(known),
          {json.value, llvm::ConstantPointerNull::get(codegen_.builder_.getPtrTy())}, "object");
    } else {
      codegen_.reflectsJson_ = true;
      object = codegen_.builder_.CreateCall(codegen_.unknownLowerer().unknownFunction("tz_reflect_from_json", ptr, {ptr, ptr}),
                                   {cls, json.value}, "object");
    }
    codegen_.refCounts().dropOperand(json);
    codegen_.throwLowerer().emitThrowCheck();
    return object;
  }
  case Builtin::ReflectMethod: {
    // The object only says which class: its method, as the description
    // would give it, is a function value without an environment, in a box
    // that lives as long as the program, as the object of a class does.
    CodeGen::Operand object = codegen_.refCounts().emitOperand(call->args[0], /*canBorrow=*/true);
    codegen_.refCounts().dropOperand(object);
    // Of several variants, the function that chooses one.
    const FunctionDecl *method = call->method->decl;
    const ClassDecl *cls = call->args[0]->type->classDecl();
    std::vector<const FunctionDecl *> variants = codegen_.classInfoLowerer().reflectVariants(cls, method->name);
    const Type *type = nullptr;
    llvm::Constant *info = variants.size() > 1 ? codegen_.reflectDispatchLowerer().reflectDispatchInfo(cls, variants, type)
                                               : codegen_.classInfoLowerer().reflectAdapterInfo(method, type);
    codegen_.unknownFunctionTypes_.insert(type); // Reflect.apply may call it
    llvm::GlobalVariable *&box = reflectBoxes_[info];
    if (!box) {
      llvm::Constant *init = llvm::ConstantStruct::getAnon(
          {codegen_.builder_.getInt64(kStaticRefCount),
           codegen_.module_->getOrInsertGlobal("tz_box_layout", codegen_.builder_.getInt8Ty()), codegen_.typeDescriptors().typeDescriptor(type),
           llvm::ConstantStruct::get(codegen_.fnTy_, {info, llvm::ConstantPointerNull::get(ptr)})});
      box = new llvm::GlobalVariable(*codegen_.module_, init->getType(), /*isConstant=*/false,
                                     llvm::GlobalValue::InternalLinkage, init,
                                     info->getName() + ".box");
    }
    return box;
  }
  default:
    llvm_unreachable("not a builtin of reflection");
  }
}

} // namespace tz
