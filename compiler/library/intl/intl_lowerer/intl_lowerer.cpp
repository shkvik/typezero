#include "compiler/library/intl/intl_lowerer/intl_lowerer.h"
#include "compiler/library/date/date_lowerer/date_lowerer.h"
#include "compiler/library/decimal/decimal_lowerer/decimal_lowerer.h"
#include "compiler/library/process/process_lowerer/process_lowerer.h"
#include "compiler/lower/expr/assign_lowerer/assign_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/function_builder/function_builder.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/functions/closure_lowerer/closure_lowerer.h"
#include "compiler/lower/layouts/object_layouts/object_layouts.h"
#include "compiler/lower/layouts/type_descriptors/type_descriptors.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/stmt/stmt_lowerer/stmt_lowerer.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/lower/types/union_layout/union_layout.h"
#include "compiler/lower/values/array_lowerer/array_lowerer.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "runtime/library/intl/intl/intl.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/ErrorHandling.h"
#include <cmath>
#include <numeric>

using llvm::BasicBlock;
using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
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

}

IntlLowerer::IntlLowerer(Lowering &lowering) : lowering_(lowering) {}

llvm::Value *IntlLowerer::emitIntlOptions(Expr *locale, Expr *options,
                                          std::vector<Lowering::Operand> &held) {
  llvm::Type *i32 = lowering_.builder_.getInt32Ty(), *i64 = lowering_.builder_.getInt64Ty(),
             *f64 = lowering_.builder_.getDoubleTy();
  auto *textTy = llvm::ArrayType::get(lowering_.strTy_, TZ_IO_COUNT);
  auto *numberTy = llvm::ArrayType::get(f64, TZ_IO_COUNT);
  auto *optionsTy =
      llvm::StructType::get(lowering_.ctx_, {lowering_.strTy_, textTy, numberTy, i64, i64, i32});
  llvm::AllocaInst *slot = lowering_.functionFrame().createStackSlot(optionsTy, "intl.options");
  lowering_.builder_.CreateStore(llvm::Constant::getNullValue(optionsTy), slot);
  llvm::Value *hasText = lowering_.builder_.getInt64(0),
              *hasNumber = lowering_.builder_.getInt64(0);
  auto mark = [&](llvm::Value *&mask, llvm::Value *has, unsigned bit) {
    mask = lowering_.builder_.CreateOr(
        mask, lowering_.builder_.CreateShl(lowering_.builder_.CreateZExt(has, i64),
                                           lowering_.builder_.getInt64(bit)));
  };
  auto split = [&](llvm::Value *value,
                   const Type *type) -> std::pair<llvm::Value *, llvm::Value *> {
    const Type *inner = type->nonOptional();
    if (UnionLayout::isAtomType(inner)) {
      llvm::Value *has =
          type->isOptional()
              ? lowering_.builder_.CreateICmpNE(value, llvm::ConstantInt::get(value->getType(), 0))
              : lowering_.builder_.getTrue();
      return {lowering_.unionLayout().atomString(value), has};
    }
    if (!type->isOptional()) {
      return {value, lowering_.builder_.getTrue()};
    }

    return {lowering_.optionalLowerer().optionalValue(value, type),
            lowering_.optionalLowerer().optionalHas(value, type)};
  };
  if (locale) {
    if (locale->type->isUndefined()) {
      lowering_.stmtLowerer().emitDiscarded(locale);
    } else {
      Lowering::Operand operand = lowering_.refCounts().emitOperand(
          locale, !options || AssignLowerer::cannotAssign(options));
      held.push_back(operand);
      auto [value, has] = split(operand.value, operand.type);
      lowering_.builder_.CreateStore(value, lowering_.builder_.CreateStructGEP(optionsTy, slot, 0));
      lowering_.builder_.CreateStore(lowering_.builder_.CreateZExt(has, i32),
                                     lowering_.builder_.CreateStructGEP(optionsTy, slot, 5));
    }
  }
  if (options) {
    if (options->type->isUndefined()) {
      lowering_.stmtLowerer().emitDiscarded(options);
    } else {
      Lowering::Operand object = lowering_.refCounts().emitOperand(options, true);
      held.push_back(object);
      for (const Field &field : object.type->fields()) {
        int index = optionIndex(field.name);
        if (index < 0 || field.type->isUndefined()) {
          continue;
        }
        llvm::Value *loaded = lowering_.builder_.CreateLoad(
            lowering_.moduleBuilder().lower(field.type),
            lowering_.objectLayouts().fieldAddress(object.value, object.type, field.name),
            field.name);
        if (const Type *u = field.type->nonOptional(); u->isTagged()) {
          llvm::Value *tag = lowering_.unionLayout().unionTag(loaded);
          bool atoms = u->slotPart(UnionSlot::Atom) != nullptr;
          llvm::Value *isText = lowering_.builder_.CreateICmpEQ(
              tag,
              lowering_.builder_.getInt32(unsigned(atoms ? UnionSlot::Atom : UnionSlot::String)));
          llvm::Value *isFlag = lowering_.builder_.CreateICmpEQ(
              tag, lowering_.builder_.getInt32(unsigned(UnionSlot::Bool)));
          llvm::Value *text = nullptr;
          if (atoms) {
            llvm::Value *atom = lowering_.unionLayout().taggedPart(loaded, u, UnionSlot::Atom);
            text = lowering_.unionLayout().atomString(lowering_.builder_.CreateSelect(
                isText, atom, llvm::ConstantInt::get(atom->getType(), 0)));
          } else {
            text = lowering_.builder_.CreateSelect(
                isText, lowering_.unionLayout().taggedPart(loaded, u, UnionSlot::String),
                llvm::Constant::getNullValue(lowering_.strTy_));
          }
          lowering_.builder_.CreateStore(
              text, lowering_.builder_.CreateConstGEP2_32(
                        textTy, lowering_.builder_.CreateStructGEP(optionsTy, slot, 1), 0,
                        unsigned(index)));
          mark(hasText, isText, unsigned(index));
          llvm::Value *flag = lowering_.builder_.CreateUIToFP(
              lowering_.builder_.CreateAnd(
                  isFlag, lowering_.unionLayout().taggedPart(loaded, u, UnionSlot::Bool)),
              f64);
          lowering_.builder_.CreateStore(
              flag, lowering_.builder_.CreateConstGEP2_32(
                        numberTy, lowering_.builder_.CreateStructGEP(optionsTy, slot, 2), 0,
                        unsigned(index)));
          mark(hasNumber, isFlag, unsigned(index));
          continue;
        }
        auto [value, has] = split(loaded, field.type);
        const Type *base = UnionLayout::storedType(field.type->nonOptional());
        if (UnionLayout::isAtomType(field.type->nonOptional()) || base->isString()) {
          lowering_.builder_.CreateStore(
              value, lowering_.builder_.CreateConstGEP2_32(
                         textTy, lowering_.builder_.CreateStructGEP(optionsTy, slot, 1), 0,
                         unsigned(index)));
          mark(hasText, has, unsigned(index));
          continue;
        }
        llvm::Value *number = base->isFloat()     ? value
                              : base->isInteger() ? lowering_.builder_.CreateSIToFP(value, f64)
                                                  : lowering_.builder_.CreateUIToFP(value, f64);
        lowering_.builder_.CreateStore(
            number, lowering_.builder_.CreateConstGEP2_32(
                        numberTy, lowering_.builder_.CreateStructGEP(optionsTy, slot, 2), 0,
                        unsigned(index)));
        mark(hasNumber, has, unsigned(index));
      }
    }
  }
  lowering_.builder_.CreateStore(hasText, lowering_.builder_.CreateStructGEP(optionsTy, slot, 3));
  lowering_.builder_.CreateStore(hasNumber, lowering_.builder_.CreateStructGEP(optionsTy, slot, 4));

  return slot;
}

Lowering::IntlNumber IntlLowerer::emitIntlNumber(Expr *arg) {
  Lowering::IntlNumber n;
  n.operand = lowering_.refCounts().emitOperand(arg, true);
  llvm::Value *value = n.operand.value;
  const Type *type = n.operand.type;
  llvm::Type *i64 = lowering_.builder_.getInt64Ty();
  n.d = llvm::ConstantFP::get(lowering_.builder_.getDoubleTy(), 0.0);
  n.integer = n.meta = lowering_.builder_.getInt64(0);
  if (type->isDecimal()) {
    n.kind = TZ_IN_DECIMAL;
    n.integer = lowering_.decimalLowerer().decimalCoef(value);
    n.meta = lowering_.decimalLowerer().decimalMeta(value);
  } else if (type->isInteger()) {
    n.kind = TZ_IN_INTEGER;
    n.integer = type->isInt() ? lowering_.builder_.CreateSExt(value, i64) : value;
  } else {
    n.kind = TZ_IN_DOUBLE;
    n.d = value;
  }

  return n;
}

std::vector<llvm::Value *> IntlLowerer::intlNumberArgs(const Lowering::IntlNumber &n) {
  return {lowering_.builder_.getInt32(uint32_t(n.kind)), n.d, n.integer, n.meta};
}

llvm::Constant *IntlLowerer::intlShape(const Type *type) {
  if (llvm::Constant *shape = intlShapes_.lookup(type)) {
    return shape;
  }
  static const llvm::StringLiteral fields[TZ_IF_COUNT] = {"type",  "value", "unit",      "segment",
                                                          "index", "input", "isWordLike"};
  const Lowering::ObjectLayout &layout = lowering_.objectLayouts().objectLayout(type);
  const llvm::StructLayout *offsets =
      lowering_.module_->getDataLayout().getStructLayout(layout.type);
  std::vector<llvm::Constant *> at;
  for (llvm::StringRef name : fields) {
    int32_t offset = -1;
    if (type->field(name)) {
      offset = int32_t(
          offsets->getElementOffset(layout.elements[ObjectLayouts::fieldIndex(type, name)]));
    }
    at.push_back(lowering_.builder_.getInt32(uint32_t(offset)));
  }
  std::vector<unsigned> keys(type->fields().size());
  std::iota(keys.begin(), keys.end(), 0u);
  auto *offsetsTy = llvm::ArrayType::get(lowering_.builder_.getInt32Ty(), TZ_IF_COUNT);
  llvm::Constant *value = llvm::ConstantStruct::getAnon(
      {lowering_.typeDescriptors().typeDescriptor(type),
       lowering_.objectLayouts().layoutDescriptor(type, keys),
       lowering_.builder_.getInt64(
           lowering_.module_->getDataLayout().getTypeAllocSize(layout.type)),
       llvm::ConstantArray::get(offsetsTy, at)});
  auto *global =
      new llvm::GlobalVariable(*lowering_.module_, value->getType(), true,
                               llvm::GlobalValue::PrivateLinkage, value, "tz.intl.shape");
  intlShapes_[type] = global;

  return global;
}

llvm::Constant *IntlLowerer::intlBoundInfo(IntlKind kind, const Type *fnType) {
  llvm::Constant *&info = intlBound_[{kind, fnType}];
  if (info) {
    return info;
  }
  llvm::Type *ptr = lowering_.builder_.getPtrTy(), *voidTy = lowering_.builder_.getVoidTy(),
             *i32 = lowering_.builder_.getInt32Ty(), *i64 = lowering_.builder_.getInt64Ty(),
             *f64 = lowering_.builder_.getDoubleTy();
  auto *code = llvm::Function::Create(lowering_.closureLowerer().closureFnType(fnType),
                                      llvm::GlobalValue::InternalLinkage, "tz.intl.bound",
                                      lowering_.module_.get());
  lowering_.functionBuilder().setFunctionAttributes(code);
  llvm::IRBuilderBase::InsertPoint point = lowering_.builder_.saveIP();
  llvm::Function *outer = lowering_.function_;
  lowering_.function_ = code;
  lowering_.builder_.SetInsertPoint(llvm::BasicBlock::Create(lowering_.ctx_, "entry", code));
  llvm::Value *self = code->getArg(0);
  const Type *param = fnType->params()[0];
  llvm::Value *result;
  if (kind == IntlKind::Collator) {
    llvm::Value *a = code->getArg(1), *b = code->getArg(2);
    result = lowering_.builder_.CreateCall(
        lowering_.processLowerer().processFunction("tz_collator_compare", i32,
                                                   {ptr, ptr, i64, ptr, i64}),
        {self, lowering_.stringLowerer().stringObj(a), lowering_.stringLowerer().stringMeta(a),
         lowering_.stringLowerer().stringObj(b), lowering_.stringLowerer().stringMeta(b)});
    lowering_.refCounts().releaseValue(a, Type::string());
    lowering_.refCounts().releaseValue(b, Type::string());
    if (fnType->ret()->isFloat()) {
      result = lowering_.builder_.CreateSIToFP(result, f64);
    }
  } else if (kind == IntlKind::DateTimeFormat) {
    llvm::Value *date = code->getArg(1);
    llvm::Value *time;
    if (param->isDate()) {
      time =
          lowering_.builder_.CreateLoad(f64, lowering_.dateLowerer().dateTimeAddress(date), "time");
    } else {
      llvm::Value *ms = param->isFloat() ? date : lowering_.builder_.CreateSIToFP(date, f64);
      time = lowering_.builder_.CreateCall(
          lowering_.processLowerer().processFunction("tz_date_clip", f64, {f64}), {ms});
    }
    result = lowering_.stringLowerer().callReturningString(
        lowering_.processLowerer().processFunction("tz_dtf_format", voidTy, {ptr, ptr, f64}),
        {self, time});
    lowering_.refCounts().releaseValue(date, param);
  } else {
    llvm::Value *x = code->getArg(1);
    int numberKind = param->isDecimal()   ? TZ_IN_DECIMAL
                     : param->isInteger() ? TZ_IN_INTEGER
                                          : TZ_IN_DOUBLE;
    llvm::Value *d = numberKind == TZ_IN_DOUBLE ? x : llvm::ConstantFP::get(f64, 0.0);
    llvm::Value *integer = numberKind == TZ_IN_DECIMAL   ? lowering_.decimalLowerer().decimalCoef(x)
                           : param->isInt()              ? lowering_.builder_.CreateSExt(x, i64)
                           : numberKind == TZ_IN_INTEGER ? x
                                                         : lowering_.builder_.getInt64(0);
    llvm::Value *meta = numberKind == TZ_IN_DECIMAL ? lowering_.decimalLowerer().decimalMeta(x)
                                                    : lowering_.builder_.getInt64(0);
    result = lowering_.stringLowerer().callReturningString(
        lowering_.processLowerer().processFunction("tz_nf_format", voidTy,
                                                   {ptr, ptr, i32, f64, i64, i64}),
        {self, lowering_.builder_.getInt32(uint32_t(numberKind)), d, integer, meta});
    lowering_.refCounts().releaseValue(x, param);
  }
  lowering_.builder_.CreateRet(result);
  lowering_.function_ = outer;
  lowering_.builder_.restoreIP(point);
  info = new llvm::GlobalVariable(
      *lowering_.module_, lowering_.fnInfoTy_, true, llvm::GlobalValue::PrivateLinkage,
      llvm::ConstantStruct::get(lowering_.fnInfoTy_,
                                {code, lowering_.stringLowerer().stringLiteral(u"")}),
      "tz.fninfo.intl");

  return info;
}

llvm::Value *IntlLowerer::emitLocaleArray(Expr *arg) {
  const Type *element = Type::string();
  if (arg && arg->type == Type::array(element)) {
    return lowering_.exprLowerer().emitExpr(arg);
  }
  llvm::Value *array = lowering_.builder_.CreateCall(
      lowering_.rt_.arrNew,
      {lowering_.typeDescriptors().typeDescriptor(element), lowering_.builder_.getInt64(0),
       lowering_.builder_.getInt64(1)},
      "locales");
  if (!arg || arg->type->isUndefined()) {
    if (arg) {
      lowering_.stmtLowerer().emitDiscarded(arg);
    }
    return array;
  }
  lowering_.arrayLowerer().emitPush(array, {lowering_.exprLowerer().emitExpr(arg)}, element);

  return array;
}

}
