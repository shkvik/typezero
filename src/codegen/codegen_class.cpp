// Classes: methods, constructors, `new`, static fields, `instanceof`, and
// object literals with methods.
//
// An instance is an object like any other (see codegen_object.cpp); its
// descriptor also names the class and gives its number. A method is a
// function (ptr this, params...) -> ret, the shape of the code of a closure,
// so the object is the method's environment: `o.m` as a value is the
// function value { info of m, o }, and `o.m == o.m`. A call is direct, and
// the method borrows the object, which the caller keeps alive.
//
// `new C(args)` allocates the object with every field empty, runs the
// initializers of the fields (the method `init`) and then the constructor,
// which set every field (Sema checked that).
//
// An object literal with methods is an instance of a class of its own. Its
// methods are closures whose environment is the object: the variables they
// capture are stored in it after its fields.

#include "codegen/codegen.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

namespace {

// A field that is a method (a method of a literal, or an arrow in a field of
// a class): node prints it as `[Function: name]`. TZ_FIELD_METHOD in
// runtime/tzrt.h.
constexpr uint32_t kMethodField = 0xFFFFFFFFu;

} // namespace

// A variable a method can neither change nor release while it runs, so the
// object in it can be lent to the method.
bool staysDuringCall(const Expr *expr) {
  while (auto *conv = dyn_cast<CastExpr>(expr)) {
    if (!conv->isImplicit() || conv->shortCircuit || !conv->operand->type->isOptional())
      return false;
    expr = conv->operand;
  }
  auto *name = dyn_cast<NameExpr>(expr);
  if (!name || !name->var)
    return false;
  const VarSymbol *var = name->var;
  return var->isConst || (!var->boxed() && !(var->isGlobal && var->usedInFunction));
}

namespace {

// The functions of a class that take `this`: its methods, constructors, the
// initializers of its fields and its arrow methods. An abstract method has
// no code.
std::vector<FunctionDecl *> instanceMethods(const ClassDecl *cls) {
  std::vector<FunctionDecl *> methods;
  // A generic method is emitted as its copies, which calls need.
  for (FunctionDecl *fn : cls->methods)
    if (!fn->isStatic && !fn->isAbstract && fn->typeParams.empty())
      methods.push_back(fn);
  methods.insert(methods.end(), cls->methodInstances.begin(), cls->methodInstances.end());
  methods.insert(methods.end(), cls->constructors.begin(), cls->constructors.end());
  if (cls->init)
    methods.push_back(cls->init);
  for (const ClassField &f : cls->fields)
    if (f.method)
      methods.push_back(f.method);
  return methods;
}

} // namespace

llvm::Constant *CodeGen::dropDescriptor(llvm::Function *drop) {
  llvm::Constant *&descriptor = dropDescriptors_[drop];
  if (!descriptor) {
    llvm::Type *ptr = builder_.getPtrTy();
    auto *type = llvm::StructType::get(ctx_, llvm::ArrayRef<llvm::Type *>(ptr));
    llvm::Constant *init = llvm::ConstantStruct::get(type, {drop});
    // The parallel runtime reads the flags of any descriptor (publish.c): a
    // whole layout, without fields.
    if (parallel_) {
      llvm::Constant *null = llvm::ConstantPointerNull::get(ptr);
      type = layoutDescTy_;
      init = llvm::ConstantStruct::get(
          layoutDescTy_, {drop, null, builder_.getInt32(0), builder_.getInt32(0), null,
                          builder_.getInt32(0), builder_.getInt32(0)});
    }
    auto *global = new llvm::GlobalVariable(*module_, type, /*isConstant=*/true,
                                            llvm::GlobalValue::PrivateLinkage, init,
                                            drop->getName() + ".descriptor");
    global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
    descriptor = global;
  }
  return descriptor;
}

llvm::Function *CodeGen::methodCode(const FunctionDecl *fn) {
  if (fn->cls->literal)
    return closureCode(const_cast<FunctionDecl *>(fn));
  return functions_.lookup(fn->sym);
}

void CodeGen::declareMethods(ClassDecl *cls) {
  // An instance of a generic class by its type arguments: `tz.Box<int>.get`.
  const ClassDecl *named = cls->generic ? cls->generic : cls;
  std::string prefix = "tz." + symbolPrefixes_.lookup(named) +
                       (cls->generic ? cls->type->name().str() : cls->name.str()) + ".";
  for (FunctionDecl *fn : instanceMethods(cls)) {
    std::string name = prefix + fn->name.str();
    if (fn->variant)
      name += "." + std::to_string(fn->variant);
    // A copy of a generic method, by its type arguments.
    if (fn->isInstance) {
      name += "<";
      for (size_t i = 0; i < fn->typeArgs.size(); ++i)
        name += (i > 0 ? ", " : "") + (fn->typeArgs[i]->isFunction() ? std::string("function")
                                                                      : fn->typeArgs[i]->name().str());
      name += ">";
    }
    auto *function = llvm::Function::Create(closureFnType(functionTypeOf(fn->sym)),
                                            llvm::GlobalValue::InternalLinkage, name, module_.get());
    setFunctionAttributes(function);
    functions_[fn->sym] = function;
  }
}

void CodeGen::emitMethods(ClassDecl *cls) {
  if (cls->builtin) {
    for (FunctionDecl *ctor : cls->constructors)
      emitErrorConstructor(ctor);
    return;
  }
  for (FunctionDecl *fn : instanceMethods(cls)) {
    if (fn->impl) { // a decorator may replace it
      emitDecoratedMethod(fn);
      continue;
    }
    if (fn->isGenerator) { // `*items()`: its object is the environment
      emitGenerator(fn, functions_.lookup(fn->sym));
      continue;
    }
    if (fn->isAsync) {
      emitAsyncFunction(fn, functions_.lookup(fn->sym));
      continue;
    }
    beginFunction(functions_.lookup(fn->sym), fn);
    envParam_ = function_->getArg(0);
    envParam_->setName("this");
    std::vector<llvm::Value *> args;
    for (unsigned i = 1; i < function_->arg_size(); ++i)
      args.push_back(function_->getArg(i));
    emitParamsAndBody(fn, args);
    finishFunction();
  }
}

llvm::Constant *CodeGen::classDescriptor(const ClassDecl *cls) {
  llvm::Constant *&descriptor = classDescriptors_[cls];
  if (descriptor)
    return descriptor;
  const Type *type = cls->type;
  const ObjectLayout &layout = objectLayout(type);
  const llvm::StructLayout *offsets = module_->getDataLayout().getStructLayout(layout.type);
  // The keys in the order node prints them: those of the classes up the
  // chain first, as their constructors set them; then the fields as
  // declared, a method of a literal or an arrow method where it is written;
  // not the `#private` fields, which are no keys.
  std::vector<llvm::Constant *> entries;
  // `shown` is the key it prints as, if not its name.
  auto field = [&](llvm::StringRef name, llvm::StringRef shown = {}) {
    unsigned i = fieldIndex(type, name);
    uint64_t offset = offsets->getElementOffset(layout.elements[i]);
    if (shown.empty())
      shown = name;
    entries.push_back(llvm::ConstantStruct::get(
        fieldDescTy_, {utf8Constant(shown), builder_.getInt32(uint32_t(shown.size())),
                       builder_.getInt32(uint32_t(offset)), typeDescriptor(type->fields()[i].type)}));
  };
  // An emitter of node:events (src/lib/events.ts) prints as node's: its
  // private fields under node's names, those that are symbols in node last
  // (TZ_LAYOUT_LIBRARY in runtime/tzrt.h).
  bool emitter = false;
  for (const ClassDecl *c = cls; c && !emitter; c = c->parent)
    emitter = c->library && c->name == "EventEmitterBase";
  bool library = emitter || (cls->library && (cls->name == "OnceWrapper" ||
                                               cls->name == "AsyncListener" ||
                                               cls->name == "ThisListener" ||
                                               cls->name == "ListenerList" ||
                                               cls->name == "CryptoKey"));
  auto method = [&](llvm::StringRef name, const FunctionDecl *fn) {
    entries.push_back(llvm::ConstantStruct::get(
        fieldDescTy_, {utf8Constant(name), builder_.getInt32(uint32_t(name.size())),
                       builder_.getInt32(kMethodField), typeDescriptor(functionTypeOf(fn->sym))}));
  };
  if (cls->literal) {
    for (const ObjectLitExpr::Property &prop : cls->literal->props)
      if (prop.method)
        method(prop.name, cast<FunctionExpr>(prop.value)->fn);
      else
        field(prop.name);
  } else {
    std::vector<const ClassDecl *> chain;
    for (const ClassDecl *c = cls; c; c = c->parent)
      chain.insert(chain.begin(), c);
    // A field declared again (or an arrow overridden) keeps its place.
    std::vector<std::pair<llvm::StringRef, const FunctionDecl *>> keys;
    if (emitter) {
      field("#events", "_events");
      field("#eventsCount", "_eventsCount");
      field("#maxListeners", "_maxListeners");
    }
    // Nor a field marked `@__tz_hidden` (ClassField::hidden).
    for (const ClassDecl *c : chain)
      for (const ClassField &f : c->fields) {
        if (f.isStatic || f.name.starts_with("#") || f.hidden)
          continue;
        auto it = llvm::find_if(keys, [&](const auto &key) { return key.first == f.name; });
        if (it == keys.end())
          keys.push_back({f.name, f.method});
        else if (f.method)
          it->second = f.method;
      }
    for (auto [name, fn] : keys)
      if (fn)
        method(name, fn);
      else
        field(name);
    if (emitter) {
      field("#shapeMode", "Symbol(shapeMode)");
      field("#capture", "Symbol(kCapture)");
    }
    // Then the getters of a class of tz's library, which print as node's
    // properties (codegen_library.cpp).
    if (cls->library)
      appendGetterKeys(chain, entries);
  }
  std::string name = cls->name.empty() ? "object" : cls->name.str();
  llvm::Constant *fields = llvm::ConstantPointerNull::get(builder_.getPtrTy());
  if (!entries.empty()) {
    auto *arrayType = llvm::ArrayType::get(fieldDescTy_, entries.size());
    auto *global = new llvm::GlobalVariable(*module_, arrayType, /*isConstant=*/true,
                                            llvm::GlobalValue::PrivateLinkage,
                                            llvm::ConstantArray::get(arrayType, entries),
                                            "tz.fields." + name);
    global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
    fields = global;
  }
  // An object of a class without a name shows the nearest name up its
  // chain, as node shows it.
  llvm::StringRef shown = cls->name;
  for (const ClassDecl *c = cls; shown.empty() && c && c->expression; c = c->parent)
    shown = c->parent ? c->parent->name : llvm::StringRef();
  llvm::Constant *className = shown.empty() ? llvm::ConstantPointerNull::get(builder_.getPtrTy())
                                            : utf8Constant(shown);
  // An Error, and an object of a class derived from it, prints as Node
  // prints errors (TZ_LAYOUT_ERROR in runtime/tzrt.h).
  uint32_t flags = derivesFromError(cls) ? 512 : 0;
  if (library)
    flags |= 1073741824; // TZ_LAYOUT_LIBRARY
  llvm::Constant *base = llvm::ConstantStruct::get(
      layoutDescTy_,
      {layout.drop, fields, builder_.getInt32(uint32_t(entries.size())), builder_.getInt32(flags),
       className, builder_.getInt32(uint32_t(shown.size())), builder_.getInt32(cls->id)});
  // Then the tables of its interfaces, and its method table: the code and
  // the info of the method at each slot (none for an abstract one).
  llvm::Constant *null = llvm::ConstantPointerNull::get(builder_.getPtrTy());
  std::vector<llvm::Constant *> slots;
  for (FunctionDecl *fn : cls->vtable) {
    slots.push_back(fn ? methodCode(fn) : null);
    slots.push_back(fn ? functionInfo(fn) : null);
  }
  auto *slotsType = llvm::ArrayType::get(builder_.getPtrTy(), slots.size());
  auto *descType = llvm::StructType::get(ctx_, {layoutDescTy_, builder_.getPtrTy(), slotsType});
  assert(module_->getDataLayout().getStructLayout(descType)->getElementOffset(2) == kVtableOffset);
  auto *global = new llvm::GlobalVariable(
      *module_, descType, /*isConstant=*/true, llvm::GlobalValue::PrivateLinkage,
      llvm::ConstantStruct::get(
          descType, {base, itablesOf(type), llvm::ConstantArray::get(slotsType, slots)}),
      "tz.layout." + name);
  global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
  descriptor = global;
  return descriptor;
}

llvm::Value *CodeGen::allocInstance(const ClassDecl *cls) {
  return allocInstance(cls, isCyclic(cls->type));
}

llvm::Value *CodeGen::allocInstance(const ClassDecl *cls, bool member) {
  const ObjectLayout &layout = objectLayout(cls->type);
  uint64_t size = module_->getDataLayout().getTypeAllocSize(layout.type);
  llvm::Value *object;
  if (member) {
    object = allocMember(size, "object"); // counted by its group
  } else {
    object = builder_.CreateCall(rt_.objAlloc, {builder_.getInt64(size)}, "object");
    builder_.CreateStore(builder_.getInt64(1), builder_.CreateStructGEP(layout.type, object, 0));
  }
  builder_.CreateStore(classDescriptor(cls), builder_.CreateStructGEP(layout.type, object, 1));
  // The fields start empty, so that setting one releases nothing.
  if (size > 16)
    builder_.CreateMemSet(builder_.CreateConstInBoundsGEP1_64(builder_.getInt8Ty(), object, 16),
                          builder_.getInt8(0), size - 16, llvm::MaybeAlign(8));
  return object;
}

// The arguments are evaluated first, as in JavaScript; then the object is
// made and its code runs. If that throws, the object goes: its fields that
// are set are released with it.
llvm::Value *CodeGen::emitNew(NewExpr *expr) {
  const ClassDecl *cls = expr->cls;
  // A class as a value, known when compiling: what makes it runs first.
  if (expr->callee && !isa<NameExpr>(expr->callee))
    emitDiscarded(expr->callee);
  if (cls->builtin)
    return emitNewError(expr);
  const CallExpr *ctor = expr->ctor;
  std::vector<llvm::Value *> args = emitHeldValues(ctor->args);
  const FuncSymbol *sym = ctor->func;
  if (sym)
    for (size_t i = args.size(); i < sym->params.size(); ++i)
      args.push_back(undefinedValue(sym->params[i]));
  llvm::Value *object = allocInstance(cls, makesMember(expr, cls->type));
  noteUnstored(object, expr);
  // The object goes with what its code throws, released as the error leaves.
  Held held = hold(object, cls->type, constructionThrows(cls, sym));
  emitConstruct(object, cls, sym, args);
  return take(held);
}

bool CodeGen::constructionThrows(const ClassDecl *cls, const FuncSymbol *ctor) const {
  if (ctor && ctor->decl->cls == cls)
    return (!cls->parent && cls->init && mayThrow(cls->init)) || mayThrow(ctor->decl);
  return (cls->parent && constructionThrows(cls->parent, ctor)) || (cls->init && mayThrow(cls->init));
}

// A class with a constructor of its own runs it: in a class that extends
// another, its `super(...)` runs the classes up the chain and then the
// initializers. A class without one runs the class it extends with the
// arguments, and then its initializers, as JavaScript's implicit
// `constructor(...args) { super(...args) }` does. After each part that may
// throw, an error goes up.
void CodeGen::emitConstruct(llvm::Value *object, const ClassDecl *cls, const FuncSymbol *ctor,
                            llvm::ArrayRef<llvm::Value *> args) {
  if (ctor && ctor->decl->cls == cls) {
    std::vector<llvm::Value *> all{object};
    if (!cls->parent && cls->init) {
      // The arguments wait meanwhile, if the initializers may throw.
      bool initThrows = mayThrow(cls->init);
      std::vector<Held> waiting;
      for (size_t i = 0; i < args.size(); ++i)
        waiting.push_back(hold(args[i], ctor->params[i], initThrows));
      builder_.CreateCall(methodCode(cls->init), {object});
      emitThrowCheckIf(initThrows);
      for (Held &w : waiting)
        all.push_back(take(w));
    } else {
      all.insert(all.end(), args.begin(), args.end());
    }
    builder_.CreateCall(methodCode(ctor->decl), all);
    emitThrowCheckIf(mayThrow(ctor->decl));
    return;
  }
  if (cls->parent)
    emitConstruct(object, cls->parent, ctor, args);
  // A kind of Error (TypeError...) names the objects of the classes that
  // extend it, as its prototype does in JavaScript. The names are ASCII.
  if (cls->builtin && cls->parent)
    builder_.CreateStore(stringLiteral(std::u16string(cls->name.begin(), cls->name.end())),
                         fieldAddress(object, cls->type, "name"));
  if (cls->init) {
    builder_.CreateCall(methodCode(cls->init), {object});
    emitThrowCheckIf(mayThrow(cls->init));
  }
}

llvm::Value *CodeGen::emitSuperCall(CallExpr *call) {
  const ClassDecl *cls = currentFn_->cls;
  std::vector<llvm::Value *> args = emitHeldValues(call->args);
  if (const FuncSymbol *sym = call->method)
    for (size_t i = args.size(); i < sym->params.size(); ++i)
      args.push_back(undefinedValue(sym->params[i]));
  emitConstruct(envParam_, cls->parent, call->method, args);
  if (cls->init) {
    builder_.CreateCall(methodCode(cls->init), {envParam_});
    emitThrowCheckIf(mayThrow(cls->init));
  }
  return nullptr;
}

llvm::Value *CodeGen::emitMethodCall(CallExpr *call) {
  auto *member = cast<MemberExpr>(call->callee);
  const FuncSymbol *method = call->method;
  bool borrow = staysDuringCall(member->object) && llvm::all_of(call->args, cannotAssign);
  Operand self = emitOperand(member->object, borrow);
  holdOperand(self, llvm::any_of(call->args, [&](Expr *arg) { return exprThrows(arg); }));
  std::vector<llvm::Value *> values = emitHeldValues(call->args);
  takeOperand(self);
  std::vector<llvm::Value *> args{self.value};
  args.insert(args.end(), values.begin(), values.end());
  for (size_t i = call->args.size(); i < method->params.size(); ++i)
    args.push_back(undefinedValue(method->params[i]));
  // A method that classes down the chain override: the one of the object's
  // class, from its method table.
  llvm::CallInst *result =
      call->virtualCall
          ? builder_.CreateCall(closureFnType(functionTypeOf(method)),
                                vtableEntry(self.value, method->decl->slot, /*info=*/false), args)
          : builder_.CreateCall(methodCode(method->decl), args);
  // The object goes before what the method threw does.
  dropOperand(self);
  emitThrowCheckIf(callMayThrow(method->decl, call->virtualCall) && call != returnCall_);
  return result->getType()->isVoidTy() ? nullptr : result;
}

// The value holds a reference to the object, its environment.
llvm::Value *CodeGen::emitBoundMethod(MemberExpr *member) {
  llvm::Value *object = emitExpr(member->object);
  llvm::Value *info = member->virtualCall
                          ? vtableEntry(object, member->method->decl->slot, /*info=*/true)
                          : functionInfo(member->method->decl);
  return makeFunctionValue(info, object);
}

// One comparison of class numbers, after the check for undefined.
llvm::Value *CodeGen::emitInstanceOf(BinaryExpr *bin) {
  const ClassDecl *cls = bin->rhs->type->classDecl();
  const Type *type = bin->lhs->type;
  Operand value = emitOperand(bin->lhs, /*canBorrow=*/true);
  // A class known only when the program runs: the numbers its object holds
  // (TzClass: { rc, layout, parent, i32 id, i32 last_id, ... }).
  llvm::Value *first = nullptr, *last = nullptr;
  if (bin->rhs->type->isConstructor()) {
    llvm::Value *rhs = emitExpr(bin->rhs); // a class is never counted
    first = builder_.CreateLoad(builder_.getInt32Ty(),
                                builder_.CreateConstInBoundsGEP1_64(builder_.getInt8Ty(), rhs, 24),
                                "first");
    last = builder_.CreateLoad(builder_.getInt32Ty(),
                               builder_.CreateConstInBoundsGEP1_64(builder_.getInt8Ty(), rhs, 28),
                               "last");
  }
  auto classMatches = [&](llvm::Value *object) {
    if (first) {
      llvm::Value *descriptor = builder_.CreateLoad(
          builder_.getPtrTy(), builder_.CreateConstInBoundsGEP1_64(builder_.getInt8Ty(), object, 8),
          "descriptor");
      llvm::Value *id = builder_.CreateLoad(
          builder_.getInt32Ty(), builder_.CreateStructGEP(layoutDescTy_, descriptor, 6), "class");
      // A class of JavaScript (`String` of a `design:*` record) has no number:
      // nothing is one of its objects.
      return builder_.CreateAnd(
          builder_.CreateICmpNE(first, builder_.getInt32(0)),
          builder_.CreateICmpULE(builder_.CreateSub(id, first), builder_.CreateSub(last, first)));
    }
    // `instanceof Date` (or Map, Set) of a union: the numbers of its members
    // that Sema gave the right side.
    if (!cls) {
      const Type *those = bin->rhs->type;
      return emitIsOneOf(objectClassId(object),
                         those->isUnion() ? those->members() : llvm::ArrayRef<const Type *>(those));
    }
    llvm::Value *descriptor = builder_.CreateLoad(
        builder_.getPtrTy(), builder_.CreateConstInBoundsGEP1_64(builder_.getInt8Ty(), object, 8),
        "descriptor");
    llvm::Value *id = builder_.CreateLoad(
        builder_.getInt32Ty(), builder_.CreateStructGEP(layoutDescTy_, descriptor, 6), "class");
    if (cls->lastId <= cls->id)
      return builder_.CreateICmpEQ(id, builder_.getInt32(cls->id));
    // The classes down its chain have the numbers that follow its own.
    return builder_.CreateICmpULE(builder_.CreateSub(id, builder_.getInt32(cls->id)),
                                  builder_.getInt32(cls->lastId - cls->id));
  };
  llvm::Value *result;
  if (type->nonOptional()->isTagged()) {
    // Only in the slot of objects: a check of the tag first.
    llvm::Value *tagged = value.value;
    const Type *u = type->nonOptional();
    llvm::BasicBlock *testBlock = newBlock("instanceof.test");
    llvm::BasicBlock *endBlock = newBlock("instanceof.end");
    llvm::BasicBlock *entry = builder_.GetInsertBlock();
    builder_.CreateCondBr(
        builder_.CreateICmpEQ(unionTag(tagged), builder_.getInt32(unsigned(UnionSlot::Object))),
        testBlock, endBlock);
    switchTo(testBlock);
    llvm::Value *matches = classMatches(taggedPart(tagged, u, UnionSlot::Object));
    llvm::BasicBlock *testEnd = builder_.GetInsertBlock();
    builder_.CreateBr(endBlock);
    switchTo(endBlock);
    llvm::PHINode *phi = builder_.CreatePHI(builder_.getInt1Ty(), 2);
    phi->addIncoming(builder_.getFalse(), entry);
    phi->addIncoming(matches, testEnd);
    result = phi;
  } else if (!type->isOptional() && !type->isUnknown()) {
    result = classMatches(value.value);
  } else {
    llvm::Value *object = optionalValue(value.value, type);
    llvm::BasicBlock *testBlock = newBlock("instanceof.test");
    llvm::BasicBlock *endBlock = newBlock("instanceof.end");
    llvm::BasicBlock *entry = builder_.GetInsertBlock();
    builder_.CreateCondBr(optionalHas(value.value, type), testBlock, endBlock);
    switchTo(testBlock);
    llvm::Value *matches = classMatches(object);
    llvm::BasicBlock *testEnd = builder_.GetInsertBlock();
    builder_.CreateBr(endBlock);
    switchTo(endBlock);
    llvm::PHINode *phi = builder_.CreatePHI(builder_.getInt1Ty(), 2);
    phi->addIncoming(builder_.getFalse(), entry);
    phi->addIncoming(matches, testEnd);
    result = phi;
  }
  dropOperand(value);
  return result;
}

// The fields in the order written, then what the methods capture, as a
// closure's environment holds it.
llvm::Value *CodeGen::emitMethodLiteral(ObjectLitExpr *lit) {
  const ClassDecl *cls = lit->cls;
  const Type *type = cls->type;
  const ObjectLayout &layout = objectLayout(type);
  uint64_t size = module_->getDataLayout().getTypeAllocSize(layout.type);
  bool member = makesMember(lit, type);
  llvm::Value *object;
  if (member) {
    object = allocMember(size, "object"); // counted by its group
  } else {
    object = builder_.CreateCall(rt_.objAlloc, {builder_.getInt64(size)}, "object");
    builder_.CreateStore(builder_.getInt64(1), builder_.CreateStructGEP(layout.type, object, 0));
  }
  builder_.CreateStore(classDescriptor(cls), builder_.CreateStructGEP(layout.type, object, 1));
  noteUnstored(object, lit);
  // Nothing can reach the new object while its values are evaluated.
  for (const ObjectLitExpr::Property &prop : lit->props) {
    if (prop.method)
      continue;
    llvm::Value *value = emitExpr(prop.value);
    builder_.CreateStore(value, fieldAddress(object, type, prop.name));
    if (member)
      groupStore(object, value, type->field(prop.name)->type);
  }
  const std::vector<VarSymbol *> &captures = cls->group->captures;
  for (size_t i = 0; i < captures.size(); ++i) {
    const VarSymbol *var = captures[i];
    llvm::Value *value;
    if (var->boxed()) {
      value = boxPointer(var);
      builder_.CreateCall(objRetainFn_, {value});
      if (member)
        builder_.CreateCall(
            groupFunction("tz_group_store", builder_.getVoidTy(), {builder_.getPtrTy(), builder_.getPtrTy()}),
            {object, value});
    } else {
      value = emitVarValue(var, /*borrow=*/false);
      if (member)
        groupStore(object, value, typeOf(var));
    }
    builder_.CreateStore(value,
                         builder_.CreateStructGEP(layout.type, object, unsigned(layout.captures + i)));
  }
  for (FunctionDecl *fn : cls->methods)
    closureCode(fn);
  return object;
}

void CodeGen::emitStaticFields(ClassDecl *cls) {
  for (const ClassField &f : cls->fields)
    if (f.isStatic && f.var && f.init) {
      llvm::Value *value = emitExpr(f.init);
      publishValue(value, f.var->type);
      storeReplacing(addressOf(f.var), value, f.var->type);
      if (isEternal(f.var))
        pinValue(value, f.var->type);
    }
}

} // namespace tz
