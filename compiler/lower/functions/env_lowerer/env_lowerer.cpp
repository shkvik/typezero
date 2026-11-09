#include "compiler/lower/functions/env_lowerer/env_lowerer.h"
#include "compiler/lower/classes/class_descriptors/class_descriptors.h"
#include "compiler/lower/debug_info/debug_info.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/functions/closure_lowerer/closure_lowerer.h"
#include "compiler/lower/layouts/object_layouts/object_layouts.h"
#include "compiler/lower/memory/group_lowerer/group_lowerer.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/memory/store_lowerer/store_lowerer.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/memory/memory_plan/memory_plan.h"
#include "llvm/ADT/STLExtras.h"

using llvm::cast;

namespace tz {

namespace {

constexpr uint64_t kStackObjectCount = uint64_t(1) << 62;

}

EnvLowerer::EnvLowerer(Lowering &owner)
    : lowering(owner) {}

std::string EnvLowerer::qualifiedName(const FunctionDecl *fn) {
  std::string name = fn->name.empty() ? "lambda" : fn->name.str();
  for (const FunctionDecl *p = fn->parent; p; p = p->parent) {
    name = (p->name.empty() ? "lambda" : p->name.str()) + "." + name;
  }

  return "tz." + name;
}

const Lowering::EnvLayout &EnvLowerer::envLayout(const FunctionDecl *fn) {
  const void *key = fn->group ? static_cast<const void *>(fn->group) : fn;
  auto [it, inserted] = this->lowering.envLayouts.try_emplace(key);
  Lowering::EnvLayout &layout = it->second;
  if (!inserted) {
    return layout;
  }
  const std::vector<VarSymbol *> &captures = fn->group ? fn->group->captures : fn->captures;
  layout.fields.assign(captures.begin(), captures.end());
  layout.key = key;
  layout.member = this->lowering.isCyclicEnv(key);
  std::vector<llvm::Type *> types{this->lowering.builder.getInt64Ty(),
                                  this->lowering.builder.getPtrTy()};
  for (const VarSymbol *var : layout.fields) {
    types.push_back(var->boxed()
                        ? this->lowering.builder.getPtrTy()
                        : this->lowering.moduleBuilder().lower(ClosureLowerer::typeOf(var)));
  }
  layout.type = llvm::StructType::get(this->lowering.ctx, types);

  auto *dropType = llvm::FunctionType::get(this->lowering.builder.getVoidTy(),
                                           {this->lowering.builder.getPtrTy()}, false);
  layout.drop = llvm::Function::Create(dropType, llvm::GlobalValue::InternalLinkage,
                                       EnvLowerer::qualifiedName(fn) + ".env.drop",
                                       this->lowering.module_.get());
  layout.drop->addFnAttr(llvm::Attribute::NoUnwind);
  llvm::IRBuilderBase::InsertPoint point = this->lowering.builder.saveIP();
  this->lowering.builder.SetInsertPoint(
      llvm::BasicBlock::Create(this->lowering.ctx, "entry", layout.drop));
  llvm::Value *env = layout.drop->getArg(0);
  for (size_t i = 0; i < layout.fields.size(); ++i) {
    const VarSymbol *var = layout.fields[i];
    if (this->lowering.memory().uncounted.count({key, var})) {
      continue;
    }
    llvm::Value *field = this->lowering.builder.CreateStructGEP(layout.type, env, unsigned(i + 2));
    llvm::Value *value = this->lowering.builder.CreateLoad(
        var->boxed() ? this->lowering.builder.getPtrTy()
                     : this->lowering.moduleBuilder().lower(ClosureLowerer::typeOf(var)),
        field);
    if (var->boxed()) {
      layout.member ? this->lowering.storeLowerer().releaseObjectIn(env, value)
                    : (void)this->lowering.builder.CreateCall(this->lowering.objReleaseFn, {value});
    } else if (layout.member) {
      this->lowering.storeLowerer().releaseIn(env, value, ClosureLowerer::typeOf(var));
    } else {
      this->lowering.refCounts().releaseValue(value, ClosureLowerer::typeOf(var));
    }
  }
  if (layout.member) {
    this->lowering.builder.CreateCall(this->lowering.groupLowerer().groupFunction(
                                          "tz_group_obj_free", this->lowering.builder.getVoidTy(),
                                          {this->lowering.builder.getPtrTy()}),
                                      {env});
    layout.descriptor = this->lowering.groupLowerer().memberDescriptor(
        layout.drop, "closure " + EnvLowerer::qualifiedName(fn).substr(3));
  } else {
    this->lowering.builder.CreateCall(this->lowering.rt_.objFree, {env});
    layout.descriptor = this->lowering.classDescriptors().dropDescriptor(layout.drop);
  }
  this->lowering.builder.CreateRetVoid();
  this->lowering.builder.restoreIP(point);

  return layout;
}

llvm::StructType *EnvLowerer::boxType(const Type *type) {
  llvm::StructType *&box = this->boxTypes[type];
  if (!box) {
    box = llvm::StructType::get(this->lowering.ctx, {this->lowering.builder.getInt64Ty(),
                                                     this->lowering.builder.getPtrTy(),
                                                     this->lowering.moduleBuilder().lower(type)});
  }

  return box;
}

llvm::Value *EnvLowerer::newBox(const Type *type, bool member) {
  llvm::StructType *box = this->boxType(type);
  llvm::Function *&drop = this->lowering.boxDrops[type];
  bool memberDrop = (member || this->lowering.isCyclicBox(type) ||
                     this->lowering.memory().mayCarryMember(type)) &&
                    type->isManaged();
  member = (member || this->lowering.isCyclicBox(type)) && type->isManaged();
  if (!drop) {
    auto *dropType = llvm::FunctionType::get(this->lowering.builder.getVoidTy(),
                                             {this->lowering.builder.getPtrTy()}, false);
    if (!type->isManaged()) {
      drop = cast<llvm::Function>(this->lowering.rt_.objFree.getCallee());
    } else {
      drop = llvm::Function::Create(dropType, llvm::GlobalValue::InternalLinkage, "tz.box.drop",
                                    this->lowering.module_.get());
      drop->addFnAttr(llvm::Attribute::NoUnwind);
      llvm::IRBuilderBase::InsertPoint point = this->lowering.builder.saveIP();
      this->lowering.builder.SetInsertPoint(
          llvm::BasicBlock::Create(this->lowering.ctx, "entry", drop));
      llvm::Value *object = drop->getArg(0);
      llvm::Value *value =
          this->lowering.builder.CreateLoad(this->lowering.moduleBuilder().lower(type),
                                            this->lowering.builder.CreateStructGEP(box, object, 2));
      if (memberDrop) {
        this->lowering.storeLowerer().releaseIn(object, value, type);
        this->lowering.builder.CreateCall(
            this->lowering.groupLowerer().groupFunction("tz_group_obj_free",
                                                        this->lowering.builder.getVoidTy(),
                                                        {this->lowering.builder.getPtrTy()}),
            {object});
      } else {
        this->lowering.refCounts().releaseValue(value, type);
        this->lowering.builder.CreateCall(this->lowering.rt_.objFree, {object});
      }
      this->lowering.builder.CreateRetVoid();
      this->lowering.builder.restoreIP(point);
    }
  }
  uint64_t size = this->lowering.module_->getDataLayout().getTypeAllocSize(box);
  llvm::Value *object;
  if (member) {
    object = this->lowering.groupLowerer().allocMember(size, "box");
    llvm::Constant *descriptor = this->lowering.groupLowerer().memberDescriptor(drop, "box");
    if (!llvm::is_contained(this->lowering.boxDescriptors, std::make_pair(descriptor, type))) {
      this->lowering.boxDescriptors.push_back({descriptor, type});
    }
    this->lowering.builder.CreateStore(descriptor,
                                       this->lowering.builder.CreateStructGEP(box, object, 1));
  } else {
    object = this->lowering.builder.CreateCall(this->lowering.rt_.objAlloc,
                                               {this->lowering.builder.getInt64(size)}, "box");
    this->lowering.builder.CreateStore(this->lowering.builder.getInt64(1),
                                       this->lowering.builder.CreateStructGEP(box, object, 0));
    this->lowering.builder.CreateStore(this->lowering.classDescriptors().dropDescriptor(drop),
                                       this->lowering.builder.CreateStructGEP(box, object, 1));
  }
  this->lowering.builder.CreateStore(
      llvm::Constant::getNullValue(this->lowering.moduleBuilder().lower(type)),
      this->lowering.builder.CreateStructGEP(box, object, 2));

  return object;
}

llvm::Value *EnvLowerer::boxPointer(const VarSymbol *var) {
  llvm::Value *box = this->lowering.envBoxes.lookup(var);
  if (box) {
    return box;
  }
  llvm::Value *slot = this->lowering.boxSlots.lookup(var);
  assert(slot && "a boxed variable used before its declaration was emitted");

  return this->lowering.builder.CreateLoad(this->lowering.builder.getPtrTy(), slot,
                                           var->name + ".box");
}

llvm::Value *EnvLowerer::createEnv(const Lowering::EnvLayout &layout, bool onStack, bool member) {
  llvm::StructType *type = layout.type;
  llvm::Value *env;
  if (onStack) {
    env = this->lowering.functionFrame().createStackSlot(type, "env");
    this->lowering.builder.CreateStore(this->lowering.builder.getInt64(kStackObjectCount),
                                       this->lowering.builder.CreateStructGEP(type, env, 0));
    this->lowering.builder.CreateStore(
        llvm::ConstantPointerNull::get(this->lowering.builder.getPtrTy()),
        this->lowering.builder.CreateStructGEP(type, env, 1));
  } else {
    uint64_t size = this->lowering.module_->getDataLayout().getTypeAllocSize(type);
    member = member && layout.member;
    if (member) {
      env = this->lowering.groupLowerer().allocMember(size, "env");
    } else {
      env = this->lowering.builder.CreateCall(this->lowering.rt_.objAlloc,
                                              {this->lowering.builder.getInt64(size)}, "env");
      this->lowering.builder.CreateStore(this->lowering.builder.getInt64(1),
                                         this->lowering.builder.CreateStructGEP(type, env, 0));
    }
    this->lowering.builder.CreateStore(layout.descriptor,
                                       this->lowering.builder.CreateStructGEP(type, env, 1));
  }
  member = member && layout.member && !onStack;
  std::vector<std::pair<llvm::Value *, const VarSymbol *>> stored;
  for (size_t i = 0; i < layout.fields.size(); ++i) {
    const VarSymbol *var = layout.fields[i];
    llvm::Value *value;
    if (var->boxed()) {
      value = this->boxPointer(var);
      if (!onStack) {
        this->lowering.builder.CreateCall(this->lowering.objRetainFn, {value});
      }
      stored.push_back({value, var});
    } else if (this->lowering.memory().uncounted.count({layout.key, var})) {
      value = this->emitVarValue(var, true);
    } else {
      value = this->emitVarValue(var, onStack);
      stored.push_back({value, var});
    }
    this->lowering.builder.CreateStore(
        value, this->lowering.builder.CreateStructGEP(type, env, unsigned(i + 2)));
  }
  if (member) {
    for (auto [value, var] : stored) {
      if (var->boxed()) {
        this->lowering.builder.CreateCall(
            this->lowering.groupLowerer().groupFunction(
                "tz_group_store", this->lowering.builder.getVoidTy(),
                {this->lowering.builder.getPtrTy(), this->lowering.builder.getPtrTy()}),
            {env, value});
      } else {
        this->lowering.storeLowerer().groupStore(env, value, ClosureLowerer::typeOf(var));
      }
    }
  }

  return env;
}

llvm::Value *EnvLowerer::emitVarValue(const VarSymbol *var, bool borrow) {
  const FunctionDecl *fn = nullptr;
  llvm::Value *env = nullptr;
  if (var->lambda() && var->lambda() == this->lowering.currentFn) {
    fn = var->lambda();
    env = this->lowering.envParam;
  } else if (const FunctionDecl *member = var->function()) {
    NestedGroup *group = member->group;
    if (this->lowering.currentFn && this->lowering.currentFn->group == group) {
      fn = member;
      env = this->lowering.envParam;
    } else if (group->owner == this->lowering.currentFn) {
      fn = member;
      env = this->lowering.builder.CreateLoad(this->lowering.builder.getPtrTy(),
                                              this->lowering.groupEnvs.lookup(group));
    }
  }
  if (fn) {
    if (!env) {
      env = llvm::ConstantPointerNull::get(this->lowering.builder.getPtrTy());
    }
    if (!borrow) {
      this->lowering.builder.CreateCall(this->lowering.objRetainFn, {env});
    }
    return this->lowering.closureLowerer().makeFunctionValue(
        this->lowering.closureLowerer().functionInfo(const_cast<FunctionDecl *>(fn)), env);
  }
  const Type *type = ClosureLowerer::typeOf(var);
  llvm::Value *value =
      this->lowering.builder.CreateLoad(this->lowering.moduleBuilder().lower(type),
                                        this->lowering.functionFrame().addressOf(var), var->name);
  if (!borrow) {
    this->lowering.refCounts().retainValue(value, type);
  }

  return value;
}

void EnvLowerer::emitGroupCreation(NestedGroup *group) {
  const Lowering::EnvLayout &layout = this->envLayout(group->members.front());
  llvm::Value *slot = this->lowering.functionFrame().createObjectSlot("group.env");
  this->lowering.groupEnvs[group] = slot;
  if (!layout.fields.empty() || this->lowering.comparesFunctions) {
    this->lowering.builder.CreateStore(this->createEnv(layout, false), slot);
  }
  for (FunctionDecl *member : group->members) {
    this->lowering.closureLowerer().closureCode(member);
  }
}

void EnvLowerer::bindCaptures(const FunctionDecl *fn) {
  if (fn->cls() && fn->cls()->literal()) {
    this->lowering.envParam->setName("this");
    const Lowering::ObjectLayout &layout =
        this->lowering.objectLayouts().objectLayout(fn->cls()->type);
    const std::vector<VarSymbol *> &captures = fn->cls()->group->captures;
    for (size_t i = 0; i < captures.size(); ++i) {
      const VarSymbol *var = captures[i];
      llvm::Value *field = this->lowering.builder.CreateStructGEP(
          layout.type, this->lowering.envParam, unsigned(layout.captures + i), var->name);
      if (var->boxed()) {
        this->lowering.envBoxes[var] = this->lowering.builder.CreateLoad(
            this->lowering.builder.getPtrTy(), field, var->name + ".box");
      } else {
        this->lowering.addresses[var] = field;
      }
      this->lowering.debugInfo().declare(var, this->lowering.functionFrame().addressOf(var));
    }
    return;
  }
  const VarSymbol *receiver = fn->receiverEnv;
  if (receiver) {
    this->lowering.envParam->setName(receiver->name);
    llvm::Value *slot = this->lowering.functionFrame().createStackSlot(
        this->lowering.builder.getPtrTy(), receiver->name);
    this->lowering.builder.CreateStore(this->lowering.envParam, slot);
    this->lowering.addresses[receiver] = slot;
    this->lowering.debugInfo().declare(receiver, slot);
    return;
  }
  this->lowering.envParam->setName("env");
  const Lowering::EnvLayout &layout = this->envLayout(fn);
  for (size_t i = 0; i < layout.fields.size(); ++i) {
    const VarSymbol *var = layout.fields[i];
    llvm::Value *field = this->lowering.builder.CreateStructGEP(
        layout.type, this->lowering.envParam, unsigned(i + 2), var->name);
    if (var->boxed()) {
      this->lowering.envBoxes[var] = this->lowering.builder.CreateLoad(
          this->lowering.builder.getPtrTy(), field, var->name + ".box");
    } else {
      this->lowering.addresses[var] = field;
    }
    this->lowering.debugInfo().declare(var, this->lowering.functionFrame().addressOf(var));
  }
}

}
