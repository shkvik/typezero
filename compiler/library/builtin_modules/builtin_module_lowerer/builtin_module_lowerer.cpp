#include "compiler/library/builtin_modules/builtin_module_lowerer/builtin_module_lowerer.h"
#include "compiler/library/date/date_lowerer/date_lowerer.h"
#include "compiler/library/fs/fs_promises_lowerer/fs_promises_lowerer.h"
#include "compiler/library/process/process_lowerer/process_lowerer.h"
#include "compiler/lower/classes/class_lowerer/class_lowerer.h"
#include "compiler/lower/errors/throw_lowerer/throw_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/function_builder/function_builder.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/layouts/object_layouts/object_layouts.h"
#include "compiler/lower/layouts/type_descriptors/type_descriptors.h"
#include "compiler/lower/memory/group_lowerer/group_lowerer.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ErrorHandling.h"
#include <vector>

using llvm::BasicBlock;
using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::Value;

namespace tz {

namespace {

// The fields of the objects that makers make, in the order of the parts:
// by the name of a class, or for an object type the one with these fields.
struct MakerFields {
  const char *cls; // null for an object type
  std::vector<llvm::StringRef> fields;
};

const std::vector<MakerFields> &makerFields() {
  static const std::vector<MakerFields> table = {
      {"SystemError", {"message", "name", "errno", "code", "syscall", "path", "dest"}},
      {"Stats",
       {"dev", "mode", "nlink", "uid", "gid", "rdev", "blksize", "ino", "size", "blocks", "atimeMs",
        "mtimeMs", "ctimeMs", "birthtimeMs"}},
      {"Dirent", {"name", "parentPath", "#type"}},
      {nullptr, {"root", "dir", "base", "ext", "name"}},         // ParsedPath
      {nullptr, {"model", "speed", "times"}},                    // CpuInfo
      {nullptr, {"user", "nice", "sys", "idle", "irq"}},         // its times
      {nullptr, {"uid", "gid", "username", "homedir", "shell"}}, // UserInfo
  };

  return table;
}

const std::vector<llvm::StringRef> &makerOrder(const Type *type) {
  for (const MakerFields &entry : makerFields()) {
    if (const ClassDecl *cls = type->classDecl()) {
      if (entry.cls && cls->name == entry.cls) {
        return entry.fields;
      }
      continue;
    }
    if (entry.cls || type->fields().size() != entry.fields.size()) {
      continue;
    }
    if (llvm::all_of(entry.fields, [&](llvm::StringRef f) { return type->field(f) != nullptr; })) {
      return entry.fields;
    }
  }
  llvm_unreachable("no maker for this type");
}

// A field whose object is made inline, from the parts that follow.
bool isNestedPart(const Type *type) { return type->isObject() && !type->isClass(); }

} // namespace

BuiltinModuleLowerer::BuiltinModuleLowerer(Lowering &lowering) : lowering_(lowering) {}

llvm::StructType *BuiltinModuleLowerer::makerParts(const Type *type) {
  std::vector<llvm::Type *> elements;
  for (llvm::StringRef name : makerOrder(type)) {
    const Type *field = type->field(name)->type;
    elements.push_back(isNestedPart(field) ? makerParts(field)
                                           : lowering_.moduleBuilder().lower(field));
  }

  return llvm::StructType::get(lowering_.ctx_, elements);
}

llvm::Function *BuiltinModuleLowerer::objectMaker(const Type *type) {
  llvm::Function *&maker = objectMakers_[type];
  if (maker) {
    return maker;
  }
  llvm::Type *ptr = lowering_.builder_.getPtrTy();
  const ClassDecl *cls = type->classDecl();
  std::string name = cls                             ? cls->name.str()
                     : type->name().starts_with("{") ? "object"
                                                     : type->name().str();
  maker = llvm::Function::Create(llvm::FunctionType::get(ptr, {ptr}, false),
                                 llvm::GlobalValue::InternalLinkage, "tz.make." + name,
                                 lowering_.module_.get());
  lowering_.functionBuilder().setFunctionAttributes(maker);
  llvm::IRBuilderBase::InsertPoint point = lowering_.builder_.saveIP();
  llvm::Function *outer = std::exchange(lowering_.function_, maker);
  lowering_.builder_.SetInsertPoint(llvm::BasicBlock::Create(lowering_.ctx_, "entry", maker));

  const std::vector<llvm::StringRef> &order = makerOrder(type);
  llvm::Value *object;
  if (cls) {
    object = lowering_.classLowerer().allocInstance(cls);
  } else {
    // The keys in the order Node prints them, which is that of the parts.
    std::vector<unsigned> keys;
    for (llvm::StringRef field : order) {
      keys.push_back(ObjectLayouts::fieldIndex(type, field));
    }
    object = lowering_.objectLayouts().allocObject(type, keys);
  }
  llvm::StructType *parts = makerParts(type);
  for (size_t i = 0; i < order.size(); ++i) {
    const Type *field = type->field(order[i])->type;
    llvm::Value *address = lowering_.builder_.CreateStructGEP(parts, maker->getArg(0), unsigned(i));
    llvm::Value *value = isNestedPart(field)
                             ? static_cast<llvm::Value *>(
                                   lowering_.builder_.CreateCall(objectMaker(field), {address}))
                             : lowering_.builder_.CreateLoad(lowering_.moduleBuilder().lower(field),
                                                             address, order[i]);
    lowering_.builder_.CreateStore(
        value, lowering_.objectLayouts().fieldAddress(object, type, order[i], /*exact=*/true));
  }
  lowering_.groupLowerer().adoptFields(object, type);
  lowering_.builder_.CreateRet(object);
  lowering_.function_ = outer;
  lowering_.builder_.restoreIP(point);

  return maker;
}

llvm::Function *BuiltinModuleLowerer::systemErrorMaker() {
  for (const ClassDecl *child : lowering_.errorClass_->children) {
    if (child->builtin && child->name == "SystemError") {
      return objectMaker(child->type);
    }
  }
  llvm_unreachable("Checker declares SystemError");
}

llvm::Value *BuiltinModuleLowerer::emitNodeBuiltin(CallExpr *call) {
  if (isFspBuiltin(call->builtin)) {
    return lowering_.fsPromisesLowerer().emitFsPromise(call);
  }
  if (call->builtin >= Builtin::RlCreate && call->builtin <= Builtin::RlClose) {
    return lowering_.fsPromisesLowerer().emitReadlineBuiltin(call);
  }
  llvm::Type *voidTy = lowering_.builder_.getVoidTy(), *i32 = lowering_.builder_.getInt32Ty();
  llvm::Type *i64 = lowering_.builder_.getInt64Ty(), *f64 = lowering_.builder_.getDoubleTy(),
             *ptr = lowering_.builder_.getPtrTy();

  if (call->builtin == Builtin::FsStatsDate) {
    // dateFromMs of Node: the milliseconds rounded, as Math.round does.
    auto *member = cast<MemberExpr>(call->callee);
    Lowering::Operand stats = lowering_.refCounts().emitOperand(member->object, /*canBorrow=*/true);
    llvm::Value *ms =
        lowering_.builder_.CreateLoad(f64,
                                      lowering_.objectLayouts().fieldAddress(
                                          stats.value, stats.type, (member->name + "Ms").str()),
                                      "ms");
    lowering_.refCounts().dropOperand(stats);
    llvm::Value *rounded = lowering_.builder_.CreateUnaryIntrinsic(
        llvm::Intrinsic::floor, lowering_.builder_.CreateFAdd(ms, llvm::ConstantFP::get(f64, 0.5)));
    return lowering_.dateLowerer().newDate(lowering_.builder_.CreateCall(
        lowering_.processLowerer().processFunction("tz_date_clip", f64, {f64}), {rounded}));
  }

  if (call->builtin == Builtin::FsIsKind) {
    // The kind of file is in the bits of the mode of a Stats, and in the
    // hidden field of a Dirent.
    auto *member = cast<MemberExpr>(call->callee);
    Lowering::Operand object =
        lowering_.refCounts().emitOperand(member->object, /*canBorrow=*/true);
    const Type *type = object.type;
    bool stats = type->field("mode") != nullptr;
    llvm::StringRef field = stats ? "mode" : "#type";
    llvm::Value *bits = lowering_.builder_.CreateLoad(
        stats ? f64 : i32, lowering_.objectLayouts().fieldAddress(object.value, type, field));
    if (stats) {
      bits = lowering_.builder_.CreateFPToSI(bits, i32);
    }
    lowering_.refCounts().dropOperand(object);
    llvm::Value *kind = lowering_.exprLowerer().emitExpr(call->args[0]);
    return lowering_.builder_.CreateICmpEQ(
        lowering_.builder_.CreateAnd(bits, lowering_.builder_.getInt32(0170000)), kind);
  }

  // The arguments, owned, released after the call: the runtime borrows them.
  std::vector<llvm::Value *> values = lowering_.throwLowerer().emitHeldValues(call->args);
  auto release = [&] {
    for (size_t i = 0; i < values.size(); ++i) {
      lowering_.refCounts().releaseValue(values[i], call->args[i]->type);
    }
  };
  auto obj = [&](size_t i) { return lowering_.stringLowerer().stringObj(values[i]); };
  auto meta = [&](size_t i) { return lowering_.stringLowerer().stringMeta(values[i]); };
  auto flag = [&](size_t i) { return lowering_.builder_.CreateZExt(values[i], i32); };
  auto fn = [&](llvm::StringRef name, llvm::Type *ret, llvm::ArrayRef<llvm::Type *> params) {
    return lowering_.processLowerer().processFunction(name, ret, params);
  };
  // A runtime function that may throw a SystemError, with its maker last.
  auto fsCall = [&](llvm::StringRef name, llvm::Type *ret, std::vector<llvm::Type *> params,
                    std::vector<llvm::Value *> args) -> llvm::Value * {
    params.push_back(ptr);
    args.push_back(systemErrorMaker());
    llvm::Value *result = lowering_.builder_.CreateCall(fn(name, ret, params), args);
    release();
    lowering_.throwLowerer().emitThrowCheck();

    return result;
  };
  auto fsString = [&](llvm::StringRef name, std::vector<llvm::Type *> params,
                      std::vector<llvm::Value *> args) {
    llvm::AllocaInst *out = lowering_.functionFrame().createStackSlot(lowering_.strTy_, "fs.out");
    params.insert(params.begin(), ptr);
    args.insert(args.begin(), out);
    fsCall(name, voidTy, params, args);

    return lowering_.builder_.CreateLoad(lowering_.strTy_, out);
  };
  // A string or undefined, from a runtime function that says whether there
  // is one.
  auto optionalString = [&](llvm::Value *out, llvm::Value *found) {
    return lowering_.optionalLowerer().makeOptional(
        lowering_.builder_.CreateLoad(lowering_.strTy_, out),
        lowering_.builder_.CreateICmpNE(found, lowering_.builder_.getInt32(0)), call->type);
  };

  llvm::Value *result = nullptr;
  switch (call->builtin) {
  case Builtin::PathJoin:
  case Builtin::PathResolve:
    result = lowering_.stringLowerer().callReturningString(
        fn(call->builtin == Builtin::PathJoin ? "tz_path_join" : "tz_path_resolve", voidTy,
           {ptr, ptr, i32}),
        {values[1], values[0]});
    break;
  case Builtin::PathNormalize:
  case Builtin::PathDirname:
  case Builtin::PathExtname:
  case Builtin::PathToNamespacedPath: {
    const char *name = call->builtin == Builtin::PathNormalize ? "tz_path_normalize"
                       : call->builtin == Builtin::PathDirname ? "tz_path_dirname"
                       : call->builtin == Builtin::PathExtname ? "tz_path_extname"
                                                               : "tz_path_to_namespaced";
    result = lowering_.stringLowerer().callReturningString(fn(name, voidTy, {ptr, ptr, i64, i32}),
                                                           {obj(1), meta(1), values[0]});
    break;
  }
  case Builtin::PathIsAbsolute:
    result = lowering_.builder_.CreateICmpNE(
        lowering_.builder_.CreateCall(fn("tz_path_is_absolute", i32, {ptr, i64, i32}),
                                      {obj(1), meta(1), values[0]}),
        lowering_.builder_.getInt32(0));
    break;
  case Builtin::PathBasename: {
    bool suffix = values.size() > 2;
    llvm::Value *null = llvm::ConstantPointerNull::get(lowering_.builder_.getPtrTy());
    result = lowering_.stringLowerer().callReturningString(
        fn("tz_path_basename", voidTy, {ptr, ptr, i64, ptr, i64, i32, i32}),
        {obj(1), meta(1), suffix ? obj(2) : null, suffix ? meta(2) : lowering_.builder_.getInt64(0),
         lowering_.builder_.getInt32(suffix ? 1 : 0), values[0]});
    break;
  }
  case Builtin::PathRelative:
    result = lowering_.stringLowerer().callReturningString(
        fn("tz_path_relative", voidTy, {ptr, ptr, i64, ptr, i64, i32}),
        {obj(1), meta(1), obj(2), meta(2), values[0]});
    break;
  case Builtin::PathParse:
    result = lowering_.builder_.CreateCall(fn("tz_path_parse", ptr, {ptr, i64, i32, ptr}),
                                           {obj(1), meta(1), values[0], objectMaker(call->type)});
    break;
  case Builtin::PathFormat: {
    // The five parts as `string | undefined`, from the fields the object has.
    const Type *object = call->args[1]->type;
    const Type *optional = Type::optional(Type::string());
    llvm::Type *part = lowering_.moduleBuilder().lower(optional);
    auto *partsType = llvm::ArrayType::get(part, 5);
    llvm::AllocaInst *parts = lowering_.functionFrame().createStackSlot(partsType, "format.parts");
    static const llvm::StringLiteral names[] = {"root", "dir", "base", "ext", "name"};
    for (unsigned i = 0; i < 5; ++i) {
      const Field *field = object->field(names[i]);
      llvm::Value *value;
      if (!field) {
        value = lowering_.optionalLowerer().undefinedValue(optional);
      } else {
        value = lowering_.builder_.CreateLoad(
            lowering_.moduleBuilder().lower(field->type),
            lowering_.objectLayouts().fieldAddress(values[1], object, names[i]));
        if (!field->type->isOptional()) {
          value = lowering_.optionalLowerer().makeOptional(value, lowering_.builder_.getTrue(),
                                                           optional);
        }
      }
      lowering_.builder_.CreateStore(
          value, lowering_.builder_.CreateConstInBoundsGEP2_32(partsType, parts, 0, i));
    }
    result = lowering_.stringLowerer().callReturningString(
        fn("tz_path_format", voidTy, {ptr, ptr, i32}), {parts, values[0]});
    break;
  }
  case Builtin::OsString:
    result = lowering_.stringLowerer().callReturningString(fn("tz_os_string", voidTy, {ptr, i32}),
                                                           {values[0]});
    break;
  case Builtin::OsNumber:
    result = lowering_.builder_.CreateCall(fn("tz_os_number", f64, {i32}), {values[0]});
    break;
  case Builtin::OsAvailableParallelism:
    result = lowering_.builder_.CreateCall(fn("tz_os_available_parallelism", i32, {}));
    break;
  case Builtin::OsCpus: {
    const Type *element = call->type->inner();
    result = lowering_.builder_.CreateCall(
        fn("tz_os_cpus", ptr, {ptr, ptr}),
        {lowering_.typeDescriptors().typeDescriptor(element), objectMaker(element)});
    break;
  }
  case Builtin::OsLoadavg:
    result = lowering_.builder_.CreateCall(
        fn("tz_os_loadavg", ptr, {ptr}),
        {lowering_.typeDescriptors().typeDescriptor(Type::doubleTy())});
    break;
  case Builtin::OsUserInfo:
    result =
        lowering_.builder_.CreateCall(fn("tz_os_user_info", ptr, {ptr}), {objectMaker(call->type)});
    break;
  case Builtin::ReadLine: {
    bool question = !values.empty();
    llvm::AllocaInst *out = lowering_.functionFrame().createStackSlot(lowering_.strTy_, "line");
    llvm::Value *found = lowering_.builder_.CreateCall(
        fn("tz_io_read_line", i32, {ptr, ptr, i64, i32}),
        {out, question ? obj(0) : llvm::ConstantPointerNull::get(lowering_.builder_.getPtrTy()),
         question ? meta(0) : lowering_.builder_.getInt64(0),
         lowering_.builder_.getInt32(question ? 1 : 0)});
    release();
    return optionalString(out, found);
  }
  case Builtin::FsReadFile:
    // Without an encoding, a Buffer; with one, its number last.
    if (call->type->isBytes()) {
      if (call->args[0]->type->isInt()) {
        return fsCall("tz_fs_read_fd_bytes", ptr, {i32}, {values[0]});
      }
      return fsCall("tz_fs_read_file_bytes", ptr, {ptr, i64, ptr, i64},
                    {obj(0), meta(0), obj(1), meta(1)});
    }
    if (call->args[0]->type->isInt()) {
      return fsString("tz_fs_read_fd", {i32, i32}, {values[0], values[2]});
    }
    return fsString("tz_fs_read_file", {ptr, i64, ptr, i64, i32},
                    {obj(0), meta(0), obj(1), meta(1), values[2]});
  case Builtin::FsWriteFile:
    // The bytes of a view, or text in the encoding last.
    if (!call->args[1]->type->isString()) {
      if (call->args[0]->type->isInt()) {
        fsCall("tz_fs_write_fd_bytes", voidTy, {i32, ptr}, {values[0], values[1]});
      } else {
        fsCall("tz_fs_write_file_bytes", voidTy, {ptr, i64, ptr, ptr, i64, i32},
               {obj(0), meta(0), values[1], obj(2), meta(2), values[3]});
      }
    } else if (call->args[0]->type->isInt()) {
      fsCall("tz_fs_write_fd", voidTy, {i32, ptr, i64, i32},
             {values[0], obj(1), meta(1), values[4]});
    } else {
      fsCall("tz_fs_write_file", voidTy, {ptr, i64, ptr, i64, ptr, i64, i32, i32},
             {obj(0), meta(0), obj(1), meta(1), obj(2), meta(2), values[3], values[4]});
    }
    return nullptr;
  case Builtin::FsExists:
    result = lowering_.builder_.CreateICmpNE(
        lowering_.builder_.CreateCall(fn("tz_fs_exists", i32, {ptr, i64}), {obj(0), meta(0)}),
        lowering_.builder_.getInt32(0));
    break;
  case Builtin::FsAccess:
  case Builtin::FsChmod:
    fsCall(call->builtin == Builtin::FsAccess ? "tz_fs_access" : "tz_fs_chmod", voidTy,
           {ptr, i64, i32}, {obj(0), meta(0), values[1]});
    return nullptr;
  case Builtin::FsTruncate:
    fsCall("tz_fs_truncate", voidTy, {ptr, i64, i64}, {obj(0), meta(0), values[1]});
    return nullptr;
  case Builtin::FsStat:
    return fsCall("tz_fs_stat", ptr, {ptr, i64, i32, i32, ptr},
                  {obj(0), meta(0), flag(1), flag(2), objectMaker(call->type->nonOptional())});
  case Builtin::FsReaddir: {
    const Type *element = call->type->inner();
    llvm::Value *dirent = element->isClass()
                              ? static_cast<llvm::Value *>(objectMaker(element))
                              : llvm::ConstantPointerNull::get(lowering_.builder_.getPtrTy());
    return fsCall(
        "tz_fs_readdir", ptr, {ptr, i64, i32, ptr, ptr},
        {obj(0), meta(0), flag(1), lowering_.typeDescriptors().typeDescriptor(element), dirent});
  }
  case Builtin::FsMkdir: {
    llvm::AllocaInst *out =
        lowering_.functionFrame().createStackSlot(lowering_.strTy_, "mkdir.first");
    llvm::Value *made = fsCall("tz_fs_mkdir", i32, {ptr, ptr, i64, i32, i32},
                               {out, obj(0), meta(0), flag(1), values[2]});
    return optionalString(out, made);
  }
  case Builtin::FsMkdtemp:
    return fsString("tz_fs_mkdtemp", {ptr, i64}, {obj(0), meta(0)});
  case Builtin::FsRealpath:
    return fsString("tz_fs_realpath", {ptr, i64}, {obj(0), meta(0)});
  case Builtin::FsRmdir:
  case Builtin::FsUnlink:
    fsCall(call->builtin == Builtin::FsRmdir ? "tz_fs_rmdir" : "tz_fs_unlink", voidTy, {ptr, i64},
           {obj(0), meta(0)});
    return nullptr;
  case Builtin::FsRm:
    fsCall("tz_fs_rm", voidTy, {ptr, i64, i32, i32}, {obj(0), meta(0), flag(1), flag(2)});
    return nullptr;
  case Builtin::FsRename:
    fsCall("tz_fs_rename", voidTy, {ptr, i64, ptr, i64}, {obj(0), meta(0), obj(1), meta(1)});
    return nullptr;
  case Builtin::FsCopyFile:
    fsCall("tz_fs_copy_file", voidTy, {ptr, i64, ptr, i64, i32},
           {obj(0), meta(0), obj(1), meta(1), values[2]});
    return nullptr;
  default:
    llvm_unreachable("not a built-in of a module");
  }
  release();

  return result;
}

} // namespace tz
