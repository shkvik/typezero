#include "codegen/codegen.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/ErrorHandling.h"

#include <array>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

namespace {

// String value layout; must match runtime/string.c.
constexpr uint64_t kStrLengthMask = (uint64_t(1) << 30) - 1;
constexpr uint64_t kStrAscii = uint64_t(1) << 60;
constexpr uint64_t kStrTwoByte = uint64_t(1) << 61;
constexpr uint64_t kStrRope = uint64_t(1) << 62;
constexpr uint64_t kStrHeap = uint64_t(1) << 63;
// A one-byte string of up to this many code units is stored in the value
// itself: `obj` holds the code units and `meta` has ROPE without HEAP.
constexpr size_t kInlineMax = 8;
// A number passed to tz_str_concat_n as it is (see runtime/string.c): the
// ROPE and TWO_BYTE flags without HEAP, and the kind of number in the low bits.
constexpr uint64_t kNumberPart = kStrRope | kStrTwoByte;
constexpr uint64_t kNumberInt = 1;
constexpr uint64_t kNumberDouble = 2;
constexpr uint32_t kKindBuffer1 = 1;
constexpr uint32_t kKindBuffer2 = 2;
// Literals are never counted (their values lack the HEAP flag); a huge count
// just makes them look alive to anyone inspecting memory.
constexpr int64_t kLiteralRefCount = int64_t(1) << 62;

// Collects the operands of a chain of string `+`. Concatenation is
// associative, so a + (b + c) flattens as well as (a + b) + c.
void collectConcat(Expr *expr, std::vector<Expr *> &parts) {
  auto *bin = dyn_cast<BinaryExpr>(expr);
  if (bin && bin->op == BinaryOp::Add && bin->type->isString()) {
    collectConcat(bin->lhs, parts);
    collectConcat(bin->rhs, parts);
  } else {
    parts.push_back(expr);
  }
}

// A value held as a pointer, null for undefined in `T | undefined`: an
// object, an iterator, a Json, a dictionary or a Promise.
bool isReference(const Type *type) {
  return type->isObject() || type->isIterator() || type->isJson() || type->isDict() ||
         type->isPromise() || type->isUnknown() || type->isClassRef() ||
         (type->isUnion() && !type->isTagged() && type->slots().front() == UnionSlot::Object);
}

} // namespace

CodeGen::CodeGen(llvm::LLVMContext &ctx, llvm::StringRef moduleName, const llvm::DataLayout &layout)
    : ctx_(ctx), module_(std::make_unique<llvm::Module>(moduleName, ctx)), builder_(ctx) {
  module_->setDataLayout(layout);
  llvm::Type *ptr = builder_.getPtrTy();
  llvm::Type *i32 = builder_.getInt32Ty();
  llvm::Type *i64 = builder_.getInt64Ty();
  strTy_ = llvm::StructType::create(ctx, {ptr, i64}, "tz.string");
  arrayTy_ = llvm::StructType::create(ctx, {i64, ptr, ptr, i32, i32, ptr}, "tz.array");
  typeDescTy_ = llvm::StructType::create(ctx, {i32, i32, ptr}, "tz.type");
  fnTy_ = llvm::StructType::create(ctx, {ptr, ptr}, "tz.fn");
  fnInfoTy_ = llvm::StructType::create(ctx, {ptr, strTy_}, "tz.fninfo");
  decTy_ = llvm::StructType::create(ctx, {i64, i64}, "tz.decimal");
  fieldDescTy_ = llvm::StructType::create(ctx, {ptr, i32, i32, ptr}, "tz.field");
  layoutDescTy_ = llvm::StructType::create(ctx, {ptr, ptr, i32, i32, ptr, i32, i32}, "tz.layout");
  promiseTy_ = llvm::StructType::create(ctx, {ptr, ptr, ptr, i32, i32}, "tz.promise");
  promiseObjTy_ = llvm::StructType::create(ctx, {i64, ptr, ptr, ptr, ptr, i32, i32}, "tz.Promise");
}

std::unique_ptr<llvm::Module> CodeGen::generate(SourceFile &file) {
  comparesFunctions_ = file.comparesFunctions;
  usesTls_ = file.usesTls;
  subtypes_ = file.subtypes;
  usesAsync_ = file.usesAsync;
  // Node runs a file with `import`, `export` or top-level `await` as an ES
  // module, inside a microtask, and any other as CommonJS, before the
  // microtasks; the order of what follows depends on it (runtime/async.c).
  esModule_ = file.topLevelAwait;
  for (const Module *module : file.modules)
    if (module->isEntry && (llvm::any_of(module->file.imports,
                                         [](const ImportDecl &i) { return !i.implicit; }) ||
                            !module->file.exports.empty()))
      esModule_ = true;
  declareRuntime();
  // Debug mode: a program compiled to check itself for leaks, as the tests
  // are, also checks what the compiler proved of its memory.
  debugMemory_ = getenv("TZ_CHECK_LEAKS") != nullptr;
  analyzeCycles(file); // the counting of members of groups depends on it
  defineRefCountHelpers();
  prepareDispatch(file);
  // The functions and globals of imported files are named after the file
  // too: "tz.geometry.circle.area".
  for (const Module *module : file.modules) {
    if (module->isEntry)
      continue;
    std::string prefix;
    llvm::StringRef name(module->name);
    name.consume_back(".ts");
    llvm::SmallVector<llvm::StringRef, 4> parts;
    name.split(parts, '/');
    for (llvm::StringRef part : parts)
      prefix += (part == ".." ? std::string("up") : part.str()) + ".";
    for (const Stmt *item : module->file.items)
      symbolPrefixes_[item] = prefix;
  }
  // A generic function is emitted as its instances, which are top-level
  // functions like the others.
  std::vector<FunctionDecl *> functions;
  std::vector<ClassDecl *> classes;
  for (Stmt *item : file.items) {
    if (auto *fn = dyn_cast<FunctionDecl>(item);
        fn && fn->typeParams.empty() && !file.unusedFunctions.contains(fn))
      functions.push_back(fn);
    if (auto *cls = dyn_cast<ClassDecl>(item)) {
      // A generic class is emitted as its instances, which share its static
      // methods.
      if (cls->isEmitted())
        classes.push_back(cls);
      // Static methods are plain functions; the others take `this`. A
      // generic method is emitted as its copies.
      if (!cls->generic)
        for (FunctionDecl *fn : cls->methods)
          if (fn->isStatic && fn->typeParams.empty())
            functions.push_back(fn);
    }
  }
  functions.insert(functions.end(), file.instances.begin(), file.instances.end());
  emittedClasses_.assign(classes.begin(), classes.end());
  for (FunctionDecl *fn : functions)
    declareFunction(fn);
  for (ClassDecl *cls : classes)
    declareMethods(cls);
  findTailGroups(functions);
  analyzeThrows(file);
  declareGlobals(file);
  for (FunctionDecl *fn : functions)
    emitFunction(fn);
  for (ClassDecl *cls : classes)
    emitMethods(cls);
  emitMain(file);
  // The closures each function creates come after it, and the iterators of
  // arrays and helpers it makes; each may need more of the other.
  while (!pendingClosures_.empty() || !pendingIterators_.empty()) {
    emitPendingClosures();
    emitPendingIterators();
  }
  finishReflection(file, mainInit_);
  finishMetadata(file, mainInit_);
  finishGroupDebug(mainInit_);
  return std::move(module_);
}

llvm::Type *CodeGen::lower(const Type *type) {
  switch (type->kind()) {
  case Type::Kind::Void: return builder_.getVoidTy();
  case Type::Kind::Undefined: return builder_.getInt1Ty(); // never read
  case Type::Kind::Bool: return builder_.getInt1Ty();
  case Type::Kind::Int: return builder_.getInt32Ty();
  case Type::Kind::Long: return builder_.getInt64Ty();
  case Type::Kind::Double: return builder_.getDoubleTy();
  case Type::Kind::Decimal: return decTy_;
  case Type::Kind::String: return strTy_;
  case Type::Kind::Array: return builder_.getPtrTy();
  case Type::Kind::Optional:
    if (zeroIsUndefined(type->inner())) // null (or zero) is undefined
      return lower(type->inner());
    return llvm::StructType::get(ctx_, {lower(type->inner()), builder_.getInt1Ty()});
  case Type::Kind::Literal:
  case Type::Kind::Union: return lowerUnion(type);
  case Type::Kind::Function: return fnTy_;
  case Type::Kind::Object: return builder_.getPtrTy();
  case Type::Kind::Result: return resultLayout(type).type;
  case Type::Kind::Never: return llvm::StructType::get(ctx_); // never holds anything
  case Type::Kind::Iterator: return builder_.getPtrTy();
  case Type::Kind::Json: return builder_.getPtrTy();
  case Type::Kind::Dict: return builder_.getPtrTy();
  case Type::Kind::Promise: return builder_.getPtrTy();
  case Type::Kind::Unknown: return builder_.getPtrTy(); // null for undefined
  case Type::Kind::ClassRef: return builder_.getPtrTy();
  case Type::Kind::Error: break;
  }
  llvm_unreachable("an erroneous type reached code generation");
}

//===----------------------------------------------------------------------===//
// Module structure
//===----------------------------------------------------------------------===//

void CodeGen::declareRuntime() {
  llvm::Type *voidTy = builder_.getVoidTy();
  llvm::Type *i32 = builder_.getInt32Ty();
  llvm::Type *i64 = builder_.getInt64Ty();
  llvm::Type *f64 = builder_.getDoubleTy();
  llvm::Type *ptr = builder_.getPtrTy();

  auto declare = [&](llvm::StringRef name, llvm::Type *ret, llvm::ArrayRef<llvm::Type *> params) {
    llvm::FunctionCallee callee =
        module_->getOrInsertFunction(name, llvm::FunctionType::get(ret, params, false));
    cast<llvm::Function>(callee.getCallee())->addFnAttr(llvm::Attribute::NoUnwind);
    return callee;
  };
  rt_.init = declare("tz_rt_init", voidTy, {});
  rt_.shutdown = declare("tz_rt_shutdown", voidTy, {});
  rt_.mainDone = declare("tz_rt_main_done", voidTy, {});
  rt_.printI32 = declare("tz_print_i32", voidTy, {i32});
  rt_.printI64 = declare("tz_print_i64", voidTy, {i64});
  rt_.printF64 = declare("tz_print_f64", voidTy, {f64});
  rt_.printBool = declare("tz_print_bool", voidTy, {i32});
  rt_.printChar = declare("tz_print_char", voidTy, {i32});
  rt_.printString = declare("tz_print_string", voidTy, {ptr, i64});
  rt_.printValue = declare("tz_print_value", voidTy, {ptr, ptr});
  rt_.debugger = declare("tz_debugger", voidTy, {});
  // Numbers (runtime/tzrt.c, number.c and math.c).
  rt_.toInt32 = declare("tz_to_int32", i32, {f64});
  rt_.mathRound = declare("tz_math_round", f64, {f64});
  rt_.mathPow = declare("tz_math_pow", f64, {f64, f64});
  rt_.mathPowLong = declare("tz_math_pow_long", i64, {i64, i64});
  rt_.mathHypot = declare("tz_math_hypot", f64, {ptr, i64});
  rt_.mathRandom = declare("tz_math_random", f64, {});
  rt_.checkedInt32OfLong = declare("tz_checked_int32_of_long", i32, {i64});
  rt_.strToNumber = declare("tz_str_to_number", f64, {ptr, i64});
  rt_.parseInt = declare("tz_parse_int", f64, {ptr, i64, i32});
  rt_.parseFloat = declare("tz_parse_float", f64, {ptr, i64});
  rt_.numToFixed = declare("tz_num_to_fixed", voidTy, {ptr, f64, i64});
  rt_.numToRadix = declare("tz_num_to_radix", voidTy, {ptr, f64, i64});

  // String functions take strings as (obj, meta) pairs and return them
  // through a pointer passed first.
  rt_.strFree = declare("tz_str_free", voidTy, {ptr});
  rt_.strConcatN = declare("tz_str_concat_n", voidTy, {ptr, ptr, i64});
  rt_.strEqual = declare("tz_str_equal", i32, {ptr, i64, ptr, i64});
  rt_.strCompare = declare("tz_str_compare", i32, {ptr, i64, ptr, i64});
  rt_.strCharAt = declare("tz_str_char_at", voidTy, {ptr, ptr, i64, i64});
  rt_.strCharCodeAt = declare("tz_str_char_code_at", i32, {ptr, i64, i64});
  rt_.strCodePointAt = declare("tz_str_code_point_at", i32, {ptr, ptr, i64, i64});
  rt_.strSlice = declare("tz_str_slice", voidTy, {ptr, ptr, i64, i64, i64, i32});
  rt_.strSubstring = declare("tz_str_substring", voidTy, {ptr, ptr, i64, i64, i64, i32});
  rt_.strIndexOf = declare("tz_str_index_of", i32, {ptr, i64, ptr, i64, i64, i32});
  rt_.strIncludes = declare("tz_str_includes", i32, {ptr, i64, ptr, i64, i64, i32});
  rt_.strStartsWith = declare("tz_str_starts_with", i32, {ptr, i64, ptr, i64, i64, i32});
  rt_.strEndsWith = declare("tz_str_ends_with", i32, {ptr, i64, ptr, i64, i64, i32});
  rt_.strRepeat = declare("tz_str_repeat", voidTy, {ptr, ptr, i64, i64});
  rt_.strFromI64 = declare("tz_str_from_i64", voidTy, {ptr, i64});
  rt_.strFromF64 = declare("tz_str_from_f64", voidTy, {ptr, f64});
  rt_.valueToString = declare("tz_value_to_string", voidTy, {ptr, ptr, ptr});
  rt_.strCharAtOrEmpty = declare("tz_str_char_at_or_empty", voidTy, {ptr, ptr, i64, i64});
  rt_.strCodePoint = declare("tz_str_code_point", i32, {ptr, i64, i64});
  rt_.strLastIndexOf = declare("tz_str_last_index_of", i32, {ptr, i64, ptr, i64, i64, i32});
  rt_.strSplit = declare("tz_str_split", ptr, {ptr, ptr, i64, ptr, i64, i32, i64, i32});
  rt_.strTrim = declare("tz_str_trim", voidTy, {ptr, ptr, i64, i32});
  rt_.strPad = declare("tz_str_pad", voidTy, {ptr, ptr, i64, i64, ptr, i64, i32, i32});
  rt_.strToUpper = declare("tz_str_to_upper", voidTy, {ptr, ptr, i64});
  rt_.strToLower = declare("tz_str_to_lower", voidTy, {ptr, ptr, i64});
  rt_.strReplace = declare("tz_str_replace", voidTy, {ptr, ptr, i64, ptr, i64, ptr, i64, i32});
  rt_.strFromCharCodes = declare("tz_str_from_char_codes", voidTy, {ptr, ptr, i64});
  rt_.strFromCodePoints = declare("tz_str_from_code_points", voidTy, {ptr, ptr, i64});

  // Arrays (runtime/array.c). Values of any type go by address, with the
  // type descriptor of the elements stored in the array.
  rt_.arrNew = declare("tz_arr_new", ptr, {ptr, i64, i64});
  rt_.arrNewLength = declare("tz_arr_new_length", ptr, {ptr, i64});
  rt_.arrFree = declare("tz_arr_free", voidTy, {ptr});
  rt_.arrGrow = declare("tz_arr_grow", voidTy, {ptr, i64});
  rt_.arrShift = declare("tz_arr_shift", i32, {ptr, ptr});
  rt_.arrUnshiftSlots = declare("tz_arr_unshift_slots", ptr, {ptr, i64});
  rt_.arrSetLength = declare("tz_arr_set_length", voidTy, {ptr, i64});
  rt_.arrSlice = declare("tz_arr_slice", ptr, {ptr, i64, i64, i32});
  rt_.arrCopy = declare("tz_arr_copy", ptr, {ptr});
  rt_.arrAppend = declare("tz_arr_append", voidTy, {ptr, ptr});
  rt_.arrAppendCodePoints = declare("tz_arr_append_code_points", voidTy, {ptr, ptr, i64});
  rt_.arrIndexOf = declare("tz_arr_index_of", i32, {ptr, ptr, i64, i32, i32});
  rt_.arrJoin = declare("tz_arr_join", voidTy, {ptr, ptr, ptr, i64, i32});
  rt_.arrReverse = declare("tz_arr_reverse", voidTy, {ptr});
  rt_.arrFill = declare("tz_arr_fill", voidTy, {ptr, ptr, i64, i64, i32, i32});
  rt_.arrSort = declare("tz_arr_sort", voidTy, {ptr});
  rt_.arrSortWith = declare("tz_arr_sort_with", voidTy, {ptr, ptr, ptr});
  rt_.arrSplice = declare("tz_arr_splice", ptr, {ptr, i64, i64, i32, i64, ptr});
  // The objects of closures (runtime/closure.c).
  rt_.objAlloc = declare("tz_obj_alloc", ptr, {i64});
  rt_.objFree = declare("tz_obj_free", voidTy, {ptr});
  // Object.keys and the like (runtime/object.c).
  rt_.objKeys = declare("tz_obj_keys", ptr, {ptr, ptr});
  rt_.objValues = declare("tz_obj_values", ptr, {ptr, ptr});
  rt_.objEntries = declare("tz_obj_entries", ptr, {ptr, ptr, ptr, i64});

  // Functions that return memory nothing else points to yet.
  for (llvm::FunctionCallee fresh : {rt_.arrNew, rt_.arrNewLength, rt_.arrSlice, rt_.arrCopy,
                                     rt_.objAlloc, rt_.objKeys, rt_.objValues, rt_.objEntries})
    cast<llvm::Function>(fresh.getCallee())->addRetAttr(llvm::Attribute::NoAlias);

  cast<llvm::Function>(rt_.arrGrow.getCallee())->addFnAttr(llvm::Attribute::Cold);
}

// The inline fast paths of reference counting:
// - tz.str.retain / tz.str.release: only heap strings, whose meta has the sign
//   bit set, are counted;
// - tz.arr.retain / tz.arr.release: every array is counted; null (the zero
//   value of a slot) is skipped. tz.arr.retain serves objects too, whose
//   count is the first word as well (see codegen_object.cpp for releasing
//   them).
void CodeGen::defineRefCountHelpers() {
  for (bool isString : {true, false}) {
    llvm::Type *param = isString ? static_cast<llvm::Type *>(strTy_) : builder_.getPtrTy();
    auto *type = llvm::FunctionType::get(builder_.getVoidTy(), {param}, false);
    for (bool isRetain : {true, false}) {
      std::string name = std::string(isString ? "tz.str." : "tz.arr.") + (isRetain ? "retain" : "release");
      auto *fn = llvm::Function::Create(type, llvm::GlobalValue::InternalLinkage, name, module_.get());
      fn->addFnAttr(llvm::Attribute::AlwaysInline);
      fn->addFnAttr(llvm::Attribute::NoUnwind);

      llvm::IRBuilder<> b(llvm::BasicBlock::Create(ctx_, "entry", fn));
      auto *countedBlock = llvm::BasicBlock::Create(ctx_, "counted", fn);
      auto *doneBlock = llvm::BasicBlock::Create(ctx_, "done", fn);
      llvm::Value *obj, *counted;
      if (isString) {
        llvm::Value *str = fn->getArg(0);
        obj = b.CreateExtractValue(str, 0, "obj");
        counted = b.CreateICmpSLT(b.CreateExtractValue(str, 1, "meta"), b.getInt64(0));
      } else {
        obj = fn->getArg(0);
        counted = b.CreateIsNotNull(obj);
      }
      b.CreateCondBr(counted, countedBlock, doneBlock);

      b.SetInsertPoint(countedBlock);
      llvm::Value *count = b.CreateLoad(b.getInt64Ty(), obj, "rc");
      if (isRetain) {
        b.CreateStore(b.CreateAdd(count, b.getInt64(1)), obj);
        b.CreateBr(doneBlock);
      } else {
        llvm::Value *left = b.CreateSub(count, b.getInt64(1));
        b.CreateStore(left, obj);
        auto *freeBlock = llvm::BasicBlock::Create(ctx_, "free", fn);
        b.CreateCondBr(b.CreateICmpEQ(left, b.getInt64(0)), freeBlock, doneBlock);
        b.SetInsertPoint(freeBlock);
        b.CreateCall(isString ? rt_.strFree : rt_.arrFree, {obj});
        b.CreateBr(doneBlock);
      }

      b.SetInsertPoint(doneBlock);
      b.CreateRetVoid();
      if (isString)
        (isRetain ? retainFn_ : releaseFn_) = fn;
      else
        (isRetain ? arrRetainFn_ : arrReleaseFn_) = fn;
    }
  }

  // tz.obj.retain / tz.obj.release: the objects of closures, which start with
  // { i64 rc, ptr descriptor }, where the descriptor starts with the drop
  // function; the last release calls it. Objects of the language start the
  // same way, so a function value's environment may be one (a bound method).
  auto *objHeader = llvm::StructType::get(ctx_, {builder_.getInt64Ty(), builder_.getPtrTy()});
  auto *type = llvm::FunctionType::get(builder_.getVoidTy(), {builder_.getPtrTy()}, false);
  for (bool isRetain : {true, false}) {
    auto *fn = llvm::Function::Create(type, llvm::GlobalValue::InternalLinkage,
                                      isRetain ? "tz.obj.retain" : "tz.obj.release", module_.get());
    fn->addFnAttr(llvm::Attribute::AlwaysInline);
    fn->addFnAttr(llvm::Attribute::NoUnwind);
    llvm::IRBuilder<> b(llvm::BasicBlock::Create(ctx_, "entry", fn));
    auto *countedBlock = llvm::BasicBlock::Create(ctx_, "counted", fn);
    auto *doneBlock = llvm::BasicBlock::Create(ctx_, "done", fn);
    llvm::Value *obj = fn->getArg(0);
    b.CreateCondBr(b.CreateIsNotNull(obj), countedBlock, doneBlock);
    b.SetInsertPoint(countedBlock);
    llvm::Value *count = b.CreateLoad(b.getInt64Ty(), obj, "rc");
    if (isRetain) {
      b.CreateStore(b.CreateAdd(count, b.getInt64(1)), obj);
      b.CreateBr(doneBlock);
    } else {
      llvm::Value *left = b.CreateSub(count, b.getInt64(1));
      b.CreateStore(left, obj);
      auto *dropBlock = llvm::BasicBlock::Create(ctx_, "drop", fn);
      b.CreateCondBr(b.CreateICmpEQ(left, b.getInt64(0)), dropBlock, doneBlock);
      b.SetInsertPoint(dropBlock);
      llvm::Value *descriptor =
          b.CreateLoad(b.getPtrTy(), b.CreateStructGEP(objHeader, obj, 1), "descriptor");
      llvm::Value *drop = b.CreateLoad(b.getPtrTy(), descriptor, "drop");
      b.CreateCall(type, drop, {obj});
      b.CreateBr(doneBlock);
    }
    b.SetInsertPoint(doneBlock);
    b.CreateRetVoid();
    (isRetain ? objRetainFn_ : objReleaseFn_) = fn;
  }
  plainObjRetainFn_ = objRetainFn_;
  plainObjReleaseFn_ = objReleaseFn_;

  // tz.ref.retain: a reference that may point at a member of a group, whose
  // count is negative (see runtime/group.c): its group's count goes up.
  refRetainFn_ = llvm::Function::Create(type, llvm::GlobalValue::InternalLinkage, "tz.ref.retain",
                                        module_.get());
  refRetainFn_->addFnAttr(llvm::Attribute::AlwaysInline);
  refRetainFn_->addFnAttr(llvm::Attribute::NoUnwind);
  {
    llvm::IRBuilder<> b(llvm::BasicBlock::Create(ctx_, "entry", refRetainFn_));
    auto *countedBlock = llvm::BasicBlock::Create(ctx_, "counted", refRetainFn_);
    auto *memberBlock = llvm::BasicBlock::Create(ctx_, "member", refRetainFn_);
    auto *plainBlock = llvm::BasicBlock::Create(ctx_, "plain", refRetainFn_);
    auto *doneBlock = llvm::BasicBlock::Create(ctx_, "done", refRetainFn_);
    llvm::Value *obj = refRetainFn_->getArg(0);
    b.CreateCondBr(b.CreateIsNotNull(obj), countedBlock, doneBlock);
    b.SetInsertPoint(countedBlock);
    llvm::Value *count = b.CreateLoad(b.getInt64Ty(), obj, "rc");
    b.CreateCondBr(b.CreateICmpSLT(count, b.getInt64(0)), memberBlock, plainBlock);
    b.SetInsertPoint(memberBlock);
    b.CreateCall(groupFunction("tz_group_retain", b.getVoidTy(), {b.getPtrTy()}), {obj});
    b.CreateBr(doneBlock);
    b.SetInsertPoint(plainBlock);
    b.CreateStore(b.CreateAdd(count, b.getInt64(1)), obj);
    b.CreateBr(doneBlock);
    b.SetInsertPoint(doneBlock);
    b.CreateRetVoid();
  }
  // Environments, boxes and objects that code counts without knowing their
  // types may be members when the program has any.
  if (hasCycles_) {
    objRetainFn_ = refRetainFn_;
    objReleaseFn_ = groupAwareRelease(plainObjReleaseFn_);
  }

  // tz.instance.release: an object of the language whose type the code does
  // not know (Type::anyObject); its header { i64 rc, ptr layout } leads to
  // the drop function, the first word of the layout.
  instanceReleaseFn_ = llvm::Function::Create(type, llvm::GlobalValue::InternalLinkage, "tz.instance.release",
                                              module_.get());
  instanceReleaseFn_->addFnAttr(llvm::Attribute::AlwaysInline);
  instanceReleaseFn_->addFnAttr(llvm::Attribute::NoUnwind);
  llvm::IRBuilder<> b(llvm::BasicBlock::Create(ctx_, "entry", instanceReleaseFn_));
  auto *countedBlock = llvm::BasicBlock::Create(ctx_, "counted", instanceReleaseFn_);
  auto *dropBlock = llvm::BasicBlock::Create(ctx_, "drop", instanceReleaseFn_);
  auto *doneBlock = llvm::BasicBlock::Create(ctx_, "done", instanceReleaseFn_);
  llvm::Value *obj = instanceReleaseFn_->getArg(0);
  b.CreateCondBr(b.CreateIsNotNull(obj), countedBlock, doneBlock);
  b.SetInsertPoint(countedBlock);
  llvm::Value *left = b.CreateSub(b.CreateLoad(b.getInt64Ty(), obj, "rc"), b.getInt64(1));
  b.CreateStore(left, obj);
  b.CreateCondBr(b.CreateICmpEQ(left, b.getInt64(0)), dropBlock, doneBlock);
  b.SetInsertPoint(dropBlock);
  llvm::Value *layout = b.CreateLoad(b.getPtrTy(), b.CreateStructGEP(objHeader, obj, 1), "layout");
  llvm::Value *drop = b.CreateLoad(b.getPtrTy(), layout, "drop");
  b.CreateCall(type, drop, {obj});
  b.CreateBr(doneBlock);
  b.SetInsertPoint(doneBlock);
  b.CreateRetVoid();
  plainInstanceReleaseFn_ = instanceReleaseFn_;
  if (hasCycles_)
    instanceReleaseFn_ = groupAwareRelease(plainInstanceReleaseFn_);
}

void CodeGen::setFunctionAttributes(llvm::Function *fn) {
  // Nothing unwinds yet, but unwind tables still let debuggers and crash
  // handlers walk the stack.
  fn->addFnAttr(llvm::Attribute::NoUnwind);
  fn->setUWTableKind(llvm::UWTableKind::Default);
}

void CodeGen::declareFunction(FunctionDecl *fn) {
  const FuncSymbol *sym = fn->sym;
  std::vector<llvm::Type *> params;
  for (const Type *param : sym->params)
    params.push_back(lower(param));
  auto *type = llvm::FunctionType::get(lower(sym->ret), params, false);
  // The whole program is one module, so everything but `main` is internal and
  // LLVM is free to inline or drop it. An instance of a generic function is
  // named with its type arguments, as Sema made them (see
  // Sema::representation): `tz.first<int>`, `tz.first<object>`.
  std::string name = "tz." + symbolPrefixes_.lookup(fn) + fn->name.str();
  if (fn->cls) // a static method
    name = "tz." + symbolPrefixes_.lookup(fn->cls) + fn->cls->name.str() + "." + fn->name.str() +
           (fn->variant ? "." + std::to_string(fn->variant) : "");
  if (fn->isInstance) {
    name += "<";
    for (size_t i = 0; i < fn->typeArgs.size(); ++i) {
      const Type *arg = fn->typeArgs[i];
      name += (i > 0 ? ", " : "") + (arg->isFunction() ? std::string("function") : arg->name().str());
    }
    name += ">";
  }
  auto *function = llvm::Function::Create(type, llvm::GlobalValue::InternalLinkage, name, module_.get());
  setFunctionAttributes(function);
  functions_[sym] = function;
}

void CodeGen::declareGlobals(SourceFile &file) {
  for (Stmt *item : file.items) {
    if (auto *cls = dyn_cast<ClassDecl>(item)) {
      for (const ClassField &f : cls->fields) {
        if (!f.isStatic || !f.var)
          continue;
        // In the object of a class used as a value, which shows them.
        llvm::Type *type = lower(f.var->type);
        globals_[f.var] = cls->usedAsValue
                              ? staticFieldAddress(cls, f.var)
                              : new llvm::GlobalVariable(*module_, type, /*isConstant=*/false,
                                                         llvm::GlobalValue::InternalLinkage,
                                                         llvm::Constant::getNullValue(type),
                                                         "tz." + symbolPrefixes_.lookup(cls) +
                                                             f.var->name);
        if (f.var->type->isManaged())
          managedGlobals_.push_back(f.var);
      }
      // What the methods that decorators may replace run: their own code
      // until the decorators run, and also before, as in JavaScript.
      for (FunctionDecl *fn : cls->methods)
        if (const VarSymbol *impl = fn->impl) {
          llvm::Constant *adapter = cast<llvm::Constant>(makeFunctionValue(
              implAdapterInfo(fn), llvm::ConstantPointerNull::get(builder_.getPtrTy())));
          globals_[impl] = new llvm::GlobalVariable(
              *module_, lower(impl->type), /*isConstant=*/false, llvm::GlobalValue::InternalLinkage,
              adapter,
              "tz." + symbolPrefixes_.lookup(cls) + cls->name.str() + "." + fn->name.str() + ".impl");
          managedGlobals_.push_back(impl);
        }
      continue;
    }
    auto *decl = dyn_cast<VarDeclStmt>(item);
    if (!decl)
      continue;
    for (VarDeclarator &d : decl->decls) {
      if (d.alias || !d.sym->usedInFunction)
        continue; // another name for a generic function, or a local of main
      llvm::Type *type = lower(d.sym->type);
      // Holds zero (or the empty string) until the top-level code that
      // declares it runs.
      globals_[d.sym] = new llvm::GlobalVariable(*module_, type, /*isConstant=*/false,
                                                 llvm::GlobalValue::InternalLinkage,
                                                 llvm::Constant::getNullValue(type),
                                                 "tz." + symbolPrefixes_.lookup(decl) + d.name);
      if (d.sym->type->isManaged())
        managedGlobals_.push_back(d.sym);
    }
  }
}

void CodeGen::beginFunction(llvm::Function *function, FunctionDecl *fn) {
  function_ = function;
  currentFn_ = fn;
  returnType_ = fn ? fn->sym->ret : nullptr;
  envParam_ = nullptr;
  unwindExit_ = nullptr;
  addresses_.clear();
  boxSlots_.clear();
  envBoxes_.clear();
  groupEnvs_.clear();
  builder_.SetInsertPoint(llvm::BasicBlock::Create(ctx_, "entry", function_));
}

void CodeGen::emitFunction(FunctionDecl *fn) {
  if (fn->external) {
    emitExternal(fn);
    return;
  }
  if (fn->impl) { // a static method a decorator may replace
    emitDecoratedMethod(fn);
    return;
  }
  if (fn->isGenerator) {
    emitGenerator(fn, functions_.lookup(fn->sym));
    return;
  }
  if (fn->isAsync) {
    emitAsyncFunction(fn, functions_.lookup(fn->sym));
    return;
  }
  if (TailGroup *group = tailGroupOf_.lookup(fn)) {
    emitTailGroup(*group);
    return;
  }
  beginFunction(functions_.lookup(fn->sym), fn);
  std::vector<llvm::Value *> args;
  for (llvm::Argument &arg : function_->args())
    args.push_back(&arg);
  emitParamsAndBody(fn, args);
  finishFunction();
}

// The parameters, from the arguments given, and the body.
void CodeGen::emitParamsAndBody(FunctionDecl *fn, llvm::ArrayRef<llvm::Value *> args) {
  pushScope(); // parameters: the callee owns its arguments
  emitParams(fn, args);
  // An owner its environment refers to without counting (layer 2 of
  // docs/objects.md, section 11) stays while the closure runs, even if what
  // it runs lets the owner go.
  if (fn && fn->isClosure() && !fn->group)
    for (const VarSymbol *var : fn->captures)
      if (uncounted_.count({fn, var})) {
        const Type *type = typeOf(var);
        llvm::Value *owner = builder_.CreateLoad(lower(type), addressOf(var), var->name);
        retainValue(owner, type);
        builder_.CreateStore(owner, createManagedSlot(type, var->name + ".held"));
      }
  // A function that may recurse checks the stack first (docs/errors.md).
  if (recursive_.contains(fn))
    emitStackCheck(fn->loc());
  emitStmt(fn->body);
  popScope();
}

void CodeGen::emitParams(FunctionDecl *fn, llvm::ArrayRef<llvm::Value *> args) {
  if (fn->thisVar) {
    // A method's object, the environment it gets: borrowed, as the caller
    // keeps it alive during the call.
    llvm::Value *slot = createStackSlot(builder_.getPtrTy(), "this");
    builder_.CreateStore(envParam_, slot);
    addresses_[fn->thisVar] = slot;
  }
  for (size_t i = 0; i < fn->params.size(); ++i) {
    const Param &param = fn->params[i];
    llvm::Value *arg = args[i];
    if (isa<llvm::Argument>(arg))
      arg->setName(param.name);
    // Parameters can be assigned to, so they live in stack slots like locals.
    llvm::Value *slot = createLocal(param.sym, param.name);
    if (!param.defaultValue) {
      storeVar(param.sym, slot, arg, /*replacing=*/false);
      continue;
    }
    // The argument is `T | undefined`; undefined takes the default value.
    const Type *passed = Type::optional(param.sym->type);
    llvm::BasicBlock *givenBlock = newBlock("param.given");
    llvm::BasicBlock *defaultBlock = newBlock("param.default");
    llvm::BasicBlock *doneBlock = newBlock("param.done");
    builder_.CreateCondBr(optionalHas(arg, passed), givenBlock, defaultBlock);
    switchTo(givenBlock);
    llvm::Value *given = param.sym->type->isOptional() ? arg : optionalValue(arg, passed);
    builder_.CreateBr(doneBlock);
    switchTo(defaultBlock);
    llvm::Value *fallback = emitExpr(param.defaultValue);
    llvm::BasicBlock *defaultEnd = builder_.GetInsertBlock();
    builder_.CreateBr(doneBlock);
    switchTo(doneBlock);
    llvm::PHINode *value = builder_.CreatePHI(lower(param.sym->type), 2);
    value->addIncoming(given, givenBlock);
    value->addIncoming(fallback, defaultEnd);
    storeVar(param.sym, slot, value, /*replacing=*/false);
  }
}

void CodeGen::emitMain(SourceFile &file) {
  // Top-level code that awaits is a coroutine of its own (codegen_async.cpp).
  llvm::Function *asyncMain = file.topLevelAwait ? emitAsyncMain(file) : nullptr;
  auto *type = llvm::FunctionType::get(builder_.getInt32Ty(), false);
  auto *main = llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage, "main", module_.get());
  setFunctionAttributes(main);
  beginFunction(main, nullptr);
  mainInit_ = builder_.CreateCall(rt_.init);
  emitErrorSupport();
  pushScope();
  llvm::Type *voidTy = builder_.getVoidTy(), *ptr = builder_.getPtrTy();
  if (asyncMain) {
    // The runtime runs the loop until nothing is left to do.
    llvm::Value *promise = builder_.CreateCall(asyncMain, {}, "main");
    builder_.CreateCall(asyncFunction("tz_async_main", voidTy, {ptr}), {promise});
    builder_.CreateCall(instanceReleaseFn_, {promise});
  } else {
    for (Stmt *item : file.items)
      if (!isa<FunctionDecl>(item))
        emitStmt(item);
    // Then the timers, the promises and the queues the code started.
    if (usesAsync_ && blockIsOpen())
      builder_.CreateCall(asyncFunction("tz_async_run", voidTy, {builder_.getInt32Ty()}),
                          {builder_.getInt32(esModule_)});
  }
  // Exits right away unless a leak check wants everything released.
  if (blockIsOpen())
    builder_.CreateCall(rt_.mainDone);
  popScope();
  if (blockIsOpen()) {
    // Release the globals too, so that a leak check finds every count at zero.
    for (const VarSymbol *var : llvm::reverse(managedGlobals_)) {
      llvm::Type *lowered = lower(var->type);
      llvm::Value *address = addressOf(var);
      llvm::Value *value = builder_.CreateLoad(lowered, address);
      builder_.CreateStore(llvm::Constant::getNullValue(lowered), address);
      if (isEternal(var))
        endValue(value, var->type); // its eternal groups go whatever their counts
      else
        releaseValue(value, var->type);
    }
    if (usesAsync_)
      builder_.CreateCall(asyncFunction("tz_async_shutdown", voidTy, {}));
    builder_.CreateCall(rt_.shutdown);
  }
  finishFunction();
}

void CodeGen::finishFunction() {
  closeBody();
  finishFunctionUnwind();
  function_ = nullptr;
}

// Control that reaches the end of a body returns.
void CodeGen::closeBody() {
  if (blockIsOpen()) {
    llvm::Type *ret = function_->getReturnType();
    if (ret->isVoidTy())
      builder_.CreateRetVoid();
    else if (function_->hasExternalLinkage()) // main
      builder_.CreateRet(builder_.getInt32(0));
    else if (returnType_ && (returnType_->isOptional() || returnType_->isUnknown())) // undefined
      builder_.CreateRet(undefinedValue(returnType_));
    else // Sema proved that every path returns
      builder_.CreateUnreachable();
  }
}

//===----------------------------------------------------------------------===//
// Blocks and scopes
//===----------------------------------------------------------------------===//

llvm::BasicBlock *CodeGen::newBlock(const llvm::Twine &name) {
  return llvm::BasicBlock::Create(ctx_, name);
}

void CodeGen::switchTo(llvm::BasicBlock *block) {
  function_->insert(function_->end(), block);
  builder_.SetInsertPoint(block);
}

void CodeGen::emitBlock(llvm::BasicBlock *block) {
  branchTo(block);
  switchTo(block);
}

bool CodeGen::blockIsOpen() { return !builder_.GetInsertBlock()->hasTerminator(); }

void CodeGen::branchTo(llvm::BasicBlock *block) {
  if (blockIsOpen())
    builder_.CreateBr(block);
}

void CodeGen::startDeadBlock() { switchTo(newBlock("dead")); }

void CodeGen::pushScope() { scopes_.emplace_back(); }

void CodeGen::popScope() {
  if (blockIsOpen())
    emitScopeCleanup(scopes_.back());
  finishScopeUnwind(scopes_.size() - 1);
  scopes_.pop_back();
}

void CodeGen::emitCleanups(size_t depth) {
  for (size_t i = scopes_.size(); i > depth; --i)
    emitScopeCleanup(scopes_[i - 1]);
}

void CodeGen::emitScopeCleanup(const Scope &scope) {
  // The iterations of dictionaries end, before the slots let them go.
  for (llvm::Value *slot : llvm::reverse(scope.dicts)) {
    llvm::Value *map = builder_.CreateLoad(builder_.getPtrTy(), slot, "map");
    llvm::BasicBlock *endBlock = newBlock("dict.iterated");
    llvm::BasicBlock *nextBlock = newBlock("dict.iteration.end");
    builder_.CreateCondBr(builder_.CreateIsNotNull(map), endBlock, nextBlock);
    switchTo(endBlock);
    llvm::Value *count = dictField(map, 8); // TzMap::iterating
    builder_.CreateStore(
        builder_.CreateSub(builder_.CreateLoad(builder_.getInt32Ty(), count), builder_.getInt32(1)),
        count);
    builder_.CreateBr(nextBlock);
    switchTo(nextBlock);
  }
  for (llvm::Value *slot : llvm::reverse(scope.asyncIterators))
    closeAsyncIterator(slot);
  for (llvm::Value *slot : llvm::reverse(scope.iterators)) {
    llvm::Value *it = builder_.CreateLoad(builder_.getPtrTy(), slot, "iterator");
    llvm::BasicBlock *closeBlock = newBlock("iter.close");
    llvm::BasicBlock *nextBlock = newBlock("iter.closed");
    builder_.CreateCondBr(builder_.CreateIsNotNull(it), closeBlock, nextBlock);
    switchTo(closeBlock);
    builder_.CreateCall(iterCloseFunction(), {it});
    builder_.CreateBr(nextBlock);
    switchTo(nextBlock);
  }
  for (const auto &[slot, type] : llvm::reverse(scope.slots)) {
    if (!type) { // a box or an environment
      llvm::Value *object = builder_.CreateLoad(builder_.getPtrTy(), slot);
      builder_.CreateStore(llvm::ConstantPointerNull::get(builder_.getPtrTy()), slot);
      builder_.CreateCall(objReleaseFn_, {object});
      continue;
    }
    llvm::Type *lowered = lower(type);
    llvm::Value *value = builder_.CreateLoad(lowered, slot);
    // Emptied, so that a second cleanup of the same slot (a loop that leaves
    // early on the next iteration) does nothing.
    builder_.CreateStore(llvm::Constant::getNullValue(lowered), slot);
    if (eternalSlots_.contains(slot)) // a global of main's, at the end
      endValue(value, type);
    else
      releaseValue(value, type);
  }
}

llvm::AllocaInst *CodeGen::createStackSlot(llvm::Type *type, const llvm::Twine &name) {
  llvm::BasicBlock &entry = function_->getEntryBlock();
  llvm::IRBuilder<> entryBuilder(&entry, entry.begin());
  return entryBuilder.CreateAlloca(type, nullptr, name);
}

llvm::Value *CodeGen::createManagedSlot(const Type *type, const llvm::Twine &name) {
  llvm::Type *lowered = lower(type);
  llvm::AllocaInst *slot = createStackSlot(lowered, name);
  // Starts empty, so that releasing it before it was assigned is harmless.
  llvm::IRBuilder<> init(slot->getParent(), std::next(slot->getIterator()));
  init.CreateStore(llvm::Constant::getNullValue(lowered), slot);
  scopes_.back().slots.push_back({slot, type});
  return slot;
}

llvm::Value *CodeGen::createObjectSlot(const llvm::Twine &name) {
  llvm::AllocaInst *slot = createStackSlot(builder_.getPtrTy(), name);
  llvm::IRBuilder<> init(slot->getParent(), std::next(slot->getIterator()));
  init.CreateStore(llvm::ConstantPointerNull::get(builder_.getPtrTy()), slot);
  scopes_.back().slots.push_back({slot, nullptr});
  return slot;
}

llvm::Value *CodeGen::createLocal(const VarSymbol *var, llvm::StringRef name) {
  if (var->boxed()) {
    // Closures share the variable: it lives in a box, a new one each time
    // the declaration runs.
    llvm::Value *slot = createObjectSlot(name + ".box");
    boxSlots_[var] = slot;
    llvm::Value *box = newBox(typeOf(var));
    llvm::Value *old = builder_.CreateLoad(builder_.getPtrTy(), slot);
    builder_.CreateStore(box, slot);
    builder_.CreateCall(objReleaseFn_, {old});
    return builder_.CreateStructGEP(boxType(typeOf(var)), box, 2, name);
  }
  llvm::Value *slot = var->type->isManaged() ? createManagedSlot(var->type, name)
                                             : createStackSlot(lower(var->type), name);
  addresses_[var] = slot;
  return slot;
}

llvm::Value *CodeGen::addressOf(const VarSymbol *var) {
  if (var->boxed())
    return builder_.CreateStructGEP(boxType(typeOf(var)), boxPointer(var), 2);
  if (llvm::Value *address = addresses_.lookup(var))
    return address;
  llvm::Value *global = globals_.lookup(var);
  assert(global && "variable used before its declaration was emitted");
  return global;
}

llvm::Constant *CodeGen::utf8Constant(llvm::StringRef text) {
  auto [it, inserted] = utf8Constants_.try_emplace(text, nullptr);
  if (inserted)
    it->second = builder_.CreateGlobalString(text, ".msg", 0, module_.get());
  return it->second;
}

llvm::Value *CodeGen::spill(llvm::Value *value, const llvm::Twine &name) {
  llvm::AllocaInst *slot = createStackSlot(value->getType(), name);
  builder_.CreateStore(value, slot);
  return slot;
}

//===----------------------------------------------------------------------===//
// Reference counting
//===----------------------------------------------------------------------===//

void CodeGen::retainValue(llvm::Value *value, const Type *type) {
  if (isa<llvm::Constant>(value)) // literals, empty slots
    return;
  switch (type->kind()) {
  case Type::Kind::String:
    builder_.CreateCall(retainFn_, {value});
    break;
  case Type::Kind::Array:
    builder_.CreateCall(refMayBeMember(type) ? refRetainFn_ : arrRetainFn_, {value});
    break;
  case Type::Kind::Function:
    builder_.CreateCall(refMayBeMember(type) ? refRetainFn_ : plainObjRetainFn_,
                        {builder_.CreateExtractValue(value, 1)});
    break;
  case Type::Kind::Object: // the count is the first word, as for arrays
  case Type::Kind::Iterator:
  case Type::Kind::Json:
  case Type::Kind::Dict:
  case Type::Kind::Promise:
  case Type::Kind::Unknown:
    builder_.CreateCall(refMayBeMember(type) ? refRetainFn_ : arrRetainFn_, {value});
    break;
  case Type::Kind::Decimal:
    builder_.CreateCall(decimalRefCount(/*retain=*/true), {value});
    break;
  case Type::Kind::Union:
    if (isObjectUnion(type))
      builder_.CreateCall(refMayBeMember(type) ? refRetainFn_ : arrRetainFn_, {value});
    else if (type->isTagged())
      countTagged(value, type, /*retain=*/true);
    break;
  case Type::Kind::Optional:
    // Undefined holds the zero value, which needs no counting.
    if (type->inner()->isManaged())
      retainValue(optionalValue(value, type), type->inner());
    break;
  case Type::Kind::Result:
    // So does the half not in use.
    if (!type->isManaged())
      break;
    retainValue(resultHalf(value, type, /*error=*/false), type->okType());
    retainValue(resultHalf(value, type, /*error=*/true), type->errType());
    break;
  default:
    break;
  }
}

void CodeGen::releaseValue(llvm::Value *value, const Type *type) {
  if (isa<llvm::Constant>(value))
    return;
  switch (type->kind()) {
  case Type::Kind::String:
    builder_.CreateCall(releaseFn_, {value});
    break;
  case Type::Kind::Array:
    builder_.CreateCall(refMayBeMember(type) ? groupAwareRelease(arrReleaseFn_) : arrReleaseFn_,
                        {value});
    break;
  case Type::Kind::Function:
    builder_.CreateCall(refMayBeMember(type) ? groupAwareRelease(plainObjReleaseFn_)
                                             : plainObjReleaseFn_,
                        {builder_.CreateExtractValue(value, 1)});
    break;
  case Type::Kind::Object: {
    // An object of any type, in an instance of a generic function, one that
    // may be of a type down the declared chain, and an Error, which may be of
    // a class derived from it: through the drop function of its own layout.
    llvm::Function *release = type->isAnyObject() || hasSubtypes(type) || type->isTimer() ||
                                      type->isRuntimeObject() || type->isCrypto() ||
                                      (type->classDecl() && type->classDecl()->builtin)
                                  ? plainInstanceReleaseFn_
                                  : objectLayout(type).release;
    builder_.CreateCall(refMayBeMember(type) ? groupAwareRelease(release) : release, {value});
    break;
  }
  case Type::Kind::Decimal:
    builder_.CreateCall(decimalRefCount(/*retain=*/false), {value});
    break;
  case Type::Kind::Iterator: // through the drop function of its layout
  case Type::Kind::Dict:
  case Type::Kind::Promise:
  case Type::Kind::Unknown: // an object, or a box, which is one too
    builder_.CreateCall(refMayBeMember(type) ? groupAwareRelease(plainInstanceReleaseFn_)
                                             : plainInstanceReleaseFn_,
                        {value});
    break;
  case Type::Kind::Union:
    if (isObjectUnion(type)) // of any of the types: through its own layout
      builder_.CreateCall(refMayBeMember(type) ? groupAwareRelease(plainInstanceReleaseFn_)
                                               : plainInstanceReleaseFn_,
                          {value});
    else if (type->isTagged())
      countTagged(value, type, /*retain=*/false);
    break;
  case Type::Kind::Json:
    builder_.CreateCall(
        jsonFunction("tz_json_release_value", builder_.getVoidTy(), {builder_.getPtrTy()}),
        {value});
    break;
  case Type::Kind::Optional:
    if (type->inner()->isManaged())
      releaseValue(optionalValue(value, type), type->inner());
    break;
  case Type::Kind::Result:
    if (!type->isManaged())
      break;
    releaseValue(resultHalf(value, type, /*error=*/false), type->okType());
    releaseValue(resultHalf(value, type, /*error=*/true), type->errType());
    break;
  default:
    break;
  }
}

void CodeGen::retainString(llvm::Value *str) { retainValue(str, Type::string()); }

void CodeGen::releaseString(llvm::Value *str) { releaseValue(str, Type::string()); }

// An unknown holds undefined itself: it is its own `T | undefined`, null for
// undefined.
llvm::Value *CodeGen::makeOptional(llvm::Value *value, llvm::Value *has, const Type *type) {
  if (type->isUnknown() || zeroIsUndefined(type->inner())) {
    if (auto *always = dyn_cast<llvm::ConstantInt>(has); always && always->isOne())
      return value;
    return builder_.CreateSelect(has, value, llvm::Constant::getNullValue(value->getType()));
  }
  llvm::Value *result = llvm::UndefValue::get(lower(type));
  result = builder_.CreateInsertValue(result, value, 0);
  return builder_.CreateInsertValue(result, has, 1);
}

llvm::Value *CodeGen::undefinedValue(const Type *optionalType) {
  return llvm::Constant::getNullValue(lower(optionalType));
}

llvm::Value *CodeGen::optionalHas(llvm::Value *value, const Type *optionalType) {
  if (optionalType->isUnknown())
    return builder_.CreateIsNotNull(value);
  const Type *inner = optionalType->inner();
  if (isReference(inner))
    return builder_.CreateIsNotNull(value);
  if (isAtomType(inner)) // atoms start at 1
    return builder_.CreateICmpNE(value, builder_.getInt32(0));
  if (inner->isTagged()) // tags too
    return builder_.CreateICmpNE(unionTag(value), builder_.getInt32(0));
  return builder_.CreateExtractValue(value, 1);
}

llvm::Value *CodeGen::optionalValue(llvm::Value *value, const Type *optionalType) {
  if (optionalType->isUnknown() || zeroIsUndefined(optionalType->inner()))
    return value;
  return builder_.CreateExtractValue(value, 0);
}

CodeGen::Operand CodeGen::emitOperand(Expr *expr, bool canBorrow) {
  if (canBorrow)
    if (llvm::Value *value = emitBorrowed(expr))
      return {value, false, expr->type};
  return {emitExpr(expr), true, expr->type};
}

// The variable keeps the objects on the way alive, and the caller made sure
// that nothing assigns to them, or to the variable, while the value is used.
llvm::Value *CodeGen::emitBorrowed(Expr *expr) {
  if (auto *name = dyn_cast<NameExpr>(expr); name && name->var)
    return emitVarValue(name->var, /*borrow=*/true);
  // A variable or a field narrowed to hold a value.
  if (auto *conv = dyn_cast<CastExpr>(expr); conv && conv->isImplicit() && !conv->shortCircuit &&
                                             conv->operand->type->isOptional() &&
                                             conv->type == conv->operand->type->inner()) {
    llvm::Value *value = emitBorrowed(conv->operand);
    if (!value)
      return nullptr;
    // A `string | undefined` made a string (a part of a template or of `+`)
    // looks the same, and may not hold one: then it is "undefined", as in
    // JavaScript (see emitConversion).
    if (conv->type->isString())
      return builder_.CreateSelect(optionalHas(value, conv->operand->type),
                                   optionalValue(value, conv->operand->type),
                                   stringLiteral(u"undefined"));
    return optionalValue(value, conv->operand->type);
  }
  // An object seen as a type up or down its declared chain (narrowed by
  // instanceof): the same reference.
  if (auto *conv = dyn_cast<CastExpr>(expr); conv && conv->isImplicit() && !conv->shortCircuit &&
                                             conv->type->isObject() &&
                                             conv->operand->type->nonOptional()->isObject()) {
    llvm::Value *value = emitBorrowed(conv->operand);
    if (value && conv->operand->type->isOptional())
      value = optionalValue(value, conv->operand->type);
    return value;
  }
  // A variable or a field of a union narrowed to some of its members: the
  // same value, or the part of it that holds them, without counting.
  if (auto *conv = dyn_cast<CastExpr>(expr); conv && conv->isImplicit() && !conv->shortCircuit &&
                                             narrowsUnion(conv->operand->type, conv->type)) {
    llvm::Value *value = emitBorrowed(conv->operand);
    return value ? emitConversion(value, conv->operand->type, conv->type) : nullptr;
  }
  // A Result variable narrowed to an Ok or an Err: the same halves, one of
  // them known to be empty.
  if (auto *conv = dyn_cast<CastExpr>(expr); conv && conv->isImplicit() && conv->type->isResult() &&
                                             conv->operand->type->isResult()) {
    llvm::Value *value = emitBorrowed(conv->operand);
    return value ? emitConversion(value, conv->operand->type, conv->type) : nullptr;
  }
  auto *member = dyn_cast<MemberExpr>(expr);
  if (!member || !member->object->type->isObject() || member->object->type->isBytes() ||
      member->method)
    return nullptr;
  if (const InterfaceMember *method =
          dispatchedMember(member->object->type, member->name, member->signature);
      method && method->callable)
    return nullptr; // a method of an interface, bound to the object when read
  llvm::Value *object = emitBorrowed(member->object);
  if (!object)
    return nullptr;
  return builder_.CreateLoad(lower(member->type), fieldAddress(object, member->object->type, member->name),
                             member->name);
}

void CodeGen::dropOperand(const Operand &operand) {
  if (operand.owned)
    releaseValue(operand.value, operand.type);
}

//===----------------------------------------------------------------------===//
// Statements
//===----------------------------------------------------------------------===//

void CodeGen::emitStmt(Stmt *stmt) {
  switch (stmt->kind()) {
  case Stmt::Kind::Block:
    pushScope();
    emitStatements(cast<BlockStmt>(stmt));
    popScope();
    break;
  case Stmt::Kind::VarDecl:
    emitVarDecl(cast<VarDeclStmt>(stmt));
    break;
  case Stmt::Kind::Function:
    break; // emitted on its own
  case Stmt::Kind::Expr:
    if (tailStatements_.contains(stmt))
      emitTailJump(cast<CallExpr>(cast<ExprStmt>(stmt)->expr));
    else
      emitDiscarded(cast<ExprStmt>(stmt)->expr);
    break;
  case Stmt::Kind::If:
    emitIf(cast<IfStmt>(stmt));
    break;
  case Stmt::Kind::While:
    emitWhile(cast<WhileStmt>(stmt));
    break;
  case Stmt::Kind::DoWhile:
    emitDoWhile(cast<DoWhileStmt>(stmt));
    break;
  case Stmt::Kind::For:
    emitFor(cast<ForStmt>(stmt));
    break;
  case Stmt::Kind::ForOf:
    emitForOf(cast<ForOfStmt>(stmt));
    break;
  case Stmt::Kind::Switch:
    emitSwitch(cast<SwitchStmt>(stmt));
    break;
  case Stmt::Kind::Labeled:
    emitLabeled(cast<LabeledStmt>(stmt));
    break;
  case Stmt::Kind::Return:
    emitReturn(cast<ReturnStmt>(stmt));
    break;
  case Stmt::Kind::Break:
  case Stmt::Kind::Continue:
    emitJump(stmt);
    break;
  case Stmt::Kind::Empty:
  case Stmt::Kind::TypeAlias:
  case Stmt::Kind::Enum:
    break;
  case Stmt::Kind::Class:
    // The class of a class expression gets its static fields where the
    // expression is.
    if (!cast<ClassDecl>(stmt)->expression)
      emitClassDeclaration(cast<ClassDecl>(stmt));
    break;
  case Stmt::Kind::Debugger:
    builder_.CreateCall(rt_.debugger);
    break;
  case Stmt::Kind::Throw:
    emitThrowStmt(cast<ThrowStmt>(stmt));
    break;
  case Stmt::Kind::Try:
    emitTry(cast<TryStmt>(stmt));
    break;
  }
}

void CodeGen::emitStatements(BlockStmt *block) {
  NestedGroup *group = block->group;
  for (size_t i = 0; i < block->body.size(); ++i) {
    if (group && i == group->firstUse)
      emitGroupCreation(group);
    emitStmt(block->body[i]);
  }
}

void CodeGen::pushLoop(llvm::BasicBlock *breakTarget, llvm::BasicBlock *continueTarget,
                       size_t scopeDepth) {
  targets_.push_back(
      {std::move(pendingLabels_), /*isLoop=*/true, /*isSwitch=*/false, breakTarget, continueTarget, scopeDepth});
  pendingLabels_.clear();
}

void CodeGen::emitJump(Stmt *stmt) {
  bool isBreak = isa<BreakStmt>(stmt);
  llvm::StringRef label = isBreak ? cast<BreakStmt>(stmt)->label : cast<ContinueStmt>(stmt)->label;
  for (size_t i = targets_.size(); i > 0; --i) {
    const JumpTarget &t = targets_[i - 1];
    bool matches = !label.empty() ? llvm::is_contained(t.labels, label)
                   : isBreak      ? t.isLoop || t.isSwitch
                                  : t.isLoop;
    if (matches) {
      emitJumpTo(i - 1, isBreak);
      return;
    }
  }
  llvm_unreachable("Sema checks every break and continue");
}

void CodeGen::emitLabeled(LabeledStmt *stmt) {
  llvm::SmallVector<llvm::StringRef, 1> labels;
  Stmt *body = stmt;
  while (auto *labeled = dyn_cast<LabeledStmt>(body)) {
    labels.push_back(labeled->label);
    body = labeled->body;
  }
  if (isa<WhileStmt>(body) || isa<DoWhileStmt>(body) || isa<ForStmt>(body) ||
      isa<ForOfStmt>(body) || isa<SwitchStmt>(body)) {
    pendingLabels_ = labels; // the loop or switch takes them
    emitStmt(body);
    return;
  }
  // `label: { ... break label; ... }`
  llvm::BasicBlock *endBlock = newBlock("label.end");
  targets_.push_back({labels, /*isLoop=*/false, /*isSwitch=*/false, endBlock, nullptr, scopes_.size()});
  emitStmt(body);
  targets_.pop_back();
  emitBlock(endBlock);
}

// The discriminant is evaluated once and compared with each test in order;
// the first one equal to it jumps to its case, and no match to `default`.
// The case bodies follow each other, so control falls through.
void CodeGen::emitSwitch(SwitchStmt *stmt) {
  const Type *type = stmt->discriminant->type;
  llvm::SmallVector<llvm::StringRef, 1> labels = std::move(pendingLabels_);
  pendingLabels_.clear();
  size_t depth = scopes_.size();
  pushScope(); // the variables of the cases
  llvm::Value *value = emitExpr(stmt->discriminant);
  llvm::Value *slot = nullptr;
  if (type->isManaged()) {
    // Released whichever way control leaves the switch.
    slot = createManagedSlot(type, "switch.value");
    builder_.CreateStore(value, slot);
  }

  llvm::BasicBlock *exitBlock = newBlock("switch.exit");
  llvm::BasicBlock *endBlock = newBlock("switch.end");
  std::vector<llvm::BasicBlock *> bodies;
  llvm::BasicBlock *defaultBody = exitBlock;
  for (const SwitchCase &c : stmt->cases) {
    bodies.push_back(newBlock(c.test ? "switch.case" : "switch.default"));
    if (!c.test)
      defaultBody = bodies.back();
  }

  for (size_t i = 0; i < stmt->cases.size(); ++i) {
    const SwitchCase &c = stmt->cases[i];
    if (!c.test)
      continue;
    llvm::Value *test = emitExpr(c.test);
    llvm::Value *current = slot ? builder_.CreateLoad(lower(type), slot) : value;
    llvm::Value *equal;
    if (!c.compareType) // `case undefined:`
      equal = type->isOptional()    ? builder_.CreateNot(optionalHas(current, type))
              : type->isUndefined() ? builder_.getTrue()
                                    : builder_.getFalse();
    else
      equal = emitValuesEqual(emitConversion(current, type, c.compareType), test, c.compareType);
    releaseValue(test, c.test->type);
    llvm::BasicBlock *next = newBlock("switch.test");
    builder_.CreateCondBr(equal, bodies[i], next);
    switchTo(next);
  }
  builder_.CreateBr(defaultBody);

  targets_.push_back({labels, /*isLoop=*/false, /*isSwitch=*/true, endBlock, nullptr, depth});
  for (size_t i = 0; i < stmt->cases.size(); ++i) {
    switchTo(bodies[i]);
    for (Stmt *s : stmt->cases[i].body)
      emitStmt(s);
    branchTo(i + 1 < bodies.size() ? bodies[i + 1] : exitBlock);
  }
  targets_.pop_back();
  switchTo(exitBlock);
  popScope();
  emitBlock(endBlock);
}

void CodeGen::storeReplacing(llvm::Value *address, llvm::Value *value, const Type *type) {
  if (!type->isManaged()) {
    builder_.CreateStore(value, address);
    return;
  }
  llvm::Value *previous = builder_.CreateLoad(lower(type), address);
  builder_.CreateStore(value, address);
  releaseValue(previous, type);
}

void CodeGen::emitVarDecl(VarDeclStmt *decl) {
  for (VarDeclarator &d : decl->decls) {
    if (d.alias) // another name for a generic function
      continue;
    const VarSymbol *var = d.sym;
    llvm::Type *type = lower(var->type);
    bool global = var->isGlobal && var->usedInFunction;
    llvm::Value *address = global ? addressOf(var) : createLocal(var, d.name);
    llvm::Value *value = d.init ? emitExpr(d.init) : llvm::Constant::getNullValue(type);
    // The slot may still hold a value from an earlier run of this declaration.
    storeVar(var, address, value, /*replacing=*/true);
    // A global that keeps its value for good: the groups in it are eternal.
    if (isEternal(var)) {
      pinValue(value, var->type);
      if (!global)
        eternalSlots_.insert(address);
    }
  }
}

void CodeGen::emitIf(IfStmt *stmt) {
  llvm::Value *cond = emitExpr(stmt->cond);
  llvm::BasicBlock *thenBlock = newBlock("if.then");
  llvm::BasicBlock *endBlock = newBlock("if.end");
  llvm::BasicBlock *elseBlock = stmt->elseStmt ? newBlock("if.else") : endBlock;
  builder_.CreateCondBr(cond, thenBlock, elseBlock);

  switchTo(thenBlock);
  emitStmt(stmt->thenStmt);
  branchTo(endBlock);

  if (stmt->elseStmt) {
    switchTo(elseBlock);
    emitStmt(stmt->elseStmt);
    branchTo(endBlock);
  }
  switchTo(endBlock);
}

void CodeGen::emitWhile(WhileStmt *stmt) {
  llvm::BasicBlock *condBlock = newBlock("while.cond");
  llvm::BasicBlock *bodyBlock = newBlock("while.body");
  llvm::BasicBlock *endBlock = newBlock("while.end");

  emitBlock(condBlock);
  llvm::Value *cond = emitExpr(stmt->cond);
  builder_.CreateCondBr(cond, bodyBlock, endBlock);

  switchTo(bodyBlock);
  pushLoop(endBlock, condBlock, scopes_.size());
  emitStmt(stmt->body);
  targets_.pop_back();
  branchTo(condBlock);

  switchTo(endBlock);
}

void CodeGen::emitDoWhile(DoWhileStmt *stmt) {
  llvm::BasicBlock *bodyBlock = newBlock("do.body");
  llvm::BasicBlock *condBlock = newBlock("do.cond");
  llvm::BasicBlock *endBlock = newBlock("do.end");

  emitBlock(bodyBlock);
  pushLoop(endBlock, condBlock, scopes_.size());
  emitStmt(stmt->body);
  targets_.pop_back();

  emitBlock(condBlock);
  llvm::Value *cond = emitExpr(stmt->cond);
  builder_.CreateCondBr(cond, bodyBlock, endBlock);

  switchTo(endBlock);
}

void CodeGen::emitFor(ForStmt *stmt) {
  pushScope(); // variables declared in the loop header
  if (stmt->init)
    emitStmt(stmt->init);

  llvm::BasicBlock *condBlock = newBlock("for.cond");
  llvm::BasicBlock *bodyBlock = newBlock("for.body");
  llvm::BasicBlock *stepBlock = newBlock("for.step");
  llvm::BasicBlock *endBlock = newBlock("for.end");

  emitBlock(condBlock);
  if (stmt->cond) {
    llvm::Value *cond = emitExpr(stmt->cond);
    builder_.CreateCondBr(cond, bodyBlock, endBlock);
  } else {
    builder_.CreateBr(bodyBlock);
  }

  switchTo(bodyBlock);
  pushLoop(endBlock, stepBlock, scopes_.size());
  emitStmt(stmt->body);
  targets_.pop_back();

  emitBlock(stepBlock);
  // `for (let i ...)`: every iteration has an i of its own. A closure that
  // captured i keeps that iteration's box; the next iteration goes on in a
  // copy.
  if (auto *decl = llvm::dyn_cast_or_null<VarDeclStmt>(stmt->init))
    for (VarDeclarator &d : decl->decls) {
      if (d.alias || !d.sym->boxed())
        continue;
      const Type *type = typeOf(d.sym);
      llvm::Value *slot = boxSlots_.lookup(d.sym);
      llvm::Value *old = builder_.CreateLoad(builder_.getPtrTy(), slot);
      llvm::Value *box = newBox(type);
      llvm::Value *value = builder_.CreateLoad(lower(type), builder_.CreateStructGEP(boxType(type), old, 2));
      retainValue(value, type);
      builder_.CreateStore(value, builder_.CreateStructGEP(boxType(type), box, 2));
      if (isCyclicBox(type))
        groupStore(box, value, type); // inside the group of the box now
      builder_.CreateStore(box, slot);
      builder_.CreateCall(objReleaseFn_, {old});
    }
  if (stmt->step)
    emitDiscarded(stmt->step);
  builder_.CreateBr(condBlock);

  switchTo(endBlock);
  popScope();
}

// `for (const x of iterable)`: the iterable is evaluated once and held in a
// hidden slot; each step reads the current length, as JavaScript's iterators
// do, so elements pushed by the body are visited too.
void CodeGen::emitForOf(ForOfStmt *stmt) {
  if (stmt->isAwait) {
    emitForAwait(stmt);
    return;
  }
  if (stmt->iterable->type->isIterator() || stmt->iterable->type->isTypedArray()) {
    emitIteratorForOf(stmt);
    return;
  }
  pushScope(); // the hidden iterable
  const Type *iterableType = stmt->iterable->type;
  llvm::Value *iterableSlot = createManagedSlot(iterableType, "for.iterable");
  builder_.CreateStore(emitExpr(stmt->iterable), iterableSlot);

  llvm::BasicBlock *condBlock = newBlock("for.of.cond");
  llvm::BasicBlock *bodyBlock = newBlock("for.of.body");
  llvm::BasicBlock *endBlock = newBlock("for.of.end");
  emitArrayForOf(stmt, iterableSlot, condBlock, bodyBlock, endBlock);
  switchTo(endBlock);
  popScope();
}

void CodeGen::emitReturn(ReturnStmt *stmt) {
  // A generator emitted inline finishes, and one emitted as a coroutine
  // gives what it returns to whoever resumed it.
  if (inline_) {
    llvm::Value *value = stmt->value ? emitExpr(stmt->value) : nullptr;
    if (value && inline_->returnSlot)
      storeReplacing(inline_->returnSlot, value, stmt->value->type);
    else if (value)
      releaseValue(value, stmt->value->type);
    emitCleanups(inline_->scopeDepth);
    builder_.CreateBr(inline_->exit);
    startDeadBlock();
    return;
  }
  if (coro_ && currentFn_ && currentFn_->isGenerator && async_) {
    emitAsyncGeneratorReturn(stmt);
    return;
  }
  if (coro_ && currentFn_ && currentFn_->isGenerator) {
    emitReturnOf(stmt->value ? emitExpr(stmt->value) : nullptr);
    return;
  }
  if (async_) { // an async function fulfills its promise
    emitAsyncReturnStmt(stmt);
    return;
  }
  // Not out of a `try` with a `finally`, which must run first.
  if (stmt->value && tailGroup_ && hasTailCall(stmt->value) && !finallyBetween(~size_t(0))) {
    emitTailReturn(stmt->value);
    return;
  }
  // In a function that recurses, the call a return gives (with numbers
  // added to it) is left unchecked: what it threw stays in tz_thrown, and
  // the caller checks. Then LLVM can turn `return f(n - 1) + f(n - 2)` into
  // a loop over the second call, at the price of that frame in the stack
  // of an error.
  const CallExpr *outer = returnCall_;
  if (stmt->value && currentFn_ && recursive_.contains(currentFn_) && !inline_ && !coro_ &&
      !finallyBetween(~size_t(0)))
    returnCall_ = returnedCall(stmt->value);
  emitReturnValue(stmt->value);
  returnCall_ = outer;
}

const CallExpr *CodeGen::returnedCall(const Expr *value) {
  if (!value->type || !(value->type->isNumeric() || value->type->isBool()))
    return nullptr;
  if (auto *call = dyn_cast<CallExpr>(value))
    return call->builtin == Builtin::None && (call->func || call->method) ? call : nullptr;
  if (auto *conv = dyn_cast<CastExpr>(value))
    return conv->isImplicit() && !conv->shortCircuit && conv->operand->type->isNumeric()
               ? returnedCall(conv->operand)
               : nullptr;
  if (auto *unary = dyn_cast<UnaryExpr>(value))
    return unary->op == UnaryOp::Neg || unary->op == UnaryOp::Not || unary->op == UnaryOp::BitNot
               ? returnedCall(unary->operand)
               : nullptr;
  auto *bin = dyn_cast<BinaryExpr>(value);
  if (!bin || !bin->lhs->type->isNumeric())
    return nullptr;
  switch (bin->op) {
  case BinaryOp::Add:
  case BinaryOp::Sub:
  case BinaryOp::Mul:
  case BinaryOp::BitAnd:
  case BinaryOp::BitOr:
  case BinaryOp::BitXor:
  case BinaryOp::Lt:
  case BinaryOp::Le:
  case BinaryOp::Gt:
  case BinaryOp::Ge:
  case BinaryOp::Eq:
  case BinaryOp::Ne:
    return returnedCall(bin->rhs);
  default:
    return nullptr;
  }
}

// `return value`, or `return` without one (null).
void CodeGen::emitReturnValue(Expr *expr) {
  llvm::Value *value = expr ? emitExpr(expr) : nullptr;
  if (value && returnType_ && returnType_->isVoid() && !expr->type->isVoid()) {
    // A callback whose result is dropped (see FuncSymbol::discardsResult).
    releaseValue(value, expr->type);
    value = nullptr;
  }
  emitReturnOf(value && !expr->type->isVoid() ? value : nullptr);
}

void CodeGen::emitDiscarded(Expr *expr) {
  if (auto *assign = dyn_cast<AssignExpr>(expr)) {
    emitAssign(assign, /*needResult=*/false);
    return;
  }
  if (auto *seq = dyn_cast<SequenceExpr>(expr)) {
    for (Expr *e : seq->exprs)
      emitDiscarded(e);
    return;
  }
  llvm::Value *value = emitExpr(expr);
  if (expr->type->isManaged())
    releaseValue(value, expr->type);
}

//===----------------------------------------------------------------------===//
// Expressions
//===----------------------------------------------------------------------===//

// A built-in that fails in it throws from where the expression is (loc_).
llvm::Value *CodeGen::emitExpr(Expr *expr) {
  llvm::SMLoc outer = std::exchange(loc_, expr->loc());
  llvm::Value *value = emitExprKind(expr);
  loc_ = outer;
  return value;
}

llvm::Value *CodeGen::emitExprKind(Expr *expr) {
  switch (expr->kind()) {
  case Expr::Kind::IntLit: {
    auto *lit = cast<IntLitExpr>(expr);
    const Type *type = storedType(lit->type); // `0 | 1` holds an int
    if (type->isFloat())
      return llvm::ConstantFP::get(builder_.getDoubleTy(), static_cast<double>(lit->value));
    if (type->isDecimal())
      return emitDecimalLiteral(lit);
    return llvm::ConstantInt::get(lower(type), lit->value);
  }
  case Expr::Kind::FloatLit:
    if (expr->type->isDecimal())
      return emitDecimalLiteral(expr);
    return llvm::ConstantFP::get(builder_.getDoubleTy(), cast<FloatLitExpr>(expr)->value);
  case Expr::Kind::BoolLit:
    return builder_.getInt1(cast<BoolLitExpr>(expr)->value);
  case Expr::Kind::StringLit:
    if (expr->type->isStringLiteral()) // a literal type: its atom
      return builder_.getInt32(expr->type->atom());
    return stringLiteral(cast<StringLitExpr>(expr)->value);
  case Expr::Kind::RegexLit:
    return emitRegexLiteral(cast<RegexLitExpr>(expr));
  case Expr::Kind::Undefined:
    return builder_.getFalse(); // conversions and printing handle it
  case Expr::Kind::Template:
    return emitTemplate(cast<TemplateExpr>(expr));
  case Expr::Kind::ArrayLit:
    return emitArrayLiteral(cast<ArrayLitExpr>(expr));
  case Expr::Kind::ObjectLit:
    return emitObjectLiteral(cast<ObjectLitExpr>(expr));
  case Expr::Kind::Name: {
    auto *name = cast<NameExpr>(expr);
    if (name->func) // a top-level function as a value
      return makeFunctionValue(functionInfo(name->func->decl),
                               llvm::ConstantPointerNull::get(builder_.getPtrTy()));
    if (!name->var && name->type->isJson()) // `null`, which only a Json holds
      return emitJsonNull();
    if (name->cls) // a class as a value
      return classObject(name->cls);
    return emitVarValue(name->var, /*borrow=*/false);
  }
  case Expr::Kind::Function:
    return emitFunctionExpr(cast<FunctionExpr>(expr));
  case Expr::Kind::Member:
    return emitMember(cast<MemberExpr>(expr));
  case Expr::Kind::Index: {
    auto *index = cast<IndexExpr>(expr);
    if (index->enumDecl)
      return emitEnumName(index);
    if (index->object->type->nonOptional()->isJson())
      return emitJsonIndex(index);
    if (index->object->type->isString())
      return emitStringIndex(index);
    if (index->keyed) { // `o[k]` with a key of literals
      Operand object = emitOperand(index->object, cannotAssign(index->index));
      llvm::Value *key = emitExpr(index->index);
      llvm::Value *value = builder_.CreateLoad(
          lower(index->type),
          keyedFieldAddress(object.value, object.type, key, index->index->type), "field");
      retainValue(value, index->type);
      dropOperand(object);
      return value;
    }
    if (index->object->type->isTypedArray())
      return emitBytesIndex(index);
    return emitArrayIndex(index);
  }
  case Expr::Kind::Unary:
    return emitUnary(cast<UnaryExpr>(expr));
  case Expr::Kind::Binary:
    return emitBinary(cast<BinaryExpr>(expr));
  case Expr::Kind::Assign:
    return emitAssign(cast<AssignExpr>(expr), /*needResult=*/true);
  case Expr::Kind::Update:
    return emitUpdate(cast<UpdateExpr>(expr));
  case Expr::Kind::Call:
    return emitCall(cast<CallExpr>(expr));
  case Expr::Kind::New: {
    auto *create = cast<NewExpr>(expr);
    if (create->reflect)
      return emitReflectNew(create);
    if (create->cls)
      return emitNew(create);
    return emitNewArray(create);
  }
  case Expr::Kind::Class:
    return emitClassExpr(cast<ClassExpr>(expr));
  case Expr::Kind::Conditional:
    return emitConditional(cast<ConditionalExpr>(expr));
  case Expr::Kind::Cast: {
    auto *c = cast<CastExpr>(expr);
    if (c->shortCircuit)
      return emitChainLink(c);
    if (c->adaptsResult)
      return emitResultAdapter(c);
    if (c->type->isFunction() && c->operand->type->isFunction())
      return emitFunctionAdapter(c);
    // The text of a value of a union, whatever member it holds.
    if (c->text) {
      llvm::Value *value = emitExpr(c->operand);
      llvm::Value *text = emitValueToString(value, c->operand->type);
      releaseValue(value, c->operand->type);
      return text;
    }
    // A Json known to be a boolean is that boolean, not a truth to test.
    if (c->type->isBool() && !c->operand->type->isBool() &&
        !c->operand->type->nonOptional()->isJson() && !c->operand->type->isUnknown())
      return emitTruthOf(c->operand);
    return emitConversion(emitExpr(c->operand), c->operand->type, c->type);
  }
  case Expr::Kind::Sequence: {
    auto *seq = cast<SequenceExpr>(expr);
    for (size_t i = 0; i + 1 < seq->exprs.size(); ++i)
      emitDiscarded(seq->exprs[i]);
    return emitExpr(seq->exprs.back());
  }
  case Expr::Kind::OptionalChain:
    return emitOptionalChain(cast<OptionalChainExpr>(expr));
  case Expr::Kind::Yield:
    // In the generator of safeTry, which is no generator, `yield* r` takes
    // the value of the Result r or returns its Err.
    if (currentFn_ && currentFn_->safeTry)
      return emitSafeTryYield(cast<YieldExpr>(expr));
    return emitYield(cast<YieldExpr>(expr));
  case Expr::Kind::Await:
    return emitAwait(cast<AwaitExpr>(expr));
  }
  llvm_unreachable("unknown expression kind");
}

// `a?.b.c`: each link whose object is undefined jumps to the end, where the
// chain is undefined.
llvm::Value *CodeGen::emitOptionalChain(OptionalChainExpr *chain) {
  if (!chain->mayShortCircuit)
    return emitExpr(chain->chain);
  llvm::BasicBlock *undefinedBlock = newBlock("chain.undefined");
  llvm::BasicBlock *endBlock = newBlock("chain.end");
  chainEnds_.push_back(undefinedBlock);
  llvm::Value *value = emitExpr(chain->chain);
  chainEnds_.pop_back();
  const Type *type = chain->chain->type;
  if (!type->isVoid() && !type->isOptional())
    value = makeOptional(value, builder_.getTrue(), chain->type);
  llvm::BasicBlock *valueEnd = builder_.GetInsertBlock();
  builder_.CreateBr(endBlock);
  switchTo(undefinedBlock);
  builder_.CreateBr(endBlock);
  switchTo(endBlock);
  if (type->isVoid())
    return nullptr;
  llvm::PHINode *phi = builder_.CreatePHI(lower(chain->type), 2);
  phi->addIncoming(value, valueEnd);
  phi->addIncoming(undefinedValue(chain->type), undefinedBlock);
  return phi;
}

llvm::Value *CodeGen::emitChainLink(CastExpr *link) {
  const Type *type = link->operand->type;
  if (type->nonOptional()->isJson())
    return emitJsonChainLink(link);
  llvm::Value *value = emitExpr(link->operand);
  llvm::BasicBlock *valueBlock = newBlock("chain.link");
  // Undefined holds nothing to release.
  builder_.CreateCondBr(optionalHas(value, type), valueBlock, chainEnds_.back());
  switchTo(valueBlock);
  return optionalValue(value, type);
}

llvm::Value *CodeGen::emitUnary(UnaryExpr *unary) {
  if (unary->op == UnaryOp::TypeOf)
    return emitTypeOf(unary->operand);
  if (unary->op == UnaryOp::NonNull)
    return emitNonNull(unary);
  if (unary->op == UnaryOp::Void) {
    emitDiscarded(unary->operand);
    return builder_.getFalse(); // undefined, as for the literal
  }
  const Type *type = unary->operand->type;
  if ((unary->op == UnaryOp::Plus || unary->op == UnaryOp::Neg) && type->isString()) {
    // `+s` is Number(s), and `-s` is -Number(s).
    Operand str = emitOperand(unary->operand, /*canBorrow=*/true);
    llvm::Value *number = emitStringToNumber(str.value);
    dropOperand(str);
    return unary->op == UnaryOp::Neg ? builder_.CreateFNeg(number) : number;
  }
  if (type->isDecimal()) { // Sema allows only `-x` and `+literal`
    if (unary->op == UnaryOp::Plus)
      return emitExpr(unary->operand);
    if (llvm::isa<IntLitExpr>(unary->operand) || llvm::isa<FloatLitExpr>(unary->operand))
      return emitDecimalLiteral(unary);
    Operand value = emitOperand(unary->operand, /*canBorrow=*/true);
    llvm::Value *negated =
        callReturningDecimal("tz_dec_neg", {decimalCoef(value.value), decimalMeta(value.value)});
    dropOperand(value);
    return negated;
  }
  llvm::Value *operand = emitExpr(unary->operand);
  switch (unary->op) {
  case UnaryOp::Neg:
    if (type->isBool()) // `-true` is -1
      return builder_.CreateFNeg(builder_.CreateUIToFP(operand, builder_.getDoubleTy()));
    return storedType(unary->type)->isFloat() ? builder_.CreateFNeg(operand)
                                              : builder_.CreateNeg(operand);
  case UnaryOp::Plus:
    return type->isBool() ? builder_.CreateUIToFP(operand, builder_.getDoubleTy()) : operand;
  case UnaryOp::Not:
  case UnaryOp::BitNot:
    return builder_.CreateNot(operand);
  case UnaryOp::TypeOf:
  case UnaryOp::NonNull:
  case UnaryOp::Void:
  case UnaryOp::Delete: // Sema rejects it for now
    break;
  }
  llvm_unreachable("unknown unary operator");
}

// The text of a name or a field read through names, for messages; empty for
// anything else.
static std::string valueText(const Expr *expr) {
  while (auto *conv = dyn_cast<CastExpr>(expr)) {
    if (!conv->isImplicit())
      return "";
    expr = conv->operand;
  }
  if (auto *name = dyn_cast<NameExpr>(expr))
    return name->name.str();
  if (auto *member = dyn_cast<MemberExpr>(expr)) {
    std::string object = valueText(member->object);
    return object.empty() ? "" : object + (member->optional ? "?." : ".") + member->name.str();
  }
  if (auto *index = dyn_cast<IndexExpr>(expr)) {
    std::string object = valueText(index->object), position = valueText(index->index);
    if (auto *lit = dyn_cast<IntLitExpr>(index->index))
      position = std::to_string(lit->value);
    return object.empty() || position.empty() ? "" : object + "[" + position + "]";
  }
  return "";
}

// `x!`: TypeError if x is undefined (undefined holds nothing to release).
llvm::Value *CodeGen::emitNonNull(UnaryExpr *unary) {
  const Type *type = unary->operand->type;
  llvm::Value *value = emitExpr(unary->operand);
  if (!type->isOptional())
    return value;
  llvm::BasicBlock *throwBlock = newBlock("nonnull.undefined");
  llvm::BasicBlock *okBlock = newBlock("nonnull.ok");
  builder_.CreateCondBr(builder_.CreateNot(optionalHas(value, type)), throwBlock, okBlock,
                        llvm::MDBuilder(ctx_).createUnlikelyBranchWeights());
  switchTo(throwBlock);
  std::string text = valueText(unary->operand);
  emitThrowNew(1, (text.empty() ? "the value" : text) + " is undefined");
  switchTo(okBlock);
  return optionalValue(value, type);
}

static std::u16string ascii16(llvm::StringRef text) { return std::u16string(text.begin(), text.end()); }

// `typeof x` follows from the type of x, except for `T | undefined`.
llvm::Value *CodeGen::emitTypeOf(Expr *operand) {
  const Type *type = operand->type;
  if (type->nonOptional()->isJson())
    return emitJsonTypeOf(operand);
  if (type->isUnknown() || type->isAnyObject())
    return emitUnknownTypeOf(operand);
  // A tagged union: by its tag, which is 0 for undefined.
  if (type->nonOptional()->isTagged()) {
    Operand value = emitOperand(operand, /*canBorrow=*/true);
    llvm::Value *name = emitUnionTypeOf(value.value, type->nonOptional());
    dropOperand(value);
    return name;
  }
  if (type->isOptional()) {
    Operand value = emitOperand(operand, /*canBorrow=*/true);
    llvm::Value *has = optionalHas(value.value, type);
    dropOperand(value);
    return builder_.CreateSelect(has, stringLiteral(ascii16(typeOfName(type->inner()))),
                                 stringLiteral(u"undefined"));
  }
  if (!isa<NameExpr>(operand)) // evaluated for its side effects
    emitDiscarded(operand);
  return stringLiteral(ascii16(typeOfName(type)));
}

// `Color[value]`: the name of the enum member with that value, or undefined.
llvm::Value *CodeGen::emitEnumName(IndexExpr *index) {
  llvm::Value *value = emitExpr(index->index);
  const Type *type = index->type;
  llvm::Value *result = undefinedValue(type);
  // Each member's test wraps those of the members before it, so that of
  // members with one value the last names it, as in JavaScript.
  for (const EnumMember &m : index->enumDecl->members) {
    llvm::SmallVector<llvm::UTF16, 16> units;
    llvm::convertUTF8ToUTF16String(m.name, units);
    llvm::Value *name = makeOptional(stringLiteral(std::u16string(units.begin(), units.end())),
                                     builder_.getTrue(), type);
    llvm::Value *matches = builder_.CreateICmpEQ(value, builder_.getInt64(static_cast<uint64_t>(int64_t(m.number))));
    result = builder_.CreateSelect(matches, name, result);
  }
  return result;
}

llvm::Value *CodeGen::emitBinary(BinaryExpr *bin) {
  switch (bin->op) {
  case BinaryOp::LogAnd:
  case BinaryOp::LogOr:
    return emitLogical(bin);
  case BinaryOp::Nullish:
    return emitNullish(bin);
  case BinaryOp::Eq:
  case BinaryOp::Ne:
    return emitEquality(bin);
  case BinaryOp::InstanceOf:
    return emitInstanceOf(bin);
  case BinaryOp::In: // of a union, which Sema allows
    return emitObjectIn(bin);
  default:
    break;
  }
  if (bin->type->isString())
    return emitConcat(bin);
  if (bin->lhs->type->isString())
    return emitStringComparison(bin);
  if (bin->lhs->type->isDecimal())
    return emitDecimalBinary(bin);

  llvm::Value *lhs = emitExpr(bin->lhs);
  llvm::Value *rhs = emitExpr(bin->rhs);
  switch (bin->op) {
  case BinaryOp::Lt:
  case BinaryOp::Le:
  case BinaryOp::Gt:
  case BinaryOp::Ge:
    return emitComparison(bin->op, lhs, rhs, bin->lhs->type);
  case BinaryOp::UShr:
    // An int's `x >>> n` that Sema made a long: the unsigned 32-bit result.
    if (bin->type != bin->lhs->type)
      return builder_.CreateZExt(emitArithmetic(bin->op, lhs, rhs, bin->lhs->type), lower(bin->type));
    return emitArithmetic(bin->op, lhs, rhs, bin->type);
  default:
    return emitArithmetic(bin->op, lhs, rhs, bin->type);
  }
}

// `&&` and `||` evaluate their right side only when needed. When the left
// side decides, it is the result (Sema converted the right side to the type
// of the whole).
llvm::Value *CodeGen::emitLogical(BinaryExpr *bin) {
  bool isAnd = bin->op == BinaryOp::LogAnd;
  const Type *lhsType = bin->lhs->type;
  llvm::Value *lhs = emitExpr(bin->lhs);
  llvm::Value *truth = emitTruth(lhs, lhsType);
  llvm::Value *decided = decidedValue(bin, lhs); // used only when it decides
  llvm::BasicBlock *lhsEnd = builder_.GetInsertBlock();
  llvm::BasicBlock *rhsBlock = newBlock(isAnd ? "and.rhs" : "or.rhs");
  llvm::BasicBlock *endBlock = newBlock(isAnd ? "and.end" : "or.end");
  if (isAnd)
    builder_.CreateCondBr(truth, rhsBlock, endBlock);
  else
    builder_.CreateCondBr(truth, endBlock, rhsBlock);

  switchTo(rhsBlock);
  releaseValue(lhs, lhsType); // the right side gives the result
  llvm::Value *rhs = emitExpr(bin->rhs);
  llvm::BasicBlock *rhsEnd = builder_.GetInsertBlock();
  builder_.CreateBr(endBlock);

  switchTo(endBlock);
  llvm::PHINode *phi = builder_.CreatePHI(lower(bin->type), 2);
  phi->addIncoming(decided, lhsEnd);
  phi->addIncoming(rhs, rhsEnd);
  return phi;
}

llvm::Value *CodeGen::decidedValue(BinaryExpr *bin, llvm::Value *lhs) {
  bool isAnd = bin->op == BinaryOp::LogAnd;
  const Type *from = bin->lhs->type, *to = bin->type;
  if (from->isBool() && to->isBool())
    return builder_.getInt1(!isAnd);
  const Type *base = from->nonOptional();
  // A falsy array, function or object is undefined, which holds nothing to
  // release.
  if (isAnd && (base->isArray() || base->isFunction() || isReference(base)))
    return undefinedValue(to);
  if (from == to)
    return lhs;
  // A truthy `T | undefined` holds a value; the other side may be undefined.
  if (from->isOptional())
    return optionalValue(lhs, from);
  return makeOptional(lhs, builder_.getTrue(), to);
}

llvm::Value *CodeGen::emitTruth(llvm::Value *value, const Type *type) {
  switch (type->kind()) {
  case Type::Kind::Bool:
    return value;
  case Type::Kind::Literal:
  case Type::Kind::Union:
    return emitUnionTruth(value, type);
  case Type::Kind::String:
    return builder_.CreateICmpNE(stringLength(value), builder_.getInt64(0));
  case Type::Kind::Optional: {
    llvm::Value *has = optionalHas(value, type);
    const Type *inner = type->inner();
    if (!inner->isBool() && !inner->isString() && !inner->isLiteral() &&
        !(inner->isUnion() && !isObjectUnion(inner)))
      return has; // an array, a function or an object is always truthy
    return builder_.CreateAnd(has, emitTruth(optionalValue(value, type), inner));
  }
  default:
    llvm_unreachable("Sema rejects testing other types");
  }
}

llvm::Value *CodeGen::emitTruthOf(Expr *expr) {
  // A narrowed variable holds a value: its own truth is the same, and it can
  // be borrowed.
  if (auto *conv = dyn_cast<CastExpr>(expr);
      conv && conv->isImplicit() && !conv->shortCircuit && isa<NameExpr>(conv->operand) &&
      conv->operand->type->isOptional() && conv->type == conv->operand->type->inner())
    expr = conv->operand;
  Operand value = emitOperand(expr, /*canBorrow=*/true);
  llvm::Value *truth = emitTruth(value.value, value.type);
  dropOperand(value);
  return truth;
}

llvm::Value *CodeGen::emitValuesEqual(llvm::Value *a, llvm::Value *b, const Type *type) {
  type = storedType(type);
  // Atoms are numbers, a union of objects a reference.
  if (isAtomType(type) || isObjectUnion(type))
    return builder_.CreateICmpEQ(a, b);
  if (type->isTagged())
    return emitUnionEqual(a, b, type);
  if (type->nonOptional()->isJson()) { // as `===`, which a switch uses
    llvm::Type *ptr = builder_.getPtrTy();
    llvm::Value *equal = builder_.CreateCall(
        jsonFunction("tz_json_strict_equal", builder_.getInt32Ty(), {ptr, ptr}), {a, b});
    return builder_.CreateICmpNE(equal, builder_.getInt32(0));
  }
  switch (type->kind()) {
  case Type::Kind::String: {
    // Comparing with a literal needs no call when the literal is empty or
    // inline: every one-byte string that short is inline, so equal ones have
    // equal words.
    if (isa<llvm::Constant>(a))
      std::swap(a, b);
    auto *literal = dyn_cast<llvm::Constant>(b);
    if (auto *meta = literal ? llvm::dyn_cast_or_null<llvm::ConstantInt>(literal->getAggregateElement(1u))
                             : nullptr) {
      uint64_t bits = meta->getZExtValue();
      if ((bits & kStrLengthMask) == 0)
        return builder_.CreateICmpEQ(stringLength(a), builder_.getInt64(0));
      if ((bits & (kStrRope | kStrTwoByte | kStrHeap)) == kStrRope)
        return builder_.CreateAnd(builder_.CreateICmpEQ(stringObj(a), stringObj(b)),
                                  builder_.CreateICmpEQ(stringMeta(a), stringMeta(b)));
    }
    llvm::Value *args[] = {stringObj(a), stringMeta(a), stringObj(b), stringMeta(b)};
    return builder_.CreateICmpNE(builder_.CreateCall(rt_.strEqual, args), builder_.getInt32(0));
  }
  case Type::Kind::Array: // the same array
  case Type::Kind::Object: // the same object
  case Type::Kind::Iterator:
  case Type::Kind::Dict:
  case Type::Kind::Bool:
  case Type::Kind::ClassRef:
    return builder_.CreateICmpEQ(a, b);
  case Type::Kind::Unknown: { // as `===`
    llvm::Type *ptr = builder_.getPtrTy();
    llvm::Value *equal = builder_.CreateCall(
        unknownFunction("tz_unknown_equal", builder_.getInt32Ty(), {ptr, ptr}), {a, b});
    return builder_.CreateICmpNE(equal, builder_.getInt32(0));
  }
  case Type::Kind::Decimal: { // the same value: 0.30 == 0.3
    llvm::Type *i64 = builder_.getInt64Ty();
    llvm::Value *equal = builder_.CreateCall(
        decimalFunction("tz_dec_equal", builder_.getInt32Ty(), {i64, i64, i64, i64}),
        {decimalCoef(a), decimalMeta(a), decimalCoef(b), decimalMeta(b)});
    return builder_.CreateICmpNE(equal, builder_.getInt32(0));
  }
  case Type::Kind::Function: // the same function with the same environment
    return builder_.CreateAnd(
        builder_.CreateICmpEQ(builder_.CreateExtractValue(a, 0), builder_.CreateExtractValue(b, 0)),
        builder_.CreateICmpEQ(builder_.CreateExtractValue(a, 1), builder_.CreateExtractValue(b, 1)));
  case Type::Kind::Optional: {
    if (isReference(type->inner()) || isAtomType(type->inner())) // both null, or the same object
      return builder_.CreateICmpEQ(a, b);
    if (type->inner()->isTagged()) // a tag 0 is undefined
      return emitUnionEqual(a, b, type->inner());
    llvm::Value *hasA = builder_.CreateExtractValue(a, 1), *hasB = builder_.CreateExtractValue(b, 1);
    llvm::Value *values = emitValuesEqual(builder_.CreateExtractValue(a, 0),
                                          builder_.CreateExtractValue(b, 0), type->inner());
    // Both undefined, or both defined and equal. The zero values of undefined
    // are valid, so comparing them is harmless.
    llvm::Value *both = builder_.CreateAnd(hasA, hasB);
    llvm::Value *neither = builder_.CreateNot(builder_.CreateOr(hasA, hasB));
    return builder_.CreateOr(neither, builder_.CreateAnd(both, values));
  }
  default:
    return emitComparison(BinaryOp::Eq, a, b, type);
  }
}

// `==` and `!=`. Sema converted both sides to one type, unless one of them
// is `undefined`.
llvm::Value *CodeGen::emitEquality(BinaryExpr *bin) {
  bool isEq = bin->op == BinaryOp::Eq;
  Expr *lhs = bin->lhs, *rhs = bin->rhs;
  if (lhs->type->nonOptional()->isJson() || rhs->type->nonOptional()->isJson())
    return emitJsonEquality(bin);
  if (lhs->type->isUndefined() || rhs->type->isUndefined()) {
    // A side that is always undefined, such as `void f()`, still runs, in
    // its turn.
    if (lhs->type->isUndefined())
      emitDiscarded(lhs);
    Expr *other = lhs->type->isUndefined() ? rhs : lhs;
    if (other->type->isUndefined()) {
      emitDiscarded(rhs);
      return builder_.getInt1(isEq);
    }
    Operand value = emitOperand(other, /*canBorrow=*/other == rhs || cannotAssign(rhs));
    if (rhs->type->isUndefined())
      emitDiscarded(rhs);
    llvm::Value *has = other->type->isOptional() || other->type->isUnknown()
                           ? optionalHas(value.value, other->type)
                           : builder_.getTrue();
    dropOperand(value);
    return isEq ? builder_.CreateNot(has) : has;
  }
  if (lhs->type->isString() && !lhs->type->isOptional())
    return emitStringComparison(bin);

  Operand a = emitOperand(lhs, cannotAssign(rhs));
  holdOperand(a, exprThrows(rhs));
  Operand b = emitOperand(rhs, /*canBorrow=*/true);
  takeOperand(a);
  llvm::Value *equal = emitValuesEqual(a.value, b.value, lhs->type);
  dropOperand(a);
  dropOperand(b);
  return isEq ? equal : builder_.CreateNot(equal);
}

// `a ?? b`: b is evaluated only when a is undefined.
llvm::Value *CodeGen::emitNullish(BinaryExpr *bin) {
  const Type *lhsType = bin->lhs->type;
  if (lhsType->nonOptional()->isJson())
    return emitJsonNullish(bin);
  if (lhsType->isUndefined()) {
    emitDiscarded(bin->lhs); // `void f() ?? x` still runs f
    return emitExpr(bin->rhs);
  }
  if (!lhsType->isOptional() && !lhsType->isUnknown()) // never undefined
    return emitExpr(bin->lhs);

  llvm::Value *lhs = emitExpr(bin->lhs);
  llvm::BasicBlock *valueBlock = newBlock("nullish.value");
  llvm::BasicBlock *defaultBlock = newBlock("nullish.default");
  llvm::BasicBlock *endBlock = newBlock("nullish.end");
  builder_.CreateCondBr(optionalHas(lhs, lhsType), valueBlock, defaultBlock);

  switchTo(valueBlock);
  llvm::Value *value = bin->type->isOptional() ? lhs : optionalValue(lhs, lhsType);
  llvm::BasicBlock *valueEnd = builder_.GetInsertBlock();
  builder_.CreateBr(endBlock);

  switchTo(defaultBlock); // lhs is undefined and holds nothing to release
  llvm::Value *fallback = emitExpr(bin->rhs);
  llvm::BasicBlock *defaultEnd = builder_.GetInsertBlock();
  builder_.CreateBr(endBlock);

  switchTo(endBlock);
  llvm::PHINode *phi = builder_.CreatePHI(lower(bin->type), 2);
  phi->addIncoming(value, valueEnd);
  phi->addIncoming(fallback, defaultEnd);
  return phi;
}

llvm::Value *CodeGen::emitArithmetic(BinaryOp op, llvm::Value *lhs, llvm::Value *rhs,
                                     const Type *type) {
  bool isFloat = type->isFloat();
  bool bitwise = op == BinaryOp::BitAnd || op == BinaryOp::BitOr || op == BinaryOp::BitXor ||
                 op == BinaryOp::Shl || op == BinaryOp::Shr || op == BinaryOp::UShr;
  if (isFloat && bitwise) {
    // `x |= v` where x is a double: on 32-bit integers, as in JavaScript (Sema
    // made v an int), and the result goes back as a double; that of `>>>=`
    // is unsigned.
    llvm::Value *result =
        emitArithmetic(op, builder_.CreateCall(rt_.toInt32, {lhs}), rhs, Type::intTy());
    return op == BinaryOp::UShr ? builder_.CreateUIToFP(result, lhs->getType())
                                : builder_.CreateSIToFP(result, lhs->getType());
  }
  if (bitwise && lhs->getType() != rhs->getType()) {
    // `x |= v` where x is a long and v an int: on 32-bit integers too (Sema
    // left v an int), and the result goes back as a long.
    llvm::Value *result =
        emitArithmetic(op, builder_.CreateTrunc(lhs, rhs->getType()), rhs, Type::intTy());
    return builder_.CreateSExt(result, lhs->getType());
  }
  switch (op) {
  // Integer arithmetic wraps around on overflow, as in Go.
  case BinaryOp::Add:
    return isFloat ? builder_.CreateFAdd(lhs, rhs) : builder_.CreateAdd(lhs, rhs);
  case BinaryOp::Sub:
    return isFloat ? builder_.CreateFSub(lhs, rhs) : builder_.CreateSub(lhs, rhs);
  case BinaryOp::Mul:
    return isFloat ? builder_.CreateFMul(lhs, rhs) : builder_.CreateMul(lhs, rhs);
  case BinaryOp::Div:
    return isFloat ? builder_.CreateFDiv(lhs, rhs) : emitIntDivRem(op, lhs, rhs);
  case BinaryOp::Rem:
    return isFloat ? builder_.CreateFRem(lhs, rhs) : emitIntDivRem(op, lhs, rhs);
  case BinaryOp::Pow: {
    if (isFloat)
      return builder_.CreateCall(rt_.mathPow, {lhs, rhs});
    // Integers: computed as longs, which wrap around like repeated `*`; the
    // low half of that is the int result.
    llvm::Type *i64 = builder_.getInt64Ty();
    llvm::Value *result = builder_.CreateCall(
        rt_.mathPowLong, {builder_.CreateSExt(lhs, i64), builder_.CreateSExt(rhs, i64)});
    emitThrowCheck(); // 0 ** -1
    return builder_.CreateTrunc(result, lhs->getType());
  }
  case BinaryOp::BitAnd:
    return builder_.CreateAnd(lhs, rhs);
  case BinaryOp::BitOr:
    return builder_.CreateOr(lhs, rhs);
  case BinaryOp::BitXor:
    return builder_.CreateXor(lhs, rhs);
  case BinaryOp::Shl:
  case BinaryOp::Shr:
  case BinaryOp::UShr: {
    // As in JavaScript, only the low bits of the count are used. This also
    // keeps the shift defined for any count.
    llvm::Value *mask = llvm::ConstantInt::get(lhs->getType(), type->bitWidth() - 1);
    llvm::Value *count = builder_.CreateAnd(rhs, mask);
    if (op == BinaryOp::Shl)
      return builder_.CreateShl(lhs, count);
    if (op == BinaryOp::Shr)
      return builder_.CreateAShr(lhs, count);
    return builder_.CreateLShr(lhs, count);
  }
  default:
    break;
  }
  llvm_unreachable("not an arithmetic operator");
}

llvm::Value *CodeGen::emitComparison(BinaryOp op, llvm::Value *lhs, llvm::Value *rhs,
                                     const Type *type) {
  if (type->isFloat()) {
    switch (op) {
    case BinaryOp::Lt: return builder_.CreateFCmpOLT(lhs, rhs);
    case BinaryOp::Le: return builder_.CreateFCmpOLE(lhs, rhs);
    case BinaryOp::Gt: return builder_.CreateFCmpOGT(lhs, rhs);
    case BinaryOp::Ge: return builder_.CreateFCmpOGE(lhs, rhs);
    case BinaryOp::Eq: return builder_.CreateFCmpOEQ(lhs, rhs);
    case BinaryOp::Ne: return builder_.CreateFCmpUNE(lhs, rhs); // NaN != NaN
    default: break;
    }
  } else {
    switch (op) {
    case BinaryOp::Lt: return builder_.CreateICmpSLT(lhs, rhs);
    case BinaryOp::Le: return builder_.CreateICmpSLE(lhs, rhs);
    case BinaryOp::Gt: return builder_.CreateICmpSGT(lhs, rhs);
    case BinaryOp::Ge: return builder_.CreateICmpSGE(lhs, rhs);
    case BinaryOp::Eq: return builder_.CreateICmpEQ(lhs, rhs);
    case BinaryOp::Ne: return builder_.CreateICmpNE(lhs, rhs);
    default: break;
    }
  }
  llvm_unreachable("not a comparison operator");
}

// Dividing by zero throws RangeError, as JavaScript's `1n / 0n`. MIN / -1
// wraps around to MIN (and MIN % -1 is 0) where the machine instruction
// would trap.
llvm::Value *CodeGen::emitIntDivRem(BinaryOp op, llvm::Value *lhs, llvm::Value *rhs) {
  bool isRem = op == BinaryOp::Rem;
  if (auto *divisor = dyn_cast<llvm::ConstantInt>(rhs);
      divisor && !divisor->isZero() && !divisor->isMinusOne())
    return isRem ? builder_.CreateSRem(lhs, rhs) : builder_.CreateSDiv(lhs, rhs);

  llvm::Type *type = lhs->getType();
  llvm::Value *isZero = builder_.CreateICmpEQ(rhs, llvm::ConstantInt::get(type, 0));
  llvm::BasicBlock *throwBlock = newBlock("div.zero");
  llvm::BasicBlock *okBlock = newBlock("div.ok");
  builder_.CreateCondBr(isZero, throwBlock, okBlock,
                        llvm::MDBuilder(ctx_).createUnlikelyBranchWeights());
  switchTo(throwBlock);
  emitThrowNew(2, "Division by zero");

  switchTo(okBlock);
  llvm::Value *isMinusOne = builder_.CreateICmpEQ(rhs, llvm::ConstantInt::getSigned(type, -1));
  llvm::Value *divisor = builder_.CreateSelect(isMinusOne, llvm::ConstantInt::get(type, 1), rhs);
  if (isRem)
    return builder_.CreateSRem(lhs, divisor); // x % 1 == x % -1 == 0
  llvm::Value *quotient = builder_.CreateSDiv(lhs, divisor);
  return builder_.CreateSelect(isMinusOne, builder_.CreateNeg(lhs), quotient);
}

llvm::Value *CodeGen::emitAssign(AssignExpr *assign, bool needResult) {
  if (assign->op && *assign->op == BinaryOp::Nullish)
    return emitNullishAssign(assign, needResult);
  if (auto *index = dyn_cast<IndexExpr>(assign->target);
      index && index->object->type->nonOptional()->isJson())
    return emitJsonAssign(assign, needResult);
  if (auto *member = dyn_cast<MemberExpr>(assign->target);
      member && member->object->type->nonOptional()->isJson())
    return emitJsonAssign(assign, needResult);
  if (auto *index = dyn_cast<IndexExpr>(assign->target); index && index->keyed)
    return emitFieldAssign(assign, needResult);
  if (auto *index = dyn_cast<IndexExpr>(assign->target);
      index && index->object->type->isTypedArray())
    return emitBytesElementAssign(assign, needResult);
  if (isa<IndexExpr>(assign->target))
    return emitElementAssign(assign, needResult);
  if (auto *member = dyn_cast<MemberExpr>(assign->target))
    return member->object->type->isObject() || member->object->type->isUnion()
               ? emitFieldAssign(assign, needResult)
                                            : emitLengthAssign(assign, needResult);

  const VarSymbol *var = cast<NameExpr>(assign->target)->var;
  llvm::Value *address = addressOf(var);
  const Type *type = assign->type;
  llvm::Value *value;
  if (!assign->op) {
    value = emitExpr(assign->value);
  } else if (type->isString()) {
    // `s += x`: the variable's own value is borrowed unless evaluating `x`
    // could reassign the variable.
    Operand old{builder_.CreateLoad(strTy_, address), false, type};
    if (!cannotAssign(assign->value)) {
      retainString(old.value);
      old.owned = true;
    }
    Operand rhs = emitConcatPart(assign->value, /*canBorrow=*/false);
    value = concatOperands({old, rhs});
  } else if (type->isDecimal()) {
    // As for strings, the old value is borrowed unless evaluating the right
    // side could reassign the variable.
    llvm::Value *old = builder_.CreateLoad(decTy_, address);
    bool hold = !cannotAssign(assign->value);
    if (hold)
      retainValue(old, type);
    value = emitDecimalCompound(*assign->op, old, assign->value);
    if (hold)
      releaseValue(old, type);
  } else {
    // As in JavaScript, the old value is read before the right side runs.
    llvm::Value *old = builder_.CreateLoad(lower(type), address);
    llvm::Value *rhs = emitExpr(assign->value);
    value = emitArithmetic(*assign->op, old, rhs, type);
  }
  storeVar(var, address, value, /*replacing=*/true);
  if (!needResult)
    return nullptr;
  retainValue(value, type);
  return value;
}

// `x ??= v`
llvm::Value *CodeGen::emitNullishAssign(AssignExpr *assign, bool needResult) {
  const VarSymbol *var = cast<NameExpr>(assign->target)->var;
  const Type *varType = var->type;
  llvm::Value *address = addressOf(var);
  llvm::Value *old = builder_.CreateLoad(lower(varType), address);
  llvm::BasicBlock *keepBlock = newBlock("nullish.keep");
  llvm::BasicBlock *assignBlock = newBlock("nullish.assign");
  llvm::BasicBlock *endBlock = newBlock("nullish.end");
  // A Json that holds null is replaced too, as in JavaScript.
  llvm::Value *has =
      varType->nonOptional()->isJson() ? jsonPresent(old) : optionalHas(old, varType);
  builder_.CreateCondBr(has, keepBlock, assignBlock);

  const Type *resultType = assign->type;
  switchTo(keepBlock);
  llvm::Value *kept = nullptr;
  if (needResult) {
    retainValue(old, varType);
    kept = resultType->isOptional() ? old : optionalValue(old, varType);
  }
  llvm::BasicBlock *keepEnd = builder_.GetInsertBlock();
  builder_.CreateBr(endBlock);

  switchTo(assignBlock);
  llvm::Value *value = emitExpr(assign->value);
  const Type *valueType = assign->value->type;
  llvm::Value *stored = valueType->isOptional() ? value : makeOptional(value, builder_.getTrue(), varType);
  storeVar(var, address, stored, /*replacing=*/true);
  llvm::Value *assigned = nullptr;
  if (needResult) {
    retainValue(stored, varType);
    assigned = resultType->isOptional() ? stored : optionalValue(stored, varType);
  }
  llvm::BasicBlock *assignEnd = builder_.GetInsertBlock();
  builder_.CreateBr(endBlock);

  switchTo(endBlock);
  if (!needResult)
    return nullptr;
  llvm::PHINode *phi = builder_.CreatePHI(lower(resultType), 2);
  phi->addIncoming(kept, keepEnd);
  phi->addIncoming(assigned, assignEnd);
  return phi;
}

llvm::Value *CodeGen::emitUpdate(UpdateExpr *update) {
  if (auto *index = dyn_cast<IndexExpr>(update->target);
      index && index->object->type->isTypedArray())
    return emitBytesUpdate(update);
  llvm::Value *address;
  Operand holder{nullptr, false, nullptr};
  if (auto *keyed = dyn_cast<IndexExpr>(update->target); keyed && keyed->keyed) {
    // `o[k]++` with a key of literals.
    holder = emitOperand(keyed->object, cannotAssign(keyed->index));
    address = keyedFieldAddress(holder.value, keyed->object->type, emitExpr(keyed->index),
                                keyed->index->type);
  } else if (auto *index = dyn_cast<IndexExpr>(update->target)) {
    // `a[i]++`: Sema proved the index in range.
    holder = emitOperand(index->object, cannotAssign(index->index));
    llvm::Value *position = emitExpr(index->index);
    address = elementAddress(arrayData(holder.value), update->type, position);
  } else if (auto *member = dyn_cast<MemberExpr>(update->target)) {
    // `o.count++`: the object holds the number.
    holder = emitOperand(member->object, /*canBorrow=*/true);
    address = fieldAddress(holder.value, member->object->type, member->name);
  } else {
    address = addressOf(cast<NameExpr>(update->target)->var);
  }
  if (update->type->isDecimal()) {
    llvm::Value *result = emitDecimalUpdate(update, address);
    if (holder.value)
      dropOperand(holder);
    return result;
  }
  llvm::Type *type = lower(update->type);
  llvm::Value *old = builder_.CreateLoad(type, address);
  llvm::Value *updated;
  if (update->type->isFloat()) {
    llvm::Value *one = llvm::ConstantFP::get(type, 1.0);
    updated = update->increment ? builder_.CreateFAdd(old, one) : builder_.CreateFSub(old, one);
  } else {
    llvm::Value *one = llvm::ConstantInt::get(type, 1);
    updated = update->increment ? builder_.CreateAdd(old, one) : builder_.CreateSub(old, one);
  }
  builder_.CreateStore(updated, address);
  if (holder.value)
    dropOperand(holder);
  return update->prefix ? updated : old;
}

llvm::Value *CodeGen::emitCall(CallExpr *call) {
  switch (call->builtin) {
  case Builtin::ConsoleLog:
  case Builtin::ConsoleError:
    emitConsoleLog(call);
    return nullptr;
  case Builtin::StringOf:
    // Sema already converted the argument to a string.
    return call->args.empty() ? stringLiteral(u"") : emitExpr(call->args[0]);
  case Builtin::NumberOf:
  case Builtin::ParseInt:
  case Builtin::ParseFloat:
  case Builtin::IsNaN:
  case Builtin::IsFinite:
  case Builtin::IsInteger:
  case Builtin::IsSafeInteger:
  case Builtin::FromCharCode:
  case Builtin::FromCodePoint:
  case Builtin::UriEncodeComponent:
  case Builtin::UriEncode:
  case Builtin::UriDecodeComponent:
  case Builtin::UriDecode:
  case Builtin::NumToFixed:
  case Builtin::NumToRadix:
    return emitNumberBuiltin(call);
  case Builtin::None:
    break;
  case Builtin::TupleJoin:
    return emitTupleJoin(call);
  case Builtin::WeakRefNew:
  case Builtin::WeakRefDeref:
    return emitWeakRefCall(call);
  case Builtin::MetaDefine:
  case Builtin::MetaGet:
  case Builtin::MetaGetOwn:
  case Builtin::MetaHas:
  case Builtin::MetaHasOwn:
  case Builtin::MetaDelete:
    return emitMetaCall(call);
  case Builtin::ClassName:
  case Builtin::ReflectGet:
  case Builtin::ReflectApply:
  case Builtin::ReflectConstruct:
  case Builtin::FromJsonClass:
  case Builtin::ReflectMethod:
    return emitReflectCall(call);
  case Builtin::ArrForEach:
  case Builtin::ArrMap:
  case Builtin::ArrFilter:
  case Builtin::ArrFlatMap:
  case Builtin::ArrReduce:
  case Builtin::ArrReduceRight:
  case Builtin::ArrFind:
  case Builtin::ArrFindIndex:
  case Builtin::ArrFindLast:
  case Builtin::ArrFindLastIndex:
  case Builtin::ArrSome:
  case Builtin::ArrEvery:
  case Builtin::ArrSortWith:
    return emitCallbackMethod(call);
  case Builtin::ObjectKeys:
  case Builtin::ObjectValues:
  case Builtin::ObjectEntries:
    return emitObjectCall(call);
  case Builtin::ArrKeys:
  case Builtin::ArrValues:
  case Builtin::ArrEntries:
  case Builtin::IterFrom:
  case Builtin::ArrayFrom:
  case Builtin::IterNext:
  case Builtin::IterMap:
  case Builtin::IterFilter:
  case Builtin::IterTake:
  case Builtin::IterDrop:
  case Builtin::IterFlatMap:
  case Builtin::IterToArray:
  case Builtin::IterForEach:
  case Builtin::IterReduce:
  case Builtin::IterSome:
  case Builtin::IterEvery:
  case Builtin::IterFind:
    return emitIteratorBuiltin(call);
  default:
    if (isLibBuiltin(call->builtin))
      return emitLibBuiltin(call);
    if (isCryptoBuiltin(call->builtin))
      return emitCryptoCall(call);
    if (isAsyncBuiltin(call->builtin))
      return emitAsyncBuiltin(call);
    if (isDateBuiltin(call->builtin))
      return emitDateCall(call);
    if (isIntlBuiltin(call->builtin))
      return emitIntlCall(call);
    if (isDictBuiltin(call->builtin))
      return emitDictCall(call);
    if (isBytesBuiltin(call->builtin))
      return emitBytesCall(call);
    if (isRegexBuiltin(call->builtin))
      return emitRegexCall(call);
    if (isJsonBuiltin(call->builtin))
      return emitJsonCall(call);
    if (isProcessBuiltin(call->builtin))
      return emitProcessBuiltin(call);
    if (isNodeBuiltin(call->builtin))
      return emitNodeBuiltin(call);
    if (isChildBuiltin(call->builtin))
      return emitChildBuiltin(call);
    if (isMathBuiltin(call->builtin))
      return emitMathCall(call);
    if (isDecimalBuiltin(call->builtin))
      return emitDecimalCall(call);
    if (isResultBuiltin(call->builtin))
      return emitResultCall(call);
    if (call->builtin == Builtin::ErrorStack || call->builtin == Builtin::ErrorToString)
      return emitErrorBuiltin(call);
    if (cast<MemberExpr>(call->callee)->object->type->isArray())
      return emitArrayMethod(call);
    return emitStringMethod(call);
  }

  if (call->superCall && !isa<MemberExpr>(call->callee))
    return emitSuperCall(call);
  if (call->method)
    return emitMethodCall(call);
  if (!call->unionMethods.empty())
    return emitUnionMethodCall(call);
  if (!call->func)
    return emitValueCall(call);
  // The callee takes ownership of its arguments. Parameters left out get
  // undefined (and then their default values).
  const FuncSymbol *func = call->func;
  std::vector<llvm::Value *> args = emitHeldValues(call->args);
  for (size_t i = args.size(); i < func->params.size(); ++i)
    args.push_back(undefinedValue(func->params[i]));
  llvm::CallInst *result;
  if (func->decl->isClosure()) {
    // A function declared in a block, or a constant arrow: its code is
    // known, its environment comes from where it is called.
    args.insert(args.begin(), closureEnv(call));
    result = builder_.CreateCall(closureCode(func->decl), args);
  } else {
    result = builder_.CreateCall(functions_.lookup(func), args);
  }
  emitThrowCheckIf(mayThrow(func->decl) && call != returnCall_);
  return result->getType()->isVoidTy() ? nullptr : result;
}

// console.log(a, b) prints its arguments separated by spaces, then a newline.
// Strings print as they are; arrays and optionals the way Node.js shows them.
// As in JavaScript, all arguments are evaluated before anything is printed:
// `console.log(a.pop(), a)` shows the array without the popped element.
// console.error prints the same way to stderr (see tz_err_begin in
// runtime/tzrt.c).
void CodeGen::emitConsoleLog(CallExpr *call) {
  size_t count = call->args.size();
  std::vector<bool> laterCannotAssign(count, true);
  for (size_t i = count; i > 1; --i)
    laterCannotAssign[i - 2] = laterCannotAssign[i - 1] && cannotAssign(call->args[i - 1]);
  std::vector<bool> laterThrows(count, false);
  for (size_t i = count; i > 1; --i)
    laterThrows[i - 2] = laterThrows[i - 1] || exprThrows(call->args[i - 1]);
  std::vector<Operand> values;
  for (size_t i = 0; i < count; ++i) {
    if (call->args[i]->type->isUndefined()) {
      emitDiscarded(call->args[i]); // `void f()` still runs f
      values.push_back({nullptr, false, call->args[i]->type});
    } else {
      values.push_back(emitOperand(call->args[i], laterCannotAssign[i]));
      holdOperand(values.back(), laterThrows[i]);
    }
  }
  for (Operand &value : values)
    takeOperand(value);

  bool toStderr = call->builtin == Builtin::ConsoleError;
  if (toStderr)
    builder_.CreateCall(processFunction("tz_err_begin", builder_.getVoidTy(), {}));
  auto print = [&](llvm::Value *value, const Type *type) {
    if (type->isUndefined() || type->isNever()) {
      llvm::Value *text = stringLiteral(u"undefined");
      builder_.CreateCall(rt_.printString, {stringObj(text), stringMeta(text)});
      return;
    }
    switch (type->kind()) {
    case Type::Kind::String:
      builder_.CreateCall(rt_.printString, {stringObj(value), stringMeta(value)});
      break;
    case Type::Kind::Int:
      builder_.CreateCall(rt_.printI32, {value});
      break;
    case Type::Kind::Long:
      builder_.CreateCall(rt_.printI64, {value});
      break;
    case Type::Kind::Double:
      builder_.CreateCall(rt_.printF64, {value});
      break;
    case Type::Kind::Bool:
      builder_.CreateCall(rt_.printBool, {builder_.CreateZExt(value, builder_.getInt32Ty())});
      break;
    case Type::Kind::Decimal: {
      llvm::Type *i64 = builder_.getInt64Ty();
      builder_.CreateCall(decimalFunction("tz_print_decimal", builder_.getVoidTy(), {i64, i64}),
                          {decimalCoef(value), decimalMeta(value)});
      break;
    }
    case Type::Kind::Array:
    case Type::Kind::Optional:
    case Type::Kind::Function:
    case Type::Kind::Object:
    case Type::Kind::Result:
    case Type::Kind::Iterator:
    case Type::Kind::Json:
    case Type::Kind::Dict:
    case Type::Kind::Literal:
    case Type::Kind::Union:
    case Type::Kind::Promise:
    case Type::Kind::Unknown:
    case Type::Kind::ClassRef:
      builder_.CreateCall(rt_.printValue, {typeDescriptor(type), spill(value, "print.value")});
      break;
    default:
      llvm_unreachable("Sema rejects other argument types");
    }
  };
  // `console.log(...xs)` prints the elements as arguments of their own: a
  // space goes before each value but the first printed.
  llvm::Value *first = nullptr;
  if (call->hasSpread()) {
    first = createStackSlot(builder_.getInt1Ty(), "print.first");
    builder_.CreateStore(builder_.getTrue(), first);
  }
  auto separate = [&](size_t i) {
    if (!first) {
      if (i > 0)
        builder_.CreateCall(rt_.printChar, {builder_.getInt32(' ')});
      return;
    }
    llvm::BasicBlock *spaceBlock = newBlock("print.space");
    llvm::BasicBlock *valueBlock = newBlock("print.value");
    builder_.CreateCondBr(builder_.CreateLoad(builder_.getInt1Ty(), first), valueBlock, spaceBlock);
    switchTo(spaceBlock);
    builder_.CreateCall(rt_.printChar, {builder_.getInt32(' ')});
    builder_.CreateBr(valueBlock);
    switchTo(valueBlock);
    builder_.CreateStore(builder_.getFalse(), first);
  };
  for (size_t i = 0; i < count; ++i) {
    const Operand &value = values[i];
    if (call->spreads(i) && value.type->isArray()) {
      // Each element, in a loop.
      const Type *element = value.type->inner();
      llvm::Value *length = arrayLength(value.value);
      llvm::Value *indexSlot = createStackSlot(builder_.getInt64Ty(), "print.index");
      builder_.CreateStore(builder_.getInt64(0), indexSlot);
      llvm::BasicBlock *condBlock = newBlock("print.cond");
      llvm::BasicBlock *bodyBlock = newBlock("print.body");
      llvm::BasicBlock *endBlock = newBlock("print.end");
      builder_.CreateBr(condBlock);
      switchTo(condBlock);
      llvm::Value *index = builder_.CreateLoad(builder_.getInt64Ty(), indexSlot);
      builder_.CreateCondBr(builder_.CreateICmpULT(index, length), bodyBlock, endBlock);
      switchTo(bodyBlock);
      separate(i);
      llvm::Value *at = builder_.CreateLoad(builder_.getInt64Ty(), indexSlot);
      llvm::Value *item =
          builder_.CreateLoad(lower(element), elementAddress(arrayData(value.value), element, at), "item");
      print(item, element);
      builder_.CreateStore(builder_.CreateAdd(at, builder_.getInt64(1)), indexSlot);
      builder_.CreateBr(condBlock);
      switchTo(endBlock);
      dropOperand(value);
      continue;
    }
    separate(i);
    print(value.value, value.type);
    dropOperand(value);
  }
  builder_.CreateCall(rt_.printChar, {builder_.getInt32('\n')});
  if (toStderr)
    builder_.CreateCall(processFunction("tz_err_end", builder_.getVoidTy(), {}));
}

llvm::Value *CodeGen::emitConditional(ConditionalExpr *cond) {
  llvm::Value *test = emitExpr(cond->cond);
  llvm::BasicBlock *thenBlock = newBlock("cond.then");
  llvm::BasicBlock *elseBlock = newBlock("cond.else");
  llvm::BasicBlock *endBlock = newBlock("cond.end");
  builder_.CreateCondBr(test, thenBlock, elseBlock);

  switchTo(thenBlock);
  llvm::Value *thenValue = emitExpr(cond->thenExpr);
  llvm::BasicBlock *thenEnd = builder_.GetInsertBlock();
  builder_.CreateBr(endBlock);

  switchTo(elseBlock);
  llvm::Value *elseValue = emitExpr(cond->elseExpr);
  llvm::BasicBlock *elseEnd = builder_.GetInsertBlock();
  builder_.CreateBr(endBlock);

  switchTo(endBlock);
  if (cond->type->isVoid())
    return nullptr;
  llvm::PHINode *phi = builder_.CreatePHI(lower(cond->type), 2);
  phi->addIncoming(thenValue, thenEnd);
  phi->addIncoming(elseValue, elseEnd);
  return phi;
}

llvm::Value *CodeGen::emitConversion(llvm::Value *value, const Type *from, const Type *to) {
  if (from == to)
    return value;
  // A string of a pattern (Type::stringPattern) is a string as it is.
  if (from->isString() && to->isString())
    return value;
  // A value that cannot exist, in code that never runs: the callback of
  // mapErr on a Result that cannot be an Err.
  if (from->isNever())
    return llvm::Constant::getNullValue(lowerHalf(to));
  if (from->isUnknown() || to->isUnknown())
    return emitUnknownConversion(value, from, to);
  // A variable narrowed to never (every member of its union ruled out), in
  // code that never runs.
  if (to->isNever()) {
    releaseValue(value, from);
    return llvm::Constant::getNullValue(lower(to));
  }
  // A number or boolean literal type is a value of its base.
  from = storedType(from);
  to = storedType(to);
  if (from == to)
    return value;
  auto unionLike = [](const Type *t) {
    t = t->nonOptional();
    return t->isUnion() || t->isStringLiteral();
  };
  if (unionLike(from) || unionLike(to))
    return emitUnionConversion(value, from, to);
  if ((from->nonOptional()->isJson() || to->nonOptional()->isJson()) &&
      from->nonOptional() != to->nonOptional())
    return emitJsonConversion(value, from, to);
  if (from->isResult() && to->isResult())
    return makeResult(to, resultIsOk(value, from),
                      convertHalf(resultHalf(value, from, false), from->okType(), to->okType()),
                      convertHalf(resultHalf(value, from, true), from->errType(), to->errType()));

  if (to->isString()) {
    if (from->isBool())
      return builder_.CreateSelect(value, stringLiteral(u"true"), stringLiteral(u"false"));
    if (from->isInteger())
      return callReturningString(rt_.strFromI64,
                                 {builder_.CreateSExt(value, builder_.getInt64Ty())});
    if (from->isFloat())
      return callReturningString(rt_.strFromF64, {value});
    if (from->isUndefined())
      return stringLiteral(u"undefined");
    if (from->isDecimal()) {
      llvm::Type *i64 = builder_.getInt64Ty(), *ptr = builder_.getPtrTy();
      llvm::Value *text = callReturningString(
          decimalFunction("tz_dec_to_string", builder_.getVoidTy(), {ptr, i64, i64}),
          {decimalCoef(value), decimalMeta(value)});
      releaseValue(value, from);
      return text;
    }
    // Arrays and optionals: the runtime knows how to show any value.
    llvm::Value *result = emitValueToString(value, from);
    releaseValue(value, from);
    return result;
  }

  if (to->isOptional()) {
    if (from->isUndefined())
      return undefinedValue(to);
    if (from->isOptional()) // numeric widening, which keeps zero zero, or an object
      return makeOptional(emitConversion(optionalValue(value, from), from->inner(), to->inner()),
                          optionalHas(value, from), to);
    return makeOptional(emitConversion(value, from, to->inner()), builder_.getTrue(), to);
  }
  if (from->isOptional()) // narrowed: known to hold a value
    return emitConversion(optionalValue(value, from), from->inner(), to);
  // An object seen through a type with read-only fields, or an iterator
  // through one that drops what it returns; an array of objects as one of a
  // type they are (`Dog[]` as `Animal[]`).
  if (isReference(from) || (from->isArray() && to->isArray()))
    return value;
  if (from->isInteger() && to->isDecimal())
    return decimalFromInteger(value);

  llvm::Type *target = lower(to);
  if (from->isInteger() && to->isInteger())
    return from->bitWidth() < to->bitWidth() ? builder_.CreateSExt(value, target)
                                             : builder_.CreateTrunc(value, target);
  if (from->isInteger() && to->isFloat())
    return builder_.CreateSIToFP(value, target);
  // An operand of a bitwise operator: JavaScript's ToInt32. A double becomes a
  // long only through Math functions (emitMathCall).
  if (from->isFloat() && to->isInt())
    return builder_.CreateCall(rt_.toInt32, {value});
  llvm_unreachable("Sema rejects this conversion");
}

//===----------------------------------------------------------------------===//
// Strings
//===----------------------------------------------------------------------===//

llvm::Value *CodeGen::stringObj(llvm::Value *str) { return builder_.CreateExtractValue(str, 0); }

llvm::Value *CodeGen::stringMeta(llvm::Value *str) { return builder_.CreateExtractValue(str, 1); }

llvm::Value *CodeGen::stringLength(llvm::Value *str) {
  return builder_.CreateAnd(stringMeta(str), builder_.getInt64(kStrLengthMask), "length");
}

bool CodeGen::cannotAssign(const Expr *expr) {
  switch (expr->kind()) {
  case Expr::Kind::IntLit:
  case Expr::Kind::FloatLit:
  case Expr::Kind::BoolLit:
  case Expr::Kind::StringLit:
  case Expr::Kind::RegexLit:
  case Expr::Kind::Undefined:
  case Expr::Kind::Name:
  case Expr::Kind::Function: // creating a closure runs none of its code
    return true;
  case Expr::Kind::Template:
    return llvm::all_of(cast<TemplateExpr>(expr)->exprs, cannotAssign);
  case Expr::Kind::ArrayLit:
    // Spreading an iterator runs a generator or the callbacks of helpers.
    return llvm::all_of(cast<ArrayLitExpr>(expr)->elements, [](const ArrayLitExpr::Element &e) {
      return cannotAssign(e.expr) && !(e.spread && e.expr->type->isIterator());
    });
  case Expr::Kind::ObjectLit:
    return llvm::all_of(cast<ObjectLitExpr>(expr)->props,
                        [](const ObjectLitExpr::Property &p) { return cannotAssign(p.value); });
  case Expr::Kind::Member:
    return cannotAssign(cast<MemberExpr>(expr)->object);
  case Expr::Kind::Index: {
    auto *index = cast<IndexExpr>(expr);
    return cannotAssign(index->object) && cannotAssign(index->index);
  }
  case Expr::Kind::Unary:
    return cannotAssign(cast<UnaryExpr>(expr)->operand);
  case Expr::Kind::Binary: {
    auto *bin = cast<BinaryExpr>(expr);
    return cannotAssign(bin->lhs) && cannotAssign(bin->rhs);
  }
  case Expr::Kind::Conditional: {
    auto *cond = cast<ConditionalExpr>(expr);
    return cannotAssign(cond->cond) && cannotAssign(cond->thenExpr) &&
           cannotAssign(cond->elseExpr);
  }
  case Expr::Kind::Cast:
    return cannotAssign(cast<CastExpr>(expr)->operand);
  case Expr::Kind::Sequence:
    return llvm::all_of(cast<SequenceExpr>(expr)->exprs, cannotAssign);
  case Expr::Kind::OptionalChain:
    return cannotAssign(cast<OptionalChainExpr>(expr)->chain);
  case Expr::Kind::New: // a constructor runs code
    return !cast<NewExpr>(expr)->cls && !cast<NewExpr>(expr)->reflect &&
           llvm::all_of(cast<NewExpr>(expr)->args, cannotAssign);
  case Expr::Kind::Call: {
    // A user function may assign to globals and fields, and so may the
    // callback of a built-in, and a generator that next() or a consumer of
    // an iterator runs; other built-ins only evaluate their operands (array
    // methods change arrays, not variables).
    auto *call = cast<CallExpr>(expr);
    if (call->builtin == Builtin::None ||
        (call->builtin >= Builtin::ArrForEach && call->builtin <= Builtin::ArrSortWith) ||
        isResultCallback(call->builtin) || call->builtin == Builtin::SafeTry ||
        call->builtin == Builtin::PromiseNew || // runs its executor
        call->builtin == Builtin::ResultAsyncMap || // asyncMap of a Result runs its function
        call->builtin == Builtin::ResultAsyncAndThen ||
        call->builtin == Builtin::IterNext || call->builtin == Builtin::ArrayFrom ||
        isIteratorSink(call->builtin) || call->builtin == Builtin::StrReplaceRegexFn ||
        call->builtin == Builtin::MapForEach || call->builtin == Builtin::DictGroupBy ||
        call->builtin == Builtin::DictNew ||
        (call->builtin == Builtin::BytesFromArray && call->args[0]->type->isIterator()))
      return false;
    if (!llvm::all_of(call->args, cannotAssign))
      return false;
    auto *member = dyn_cast<MemberExpr>(call->callee);
    return !member || call->builtin == Builtin::ConsoleLog || isMathBuiltin(call->builtin) ||
           cannotAssign(member->object);
  }
  case Expr::Kind::Assign:
  case Expr::Kind::Update:
  case Expr::Kind::Yield: // whoever iterates runs any code
  case Expr::Kind::Await: // so do the other tasks meanwhile
  case Expr::Kind::Class: // its static initializers run
    return false;
  }
  return false;
}

llvm::Value *CodeGen::callReturningString(llvm::FunctionCallee fn,
                                          llvm::ArrayRef<llvm::Value *> args) {
  llvm::AllocaInst *out = createStackSlot(strTy_, "str.out");
  std::vector<llvm::Value *> all{out};
  all.insert(all.end(), args.begin(), args.end());
  builder_.CreateCall(fn, all);
  return builder_.CreateLoad(strTy_, out);
}

llvm::Value *CodeGen::concatOperands(llvm::ArrayRef<Operand> operands) {
  auto *arrayTy = llvm::ArrayType::get(strTy_, operands.size());
  llvm::AllocaInst *array = createStackSlot(arrayTy, "concat.parts");
  for (size_t i = 0; i < operands.size(); ++i)
    builder_.CreateStore(operands[i].value,
                         builder_.CreateConstInBoundsGEP2_32(arrayTy, array, 0, static_cast<unsigned>(i)));
  llvm::Value *result =
      callReturningString(rt_.strConcatN, {array, builder_.getInt64(operands.size())});
  for (const Operand &operand : operands)
    dropOperand(operand);
  return result;
}

llvm::Value *CodeGen::emitConcat(BinaryExpr *bin) {
  std::vector<Expr *> parts;
  collectConcat(bin, parts);
  // A part can be borrowed if nothing after it can reassign its variable.
  std::vector<bool> laterCannotAssign(parts.size(), true);
  for (size_t i = parts.size() - 1; i > 0; --i)
    laterCannotAssign[i - 1] = laterCannotAssign[i] && cannotAssign(parts[i]);

  std::vector<bool> laterThrows(parts.size(), false);
  for (size_t i = parts.size() - 1; i > 0; --i)
    laterThrows[i - 1] = laterThrows[i] || exprThrows(parts[i]);
  std::vector<Operand> operands;
  for (size_t i = 0; i < parts.size(); ++i) {
    operands.push_back(emitConcatPart(parts[i], laterCannotAssign[i]));
    holdOperand(operands.back(), laterThrows[i]);
  }
  for (Operand &operand : operands)
    takeOperand(operand);
  return concatOperands(operands);
}

// A part of a concatenation. A number converted to a string goes to the
// runtime as it is, which writes its digits straight into the result instead
// of making a string for them first.
CodeGen::Operand CodeGen::emitConcatPart(Expr *expr, bool canBorrow) {
  auto *conv = dyn_cast<CastExpr>(expr);
  const Type *from = conv && conv->isImplicit() ? conv->operand->type : nullptr;
  if (!from || !(from->isInteger() || from->isFloat()))
    return emitOperand(expr, canBorrow);
  llvm::Value *value = emitExpr(conv->operand);
  llvm::Value *bits = from->isFloat() ? builder_.CreateBitCast(value, builder_.getInt64Ty())
                                      : builder_.CreateSExt(value, builder_.getInt64Ty());
  llvm::Value *part = llvm::UndefValue::get(strTy_);
  part = builder_.CreateInsertValue(part, builder_.CreateIntToPtr(bits, builder_.getPtrTy()), 0);
  part = builder_.CreateInsertValue(
      part, builder_.getInt64(kNumberPart | (from->isFloat() ? kNumberDouble : kNumberInt)), 1);
  return {part, /*owned=*/false, Type::string()};
}

llvm::Value *CodeGen::emitTemplate(TemplateExpr *tmpl) {
  size_t count = tmpl->exprs.size();
  std::vector<bool> laterCannotAssign(count, true);
  for (size_t i = count; i > 1; --i)
    laterCannotAssign[i - 2] = laterCannotAssign[i - 1] && cannotAssign(tmpl->exprs[i - 1]);

  // `${x}` alone is just x as a string.
  if (count == 1 && tmpl->quasis[0].empty() && tmpl->quasis[1].empty())
    return emitExpr(tmpl->exprs[0]);
  std::vector<bool> laterThrows(count, false);
  for (size_t i = count; i > 1; --i)
    laterThrows[i - 2] = laterThrows[i - 1] || exprThrows(tmpl->exprs[i - 1]);
  std::vector<Operand> operands;
  for (size_t i = 0; i <= count; ++i) {
    if (!tmpl->quasis[i].empty())
      operands.push_back({stringLiteral(tmpl->quasis[i]), false, Type::string()});
    if (i < count) {
      operands.push_back(emitConcatPart(tmpl->exprs[i], laterCannotAssign[i]));
      holdOperand(operands.back(), laterThrows[i]);
    }
  }
  for (Operand &operand : operands)
    takeOperand(operand);
  if (operands.empty())
    return stringLiteral(u"");
  if (operands.size() == 1) // a template without substitutions
    return operands[0].value;
  return concatOperands(operands);
}

llvm::Value *CodeGen::emitStringComparison(BinaryExpr *bin) {
  Operand lhs = emitOperand(bin->lhs, cannotAssign(bin->rhs));
  holdOperand(lhs, exprThrows(bin->rhs));
  Operand rhs = emitOperand(bin->rhs, /*canBorrow=*/true);
  takeOperand(lhs);
  llvm::Value *args[] = {stringObj(lhs.value), stringMeta(lhs.value), stringObj(rhs.value),
                         stringMeta(rhs.value)};
  llvm::Value *zero = builder_.getInt32(0);
  llvm::Value *result;
  switch (bin->op) {
  case BinaryOp::Eq:
    result = emitValuesEqual(lhs.value, rhs.value, Type::string());
    break;
  case BinaryOp::Ne:
    result = builder_.CreateNot(emitValuesEqual(lhs.value, rhs.value, Type::string()));
    break;
  case BinaryOp::Lt:
    result = builder_.CreateICmpSLT(builder_.CreateCall(rt_.strCompare, args), zero);
    break;
  case BinaryOp::Le:
    result = builder_.CreateICmpSLE(builder_.CreateCall(rt_.strCompare, args), zero);
    break;
  case BinaryOp::Gt:
    result = builder_.CreateICmpSGT(builder_.CreateCall(rt_.strCompare, args), zero);
    break;
  case BinaryOp::Ge:
    result = builder_.CreateICmpSGE(builder_.CreateCall(rt_.strCompare, args), zero);
    break;
  default:
    llvm_unreachable("not a string comparison");
  }
  dropOperand(lhs);
  dropOperand(rhs);
  return result;
}

llvm::Value *CodeGen::emitMember(MemberExpr *member) {
  if (member->method)
    return emitBoundMethod(member);
  if (member->object->type->nonOptional()->isJson())
    return emitJsonMember(member);
  // A union of binary data too, whose members keep the length alike.
  const Type *objectType = member->object->type;
  if (objectType->isBytes() ||
      (objectType->isUnion() &&
       llvm::all_of(objectType->members(), [](const Type *m) { return m->isBytes(); })))
    return emitBytesMember(member);
  if (member->object->type->isObject() || member->object->type->isUnion())
    return emitFieldRead(member);
  if (member->object->type->isResult())
    return emitResultMember(member);
  // Otherwise Sema allows only `.length` of a string or an array.
  Operand object = emitOperand(member->object, /*canBorrow=*/true);
  llvm::Value *length = object.type->isArray() ? arrayLength(object.value) : stringLength(object.value);
  dropOperand(object);
  return builder_.CreateTrunc(length, builder_.getInt32Ty(), "length");
}

// `s[i]`: a string of one code unit; `undefined` out of range unless Sema
// proved the index in range.
llvm::Value *CodeGen::emitStringIndex(IndexExpr *index) {
  Operand object = emitOperand(index->object, cannotAssign(index->index));
  holdOperand(object, exprThrows(index->index));
  llvm::Value *position = emitExpr(index->index);
  takeOperand(object);
  auto read = [&] {
    return callReturningString(rt_.strCharAt,
                               {stringObj(object.value), stringMeta(object.value), position});
  };
  llvm::Value *result;
  if (!index->mayBeOutOfRange)
    result = read();
  else
    result = emitCheckedRead(builder_.CreateICmpULT(position, stringLength(object.value)),
                             index->type, read);
  dropOperand(object);
  return result;
}

llvm::Value *CodeGen::emitStringMethod(CallExpr *call) {
  auto *member = cast<MemberExpr>(call->callee);
  if (call->builtin == Builtin::StrToString)
    return emitExpr(member->object);
  if (call->builtin == Builtin::StrConcat) {
    // `s.concat(a, b)` is `s + a + b`.
    std::vector<Expr *> parts{member->object};
    parts.insert(parts.end(), call->args.begin(), call->args.end());
    std::vector<Operand> operands;
    for (size_t i = 0; i < parts.size(); ++i) {
      bool later = std::all_of(parts.begin() + i + 1, parts.end(), cannotAssign);
      operands.push_back(emitConcatPart(parts[i], later));
    }
    return concatOperands(operands);
  }

  Operand receiver = emitOperand(member->object, llvm::all_of(call->args, cannotAssign));
  holdOperand(receiver, llvm::any_of(call->args, [&](Expr *arg) { return exprThrows(arg); }));
  std::vector<Operand> strings;
  std::vector<llvm::Value *> numbers; // positions and counts, as i64
  for (size_t i = 0; i < call->args.size(); ++i) {
    Expr *arg = call->args[i];
    if (arg->type->isString()) {
      bool later = std::all_of(call->args.begin() + i + 1, call->args.end(), cannotAssign);
      strings.push_back(emitOperand(arg, later));
      holdOperand(strings.back(), std::any_of(call->args.begin() + i + 1, call->args.end(),
                                              [&](Expr *next) { return exprThrows(next); }));
    } else {
      numbers.push_back(builder_.CreateSExt(emitExpr(arg), builder_.getInt64Ty()));
    }
  }
  takeOperand(receiver);
  for (Operand &str : strings)
    takeOperand(str);
  // An optional position: its value and whether it was given.
  auto optional = [&](size_t i) -> std::pair<llvm::Value *, llvm::Value *> {
    if (i < numbers.size())
      return {numbers[i], builder_.getInt32(1)};
    return {builder_.getInt64(0), builder_.getInt32(0)};
  };
  // An optional string argument: its parts and whether it was given.
  auto optionalString = [&](size_t i) -> std::array<llvm::Value *, 3> {
    if (i < strings.size())
      return {stringObj(strings[i].value), stringMeta(strings[i].value), builder_.getInt32(1)};
    return {llvm::ConstantPointerNull::get(builder_.getPtrTy()), builder_.getInt64(0),
            builder_.getInt32(0)};
  };

  llvm::Value *obj = stringObj(receiver.value);
  llvm::Value *meta = stringMeta(receiver.value);
  llvm::Value *result = nullptr;
  bool failable = false; // the runtime may throw
  switch (call->builtin) {
  case Builtin::StrCharCodeAt: {
    llvm::Value *position = numbers[0];
    if (!call->mayBeOutOfRange) {
      result = builder_.CreateCall(rt_.strCharCodeAt, {obj, meta, position});
      break;
    }
    // Out of range, JavaScript returns NaN.
    llvm::Value *inRange = builder_.CreateICmpULT(position, stringLength(receiver.value));
    llvm::BasicBlock *readBlock = newBlock("code.read");
    llvm::BasicBlock *endBlock = newBlock("code.end");
    llvm::BasicBlock *before = builder_.GetInsertBlock();
    builder_.CreateCondBr(inRange, readBlock, endBlock);
    switchTo(readBlock);
    llvm::Value *code = builder_.CreateSIToFP(builder_.CreateCall(rt_.strCharCodeAt, {obj, meta, position}),
                                              builder_.getDoubleTy());
    llvm::BasicBlock *readEnd = builder_.GetInsertBlock();
    builder_.CreateBr(endBlock);
    switchTo(endBlock);
    llvm::PHINode *phi = builder_.CreatePHI(builder_.getDoubleTy(), 2);
    phi->addIncoming(llvm::ConstantFP::getNaN(builder_.getDoubleTy()), before);
    phi->addIncoming(code, readEnd);
    result = phi;
    break;
  }
  case Builtin::StrAt: {
    // A negative position counts from the end.
    llvm::Value *length = stringLength(receiver.value);
    llvm::Value *position = numbers[0];
    position = builder_.CreateSelect(builder_.CreateICmpSLT(position, builder_.getInt64(0)),
                                     builder_.CreateAdd(position, length), position);
    auto read = [&] { return callReturningString(rt_.strCharAt, {obj, meta, position}); };
    result = call->mayBeOutOfRange
                 ? emitCheckedRead(builder_.CreateICmpULT(position, length), call->type, read)
                 : read();
    break;
  }
  case Builtin::StrSlice:
  case Builtin::StrSubstring: {
    llvm::Value *start = numbers.empty() ? builder_.getInt64(0) : numbers[0];
    auto [end, hasEnd] = optional(1);
    result = callReturningString(
        call->builtin == Builtin::StrSlice ? rt_.strSlice : rt_.strSubstring,
        {obj, meta, start, end, hasEnd});
    break;
  }
  case Builtin::StrIndexOf:
  case Builtin::StrIncludes:
  case Builtin::StrStartsWith:
  case Builtin::StrEndsWith: {
    llvm::FunctionCallee fn = call->builtin == Builtin::StrIndexOf    ? rt_.strIndexOf
                              : call->builtin == Builtin::StrIncludes ? rt_.strIncludes
                              : call->builtin == Builtin::StrStartsWith ? rt_.strStartsWith
                                                                        : rt_.strEndsWith;
    llvm::Value *search = strings[0].value;
    auto [position, hasPosition] = optional(0);
    result = builder_.CreateCall(
        fn, {obj, meta, stringObj(search), stringMeta(search), position, hasPosition});
    if (call->builtin != Builtin::StrIndexOf)
      result = builder_.CreateICmpNE(result, builder_.getInt32(0));
    break;
  }
  case Builtin::StrRepeat:
    result = callReturningString(rt_.strRepeat, {obj, meta, numbers[0]});
    failable = true; // a negative count
    break;
  case Builtin::StrCharAt: // "" out of range
    result = callReturningString(rt_.strCharAtOrEmpty,
                                 {obj, meta, numbers.empty() ? builder_.getInt64(0) : numbers[0]});
    break;
  case Builtin::StrCodePointAt: {
    llvm::Value *position = numbers[0];
    auto read = [&]() -> llvm::Value * {
      return builder_.CreateCall(rt_.strCodePoint, {obj, meta, position});
    };
    result = call->mayBeOutOfRange
                 ? emitCheckedRead(builder_.CreateICmpULT(position, stringLength(receiver.value)),
                                   call->type, read)
                 : read();
    break;
  }
  case Builtin::StrLastIndexOf: {
    llvm::Value *search = strings[0].value;
    auto [position, hasPosition] = optional(0);
    result = builder_.CreateCall(rt_.strLastIndexOf, {obj, meta, stringObj(search), stringMeta(search),
                                                      position, hasPosition});
    break;
  }
  case Builtin::StrSplit: {
    auto [sepObj, sepMeta, hasSep] = optionalString(0);
    auto [limit, hasLimit] = optional(0);
    result = builder_.CreateCall(rt_.strSplit, {typeDescriptor(Type::string()), obj, meta, sepObj,
                                                sepMeta, hasSep, limit, hasLimit},
                                 "split");
    break;
  }
  case Builtin::StrTrim:
  case Builtin::StrTrimStart:
  case Builtin::StrTrimEnd: {
    int ends = call->builtin == Builtin::StrTrimStart ? 1 : call->builtin == Builtin::StrTrimEnd ? 2 : 3;
    result = callReturningString(rt_.strTrim, {obj, meta, builder_.getInt32(ends)});
    break;
  }
  case Builtin::StrPadStart:
  case Builtin::StrPadEnd: {
    auto [fillObj, fillMeta, hasFill] = optionalString(0);
    result = callReturningString(rt_.strPad, {obj, meta, numbers[0], fillObj, fillMeta, hasFill,
                                              builder_.getInt32(call->builtin == Builtin::StrPadStart)});
    failable = true; // a string too long
    break;
  }
  case Builtin::StrToUpperCase:
  case Builtin::StrToLowerCase:
    result = callReturningString(
        call->builtin == Builtin::StrToUpperCase ? rt_.strToUpper : rt_.strToLower, {obj, meta});
    break;
  case Builtin::StrReplace:
  case Builtin::StrReplaceAll: {
    llvm::Value *pattern = strings[0].value, *replacement = strings[1].value;
    result = callReturningString(
        rt_.strReplace, {obj, meta, stringObj(pattern), stringMeta(pattern), stringObj(replacement),
                         stringMeta(replacement), builder_.getInt32(call->builtin == Builtin::StrReplaceAll)});
    break;
  }
  default:
    llvm_unreachable("not a string method");
  }

  dropOperand(receiver);
  for (const Operand &str : strings)
    dropOperand(str);
  emitThrowCheckIf(failable);
  return result;
}

// A short literal is an inline string. A longer one lives in the executable
// in the same layout as a heap buffer (see runtime/string.c), one byte per
// code unit when all of them fit.
llvm::Constant *CodeGen::stringLiteral(const std::u16string &text) {
  auto found = literals_.find(text);
  if (found != literals_.end())
    return found->second;

  bool twoByte = llvm::any_of(text, [](char16_t c) { return c > 0xFF; });
  bool ascii = llvm::all_of(text, [](char16_t c) { return c < 0x80; });
  llvm::Constant *value;
  if (text.empty()) {
    value = llvm::ConstantStruct::get(
        strTy_, {llvm::ConstantPointerNull::get(builder_.getPtrTy()), builder_.getInt64(kStrAscii)});
  } else if (!twoByte && text.size() <= kInlineMax) {
    // The first code unit goes to the lowest address, zeros after the last.
    bool little = module_->getDataLayout().isLittleEndian();
    uint64_t bits = 0;
    for (size_t i = 0; i < text.size(); ++i)
      bits |= uint64_t(static_cast<uint8_t>(text[i])) << (8 * (little ? i : 7 - i));
    uint64_t meta = text.size() | kStrRope | (ascii ? kStrAscii : 0);
    value = llvm::ConstantStruct::get(
        strTy_, {llvm::ConstantExpr::getIntToPtr(builder_.getInt64(bits), builder_.getPtrTy()),
                 builder_.getInt64(meta)});
  } else {
    llvm::Constant *data;
    if (twoByte) {
      std::vector<uint16_t> units(text.begin(), text.end());
      data = llvm::ConstantDataArray::get(ctx_, units);
    } else {
      std::vector<uint8_t> units;
      for (char16_t c : text)
        units.push_back(static_cast<uint8_t>(c));
      data = llvm::ConstantDataArray::get(ctx_, units);
    }
    llvm::Type *i32 = builder_.getInt32Ty();
    auto *layout = llvm::StructType::get(ctx_, {builder_.getInt64Ty(), i32, i32, i32, i32, data->getType()});
    auto length = static_cast<uint32_t>(text.size());
    llvm::Constant *init = llvm::ConstantStruct::get(
        layout, {builder_.getInt64(kLiteralRefCount),
                 builder_.getInt32(twoByte ? kKindBuffer2 : kKindBuffer1), builder_.getInt32(0),
                 builder_.getInt32(length), builder_.getInt32(length), data});
    auto *global = new llvm::GlobalVariable(*module_, layout, /*isConstant=*/true,
                                            llvm::GlobalValue::PrivateLinkage, init, ".str");
    global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
    global->setAlignment(llvm::Align(8));
    uint64_t meta = length | (twoByte ? kStrTwoByte : 0) | (ascii ? kStrAscii : 0);
    value = llvm::ConstantStruct::get(strTy_, {global, builder_.getInt64(meta)});
  }
  literals_.emplace(text, value);
  return value;
}

} // namespace tz
