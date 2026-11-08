#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/async/async_lowerer/async_lowerer.h"
#include "compiler/lower/classes/class_descriptors/class_descriptors.h"
#include "compiler/lower/classes/decorator_lowerer/decorator_lowerer.h"
#include "compiler/lower/classes/inherit_lowerer/inherit_lowerer.h"
#include "compiler/lower/debug_info/debug_info.h"
#include "compiler/lower/function_builder/function_builder.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/functions/closure_lowerer/closure_lowerer.h"
#include "compiler/lower/functions/tail_call_lowerer/tail_call_lowerer.h"
#include "compiler/lower/iterators/generator_lowerer/generator_lowerer.h"
#include "compiler/lower/library/json/json_writers/json_writers.h"
#include "compiler/lower/library/reflect/reflect_lowerer/reflect_lowerer.h"
#include "compiler/lower/library/result/result_lowerer/result_lowerer.h"
#include "compiler/lower/memory/memory_debug/memory_debug.h"
#include "compiler/lower/memory/publish_lowerer/publish_lowerer.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/runtime_functions/runtime_functions.h"
#include "compiler/lower/stmt/stmt_lowerer/stmt_lowerer.h"
#include "compiler/lower/types/union_layout/union_layout.h"
#include "compiler/lower/types/unknown_lowerer/unknown_lowerer.h"
#include "compiler/memory/memory_plan/memory_plan.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/Support/ErrorHandling.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

ModuleBuilder::ModuleBuilder(Lowering &owner)
    : lowering(owner) {}

std::unique_ptr<llvm::Module> ModuleBuilder::generate(SourceFile &file) {
  this->lowering.comparesFunctions = file.inventory.comparesFunctions;
  this->lowering.debugInfo().start(file);
  if (this->lowering.parallel_) {
    for (const Module *module : file.modules) {
      if (!module->builtin) {
        continue;
      }
      for (Stmt *item : module->file.items) {
        auto *cls = dyn_cast<ClassDecl>(item);
        if (cls) {
          for (const ClassField &f : cls->fields) {
            if (f.isStatic() && f.var()) {
              this->lowering.libraryState.insert(f.var());
            }
          }
          continue;
        }
        auto *decl = dyn_cast<VarDeclStmt>(item);
        if (decl) {
          for (VarDeclarator &d : decl->decls) {
            const Type *t = d.sym->type;
            bool value = t && (t->isNumeric() || t->isBool() || t->isString());
            if (d.sym->usedInFunction() && !(d.sym->isConst && value)) {
              this->lowering.libraryState.insert(d.sym);
            }
          }
        }
      }
    }
  }
  this->lowering.usesTls = file.inventory.usesTls;
  this->lowering.nativeError = file.inventory.nativeError;
  this->lowering.subtypes = file.inventory.subtypes;
  this->lowering.usesAsync = file.inventory.usesAsync;
  this->lowering.esModule = file.inventory.topLevelAwait;
  for (const Module *module : file.modules) {
    if (module->isEntry &&
        (llvm::any_of(module->file.imports, [](const ImportDecl &i) { return !i.implicit; }) ||
         !module->file.exports.empty())) {
      this->lowering.esModule = true;
    }
  }
  this->lowering.runtimeFunctions().declareRuntime();
  this->lowering.debugMemory = getenv("TZ_CHECK_LEAKS") != nullptr;
  this->lowering.runtimeFunctions().defineRefCountHelpers();
  this->lowering.inheritLowerer().prepareDispatch(file);
  for (const Module *module : file.modules) {
    if (module->isEntry) {
      continue;
    }
    std::string prefix;
    llvm::StringRef name(module->name);
    name.consume_back(".tz");
    llvm::SmallVector<llvm::StringRef, 4> parts;
    name.split(parts, '/');
    for (llvm::StringRef part : parts) {
      prefix += (part == ".." ? std::string("up") : part.str()) + ".";
    }
    for (const Stmt *item : module->file.items) {
      this->lowering.symbolPrefixes[item] = prefix;
    }
  }
  std::vector<FunctionDecl *> functions;
  std::vector<ClassDecl *> classes;
  for (Stmt *item : file.items) {
    if (auto *fn = dyn_cast<FunctionDecl>(item);
        fn && fn->typeParams.empty() && !fn->intrinsic() && !file.unusedFunctions.contains(fn)) {
      functions.push_back(fn);
    }
    auto *cls = dyn_cast<ClassDecl>(item);
    if (cls) {
      if (cls->isEmitted()) {
        classes.push_back(cls);
      }
      if (!cls->generic()) {
        for (FunctionDecl *fn : cls->methods) {
          if (fn->isStatic() && fn->typeParams.empty()) {
            functions.push_back(fn);
          }
        }
      }
    }
  }
  functions.insert(functions.end(), file.inventory.instances.begin(),
                   file.inventory.instances.end());
  this->lowering.emittedClasses.assign(classes.begin(), classes.end());
  for (FunctionDecl *fn : functions) {
    this->lowering.functionBuilder().declareFunction(fn);
  }
  for (ClassDecl *cls : classes) {
    this->lowering.classDescriptors().declareMethods(cls);
  }
  this->lowering.tailCallLowerer().findTailGroups(functions);
  this->declareGlobals(file);
  for (FunctionDecl *fn : functions) {
    this->lowering.functionBuilder().emitFunction(fn);
  }
  for (ClassDecl *cls : classes) {
    this->lowering.classDescriptors().emitMethods(cls);
  }
  this->emitMain(file);
  while (!this->lowering.pendingClosures.empty() || !this->lowering.pendingIterators.empty()) {
    this->lowering.closureLowerer().emitPendingClosures();
    this->lowering.generatorLowerer().emitPendingIterators();
  }
  this->lowering.reflectLowerer().finishReflection(file, this->mainInit);
  this->lowering.decoratorLowerer().finishMetadata(file, this->mainInit);
  this->lowering.memoryDebug().finishGroupDebug(this->mainInit);
  this->lowering.publishLowerer().finishPublishTable(this->mainInit);
  this->lowering.jsonWriters().finish();
  this->lowering.debugInfo().finish();

  return std::move(this->lowering.module_);
}

llvm::Type *ModuleBuilder::lower(const Type *type) {
  switch (type->kind()) {
  case Type::Kind::Void:
    return this->lowering.builder.getVoidTy();
  case Type::Kind::Undefined:
    return this->lowering.builder.getInt1Ty();
  case Type::Kind::Bool:
    return this->lowering.builder.getInt1Ty();
  case Type::Kind::Int:
    return this->lowering.builder.getIntNTy(type->bitWidth());
  case Type::Kind::Double:
    return this->lowering.builder.getDoubleTy();
  case Type::Kind::Decimal:
    return this->lowering.decTy;
  case Type::Kind::String:
    return this->lowering.strTy;
  case Type::Kind::Array:
    return this->lowering.builder.getPtrTy();
  case Type::Kind::Optional:
    if (UnionLayout::zeroIsUndefined(type->inner())) {
      return this->lower(type->inner());
    }
    return llvm::StructType::get(this->lowering.ctx,
                                 {this->lower(type->inner()), this->lowering.builder.getInt1Ty()});
  case Type::Kind::Literal:
  case Type::Kind::Union:
    return this->lowering.unionLayout().lowerUnion(type);
  case Type::Kind::Function:
    return this->lowering.fnTy;
  case Type::Kind::Object:
    return this->lowering.builder.getPtrTy();
  case Type::Kind::Result:
    return this->lowering.resultLowerer().resultLayout(type).type;
  case Type::Kind::Never:
    return llvm::StructType::get(this->lowering.ctx);
  case Type::Kind::Iterator:
    return this->lowering.builder.getPtrTy();
  case Type::Kind::Json:
    return this->lowering.builder.getPtrTy();
  case Type::Kind::Dict:
    return this->lowering.builder.getPtrTy();
  case Type::Kind::Promise:
    return this->lowering.builder.getPtrTy();
  case Type::Kind::Unknown:
    return this->lowering.builder.getPtrTy();
  case Type::Kind::ClassRef:
    return this->lowering.builder.getPtrTy();
  case Type::Kind::Error:
  case Type::Kind::Param:
    break;
  }
  llvm_unreachable("an erroneous type reached code generation");
}

void ModuleBuilder::declareGlobals(SourceFile &file) {
  for (Stmt *item : file.items) {
    auto *cls = dyn_cast<ClassDecl>(item);
    if (cls) {
      for (const ClassField &f : cls->fields) {
        if (!f.isStatic() || !f.var()) {
          continue;
        }
        llvm::Type *type = this->lower(f.var()->type);
        this->lowering.globals[f.var()] =
            cls->usedAsValue
                ? this->lowering.unknownLowerer().staticFieldAddress(cls, f.var())
                : new llvm::GlobalVariable(
                      *this->lowering.module_, type, false, llvm::GlobalValue::InternalLinkage,
                      llvm::Constant::getNullValue(type),
                      "tz." + this->lowering.symbolPrefixes.lookup(cls) + f.var()->name);
        this->lowering.debugInfo().declareGlobal(f.var(), this->lowering.globals[f.var()]);
        if (f.var()->type->isManaged()) {
          this->managedGlobals.push_back(f.var());
        }
      }
      for (FunctionDecl *fn : cls->methods) {
        const VarSymbol *impl = fn->impl();
        if (impl) {
          llvm::Constant *adapter =
              cast<llvm::Constant>(this->lowering.closureLowerer().makeFunctionValue(
                  this->lowering.decoratorLowerer().implAdapterInfo(fn),
                  llvm::ConstantPointerNull::get(this->lowering.builder.getPtrTy())));
          this->lowering.globals[impl] =
              new llvm::GlobalVariable(*this->lowering.module_, this->lower(impl->type), false,
                                       llvm::GlobalValue::InternalLinkage, adapter,
                                       "tz." + this->lowering.symbolPrefixes.lookup(cls) +
                                           cls->name.str() + "." + fn->name.str() + ".impl");
          this->managedGlobals.push_back(impl);
        }
      }
      continue;
    }
    auto *decl = dyn_cast<VarDeclStmt>(item);
    if (!decl) {
      continue;
    }
    for (VarDeclarator &d : decl->decls) {
      if (d.alias || !d.sym->usedInFunction()) {
        continue;
      }
      llvm::Type *type = this->lower(d.sym->type);
      this->lowering.globals[d.sym] = new llvm::GlobalVariable(
          *this->lowering.module_, type, false, llvm::GlobalValue::InternalLinkage,
          llvm::Constant::getNullValue(type),
          "tz." + this->lowering.symbolPrefixes.lookup(decl) + d.name);
      this->lowering.debugInfo().declareGlobal(d.sym, this->lowering.globals[d.sym]);
      if (d.sym->type->isManaged()) {
        this->managedGlobals.push_back(d.sym);
      }
    }
  }
}

void ModuleBuilder::emitMain(SourceFile &file) {
  llvm::Function *asyncMain =
      file.inventory.topLevelAwait ? this->lowering.asyncLowerer().emitAsyncMain(file) : nullptr;
  auto *type = llvm::FunctionType::get(this->lowering.builder.getInt32Ty(), false);
  auto *main = llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage, "main",
                                      this->lowering.module_.get());
  this->lowering.functionBuilder().setFunctionAttributes(main);
  this->lowering.functionBuilder().beginFunction(main, nullptr);
  if (!asyncMain) {
    this->lowering.debugInfo().beginTopLevel(main);
  }
  this->mainInit = this->lowering.builder.CreateCall(this->lowering.rt_.init);
  this->lowering.functionFrame().pushScope();
  llvm::Type *voidTy = this->lowering.builder.getVoidTy();
  llvm::Type *ptr = this->lowering.builder.getPtrTy();
  if (asyncMain) {
    llvm::Value *promise = this->lowering.builder.CreateCall(asyncMain, {}, "main");
    this->lowering.builder.CreateCall(
        this->lowering.asyncLowerer().asyncFunction("tz_async_main", voidTy, {ptr}), {promise});
    this->lowering.builder.CreateCall(this->lowering.instanceReleaseFn, {promise});
  } else {
    for (Stmt *item : file.items) {
      if (!isa<FunctionDecl>(item)) {
        this->lowering.stmtLowerer().emitStmt(item);
      }
    }
    if (this->lowering.usesAsync && this->lowering.functionFrame().blockIsOpen()) {
      this->lowering.builder.CreateCall(
          this->lowering.asyncLowerer().asyncFunction("tz_async_run", voidTy,
                                                      {this->lowering.builder.getInt32Ty()}),
          {this->lowering.builder.getInt32(this->lowering.esModule)});
    }
  }
  if (this->lowering.functionFrame().blockIsOpen()) {
    this->lowering.builder.CreateCall(this->lowering.rt_.mainDone);
  }
  this->lowering.functionFrame().popScope();
  if (this->lowering.functionFrame().blockIsOpen()) {
    for (const VarSymbol *var : llvm::reverse(this->managedGlobals)) {
      llvm::Type *lowered = this->lower(var->type);
      llvm::Value *address = this->lowering.functionFrame().addressOf(var);
      llvm::Value *value = this->lowering.builder.CreateLoad(lowered, address);
      this->lowering.builder.CreateStore(llvm::Constant::getNullValue(lowered), address);
      if (this->lowering.isEternal(var)) {
        this->lowering.publishLowerer().endValue(value, var->type);
      } else {
        this->lowering.refCounts().releaseValue(value, var->type);
      }
    }
    if (this->lowering.usesAsync) {
      this->lowering.builder.CreateCall(
          this->lowering.asyncLowerer().asyncFunction("tz_async_shutdown", voidTy, {}));
    }
    this->lowering.builder.CreateCall(this->lowering.rt_.shutdown);
  }
  this->lowering.functionBuilder().finishFunction();
}

}
