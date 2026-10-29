#include "compiler/library/reflect/reflect_lowerer/reflect_lowerer.h"
#include "compiler/library/json/json_lowerer/json_lowerer.h"
#include "compiler/library/reflect/class_info_lowerer/class_info_lowerer.h"
#include "compiler/library/reflect/reflect_dispatch_lowerer/reflect_dispatch_lowerer.h"
#include "compiler/library/result/result_lowerer/result_lowerer.h"
#include "compiler/lower/classes/class_lowerer/class_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/function_builder/function_builder.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/layouts/object_layouts/object_layouts.h"
#include "compiler/lower/layouts/type_descriptors/type_descriptors.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/memory/store_lowerer/store_lowerer.h"
#include "compiler/lower/stmt/stmt_lowerer/stmt_lowerer.h"
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

constexpr int64_t kStaticRefCount = int64_t(1) << 62;

std::string className(const ClassDecl *cls) {
  return cls->name.empty() && cls->type ? cls->type->name().str() : cls->name.str();
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

ReflectLowerer::ReflectLowerer(Lowering &lowering) : lowering_(lowering) {}

llvm::Function *ReflectLowerer::constructFunction(const ClassDecl *cls) {
  if (llvm::Function *found = constructFunctions_.lookup(cls)) {
    return found;
  }
  llvm::Type *ptr = lowering_.builder_.getPtrTy();
  auto *fn = llvm::Function::Create(
      llvm::FunctionType::get(ptr, {ptr, lowering_.builder_.getInt32Ty()}, false),
      llvm::GlobalValue::InternalLinkage,
      "tz.construct." + lowering_.symbolPrefixes_.lookup(cls) + className(cls),
      lowering_.module_.get());
  lowering_.functionBuilder().setFunctionAttributes(fn);
  constructFunctions_[cls] = fn;

  return fn;
}

void ReflectLowerer::finishReflection(SourceFile &file, llvm::Instruction *init) {
  auto described = [&](const ClassDecl *cls) {
    return cls->isEmitted() && !cls->literal && !cls->builtin &&
           ((lowering_.reflectsNew_ && cls->reflectNew) ||
            (lowering_.reflectsGet_ && cls->reflectMethods));
  };
  llvm::Type *ptr = lowering_.builder_.getPtrTy(), *i32 = lowering_.builder_.getInt32Ty();
  llvm::Constant *null = llvm::ConstantPointerNull::get(lowering_.builder_.getPtrTy());
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
  std::vector<const Type *> invoked;
  for (const ClassDecl *cls : classes) {
    llvm::Constant *construct = null;
    if (lowering_.reflectsNew_ && cls->reflectNew && !cls->isAbstract) {
      construct = constructFunction(cls);
    }
    std::vector<llvm::Constant *> methods;
    llvm::StringSet<> named;
    for (const ClassDecl *c = cls; c && lowering_.reflectsGet_ && cls->reflectMethods;
         c = c->parent) {
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
        std::vector<const FunctionDecl *> variants =
            lowering_.classInfoLowerer().reflectVariants(cls, fn->name);
        if (variants.empty()) {
          continue;
        }
        named.insert(fn->name);
        const Type *type = nullptr;
        llvm::Constant *info =
            variants.size() == 1
                ? lowering_.classInfoLowerer().reflectAdapterInfo(variants[0], type)
                : lowering_.reflectDispatchLowerer().reflectDispatchInfo(cls, variants, type);
        invoked.push_back(type);
        methods.push_back(llvm::ConstantStruct::get(
            reflectMethodTy(), {lowering_.stringLowerer().utf8Constant(fn->name),
                                lowering_.builder_.getInt32(uint32_t(fn->name.size())),
                                lowering_.typeDescriptors().typeDescriptor(type), info}));
      }
    }
    llvm::Constant *table = null;
    if (!methods.empty()) {
      auto *type = llvm::ArrayType::get(reflectMethodTy(), methods.size());
      auto *global = new llvm::GlobalVariable(
          *lowering_.module_, type, true, llvm::GlobalValue::PrivateLinkage,
          llvm::ConstantArray::get(type, methods), "tz.methods." + className(cls));
      global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
      table = global;
    }
    auto *info = cast<llvm::GlobalVariable>(lowering_.classInfoLowerer().classInfoGlobal(cls));
    info->setInitializer(llvm::ConstantStruct::get(
        lowering_.classInfoLowerer().classInfoType(),
        {construct, table, lowering_.builder_.getInt32(uint32_t(methods.size()))}));
  }
  for (size_t i = 0; i < constructFunctions_.size(); ++i) {
    auto [cls, fn] = *(constructFunctions_.begin() + i);
    if (fn->empty()) {
      lowering_.classInfoLowerer().emitConstructThunk(cls, fn);
    }
  }
  for (auto &[cls, global] : lowering_.classInfos_) {
    if (global && !llvm::is_contained(classes, cls)) {
      global->replaceAllUsesWith(null);
      global->eraseFromParent();
      global = nullptr;
    }
  }
  if (!lowering_.reflectsNew_ && !lowering_.reflectsGet_ && !usesApply_) {
    return;
  }

  std::vector<llvm::Constant *> byId(count, null);
  for (const ClassDecl *cls : classes) {
    byId[cls->id] = lowering_.unknownLowerer().classObject(cls);
  }
  auto *classesType = llvm::ArrayType::get(ptr, count);
  auto *classTable = new llvm::GlobalVariable(
      *lowering_.module_, classesType, true, llvm::GlobalValue::PrivateLinkage,
      llvm::ConstantArray::get(classesType, byId), "tz.reflect.classes");
  std::vector<llvm::Constant *> entries;
  if (usesApply_) {
    invoked.insert(invoked.end(), lowering_.unknownFunctionTypes_.begin(),
                   lowering_.unknownFunctionTypes_.end());
    llvm::SmallPtrSet<const Type *, 8> seen;
    auto *entryType = llvm::StructType::get(lowering_.ctx_, {ptr, ptr});
    for (const Type *type : invoked) {
      if (seen.insert(type).second) {
        entries.push_back(llvm::ConstantStruct::get(
            entryType, {lowering_.typeDescriptors().typeDescriptor(type),
                        lowering_.reflectDispatchLowerer().invokerFor(type)}));
      }
    }
  }
  llvm::Constant *invokers = null;
  if (!entries.empty()) {
    auto *type = llvm::ArrayType::get(entries.front()->getType(), entries.size());
    invokers =
        new llvm::GlobalVariable(*lowering_.module_, type, true, llvm::GlobalValue::PrivateLinkage,
                                 llvm::ConstantArray::get(type, entries), "tz.invokers");
  }
  llvm::IRBuilder<> b(init->getNextNode());
  b.CreateCall(lowering_.unknownLowerer().unknownFunction(
                   "tz_reflect_init", lowering_.builder_.getVoidTy(), {ptr, i32, ptr, i32}),
               {classTable, b.getInt32(count), invokers, b.getInt32(uint32_t(entries.size()))});
}

llvm::StructType *ReflectLowerer::reflectMethodTy() {
  if (!reflectMethodTy_) {
    llvm::Type *ptr = lowering_.builder_.getPtrTy();
    reflectMethodTy_ = llvm::StructType::create(
        lowering_.ctx_, {ptr, lowering_.builder_.getInt32Ty(), ptr, ptr}, "tz.reflectmethod");
  }

  return reflectMethodTy_;
}

llvm::Value *ReflectLowerer::emitReflectNew(NewExpr *expr) {
  llvm::Type *ptr = lowering_.builder_.getPtrTy(), *i32 = lowering_.builder_.getInt32Ty();
  lowering_.reflectsNew_ = true;
  llvm::Value *cls = lowering_.exprLowerer().emitExpr(expr->callee);
  std::vector<llvm::Value *> values = lowering_.exprLowerer().emitExprs(expr->args);
  auto *arrayType = llvm::ArrayType::get(ptr, std::max<size_t>(values.size(), 1));
  llvm::Value *args = lowering_.functionFrame().createStackSlot(arrayType, "reflect.args");
  for (size_t i = 0; i < values.size(); ++i) {
    lowering_.builder_.CreateStore(
        values[i], lowering_.builder_.CreateConstInBoundsGEP2_32(arrayType, args, 0, unsigned(i)));
  }
  llvm::Value *object = lowering_.builder_.CreateCall(
      lowering_.unknownLowerer().unknownFunction("tz_reflect_new", ptr, {ptr, ptr, i32}),
      {cls, args, lowering_.builder_.getInt32(uint32_t(values.size()))}, "object");
  for (llvm::Value *value : values) {
    lowering_.refCounts().releaseValue(value, Type::unknown());
  }

  return object;
}

void ReflectLowerer::useErrors(const Type *errorType) {
  lowering_.builder_.CreateCall(
      lowering_.unknownLowerer().unknownFunction(
          "tz_reflect_errors", lowering_.builder_.getVoidTy(), {lowering_.builder_.getPtrTy()}),
      {lowering_.resultLowerer().failureMaker(errorType)});
}

llvm::Value *ReflectLowerer::emitReflectCall(CallExpr *call) {
  llvm::Type *ptr = lowering_.builder_.getPtrTy(), *i64 = lowering_.builder_.getInt64Ty();
  switch (call->builtin) {
  case Builtin::ClassName: {
    llvm::Value *cls = lowering_.exprLowerer().emitExpr(call->args[0]);
    return lowering_.stringLowerer().callReturningString(
        lowering_.unknownLowerer().unknownFunction("tz_class_name", lowering_.builder_.getVoidTy(),
                                                   {ptr, ptr}),
        {cls});
  }
  case Builtin::ReflectGet: {
    lowering_.reflectsGet_ = true;
    std::vector<llvm::Value *> values = lowering_.exprLowerer().emitExprs(call->args);
    llvm::Value *result = lowering_.builder_.CreateCall(
        lowering_.unknownLowerer().unknownFunction("tz_reflect_get", ptr, {ptr, ptr, i64}),
        {values[0], lowering_.stringLowerer().stringObj(values[1]),
         lowering_.stringLowerer().stringMeta(values[1])},
        "got");
    lowering_.refCounts().releaseValue(values[0], call->args[0]->type);
    lowering_.refCounts().releaseValue(values[1], Type::string());
    return result;
  }
  case Builtin::ReflectApply: {
    usesApply_ = true;
    std::vector<llvm::Value *> values = lowering_.exprLowerer().emitExprs(call->args);
    useErrors(call->type->errType());
    llvm::Value *result = lowering_.builder_.CreateCall(
        lowering_.unknownLowerer().unknownFunction("tz_reflect_apply", ptr, {ptr, ptr, ptr}),
        {values[0], values[1], values[2]}, "result");
    for (size_t i = 0; i < values.size(); ++i) {
      lowering_.refCounts().releaseValue(values[i], call->args[i]->type);
    }
    return lowering_.resultLowerer().catchFailure(result, call->type, call->loc());
  }
  case Builtin::ReflectConstruct: {
    std::vector<llvm::Value *> values = lowering_.exprLowerer().emitExprs(call->args);
    useErrors(call->type->errType());
    llvm::Value *object;
    if (const ClassDecl *known = call->args[0]->type->classRef()) {
      object = lowering_.builder_.CreateCall(
          lowering_.unknownLowerer().unknownFunction("tz_reflect_make", ptr, {ptr, ptr}),
          {constructFunction(known), values[1]}, "object");
    } else {
      lowering_.reflectsNew_ = true;
      object = lowering_.builder_.CreateCall(
          lowering_.unknownLowerer().unknownFunction("tz_reflect_construct", ptr, {ptr, ptr}),
          {values[0], values[1]}, "object");
    }
    lowering_.refCounts().releaseValue(values[1], call->args[1]->type);
    return lowering_.resultLowerer().catchFailure(object, call->type, call->loc());
  }
  case Builtin::ReflectMethod: {
    Lowering::Operand object = lowering_.refCounts().emitOperand(call->args[0], true);
    lowering_.refCounts().dropOperand(object);
    const FunctionDecl *method = call->method->decl;
    const ClassDecl *cls = call->args[0]->type->classDecl();
    std::vector<const FunctionDecl *> variants =
        lowering_.classInfoLowerer().reflectVariants(cls, method->name);
    const Type *type = nullptr;
    llvm::Constant *info =
        variants.size() > 1
            ? lowering_.reflectDispatchLowerer().reflectDispatchInfo(cls, variants, type)
            : lowering_.classInfoLowerer().reflectAdapterInfo(method, type);
    lowering_.unknownFunctionTypes_.insert(type);
    llvm::GlobalVariable *&box = reflectBoxes_[info];
    if (!box) {
      llvm::Constant *init = llvm::ConstantStruct::getAnon(
          {lowering_.builder_.getInt64(kStaticRefCount),
           lowering_.module_->getOrInsertGlobal("tz_box_layout", lowering_.builder_.getInt8Ty()),
           lowering_.typeDescriptors().typeDescriptor(type),
           llvm::ConstantStruct::get(lowering_.fnTy_,
                                     {info, llvm::ConstantPointerNull::get(ptr)})});
      box = new llvm::GlobalVariable(*lowering_.module_, init->getType(), false,
                                     llvm::GlobalValue::InternalLinkage, init,
                                     info->getName() + ".box");
    }
    return box;
  }
  default:
    llvm_unreachable("not a builtin of reflection");
  }
}

}
