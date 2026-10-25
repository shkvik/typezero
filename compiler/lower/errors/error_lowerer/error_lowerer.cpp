#include "compiler/lower/errors/error_lowerer/error_lowerer.h"
#include "compiler/lower/function_builder/function_builder.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "compiler/lower/layouts/type_descriptors/type_descriptors.h"
#include "compiler/lower/classes/class_descriptors/class_descriptors.h"
#include "compiler/lower/memory/store_lowerer/store_lowerer.h"
#include "compiler/lower/layouts/object_layouts/object_layouts.h"
#include "compiler/lower/errors/throw_lowerer/throw_lowerer.h"
#include "compiler/library/process/process_lowerer/process_lowerer.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include <functional>
#include <map>
#include <optional>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::BasicBlock;
using llvm::Value;

namespace tz {

namespace {

std::u16string utf16(llvm::StringRef text) {
  llvm::SmallVector<llvm::UTF16, 32> units;
  llvm::convertUTF8ToUTF16String(text, units);

  return std::u16string(units.begin(), units.end());
}

} // namespace

ErrorLowerer::ErrorLowerer(CodeGen &codegen) : codegen_(codegen) {}

// For inheritance (see sema_error.cpp): a class derived from Error calls this
// constructor for `super(message, options)` with its object as `this`; its
// descriptor needs TZ_LAYOUT_ERROR in its flags (as classDescriptor gives
// Error's), and its instances the fields of Error at the offsets that
// emitErrorSupport tells the runtime, which a subclass that keeps its
// parent's fields first has. A value of type Error is released through the
// descriptor of its object (releaseValue), so instances of subclasses are.
//
// constructor(message?: string, options?: { cause?: Error }): the message (or
// ""), the cause and the name "Error"; the places come when it is thrown.
void ErrorLowerer::emitErrorConstructor(FunctionDecl *fn) {
  codegen_.functionBuilder().beginFunction(codegen_.functions_.lookup(fn->sym), fn);
  const Type *type = fn->cls->type;
  llvm::Value *self = codegen_.function_->getArg(0);
  llvm::Value *message = codegen_.function_->getArg(1);
  llvm::Value *options = codegen_.function_->getArg(2);
  const Type *messageType = fn->sym->params[0], *optionsType = fn->sym->params[1];
  llvm::Value *text = codegen_.builder_.CreateSelect(
      codegen_.optionalLowerer().optionalHas(message, messageType), codegen_.optionalLowerer().optionalValue(message, messageType), codegen_.stringLowerer().stringLiteral(u""));
  codegen_.builder_.CreateStore(text, codegen_.objectLayouts().fieldAddress(self, type, "message"));
  codegen_.builder_.CreateStore(codegen_.stringLowerer().stringLiteral(u"Error"), codegen_.objectLayouts().fieldAddress(self, type, "name"));
  llvm::BasicBlock *causeBlock = codegen_.functionFrame().newBlock("cause");
  llvm::BasicBlock *doneBlock = codegen_.functionFrame().newBlock("done");
  codegen_.builder_.CreateCondBr(codegen_.optionalLowerer().optionalHas(options, optionsType), causeBlock, doneBlock);
  codegen_.functionFrame().switchTo(causeBlock);
  const Type *object = optionsType->inner();
  llvm::Value *cause =
      codegen_.builder_.CreateLoad(codegen_.builder_.getPtrTy(), codegen_.objectLayouts().fieldAddress(options, object, "cause"));
  codegen_.refCounts().retainValue(cause, Type::optional(type));
  codegen_.builder_.CreateStore(cause, codegen_.objectLayouts().fieldAddress(self, type, "cause"));
  // An Error may be a member of a group (codegen_cycles.cpp): the cause
  // joins it, as a store of the program's would.
  codegen_.storeLowerer().groupStore(self, cause, Type::optional(type));
  // The constructor owns its arguments. The options may be a literal of a
  // type of its own, a member of a group: let go of through its descriptor.
  if (codegen_.hasCycles_) {
    codegen_.builder_.CreateCall(codegen_.instanceReleaseFn_, {options});
  } else {
    codegen_.refCounts().releaseValue(options, object);
  }
  codegen_.builder_.CreateBr(doneBlock);
  codegen_.functionFrame().switchTo(doneBlock);
  codegen_.builder_.CreateRetVoid();
  codegen_.function_ = nullptr;
}

// The class of a kind of error (an index of kErrorKinds): Error, or the
// class of the kind that extends it.
const ClassDecl *ErrorLowerer::errorKindClass(int kind) const {
  if (kind > 0) {
    for (const ClassDecl *child : codegen_.errorClass_->children) {
      if (child->builtin && child->name == kErrorKinds[kind]) {
        return child;
      }
    }
  }

  return codegen_.errorClass_;
}

// The descriptor of Error, and of each kind: the classes' own.
llvm::Constant *ErrorLowerer::errorDescriptor(int kind) { return codegen_.classDescriptors().classDescriptor(errorKindClass(kind)); }

// (i32 kind, string message) -> Error: how the runtime makes the errors of
// its own failures (TzErrorInfo::make), and the built-ins of the program.
llvm::Function *ErrorLowerer::errorMaker() {
  if (errorMaker_) {
    return errorMaker_;
  }
  llvm::Type *ptr = codegen_.builder_.getPtrTy(), *i32 = codegen_.builder_.getInt32Ty(), *i64 = codegen_.builder_.getInt64Ty();
  errorMaker_ =
      llvm::Function::Create(llvm::FunctionType::get(ptr, {i32, ptr, i64}, false),
                             llvm::GlobalValue::InternalLinkage, "tz.error.new", codegen_.module_.get());
  codegen_.functionBuilder().setFunctionAttributes(errorMaker_);
  errorMaker_->addFnAttr(llvm::Attribute::Cold);
  llvm::IRBuilderBase::InsertPoint point = codegen_.builder_.saveIP();
  llvm::Function *outer = std::exchange(codegen_.function_, errorMaker_);
  codegen_.builder_.SetInsertPoint(llvm::BasicBlock::Create(codegen_.ctx_, "entry", errorMaker_));
  size_t count = std::size(kErrorKinds);
  std::vector<llvm::Constant *> descriptors;
  for (size_t k = 0; k < count; ++k) {
    descriptors.push_back(errorDescriptor(int(k)));
  }
  auto *tableType = llvm::ArrayType::get(ptr, count);
  auto *table = new llvm::GlobalVariable(
      *codegen_.module_, tableType, /*isConstant=*/true, llvm::GlobalValue::PrivateLinkage,
      llvm::ConstantArray::get(tableType, descriptors), "tz.errors");
  llvm::Value *kind = errorMaker_->getArg(0);
  llvm::Value *descriptor = codegen_.builder_.CreateLoad(
      ptr, codegen_.builder_.CreateInBoundsGEP(tableType, table, {codegen_.builder_.getInt64(0), kind}), "layout");
  const Type *type = codegen_.errorClass_->type;
  const CodeGen::ObjectLayout &layout = codegen_.objectLayouts().objectLayout(type);
  uint64_t size = codegen_.module_->getDataLayout().getTypeAllocSize(layout.type);
  llvm::Value *object = codegen_.builder_.CreateCall(codegen_.rt_.objAlloc, {codegen_.builder_.getInt64(size)}, "error");
  codegen_.builder_.CreateStore(codegen_.builder_.getInt64(1), codegen_.builder_.CreateStructGEP(layout.type, object, 0));
  codegen_.builder_.CreateStore(descriptor, codegen_.builder_.CreateStructGEP(layout.type, object, 1));
  codegen_.builder_.CreateMemSet(codegen_.builder_.CreateConstInBoundsGEP1_64(codegen_.builder_.getInt8Ty(), object, 16),
                        codegen_.builder_.getInt8(0), size - 16, llvm::MaybeAlign(8));
  llvm::Value *message = llvm::UndefValue::get(codegen_.strTy_);
  message = codegen_.builder_.CreateInsertValue(message, errorMaker_->getArg(1), 0);
  message = codegen_.builder_.CreateInsertValue(message, errorMaker_->getArg(2), 1);
  codegen_.builder_.CreateStore(message, codegen_.objectLayouts().fieldAddress(object, type, "message"));
  // The name of the kind: a literal, one of a table like the descriptors.
  std::vector<llvm::Constant *> names;
  for (size_t k = 0; k < count; ++k) {
    names.push_back(codegen_.stringLowerer().stringLiteral(utf16(kErrorKinds[k])));
  }
  auto *namesType = llvm::ArrayType::get(codegen_.strTy_, count);
  auto *namesTable = new llvm::GlobalVariable(
      *codegen_.module_, namesType, /*isConstant=*/true, llvm::GlobalValue::PrivateLinkage,
      llvm::ConstantArray::get(namesType, names), "tz.error.names");
  llvm::Value *name = codegen_.builder_.CreateLoad(
      codegen_.strTy_, codegen_.builder_.CreateInBoundsGEP(namesType, namesTable, {codegen_.builder_.getInt64(0), kind}));
  codegen_.builder_.CreateStore(name, codegen_.objectLayouts().fieldAddress(object, type, "name"));
  codegen_.builder_.CreateRet(object);
  codegen_.function_ = outer;
  codegen_.builder_.restoreIP(point);

  return errorMaker_;
}

// What main tells the runtime first: how to make an Error, and where its
// fields are (TzErrorInfo in runtime/core/tzrt/tzrt.h).
void ErrorLowerer::emitErrorSupport() {
  if (!codegen_.errorClass_) {
    return;
  }
  const Type *type = codegen_.errorClass_->type;
  const CodeGen::ObjectLayout &layout = codegen_.objectLayouts().objectLayout(type);
  const llvm::StructLayout *offsets = codegen_.module_->getDataLayout().getStructLayout(layout.type);
  auto offset = [&](llvm::StringRef field) {
    return codegen_.builder_.getInt32(
        uint32_t(offsets->getElementOffset(layout.elements[ObjectLayouts::fieldIndex(type, field)])));
  };
  llvm::Type *ptr = codegen_.builder_.getPtrTy(), *i32 = codegen_.builder_.getInt32Ty();
  auto *infoType = llvm::StructType::get(codegen_.ctx_, {ptr, ptr, i32, i32, i32, i32});
  llvm::Constant *init = llvm::ConstantStruct::get(
      infoType, {errorMaker(), codegen_.typeDescriptors().typeDescriptor(Type::longTy()), offset("name"), offset("message"),
                 offset("cause"), offset("#frames")});
  auto *info = new llvm::GlobalVariable(*codegen_.module_, infoType, /*isConstant=*/true,
                                        llvm::GlobalValue::PrivateLinkage, init, "tz.error.info");
  codegen_.builder_.CreateCall(codegen_.processLowerer().processFunction("tz_error_init", codegen_.builder_.getVoidTy(), {ptr}), {info});
}

// `new Error(message, options)` and the kinds: the constructor of Error, and
// the name of the kind.
llvm::Value *ErrorLowerer::emitNewError(NewExpr *expr) {
  int kind = std::max(errorKindIndex(expr->cls->name), 0);
  const CallExpr *ctor = expr->ctor;
  const FuncSymbol *sym = ctor->func;
  std::vector<llvm::Value *> args = codegen_.throwLowerer().emitHeldValues(ctor->args);
  for (size_t i = args.size(); i < sym->params.size(); ++i) {
    args.push_back(codegen_.optionalLowerer().undefinedValue(sym->params[i]));
  }
  const Type *type = codegen_.errorClass_->type;
  const CodeGen::ObjectLayout &layout = codegen_.objectLayouts().objectLayout(type);
  uint64_t size = codegen_.module_->getDataLayout().getTypeAllocSize(layout.type);
  llvm::Value *object = codegen_.builder_.CreateCall(codegen_.rt_.objAlloc, {codegen_.builder_.getInt64(size)}, "error");
  codegen_.builder_.CreateStore(codegen_.builder_.getInt64(1), codegen_.builder_.CreateStructGEP(layout.type, object, 0));
  codegen_.builder_.CreateStore(errorDescriptor(kind), codegen_.builder_.CreateStructGEP(layout.type, object, 1));
  codegen_.builder_.CreateMemSet(codegen_.builder_.CreateConstInBoundsGEP1_64(codegen_.builder_.getInt8Ty(), object, 16),
                        codegen_.builder_.getInt8(0), size - 16, llvm::MaybeAlign(8));
  args.insert(args.begin(), object);
  codegen_.builder_.CreateCall(codegen_.classDescriptors().methodCode(sym->decl), args);
  if (kind != 0) {
    codegen_.builder_.CreateStore(codegen_.stringLowerer().stringLiteral(utf16(kErrorKinds[kind])),
                         codegen_.objectLayouts().fieldAddress(object, type, "name"));
  }

  return object;
}

// `e.stack` and `e.toString()`.
llvm::Value *ErrorLowerer::emitErrorBuiltin(CallExpr *call) {
  auto *member = cast<MemberExpr>(call->callee);
  CodeGen::Operand error = codegen_.refCounts().emitOperand(member->object, /*canBorrow=*/true);
  llvm::Type *ptr = codegen_.builder_.getPtrTy();
  const char *name = call->builtin == Builtin::ErrorStack ? "tz_error_stack" : "tz_error_to_string";
  llvm::Value *text =
      codegen_.stringLowerer().callReturningString(codegen_.processLowerer().processFunction(name, codegen_.builder_.getVoidTy(), {ptr, ptr}), {error.value});
  codegen_.refCounts().dropOperand(error);

  return text;
}

} // namespace tz
