#include "compiler/library/path/path_lowerer/path_lowerer.h"
#include "compiler/library/process/process_lowerer/process_lowerer.h"
#include "compiler/lower/errors/throw_lowerer/throw_lowerer.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/layouts/object_layouts/object_layouts.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/objects/object_maker/object_maker.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "llvm/Support/ErrorHandling.h"
#include <vector>

namespace tz {

namespace {

const std::vector<MakerFields> &pathObjects() {
  static const std::vector<MakerFields> table = {
      {nullptr, {"root", "dir", "base", "ext", "name"}},
  };

  return table;
}

}

PathLowerer::PathLowerer(Lowering &lowering) : lowering_(lowering) {}

llvm::Value *PathLowerer::emitPathCall(CallExpr *call) {
  llvm::Type *voidTy = lowering_.builder_.getVoidTy(), *i32 = lowering_.builder_.getInt32Ty();
  llvm::Type *i64 = lowering_.builder_.getInt64Ty(), *ptr = lowering_.builder_.getPtrTy();
  std::vector<llvm::Value *> values = lowering_.throwLowerer().emitHeldValues(call->args);
  auto obj = [&](size_t i) { return lowering_.stringLowerer().stringObj(values[i]); };
  auto meta = [&](size_t i) { return lowering_.stringLowerer().stringMeta(values[i]); };
  auto fn = [&](llvm::StringRef name, llvm::Type *ret, llvm::ArrayRef<llvm::Type *> params) {
    return lowering_.processLowerer().processFunction(name, ret, params);
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
    result = lowering_.builder_.CreateCall(
        fn("tz_path_parse", ptr, {ptr, i64, i32, ptr}),
        {obj(1), meta(1), values[0], lowering_.objectMaker().maker(call->type, pathObjects())});
    break;
  case Builtin::PathFormat: {
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
  default:
    llvm_unreachable("not a built-in of node:path");
  }
  for (size_t i = 0; i < values.size(); ++i) {
    lowering_.refCounts().releaseValue(values[i], call->args[i]->type);
  }

  return result;
}

}
