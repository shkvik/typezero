// Intl beyond dates (docs/intl.md). Its objects are objects of the runtime
// (runtime/library/intl/intl/intl.h), which the code holds, passes to the
// runtime's functions and releases through the drop function of their own
// layouts. The locale and the options go to the runtime as a TzIntlOptions on
// the stack; a number as its kind and its words; what the runtime makes for the
// program (the parts of formatToParts, the data of a segment) it lays out by a
// TzIntlShape that the code gives it.
//
// `nf.format` and `collator.compare` as values are function values whose
// environment is the object, with code that calls the runtime.
//
// What JavaScript throws, the runtime throws (docs/errors.md): a locale or
// options that only the running program shows to be wrong, a unit or a code
// that is not one. The program checks tz_thrown after those calls, and
// ThrowScanner (codegen_throw.cpp) knows them (intlCallMayThrow).

#include "compiler/lower/codegen/codegen.h"

#include "runtime/library/intl/intl/intl.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/ErrorHandling.h"

#include <cmath>
#include <numeric>

using llvm::cast;
using llvm::dyn_cast;

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

const Expr *withoutCasts(const Expr *e) {
  while (auto *cast = dyn_cast<CastExpr>(e)) {
    if (!cast->isImplicit())
      break;
    e = cast->operand;
  }
  return e;
}

// Text known when compiling: a literal, or a template without values.
bool isLiteralText(const Expr *e) {
  e = withoutCasts(e);
  auto *tmpl = dyn_cast<TemplateExpr>(e);
  return llvm::isa<StringLitExpr>(e) || (tmpl && tmpl->exprs.empty());
}

bool isLiteralNumber(const Expr *e) {
  e = withoutCasts(e);
  if (auto *unary = dyn_cast<UnaryExpr>(e); unary && unary->op == UnaryOp::Neg)
    e = withoutCasts(unary->operand);
  return llvm::isa<IntLitExpr, FloatLitExpr>(e);
}

bool isLiteralValue(const Expr *e) {
  return isLiteralText(e) || isLiteralNumber(e) ||
         llvm::isa<BoolLitExpr, UndefinedExpr>(withoutCasts(e));
}

// The locale and the options (both may be left out), with all Sema could
// check written out: a locale that is a literal and an object literal of
// literals, without roundingIncrement, whose combinations only the running
// program checks.
bool optionsMayThrow(const Expr *locale, const Expr *options) {
  if (locale && !locale->type->isUndefined() && !isLiteralText(locale))
    return true;
  if (!options || options->type->isUndefined())
    return false;
  auto *literal = dyn_cast<ObjectLitExpr>(withoutCasts(options));
  if (!literal)
    return true;
  return llvm::any_of(literal->props, [](const ObjectLitExpr::Property &prop) {
    return prop.spread || !prop.value || !isLiteralValue(prop.value) ||
           prop.name == "roundingIncrement";
  });
}

const Expr *argAt(const CallExpr *call, size_t i) {
  return i < call->args.size() ? call->args[i] : nullptr;
}

// The locales of getCanonicalLocales and supportedLocalesOf: a literal, or
// an array literal of them, Sema checked.
bool localesMayThrow(const Expr *arg) {
  if (!arg)
    return false;
  if (auto *array = dyn_cast<ArrayLitExpr>(withoutCasts(arg)))
    return !llvm::all_of(array->elements, [](const ArrayLitExpr::Element &e) {
      return !e.spread && isLiteralText(e.expr);
    });
  return !isLiteralValue(arg);
}

} // namespace

bool intlCallMayThrow(const CallExpr *call) {
  switch (call->builtin) {
  case Builtin::IntlNew: // the kind, then the locale and the options
    return optionsMayThrow(argAt(call, 1), argAt(call, 2));
  case Builtin::IntlSupportedLocales: // the name, the locales and the options
    return localesMayThrow(argAt(call, 1)) || optionsMayThrow(nullptr, argAt(call, 2));
  case Builtin::NumToLocale:
    return optionsMayThrow(argAt(call, 0), argAt(call, 1));
  case Builtin::StrLocaleCompare:
    return optionsMayThrow(argAt(call, 1), argAt(call, 2));
  case Builtin::IntlCanonicalLocales:
    return localesMayThrow(argAt(call, 0));
  case Builtin::StrNormalize:
    return !call->args.empty() && !isLiteralText(call->args[0]);
  case Builtin::RtfFormat:
  case Builtin::RtfFormatToParts:
    // A unit that is not one; a value that is NaN or infinite.
    return !isLiteralText(call->args[1]) || !isLiteralNumber(call->args[0]);
  case Builtin::DnOf:
    return true; // a code that is not one, which only the runtime knows
  default:
    return false;
  }
}

// TzIntlOptions (runtime/library/intl/intl/intl.h): { string locale, [N x
// string] text, [N x double] number, i64 has_text, i64 has_number, i32
// has_locale }. The strings are borrowed from the operands, which `held` keeps
// until after the call.
llvm::Value *CodeGen::emitIntlOptions(Expr *locale, Expr *options, std::vector<Operand> &held) {
  llvm::Type *i32 = builder_.getInt32Ty(), *i64 = builder_.getInt64Ty(),
             *f64 = builder_.getDoubleTy();
  auto *textTy = llvm::ArrayType::get(strTy_, TZ_IO_COUNT);
  auto *numberTy = llvm::ArrayType::get(f64, TZ_IO_COUNT);
  auto *optionsTy = llvm::StructType::get(ctx_, {strTy_, textTy, numberTy, i64, i64, i32});
  llvm::AllocaInst *slot = createStackSlot(optionsTy, "intl.options");
  builder_.CreateStore(llvm::Constant::getNullValue(optionsTy), slot);
  llvm::Value *hasText = builder_.getInt64(0), *hasNumber = builder_.getInt64(0);
  auto mark = [&](llvm::Value *&mask, llvm::Value *has, unsigned bit) {
    mask = builder_.CreateOr(
        mask, builder_.CreateShl(builder_.CreateZExt(has, i64), builder_.getInt64(bit)));
  };
  // A value and whether it is there, of a `T | undefined` or a T; an atom
  // (a string literal type) as its text.
  auto split = [&](llvm::Value *value,
                   const Type *type) -> std::pair<llvm::Value *, llvm::Value *> {
    const Type *inner = type->nonOptional();
    if (isAtomType(inner)) {
      llvm::Value *has =
          type->isOptional()
              ? builder_.CreateICmpNE(value, llvm::ConstantInt::get(value->getType(), 0))
              : builder_.getTrue();
      return {atomString(value), has};
    }
    if (!type->isOptional())
      return {value, builder_.getTrue()};
    return {optionalValue(value, type), optionalHas(value, type)};
  };
  if (locale) {
    if (locale->type->isUndefined()) {
      emitDiscarded(locale);
    } else {
      Operand operand = emitOperand(locale, !options || cannotAssign(options));
      holdOperand(operand, options && exprThrows(options));
      held.push_back(operand);
      auto [value, has] = split(operand.value, operand.type);
      builder_.CreateStore(value, builder_.CreateStructGEP(optionsTy, slot, 0));
      builder_.CreateStore(builder_.CreateZExt(has, i32),
                           builder_.CreateStructGEP(optionsTy, slot, 5));
    }
  }
  if (options) {
    if (options->type->isUndefined()) {
      emitDiscarded(options);
    } else {
      Operand object = emitOperand(options, /*canBorrow=*/true);
      held.push_back(object);
      for (const Field &field : object.type->fields()) {
        int index = optionIndex(field.name);
        if (index < 0 || field.type->isUndefined()) // undefined is not given
          continue;
        llvm::Value *loaded = builder_.CreateLoad(
            lower(field.type), fieldAddress(object.value, object.type, field.name), field.name);
        // useGrouping of `string | boolean`: the part its tag says.
        if (const Type *u = field.type->nonOptional(); u->isTagged()) {
          llvm::Value *tag = unionTag(loaded);
          llvm::Value *isText =
              builder_.CreateICmpEQ(tag, builder_.getInt32(unsigned(UnionSlot::String)));
          llvm::Value *isFlag =
              builder_.CreateICmpEQ(tag, builder_.getInt32(unsigned(UnionSlot::Bool)));
          llvm::Value *text =
              builder_.CreateSelect(isText, taggedPart(loaded, u, UnionSlot::String),
                                    llvm::Constant::getNullValue(strTy_));
          builder_.CreateStore(
              text, builder_.CreateConstGEP2_32(
                        textTy, builder_.CreateStructGEP(optionsTy, slot, 1), 0, unsigned(index)));
          mark(hasText, isText, unsigned(index));
          llvm::Value *flag = builder_.CreateUIToFP(
              builder_.CreateAnd(isFlag, taggedPart(loaded, u, UnionSlot::Bool)), f64);
          builder_.CreateStore(flag, builder_.CreateConstGEP2_32(
                                         numberTy, builder_.CreateStructGEP(optionsTy, slot, 2), 0,
                                         unsigned(index)));
          mark(hasNumber, isFlag, unsigned(index));
          continue;
        }
        auto [value, has] = split(loaded, field.type);
        const Type *base = storedType(field.type->nonOptional());
        if (isAtomType(field.type->nonOptional()) || base->isString()) {
          builder_.CreateStore(
              value, builder_.CreateConstGEP2_32(
                         textTy, builder_.CreateStructGEP(optionsTy, slot, 1), 0, unsigned(index)));
          mark(hasText, has, unsigned(index));
          continue;
        }
        llvm::Value *number = base->isFloat()     ? value
                              : base->isInteger() ? builder_.CreateSIToFP(value, f64)
                                                  : builder_.CreateUIToFP(value, f64);
        builder_.CreateStore(number, builder_.CreateConstGEP2_32(
                                         numberTy, builder_.CreateStructGEP(optionsTy, slot, 2), 0,
                                         unsigned(index)));
        mark(hasNumber, has, unsigned(index));
      }
    }
  }
  for (Operand &operand : held)
    takeOperand(operand);
  builder_.CreateStore(hasText, builder_.CreateStructGEP(optionsTy, slot, 3));
  builder_.CreateStore(hasNumber, builder_.CreateStructGEP(optionsTy, slot, 4));
  return slot;
}

CodeGen::IntlNumber CodeGen::emitIntlNumber(Expr *arg) {
  IntlNumber n;
  n.operand = emitOperand(arg, /*canBorrow=*/true);
  llvm::Value *value = n.operand.value;
  const Type *type = n.operand.type;
  llvm::Type *i64 = builder_.getInt64Ty();
  n.d = llvm::ConstantFP::get(builder_.getDoubleTy(), 0.0);
  n.integer = n.meta = builder_.getInt64(0);
  if (type->isDecimal()) {
    n.kind = TZ_IN_DECIMAL;
    n.integer = decimalCoef(value);
    n.meta = decimalMeta(value);
  } else if (type->isInteger()) {
    n.kind = TZ_IN_INTEGER;
    n.integer = type->isInt() ? builder_.CreateSExt(value, i64) : value;
  } else {
    n.kind = TZ_IN_DOUBLE;
    n.d = value;
  }
  return n;
}

std::vector<llvm::Value *> CodeGen::intlNumberArgs(const IntlNumber &n) {
  return {builder_.getInt32(uint32_t(n.kind)), n.d, n.integer, n.meta};
}

// TzIntlShape: what an array of `type` holds, the layout of its objects with
// the fields in their order, its size, and the offset of each field of
// TZ_IF_*, -1 for those it has not.
llvm::Constant *CodeGen::intlShape(const Type *type) {
  if (llvm::Constant *shape = intlShapes_.lookup(type))
    return shape;
  static const llvm::StringLiteral fields[TZ_IF_COUNT] = {"type",  "value", "unit",      "segment",
                                                          "index", "input", "isWordLike"};
  const ObjectLayout &layout = objectLayout(type);
  const llvm::StructLayout *offsets = module_->getDataLayout().getStructLayout(layout.type);
  std::vector<llvm::Constant *> at;
  for (llvm::StringRef name : fields) {
    int32_t offset = -1;
    if (type->field(name))
      offset = int32_t(offsets->getElementOffset(layout.elements[fieldIndex(type, name)]));
    at.push_back(builder_.getInt32(uint32_t(offset)));
  }
  std::vector<unsigned> keys(type->fields().size());
  std::iota(keys.begin(), keys.end(), 0u);
  auto *offsetsTy = llvm::ArrayType::get(builder_.getInt32Ty(), TZ_IF_COUNT);
  llvm::Constant *value = llvm::ConstantStruct::getAnon(
      {typeDescriptor(type), layoutDescriptor(type, keys),
       builder_.getInt64(module_->getDataLayout().getTypeAllocSize(layout.type)),
       llvm::ConstantArray::get(offsetsTy, at)});
  auto *global =
      new llvm::GlobalVariable(*module_, value->getType(), /*isConstant=*/true,
                               llvm::GlobalValue::PrivateLinkage, value, "tz.intl.shape");
  intlShapes_[type] = global;
  return global;
}

// The code of `nf.format`, `dtf.format` or `collator.compare` as a value of
// `fnType`: it takes the object as its environment, calls the runtime and
// releases its arguments, which it owns.
llvm::Constant *CodeGen::intlBoundInfo(IntlKind kind, const Type *fnType) {
  llvm::Constant *&info = intlBound_[{kind, fnType}];
  if (info)
    return info;
  llvm::Type *ptr = builder_.getPtrTy(), *voidTy = builder_.getVoidTy(),
             *i32 = builder_.getInt32Ty(), *i64 = builder_.getInt64Ty(),
             *f64 = builder_.getDoubleTy();
  auto *code = llvm::Function::Create(closureFnType(fnType), llvm::GlobalValue::InternalLinkage,
                                      "tz.intl.bound", module_.get());
  setFunctionAttributes(code);
  llvm::IRBuilderBase::InsertPoint point = builder_.saveIP();
  llvm::Function *outer = function_;
  function_ = code;
  builder_.SetInsertPoint(llvm::BasicBlock::Create(ctx_, "entry", code));
  llvm::Value *self = code->getArg(0);
  const Type *param = fnType->params()[0];
  llvm::Value *result;
  if (kind == IntlKind::Collator) {
    llvm::Value *a = code->getArg(1), *b = code->getArg(2);
    result =
        builder_.CreateCall(processFunction("tz_collator_compare", i32, {ptr, ptr, i64, ptr, i64}),
                            {self, stringObj(a), stringMeta(a), stringObj(b), stringMeta(b)});
    releaseValue(a, Type::string());
    releaseValue(b, Type::string());
    if (fnType->ret()->isFloat())
      result = builder_.CreateSIToFP(result, f64);
  } else if (kind == IntlKind::DateTimeFormat) {
    llvm::Value *date = code->getArg(1);
    llvm::Value *time;
    if (param->isDate()) {
      time = builder_.CreateLoad(f64, dateTimeAddress(date), "time");
    } else {
      llvm::Value *ms = param->isFloat() ? date : builder_.CreateSIToFP(date, f64);
      time = builder_.CreateCall(processFunction("tz_date_clip", f64, {f64}), {ms});
    }
    result = callReturningString(processFunction("tz_dtf_format", voidTy, {ptr, ptr, f64}),
                                 {self, time});
    releaseValue(date, param);
  } else {
    llvm::Value *x = code->getArg(1);
    int numberKind = param->isDecimal()   ? TZ_IN_DECIMAL
                     : param->isInteger() ? TZ_IN_INTEGER
                                          : TZ_IN_DOUBLE;
    llvm::Value *d = numberKind == TZ_IN_DOUBLE ? x : llvm::ConstantFP::get(f64, 0.0);
    llvm::Value *integer = numberKind == TZ_IN_DECIMAL   ? decimalCoef(x)
                           : param->isInt()              ? builder_.CreateSExt(x, i64)
                           : numberKind == TZ_IN_INTEGER ? x
                                                         : builder_.getInt64(0);
    llvm::Value *meta = numberKind == TZ_IN_DECIMAL ? decimalMeta(x) : builder_.getInt64(0);
    result =
        callReturningString(processFunction("tz_nf_format", voidTy, {ptr, ptr, i32, f64, i64, i64}),
                            {self, builder_.getInt32(uint32_t(numberKind)), d, integer, meta});
    releaseValue(x, param);
  }
  builder_.CreateRet(result);
  function_ = outer;
  builder_.restoreIP(point);
  info = new llvm::GlobalVariable(
      *module_, fnInfoTy_, /*isConstant=*/true, llvm::GlobalValue::PrivateLinkage,
      llvm::ConstantStruct::get(fnInfoTy_, {code, stringLiteral(u"")}), "tz.fninfo.intl");
  return info;
}

// The locales of getCanonicalLocales and supportedLocalesOf as an owned
// array: none, one string, or the array itself.
llvm::Value *CodeGen::emitLocaleArray(Expr *arg) {
  const Type *element = Type::string();
  if (arg && arg->type == Type::array(element))
    return emitExpr(arg);
  llvm::Value *array = builder_.CreateCall(
      rt_.arrNew, {typeDescriptor(element), builder_.getInt64(0), builder_.getInt64(1)}, "locales");
  if (!arg || arg->type->isUndefined()) {
    if (arg)
      emitDiscarded(arg);
    return array;
  }
  emitPush(array, {emitExpr(arg)}, element);
  return array;
}

llvm::Value *CodeGen::emitIntlCall(CallExpr *call) {
  llvm::Type *voidTy = builder_.getVoidTy(), *i32 = builder_.getInt32Ty(),
             *i64 = builder_.getInt64Ty(), *f64 = builder_.getDoubleTy(),
             *ptr = builder_.getPtrTy();
  auto *member = dyn_cast<MemberExpr>(call->callee);
  bool throws = intlCallMayThrow(call);
  // The receiver of a method, kept while the arguments from `from` on are
  // evaluated (held, if they may throw).
  auto receiver = [&](size_t from) {
    auto rest = llvm::ArrayRef<Expr *>(call->args).drop_front(std::min(from, call->args.size()));
    Operand self = emitOperand(member->object, llvm::all_of(rest, cannotAssign));
    holdOperand(self, llvm::any_of(rest, [&](Expr *arg) { return exprThrows(arg); }));
    return self;
  };
  auto stringOf = [&](llvm::StringRef name, std::vector<llvm::Value *> args,
                      std::vector<llvm::Type *> params) {
    params.insert(params.begin(), ptr);
    return callReturningString(processFunction(name, voidTy, params), args);
  };
  auto at = [&](size_t i) -> Expr * { return i < call->args.size() ? call->args[i] : nullptr; };
  auto dropAll = [&](std::vector<Operand> &held) {
    for (const Operand &operand : held)
      dropOperand(operand);
  };

  switch (call->builtin) {
  case Builtin::IntlNew: {
    int kind = int(cast<IntLitExpr>(call->args[0])->value);
    std::vector<Operand> held;
    llvm::Value *options = emitIntlOptions(at(1), at(2), held);
    llvm::Value *object = builder_.CreateCall(processFunction("tz_intl_new", ptr, {i32, ptr}),
                                              {builder_.getInt32(uint32_t(kind)), options}, "intl");
    dropAll(held);
    emitThrowCheckIf(throws);
    return object;
  }
  case Builtin::IntlSupportedLocales:
  case Builtin::IntlCanonicalLocales: {
    bool supported = call->builtin == Builtin::IntlSupportedLocales;
    llvm::Value *locales = emitLocaleArray(at(supported ? 1 : 0));
    llvm::Value *result;
    if (supported) {
      Held heldLocales = hold(locales, Type::array(Type::string()), at(2) && exprThrows(at(2)));
      std::vector<Operand> held;
      llvm::Value *options = emitIntlOptions(nullptr, at(2), held);
      locales = take(heldLocales);
      const std::u16string &text = cast<StringLitExpr>(call->args[0])->value;
      std::string name;
      llvm::convertUTF16ToUTF8String(
          llvm::ArrayRef<llvm::UTF16>(reinterpret_cast<const llvm::UTF16 *>(text.data()),
                                      text.size()),
          name);
      result =
          builder_.CreateCall(processFunction("tz_intl_supported_locales", ptr, {ptr, ptr, ptr}),
                              {locales, options, utf8Constant(name)}, "supported");
      dropAll(held);
    } else {
      result = builder_.CreateCall(processFunction("tz_intl_canonical_locales", ptr, {ptr}),
                                   {locales}, "canonical");
    }
    releaseValue(locales, Type::array(Type::string()));
    emitThrowCheckIf(throws);
    return result;
  }
  case Builtin::IntlResolvedOptions: {
    Operand self = receiver(0);
    const Type *type = call->type;
    std::vector<unsigned> keys(type->fields().size());
    std::iota(keys.begin(), keys.end(), 0u);
    llvm::Value *object = allocObject(type, keys);
    for (const Field &field : type->fields()) {
      llvm::Value *address = fieldAddress(object, type, field.name);
      const Type *inner = field.type->nonOptional();
      llvm::Value *value;
      if (field.name == "pluralCategories") {
        value = builder_.CreateCall(processFunction("tz_pr_categories", ptr, {ptr}), {self.value});
      } else if (field.name == "useGrouping") {
        // A string, or false.
        llvm::AllocaInst *out = createStackSlot(strTy_, "grouping.out");
        llvm::Value *on = builder_.CreateICmpNE(
            builder_.CreateCall(processFunction("tz_intl_grouping", i32, {ptr, ptr}),
                                {out, self.value}),
            builder_.getInt32(0));
        llvm::Value *text =
            makeTagged(field.type, UnionSlot::String, builder_.CreateLoad(strTy_, out));
        llvm::Value *off = makeTagged(field.type, UnionSlot::Bool, builder_.getFalse());
        value = builder_.CreateSelect(on, text, off);
      } else if (inner->isString()) {
        int which = field.name == "locale" ? -1 : optionIndex(field.name);
        llvm::AllocaInst *out = createStackSlot(strTy_, "option.out");
        llvm::Value *has = builder_.CreateICmpNE(
            builder_.CreateCall(processFunction("tz_intl_text", i32, {ptr, ptr, i32}),
                                {out, self.value, builder_.getInt32(uint32_t(which))}),
            builder_.getInt32(0));
        llvm::Value *text = builder_.CreateLoad(strTy_, out);
        value = field.type->isOptional() ? makeOptional(text, has, field.type) : text;
      } else {
        llvm::Value *number =
            builder_.CreateCall(processFunction("tz_intl_number", f64, {ptr, i32}),
                                {self.value, builder_.getInt32(uint32_t(optionIndex(field.name)))});
        llvm::Value *has = builder_.CreateFCmpORD(number, number);
        number = builder_.CreateSelect(has, number, llvm::ConstantFP::get(f64, 0.0));
        llvm::Value *v = inner->isBool()
                             ? builder_.CreateFCmpUNE(number, llvm::ConstantFP::get(f64, 0.0))
                             : builder_.CreateFPToSI(number, lower(inner));
        value = field.type->isOptional() ? makeOptional(v, has, field.type) : v;
      }
      builder_.CreateStore(value, address);
    }
    dropOperand(self);
    return object;
  }
  case Builtin::IntlBound: {
    IntlKind kind = member->object->type->intlKind();
    llvm::Value *object = emitExpr(member->object); // the environment's reference
    return makeFunctionValue(intlBoundInfo(kind, call->type), object);
  }
  case Builtin::NumToLocale: {
    IntlNumber n = emitIntlNumber(member->object);
    holdOperand(n.operand, (at(0) && exprThrows(at(0))) || (at(1) && exprThrows(at(1))));
    std::vector<Operand> held;
    llvm::Value *options = emitIntlOptions(at(0), at(1), held);
    takeOperand(n.operand);
    std::vector<llvm::Value *> args = intlNumberArgs(n);
    args.push_back(options);
    llvm::Value *text = stringOf("tz_num_locale", args, {i32, f64, i64, i64, ptr});
    dropAll(held);
    dropOperand(n.operand);
    emitThrowCheckIf(throws);
    return text;
  }
  case Builtin::NfFormat:
  case Builtin::NfFormatToParts:
  case Builtin::PrSelect: {
    Operand self = receiver(0);
    IntlNumber n = emitIntlNumber(call->args[0]);
    takeOperand(self);
    std::vector<llvm::Value *> args{self.value};
    for (llvm::Value *v : intlNumberArgs(n))
      args.push_back(v);
    llvm::Value *result;
    if (call->builtin == Builtin::NfFormatToParts) {
      args.push_back(intlShape(call->type->inner()));
      result = builder_.CreateCall(
          processFunction("tz_nf_format_to_parts", ptr, {ptr, i32, f64, i64, i64, ptr}), args,
          "parts");
    } else {
      result = stringOf(call->builtin == Builtin::NfFormat ? "tz_nf_format" : "tz_pr_select", args,
                        {ptr, i32, f64, i64, i64});
    }
    dropOperand(n.operand);
    dropOperand(self);
    return result;
  }
  case Builtin::CollatorCompare: {
    Operand self = receiver(0);
    Operand a = emitOperand(call->args[0], cannotAssign(call->args[1]));
    holdOperand(a, exprThrows(call->args[1]));
    Operand b = emitOperand(call->args[1], /*canBorrow=*/true);
    takeOperand(a);
    takeOperand(self);
    llvm::Value *result =
        builder_.CreateCall(processFunction("tz_collator_compare", i32, {ptr, ptr, i64, ptr, i64}),
                            {self.value, stringObj(a.value), stringMeta(a.value),
                             stringObj(b.value), stringMeta(b.value)},
                            "order");
    dropOperand(b);
    dropOperand(a);
    dropOperand(self);
    return result;
  }
  case Builtin::StrLocaleCompare: {
    Operand self = receiver(0);
    Operand that = emitOperand(
        call->args[0], llvm::all_of(llvm::ArrayRef<Expr *>(call->args).drop_front(), cannotAssign));
    holdOperand(that, (at(1) && exprThrows(at(1))) || (at(2) && exprThrows(at(2))));
    std::vector<Operand> held;
    llvm::Value *options = emitIntlOptions(at(1), at(2), held);
    takeOperand(that);
    takeOperand(self);
    llvm::Value *result = builder_.CreateCall(
        processFunction("tz_str_locale_compare", i32, {ptr, i64, ptr, i64, ptr}),
        {stringObj(self.value), stringMeta(self.value), stringObj(that.value),
         stringMeta(that.value), options},
        "order");
    dropAll(held);
    dropOperand(that);
    dropOperand(self);
    emitThrowCheckIf(throws);
    return result;
  }
  case Builtin::StrNormalize: {
    Operand self = receiver(0);
    llvm::Value *has = builder_.getInt32(0);
    llvm::Value *formObj = llvm::ConstantPointerNull::get(builder_.getPtrTy());
    llvm::Value *formMeta = builder_.getInt64(0);
    std::optional<Operand> form;
    if (!call->args.empty()) {
      form = emitOperand(call->args[0], /*canBorrow=*/true);
      has = builder_.getInt32(1);
      formObj = stringObj(form->value);
      formMeta = stringMeta(form->value);
    }
    takeOperand(self);
    llvm::Value *text = stringOf(
        "tz_str_normalize", {stringObj(self.value), stringMeta(self.value), has, formObj, formMeta},
        {ptr, i64, i32, ptr, i64});
    if (form)
      dropOperand(*form);
    dropOperand(self);
    emitThrowCheckIf(throws);
    return text;
  }
  case Builtin::RtfFormat:
  case Builtin::RtfFormatToParts: {
    Operand self = receiver(0);
    llvm::Value *value = emitExpr(call->args[0]);
    Operand unit = emitOperand(call->args[1], /*canBorrow=*/true);
    takeOperand(self);
    llvm::Value *result;
    if (call->builtin == Builtin::RtfFormat)
      result = stringOf("tz_rtf_format",
                        {self.value, value, stringObj(unit.value), stringMeta(unit.value)},
                        {ptr, f64, ptr, i64});
    else
      result = builder_.CreateCall(
          processFunction("tz_rtf_format_to_parts", ptr, {ptr, f64, ptr, i64, ptr}),
          {self.value, value, stringObj(unit.value), stringMeta(unit.value),
           intlShape(call->type->inner())},
          "parts");
    dropOperand(unit);
    dropOperand(self);
    emitThrowCheckIf(throws);
    return result;
  }
  case Builtin::LfFormat:
  case Builtin::LfFormatToParts: {
    Operand self = receiver(0);
    Operand list = emitOperand(call->args[0], /*canBorrow=*/true);
    takeOperand(self);
    llvm::Value *result =
        call->builtin == Builtin::LfFormat
            ? stringOf("tz_lf_format", {self.value, list.value}, {ptr, ptr})
            : builder_.CreateCall(processFunction("tz_lf_format_to_parts", ptr, {ptr, ptr, ptr}),
                                  {self.value, list.value, intlShape(call->type->inner())},
                                  "parts");
    dropOperand(list);
    dropOperand(self);
    return result;
  }
  case Builtin::DnOf: {
    Operand self = receiver(0);
    Operand code = emitOperand(call->args[0], /*canBorrow=*/true);
    takeOperand(self);
    llvm::AllocaInst *out = createStackSlot(strTy_, "name.out");
    llvm::Value *has =
        builder_.CreateCall(processFunction("tz_dn_of", i32, {ptr, ptr, ptr, i64}),
                            {out, self.value, stringObj(code.value), stringMeta(code.value)});
    dropOperand(code);
    dropOperand(self);
    emitThrowCheckIf(throws);
    return makeOptional(builder_.CreateLoad(strTy_, out),
                        builder_.CreateICmpNE(has, builder_.getInt32(0)), call->type);
  }
  case Builtin::SegSegment: {
    Operand self = receiver(0);
    Operand text = emitOperand(call->args[0], /*canBorrow=*/true);
    takeOperand(self);
    llvm::Value *segments = builder_.CreateCall(
        processFunction("tz_seg_segment", ptr, {ptr, ptr, i64}),
        {self.value, stringObj(text.value), stringMeta(text.value)}, "segments");
    dropOperand(text);
    dropOperand(self);
    return segments;
  }
  case Builtin::SegmentsArray:
  case Builtin::SegmentsContaining: {
    Operand self = receiver(0);
    llvm::Value *result;
    if (call->builtin == Builtin::SegmentsArray) {
      takeOperand(self);
      result = builder_.CreateCall(processFunction("tz_segments_array", ptr, {ptr, ptr}),
                                   {self.value, intlShape(call->type->inner())}, "segments");
    } else {
      llvm::Value *index =
          call->args.empty() ? llvm::ConstantFP::get(f64, 0.0) : emitExpr(call->args[0]);
      takeOperand(self);
      result =
          builder_.CreateCall(processFunction("tz_segments_containing", ptr, {ptr, f64, ptr}),
                              {self.value, index, intlShape(call->type->nonOptional())}, "segment");
    }
    dropOperand(self);
    return result;
  }
  default:
    llvm_unreachable("not a built-in of Intl");
  }
}

} // namespace tz
