#include "compiler/lower/functions/env_lowerer/env_lowerer.h"
#include "compiler/lower/classes/class_descriptors/class_descriptors.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/functions/closure_lowerer/closure_lowerer.h"
#include "compiler/lower/layouts/object_layouts/object_layouts.h"
#include "compiler/lower/memory/group_lowerer/group_lowerer.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/memory/store_lowerer/store_lowerer.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/memory/memory_plan/memory_plan.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"

using llvm::BasicBlock;
using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::Value;

namespace tz {

namespace {

// A literal environment on the stack: never freed, so its count only needs
// to stay far from zero.
constexpr uint64_t kStackObjectCount = uint64_t(1) << 62;

// The name of a closure's code in the IR: the functions it is nested in, then
// its own.
std::string qualifiedName(const FunctionDecl *fn) {
  std::string name = fn->name.empty() ? "lambda" : fn->name.str();
  for (const FunctionDecl *p = fn->parent; p; p = p->parent) {
    name = (p->name.empty() ? "lambda" : p->name.str()) + "." + name;
  }

  return "tz." + name;
}

} // namespace

EnvLowerer::EnvLowerer(Lowering &lowering) : lowering_(lowering) {}

const Lowering::EnvLayout &EnvLowerer::envLayout(const FunctionDecl *fn) {
  const void *key = fn->group ? static_cast<const void *>(fn->group) : fn;
  auto [it, inserted] = lowering_.envLayouts_.try_emplace(key);
  Lowering::EnvLayout &layout = it->second;
  if (!inserted) {
    return layout;
  }
  const std::vector<VarSymbol *> &captures = fn->group ? fn->group->captures : fn->captures;
  layout.fields.assign(captures.begin(), captures.end());
  layout.key = key;
  layout.member = lowering_.isCyclicEnv(key);
  std::vector<llvm::Type *> types{lowering_.builder_.getInt64Ty(), lowering_.builder_.getPtrTy()};
  for (const VarSymbol *var : layout.fields) {
    types.push_back(var->boxed() ? lowering_.builder_.getPtrTy()
                                 : lowering_.moduleBuilder().lower(ClosureLowerer::typeOf(var)));
  }
  layout.type = llvm::StructType::get(lowering_.ctx_, types);

  // Releases what the environment holds, then frees it.
  auto *dropType = llvm::FunctionType::get(lowering_.builder_.getVoidTy(),
                                           {lowering_.builder_.getPtrTy()}, false);
  layout.drop = llvm::Function::Create(dropType, llvm::GlobalValue::InternalLinkage,
                                       qualifiedName(fn) + ".env.drop", lowering_.module_.get());
  layout.drop->addFnAttr(llvm::Attribute::NoUnwind);
  llvm::IRBuilderBase::InsertPoint point = lowering_.builder_.saveIP();
  lowering_.builder_.SetInsertPoint(llvm::BasicBlock::Create(lowering_.ctx_, "entry", layout.drop));
  llvm::Value *env = layout.drop->getArg(0);
  // A member releases what it holds as one (see runtime/core/group/group.c).
  for (size_t i = 0; i < layout.fields.size(); ++i) {
    const VarSymbol *var = layout.fields[i];
    if (lowering_.memory().uncounted.count({key, var})) { // an owner it lives in (layer 2)
      continue;
    }
    llvm::Value *field = lowering_.builder_.CreateStructGEP(layout.type, env, unsigned(i + 2));
    llvm::Value *value = lowering_.builder_.CreateLoad(
        var->boxed() ? lowering_.builder_.getPtrTy()
                     : lowering_.moduleBuilder().lower(ClosureLowerer::typeOf(var)),
        field);
    if (var->boxed()) {
      layout.member ? lowering_.storeLowerer().releaseObjectIn(env, value)
                    : (void)lowering_.builder_.CreateCall(lowering_.objReleaseFn_, {value});
    } else if (layout.member) {
      lowering_.storeLowerer().releaseIn(env, value, ClosureLowerer::typeOf(var));
    } else {
      lowering_.refCounts().releaseValue(value, ClosureLowerer::typeOf(var));
    }
  }
  if (layout.member) {
    lowering_.builder_.CreateCall(
        lowering_.groupLowerer().groupFunction("tz_group_obj_free", lowering_.builder_.getVoidTy(),
                                               {lowering_.builder_.getPtrTy()}),
        {env});
    layout.descriptor = lowering_.groupLowerer().memberDescriptor(
        layout.drop, "closure " + qualifiedName(fn).substr(3));
  } else {
    lowering_.builder_.CreateCall(lowering_.rt_.objFree, {env});
    layout.descriptor = lowering_.classDescriptors().dropDescriptor(layout.drop);
  }
  lowering_.builder_.CreateRetVoid();
  lowering_.builder_.restoreIP(point);

  return layout;
}

llvm::StructType *EnvLowerer::boxType(const Type *type) {
  llvm::StructType *&box = boxTypes_[type];
  if (!box) {
    box = llvm::StructType::get(lowering_.ctx_,
                                {lowering_.builder_.getInt64Ty(), lowering_.builder_.getPtrTy(),
                                 lowering_.moduleBuilder().lower(type)});
  }

  return box;
}

llvm::Value *EnvLowerer::newBox(const Type *type, bool member) {
  llvm::StructType *box = boxType(type);
  llvm::Function *&drop = lowering_.boxDrops_[type];
  // A box of a variable whose values may form cycles with it is a member of
  // a group (see runtime/core/group/group.c). Its drop lets go of the value as
  // from a member, which is right for a box outside the groups too (a box of
  // the same type may be either).
  bool memberDrop =
      (member || lowering_.isCyclicBox(type) || lowering_.memory().mayHoldMember(type)) &&
      type->isManaged();
  member = (member || lowering_.isCyclicBox(type)) && type->isManaged();
  if (!drop) {
    // Releases the value, then frees the box; without a value to release,
    // just frees it.
    auto *dropType = llvm::FunctionType::get(lowering_.builder_.getVoidTy(),
                                             {lowering_.builder_.getPtrTy()}, false);
    if (!type->isManaged()) {
      drop = cast<llvm::Function>(lowering_.rt_.objFree.getCallee());
    } else {
      drop = llvm::Function::Create(dropType, llvm::GlobalValue::InternalLinkage, "tz.box.drop",
                                    lowering_.module_.get());
      drop->addFnAttr(llvm::Attribute::NoUnwind);
      llvm::IRBuilderBase::InsertPoint point = lowering_.builder_.saveIP();
      lowering_.builder_.SetInsertPoint(llvm::BasicBlock::Create(lowering_.ctx_, "entry", drop));
      llvm::Value *object = drop->getArg(0);
      llvm::Value *value =
          lowering_.builder_.CreateLoad(lowering_.moduleBuilder().lower(type),
                                        lowering_.builder_.CreateStructGEP(box, object, 2));
      if (memberDrop) {
        lowering_.storeLowerer().releaseIn(object, value, type);
        lowering_.builder_.CreateCall(lowering_.groupLowerer().groupFunction(
                                          "tz_group_obj_free", lowering_.builder_.getVoidTy(),
                                          {lowering_.builder_.getPtrTy()}),
                                      {object});
      } else {
        lowering_.refCounts().releaseValue(value, type);
        lowering_.builder_.CreateCall(lowering_.rt_.objFree, {object});
      }
      lowering_.builder_.CreateRetVoid();
      lowering_.builder_.restoreIP(point);
    }
  }
  uint64_t size = lowering_.module_->getDataLayout().getTypeAllocSize(box);
  llvm::Value *object;
  if (member) {
    object = lowering_.groupLowerer().allocMember(size, "box");
    llvm::Constant *descriptor = lowering_.groupLowerer().memberDescriptor(drop, "box");
    if (!llvm::is_contained(lowering_.boxDescriptors_, std::make_pair(descriptor, type))) {
      lowering_.boxDescriptors_.push_back({descriptor, type});
    }
    lowering_.builder_.CreateStore(descriptor, lowering_.builder_.CreateStructGEP(box, object, 1));
  } else {
    object = lowering_.builder_.CreateCall(lowering_.rt_.objAlloc,
                                           {lowering_.builder_.getInt64(size)}, "box");
    lowering_.builder_.CreateStore(lowering_.builder_.getInt64(1),
                                   lowering_.builder_.CreateStructGEP(box, object, 0));
    lowering_.builder_.CreateStore(lowering_.classDescriptors().dropDescriptor(drop),
                                   lowering_.builder_.CreateStructGEP(box, object, 1));
  }
  lowering_.builder_.CreateStore(
      llvm::Constant::getNullValue(lowering_.moduleBuilder().lower(type)),
      lowering_.builder_.CreateStructGEP(box, object, 2));

  return object;
}

llvm::Value *EnvLowerer::boxPointer(const VarSymbol *var) {
  if (llvm::Value *box = lowering_.envBoxes_.lookup(var)) {
    return box;
  }
  llvm::Value *slot = lowering_.boxSlots_.lookup(var);
  assert(slot && "a boxed variable used before its declaration was emitted");

  return lowering_.builder_.CreateLoad(lowering_.builder_.getPtrTy(), slot, var->name + ".box");
}

llvm::Value *EnvLowerer::createEnv(const Lowering::EnvLayout &layout, bool onStack, bool member) {
  llvm::StructType *type = layout.type;
  llvm::Value *env;
  if (onStack) {
    env = lowering_.functionFrame().createStackSlot(type, "env");
    lowering_.builder_.CreateStore(lowering_.builder_.getInt64(kStackObjectCount),
                                   lowering_.builder_.CreateStructGEP(type, env, 0));
    lowering_.builder_.CreateStore(llvm::ConstantPointerNull::get(lowering_.builder_.getPtrTy()),
                                   lowering_.builder_.CreateStructGEP(type, env, 1));
  } else {
    uint64_t size = lowering_.module_->getDataLayout().getTypeAllocSize(type);
    member = member && layout.member;
    if (member) {
      env = lowering_.groupLowerer().allocMember(size, "env");
    } else {
      env = lowering_.builder_.CreateCall(lowering_.rt_.objAlloc,
                                          {lowering_.builder_.getInt64(size)}, "env");
      lowering_.builder_.CreateStore(lowering_.builder_.getInt64(1),
                                     lowering_.builder_.CreateStructGEP(type, env, 0));
    }
    lowering_.builder_.CreateStore(layout.descriptor,
                                   lowering_.builder_.CreateStructGEP(type, env, 1));
  }
  // An environment on the stack lives only while the variables do, so it
  // borrows their values.
  member = member && layout.member && !onStack;
  for (size_t i = 0; i < layout.fields.size(); ++i) {
    const VarSymbol *var = layout.fields[i];
    llvm::Value *value;
    if (var->boxed()) {
      value = boxPointer(var);
      if (!onStack) {
        lowering_.builder_.CreateCall(lowering_.objRetainFn_, {value});
      }
      if (member) {
        lowering_.builder_.CreateCall(
            lowering_.groupLowerer().groupFunction(
                "tz_group_store", lowering_.builder_.getVoidTy(),
                {lowering_.builder_.getPtrTy(), lowering_.builder_.getPtrTy()}),
            {env, value});
      }
    } else if (lowering_.memory().uncounted.count({layout.key, var})) {
      // The closure lives in an object that the object it refers to made
      // and holds alone (docs/objects.md, section 11, layer 2).
      value = emitVarValue(var, /*borrow=*/true);
    } else {
      value = emitVarValue(var, /*borrow=*/onStack);
      if (member) {
        lowering_.storeLowerer().groupStore(env, value, ClosureLowerer::typeOf(var));
      }
    }
    lowering_.builder_.CreateStore(value,
                                   lowering_.builder_.CreateStructGEP(type, env, unsigned(i + 2)));
  }

  return env;
}

llvm::Value *EnvLowerer::emitVarValue(const VarSymbol *var, bool borrow) {
  // A function's own value: it is the one being emitted (a constant arrow
  // calling itself), or one of the group being emitted or created here.
  const FunctionDecl *fn = nullptr;
  llvm::Value *env = nullptr;
  if (var->lambda && var->lambda == lowering_.currentFn_) {
    fn = var->lambda;
    env = lowering_.envParam_;
  } else if (const FunctionDecl *member = var->function) {
    NestedGroup *group = member->group;
    if (lowering_.currentFn_ && lowering_.currentFn_->group == group) {
      fn = member;
      env = lowering_.envParam_;
    } else if (group->owner == lowering_.currentFn_) {
      fn = member;
      env = lowering_.builder_.CreateLoad(lowering_.builder_.getPtrTy(),
                                          lowering_.groupEnvs_.lookup(group));
    }
  }
  if (fn) {
    if (!env) {
      env = llvm::ConstantPointerNull::get(lowering_.builder_.getPtrTy());
    }
    if (!borrow) {
      lowering_.builder_.CreateCall(lowering_.objRetainFn_, {env});
    }
    return lowering_.closureLowerer().makeFunctionValue(
        lowering_.closureLowerer().functionInfo(const_cast<FunctionDecl *>(fn)), env);
  }
  const Type *type = ClosureLowerer::typeOf(var);
  llvm::Value *value = lowering_.builder_.CreateLoad(
      lowering_.moduleBuilder().lower(type), lowering_.functionFrame().addressOf(var), var->name);
  if (!borrow) {
    lowering_.refCounts().retainValue(value, type);
  }

  return value;
}

void EnvLowerer::emitGroupCreation(NestedGroup *group) {
  const Lowering::EnvLayout &layout = envLayout(group->members.front());
  llvm::Value *slot = lowering_.functionFrame().createObjectSlot("group.env");
  lowering_.groupEnvs_[group] = slot;
  if (!layout.fields.empty() || lowering_.comparesFunctions_) {
    lowering_.builder_.CreateStore(createEnv(layout, /*onStack=*/false), slot);
  }
  for (FunctionDecl *member : group->members) {
    lowering_.closureLowerer().closureCode(member);
  }
}

void EnvLowerer::bindCaptures(const FunctionDecl *fn) {
  // A method of an object literal: the object holds what it captured.
  if (fn->cls && fn->cls->literal) {
    lowering_.envParam_->setName("this");
    const Lowering::ObjectLayout &layout = lowering_.objectLayouts().objectLayout(fn->cls->type);
    const std::vector<VarSymbol *> &captures = fn->cls->group->captures;
    for (size_t i = 0; i < captures.size(); ++i) {
      const VarSymbol *var = captures[i];
      llvm::Value *field = lowering_.builder_.CreateStructGEP(
          layout.type, lowering_.envParam_, unsigned(layout.captures + i), var->name);
      if (var->boxed()) {
        lowering_.envBoxes_[var] =
            lowering_.builder_.CreateLoad(lowering_.builder_.getPtrTy(), field, var->name + ".box");
      } else {
        lowering_.addresses_[var] = field;
      }
    }
    return;
  }
  // The captured variables: the values copied into the environment, and the
  // boxes it holds.
  lowering_.envParam_->setName("env");
  const Lowering::EnvLayout &layout = envLayout(fn);
  for (size_t i = 0; i < layout.fields.size(); ++i) {
    const VarSymbol *var = layout.fields[i];
    llvm::Value *field = lowering_.builder_.CreateStructGEP(layout.type, lowering_.envParam_,
                                                            unsigned(i + 2), var->name);
    if (var->boxed()) {
      lowering_.envBoxes_[var] =
          lowering_.builder_.CreateLoad(lowering_.builder_.getPtrTy(), field, var->name + ".box");
    } else {
      lowering_.addresses_[var] = field;
    }
  }
}

} // namespace tz
