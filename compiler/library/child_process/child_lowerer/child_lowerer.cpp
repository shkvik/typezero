#include "compiler/library/child_process/child_lowerer/child_lowerer.h"
#include "compiler/library/process/process_lowerer/process_lowerer.h"
#include "compiler/library/result/result_lowerer/result_lowerer.h"
#include "compiler/lower/classes/class_lowerer/class_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/function_builder/function_builder.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/layouts/object_layouts/object_layouts.h"
#include "compiler/lower/layouts/type_descriptors/type_descriptors.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/objects/object_maker/object_maker.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/lower/types/union_layout/union_layout.h"
#include "compiler/lower/values/array_lowerer/array_lowerer.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "llvm/Support/ErrorHandling.h"

using llvm::BasicBlock;
using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::Value;

namespace tz {

namespace {

enum RequestField : unsigned {
  RqFile,
  RqArgs,
  RqCwd,
  RqInputText,
  RqKillName,
  RqShell,
  RqArgv0,
  RqEnv,
  RqInputBytes,
  RqStringType,
  RqTimeout,
  RqMaxBuffer,
  RqMode,
  RqHas,
  RqInput,
  RqKillNumber,
  RqShellKind,
  RqWindowsHide,
  RqVerbatim,
  RqDetached,
  RqUid,
  RqGid,
  RqStdio,
  RqEncoding,
};

enum PartsField : unsigned {
  PtMessage,
  PtCode,
  PtSyscall,
  PtPath,
  PtSpawnargs,
  PtSignal,
  PtText,
  PtBytes,
  PtErrno,
  PtHasCode,
  PtStatus,
  PtHasStatus,
  PtHasSignal,
  PtPid,
  PtTextMode,
  PtPiped,
  PtReason,
  PtCodeName,
  PtDetail,
  PtCommand,
  PtCwd,
  PtShell,
  PtArgument,
  PtLimit,
  PtValue,
};

enum : uint32_t {
  HasCwd = 1,
  HasTimeout = 2,
  HasMaxBuffer = 4,
  HasKillName = 8,
  HasKillNumber = 16,
  HasArgv0 = 32,
  HasUid = 64,
  HasGid = 128,
  HasStdio = 256,
};

const Type *bufferType() { return Type::bytesType(Type::Bytes::Buffer); }

bool holds(const Type *type, const Type *member) {
  type = type->nonOptional();

  return type == member || llvm::is_contained(type->members(), member);
}

}

ChildLowerer::ChildLowerer(Lowering &lowering) : lowering_(lowering) {}

llvm::StructType *ChildLowerer::spawnPartsType() {
  llvm::Type *ptr = lowering_.builder_.getPtrTy(), *i32 = lowering_.builder_.getInt32Ty();

  return llvm::StructType::get(lowering_.ctx_, {lowering_.strTy_,
                                                lowering_.strTy_,
                                                lowering_.strTy_,
                                                lowering_.strTy_,
                                                ptr,
                                                lowering_.strTy_,
                                                llvm::ArrayType::get(lowering_.strTy_, 3),
                                                llvm::ArrayType::get(ptr, 3),
                                                i32,
                                                i32,
                                                i32,
                                                i32,
                                                i32,
                                                i32,
                                                i32,
                                                llvm::ArrayType::get(i32, 3),
                                                ptr,
                                                ptr,
                                                ptr,
                                                lowering_.strTy_,
                                                lowering_.strTy_,
                                                lowering_.strTy_,
                                                lowering_.strTy_,
                                                lowering_.builder_.getInt64Ty(),
                                                lowering_.builder_.getDoubleTy()});
}

llvm::Value *ChildLowerer::spawnStream(llvm::Value *parts, unsigned stream, const Type *to,
                                       bool retain) {
  llvm::StructType *partsType = spawnPartsType();
  llvm::Type *i32 = lowering_.builder_.getInt32Ty();
  auto field = [&](unsigned f, unsigned i) {
    return lowering_.builder_.CreateConstInBoundsGEP2_32(
        partsType->getElementType(f), lowering_.builder_.CreateStructGEP(partsType, parts, f), 0,
        i);
  };
  llvm::Value *piped = lowering_.builder_.CreateICmpNE(
      lowering_.builder_.CreateLoad(i32, field(PtPiped, stream)), lowering_.builder_.getInt32(0));
  auto part = [&](const Type *member) {
    llvm::Value *raw =
        lowering_.builder_.CreateLoad(lowering_.moduleBuilder().lower(member),
                                      field(member->isString() ? PtText : PtBytes, stream));
    if (retain) {
      lowering_.refCounts().retainValue(raw, member);
    }

    return raw;
  };
  bool text = holds(to, Type::string()), bytes = holds(to, bufferType());
  if (!to->isOptional() && !to->isUnion()) {
    return part(to);
  }
  if (to->isOptional() && !to->inner()->isUnion()) {
    return lowering_.optionalLowerer().makeOptional(part(to->inner()), piped, to);
  }
  llvm::Value *value;
  if (text && bytes) {
    llvm::Value *mode = lowering_.builder_.CreateICmpNE(
        lowering_.builder_.CreateLoad(
            i32, lowering_.builder_.CreateStructGEP(partsType, parts, PtTextMode)),
        lowering_.builder_.getInt32(0));
    llvm::Value *fromText =
        lowering_.exprLowerer().emitConversion(part(Type::string()), Type::string(), to);
    llvm::Value *fromBytes =
        lowering_.exprLowerer().emitConversion(part(bufferType()), bufferType(), to);
    value = lowering_.builder_.CreateSelect(mode, fromText, fromBytes);
  } else {
    const Type *member = text ? Type::string() : bufferType();
    value = lowering_.exprLowerer().emitConversion(part(member), member, to);
  }

  return to->isOptional() ? lowering_.builder_.CreateSelect(
                                piped, value, lowering_.optionalLowerer().undefinedValue(to))
                          : value;
}

llvm::Function *ChildLowerer::spawnMaker(const Type *type) {
  llvm::Function *&maker = spawnMakers_[type];
  if (maker) {
    return maker;
  }
  llvm::Type *ptr = lowering_.builder_.getPtrTy();
  const ClassDecl *cls = type->classDecl();
  maker = llvm::Function::Create(
      llvm::FunctionType::get(ptr, {ptr}, false), llvm::GlobalValue::InternalLinkage,
      cls ? "tz.make." + cls->name.str() : "tz.make.spawn", lowering_.module_.get());
  lowering_.functionBuilder().setFunctionAttributes(maker);
  llvm::IRBuilderBase::InsertPoint point = lowering_.builder_.saveIP();
  llvm::Function *outer = std::exchange(lowering_.function_, maker);
  lowering_.builder_.SetInsertPoint(llvm::BasicBlock::Create(lowering_.ctx_, "entry", maker));

  llvm::Value *parts = maker->getArg(0);
  llvm::StructType *partsType = spawnPartsType();
  llvm::Value *object;
  if (cls) {
    object = lowering_.classLowerer().allocInstance(cls);
  } else {
    std::vector<unsigned> keys;
    for (const Field &f : type->fields()) {
      keys.push_back(ObjectLayouts::fieldIndex(type, f.name));
    }
    object = lowering_.objectLayouts().allocObject(type, keys);
  }
  auto load = [&](unsigned f) {
    return lowering_.builder_.CreateLoad(partsType->getElementType(f),
                                         lowering_.builder_.CreateStructGEP(partsType, parts, f));
  };
  auto flag = [&](unsigned f) {
    return lowering_.builder_.CreateICmpNE(load(f), lowering_.builder_.getInt32(0));
  };
  auto number = [&](unsigned f) {
    return lowering_.builder_.CreateSExt(load(f), lowering_.builder_.getInt64Ty());
  };
  auto atom = [&](const Type *t, unsigned part) {
    return lowering_.objectMaker().atomOf(t, load(part));
  };
  for (const Field &f : type->fields()) {
    llvm::StringRef name = f.name;
    const Type *t = f.type;
    auto maybe = [&](unsigned part, unsigned has) {
      llvm::Value *raw = t->nonOptional()->isInteger() ? number(part) : load(part);

      return t->isOptional() ? lowering_.optionalLowerer().makeOptional(raw, flag(has), t) : raw;
    };
    bool atomic = UnionLayout::isAtomType(t);
    llvm::Value *value;
    if (t->isStringLiteral()) {
      value = lowering_.builder_.getInt32(t->atom());
    } else if (name == "message") {
      value = load(PtMessage);
    } else if (name == "code") {
      value = atomic ? atom(t, PtCodeName) : load(PtCode);
    } else if (name == "syscall") {
      value = load(PtSyscall);
    } else if (name == "command" || name == "file") {
      value = load(PtCommand);
    } else if (name == "args") {
      value = load(PtSpawnargs);
    } else if (name == "cwd") {
      value = load(PtCwd);
    } else if (name == "shell") {
      value = load(PtShell);
    } else if (name == "argument") {
      value = atomic ? atom(t, PtDetail) : load(PtArgument);
    } else if (name == "timeout" || name == "maxBuffer") {
      value = load(PtLimit);
    } else if (name == "value") {
      value = load(PtValue);
    } else if (name == "stream" || name == "crash") {
      value = atom(t, PtDetail);
    } else if (name == "status") {
      value = maybe(PtStatus, PtHasStatus);
    } else if (name == "signal") {
      value = atomic            ? atom(t, PtDetail)
              : t->isOptional() ? maybe(PtSignal, PtHasSignal)
                                : load(PtSignal);
    } else if (name == "pid") {
      value = number(PtPid);
    } else if (name == "stdout" || name == "stderr") {
      value = spawnStream(parts, name == "stdout" ? 1 : 2, t, false);
    } else if (name == "output") {
      const Type *element = t->inner();
      llvm::Value *count = lowering_.builder_.getInt64(3);
      value = lowering_.builder_.CreateCall(
          lowering_.rt_.arrNew, {lowering_.typeDescriptors().typeDescriptor(element), count, count},
          "output");
      llvm::Value *data = lowering_.arrayLowerer().arrayData(value);
      for (unsigned i = 0; i < 3; ++i) {
        lowering_.builder_.CreateStore(
            spawnStream(parts, i, element, true),
            lowering_.arrayLowerer().elementAddress(data, element, lowering_.builder_.getInt64(i)));
      }
    } else {
      continue;
    }
    lowering_.builder_.CreateStore(
        value, lowering_.objectLayouts().fieldAddress(object, type, name, true));
  }
  lowering_.builder_.CreateRet(object);
  lowering_.function_ = outer;
  lowering_.builder_.restoreIP(point);

  return maker;
}

llvm::Function *ChildLowerer::errorMaker(const Type *type) {
  if (!type->isUnion()) {
    return spawnMaker(type);
  }
  llvm::Function *&made = spawnMakers_[type];
  if (made) {
    return made;
  }
  llvm::Type *ptr = lowering_.builder_.getPtrTy(), *i32 = lowering_.builder_.getInt32Ty();
  std::vector<const Type *> variants(type->members().begin(), type->members().end());
  std::vector<llvm::Function *> makers;
  std::vector<std::string> reasons;
  unsigned system = 0;
  for (const Type *variant : variants) {
    makers.push_back(spawnMaker(variant));
    std::string kind;
    for (char16_t unit : variant->field("kind")->type->literalText()) {
      kind.push_back(char(unit));
    }
    size_t at = kind.find("_ERROR_");
    reasons.push_back(at == std::string::npos ? kind : kind.substr(at + 7));
    if (reasons.back() == "SYSTEM") {
      system = unsigned(reasons.size() - 1);
    }
  }
  made = llvm::Function::Create(llvm::FunctionType::get(ptr, {ptr}, false),
                                llvm::GlobalValue::InternalLinkage, "tz.make." + type->name().str(),
                                lowering_.module_.get());
  llvm::Function *function = made;
  lowering_.functionBuilder().setFunctionAttributes(function);
  llvm::IRBuilderBase::InsertPoint point = lowering_.builder_.saveIP();
  llvm::Function *outer = std::exchange(lowering_.function_, function);
  lowering_.builder_.SetInsertPoint(llvm::BasicBlock::Create(lowering_.ctx_, "entry", function));
  llvm::Value *parts = function->getArg(0);
  llvm::StructType *partsType = spawnPartsType();
  llvm::Value *reason = lowering_.builder_.CreateLoad(
      ptr, lowering_.builder_.CreateStructGEP(partsType, parts, PtReason));
  llvm::FunctionCallee compare = lowering_.module_->getOrInsertFunction(
      "strcmp", llvm::FunctionType::get(i32, {ptr, ptr}, false));
  llvm::Value *none = llvm::ConstantInt::getSigned(i32, -1);
  llvm::Value *index = none;
  for (size_t v = 0; v < variants.size(); ++v) {
    llvm::Value *same = lowering_.builder_.CreateICmpEQ(
        lowering_.builder_.CreateCall(compare,
                                      {reason, lowering_.stringLowerer().utf8Constant(reasons[v])}),
        lowering_.builder_.getInt32(0));
    index = lowering_.builder_.CreateSelect(same, lowering_.builder_.getInt32(unsigned(v)), index);
  }
  llvm::BasicBlock *unknown = llvm::BasicBlock::Create(lowering_.ctx_, "unknown", function);
  llvm::SwitchInst *pick =
      lowering_.builder_.CreateSwitch(index, unknown, unsigned(variants.size()));
  for (size_t v = 0; v < variants.size(); ++v) {
    llvm::BasicBlock *block = llvm::BasicBlock::Create(lowering_.ctx_, "variant", function);
    pick->addCase(lowering_.builder_.getInt32(unsigned(v)), block);
    lowering_.builder_.SetInsertPoint(block);
    lowering_.builder_.CreateRet(lowering_.builder_.CreateCall(makers[v], {parts}));
  }
  lowering_.builder_.SetInsertPoint(unknown);
  lowering_.builder_.CreateStore(lowering_.stringLowerer().utf8Constant("UNKNOWN"),
                                 lowering_.builder_.CreateStructGEP(partsType, parts, PtCodeName));
  lowering_.builder_.CreateRet(lowering_.builder_.CreateCall(makers[system], {parts}));
  lowering_.function_ = outer;
  lowering_.builder_.restoreIP(point);

  return function;
}

llvm::Value *ChildLowerer::emitChildBuiltin(CallExpr *call) {
  llvm::Type *ptr = lowering_.builder_.getPtrTy(), *i32 = lowering_.builder_.getInt32Ty();
  llvm::Type *f64 = lowering_.builder_.getDoubleTy();
  std::vector<llvm::Type *> fields(RqEnv, lowering_.strTy_);
  fields[RqArgs] = ptr;
  fields.insert(fields.end(), {ptr, ptr, ptr, f64, f64});
  fields.insert(fields.end(), RqStdio - RqMode, i32);
  fields.push_back(llvm::ArrayType::get(i32, 3));
  fields.push_back(i32);
  llvm::StructType *requestType = llvm::StructType::get(lowering_.ctx_, fields);

  std::vector<llvm::Value *> values = lowering_.exprLowerer().emitExprs(call->args);
  llvm::AllocaInst *request =
      lowering_.functionFrame().createStackSlot(requestType, "spawn.request");
  lowering_.builder_.CreateStore(llvm::Constant::getNullValue(requestType), request);
  auto set = [&](unsigned f, llvm::Value *value) {
    lowering_.builder_.CreateStore(value,
                                   lowering_.builder_.CreateStructGEP(requestType, request, f));
  };
  auto literal = [&](unsigned arg) { return unsigned(cast<IntLitExpr>(call->args[arg])->value); };
  llvm::Value *has = lowering_.builder_.getInt32(literal(ChildStdioGiven) ? HasStdio : 0);
  auto given = [&](unsigned arg) -> std::pair<llvm::Value *, llvm::Value *> {
    const Type *type = call->args[arg]->type;
    if (!type->isOptional()) {
      return {lowering_.builder_.getTrue(), values[arg]};
    }

    return {lowering_.optionalLowerer().optionalHas(values[arg], type),
            lowering_.optionalLowerer().optionalValue(values[arg], type)};
  };
  auto option = [&](unsigned arg, unsigned f, uint32_t bit) {
    auto [present, value] = given(arg);
    if (bit) {
      has = lowering_.builder_.CreateOr(
          has, lowering_.builder_.CreateSelect(present, lowering_.builder_.getInt32(bit),
                                               lowering_.builder_.getInt32(0)));
    }
    set(f, value);

    return present;
  };
  auto boolean = [&](unsigned arg, unsigned f) {
    auto [present, value] = given(arg);
    set(f, lowering_.builder_.CreateZExt(lowering_.builder_.CreateAnd(present, value), i32));
  };

  set(RqFile, values[ChildFile]);
  set(RqArgs, values[ChildArgs]);
  option(ChildCwd, RqCwd, HasCwd);
  option(ChildEnv, RqEnv, 0);
  {
    const Type *input = call->args[ChildInput]->type->nonOptional();
    bool text = input->isString();
    llvm::Value *present = option(ChildInput, text ? RqInputText : RqInputBytes, 0);
    set(RqInput, lowering_.builder_.CreateSelect(present, lowering_.builder_.getInt32(text ? 1 : 2),
                                                 lowering_.builder_.getInt32(0)));
  }
  option(ChildTimeout, RqTimeout, HasTimeout);
  option(ChildMaxBuffer, RqMaxBuffer, HasMaxBuffer);
  if (call->args[ChildKillSignal]->type->nonOptional()->isString()) {
    option(ChildKillSignal, RqKillName, HasKillName);
  } else {
    option(ChildKillSignal, RqKillNumber, HasKillNumber);
  }
  if (call->args[ChildShell]->type->nonOptional()->isString()) {
    llvm::Value *present = option(ChildShell, RqShell, 0);
    set(RqShellKind, lowering_.builder_.CreateSelect(present, lowering_.builder_.getInt32(2),
                                                     lowering_.builder_.getInt32(0)));
  } else {
    boolean(ChildShell, RqShellKind);
  }
  boolean(ChildWindowsHide, RqWindowsHide);
  boolean(ChildVerbatim, RqVerbatim);
  boolean(ChildDetached, RqDetached);
  option(ChildArgv0, RqArgv0, HasArgv0);
  option(ChildUid, RqUid, HasUid);
  option(ChildGid, RqGid, HasGid);
  set(RqHas, has);
  set(RqStringType, lowering_.typeDescriptors().typeDescriptor(Type::string()));
  set(RqMode, lowering_.builder_.getInt32(call->builtin == Builtin::ChildSpawnSync      ? 0
                                          : call->builtin == Builtin::ChildExecFileSync ? 1
                                                                                        : 2));
  for (unsigned i = 0; i < 3; ++i) {
    lowering_.builder_.CreateStore(
        lowering_.builder_.getInt32(literal(ChildStdin + i)),
        lowering_.builder_.CreateConstInBoundsGEP2_32(
            requestType->getElementType(RqStdio),
            lowering_.builder_.CreateStructGEP(requestType, request, RqStdio), 0, i));
  }
  set(RqEncoding, lowering_.builder_.getInt32(literal(ChildEncoding)));

  bool spawn = call->builtin == Builtin::ChildSpawnSync;
  const Type *value = call->type->okType();
  llvm::Value *resultMaker = llvm::ConstantPointerNull::get(lowering_.builder_.getPtrTy());
  if (spawn) {
    resultMaker = spawnMaker(value);
  }
  llvm::AllocaInst *parts =
      lowering_.functionFrame().createStackSlot(spawnPartsType(), "spawn.parts");
  llvm::Value *result = lowering_.builder_.CreateCall(
      lowering_.processLowerer().processFunction("tz_child_run", ptr, {ptr, ptr, ptr, ptr}),
      {request, parts, errorMaker(call->type->errType()), resultMaker});
  for (size_t i = 0; i < values.size(); ++i) {
    lowering_.refCounts().releaseValue(values[i], call->args[i]->type);
  }

  return lowering_.resultLowerer().catchFailure(
      spawn ? result : spawnStream(parts, 1, value, false), call->type, call->loc());
}

}
