#include "compiler/library/child_process/child_lowerer/child_lowerer.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/function_builder/function_builder.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/layouts/type_descriptors/type_descriptors.h"
#include "compiler/lower/values/array_lowerer/array_lowerer.h"
#include "compiler/lower/classes/class_lowerer/class_lowerer.h"
#include "compiler/lower/layouts/object_layouts/object_layouts.h"
#include "compiler/lower/errors/throw_lowerer/throw_lowerer.h"
#include "compiler/library/builtin_modules/builtin_module_lowerer/builtin_module_lowerer.h"
#include "compiler/library/process/process_lowerer/process_lowerer.h"
#include "llvm/Support/ErrorHandling.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::BasicBlock;
using llvm::Value;

namespace tz {

namespace {

// The fields of TzSpawnRequest and TzSpawnParts
// (runtime/library/child_process/child/child.c).
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
  PtName,
  PtError,
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
};

// HAS_CWD and the others of runtime/library/child_process/child/child.c.
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

// Whether a value of `type` may be text, or a Buffer.
bool holds(const Type *type, const Type *member) {
  type = type->nonOptional();

  return type == member || llvm::is_contained(type->members(), member);
}

} // namespace

ChildLowerer::ChildLowerer(CodeGen &codegen) : codegen_(codegen) {}

llvm::StructType *ChildLowerer::spawnPartsType() {
  llvm::Type *ptr = codegen_.builder_.getPtrTy(), *i32 = codegen_.builder_.getInt32Ty();

  return llvm::StructType::get(codegen_.ctx_,
                               {codegen_.strTy_, codegen_.strTy_, ptr, codegen_.strTy_, codegen_.strTy_, codegen_.strTy_, ptr, codegen_.strTy_,
                                llvm::ArrayType::get(codegen_.strTy_, 3), llvm::ArrayType::get(ptr, 3), i32,
                                i32, i32, i32, i32, i32, i32, llvm::ArrayType::get(i32, 3)});
}

llvm::Value *ChildLowerer::spawnStream(llvm::Value *parts, unsigned stream, const Type *to,
                                  bool retain) {
  llvm::StructType *partsType = spawnPartsType();
  llvm::Type *i32 = codegen_.builder_.getInt32Ty();
  auto field = [&](unsigned f, unsigned i) {
    return codegen_.builder_.CreateConstInBoundsGEP2_32(partsType->getElementType(f),
                                               codegen_.builder_.CreateStructGEP(partsType, parts, f), 0, i);
  };
  llvm::Value *piped =
      codegen_.builder_.CreateICmpNE(codegen_.builder_.CreateLoad(i32, field(PtPiped, stream)), codegen_.builder_.getInt32(0));
  // The parts not in use are zero, which needs no counting: a value made of
  // them is only dropped.
  auto part = [&](const Type *member) {
    llvm::Value *raw =
        codegen_.builder_.CreateLoad(codegen_.moduleBuilder().lower(member), field(member->isString() ? PtText : PtBytes, stream));
    if (retain) {
      codegen_.refCounts().retainValue(raw, member);
    }

    return raw;
  };
  bool text = holds(to, Type::string()), bytes = holds(to, bufferType());
  if (!to->isOptional() && !to->isUnion()) { // the stream is a pipe
    return part(to);
  }
  if (to->isOptional() && !to->inner()->isUnion()) {
    return codegen_.optionalLowerer().makeOptional(part(to->inner()), piped, to);
  }
  llvm::Value *value;
  if (text && bytes) {
    llvm::Value *mode = codegen_.builder_.CreateICmpNE(
        codegen_.builder_.CreateLoad(i32, codegen_.builder_.CreateStructGEP(partsType, parts, PtTextMode)),
        codegen_.builder_.getInt32(0));
    llvm::Value *fromText = codegen_.exprLowerer().emitConversion(part(Type::string()), Type::string(), to);
    llvm::Value *fromBytes = codegen_.exprLowerer().emitConversion(part(bufferType()), bufferType(), to);
    value = codegen_.builder_.CreateSelect(mode, fromText, fromBytes);
  } else {
    const Type *member = text ? Type::string() : bufferType();
    value = codegen_.exprLowerer().emitConversion(part(member), member, to);
  }

  return to->isOptional() ? codegen_.builder_.CreateSelect(piped, value, codegen_.optionalLowerer().undefinedValue(to)) : value;
}

llvm::Function *ChildLowerer::spawnMaker(const Type *type) {
  llvm::Function *&maker = spawnMakers_[type];
  if (maker) {
    return maker;
  }
  llvm::Type *ptr = codegen_.builder_.getPtrTy();
  const ClassDecl *cls = type->classDecl();
  maker = llvm::Function::Create(
      llvm::FunctionType::get(ptr, {ptr}, false), llvm::GlobalValue::InternalLinkage,
      cls ? "tz.make." + cls->name.str() : "tz.make.SpawnSyncReturns", codegen_.module_.get());
  codegen_.functionBuilder().setFunctionAttributes(maker);
  llvm::IRBuilderBase::InsertPoint point = codegen_.builder_.saveIP();
  llvm::Function *outer = std::exchange(codegen_.function_, maker);
  codegen_.builder_.SetInsertPoint(llvm::BasicBlock::Create(codegen_.ctx_, "entry", maker));

  llvm::Value *parts = maker->getArg(0);
  llvm::StructType *partsType = spawnPartsType();
  llvm::Value *object;
  if (cls) {
    object = codegen_.classLowerer().allocInstance(cls);
  } else {
    std::vector<unsigned> keys;
    for (const Field &f : type->fields()) {
      keys.push_back(ObjectLayouts::fieldIndex(type, f.name));
    }
    object = codegen_.objectLayouts().allocObject(type, keys);
  }
  auto load = [&](unsigned f) {
    return codegen_.builder_.CreateLoad(partsType->getElementType(f),
                               codegen_.builder_.CreateStructGEP(partsType, parts, f));
  };
  auto flag = [&](unsigned f) { return codegen_.builder_.CreateICmpNE(load(f), codegen_.builder_.getInt32(0)); };
  for (const Field &f : type->fields()) {
    llvm::StringRef name = f.name;
    const Type *t = f.type;
    llvm::Value *value;
    if (name == "message") {
      value = load(PtMessage);
    } else if (name == "name") {
      value = load(PtName);
    } else if (name == "error") {
      value = load(PtError); // a SystemError, or null for undefined
    } else if (name == "errno") {
      value = codegen_.optionalLowerer().makeOptional(load(PtErrno), flag(PtHasCode), t);
    } else if (name == "code") {
      value = codegen_.optionalLowerer().makeOptional(load(PtCode), flag(PtHasCode), t);
    } else if (name == "syscall") {
      value = codegen_.optionalLowerer().makeOptional(load(PtSyscall), flag(PtHasCode), t);
    } else if (name == "path") {
      value = codegen_.optionalLowerer().makeOptional(load(PtPath), flag(PtHasCode), t);
    } else if (name == "spawnargs") {
      value = codegen_.optionalLowerer().makeOptional(load(PtSpawnargs), flag(PtHasCode), t);
    } else if (name == "status") {
      value = codegen_.optionalLowerer().makeOptional(load(PtStatus), flag(PtHasStatus), t);
    } else if (name == "signal") {
      value = codegen_.optionalLowerer().makeOptional(load(PtSignal), flag(PtHasSignal), t);
    } else if (name == "pid") {
      value = load(PtPid);
    } else if (name == "stdout" || name == "stderr") {
      value = spawnStream(parts, name == "stdout" ? 1 : 2, t, /*retain=*/false);
    } else if (name == "output") {
      // [undefined, stdout, stderr]: stdin is never output. The fields
      // stdout and stderr hold them too.
      const Type *element = t->inner();
      llvm::Value *count = codegen_.builder_.getInt64(3);
      value = codegen_.builder_.CreateCall(codegen_.rt_.arrNew, {codegen_.typeDescriptors().typeDescriptor(element), count, count}, "output");
      llvm::Value *data = codegen_.arrayLowerer().arrayData(value);
      for (unsigned i = 0; i < 3; ++i) {
        codegen_.builder_.CreateStore(spawnStream(parts, i, element, /*retain=*/true),
                             codegen_.arrayLowerer().elementAddress(data, element, codegen_.builder_.getInt64(i)));
      }
    } else {
      continue; // cause and the places of an error, which start empty
    }
    codegen_.builder_.CreateStore(value, codegen_.objectLayouts().fieldAddress(object, type, name, /*exact=*/true));
  }
  codegen_.builder_.CreateRet(object);
  codegen_.function_ = outer;
  codegen_.builder_.restoreIP(point);

  return maker;
}

llvm::Value *ChildLowerer::emitChildBuiltin(CallExpr *call) {
  llvm::Type *ptr = codegen_.builder_.getPtrTy(), *i32 = codegen_.builder_.getInt32Ty();
  // The strings of TzSpawnRequest, args among them, its pointers and
  // doubles, then its ints from mode to gid, stdio and the encoding.
  llvm::Type *f64 = codegen_.builder_.getDoubleTy();
  std::vector<llvm::Type *> fields(RqEnv, codegen_.strTy_);
  fields[RqArgs] = ptr;
  fields.insert(fields.end(), {ptr, ptr, ptr, f64, f64});
  fields.insert(fields.end(), RqStdio - RqMode, i32);
  fields.push_back(llvm::ArrayType::get(i32, 3));
  fields.push_back(i32);
  llvm::StructType *requestType = llvm::StructType::get(codegen_.ctx_, fields);

  // The arguments, owned, released after the call: the runtime borrows them.
  std::vector<llvm::Value *> values = codegen_.throwLowerer().emitHeldValues(call->args);
  llvm::AllocaInst *request = codegen_.functionFrame().createStackSlot(requestType, "spawn.request");
  codegen_.builder_.CreateStore(llvm::Constant::getNullValue(requestType), request);
  auto set = [&](unsigned f, llvm::Value *value) {
    codegen_.builder_.CreateStore(value, codegen_.builder_.CreateStructGEP(requestType, request, f));
  };
  auto literal = [&](unsigned arg) { return unsigned(cast<IntLitExpr>(call->args[arg])->value); };
  // Which options the call gave, as bits, and the value of each.
  llvm::Value *has = codegen_.builder_.getInt32(literal(ChildStdioGiven) ? HasStdio : 0);
  auto given = [&](unsigned arg) -> std::pair<llvm::Value *, llvm::Value *> {
    const Type *type = call->args[arg]->type;
    if (!type->isOptional()) {
      return {codegen_.builder_.getTrue(), values[arg]};
    }

    return {codegen_.optionalLowerer().optionalHas(values[arg], type), codegen_.optionalLowerer().optionalValue(values[arg], type)};
  };
  auto option = [&](unsigned arg, unsigned f, uint32_t bit) {
    auto [present, value] = given(arg);
    if (bit) {
      has = codegen_.builder_.CreateOr(
          has, codegen_.builder_.CreateSelect(present, codegen_.builder_.getInt32(bit), codegen_.builder_.getInt32(0)));
    }
    set(f, value);

    return present;
  };
  // A boolean option, false when not given.
  auto boolean = [&](unsigned arg, unsigned f) {
    auto [present, value] = given(arg);
    set(f, codegen_.builder_.CreateZExt(codegen_.builder_.CreateAnd(present, value), i32));
  };

  set(RqFile, values[ChildFile]);
  set(RqArgs, values[ChildArgs]);
  option(ChildCwd, RqCwd, HasCwd);
  option(ChildEnv, RqEnv, 0); // null for undefined
  {
    const Type *input = call->args[ChildInput]->type->nonOptional();
    bool text = input->isString();
    llvm::Value *present = option(ChildInput, text ? RqInputText : RqInputBytes, 0);
    set(RqInput,
        codegen_.builder_.CreateSelect(present, codegen_.builder_.getInt32(text ? 1 : 2), codegen_.builder_.getInt32(0)));
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
    set(RqShellKind, codegen_.builder_.CreateSelect(present, codegen_.builder_.getInt32(2), codegen_.builder_.getInt32(0)));
  } else {
    boolean(ChildShell, RqShellKind); // 1 for the system's
  }
  boolean(ChildWindowsHide, RqWindowsHide);
  boolean(ChildVerbatim, RqVerbatim);
  boolean(ChildDetached, RqDetached);
  option(ChildArgv0, RqArgv0, HasArgv0);
  option(ChildUid, RqUid, HasUid);
  option(ChildGid, RqGid, HasGid);
  set(RqHas, has);
  set(RqStringType, codegen_.typeDescriptors().typeDescriptor(Type::string()));
  set(RqMode, codegen_.builder_.getInt32(call->builtin == Builtin::ChildSpawnSync      ? 0
                                : call->builtin == Builtin::ChildExecFileSync ? 1
                                                                              : 2));
  for (unsigned i = 0; i < 3; ++i) {
    codegen_.builder_.CreateStore(codegen_.builder_.getInt32(literal(ChildStdin + i)),
                         codegen_.builder_.CreateConstInBoundsGEP2_32(
                             requestType->getElementType(RqStdio),
                             codegen_.builder_.CreateStructGEP(requestType, request, RqStdio), 0, i));
  }
  set(RqEncoding, codegen_.builder_.getInt32(literal(ChildEncoding)));

  // What the program gives: the result of spawnSync, or an ExecSyncError
  // when execSync and execFileSync fail.
  const Type *made = call->type;
  if (call->builtin != Builtin::ChildSpawnSync) {
    made = nullptr;
    for (const ClassDecl *child : codegen_.errorClass_->children) {
      if (child->builtin && child->name == "ExecSyncError") {
        made = child->type;
      }
    }
    if (!made) {
      llvm_unreachable("Sema declares ExecSyncError");
    }
  }
  llvm::AllocaInst *parts = codegen_.functionFrame().createStackSlot(spawnPartsType(), "spawn.parts");
  llvm::Value *result =
      codegen_.builder_.CreateCall(codegen_.processLowerer().processFunction("tz_child_run", ptr, {ptr, ptr, ptr, ptr}),
                          {request, parts, codegen_.builtinModuleLowerer().systemErrorMaker(), spawnMaker(made)});
  for (size_t i = 0; i < values.size(); ++i) {
    codegen_.refCounts().releaseValue(values[i], call->args[i]->type);
  }
  codegen_.throwLowerer().emitThrowCheck();
  if (call->builtin == Builtin::ChildSpawnSync) {
    return result;
  }

  return spawnStream(parts, 1, call->type, /*retain=*/false);
}

} // namespace tz
