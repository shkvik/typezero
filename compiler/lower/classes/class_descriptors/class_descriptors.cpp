#include "compiler/lower/classes/class_descriptors/class_descriptors.h"
#include "compiler/library/tz_modules/tz_modules_lowerer/tz_modules_lowerer.h"
#include "compiler/lower/async/async_lowerer/async_lowerer.h"
#include "compiler/lower/classes/decorator_lowerer/decorator_lowerer.h"
#include "compiler/lower/classes/inherit_lowerer/inherit_lowerer.h"
#include "compiler/lower/errors/error_lowerer/error_lowerer.h"
#include "compiler/lower/function_builder/function_builder.h"
#include "compiler/lower/functions/closure_lowerer/closure_lowerer.h"
#include "compiler/lower/iterators/generator_lowerer/generator_lowerer.h"
#include "compiler/lower/layouts/object_layouts/object_layouts.h"
#include "compiler/lower/layouts/type_descriptors/type_descriptors.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"

using llvm::BasicBlock;
using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::Value;

namespace tz {

namespace {

// A field that is a method (a method of a literal, or an arrow in a field of
// a class): node prints it as `[Function: name]`. TZ_FIELD_METHOD in
// runtime/core/tzrt/tzrt.h.
constexpr uint32_t kMethodField = 0xFFFFFFFFu;

// The functions of a class that take `this`: its methods, constructors, the
// initializers of its fields and its arrow methods. An abstract method has
// no code.
std::vector<FunctionDecl *> instanceMethods(const ClassDecl *cls) {
  std::vector<FunctionDecl *> methods;
  // A generic method is emitted as its copies, which calls need.
  for (FunctionDecl *fn : cls->methods) {
    if (!fn->isStatic && !fn->isAbstract && fn->typeParams.empty()) {
      methods.push_back(fn);
    }
  }
  methods.insert(methods.end(), cls->methodInstances.begin(), cls->methodInstances.end());
  methods.insert(methods.end(), cls->constructors.begin(), cls->constructors.end());
  if (cls->init) {
    methods.push_back(cls->init);
  }
  for (const ClassField &f : cls->fields) {
    if (f.method) {
      methods.push_back(f.method);
    }
  }

  return methods;
}

} // namespace

ClassDescriptors::ClassDescriptors(Lowering &lowering) : lowering_(lowering) {}

llvm::Constant *ClassDescriptors::dropDescriptor(llvm::Function *drop) {
  llvm::Constant *&descriptor = lowering_.dropDescriptors_[drop];
  if (!descriptor) {
    llvm::Type *ptr = lowering_.builder_.getPtrTy();
    auto *type = llvm::StructType::get(lowering_.ctx_, llvm::ArrayRef<llvm::Type *>(ptr));
    llvm::Constant *init = llvm::ConstantStruct::get(type, {drop});
    // The parallel runtime reads the flags of any descriptor (publish.c): a
    // whole layout, without fields.
    if (lowering_.parallel_) {
      llvm::Constant *null = llvm::ConstantPointerNull::get(ptr);
      type = lowering_.layoutDescTy_;
      init = llvm::ConstantStruct::get(
          lowering_.layoutDescTy_,
          {drop, null, lowering_.builder_.getInt32(0), lowering_.builder_.getInt32(0), null,
           lowering_.builder_.getInt32(0), lowering_.builder_.getInt32(0)});
    }
    auto *global = new llvm::GlobalVariable(*lowering_.module_, type, /*isConstant=*/true,
                                            llvm::GlobalValue::PrivateLinkage, init,
                                            drop->getName() + ".descriptor");
    global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
    descriptor = global;
  }

  return descriptor;
}

llvm::Function *ClassDescriptors::methodCode(const FunctionDecl *fn) {
  if (fn->cls->literal) {
    return lowering_.closureLowerer().closureCode(const_cast<FunctionDecl *>(fn));
  }

  return lowering_.functions_.lookup(fn->sym);
}

void ClassDescriptors::declareMethods(ClassDecl *cls) {
  // An instance of a generic class by its type arguments: `tz.Box<int>.get`.
  const ClassDecl *named = cls->generic ? cls->generic : cls;
  std::string prefix = "tz." + lowering_.symbolPrefixes_.lookup(named) +
                       (cls->generic ? cls->type->name().str() : cls->name.str()) + ".";
  for (FunctionDecl *fn : instanceMethods(cls)) {
    std::string name = prefix + fn->name.str();
    if (fn->variant) {
      name += "." + std::to_string(fn->variant);
    }
    // A copy of a generic method, by its type arguments.
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
        lowering_.closureLowerer().closureFnType(ClosureLowerer::functionTypeOf(fn->sym)),
        llvm::GlobalValue::InternalLinkage, name, lowering_.module_.get());
    lowering_.functionBuilder().setFunctionAttributes(function);
    lowering_.functions_[fn->sym] = function;
  }
}

void ClassDescriptors::emitMethods(ClassDecl *cls) {
  if (cls->builtin) {
    for (FunctionDecl *ctor : cls->constructors) {
      lowering_.errorLowerer().emitErrorConstructor(ctor);
    }
    return;
  }
  for (FunctionDecl *fn : instanceMethods(cls)) {
    if (fn->impl) { // a decorator may replace it
      lowering_.decoratorLowerer().emitDecoratedMethod(fn);
      continue;
    }
    if (fn->isGenerator) { // `*items()`: its object is the environment
      lowering_.generatorLowerer().emitGenerator(fn, lowering_.functions_.lookup(fn->sym));
      continue;
    }
    if (fn->isAsync) {
      lowering_.asyncLowerer().emitAsyncFunction(fn, lowering_.functions_.lookup(fn->sym));
      continue;
    }
    lowering_.functionBuilder().beginFunction(lowering_.functions_.lookup(fn->sym), fn);
    lowering_.envParam_ = lowering_.function_->getArg(0);
    lowering_.envParam_->setName("this");
    std::vector<llvm::Value *> args;
    for (unsigned i = 1; i < lowering_.function_->arg_size(); ++i) {
      args.push_back(lowering_.function_->getArg(i));
    }
    lowering_.functionBuilder().emitParamsAndBody(fn, args);
    lowering_.functionBuilder().finishFunction();
  }
}

llvm::Constant *ClassDescriptors::classDescriptor(const ClassDecl *cls) {
  llvm::Constant *&descriptor = lowering_.classDescriptors_[cls];
  if (descriptor) {
    return descriptor;
  }
  const Type *type = cls->type;
  const Lowering::ObjectLayout &layout = lowering_.objectLayouts().objectLayout(type);
  const llvm::StructLayout *offsets =
      lowering_.module_->getDataLayout().getStructLayout(layout.type);
  // The keys in the order node prints them: those of the classes up the
  // chain first, as their constructors set them; then the fields as
  // declared, a method of a literal or an arrow method where it is written;
  // not the `#private` fields, which are no keys.
  std::vector<llvm::Constant *> entries;
  // `shown` is the key it prints as, if not its name.
  auto field = [&](llvm::StringRef name, llvm::StringRef shown = {}) {
    unsigned i = ObjectLayouts::fieldIndex(type, name);
    uint64_t offset = offsets->getElementOffset(layout.elements[i]);
    if (shown.empty()) {
      shown = name;
    }
    entries.push_back(llvm::ConstantStruct::get(
        lowering_.fieldDescTy_,
        {lowering_.stringLowerer().utf8Constant(shown),
         lowering_.builder_.getInt32(uint32_t(shown.size())),
         lowering_.builder_.getInt32(uint32_t(offset)),
         lowering_.typeDescriptors().typeDescriptor(type->fields()[i].type)}));
  };
  // An emitter of node:events (library/events/events.tz) prints as node's: its
  // private fields under node's names, those that are symbols in node last
  // (TZ_LAYOUT_LIBRARY in runtime/core/tzrt/tzrt.h).
  bool emitter = false;
  for (const ClassDecl *c = cls; c && !emitter; c = c->parent) {
    emitter = c->library && c->name == "EventEmitterBase";
  }
  bool library =
      emitter || (cls->library && (cls->name == "OnceWrapper" || cls->name == "AsyncListener" ||
                                   cls->name == "ThisListener" || cls->name == "ListenerList" ||
                                   cls->name == "CryptoKey"));
  auto method = [&](llvm::StringRef name, const FunctionDecl *fn) {
    entries.push_back(llvm::ConstantStruct::get(
        lowering_.fieldDescTy_,
        {lowering_.stringLowerer().utf8Constant(name),
         lowering_.builder_.getInt32(uint32_t(name.size())),
         lowering_.builder_.getInt32(kMethodField),
         lowering_.typeDescriptors().typeDescriptor(ClosureLowerer::functionTypeOf(fn->sym))}));
  };
  if (cls->literal) {
    for (const ObjectLitExpr::Property &prop : cls->literal->props) {
      if (prop.method) {
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
    // A field declared again (or an arrow overridden) keeps its place.
    std::vector<std::pair<llvm::StringRef, const FunctionDecl *>> keys;
    if (emitter) {
      field("#events", "_events");
      field("#eventsCount", "_eventsCount");
      field("#maxListeners", "_maxListeners");
    }
    // Nor a field marked `@__tz_hidden` (ClassField::hidden).
    for (const ClassDecl *c : chain) {
      for (const ClassField &f : c->fields) {
        if (f.isStatic || f.name.starts_with("#") || f.hidden) {
          continue;
        }
        auto it = llvm::find_if(keys, [&](const auto &key) { return key.first == f.name; });
        if (it == keys.end()) {
          keys.push_back({f.name, f.method});
        } else if (f.method) {
          it->second = f.method;
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
      field("#shapeMode", "Symbol(shapeMode)");
      field("#capture", "Symbol(kCapture)");
    }
    // Then the getters of a class of tz's library, which print as node's
    // properties (codegen_library.cpp).
    if (cls->library) {
      lowering_.tzModulesLowerer().appendGetterKeys(chain, entries);
    }
  }
  std::string name = cls->name.empty() ? "object" : cls->name.str();
  llvm::Constant *fields = llvm::ConstantPointerNull::get(lowering_.builder_.getPtrTy());
  if (!entries.empty()) {
    auto *arrayType = llvm::ArrayType::get(lowering_.fieldDescTy_, entries.size());
    auto *global = new llvm::GlobalVariable(
        *lowering_.module_, arrayType, /*isConstant=*/true, llvm::GlobalValue::PrivateLinkage,
        llvm::ConstantArray::get(arrayType, entries), "tz.fields." + name);
    global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
    fields = global;
  }
  // An object of a class without a name shows the nearest name up its
  // chain, as node shows it.
  llvm::StringRef shown = cls->name;
  for (const ClassDecl *c = cls; shown.empty() && c && c->expression; c = c->parent) {
    shown = c->parent ? c->parent->name : llvm::StringRef();
  }
  llvm::Constant *className = shown.empty()
                                  ? llvm::ConstantPointerNull::get(lowering_.builder_.getPtrTy())
                                  : lowering_.stringLowerer().utf8Constant(shown);
  // An Error, and an object of a class derived from it, prints as Node
  // prints errors (TZ_LAYOUT_ERROR in runtime/core/tzrt/tzrt.h).
  uint32_t flags = derivesFromError(cls) ? 512 : 0;
  if (library) {
    flags |= 1073741824; // TZ_LAYOUT_LIBRARY
  }
  llvm::Constant *base = llvm::ConstantStruct::get(
      lowering_.layoutDescTy_,
      {layout.drop, fields, lowering_.builder_.getInt32(uint32_t(entries.size())),
       lowering_.builder_.getInt32(flags), className,
       lowering_.builder_.getInt32(uint32_t(shown.size())), lowering_.builder_.getInt32(cls->id)});
  // Then the tables of its interfaces, and its method table: the code and
  // the info of the method at each slot (none for an abstract one).
  llvm::Constant *null = llvm::ConstantPointerNull::get(lowering_.builder_.getPtrTy());
  std::vector<llvm::Constant *> slots;
  for (FunctionDecl *fn : cls->vtable) {
    slots.push_back(fn ? methodCode(fn) : null);
    slots.push_back(fn ? lowering_.closureLowerer().functionInfo(fn) : null);
  }
  auto *slotsType = llvm::ArrayType::get(lowering_.builder_.getPtrTy(), slots.size());
  auto *descType = llvm::StructType::get(
      lowering_.ctx_, {lowering_.layoutDescTy_, lowering_.builder_.getPtrTy(), slotsType});
  assert(lowering_.module_->getDataLayout().getStructLayout(descType)->getElementOffset(2) ==
         lowering_.kVtableOffset);
  auto *global = new llvm::GlobalVariable(
      *lowering_.module_, descType, /*isConstant=*/true, llvm::GlobalValue::PrivateLinkage,
      llvm::ConstantStruct::get(descType, {base, lowering_.inheritLowerer().itablesOf(type),
                                           llvm::ConstantArray::get(slotsType, slots)}),
      "tz.layout." + name);
  global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
  descriptor = global;

  return descriptor;
}

} // namespace tz
