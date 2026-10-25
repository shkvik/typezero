// spawnSync, execFileSync and execSync of node:child_process
// (docs/process.md): a call of tz_child_run in
// runtime/library/child_process/child/child.c.
//
// The call fills a TzSpawnRequest on the stack with its arguments, borrowed
// for the call (see ChildArg in compiler/program/ast/ast.h for what Sema
// gives). The runtime runs the program and gives back what happened as
// TzSpawnParts, which a maker of the program turns into the result of spawnSync
// or an ExecSyncError: it takes each field the type has from the parts,
// converted to the field's type (stdout may be a string, a Buffer, undefined or
// a union of them). For execSync and execFileSync, stdout comes back in the
// parts the call passes, and the call converts it the same way.

#include "compiler/lower/codegen/codegen.h"

#include "llvm/Support/ErrorHandling.h"

using llvm::cast;

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

llvm::StructType *CodeGen::spawnPartsType() {
  llvm::Type *ptr = builder_.getPtrTy(), *i32 = builder_.getInt32Ty();
  return llvm::StructType::get(ctx_,
                               {strTy_, strTy_, ptr, strTy_, strTy_, strTy_, ptr, strTy_,
                                llvm::ArrayType::get(strTy_, 3), llvm::ArrayType::get(ptr, 3), i32,
                                i32, i32, i32, i32, i32, i32, llvm::ArrayType::get(i32, 3)});
}

llvm::Value *CodeGen::spawnStream(llvm::Value *parts, unsigned stream, const Type *to,
                                  bool retain) {
  llvm::StructType *partsType = spawnPartsType();
  llvm::Type *i32 = builder_.getInt32Ty();
  auto field = [&](unsigned f, unsigned i) {
    return builder_.CreateConstInBoundsGEP2_32(partsType->getElementType(f),
                                               builder_.CreateStructGEP(partsType, parts, f), 0, i);
  };
  llvm::Value *piped =
      builder_.CreateICmpNE(builder_.CreateLoad(i32, field(PtPiped, stream)), builder_.getInt32(0));
  // The parts not in use are zero, which needs no counting: a value made of
  // them is only dropped.
  auto part = [&](const Type *member) {
    llvm::Value *raw =
        builder_.CreateLoad(lower(member), field(member->isString() ? PtText : PtBytes, stream));
    if (retain)
      retainValue(raw, member);
    return raw;
  };
  bool text = holds(to, Type::string()), bytes = holds(to, bufferType());
  if (!to->isOptional() && !to->isUnion()) // the stream is a pipe
    return part(to);
  if (to->isOptional() && !to->inner()->isUnion())
    return makeOptional(part(to->inner()), piped, to);
  llvm::Value *value;
  if (text && bytes) {
    llvm::Value *mode = builder_.CreateICmpNE(
        builder_.CreateLoad(i32, builder_.CreateStructGEP(partsType, parts, PtTextMode)),
        builder_.getInt32(0));
    llvm::Value *fromText = emitConversion(part(Type::string()), Type::string(), to);
    llvm::Value *fromBytes = emitConversion(part(bufferType()), bufferType(), to);
    value = builder_.CreateSelect(mode, fromText, fromBytes);
  } else {
    const Type *member = text ? Type::string() : bufferType();
    value = emitConversion(part(member), member, to);
  }
  return to->isOptional() ? builder_.CreateSelect(piped, value, undefinedValue(to)) : value;
}

llvm::Function *CodeGen::spawnMaker(const Type *type) {
  llvm::Function *&maker = spawnMakers_[type];
  if (maker)
    return maker;
  llvm::Type *ptr = builder_.getPtrTy();
  const ClassDecl *cls = type->classDecl();
  maker = llvm::Function::Create(
      llvm::FunctionType::get(ptr, {ptr}, false), llvm::GlobalValue::InternalLinkage,
      cls ? "tz.make." + cls->name.str() : "tz.make.SpawnSyncReturns", module_.get());
  setFunctionAttributes(maker);
  llvm::IRBuilderBase::InsertPoint point = builder_.saveIP();
  llvm::Function *outer = std::exchange(function_, maker);
  builder_.SetInsertPoint(llvm::BasicBlock::Create(ctx_, "entry", maker));

  llvm::Value *parts = maker->getArg(0);
  llvm::StructType *partsType = spawnPartsType();
  llvm::Value *object;
  if (cls) {
    object = allocInstance(cls);
  } else {
    std::vector<unsigned> keys;
    for (const Field &f : type->fields())
      keys.push_back(fieldIndex(type, f.name));
    object = allocObject(type, keys);
  }
  auto load = [&](unsigned f) {
    return builder_.CreateLoad(partsType->getElementType(f),
                               builder_.CreateStructGEP(partsType, parts, f));
  };
  auto flag = [&](unsigned f) { return builder_.CreateICmpNE(load(f), builder_.getInt32(0)); };
  for (const Field &f : type->fields()) {
    llvm::StringRef name = f.name;
    const Type *t = f.type;
    llvm::Value *value;
    if (name == "message")
      value = load(PtMessage);
    else if (name == "name")
      value = load(PtName);
    else if (name == "error")
      value = load(PtError); // a SystemError, or null for undefined
    else if (name == "errno")
      value = makeOptional(load(PtErrno), flag(PtHasCode), t);
    else if (name == "code")
      value = makeOptional(load(PtCode), flag(PtHasCode), t);
    else if (name == "syscall")
      value = makeOptional(load(PtSyscall), flag(PtHasCode), t);
    else if (name == "path")
      value = makeOptional(load(PtPath), flag(PtHasCode), t);
    else if (name == "spawnargs")
      value = makeOptional(load(PtSpawnargs), flag(PtHasCode), t);
    else if (name == "status")
      value = makeOptional(load(PtStatus), flag(PtHasStatus), t);
    else if (name == "signal")
      value = makeOptional(load(PtSignal), flag(PtHasSignal), t);
    else if (name == "pid")
      value = load(PtPid);
    else if (name == "stdout" || name == "stderr")
      value = spawnStream(parts, name == "stdout" ? 1 : 2, t, /*retain=*/false);
    else if (name == "output") {
      // [undefined, stdout, stderr]: stdin is never output. The fields
      // stdout and stderr hold them too.
      const Type *element = t->inner();
      llvm::Value *count = builder_.getInt64(3);
      value = builder_.CreateCall(rt_.arrNew, {typeDescriptor(element), count, count}, "output");
      llvm::Value *data = arrayData(value);
      for (unsigned i = 0; i < 3; ++i)
        builder_.CreateStore(spawnStream(parts, i, element, /*retain=*/true),
                             elementAddress(data, element, builder_.getInt64(i)));
    } else {
      continue; // cause and the places of an error, which start empty
    }
    builder_.CreateStore(value, fieldAddress(object, type, name, /*exact=*/true));
  }
  builder_.CreateRet(object);
  function_ = outer;
  builder_.restoreIP(point);
  return maker;
}

llvm::Value *CodeGen::emitChildBuiltin(CallExpr *call) {
  llvm::Type *ptr = builder_.getPtrTy(), *i32 = builder_.getInt32Ty();
  // The strings of TzSpawnRequest, args among them, its pointers and
  // doubles, then its ints from mode to gid, stdio and the encoding.
  llvm::Type *f64 = builder_.getDoubleTy();
  std::vector<llvm::Type *> fields(RqEnv, strTy_);
  fields[RqArgs] = ptr;
  fields.insert(fields.end(), {ptr, ptr, ptr, f64, f64});
  fields.insert(fields.end(), RqStdio - RqMode, i32);
  fields.push_back(llvm::ArrayType::get(i32, 3));
  fields.push_back(i32);
  llvm::StructType *requestType = llvm::StructType::get(ctx_, fields);

  // The arguments, owned, released after the call: the runtime borrows them.
  std::vector<llvm::Value *> values = emitHeldValues(call->args);
  llvm::AllocaInst *request = createStackSlot(requestType, "spawn.request");
  builder_.CreateStore(llvm::Constant::getNullValue(requestType), request);
  auto set = [&](unsigned f, llvm::Value *value) {
    builder_.CreateStore(value, builder_.CreateStructGEP(requestType, request, f));
  };
  auto literal = [&](unsigned arg) { return unsigned(cast<IntLitExpr>(call->args[arg])->value); };
  // Which options the call gave, as bits, and the value of each.
  llvm::Value *has = builder_.getInt32(literal(ChildStdioGiven) ? HasStdio : 0);
  auto given = [&](unsigned arg) -> std::pair<llvm::Value *, llvm::Value *> {
    const Type *type = call->args[arg]->type;
    if (!type->isOptional())
      return {builder_.getTrue(), values[arg]};
    return {optionalHas(values[arg], type), optionalValue(values[arg], type)};
  };
  auto option = [&](unsigned arg, unsigned f, uint32_t bit) {
    auto [present, value] = given(arg);
    if (bit)
      has = builder_.CreateOr(
          has, builder_.CreateSelect(present, builder_.getInt32(bit), builder_.getInt32(0)));
    set(f, value);
    return present;
  };
  // A boolean option, false when not given.
  auto boolean = [&](unsigned arg, unsigned f) {
    auto [present, value] = given(arg);
    set(f, builder_.CreateZExt(builder_.CreateAnd(present, value), i32));
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
        builder_.CreateSelect(present, builder_.getInt32(text ? 1 : 2), builder_.getInt32(0)));
  }
  option(ChildTimeout, RqTimeout, HasTimeout);
  option(ChildMaxBuffer, RqMaxBuffer, HasMaxBuffer);
  if (call->args[ChildKillSignal]->type->nonOptional()->isString())
    option(ChildKillSignal, RqKillName, HasKillName);
  else
    option(ChildKillSignal, RqKillNumber, HasKillNumber);
  if (call->args[ChildShell]->type->nonOptional()->isString()) {
    llvm::Value *present = option(ChildShell, RqShell, 0);
    set(RqShellKind, builder_.CreateSelect(present, builder_.getInt32(2), builder_.getInt32(0)));
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
  set(RqStringType, typeDescriptor(Type::string()));
  set(RqMode, builder_.getInt32(call->builtin == Builtin::ChildSpawnSync      ? 0
                                : call->builtin == Builtin::ChildExecFileSync ? 1
                                                                              : 2));
  for (unsigned i = 0; i < 3; ++i)
    builder_.CreateStore(builder_.getInt32(literal(ChildStdin + i)),
                         builder_.CreateConstInBoundsGEP2_32(
                             requestType->getElementType(RqStdio),
                             builder_.CreateStructGEP(requestType, request, RqStdio), 0, i));
  set(RqEncoding, builder_.getInt32(literal(ChildEncoding)));

  // What the program gives: the result of spawnSync, or an ExecSyncError
  // when execSync and execFileSync fail.
  const Type *made = call->type;
  if (call->builtin != Builtin::ChildSpawnSync) {
    made = nullptr;
    for (const ClassDecl *child : errorClass_->children)
      if (child->builtin && child->name == "ExecSyncError")
        made = child->type;
    if (!made)
      llvm_unreachable("Sema declares ExecSyncError");
  }
  llvm::AllocaInst *parts = createStackSlot(spawnPartsType(), "spawn.parts");
  llvm::Value *result =
      builder_.CreateCall(processFunction("tz_child_run", ptr, {ptr, ptr, ptr, ptr}),
                          {request, parts, systemErrorMaker(), spawnMaker(made)});
  for (size_t i = 0; i < values.size(); ++i)
    releaseValue(values[i], call->args[i]->type);
  emitThrowCheck();
  if (call->builtin == Builtin::ChildSpawnSync)
    return result;
  return spawnStream(parts, 1, call->type, /*retain=*/false);
}

} // namespace tz
