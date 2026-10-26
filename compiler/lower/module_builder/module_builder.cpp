#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/library/reflect/reflect_lowerer/reflect_lowerer.h"
#include "compiler/library/result/result_lowerer/result_lowerer.h"
#include "compiler/lower/async/async_lowerer/async_lowerer.h"
#include "compiler/lower/classes/class_descriptors/class_descriptors.h"
#include "compiler/lower/classes/decorator_lowerer/decorator_lowerer.h"
#include "compiler/lower/classes/inherit_lowerer/inherit_lowerer.h"
#include "compiler/lower/errors/error_lowerer/error_lowerer.h"
#include "compiler/lower/errors/may_throw/may_throw.h"
#include "compiler/lower/function_builder/function_builder.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/functions/closure_lowerer/closure_lowerer.h"
#include "compiler/lower/functions/tail_call_lowerer/tail_call_lowerer.h"
#include "compiler/lower/iterators/generator_lowerer/generator_lowerer.h"
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
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/ErrorHandling.h"
#include <array>

using llvm::BasicBlock;
using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::Value;

namespace tz {

ModuleBuilder::ModuleBuilder(Lowering &lowering) : lowering_(lowering) {}

std::unique_ptr<llvm::Module> ModuleBuilder::generate(SourceFile &file) {
  lowering_.comparesFunctions_ = file.comparesFunctions;
  if (lowering_.parallel_) {
    for (const Module *module : file.modules) {
      if (!module->builtin) {
        continue;
      }
      for (Stmt *item : module->file.items) {
        if (auto *cls = dyn_cast<ClassDecl>(item)) {
          for (const ClassField &f : cls->fields) {
            if (f.isStatic && f.var) {
              lowering_.libraryState_.insert(f.var);
            }
          }
          continue;
        }
        if (auto *decl = dyn_cast<VarDeclStmt>(item)) {
          for (VarDeclarator &d : decl->decls) {
            const Type *t = d.sym->type;
            bool value = t && (t->isNumeric() || t->isBool() || t->isString());
            if (d.sym->usedInFunction && !(d.sym->isConst && value)) {
              lowering_.libraryState_.insert(d.sym);
            }
          }
        }
      }
    }
  }
  lowering_.usesTls_ = file.usesTls;
  lowering_.subtypes_ = file.subtypes;
  lowering_.usesAsync_ = file.usesAsync;
  lowering_.esModule_ = file.topLevelAwait;
  for (const Module *module : file.modules) {
    if (module->isEntry &&
        (llvm::any_of(module->file.imports, [](const ImportDecl &i) { return !i.implicit; }) ||
         !module->file.exports.empty())) {
      lowering_.esModule_ = true;
    }
  }
  lowering_.runtimeFunctions().declareRuntime();
  lowering_.debugMemory_ = getenv("TZ_CHECK_LEAKS") != nullptr;
  lowering_.runtimeFunctions().defineRefCountHelpers();
  lowering_.inheritLowerer().prepareDispatch(file);
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
      lowering_.symbolPrefixes_[item] = prefix;
    }
  }
  std::vector<FunctionDecl *> functions;
  std::vector<ClassDecl *> classes;
  for (Stmt *item : file.items) {
    if (auto *fn = dyn_cast<FunctionDecl>(item);
        fn && fn->typeParams.empty() && !file.unusedFunctions.contains(fn)) {
      functions.push_back(fn);
    }
    if (auto *cls = dyn_cast<ClassDecl>(item)) {
      if (cls->isEmitted()) {
        classes.push_back(cls);
      }
      if (!cls->generic) {
        for (FunctionDecl *fn : cls->methods) {
          if (fn->isStatic && fn->typeParams.empty()) {
            functions.push_back(fn);
          }
        }
      }
    }
  }
  functions.insert(functions.end(), file.instances.begin(), file.instances.end());
  lowering_.emittedClasses_.assign(classes.begin(), classes.end());
  for (FunctionDecl *fn : functions) {
    lowering_.functionBuilder().declareFunction(fn);
  }
  for (ClassDecl *cls : classes) {
    lowering_.classDescriptors().declareMethods(cls);
  }
  lowering_.tailCallLowerer().findTailGroups(functions);
  lowering_.mayThrow().analyzeThrows(file);
  declareGlobals(file);
  for (FunctionDecl *fn : functions) {
    lowering_.functionBuilder().emitFunction(fn);
  }
  for (ClassDecl *cls : classes) {
    lowering_.classDescriptors().emitMethods(cls);
  }
  emitMain(file);
  while (!lowering_.pendingClosures_.empty() || !lowering_.pendingIterators_.empty()) {
    lowering_.closureLowerer().emitPendingClosures();
    lowering_.generatorLowerer().emitPendingIterators();
  }
  lowering_.reflectLowerer().finishReflection(file, mainInit_);
  lowering_.decoratorLowerer().finishMetadata(file, mainInit_);
  lowering_.memoryDebug().finishGroupDebug(mainInit_);
  lowering_.publishLowerer().finishPublishTable(mainInit_);

  return std::move(lowering_.module_);
}

llvm::Type *ModuleBuilder::lower(const Type *type) {
  switch (type->kind()) {
  case Type::Kind::Void:
    return lowering_.builder_.getVoidTy();
  case Type::Kind::Undefined:
    return lowering_.builder_.getInt1Ty();
  case Type::Kind::Bool:
    return lowering_.builder_.getInt1Ty();
  case Type::Kind::Int:
    return lowering_.builder_.getInt32Ty();
  case Type::Kind::Long:
    return lowering_.builder_.getInt64Ty();
  case Type::Kind::Double:
    return lowering_.builder_.getDoubleTy();
  case Type::Kind::Decimal:
    return lowering_.decTy_;
  case Type::Kind::String:
    return lowering_.strTy_;
  case Type::Kind::Array:
    return lowering_.builder_.getPtrTy();
  case Type::Kind::Optional:
    if (UnionLayout::zeroIsUndefined(type->inner())) {
      return lower(type->inner());
    }
    return llvm::StructType::get(lowering_.ctx_,
                                 {lower(type->inner()), lowering_.builder_.getInt1Ty()});
  case Type::Kind::Literal:
  case Type::Kind::Union:
    return lowering_.unionLayout().lowerUnion(type);
  case Type::Kind::Function:
    return lowering_.fnTy_;
  case Type::Kind::Object:
    return lowering_.builder_.getPtrTy();
  case Type::Kind::Result:
    return lowering_.resultLowerer().resultLayout(type).type;
  case Type::Kind::Never:
    return llvm::StructType::get(lowering_.ctx_);
  case Type::Kind::Iterator:
    return lowering_.builder_.getPtrTy();
  case Type::Kind::Json:
    return lowering_.builder_.getPtrTy();
  case Type::Kind::Dict:
    return lowering_.builder_.getPtrTy();
  case Type::Kind::Promise:
    return lowering_.builder_.getPtrTy();
  case Type::Kind::Unknown:
    return lowering_.builder_.getPtrTy();
  case Type::Kind::ClassRef:
    return lowering_.builder_.getPtrTy();
  case Type::Kind::Error:
    break;
  }
  llvm_unreachable("an erroneous type reached code generation");
}

void ModuleBuilder::declareGlobals(SourceFile &file) {
  for (Stmt *item : file.items) {
    if (auto *cls = dyn_cast<ClassDecl>(item)) {
      for (const ClassField &f : cls->fields) {
        if (!f.isStatic || !f.var) {
          continue;
        }
        llvm::Type *type = lower(f.var->type);
        lowering_.globals_[f.var] =
            cls->usedAsValue
                ? lowering_.unknownLowerer().staticFieldAddress(cls, f.var)
                : new llvm::GlobalVariable(
                      *lowering_.module_, type, false, llvm::GlobalValue::InternalLinkage,
                      llvm::Constant::getNullValue(type),
                      "tz." + lowering_.symbolPrefixes_.lookup(cls) + f.var->name);
        if (f.var->type->isManaged()) {
          managedGlobals_.push_back(f.var);
        }
      }
      for (FunctionDecl *fn : cls->methods) {
        if (const VarSymbol *impl = fn->impl) {
          llvm::Constant *adapter =
              cast<llvm::Constant>(lowering_.closureLowerer().makeFunctionValue(
                  lowering_.decoratorLowerer().implAdapterInfo(fn),
                  llvm::ConstantPointerNull::get(lowering_.builder_.getPtrTy())));
          lowering_.globals_[impl] =
              new llvm::GlobalVariable(*lowering_.module_, lower(impl->type), false,
                                       llvm::GlobalValue::InternalLinkage, adapter,
                                       "tz." + lowering_.symbolPrefixes_.lookup(cls) +
                                           cls->name.str() + "." + fn->name.str() + ".impl");
          managedGlobals_.push_back(impl);
        }
      }
      continue;
    }
    auto *decl = dyn_cast<VarDeclStmt>(item);
    if (!decl) {
      continue;
    }
    for (VarDeclarator &d : decl->decls) {
      if (d.alias || !d.sym->usedInFunction) {
        continue;
      }
      llvm::Type *type = lower(d.sym->type);
      lowering_.globals_[d.sym] = new llvm::GlobalVariable(
          *lowering_.module_, type, false, llvm::GlobalValue::InternalLinkage,
          llvm::Constant::getNullValue(type),
          "tz." + lowering_.symbolPrefixes_.lookup(decl) + d.name);
      if (d.sym->type->isManaged()) {
        managedGlobals_.push_back(d.sym);
      }
    }
  }
}

void ModuleBuilder::emitMain(SourceFile &file) {
  llvm::Function *asyncMain =
      file.topLevelAwait ? lowering_.asyncLowerer().emitAsyncMain(file) : nullptr;
  auto *type = llvm::FunctionType::get(lowering_.builder_.getInt32Ty(), false);
  auto *main = llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage, "main",
                                      lowering_.module_.get());
  lowering_.functionBuilder().setFunctionAttributes(main);
  lowering_.functionBuilder().beginFunction(main, nullptr);
  mainInit_ = lowering_.builder_.CreateCall(lowering_.rt_.init);
  lowering_.errorLowerer().emitErrorSupport();
  lowering_.functionFrame().pushScope();
  llvm::Type *voidTy = lowering_.builder_.getVoidTy(), *ptr = lowering_.builder_.getPtrTy();
  if (asyncMain) {
    llvm::Value *promise = lowering_.builder_.CreateCall(asyncMain, {}, "main");
    lowering_.builder_.CreateCall(
        lowering_.asyncLowerer().asyncFunction("tz_async_main", voidTy, {ptr}), {promise});
    lowering_.builder_.CreateCall(lowering_.instanceReleaseFn_, {promise});
  } else {
    for (Stmt *item : file.items) {
      if (!isa<FunctionDecl>(item)) {
        lowering_.stmtLowerer().emitStmt(item);
      }
    }
    if (lowering_.usesAsync_ && lowering_.functionFrame().blockIsOpen()) {
      lowering_.builder_.CreateCall(lowering_.asyncLowerer().asyncFunction(
                                        "tz_async_run", voidTy, {lowering_.builder_.getInt32Ty()}),
                                    {lowering_.builder_.getInt32(lowering_.esModule_)});
    }
  }
  if (lowering_.functionFrame().blockIsOpen()) {
    lowering_.builder_.CreateCall(lowering_.rt_.mainDone);
  }
  lowering_.functionFrame().popScope();
  if (lowering_.functionFrame().blockIsOpen()) {
    for (const VarSymbol *var : llvm::reverse(managedGlobals_)) {
      llvm::Type *lowered = lower(var->type);
      llvm::Value *address = lowering_.functionFrame().addressOf(var);
      llvm::Value *value = lowering_.builder_.CreateLoad(lowered, address);
      lowering_.builder_.CreateStore(llvm::Constant::getNullValue(lowered), address);
      if (lowering_.isEternal(var)) {
        lowering_.publishLowerer().endValue(value, var->type);
      } else {
        lowering_.refCounts().releaseValue(value, var->type);
      }
    }
    if (lowering_.usesAsync_) {
      lowering_.builder_.CreateCall(
          lowering_.asyncLowerer().asyncFunction("tz_async_shutdown", voidTy, {}));
    }
    lowering_.builder_.CreateCall(lowering_.rt_.shutdown);
  }
  lowering_.functionBuilder().finishFunction();
}

}
