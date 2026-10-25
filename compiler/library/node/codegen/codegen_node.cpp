// The built-in modules (docs/io.md): calls of runtime/path.c, os.c and fs.c.
//
// What the runtime makes objects of (a SystemError, a Stats, a Dirent, what
// path.parse() gives...) it makes through a function of the program, a
// maker: the runtime fills a struct with the fields of the object, in an
// order both sides know (makerFields here, the Tz...Parts structs in
// runtime/tzrt.h), and the maker allocates the object and moves them in.
// The program passes its makers with each call.

#include "codegen/codegen.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ErrorHandling.h"

#include <vector>

using llvm::cast;

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
      if (entry.cls && cls->name == entry.cls)
        return entry.fields;
      continue;
    }
    if (entry.cls || type->fields().size() != entry.fields.size())
      continue;
    if (llvm::all_of(entry.fields, [&](llvm::StringRef f) { return type->field(f) != nullptr; }))
      return entry.fields;
  }
  llvm_unreachable("no maker for this type");
}

// A field whose object is made inline, from the parts that follow.
bool isNestedPart(const Type *type) { return type->isObject() && !type->isClass(); }

} // namespace

llvm::StructType *CodeGen::makerParts(const Type *type) {
  std::vector<llvm::Type *> elements;
  for (llvm::StringRef name : makerOrder(type)) {
    const Type *field = type->field(name)->type;
    elements.push_back(isNestedPart(field) ? makerParts(field) : lower(field));
  }
  return llvm::StructType::get(ctx_, elements);
}

llvm::Function *CodeGen::objectMaker(const Type *type) {
  llvm::Function *&maker = objectMakers_[type];
  if (maker)
    return maker;
  llvm::Type *ptr = builder_.getPtrTy();
  const ClassDecl *cls = type->classDecl();
  std::string name = cls                             ? cls->name.str()
                     : type->name().starts_with("{") ? "object"
                                                     : type->name().str();
  maker =
      llvm::Function::Create(llvm::FunctionType::get(ptr, {ptr}, false),
                             llvm::GlobalValue::InternalLinkage, "tz.make." + name, module_.get());
  setFunctionAttributes(maker);
  llvm::IRBuilderBase::InsertPoint point = builder_.saveIP();
  llvm::Function *outer = std::exchange(function_, maker);
  builder_.SetInsertPoint(llvm::BasicBlock::Create(ctx_, "entry", maker));

  const std::vector<llvm::StringRef> &order = makerOrder(type);
  llvm::Value *object;
  if (cls) {
    object = allocInstance(cls);
  } else {
    // The keys in the order Node prints them, which is that of the parts.
    std::vector<unsigned> keys;
    for (llvm::StringRef field : order)
      keys.push_back(fieldIndex(type, field));
    object = allocObject(type, keys);
  }
  llvm::StructType *parts = makerParts(type);
  for (size_t i = 0; i < order.size(); ++i) {
    const Type *field = type->field(order[i])->type;
    llvm::Value *address = builder_.CreateStructGEP(parts, maker->getArg(0), unsigned(i));
    llvm::Value *value =
        isNestedPart(field)
            ? static_cast<llvm::Value *>(builder_.CreateCall(objectMaker(field), {address}))
            : builder_.CreateLoad(lower(field), address, order[i]);
    builder_.CreateStore(value, fieldAddress(object, type, order[i], /*exact=*/true));
  }
  adoptFields(object, type);
  builder_.CreateRet(object);
  function_ = outer;
  builder_.restoreIP(point);
  return maker;
}

llvm::Function *CodeGen::systemErrorMaker() {
  for (const ClassDecl *child : errorClass_->children)
    if (child->builtin && child->name == "SystemError")
      return objectMaker(child->type);
  llvm_unreachable("Sema declares SystemError");
}

llvm::Value *CodeGen::emitNodeBuiltin(CallExpr *call) {
  if (isFspBuiltin(call->builtin))
    return emitFsPromise(call);
  if (call->builtin >= Builtin::RlCreate && call->builtin <= Builtin::RlClose)
    return emitReadlineBuiltin(call);
  llvm::Type *voidTy = builder_.getVoidTy(), *i32 = builder_.getInt32Ty();
  llvm::Type *i64 = builder_.getInt64Ty(), *f64 = builder_.getDoubleTy(),
             *ptr = builder_.getPtrTy();

  if (call->builtin == Builtin::FsStatsDate) {
    // dateFromMs of Node: the milliseconds rounded, as Math.round does.
    auto *member = cast<MemberExpr>(call->callee);
    Operand stats = emitOperand(member->object, /*canBorrow=*/true);
    llvm::Value *ms = builder_.CreateLoad(
        f64, fieldAddress(stats.value, stats.type, (member->name + "Ms").str()), "ms");
    dropOperand(stats);
    llvm::Value *rounded = builder_.CreateUnaryIntrinsic(
        llvm::Intrinsic::floor, builder_.CreateFAdd(ms, llvm::ConstantFP::get(f64, 0.5)));
    return newDate(builder_.CreateCall(processFunction("tz_date_clip", f64, {f64}), {rounded}));
  }

  if (call->builtin == Builtin::FsIsKind) {
    // The kind of file is in the bits of the mode of a Stats, and in the
    // hidden field of a Dirent.
    auto *member = cast<MemberExpr>(call->callee);
    Operand object = emitOperand(member->object, /*canBorrow=*/true);
    const Type *type = object.type;
    bool stats = type->field("mode") != nullptr;
    llvm::StringRef field = stats ? "mode" : "#type";
    llvm::Value *bits =
        builder_.CreateLoad(stats ? f64 : i32, fieldAddress(object.value, type, field));
    if (stats)
      bits = builder_.CreateFPToSI(bits, i32);
    dropOperand(object);
    llvm::Value *kind = emitExpr(call->args[0]);
    return builder_.CreateICmpEQ(builder_.CreateAnd(bits, builder_.getInt32(0170000)), kind);
  }

  // The arguments, owned, released after the call: the runtime borrows them.
  std::vector<llvm::Value *> values = emitHeldValues(call->args);
  auto release = [&] {
    for (size_t i = 0; i < values.size(); ++i)
      releaseValue(values[i], call->args[i]->type);
  };
  auto obj = [&](size_t i) { return stringObj(values[i]); };
  auto meta = [&](size_t i) { return stringMeta(values[i]); };
  auto flag = [&](size_t i) { return builder_.CreateZExt(values[i], i32); };
  auto fn = [&](llvm::StringRef name, llvm::Type *ret, llvm::ArrayRef<llvm::Type *> params) {
    return processFunction(name, ret, params);
  };
  // A runtime function that may throw a SystemError, with its maker last.
  auto fsCall = [&](llvm::StringRef name, llvm::Type *ret, std::vector<llvm::Type *> params,
                    std::vector<llvm::Value *> args) -> llvm::Value * {
    params.push_back(ptr);
    args.push_back(systemErrorMaker());
    llvm::Value *result = builder_.CreateCall(fn(name, ret, params), args);
    release();
    emitThrowCheck();
    return result;
  };
  auto fsString = [&](llvm::StringRef name, std::vector<llvm::Type *> params,
                      std::vector<llvm::Value *> args) {
    llvm::AllocaInst *out = createStackSlot(strTy_, "fs.out");
    params.insert(params.begin(), ptr);
    args.insert(args.begin(), out);
    fsCall(name, voidTy, params, args);
    return builder_.CreateLoad(strTy_, out);
  };
  // A string or undefined, from a runtime function that says whether there
  // is one.
  auto optionalString = [&](llvm::Value *out, llvm::Value *found) {
    return makeOptional(builder_.CreateLoad(strTy_, out),
                        builder_.CreateICmpNE(found, builder_.getInt32(0)), call->type);
  };

  llvm::Value *result = nullptr;
  switch (call->builtin) {
  case Builtin::PathJoin:
  case Builtin::PathResolve:
    result = callReturningString(
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
    result =
        callReturningString(fn(name, voidTy, {ptr, ptr, i64, i32}), {obj(1), meta(1), values[0]});
    break;
  }
  case Builtin::PathIsAbsolute:
    result =
        builder_.CreateICmpNE(builder_.CreateCall(fn("tz_path_is_absolute", i32, {ptr, i64, i32}),
                                                  {obj(1), meta(1), values[0]}),
                              builder_.getInt32(0));
    break;
  case Builtin::PathBasename: {
    bool suffix = values.size() > 2;
    llvm::Value *null = llvm::ConstantPointerNull::get(builder_.getPtrTy());
    result = callReturningString(
        fn("tz_path_basename", voidTy, {ptr, ptr, i64, ptr, i64, i32, i32}),
        {obj(1), meta(1), suffix ? obj(2) : null, suffix ? meta(2) : builder_.getInt64(0),
         builder_.getInt32(suffix ? 1 : 0), values[0]});
    break;
  }
  case Builtin::PathRelative:
    result = callReturningString(fn("tz_path_relative", voidTy, {ptr, ptr, i64, ptr, i64, i32}),
                                 {obj(1), meta(1), obj(2), meta(2), values[0]});
    break;
  case Builtin::PathParse:
    result = builder_.CreateCall(fn("tz_path_parse", ptr, {ptr, i64, i32, ptr}),
                                 {obj(1), meta(1), values[0], objectMaker(call->type)});
    break;
  case Builtin::PathFormat: {
    // The five parts as `string | undefined`, from the fields the object has.
    const Type *object = call->args[1]->type;
    const Type *optional = Type::optional(Type::string());
    llvm::Type *part = lower(optional);
    auto *partsType = llvm::ArrayType::get(part, 5);
    llvm::AllocaInst *parts = createStackSlot(partsType, "format.parts");
    static const llvm::StringLiteral names[] = {"root", "dir", "base", "ext", "name"};
    for (unsigned i = 0; i < 5; ++i) {
      const Field *field = object->field(names[i]);
      llvm::Value *value;
      if (!field) {
        value = undefinedValue(optional);
      } else {
        value = builder_.CreateLoad(lower(field->type), fieldAddress(values[1], object, names[i]));
        if (!field->type->isOptional())
          value = makeOptional(value, builder_.getTrue(), optional);
      }
      builder_.CreateStore(value, builder_.CreateConstInBoundsGEP2_32(partsType, parts, 0, i));
    }
    result = callReturningString(fn("tz_path_format", voidTy, {ptr, ptr, i32}), {parts, values[0]});
    break;
  }
  case Builtin::OsString:
    result = callReturningString(fn("tz_os_string", voidTy, {ptr, i32}), {values[0]});
    break;
  case Builtin::OsNumber:
    result = builder_.CreateCall(fn("tz_os_number", f64, {i32}), {values[0]});
    break;
  case Builtin::OsAvailableParallelism:
    result = builder_.CreateCall(fn("tz_os_available_parallelism", i32, {}));
    break;
  case Builtin::OsCpus: {
    const Type *element = call->type->inner();
    result = builder_.CreateCall(fn("tz_os_cpus", ptr, {ptr, ptr}),
                                 {typeDescriptor(element), objectMaker(element)});
    break;
  }
  case Builtin::OsLoadavg:
    result =
        builder_.CreateCall(fn("tz_os_loadavg", ptr, {ptr}), {typeDescriptor(Type::doubleTy())});
    break;
  case Builtin::OsUserInfo:
    result = builder_.CreateCall(fn("tz_os_user_info", ptr, {ptr}), {objectMaker(call->type)});
    break;
  case Builtin::ReadLine: {
    bool question = !values.empty();
    llvm::AllocaInst *out = createStackSlot(strTy_, "line");
    llvm::Value *found = builder_.CreateCall(
        fn("tz_io_read_line", i32, {ptr, ptr, i64, i32}),
        {out, question ? obj(0) : llvm::ConstantPointerNull::get(builder_.getPtrTy()),
         question ? meta(0) : builder_.getInt64(0), builder_.getInt32(question ? 1 : 0)});
    release();
    return optionalString(out, found);
  }
  case Builtin::FsReadFile:
    // Without an encoding, a Buffer; with one, its number last.
    if (call->type->isBytes()) {
      if (call->args[0]->type->isInt())
        return fsCall("tz_fs_read_fd_bytes", ptr, {i32}, {values[0]});
      return fsCall("tz_fs_read_file_bytes", ptr, {ptr, i64, ptr, i64},
                    {obj(0), meta(0), obj(1), meta(1)});
    }
    if (call->args[0]->type->isInt())
      return fsString("tz_fs_read_fd", {i32, i32}, {values[0], values[2]});
    return fsString("tz_fs_read_file", {ptr, i64, ptr, i64, i32},
                    {obj(0), meta(0), obj(1), meta(1), values[2]});
  case Builtin::FsWriteFile:
    // The bytes of a view, or text in the encoding last.
    if (!call->args[1]->type->isString()) {
      if (call->args[0]->type->isInt())
        fsCall("tz_fs_write_fd_bytes", voidTy, {i32, ptr}, {values[0], values[1]});
      else
        fsCall("tz_fs_write_file_bytes", voidTy, {ptr, i64, ptr, ptr, i64, i32},
               {obj(0), meta(0), values[1], obj(2), meta(2), values[3]});
    } else if (call->args[0]->type->isInt()) {
      fsCall("tz_fs_write_fd", voidTy, {i32, ptr, i64, i32},
             {values[0], obj(1), meta(1), values[4]});
    } else {
      fsCall("tz_fs_write_file", voidTy, {ptr, i64, ptr, i64, ptr, i64, i32, i32},
             {obj(0), meta(0), obj(1), meta(1), obj(2), meta(2), values[3], values[4]});
    }
    return nullptr;
  case Builtin::FsExists:
    result = builder_.CreateICmpNE(
        builder_.CreateCall(fn("tz_fs_exists", i32, {ptr, i64}), {obj(0), meta(0)}),
        builder_.getInt32(0));
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
    llvm::Value *dirent = element->isClass() ? static_cast<llvm::Value *>(objectMaker(element))
                                             : llvm::ConstantPointerNull::get(builder_.getPtrTy());
    return fsCall("tz_fs_readdir", ptr, {ptr, i64, i32, ptr, ptr},
                  {obj(0), meta(0), flag(1), typeDescriptor(element), dirent});
  }
  case Builtin::FsMkdir: {
    llvm::AllocaInst *out = createStackSlot(strTy_, "mkdir.first");
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
