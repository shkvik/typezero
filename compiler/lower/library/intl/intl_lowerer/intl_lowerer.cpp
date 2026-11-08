#include "compiler/lower/library/intl/intl_lowerer/intl_lowerer.h"
#include "compiler/lower/expr/assign_lowerer/assign_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/function_builder/function_builder.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/functions/closure_lowerer/closure_lowerer.h"
#include "compiler/lower/layouts/object_layouts/object_layouts.h"
#include "compiler/lower/layouts/type_descriptors/type_descriptors.h"
#include "compiler/lower/library/date/date_lowerer/date_lowerer.h"
#include "compiler/lower/library/decimal/decimal_lowerer/decimal_lowerer.h"
#include "compiler/lower/library/process/process_lowerer/process_lowerer.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/stmt/stmt_lowerer/stmt_lowerer.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/lower/types/union_layout/union_layout.h"
#include "compiler/lower/values/array_lowerer/array_lowerer.h"
#include "compiler/lower/values/number_lowerer/number_lowerer.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "runtime/library/intl/intl/intl.h"
#include "llvm/ADT/STLExtras.h"
#include <cmath>
#include <numeric>

namespace tz {

namespace {

const llvm::StringLiteral kOptionNames[TZ_IO_COUNT] = {
#define TZ_INTL_OPTION_NAME(id, name) name,
    TZ_INTL_OPTIONS(TZ_INTL_OPTION_NAME)
#undef TZ_INTL_OPTION_NAME
};

}

int IntlLowerer::optionIndex(llvm::StringRef name) {
  auto at = llvm::find(kOptionNames, name);

  return at == std::end(kOptionNames) ? -1 : int(at - std::begin(kOptionNames));
}

IntlLowerer::IntlLowerer(Lowering &owner)
    : lowering(owner) {}

llvm::Value *IntlLowerer::emitIntlOptions(Expr *locale, Expr *options,
                                          std::vector<Lowering::Operand> &held) {
  llvm::Type *i32 = this->lowering.builder.getInt32Ty(), *i64 = this->lowering.builder.getInt64Ty(),
             *f64 = this->lowering.builder.getDoubleTy();
  auto *textTy = llvm::ArrayType::get(this->lowering.strTy, TZ_IO_COUNT);
  auto *numberTy = llvm::ArrayType::get(f64, TZ_IO_COUNT);
  auto *optionsTy = llvm::StructType::get(this->lowering.ctx,
                                          {this->lowering.strTy, textTy, numberTy, i64, i64, i32});
  llvm::AllocaInst *slot =
      this->lowering.functionFrame().createStackSlot(optionsTy, "intl.options");
  this->lowering.builder.CreateStore(llvm::Constant::getNullValue(optionsTy), slot);
  llvm::Value *hasText = this->lowering.builder.getInt64(0),
              *hasNumber = this->lowering.builder.getInt64(0);
  auto mark = [&](llvm::Value *&mask, llvm::Value *has, unsigned bit) {
    mask = this->lowering.builder.CreateOr(
        mask, this->lowering.builder.CreateShl(this->lowering.builder.CreateZExt(has, i64),
                                               this->lowering.builder.getInt64(bit)));
  };
  auto split = [&](llvm::Value *value,
                   const Type *type) -> std::pair<llvm::Value *, llvm::Value *> {
    const Type *inner = type->nonOptional();
    if (UnionLayout::isAtomType(inner)) {
      llvm::Value *has = type->isOptional()
                             ? this->lowering.builder.CreateICmpNE(
                                   value, llvm::ConstantInt::get(value->getType(), 0))
                             : this->lowering.builder.getTrue();
      return {this->lowering.unionLayout().atomString(value), has};
    }
    if (!type->isOptional()) {
      return {value, this->lowering.builder.getTrue()};
    }

    return {this->lowering.optionalLowerer().optionalValue(value, type),
            this->lowering.optionalLowerer().optionalHas(value, type)};
  };
  if (locale) {
    if (locale->type->isUndefined()) {
      this->lowering.stmtLowerer().emitDiscarded(locale);
    } else {
      Lowering::Operand operand = this->lowering.refCounts().emitOperand(
          locale, !options || AssignLowerer::cannotAssign(options));
      held.push_back(operand);
      auto [value, has] = split(operand.value, operand.type);
      this->lowering.builder.CreateStore(
          value, this->lowering.builder.CreateStructGEP(optionsTy, slot, 0));
      this->lowering.builder.CreateStore(
          this->lowering.builder.CreateZExt(has, i32),
          this->lowering.builder.CreateStructGEP(optionsTy, slot, 5));
    }
  }
  if (options) {
    if (options->type->isUndefined()) {
      this->lowering.stmtLowerer().emitDiscarded(options);
    } else {
      Lowering::Operand object = this->lowering.refCounts().emitOperand(options, true);
      held.push_back(object);
      for (const Field &field : object.type->fields()) {
        int index = IntlLowerer::optionIndex(field.name);
        if (index < 0 || field.type->isUndefined()) {
          continue;
        }
        llvm::Value *loaded = this->lowering.builder.CreateLoad(
            this->lowering.moduleBuilder().lower(field.type),
            this->lowering.objectLayouts().fieldAddress(object.value, object.type, field.name),
            field.name);
        if (const Type *u = field.type->nonOptional(); u->isTagged()) {
          llvm::Value *tag = this->lowering.unionLayout().unionTag(loaded);
          bool atoms = u->slotPart(UnionSlot::Atom) != nullptr;
          llvm::Value *isText = this->lowering.builder.CreateICmpEQ(
              tag, this->lowering.builder.getInt32(
                       unsigned(atoms ? UnionSlot::Atom : UnionSlot::String)));
          llvm::Value *isFlag = this->lowering.builder.CreateICmpEQ(
              tag, this->lowering.builder.getInt32(unsigned(UnionSlot::Bool)));
          llvm::Value *text = nullptr;
          if (atoms) {
            llvm::Value *atom = this->lowering.unionLayout().taggedPart(loaded, u, UnionSlot::Atom);
            text = this->lowering.unionLayout().atomString(this->lowering.builder.CreateSelect(
                isText, atom, llvm::ConstantInt::get(atom->getType(), 0)));
          } else {
            text = this->lowering.builder.CreateSelect(
                isText, this->lowering.unionLayout().taggedPart(loaded, u, UnionSlot::String),
                llvm::Constant::getNullValue(this->lowering.strTy));
          }
          this->lowering.builder.CreateStore(
              text, this->lowering.builder.CreateConstGEP2_32(
                        textTy, this->lowering.builder.CreateStructGEP(optionsTy, slot, 1), 0,
                        unsigned(index)));
          mark(hasText, isText, unsigned(index));
          llvm::Value *flag = this->lowering.builder.CreateUIToFP(
              this->lowering.builder.CreateAnd(
                  isFlag, this->lowering.unionLayout().taggedPart(loaded, u, UnionSlot::Bool)),
              f64);
          this->lowering.builder.CreateStore(
              flag, this->lowering.builder.CreateConstGEP2_32(
                        numberTy, this->lowering.builder.CreateStructGEP(optionsTy, slot, 2), 0,
                        unsigned(index)));
          mark(hasNumber, isFlag, unsigned(index));
          continue;
        }
        auto [value, has] = split(loaded, field.type);
        const Type *base = UnionLayout::storedType(field.type->nonOptional());
        if (UnionLayout::isAtomType(field.type->nonOptional()) || base->isString()) {
          this->lowering.builder.CreateStore(
              value, this->lowering.builder.CreateConstGEP2_32(
                         textTy, this->lowering.builder.CreateStructGEP(optionsTy, slot, 1), 0,
                         unsigned(index)));
          mark(hasText, has, unsigned(index));
          continue;
        }
        llvm::Value *number = base->isFloat()     ? value
                              : base->isInteger() ? this->lowering.builder.CreateSIToFP(value, f64)
                                                  : this->lowering.builder.CreateUIToFP(value, f64);
        this->lowering.builder.CreateStore(
            number, this->lowering.builder.CreateConstGEP2_32(
                        numberTy, this->lowering.builder.CreateStructGEP(optionsTy, slot, 2), 0,
                        unsigned(index)));
        mark(hasNumber, has, unsigned(index));
      }
    }
  }
  this->lowering.builder.CreateStore(hasText,
                                     this->lowering.builder.CreateStructGEP(optionsTy, slot, 3));
  this->lowering.builder.CreateStore(hasNumber,
                                     this->lowering.builder.CreateStructGEP(optionsTy, slot, 4));

  return slot;
}

Lowering::IntlNumber IntlLowerer::emitIntlNumber(Expr *arg) {
  Lowering::IntlNumber n;
  n.operand = this->lowering.refCounts().emitOperand(arg, true);
  llvm::Value *value = n.operand.value;
  const Type *type = n.operand.type;
  n.d = llvm::ConstantFP::get(this->lowering.builder.getDoubleTy(), 0.0);
  n.integer = n.meta = this->lowering.builder.getInt64(0);
  if (type->isDecimal()) {
    n.kind = TZ_IN_DECIMAL;
    n.integer = this->lowering.decimalLowerer().decimalCoef(value);
    n.meta = this->lowering.decimalLowerer().decimalMeta(value);
  } else if (type->isInteger()) {
    n.kind = TZ_IN_INTEGER;
    n.integer = this->lowering.numberLowerer().toI64(value, type);
  } else {
    n.kind = TZ_IN_DOUBLE;
    n.d = value;
  }

  return n;
}

std::vector<llvm::Value *> IntlLowerer::intlNumberArgs(const Lowering::IntlNumber &n) {
  return {this->lowering.builder.getInt32(uint32_t(n.kind)), n.d, n.integer, n.meta};
}

llvm::Constant *IntlLowerer::intlShape(const Type *type) {
  if (llvm::Constant *shape = this->intlShapes.lookup(type)) {
    return shape;
  }
  static const llvm::StringLiteral fields[TZ_IF_COUNT] = {"type",  "value", "unit",      "segment",
                                                          "index", "input", "isWordLike"};
  const Lowering::ObjectLayout &layout = this->lowering.objectLayouts().objectLayout(type);
  const llvm::StructLayout *offsets =
      this->lowering.module_->getDataLayout().getStructLayout(layout.type);
  std::vector<llvm::Constant *> at;
  for (llvm::StringRef name : fields) {
    int32_t offset = -1;
    if (type->field(name)) {
      offset = int32_t(
          offsets->getElementOffset(layout.elements[ObjectLayouts::fieldIndex(type, name)]));
    }
    at.push_back(this->lowering.builder.getInt32(uint32_t(offset)));
  }
  std::vector<unsigned> keys(type->fields().size());
  std::iota(keys.begin(), keys.end(), 0u);
  auto *offsetsTy = llvm::ArrayType::get(this->lowering.builder.getInt32Ty(), TZ_IF_COUNT);
  llvm::Constant *value = llvm::ConstantStruct::getAnon(
      {this->lowering.typeDescriptors().typeDescriptor(type),
       this->lowering.objectLayouts().layoutDescriptor(type, keys),
       this->lowering.builder.getInt64(
           this->lowering.module_->getDataLayout().getTypeAllocSize(layout.type)),
       llvm::ConstantArray::get(offsetsTy, at)});
  auto *global =
      new llvm::GlobalVariable(*this->lowering.module_, value->getType(), true,
                               llvm::GlobalValue::PrivateLinkage, value, "tz.intl.shape");
  this->intlShapes[type] = global;

  return global;
}

llvm::Constant *IntlLowerer::intlBoundInfo(IntlKind kind, const Type *fnType) {
  llvm::Constant *&info = this->intlBound[{kind, fnType}];
  if (info) {
    return info;
  }
  llvm::Type *ptr = this->lowering.builder.getPtrTy(), *voidTy = this->lowering.builder.getVoidTy(),
             *i32 = this->lowering.builder.getInt32Ty(), *i64 = this->lowering.builder.getInt64Ty(),
             *f64 = this->lowering.builder.getDoubleTy();
  auto *code = llvm::Function::Create(this->lowering.closureLowerer().closureFnType(fnType),
                                      llvm::GlobalValue::InternalLinkage, "tz.intl.bound",
                                      this->lowering.module_.get());
  this->lowering.functionBuilder().setFunctionAttributes(code);
  llvm::IRBuilderBase::InsertPoint point = this->lowering.builder.saveIP();
  llvm::Function *outer = this->lowering.function;
  this->lowering.function = code;
  this->lowering.builder.SetInsertPoint(
      llvm::BasicBlock::Create(this->lowering.ctx, "entry", code));
  llvm::Value *self = code->getArg(0);
  const Type *param = fnType->params()[0];
  llvm::Value *result;
  if (kind == IntlKind::Collator) {
    llvm::Value *a = code->getArg(1);
    llvm::Value *b = code->getArg(2);
    result = this->lowering.builder.CreateCall(
        this->lowering.processLowerer().processFunction("tz_collator_compare", i32,
                                                        {ptr, ptr, i64, ptr, i64}),
        {self, this->lowering.stringLowerer().stringObj(a),
         this->lowering.stringLowerer().stringMeta(a), this->lowering.stringLowerer().stringObj(b),
         this->lowering.stringLowerer().stringMeta(b)});
    this->lowering.refCounts().releaseValue(a, Type::string());
    this->lowering.refCounts().releaseValue(b, Type::string());
    result = fnType->ret()->isFloat() ? this->lowering.builder.CreateSIToFP(result, f64)
                                      : this->lowering.builder.CreateSExt(result, i64);
  } else if (kind == IntlKind::DateTimeFormat) {
    llvm::Value *date = code->getArg(1);
    llvm::Value *time;
    if (param->isDate()) {
      time = this->lowering.builder.CreateLoad(this->lowering.builder.getInt64Ty(),
                                               this->lowering.dateLowerer().dateTimeAddress(date),
                                               "time");
    } else {
      time = param->isFloat() ? this->lowering.dateLowerer().wholeTime(date)
                              : this->lowering.numberLowerer().toI64(date, param);
    }
    result = this->lowering.stringLowerer().callReturningString(
        this->lowering.processLowerer().processFunction(
            "tz_dtf_format", voidTy, {ptr, ptr, this->lowering.builder.getInt64Ty()}),
        {self, time});
    this->lowering.refCounts().releaseValue(date, param);
  } else {
    llvm::Value *x = code->getArg(1);
    int numberKind = param->isDecimal()   ? TZ_IN_DECIMAL
                     : param->isInteger() ? TZ_IN_INTEGER
                                          : TZ_IN_DOUBLE;
    llvm::Value *d = numberKind == TZ_IN_DOUBLE ? x : llvm::ConstantFP::get(f64, 0.0);
    llvm::Value *integer =
        numberKind == TZ_IN_DECIMAL   ? this->lowering.decimalLowerer().decimalCoef(x)
        : numberKind == TZ_IN_INTEGER ? this->lowering.numberLowerer().toI64(x, param)
                                      : this->lowering.builder.getInt64(0);
    llvm::Value *meta = numberKind == TZ_IN_DECIMAL ? this->lowering.decimalLowerer().decimalMeta(x)
                                                    : this->lowering.builder.getInt64(0);
    result = this->lowering.stringLowerer().callReturningString(
        this->lowering.processLowerer().processFunction("tz_nf_format", voidTy,
                                                        {ptr, ptr, i32, f64, i64, i64}),
        {self, this->lowering.builder.getInt32(uint32_t(numberKind)), d, integer, meta});
    this->lowering.refCounts().releaseValue(x, param);
  }
  this->lowering.builder.CreateRet(result);
  this->lowering.function = outer;
  this->lowering.builder.restoreIP(point);
  info = new llvm::GlobalVariable(
      *this->lowering.module_, this->lowering.fnInfoTy, true, llvm::GlobalValue::PrivateLinkage,
      llvm::ConstantStruct::get(this->lowering.fnInfoTy,
                                {code, this->lowering.stringLowerer().stringLiteral(u"")}),
      "tz.fninfo.intl");

  return info;
}

llvm::Value *IntlLowerer::emitLocaleArray(Expr *arg) {
  const Type *element = Type::string();
  if (arg && arg->type == Type::array(element)) {
    return this->lowering.exprLowerer().emitExpr(arg);
  }
  llvm::Value *array = this->lowering.builder.CreateCall(
      this->lowering.rt_.arrNew,
      {this->lowering.typeDescriptors().typeDescriptor(element), this->lowering.builder.getInt64(0),
       this->lowering.builder.getInt64(1)},
      "locales");
  if (!arg || arg->type->isUndefined()) {
    if (arg) {
      this->lowering.stmtLowerer().emitDiscarded(arg);
    }
    return array;
  }
  this->lowering.arrayLowerer().emitPush(array, {this->lowering.exprLowerer().emitExpr(arg)},
                                         element);

  return array;
}

}
