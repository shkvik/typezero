#include "compiler/library/intl/intl_lowerer/intl_lowerer.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/function_builder/function_builder.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/lower/stmt/stmt_lowerer/stmt_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/expr/assign_lowerer/assign_lowerer.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "compiler/lower/layouts/type_descriptors/type_descriptors.h"
#include "compiler/lower/values/array_lowerer/array_lowerer.h"
#include "compiler/lower/functions/closure_lowerer/closure_lowerer.h"
#include "compiler/lower/layouts/object_layouts/object_layouts.h"
#include "compiler/lower/errors/may_throw/may_throw.h"
#include "compiler/lower/errors/throw_lowerer/throw_lowerer.h"
#include "compiler/lower/types/union_layout/union_layout.h"
#include "compiler/library/date/date_lowerer/date_lowerer.h"
#include "compiler/library/decimal/decimal_lowerer/decimal_lowerer.h"
#include "compiler/library/process/process_lowerer/process_lowerer.h"
#include "runtime/library/intl/intl/intl.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/ErrorHandling.h"
#include <cmath>
#include <numeric>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::BasicBlock;
using llvm::Value;

namespace tz {

namespace {

const llvm::StringLiteral kOptionNames[TZ_IO_COUNT] = {
#define TZ_INTL_OPTION_NAME(id, name) name,
    TZ_INTL_OPTIONS(TZ_INTL_OPTION_NAME)
#undef TZ_INTL_OPTION_NAME
};

int optionIndex(llvm::StringRef name) {
  auto at = llvm::find(kOptionNames, name);

  return at == std::end(kOptionNames) ? -1 : int(at - std::begin(kOptionNames));
}

} // namespace

IntlLowerer::IntlLowerer(CodeGen &codegen) : codegen_(codegen) {}

// TzIntlOptions (runtime/library/intl/intl/intl.h): { string locale, [N x
// string] text, [N x double] number, i64 has_text, i64 has_number, i32
// has_locale }. The strings are borrowed from the operands, which `held` keeps
// until after the call.
llvm::Value *IntlLowerer::emitIntlOptions(Expr *locale, Expr *options, std::vector<CodeGen::Operand> &held) {
  llvm::Type *i32 = codegen_.builder_.getInt32Ty(), *i64 = codegen_.builder_.getInt64Ty(),
             *f64 = codegen_.builder_.getDoubleTy();
  auto *textTy = llvm::ArrayType::get(codegen_.strTy_, TZ_IO_COUNT);
  auto *numberTy = llvm::ArrayType::get(f64, TZ_IO_COUNT);
  auto *optionsTy = llvm::StructType::get(codegen_.ctx_, {codegen_.strTy_, textTy, numberTy, i64, i64, i32});
  llvm::AllocaInst *slot = codegen_.functionFrame().createStackSlot(optionsTy, "intl.options");
  codegen_.builder_.CreateStore(llvm::Constant::getNullValue(optionsTy), slot);
  llvm::Value *hasText = codegen_.builder_.getInt64(0), *hasNumber = codegen_.builder_.getInt64(0);
  auto mark = [&](llvm::Value *&mask, llvm::Value *has, unsigned bit) {
    mask = codegen_.builder_.CreateOr(
        mask, codegen_.builder_.CreateShl(codegen_.builder_.CreateZExt(has, i64), codegen_.builder_.getInt64(bit)));
  };
  // A value and whether it is there, of a `T | undefined` or a T; an atom
  // (a string literal type) as its text.
  auto split = [&](llvm::Value *value,
                   const Type *type) -> std::pair<llvm::Value *, llvm::Value *> {
    const Type *inner = type->nonOptional();
    if (UnionLayout::isAtomType(inner)) {
      llvm::Value *has =
          type->isOptional()
              ? codegen_.builder_.CreateICmpNE(value, llvm::ConstantInt::get(value->getType(), 0))
              : codegen_.builder_.getTrue();
      return {codegen_.unionLayout().atomString(value), has};
    }
    if (!type->isOptional()) {
      return {value, codegen_.builder_.getTrue()};
    }

    return {codegen_.optionalLowerer().optionalValue(value, type), codegen_.optionalLowerer().optionalHas(value, type)};
  };
  if (locale) {
    if (locale->type->isUndefined()) {
      codegen_.stmtLowerer().emitDiscarded(locale);
    } else {
      CodeGen::Operand operand = codegen_.refCounts().emitOperand(locale, !options || AssignLowerer::cannotAssign(options));
      codegen_.throwLowerer().holdOperand(operand, options && codegen_.mayThrow().exprThrows(options));
      held.push_back(operand);
      auto [value, has] = split(operand.value, operand.type);
      codegen_.builder_.CreateStore(value, codegen_.builder_.CreateStructGEP(optionsTy, slot, 0));
      codegen_.builder_.CreateStore(codegen_.builder_.CreateZExt(has, i32),
                           codegen_.builder_.CreateStructGEP(optionsTy, slot, 5));
    }
  }
  if (options) {
    if (options->type->isUndefined()) {
      codegen_.stmtLowerer().emitDiscarded(options);
    } else {
      CodeGen::Operand object = codegen_.refCounts().emitOperand(options, /*canBorrow=*/true);
      held.push_back(object);
      for (const Field &field : object.type->fields()) {
        int index = optionIndex(field.name);
        if (index < 0 || field.type->isUndefined()) { // undefined is not given
          continue;
        }
        llvm::Value *loaded = codegen_.builder_.CreateLoad(
            codegen_.moduleBuilder().lower(field.type), codegen_.objectLayouts().fieldAddress(object.value, object.type, field.name), field.name);
        // useGrouping of `string | boolean`: the part its tag says.
        if (const Type *u = field.type->nonOptional(); u->isTagged()) {
          llvm::Value *tag = codegen_.unionLayout().unionTag(loaded);
          llvm::Value *isText =
              codegen_.builder_.CreateICmpEQ(tag, codegen_.builder_.getInt32(unsigned(UnionSlot::String)));
          llvm::Value *isFlag =
              codegen_.builder_.CreateICmpEQ(tag, codegen_.builder_.getInt32(unsigned(UnionSlot::Bool)));
          llvm::Value *text =
              codegen_.builder_.CreateSelect(isText, codegen_.unionLayout().taggedPart(loaded, u, UnionSlot::String),
                                    llvm::Constant::getNullValue(codegen_.strTy_));
          codegen_.builder_.CreateStore(
              text, codegen_.builder_.CreateConstGEP2_32(
                        textTy, codegen_.builder_.CreateStructGEP(optionsTy, slot, 1), 0, unsigned(index)));
          mark(hasText, isText, unsigned(index));
          llvm::Value *flag = codegen_.builder_.CreateUIToFP(
              codegen_.builder_.CreateAnd(isFlag, codegen_.unionLayout().taggedPart(loaded, u, UnionSlot::Bool)), f64);
          codegen_.builder_.CreateStore(flag, codegen_.builder_.CreateConstGEP2_32(
                                         numberTy, codegen_.builder_.CreateStructGEP(optionsTy, slot, 2), 0,
                                         unsigned(index)));
          mark(hasNumber, isFlag, unsigned(index));
          continue;
        }
        auto [value, has] = split(loaded, field.type);
        const Type *base = UnionLayout::storedType(field.type->nonOptional());
        if (UnionLayout::isAtomType(field.type->nonOptional()) || base->isString()) {
          codegen_.builder_.CreateStore(
              value, codegen_.builder_.CreateConstGEP2_32(
                         textTy, codegen_.builder_.CreateStructGEP(optionsTy, slot, 1), 0, unsigned(index)));
          mark(hasText, has, unsigned(index));
          continue;
        }
        llvm::Value *number = base->isFloat()     ? value
                              : base->isInteger() ? codegen_.builder_.CreateSIToFP(value, f64)
                                                  : codegen_.builder_.CreateUIToFP(value, f64);
        codegen_.builder_.CreateStore(number, codegen_.builder_.CreateConstGEP2_32(
                                         numberTy, codegen_.builder_.CreateStructGEP(optionsTy, slot, 2), 0,
                                         unsigned(index)));
        mark(hasNumber, has, unsigned(index));
      }
    }
  }
  for (CodeGen::Operand &operand : held) {
    codegen_.throwLowerer().takeOperand(operand);
  }
  codegen_.builder_.CreateStore(hasText, codegen_.builder_.CreateStructGEP(optionsTy, slot, 3));
  codegen_.builder_.CreateStore(hasNumber, codegen_.builder_.CreateStructGEP(optionsTy, slot, 4));

  return slot;
}

CodeGen::IntlNumber IntlLowerer::emitIntlNumber(Expr *arg) {
  CodeGen::IntlNumber n;
  n.operand = codegen_.refCounts().emitOperand(arg, /*canBorrow=*/true);
  llvm::Value *value = n.operand.value;
  const Type *type = n.operand.type;
  llvm::Type *i64 = codegen_.builder_.getInt64Ty();
  n.d = llvm::ConstantFP::get(codegen_.builder_.getDoubleTy(), 0.0);
  n.integer = n.meta = codegen_.builder_.getInt64(0);
  if (type->isDecimal()) {
    n.kind = TZ_IN_DECIMAL;
    n.integer = codegen_.decimalLowerer().decimalCoef(value);
    n.meta = codegen_.decimalLowerer().decimalMeta(value);
  } else if (type->isInteger()) {
    n.kind = TZ_IN_INTEGER;
    n.integer = type->isInt() ? codegen_.builder_.CreateSExt(value, i64) : value;
  } else {
    n.kind = TZ_IN_DOUBLE;
    n.d = value;
  }

  return n;
}

std::vector<llvm::Value *> IntlLowerer::intlNumberArgs(const CodeGen::IntlNumber &n) {
  return {codegen_.builder_.getInt32(uint32_t(n.kind)), n.d, n.integer, n.meta};
}

// TzIntlShape: what an array of `type` holds, the layout of its objects with
// the fields in their order, its size, and the offset of each field of
// TZ_IF_*, -1 for those it has not.
llvm::Constant *IntlLowerer::intlShape(const Type *type) {
  if (llvm::Constant *shape = intlShapes_.lookup(type)) {
    return shape;
  }
  static const llvm::StringLiteral fields[TZ_IF_COUNT] = {"type",  "value", "unit",      "segment",
                                                          "index", "input", "isWordLike"};
  const CodeGen::ObjectLayout &layout = codegen_.objectLayouts().objectLayout(type);
  const llvm::StructLayout *offsets = codegen_.module_->getDataLayout().getStructLayout(layout.type);
  std::vector<llvm::Constant *> at;
  for (llvm::StringRef name : fields) {
    int32_t offset = -1;
    if (type->field(name)) {
      offset = int32_t(offsets->getElementOffset(layout.elements[ObjectLayouts::fieldIndex(type, name)]));
    }
    at.push_back(codegen_.builder_.getInt32(uint32_t(offset)));
  }
  std::vector<unsigned> keys(type->fields().size());
  std::iota(keys.begin(), keys.end(), 0u);
  auto *offsetsTy = llvm::ArrayType::get(codegen_.builder_.getInt32Ty(), TZ_IF_COUNT);
  llvm::Constant *value = llvm::ConstantStruct::getAnon(
      {codegen_.typeDescriptors().typeDescriptor(type), codegen_.objectLayouts().layoutDescriptor(type, keys),
       codegen_.builder_.getInt64(codegen_.module_->getDataLayout().getTypeAllocSize(layout.type)),
       llvm::ConstantArray::get(offsetsTy, at)});
  auto *global =
      new llvm::GlobalVariable(*codegen_.module_, value->getType(), /*isConstant=*/true,
                               llvm::GlobalValue::PrivateLinkage, value, "tz.intl.shape");
  intlShapes_[type] = global;

  return global;
}

// The code of `nf.format`, `dtf.format` or `collator.compare` as a value of
// `fnType`: it takes the object as its environment, calls the runtime and
// releases its arguments, which it owns.
llvm::Constant *IntlLowerer::intlBoundInfo(IntlKind kind, const Type *fnType) {
  llvm::Constant *&info = intlBound_[{kind, fnType}];
  if (info) {
    return info;
  }
  llvm::Type *ptr = codegen_.builder_.getPtrTy(), *voidTy = codegen_.builder_.getVoidTy(),
             *i32 = codegen_.builder_.getInt32Ty(), *i64 = codegen_.builder_.getInt64Ty(),
             *f64 = codegen_.builder_.getDoubleTy();
  auto *code = llvm::Function::Create(codegen_.closureLowerer().closureFnType(fnType), llvm::GlobalValue::InternalLinkage,
                                      "tz.intl.bound", codegen_.module_.get());
  codegen_.functionBuilder().setFunctionAttributes(code);
  llvm::IRBuilderBase::InsertPoint point = codegen_.builder_.saveIP();
  llvm::Function *outer = codegen_.function_;
  codegen_.function_ = code;
  codegen_.builder_.SetInsertPoint(llvm::BasicBlock::Create(codegen_.ctx_, "entry", code));
  llvm::Value *self = code->getArg(0);
  const Type *param = fnType->params()[0];
  llvm::Value *result;
  if (kind == IntlKind::Collator) {
    llvm::Value *a = code->getArg(1), *b = code->getArg(2);
    result =
        codegen_.builder_.CreateCall(codegen_.processLowerer().processFunction("tz_collator_compare", i32, {ptr, ptr, i64, ptr, i64}),
                            {self, codegen_.stringLowerer().stringObj(a), codegen_.stringLowerer().stringMeta(a), codegen_.stringLowerer().stringObj(b), codegen_.stringLowerer().stringMeta(b)});
    codegen_.refCounts().releaseValue(a, Type::string());
    codegen_.refCounts().releaseValue(b, Type::string());
    if (fnType->ret()->isFloat()) {
      result = codegen_.builder_.CreateSIToFP(result, f64);
    }
  } else if (kind == IntlKind::DateTimeFormat) {
    llvm::Value *date = code->getArg(1);
    llvm::Value *time;
    if (param->isDate()) {
      time = codegen_.builder_.CreateLoad(f64, codegen_.dateLowerer().dateTimeAddress(date), "time");
    } else {
      llvm::Value *ms = param->isFloat() ? date : codegen_.builder_.CreateSIToFP(date, f64);
      time = codegen_.builder_.CreateCall(codegen_.processLowerer().processFunction("tz_date_clip", f64, {f64}), {ms});
    }
    result = codegen_.stringLowerer().callReturningString(codegen_.processLowerer().processFunction("tz_dtf_format", voidTy, {ptr, ptr, f64}),
                                 {self, time});
    codegen_.refCounts().releaseValue(date, param);
  } else {
    llvm::Value *x = code->getArg(1);
    int numberKind = param->isDecimal()   ? TZ_IN_DECIMAL
                     : param->isInteger() ? TZ_IN_INTEGER
                                          : TZ_IN_DOUBLE;
    llvm::Value *d = numberKind == TZ_IN_DOUBLE ? x : llvm::ConstantFP::get(f64, 0.0);
    llvm::Value *integer = numberKind == TZ_IN_DECIMAL   ? codegen_.decimalLowerer().decimalCoef(x)
                           : param->isInt()              ? codegen_.builder_.CreateSExt(x, i64)
                           : numberKind == TZ_IN_INTEGER ? x
                                                         : codegen_.builder_.getInt64(0);
    llvm::Value *meta = numberKind == TZ_IN_DECIMAL ? codegen_.decimalLowerer().decimalMeta(x) : codegen_.builder_.getInt64(0);
    result =
        codegen_.stringLowerer().callReturningString(codegen_.processLowerer().processFunction("tz_nf_format", voidTy, {ptr, ptr, i32, f64, i64, i64}),
                            {self, codegen_.builder_.getInt32(uint32_t(numberKind)), d, integer, meta});
    codegen_.refCounts().releaseValue(x, param);
  }
  codegen_.builder_.CreateRet(result);
  codegen_.function_ = outer;
  codegen_.builder_.restoreIP(point);
  info = new llvm::GlobalVariable(
      *codegen_.module_, codegen_.fnInfoTy_, /*isConstant=*/true, llvm::GlobalValue::PrivateLinkage,
      llvm::ConstantStruct::get(codegen_.fnInfoTy_, {code, codegen_.stringLowerer().stringLiteral(u"")}), "tz.fninfo.intl");

  return info;
}

// The locales of getCanonicalLocales and supportedLocalesOf as an owned
// array: none, one string, or the array itself.
llvm::Value *IntlLowerer::emitLocaleArray(Expr *arg) {
  const Type *element = Type::string();
  if (arg && arg->type == Type::array(element)) {
    return codegen_.exprLowerer().emitExpr(arg);
  }
  llvm::Value *array = codegen_.builder_.CreateCall(
      codegen_.rt_.arrNew, {codegen_.typeDescriptors().typeDescriptor(element), codegen_.builder_.getInt64(0), codegen_.builder_.getInt64(1)}, "locales");
  if (!arg || arg->type->isUndefined()) {
    if (arg) {
      codegen_.stmtLowerer().emitDiscarded(arg);
    }
    return array;
  }
  codegen_.arrayLowerer().emitPush(array, {codegen_.exprLowerer().emitExpr(arg)}, element);

  return array;
}

} // namespace tz
