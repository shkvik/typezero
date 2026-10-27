#include "compiler/library/regex/regex_lowerer/regex_lowerer.h"
#include "compiler/library/regex/regex_native_lowerer/regex_native_lowerer.h"
#include "compiler/library/result/result_lowerer/result_lowerer.h"
#include "compiler/lower/expr/assign_lowerer/assign_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/functions/closure_lowerer/closure_lowerer.h"
#include "compiler/lower/iterators/generator_lowerer/generator_lowerer.h"
#include "compiler/lower/layouts/object_layouts/object_layouts.h"
#include "compiler/lower/layouts/type_descriptors/type_descriptors.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "runtime/library/regex/regex/regex.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include <numeric>

using llvm::BasicBlock;
using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::Value;

namespace tz {

namespace {

constexpr int64_t kConstantRefCount = int64_t(1) << 62;

constexpr unsigned kReplacerWords = 16;

}

RegexLowerer::RegexLowerer(Lowering &lowering) : lowering_(lowering) {}

llvm::FunctionCallee RegexLowerer::regexFunction(llvm::StringRef name, llvm::Type *ret,
                                                 llvm::ArrayRef<llvm::Type *> params) {
  llvm::FunctionCallee callee =
      lowering_.module_->getOrInsertFunction(name, llvm::FunctionType::get(ret, params, false));
  cast<llvm::Function>(callee.getCallee())->addFnAttr(llvm::Attribute::NoUnwind);

  return callee;
}

llvm::Constant *RegexLowerer::regexProgram(const RegexLitExpr *lit) {
  llvm::Constant *&program = regexPrograms_[lit->program];
  if (program) {
    return program;
  }
  auto *data = llvm::ConstantDataArray::get(lowering_.ctx_, lit->program);
  auto *global =
      new llvm::GlobalVariable(*lowering_.module_, data->getType(), true,
                               llvm::GlobalValue::PrivateLinkage, data, "tz.regex.program");
  global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
  global->setAlignment(llvm::Align(8));
  program = global;

  return program;
}

llvm::Value *RegexLowerer::emitRegexLiteral(RegexLitExpr *lit) {
  llvm::Type *ptr = lowering_.builder_.getPtrTy(), *i64 = lowering_.builder_.getInt64Ty();
  llvm::Constant *program = regexProgram(lit);
  llvm::Constant *match = lowering_.regexNativeLowerer().regexMatcher(lit);
  if (tz_re_flags(lit->program.data()) & (TZ_RE_GLOBAL | TZ_RE_STICKY)) {
    return lowering_.builder_.CreateCall(regexFunction("tz_re_new", ptr, {ptr, ptr}),
                                         {program, match}, "regex");
  }
  llvm::GlobalVariable *layout = lowering_.module_->getNamedGlobal("tz_re_layout");
  if (!layout) {
    layout = new llvm::GlobalVariable(*lowering_.module_, lowering_.layoutDescTy_, true,
                                      llvm::GlobalValue::ExternalLinkage, nullptr, "tz_re_layout");
  }
  auto *type = llvm::StructType::get(lowering_.ctx_, {i64, ptr, i64, ptr, ptr, ptr});
  auto *init = llvm::ConstantStruct::get(
      type, {lowering_.builder_.getInt64(kConstantRefCount), layout, lowering_.builder_.getInt64(0),
             program, llvm::ConstantPointerNull::get(lowering_.builder_.getPtrTy()), match});
  auto *global = new llvm::GlobalVariable(*lowering_.module_, type, false,
                                          llvm::GlobalValue::PrivateLinkage, init, "tz.regex");
  global->setAlignment(llvm::Align(8));

  return global;
}

llvm::Constant *RegexLowerer::matchShape(const Type *type, const RegexLitExpr *regex) {
  std::vector<uint32_t> program = regex ? regex->program : std::vector<uint32_t>();
  llvm::Constant *&shape = matchShapes_[{type, program}];
  if (shape) {
    return shape;
  }
  llvm::Type *ptr = lowering_.builder_.getPtrTy(), *i32 = lowering_.builder_.getInt32Ty();
  const llvm::DataLayout &data = lowering_.module_->getDataLayout();
  llvm::Constant *null = llvm::ConstantPointerNull::get(lowering_.builder_.getPtrTy());
  auto offsetOf = [&](const Type *object, llvm::StringRef name) -> uint32_t {
    const Lowering::ObjectLayout &layout = lowering_.objectLayouts().objectLayout(object);
    llvm::ArrayRef<Field> fields = object->fields();
    for (unsigned i = 0; i < fields.size(); ++i) {
      if (fields[i].name == name) {
        return uint32_t(data.getStructLayout(layout.type)->getElementOffset(layout.elements[i]));
      }
    }

    return 0;
  };
  auto sizeOf = [&](const Type *object) {
    return lowering_.builder_.getInt32(
        uint32_t(data.getTypeAllocSize(lowering_.objectLayouts().objectLayout(object).type)));
  };
  auto keysOf = [](const Type *object) {
    std::vector<unsigned> keys(object->fields().size());
    std::iota(keys.begin(), keys.end(), 0u);

    return keys;
  };

  llvm::Constant *layout = null, *size = lowering_.builder_.getInt32(0);
  uint32_t items = 0, index = 0, input = 0, groupsAt = 0, indices = 0;
  llvm::Constant *pairLayout = null, *pairSize = lowering_.builder_.getInt32(0);
  const Type *groups = nullptr;
  if (type->isMatch()) {
    layout = lowering_.objectLayouts().layoutDescriptor(type, keysOf(type));
    size = sizeOf(type);
    items = offsetOf(type, "%items");
    index = offsetOf(type, "index");
    input = offsetOf(type, "input");
    groupsAt = offsetOf(type, "groups");
    groups = type->field("groups")->type;
    if (const Field *field = type->field("indices")) {
      indices = offsetOf(type, "indices");
      const Type *pair = field->type->inner()->inner();
      pairLayout = lowering_.objectLayouts().layoutDescriptor(pair, keysOf(pair));
      pairSize = sizeOf(pair);
    }
  } else {
    groups = type;
  }

  llvm::Constant *groupsLayout = null, *names = null, *groupsSize = lowering_.builder_.getInt32(0);
  uint32_t nameCount = 0;
  if (groups && !groups->isOptional() && regex) {
    const uint32_t *p = regex->program.data();
    groupsLayout = lowering_.objectLayouts().layoutDescriptor(groups, keysOf(groups), 128);
    groupsSize = sizeOf(groups);
    auto *nameTy = llvm::StructType::get(lowering_.ctx_, {i32, i32, i32, i32});
    std::vector<llvm::Constant *> entries;
    for (const Field &f : groups->fields()) {
      for (uint32_t k = 1; k < tz_re_captures(p); ++k) {
        uint32_t start, length = tz_re_name(p, k, &start);
        std::vector<llvm::UTF16> name;
        for (uint32_t i = 0; i < length; ++i) {
          name.push_back(tz_re_name_unit(p, start, i));
        }
        std::string utf8;
        llvm::convertUTF16ToUTF8String(name, utf8);
        if (!length || utf8 != f.name) {
          continue;
        }
        entries.push_back(llvm::ConstantStruct::get(
            nameTy,
            {lowering_.builder_.getInt32(offsetOf(groups, f.name)), lowering_.builder_.getInt32(k),
             lowering_.builder_.getInt32(f.type->isOptional() ? 1 : 0),
             lowering_.builder_.getInt32(0)}));
      }
    }
    nameCount = uint32_t(entries.size());
    auto *arrayType = llvm::ArrayType::get(nameTy, entries.size());
    auto *global = new llvm::GlobalVariable(
        *lowering_.module_, arrayType, true, llvm::GlobalValue::PrivateLinkage,
        llvm::ConstantArray::get(arrayType, entries), "tz.groups");
    global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
    names = global;
  }

  auto *shapeTy = llvm::StructType::get(
      lowering_.ctx_, {ptr, ptr, ptr, ptr, ptr, i32, i32, i32, i32, i32, i32, i32, i32, i32, i32});
  llvm::Constant *init = llvm::ConstantStruct::get(
      shapeTy,
      {layout, lowering_.typeDescriptors().typeDescriptor(Type::optional(Type::string())),
       groupsLayout, names, pairLayout, size, lowering_.builder_.getInt32(items),
       lowering_.builder_.getInt32(index), lowering_.builder_.getInt32(input),
       lowering_.builder_.getInt32(groupsAt), lowering_.builder_.getInt32(indices), groupsSize,
       lowering_.builder_.getInt32(nameCount), pairSize, lowering_.builder_.getInt32(0)});
  auto *global = new llvm::GlobalVariable(
      *lowering_.module_, shapeTy, true, llvm::GlobalValue::PrivateLinkage, init, "tz.match.shape");
  global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
  shape = global;

  return shape;
}

llvm::Value *RegexLowerer::emitRegexCall(CallExpr *call) {
  llvm::Type *ptr = lowering_.builder_.getPtrTy(), *i32 = lowering_.builder_.getInt32Ty(),
             *i64 = lowering_.builder_.getInt64Ty();
  llvm::Type *voidTy = lowering_.builder_.getVoidTy();
  auto *member = dyn_cast<MemberExpr>(call->callee);
  auto laterCannotAssign = [&](int i) {
    return std::all_of(call->args.begin() + (i + 1), call->args.end(), AssignLowerer::cannotAssign);
  };
  auto shapeFor = [&](const Type *result) {
    return matchShape(result->nonOptional(), call->regex);
  };

  switch (call->builtin) {
  case Builtin::RegExpNew: {
    Lowering::Operand pattern =
        lowering_.refCounts().emitOperand(call->args[0], laterCannotAssign(0));
    llvm::Value *regex;
    if (call->args.size() > 1) {
      Lowering::Operand flags = lowering_.refCounts().emitOperand(call->args[1], true);
      regex = lowering_.builder_.CreateCall(
          regexFunction("tz_re_from_string", ptr, {ptr, i64, ptr, i64, i32}),
          {lowering_.stringLowerer().stringObj(pattern.value),
           lowering_.stringLowerer().stringMeta(pattern.value),
           lowering_.stringLowerer().stringObj(flags.value),
           lowering_.stringLowerer().stringMeta(flags.value), lowering_.builder_.getInt32(1)},
          "regex");
      lowering_.refCounts().dropOperand(flags);
    } else {
      regex = lowering_.builder_.CreateCall(
          regexFunction("tz_re_from_string", ptr, {ptr, i64, ptr, i64, i32}),
          {lowering_.stringLowerer().stringObj(pattern.value),
           lowering_.stringLowerer().stringMeta(pattern.value),
           llvm::ConstantPointerNull::get(lowering_.builder_.getPtrTy()),
           lowering_.builder_.getInt64(0), lowering_.builder_.getInt32(0)},
          "regex");
    }
    lowering_.refCounts().dropOperand(pattern);
    return regex;
  }
  case Builtin::RegExpParse: {
    Lowering::Operand pattern =
        lowering_.refCounts().emitOperand(call->args[0], laterCannotAssign(0));
    bool given = call->args.size() > 1;
    Lowering::Operand flags{nullptr, false, nullptr};
    if (given) {
      flags = lowering_.refCounts().emitOperand(call->args[1], true);
    }
    llvm::Value *null = llvm::ConstantPointerNull::get(ptr);
    llvm::Value *regex = lowering_.builder_.CreateCall(
        regexFunction("tz_re_parse", ptr, {ptr, i64, ptr, i64, i32, ptr}),
        {lowering_.stringLowerer().stringObj(pattern.value),
         lowering_.stringLowerer().stringMeta(pattern.value),
         given ? lowering_.stringLowerer().stringObj(flags.value) : null,
         given ? lowering_.stringLowerer().stringMeta(flags.value) : lowering_.builder_.getInt64(0),
         lowering_.builder_.getInt32(given ? 1 : 0),
         lowering_.resultLowerer().failureMaker(call->type->errType())},
        "regex");
    if (given) {
      lowering_.refCounts().dropOperand(flags);
    }
    lowering_.refCounts().dropOperand(pattern);
    return lowering_.resultLowerer().catchFailure(regex, call->type, call->loc());
  }
  case Builtin::RegExpEscape: {
    Lowering::Operand text = lowering_.refCounts().emitOperand(call->args[0], true);
    llvm::Value *result = lowering_.stringLowerer().callReturningString(
        regexFunction("tz_re_escape", voidTy, {ptr, ptr, i64}),
        {lowering_.stringLowerer().stringObj(text.value),
         lowering_.stringLowerer().stringMeta(text.value)});
    lowering_.refCounts().dropOperand(text);
    return result;
  }
  case Builtin::RegExpToString:
  case Builtin::RegExpSource:
  case Builtin::RegExpFlags:
  case Builtin::RegExpFlag: {
    Expr *object = call->builtin == Builtin::RegExpToString ? call->args[0] : member->object;
    Lowering::Operand regex = lowering_.refCounts().emitOperand(object, true);
    llvm::Value *result;
    if (call->builtin == Builtin::RegExpFlag) {
      result = lowering_.builder_.CreateCall(
          regexFunction("tz_re_flag", i32, {ptr, i64}),
          {regex.value, lowering_.exprLowerer().emitExpr(call->args[0])});
      result = lowering_.builder_.CreateICmpNE(result, lowering_.builder_.getInt32(0));
    } else {
      llvm::StringRef name = call->builtin == Builtin::RegExpToString ? "tz_re_to_string"
                             : call->builtin == Builtin::RegExpSource ? "tz_re_source"
                                                                      : "tz_re_flags_text";
      result = lowering_.stringLowerer().callReturningString(
          regexFunction(name, voidTy, {ptr, ptr}), {regex.value});
    }
    lowering_.refCounts().dropOperand(regex);
    return result;
  }
  case Builtin::RegExpTest:
  case Builtin::RegExpExec: {
    Lowering::Operand regex =
        lowering_.refCounts().emitOperand(member->object, laterCannotAssign(-1));
    Lowering::Operand text = lowering_.refCounts().emitOperand(call->args[0], true);
    llvm::Value *result;
    if (call->builtin == Builtin::RegExpTest) {
      result = lowering_.builder_.CreateCall(regexFunction("tz_re_test", i32, {ptr, ptr, i64}),
                                             {regex.value,
                                              lowering_.stringLowerer().stringObj(text.value),
                                              lowering_.stringLowerer().stringMeta(text.value)});
      result = lowering_.builder_.CreateICmpNE(result, lowering_.builder_.getInt32(0));
    } else {
      result = lowering_.builder_.CreateCall(
          regexFunction("tz_re_exec", ptr, {ptr, ptr, i64, ptr}),
          {regex.value, lowering_.stringLowerer().stringObj(text.value),
           lowering_.stringLowerer().stringMeta(text.value), shapeFor(call->type)},
          "match");
    }
    lowering_.refCounts().dropOperand(text);
    lowering_.refCounts().dropOperand(regex);
    return result;
  }
  case Builtin::StrReplaceRegexFn:
    return emitReplaceWith(call);
  case Builtin::StrMatchAll: {
    std::vector<llvm::Value *> operands =
        lowering_.exprLowerer().emitExprs({member->object, call->args[0]});
    llvm::Value *text = operands[0], *regex = operands[1];
    if (!call->regex) {
      lowering_.builder_.CreateCall(regexFunction("tz_re_check_match_all", voidTy, {ptr}), {regex});
    }
    llvm::Function *fn = lowering_.generatorLowerer().iteratorFunction(
        Builtin::StrMatchAll, Type::string(), call->args[0]->type, call->type);
    return lowering_.builder_.CreateCall(fn, {text, regex, shapeFor(call->type->yielded())},
                                         "matches");
  }
  default:
    break;
  }

  Lowering::Operand text = lowering_.refCounts().emitOperand(member->object, laterCannotAssign(-1));
  Lowering::Operand regex = lowering_.refCounts().emitOperand(call->args[0], laterCannotAssign(0));
  Lowering::Operand replacement{nullptr, false, Type::string()};
  llvm::Value *limit = lowering_.builder_.getInt64(0), *hasLimit = lowering_.builder_.getInt32(0);
  if (call->builtin == Builtin::StrReplaceRegex) {
    replacement = lowering_.refCounts().emitOperand(call->args[1], true);
  } else if (call->builtin == Builtin::StrSplitRegex && call->args.size() > 1) {
    limit = lowering_.builder_.CreateSExt(lowering_.exprLowerer().emitExpr(call->args[1]), i64);
    hasLimit = lowering_.builder_.getInt32(1);
  }
  llvm::Value *obj = lowering_.stringLowerer().stringObj(text.value),
              *meta = lowering_.stringLowerer().stringMeta(text.value);
  llvm::Value *result = nullptr;
  switch (call->builtin) {
  case Builtin::StrMatch:
    result = lowering_.builder_.CreateCall(regexFunction("tz_re_exec", ptr, {ptr, ptr, i64, ptr}),
                                           {regex.value, obj, meta, shapeFor(call->type)}, "match");
    break;
  case Builtin::StrMatchGlobal:
    result = lowering_.builder_.CreateCall(
        regexFunction("tz_re_match_global", ptr, {ptr, ptr, i64, ptr}),
        {regex.value, obj, meta, lowering_.typeDescriptors().typeDescriptor(Type::string())},
        "texts");
    result = lowering_.optionalLowerer().makeOptional(
        result, lowering_.builder_.CreateIsNotNull(result), call->type);
    break;
  case Builtin::StrSearch:
    result = lowering_.builder_.CreateSExt(
        lowering_.builder_.CreateCall(regexFunction("tz_re_search", i32, {ptr, ptr, i64}),
                                      {regex.value, obj, meta}),
        lowering_.builder_.getInt64Ty());
    break;
  case Builtin::StrReplaceRegex: {
    result = lowering_.stringLowerer().callReturningString(
        regexFunction("tz_re_replace", voidTy, {ptr, ptr, ptr, i64, ptr, i64, i32}),
        {regex.value, obj, meta, lowering_.stringLowerer().stringObj(replacement.value),
         lowering_.stringLowerer().stringMeta(replacement.value),
         lowering_.builder_.getInt32(member->name == "replaceAll")});
    lowering_.refCounts().dropOperand(replacement);
    break;
  }
  case Builtin::StrSplitRegex: {
    result = lowering_.builder_.CreateCall(
        regexFunction("tz_re_split", ptr, {ptr, ptr, i64, i64, i32, ptr}),
        {regex.value, obj, meta, limit, hasLimit,
         lowering_.typeDescriptors().typeDescriptor(call->type->inner())},
        "split");
    break;
  }
  default:
    llvm_unreachable("not a method that takes a regex");
  }
  lowering_.refCounts().dropOperand(regex);
  lowering_.refCounts().dropOperand(text);

  return result;
}

llvm::Value *RegexLowerer::emitReplaceWith(CallExpr *call) {
  llvm::Type *ptr = lowering_.builder_.getPtrTy(), *i32 = lowering_.builder_.getInt32Ty(),
             *i64 = lowering_.builder_.getInt64Ty();
  llvm::Type *voidTy = lowering_.builder_.getVoidTy();
  auto *member = cast<MemberExpr>(call->callee);
  std::vector<llvm::Value *> operands =
      lowering_.exprLowerer().emitExprs({member->object, call->args[0], call->args[1]});
  llvm::Value *text = operands[0], *regex = operands[1], *function = operands[2];
  const Type *functionType = call->args[1]->type;
  const Type *regexType = call->args[0]->type;

  auto *replacerTy = llvm::ArrayType::get(i64, kReplacerWords);
  llvm::Value *replacer = lowering_.functionFrame().createStackSlot(replacerTy, "replacer");
  auto dropAll = [&] {
    lowering_.builder_.CreateCall(regexFunction("tz_re_replacer_drop", voidTy, {ptr}), {replacer});
    lowering_.refCounts().releaseValue(function, functionType);
    lowering_.refCounts().releaseValue(regex, regexType);
    lowering_.refCounts().releaseString(text);
  };
  lowering_.builder_.CreateCall(
      regexFunction("tz_re_replacer_init", voidTy, {ptr, ptr, ptr, i64, i32}),
      {replacer, regex, lowering_.stringLowerer().stringObj(text),
       lowering_.stringLowerer().stringMeta(text),
       lowering_.builder_.getInt32(member->name == "replaceAll")});
  llvm::FunctionCallee next = regexFunction("tz_re_replacer_next", ptr, {ptr});
  llvm::FunctionCallee capture = regexFunction("tz_re_replacer_capture", i32, {ptr, ptr, ptr, i32});

  llvm::BasicBlock *headBlock = lowering_.functionFrame().newBlock("replace.next");
  llvm::BasicBlock *bodyBlock = lowering_.functionFrame().newBlock("replace.match");
  llvm::BasicBlock *endBlock = lowering_.functionFrame().newBlock("replace.end");
  lowering_.functionFrame().emitBlock(headBlock);
  llvm::Value *caps = lowering_.builder_.CreateCall(next, {replacer}, "caps");
  lowering_.builder_.CreateCondBr(lowering_.builder_.CreateIsNotNull(caps), bodyBlock, endBlock);

  lowering_.functionFrame().switchTo(bodyBlock);
  llvm::ArrayRef<const Type *> params = functionType->params();
  int groups = call->regex ? int(tz_re_captures(call->regex->program.data())) - 1 : 0;
  std::vector<llvm::Value *> args;
  for (size_t i = 0; i < params.size(); ++i) {
    const Type *param = params[i];
    int k = int(i);
    if (k <= groups) {
      llvm::AllocaInst *out =
          lowering_.functionFrame().createStackSlot(lowering_.strTy_, "capture");
      llvm::Value *has = lowering_.builder_.CreateCall(
          capture, {out, replacer, caps, lowering_.builder_.getInt32(uint32_t(k))});
      llvm::Value *value = lowering_.builder_.CreateLoad(lowering_.strTy_, out);
      args.push_back(param->isOptional()
                         ? lowering_.optionalLowerer().makeOptional(
                               value,
                               lowering_.builder_.CreateICmpNE(has, lowering_.builder_.getInt32(0)),
                               param)
                         : value);
    } else if (k == groups + 1) {
      args.push_back(
          lowering_.builder_.CreateSExt(lowering_.builder_.CreateLoad(i32, caps, "offset"), i64));
    } else if (k == groups + 2) {
      lowering_.refCounts().retainValue(text, Type::string());
      args.push_back(text);
    } else {
      args.push_back(lowering_.builder_.CreateCall(
          regexFunction("tz_re_replacer_groups", ptr, {ptr, ptr, ptr}),
          {replacer, matchShape(param, call->regex), caps}, "groups"));
    }
  }
  llvm::Value *result = lowering_.closureLowerer().callFunctionValue(function, functionType, args);
  lowering_.builder_.CreateCall(regexFunction("tz_re_replacer_put", voidTy, {ptr, ptr, ptr, i64}),
                                {replacer, caps, lowering_.stringLowerer().stringObj(result),
                                 lowering_.stringLowerer().stringMeta(result)});
  lowering_.refCounts().releaseString(result);
  lowering_.builder_.CreateBr(headBlock);

  lowering_.functionFrame().switchTo(endBlock);
  llvm::Value *replaced = lowering_.stringLowerer().callReturningString(
      regexFunction("tz_re_replacer_finish", voidTy, {ptr, ptr}), {replacer});
  lowering_.refCounts().releaseValue(function, functionType);
  lowering_.refCounts().releaseValue(regex, regexType);
  lowering_.refCounts().releaseString(text);

  return replaced;
}

Lowering::Producer RegexLowerer::matchAllProducer(llvm::Value *textSlot, llvm::Value *regexSlot,
                                                  llvm::Value *lastSlot, llvm::Value *shape) {
  Lowering::Producer producer;
  producer.run = [this, textSlot, regexSlot, lastSlot, shape](const Lowering::Consumer &consumer) {
    llvm::Type *ptr = lowering_.builder_.getPtrTy(), *i64 = lowering_.builder_.getInt64Ty();
    llvm::BasicBlock *headBlock = lowering_.functionFrame().newBlock("iter.match");
    llvm::BasicBlock *bodyBlock = lowering_.functionFrame().newBlock("iter.match.body");
    llvm::BasicBlock *endBlock = lowering_.functionFrame().newBlock("iter.match.end");
    lowering_.functionFrame().emitBlock(headBlock);
    llvm::Value *text = lowering_.builder_.CreateLoad(lowering_.strTy_, textSlot, "text");
    llvm::Value *regex = lowering_.builder_.CreateLoad(ptr, regexSlot, "regex");
    llvm::Value *match = lowering_.builder_.CreateCall(
        regexFunction("tz_re_match_next", ptr, {ptr, ptr, ptr, i64, ptr}),
        {regex, lastSlot, lowering_.stringLowerer().stringObj(text),
         lowering_.stringLowerer().stringMeta(text), shape},
        "match");
    lowering_.builder_.CreateCondBr(lowering_.builder_.CreateIsNotNull(match), bodyBlock, endBlock);
    lowering_.functionFrame().switchTo(bodyBlock);
    if (llvm::Value *sent = consumer.each(match)) {
      lowering_.refCounts().releaseValue(sent, consumer.sent);
    }
    lowering_.functionFrame().branchTo(headBlock);
    lowering_.functionFrame().switchTo(endBlock);
  };

  return producer;
}

}
