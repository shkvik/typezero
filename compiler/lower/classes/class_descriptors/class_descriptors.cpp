#include "compiler/lower/classes/class_descriptors/class_descriptors.h"
#include "compiler/lower/async/async_lowerer/async_lowerer.h"
#include "compiler/lower/classes/decorator_lowerer/decorator_lowerer.h"
#include "compiler/lower/classes/inherit_lowerer/inherit_lowerer.h"
#include "compiler/lower/function_builder/function_builder.h"
#include "compiler/lower/functions/closure_lowerer/closure_lowerer.h"
#include "compiler/lower/iterators/generator_lowerer/generator_lowerer.h"
#include "compiler/lower/layouts/object_layouts/object_layouts.h"
#include "compiler/lower/layouts/type_descriptors/type_descriptors.h"
#include "compiler/lower/library/tz_modules/tz_modules_lowerer/tz_modules_lowerer.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "llvm/ADT/STLExtras.h"

using llvm::cast;

namespace tz {

namespace {

constexpr uint32_t kMethodField = 0xFFFFFFFFu;

}

ClassDescriptors::ClassDescriptors(Lowering &owner)
    : lowering(owner) {}

std::vector<FunctionDecl *> ClassDescriptors::instanceMethods(const ClassDecl *cls) {
  std::vector<FunctionDecl *> methods;
  for (FunctionDecl *fn : cls->methods) {
    if (!fn->isStatic() && !fn->isAbstract() && fn->typeParams.empty()) {
      methods.push_back(fn);
    }
  }
  methods.insert(methods.end(), cls->methodInstances.begin(), cls->methodInstances.end());
  methods.insert(methods.end(), cls->constructors.begin(), cls->constructors.end());
  if (cls->init) {
    methods.push_back(cls->init);
  }
  for (const ClassField &f : cls->fields) {
    if (f.method()) {
      methods.push_back(f.method());
    }
  }

  return methods;
}

llvm::Constant *ClassDescriptors::dropDescriptor(llvm::Function *drop) {
  llvm::Constant *&descriptor = this->lowering.dropDescriptors[drop];
  if (!descriptor) {
    llvm::Type *ptr = this->lowering.builder.getPtrTy();
    auto *type = llvm::StructType::get(this->lowering.ctx, llvm::ArrayRef<llvm::Type *>(ptr));
    llvm::Constant *init = llvm::ConstantStruct::get(type, {drop});
    if (this->lowering.parallel_) {
      llvm::Constant *null = llvm::ConstantPointerNull::get(ptr);
      type = this->lowering.layoutDescTy;
      init = llvm::ConstantStruct::get(
          this->lowering.layoutDescTy,
          {drop, null, this->lowering.builder.getInt32(0), this->lowering.builder.getInt32(0), null,
           this->lowering.builder.getInt32(0), this->lowering.builder.getInt32(0)});
    }
    auto *global = new llvm::GlobalVariable(*this->lowering.module_, type, true,
                                            llvm::GlobalValue::PrivateLinkage, init,
                                            drop->getName() + ".descriptor");
    global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
    descriptor = global;
  }

  return descriptor;
}

llvm::Function *ClassDescriptors::methodCode(const FunctionDecl *fn) {
  if (fn->cls()->literal()) {
    return this->lowering.closureLowerer().closureCode(const_cast<FunctionDecl *>(fn));
  }

  return this->lowering.functions_.lookup(fn->sym);
}

void ClassDescriptors::declareMethods(ClassDecl *cls) {
  const ClassDecl *named = cls->generic() ? cls->generic() : cls;
  std::string prefix = "tz." + this->lowering.symbolPrefixes.lookup(named) +
                       (cls->generic() ? cls->type->name().str() : cls->name.str()) + ".";
  for (FunctionDecl *fn : ClassDescriptors::instanceMethods(cls)) {
    std::string name = prefix + fn->name.str();
    if (fn->variant()) {
      name += "." + std::to_string(fn->variant());
    }
    if (fn->isInstance) {
      name += "<";
      for (size_t i = 0; i < fn->typeArgs.size(); ++i) {
        name +=
            (i > 0 ? ", " : "") + (fn->typeArgs[i]->isFunction() ? std::string("function")
                                                                 : fn->typeArgs[i]->name().str());
      }
      name += ">";
    }
    auto *function = llvm::Function::Create(
        this->lowering.closureLowerer().closureFnType(ClosureLowerer::functionTypeOf(fn->sym)),
        llvm::GlobalValue::InternalLinkage, name, this->lowering.module_.get());
    this->lowering.functionBuilder().setFunctionAttributes(function);
    this->lowering.functions_[fn->sym] = function;
  }
}

void ClassDescriptors::emitMethods(ClassDecl *cls) {
  if (cls->builtin()) {
    return;
  }
  for (FunctionDecl *fn : ClassDescriptors::instanceMethods(cls)) {
    if (fn->impl()) {
      this->lowering.decoratorLowerer().emitDecoratedMethod(fn);
      continue;
    }
    if (fn->isGenerator) {
      this->lowering.generatorLowerer().emitGenerator(fn,
                                                      this->lowering.functions_.lookup(fn->sym));
      continue;
    }
    if (fn->isAsync) {
      this->lowering.asyncLowerer().emitAsyncFunction(fn,
                                                      this->lowering.functions_.lookup(fn->sym));
      continue;
    }
    this->lowering.functionBuilder().beginFunction(this->lowering.functions_.lookup(fn->sym), fn);
    this->lowering.envParam = this->lowering.function->getArg(0);
    this->lowering.envParam->setName("this");
    std::vector<llvm::Value *> args;
    for (unsigned i = 1; i < this->lowering.function->arg_size(); ++i) {
      args.push_back(this->lowering.function->getArg(i));
    }
    this->lowering.functionBuilder().emitParamsAndBody(fn, args);
    this->lowering.functionBuilder().finishFunction();
  }
}

llvm::Constant *ClassDescriptors::classDescriptor(const ClassDecl *cls) {
  llvm::Constant *&descriptor = this->lowering.classDescriptors_[cls];
  if (descriptor) {
    return descriptor;
  }
  const Type *type = cls->type;
  const Lowering::ObjectLayout &layout = this->lowering.objectLayouts().objectLayout(type);
  const llvm::StructLayout *offsets =
      this->lowering.module_->getDataLayout().getStructLayout(layout.type);
  std::vector<llvm::Constant *> entries;
  auto field = [&](llvm::StringRef name, llvm::StringRef shown = {}) {
    unsigned i = ObjectLayouts::fieldIndex(type, name);
    uint64_t offset = offsets->getElementOffset(layout.elements[i]);
    if (shown.empty()) {
      shown = name;
    }
    entries.push_back(llvm::ConstantStruct::get(
        this->lowering.fieldDescTy,
        {this->lowering.stringLowerer().utf8Constant(shown),
         this->lowering.builder.getInt32(uint32_t(shown.size())),
         this->lowering.builder.getInt32(uint32_t(offset)),
         this->lowering.typeDescriptors().typeDescriptor(type->fields()[i].type)}));
  };
  bool emitter = false;
  for (const ClassDecl *c = cls; c && !emitter; c = c->parent) {
    emitter = c->library() && c->name == "EventEmitterBase";
  }
  bool library = emitter || cls->builtin() ||
                 (cls->library() && (cls->name == "OnceWrapper" || cls->name == "AsyncListener" ||
                                     cls->name == "ThisListener" || cls->name == "ListenerList" ||
                                     cls->name == "CryptoKey"));
  auto method = [&](llvm::StringRef name, const FunctionDecl *fn) {
    entries.push_back(llvm::ConstantStruct::get(
        this->lowering.fieldDescTy, {this->lowering.stringLowerer().utf8Constant(name),
                                     this->lowering.builder.getInt32(uint32_t(name.size())),
                                     this->lowering.builder.getInt32(kMethodField),
                                     this->lowering.typeDescriptors().typeDescriptor(
                                         ClosureLowerer::functionTypeOf(fn->sym))}));
  };
  if (cls->literal()) {
    for (const ObjectLitExpr::Property &prop : cls->literal()->props) {
      if (prop.method()) {
        method(prop.name, cast<FunctionExpr>(prop.value)->fn);
      } else {
        field(prop.name);
      }
    }
  } else {
    std::vector<const ClassDecl *> chain;
    for (const ClassDecl *c = cls; c; c = c->parent) {
      chain.insert(chain.begin(), c);
    }
    std::vector<std::pair<llvm::StringRef, const FunctionDecl *>> keys;
    auto symbol = [&](llvm::StringRef name) {
      return emitter && (name == "shapeMode" || name == "capture");
    };
    for (const ClassDecl *c : chain) {
      for (const ClassField &f : c->fields) {
        if (f.isStatic() || symbol(f.name)) {
          continue;
        }
        auto it = llvm::find_if(keys, [&](const auto &key) { return key.first == f.name; });
        if (it == keys.end()) {
          keys.push_back({f.name, f.method()});
        } else if (f.method()) {
          it->second = f.method();
        }
      }
    }
    for (auto [name, fn] : keys) {
      if (fn) {
        method(name, fn);
      } else {
        field(name);
      }
    }
    if (emitter) {
      field("shapeMode", "Symbol(shapeMode)");
      field("capture", "Symbol(kCapture)");
    }
    if (cls->library()) {
      this->lowering.tzModulesLowerer().appendPrintHooks(chain, entries);
    }
  }
  std::string name = cls->name.empty() ? "object" : cls->name.str();
  llvm::Constant *fields = llvm::ConstantPointerNull::get(this->lowering.builder.getPtrTy());
  if (!entries.empty()) {
    auto *arrayType = llvm::ArrayType::get(this->lowering.fieldDescTy, entries.size());
    auto *global = new llvm::GlobalVariable(
        *this->lowering.module_, arrayType, true, llvm::GlobalValue::PrivateLinkage,
        llvm::ConstantArray::get(arrayType, entries), "tz.fields." + name);
    global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
    fields = global;
  }
  llvm::StringRef shown = cls->name;
  for (const ClassDecl *c = cls; shown.empty() && c && c->expression(); c = c->parent) {
    shown = c->parent ? c->parent->name : llvm::StringRef();
  }
  llvm::Constant *className =
      shown.empty() ? llvm::ConstantPointerNull::get(this->lowering.builder.getPtrTy())
                    : this->lowering.stringLowerer().utf8Constant(shown);
  uint32_t flags = library ? 1073741824 : 0;
  llvm::Constant *base = llvm::ConstantStruct::get(
      this->lowering.layoutDescTy,
      {layout.drop, fields, this->lowering.builder.getInt32(uint32_t(entries.size())),
       this->lowering.builder.getInt32(flags), className,
       this->lowering.builder.getInt32(uint32_t(shown.size())),
       this->lowering.builder.getInt32(cls->id)});
  llvm::Constant *null = llvm::ConstantPointerNull::get(this->lowering.builder.getPtrTy());
  std::vector<llvm::Constant *> slots;
  for (FunctionDecl *fn : cls->vtable) {
    slots.push_back(fn ? this->methodCode(fn) : null);
    slots.push_back(fn ? this->lowering.closureLowerer().functionInfo(fn) : null);
  }
  auto *slotsType = llvm::ArrayType::get(this->lowering.builder.getPtrTy(), slots.size());
  auto *descType =
      llvm::StructType::get(this->lowering.ctx, {this->lowering.layoutDescTy,
                                                 this->lowering.builder.getPtrTy(), slotsType});
  assert(this->lowering.module_->getDataLayout().getStructLayout(descType)->getElementOffset(2) ==
         this->lowering.kVtableOffset);
  auto *global = new llvm::GlobalVariable(
      *this->lowering.module_, descType, true, llvm::GlobalValue::PrivateLinkage,
      llvm::ConstantStruct::get(descType, {base, this->lowering.inheritLowerer().itablesOf(type),
                                           llvm::ConstantArray::get(slotsType, slots)}),
      "tz.layout." + name);
  global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
  descriptor = global;

  return descriptor;
}

}
