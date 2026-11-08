#include "compiler/lower/library/reflect/reflect_lowerer/reflect_lowerer.h"
#include "compiler/lower/classes/class_lowerer/class_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/function_builder/function_builder.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/layouts/object_layouts/object_layouts.h"
#include "compiler/lower/layouts/type_descriptors/type_descriptors.h"
#include "compiler/lower/library/json/json_lowerer/json_lowerer.h"
#include "compiler/lower/library/process/process_lowerer/process_lowerer.h"
#include "compiler/lower/library/reflect/class_info_lowerer/class_info_lowerer.h"
#include "compiler/lower/library/reflect/reflect_dispatch_lowerer/reflect_dispatch_lowerer.h"
#include "compiler/lower/library/result/result_lowerer/result_lowerer.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/memory/store_lowerer/store_lowerer.h"
#include "compiler/lower/stmt/stmt_lowerer/stmt_lowerer.h"
#include "compiler/lower/types/unknown_cast_lowerer/unknown_cast_lowerer.h"
#include "compiler/lower/types/unknown_lowerer/unknown_lowerer.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/MDBuilder.h"

using llvm::cast;
using llvm::dyn_cast;

namespace tz {

namespace {

constexpr int64_t kStaticRefCount = int64_t(1) << 62;

}

std::string ReflectLowerer::className(const ClassDecl *cls) {
  return cls->name.empty() && cls->type ? cls->type->name().str() : cls->name.str();
}

std::vector<FunctionDecl *> ReflectLowerer::constructorsOf(const ClassDecl *cls) {
  for (const ClassDecl *c = cls; c; c = c->parent) {
    if (!c->constructors.empty()) {
      return c->constructors;
    }
  }

  return {};
}

ReflectLowerer::ReflectLowerer(Lowering &owner)
    : lowering(owner) {}

llvm::Function *ReflectLowerer::constructFunction(const ClassDecl *cls) {
  if (llvm::Function *found = this->constructFunctions.lookup(cls)) {
    return found;
  }
  llvm::Type *ptr = this->lowering.builder.getPtrTy();
  auto *fn = llvm::Function::Create(
      llvm::FunctionType::get(ptr, {ptr, this->lowering.builder.getInt32Ty()}, false),
      llvm::GlobalValue::InternalLinkage,
      "tz.construct." + this->lowering.symbolPrefixes.lookup(cls) + ReflectLowerer::className(cls),
      this->lowering.module_.get());
  this->lowering.functionBuilder().setFunctionAttributes(fn);
  this->constructFunctions[cls] = fn;

  return fn;
}

llvm::Function *ReflectLowerer::fillFunction(const ClassDecl *cls) {
  if (llvm::Function *found = this->fillFunctions.lookup(cls)) {
    return found;
  }
  llvm::Type *ptr = this->lowering.builder.getPtrTy();
  auto *fn = llvm::Function::Create(
      llvm::FunctionType::get(ptr, {ptr}, false), llvm::GlobalValue::InternalLinkage,
      "tz.fill." + this->lowering.symbolPrefixes.lookup(cls) + ReflectLowerer::className(cls),
      this->lowering.module_.get());
  this->lowering.functionBuilder().setFunctionAttributes(fn);
  this->fillFunctions[cls] = fn;

  return fn;
}

void ReflectLowerer::noteFillErrors(const Type *errorType) {
  std::vector<const Type *> variants;
  if (errorType->isUnion()) {
    variants.assign(errorType->members().begin(), errorType->members().end());
  } else {
    variants.push_back(errorType);
  }
  for (const Type *variant : variants) {
    const Field *kind = variant->field("kind");
    if (!kind || !kind->type->isStringLiteral()) {
      continue;
    }
    const std::u16string &text = kind->type->literalText();
    if (text == u"REFLECT_FILL_ERROR_TYPE") {
      this->fillTypeError = variant;
    } else if (text == u"REFLECT_FILL_ERROR_UNKNOWN_FIELD") {
      this->fillUnknownError = variant;
    } else if (text == u"REFLECT_FILL_ERROR_CLASS") {
      this->fillClassError = variant;
    }
  }
}

void ReflectLowerer::finishReflection(SourceFile &file, llvm::Instruction *init) {
  auto described = [&](const ClassDecl *cls) {
    return cls->isEmitted() && !cls->literal() && !cls->builtin() &&
           ((this->lowering.reflectsNew && cls->reflectNew) ||
            (this->lowering.reflectsGet && cls->reflectMethods) ||
            (this->usesFillAny && cls->reflectFill));
  };
  auto fillProblem = [&](const ClassDecl *cls) -> uint32_t {
    if (cls->isAbstract) {
      return 2;
    }
    for (const ClassDecl *c = cls; c; c = c->parent) {
      if (!c->constructors.empty()) {
        return 1;
      }
    }
    return 0;
  };
  llvm::Type *ptr = this->lowering.builder.getPtrTy();
  llvm::Type *i32 = this->lowering.builder.getInt32Ty();
  llvm::Constant *null = llvm::ConstantPointerNull::get(this->lowering.builder.getPtrTy());
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
    if (this->lowering.reflectsNew && cls->reflectNew && !cls->isAbstract) {
      construct = this->constructFunction(cls);
    }
    std::vector<llvm::Constant *> methods;
    llvm::StringSet<> named;
    for (const ClassDecl *c = cls; c && this->lowering.reflectsGet && cls->reflectMethods;
         c = c->parent) {
      std::vector<FunctionDecl *> own;
      for (FunctionDecl *fn : c->methods) {
        own.push_back(fn);
      }
      for (const ClassField &f : c->fields) {
        if (f.method()) {
          own.push_back(f.method());
        }
      }
      for (FunctionDecl *fn : own) {
        if (fn->isStatic() || named.contains(fn->name)) {
          continue;
        }
        std::vector<const FunctionDecl *> variants =
            this->lowering.classInfoLowerer().reflectVariants(cls, fn->name);
        if (variants.empty()) {
          continue;
        }
        named.insert(fn->name);
        const Type *type = nullptr;
        llvm::Constant *info =
            variants.size() == 1
                ? this->lowering.classInfoLowerer().reflectAdapterInfo(variants[0], type)
                : this->lowering.reflectDispatchLowerer().reflectDispatchInfo(cls, variants, type);
        invoked.push_back(type);
        methods.push_back(llvm::ConstantStruct::get(
            this->reflectMethodTy(),
            {this->lowering.stringLowerer().utf8Constant(fn->name),
             this->lowering.builder.getInt32(uint32_t(fn->name.size())),
             this->lowering.typeDescriptors().typeDescriptor(type), info}));
      }
    }
    llvm::Constant *table = null;
    if (!methods.empty()) {
      auto *type = llvm::ArrayType::get(this->reflectMethodTy(), methods.size());
      auto *global = new llvm::GlobalVariable(
          *this->lowering.module_, type, true, llvm::GlobalValue::PrivateLinkage,
          llvm::ConstantArray::get(type, methods), "tz.methods." + ReflectLowerer::className(cls));
      global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
      table = global;
    }
    llvm::Constant *fill = null;
    uint32_t problem = fillProblem(cls);
    if (this->usesFillAny && cls->reflectFill && problem == 0) {
      fill = this->fillFunction(cls);
    }
    auto *info = cast<llvm::GlobalVariable>(this->lowering.classInfoLowerer().classInfoGlobal(cls));
    info->setInitializer(llvm::ConstantStruct::get(
        this->lowering.classInfoLowerer().classInfoType(),
        {construct, table, this->lowering.builder.getInt32(uint32_t(methods.size())), fill,
         this->lowering.builder.getInt32(problem)}));
  }
  for (size_t i = 0; i < this->constructFunctions.size(); ++i) {
    auto [cls, fn] = *(this->constructFunctions.begin() + i);
    if (fn->empty()) {
      this->lowering.classInfoLowerer().emitConstructThunk(cls, fn);
    }
  }
  ClassInfoLowerer::FillErrors fillErrors{this->fillTypeError, this->fillUnknownError,
                                          this->fillClassError};
  for (size_t i = 0; i < this->fillFunctions.size(); ++i) {
    auto [cls, fn] = *(this->fillFunctions.begin() + i);
    if (fn->empty()) {
      this->lowering.classInfoLowerer().emitFillThunk(cls, fn, fillErrors);
    }
  }
  for (auto &[cls, global] : this->lowering.classInfos) {
    if (global && !llvm::is_contained(classes, cls)) {
      global->replaceAllUsesWith(null);
      global->eraseFromParent();
      global = nullptr;
    }
  }
  if (!this->lowering.reflectsNew && !this->lowering.reflectsGet && !this->usesApply &&
      !this->usesFillAny) {
    return;
  }

  std::vector<llvm::Constant *> byId(count, null);
  for (const ClassDecl *cls : classes) {
    byId[cls->id] = this->lowering.unknownLowerer().classObject(cls);
  }
  auto *classesType = llvm::ArrayType::get(ptr, count);
  auto *classTable = new llvm::GlobalVariable(
      *this->lowering.module_, classesType, true, llvm::GlobalValue::PrivateLinkage,
      llvm::ConstantArray::get(classesType, byId), "tz.reflect.classes");
  std::vector<llvm::Constant *> entries;
  if (this->usesApply) {
    invoked.insert(invoked.end(), this->lowering.unknownFunctionTypes.begin(),
                   this->lowering.unknownFunctionTypes.end());
    llvm::SmallPtrSet<const Type *, 8> seen;
    auto *entryType = llvm::StructType::get(this->lowering.ctx, {ptr, ptr});
    for (const Type *type : invoked) {
      if (seen.insert(type).second) {
        entries.push_back(llvm::ConstantStruct::get(
            entryType, {this->lowering.typeDescriptors().typeDescriptor(type),
                        this->lowering.reflectDispatchLowerer().invokerFor(type)}));
      }
    }
  }
  llvm::Constant *invokers = null;
  if (!entries.empty()) {
    auto *type = llvm::ArrayType::get(entries.front()->getType(), entries.size());
    invokers = new llvm::GlobalVariable(*this->lowering.module_, type, true,
                                        llvm::GlobalValue::PrivateLinkage,
                                        llvm::ConstantArray::get(type, entries), "tz.invokers");
  }
  llvm::IRBuilder<> b(init->getNextNode());
  b.CreateCall(this->lowering.unknownLowerer().unknownFunction(
                   "tz_reflect_init", this->lowering.builder.getVoidTy(), {ptr, i32, ptr, i32}),
               {classTable, b.getInt32(count), invokers, b.getInt32(uint32_t(entries.size()))});
}

llvm::StructType *ReflectLowerer::reflectMethodTy() {
  if (!this->reflectMethodType) {
    llvm::Type *ptr = this->lowering.builder.getPtrTy();
    this->reflectMethodType = llvm::StructType::create(
        this->lowering.ctx, {ptr, this->lowering.builder.getInt32Ty(), ptr, ptr},
        "tz.reflectmethod");
  }

  return this->reflectMethodType;
}

llvm::Value *ReflectLowerer::emitReflectNew(NewExpr *expr) {
  llvm::Type *ptr = this->lowering.builder.getPtrTy();
  llvm::Type *i32 = this->lowering.builder.getInt32Ty();
  this->lowering.reflectsNew = true;
  llvm::Value *cls = this->lowering.exprLowerer().emitExpr(expr->callee);
  std::vector<llvm::Value *> values = this->lowering.exprLowerer().emitExprs(expr->args);
  auto *arrayType = llvm::ArrayType::get(ptr, std::max<size_t>(values.size(), 1));
  llvm::Value *args = this->lowering.functionFrame().createStackSlot(arrayType, "reflect.args");
  for (size_t i = 0; i < values.size(); ++i) {
    this->lowering.builder.CreateStore(values[i], this->lowering.builder.CreateConstInBoundsGEP2_32(
                                                      arrayType, args, 0, unsigned(i)));
  }
  llvm::Value *object = this->lowering.builder.CreateCall(
      this->lowering.unknownLowerer().unknownFunction("tz_reflect_new", ptr, {ptr, ptr, i32}),
      {cls, args, this->lowering.builder.getInt32(uint32_t(values.size()))}, "object");
  for (llvm::Value *value : values) {
    this->lowering.refCounts().releaseValue(value, Type::unknown());
  }

  return object;
}

void ReflectLowerer::useErrors(const Type *errorType) {
  this->lowering.builder.CreateCall(this->lowering.unknownLowerer().unknownFunction(
                                        "tz_reflect_errors", this->lowering.builder.getVoidTy(),
                                        {this->lowering.builder.getPtrTy()}),
                                    {this->lowering.resultLowerer().failureMaker(errorType)});
}

llvm::Value *ReflectLowerer::emitReflectCall(CallExpr *call) {
  llvm::Type *ptr = this->lowering.builder.getPtrTy();
  llvm::Type *i64 = this->lowering.builder.getInt64Ty();
  switch (call->builtin()) {
    case Builtin::ClassName: {
      llvm::Value *cls = this->lowering.exprLowerer().emitExpr(call->args[0]);
      return this->lowering.stringLowerer().callReturningString(
          this->lowering.unknownLowerer().unknownFunction(
              "tz_class_name", this->lowering.builder.getVoidTy(), {ptr, ptr}),
          {cls});
    }
    case Builtin::ReflectGet: {
      this->lowering.reflectsGet = true;
      std::vector<llvm::Value *> values = this->lowering.exprLowerer().emitExprs(call->args);
      llvm::Value *result = this->lowering.builder.CreateCall(
          this->lowering.unknownLowerer().unknownFunction("tz_reflect_get", ptr, {ptr, ptr, i64}),
          {values[0], this->lowering.stringLowerer().stringObj(values[1]),
           this->lowering.stringLowerer().stringMeta(values[1])},
          "got");
      this->lowering.refCounts().releaseValue(values[0], call->args[0]->type);
      this->lowering.refCounts().releaseValue(values[1], Type::string());
      return result;
    }
    case Builtin::ReflectApply: {
      this->usesApply = true;
      std::vector<llvm::Value *> values = this->lowering.exprLowerer().emitExprs(call->args);
      this->useErrors(call->type->errType());
      llvm::Value *result = this->lowering.builder.CreateCall(
          this->lowering.unknownLowerer().unknownFunction("tz_reflect_apply", ptr, {ptr, ptr, ptr}),
          {values[0], values[1], values[2]}, "result");
      for (size_t i = 0; i < values.size(); ++i) {
        this->lowering.refCounts().releaseValue(values[i], call->args[i]->type);
      }
      const Type *gives = call->type->okType();
      if (gives->isUnknown()) {
        return this->lowering.resultLowerer().catchFailure(result, call->type, call->loc());
      }
      return this->lowering.resultLowerer().catchFailure(
          call->type, call->loc(), [&]() -> llvm::Value * {
            if (gives->isVoid()) {
              this->lowering.refCounts().releaseValue(result, Type::unknown());
              return nullptr;
            }
            llvm::BasicBlock *mismatch =
                this->lowering.functionFrame().newBlock("reflect.result.mismatch");
            llvm::Value *value =
                this->lowering.unknownCastLowerer().emitUnknownAs(result, gives, mismatch);
            this->lowering.refCounts().releaseValue(result, Type::unknown());
            llvm::IRBuilderBase::InsertPoint point = this->lowering.builder.saveIP();
            this->lowering.functionFrame().switchTo(mismatch);
            this->lowering.processLowerer().emitPanic(
                "internal error: Reflect.apply gave a value of "
                "another type than the function returns");
            this->lowering.builder.restoreIP(point);
            return value;
          });
    }
    case Builtin::ReflectFill: {
      std::vector<llvm::Value *> values = this->lowering.exprLowerer().emitExprs(call->args);
      this->noteFillErrors(call->type->errType());
      llvm::Value *object;
      if (const ClassDecl *known = call->args[0]->type->classRef()) {
        object =
            this->lowering.builder.CreateCall(this->fillFunction(known), {values[1]}, "filled");
      } else {
        this->usesFillAny = true;
        object = this->lowering.builder.CreateCall(
            this->lowering.unknownLowerer().unknownFunction("tz_reflect_fill", ptr,
                                                            {ptr, ptr, ptr}),
            {values[0], values[1],
             this->lowering.classInfoLowerer().fillMaker(this->fillClassError)},
            "filled");
      }
      this->lowering.refCounts().releaseValue(values[0], call->args[0]->type);
      this->lowering.refCounts().releaseValue(values[1], call->args[1]->type);
      return this->lowering.resultLowerer().catchFailure(object, call->type, call->loc());
    }
    case Builtin::ReflectConstruct: {
      std::vector<llvm::Value *> values = this->lowering.exprLowerer().emitExprs(call->args);
      this->useErrors(call->type->errType());
      llvm::Value *object;
      if (const ClassDecl *known = call->args[0]->type->classRef()) {
        object = this->lowering.builder.CreateCall(
            this->lowering.unknownLowerer().unknownFunction("tz_reflect_make", ptr, {ptr, ptr}),
            {this->constructFunction(known), values[1]}, "object");
      } else {
        this->lowering.reflectsNew = true;
        object = this->lowering.builder.CreateCall(this->lowering.unknownLowerer().unknownFunction(
                                                       "tz_reflect_construct", ptr, {ptr, ptr}),
                                                   {values[0], values[1]}, "object");
      }
      this->lowering.refCounts().releaseValue(values[1], call->args[1]->type);
      return this->lowering.resultLowerer().catchFailure(object, call->type, call->loc());
    }
    case Builtin::ReflectMethod: {
      Lowering::Operand object = this->lowering.refCounts().emitOperand(call->args[0], true);
      this->lowering.refCounts().dropOperand(object);
      const FunctionDecl *method = call->method()->decl;
      const ClassDecl *cls = call->args[0]->type->classDecl();
      std::vector<const FunctionDecl *> variants =
          this->lowering.classInfoLowerer().reflectVariants(cls, method->name);
      const Type *type = nullptr;
      llvm::Constant *info =
          variants.size() > 1
              ? this->lowering.reflectDispatchLowerer().reflectDispatchInfo(cls, variants, type)
              : this->lowering.classInfoLowerer().reflectAdapterInfo(method, type);
      this->lowering.unknownFunctionTypes.insert(type);
      llvm::GlobalVariable *&box = this->reflectBoxes[info];
      if (!box) {
        llvm::Constant *init = llvm::ConstantStruct::getAnon(
            {this->lowering.builder.getInt64(kStaticRefCount),
             this->lowering.module_->getOrInsertGlobal("tz_box_layout",
                                                       this->lowering.builder.getInt8Ty()),
             this->lowering.typeDescriptors().typeDescriptor(type),
             llvm::ConstantStruct::get(this->lowering.fnTy,
                                       {info, llvm::ConstantPointerNull::get(ptr)})});
        box = new llvm::GlobalVariable(*this->lowering.module_, init->getType(), false,
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
