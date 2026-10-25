#include "compiler/library/regex/regex_lowerer/regex_lowerer.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/expr/assign_lowerer/assign_lowerer.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "compiler/lower/layouts/type_descriptors/type_descriptors.h"
#include "compiler/lower/functions/closure_lowerer/closure_lowerer.h"
#include "compiler/lower/iterators/generator_lowerer/generator_lowerer.h"
#include "compiler/lower/layouts/object_layouts/object_layouts.h"
#include "compiler/lower/errors/may_throw/may_throw.h"
#include "compiler/lower/errors/throw_lowerer/throw_lowerer.h"
#include "compiler/library/regex/regex_native_lowerer/regex_native_lowerer.h"
#include "runtime/library/regex/regex/regex.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include <numeric>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::BasicBlock;
using llvm::Value;

namespace tz {

namespace {

// Never reaches zero: the constant regexes are never freed.
constexpr int64_t kConstantRefCount = int64_t(1) << 62;

// The room the compiled code reserves for a TzReplacer (TZ_REPLACER_SIZE).
constexpr unsigned kReplacerWords = 16;

} // namespace

RegexLowerer::RegexLowerer(CodeGen &codegen) : codegen_(codegen) {}

llvm::FunctionCallee RegexLowerer::regexFunction(llvm::StringRef name, llvm::Type *ret,
                                            llvm::ArrayRef<llvm::Type *> params) {
  llvm::FunctionCallee callee =
      codegen_.module_->getOrInsertFunction(name, llvm::FunctionType::get(ret, params, false));
  cast<llvm::Function>(callee.getCallee())->addFnAttr(llvm::Attribute::NoUnwind);

  return callee;
}

llvm::Constant *RegexLowerer::regexProgram(const RegexLitExpr *lit) {
  llvm::Constant *&program = regexPrograms_[lit->program];
  if (program) {
    return program;
  }
  auto *data = llvm::ConstantDataArray::get(codegen_.ctx_, lit->program);
  auto *global =
      new llvm::GlobalVariable(*codegen_.module_, data->getType(), /*isConstant=*/true,
                               llvm::GlobalValue::PrivateLinkage, data, "tz.regex.program");
  global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
  global->setAlignment(llvm::Align(8));
  program = global;

  return program;
}

llvm::Value *RegexLowerer::emitRegexLiteral(RegexLitExpr *lit) {
  llvm::Type *ptr = codegen_.builder_.getPtrTy(), *i32 = codegen_.builder_.getInt32Ty(), *i64 = codegen_.builder_.getInt64Ty();
  llvm::Constant *program = regexProgram(lit);
  llvm::Constant *match = codegen_.regexNativeLowerer().regexMatcher(lit);
  if (tz_re_flags(lit->program.data()) & (TZ_RE_GLOBAL | TZ_RE_STICKY)) {
    return codegen_.builder_.CreateCall(regexFunction("tz_re_new", ptr, {ptr, ptr}), {program, match},
                               "regex");
  }
  // { rc, layout, lastIndex, reserved, program, owned, match }, as TzRegex;
  // written to (lastIndex, the count), so not in read-only memory.
  llvm::GlobalVariable *layout = codegen_.module_->getNamedGlobal("tz_re_layout");
  if (!layout) {
    layout = new llvm::GlobalVariable(*codegen_.module_, codegen_.layoutDescTy_, /*isConstant=*/true,
                                      llvm::GlobalValue::ExternalLinkage, nullptr, "tz_re_layout");
  }
  auto *type = llvm::StructType::get(codegen_.ctx_, {i64, ptr, i32, i32, ptr, ptr, ptr});
  auto *init =
      llvm::ConstantStruct::get(type, {codegen_.builder_.getInt64(kConstantRefCount), layout,
                                       codegen_.builder_.getInt32(0), codegen_.builder_.getInt32(0), program,
                                       llvm::ConstantPointerNull::get(codegen_.builder_.getPtrTy()), match});
  auto *global = new llvm::GlobalVariable(*codegen_.module_, type, /*isConstant=*/false,
                                          llvm::GlobalValue::PrivateLinkage, init, "tz.regex");
  global->setAlignment(llvm::Align(8));

  return global;
}

// TzMatchShape: { ptr layout, ptr item_type, ptr groups_layout, ptr names, ptr pair_layout,
// i32 size, items, index, input, groups, indices, groups_size, name_count, pair_size,
// reserved }.
llvm::Constant *RegexLowerer::matchShape(const Type *type, const RegexLitExpr *regex) {
  std::vector<uint32_t> program = regex ? regex->program : std::vector<uint32_t>();
  llvm::Constant *&shape = matchShapes_[{type, program}];
  if (shape) {
    return shape;
  }
  llvm::Type *ptr = codegen_.builder_.getPtrTy(), *i32 = codegen_.builder_.getInt32Ty();
  const llvm::DataLayout &data = codegen_.module_->getDataLayout();
  llvm::Constant *null = llvm::ConstantPointerNull::get(codegen_.builder_.getPtrTy());
  auto offsetOf = [&](const Type *object, llvm::StringRef name) -> uint32_t {
    const CodeGen::ObjectLayout &layout = codegen_.objectLayouts().objectLayout(object);
    llvm::ArrayRef<Field> fields = object->fields();
    for (unsigned i = 0; i < fields.size(); ++i) {
      if (fields[i].name == name) {
        return uint32_t(data.getStructLayout(layout.type)->getElementOffset(layout.elements[i]));
      }
    }

    return 0;
  };
  auto sizeOf = [&](const Type *object) {
    return codegen_.builder_.getInt32(uint32_t(data.getTypeAllocSize(codegen_.objectLayouts().objectLayout(object).type)));
  };
  auto keysOf = [](const Type *object) {
    std::vector<unsigned> keys(object->fields().size());
    std::iota(keys.begin(), keys.end(), 0u);

    return keys;
  };

  llvm::Constant *layout = null, *size = codegen_.builder_.getInt32(0);
  uint32_t items = 0, index = 0, input = 0, groupsAt = 0, indices = 0;
  llvm::Constant *pairLayout = null, *pairSize = codegen_.builder_.getInt32(0);
  const Type *groups = nullptr;
  if (type->isMatch()) {
    // Printed in the order of the fields: the captures, index, input,
    // groups, indices.
    layout = codegen_.objectLayouts().layoutDescriptor(type, keysOf(type));
    size = sizeOf(type);
    items = offsetOf(type, "%items");
    index = offsetOf(type, "index");
    input = offsetOf(type, "input");
    groupsAt = offsetOf(type, "groups");
    groups = type->field("groups")->type;
    if (const Field *field = type->field("indices")) {
      indices = offsetOf(type, "indices");
      const Type *pair = field->type->inner()->inner();
      pairLayout = codegen_.objectLayouts().layoutDescriptor(pair, keysOf(pair));
      pairSize = sizeOf(pair);
    }
  } else {
    groups = type; // only the groups, for replace() with a function
  }

  // The named groups: a field each, filled from the captures of that name.
  llvm::Constant *groupsLayout = null, *names = null, *groupsSize = codegen_.builder_.getInt32(0);
  uint32_t nameCount = 0;
  if (groups && !groups->isOptional() && regex) {
    const uint32_t *p = regex->program.data();
    groupsLayout = codegen_.objectLayouts().layoutDescriptor(groups, keysOf(groups), 128); // TZ_LAYOUT_GROUPS
    groupsSize = sizeOf(groups);
    auto *nameTy = llvm::StructType::get(codegen_.ctx_, {i32, i32, i32, i32});
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
            nameTy, {codegen_.builder_.getInt32(offsetOf(groups, f.name)), codegen_.builder_.getInt32(k),
                     codegen_.builder_.getInt32(f.type->isOptional() ? 1 : 0), codegen_.builder_.getInt32(0)}));
      }
    }
    nameCount = uint32_t(entries.size());
    auto *arrayType = llvm::ArrayType::get(nameTy, entries.size());
    auto *global = new llvm::GlobalVariable(
        *codegen_.module_, arrayType, /*isConstant=*/true, llvm::GlobalValue::PrivateLinkage,
        llvm::ConstantArray::get(arrayType, entries), "tz.groups");
    global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
    names = global;
  }

  auto *shapeTy = llvm::StructType::get(
      codegen_.ctx_, {ptr, ptr, ptr, ptr, ptr, i32, i32, i32, i32, i32, i32, i32, i32, i32, i32});
  llvm::Constant *init = llvm::ConstantStruct::get(
      shapeTy, {layout, codegen_.typeDescriptors().typeDescriptor(Type::optional(Type::string())), groupsLayout, names,
                pairLayout, size, codegen_.builder_.getInt32(items), codegen_.builder_.getInt32(index),
                codegen_.builder_.getInt32(input), codegen_.builder_.getInt32(groupsAt), codegen_.builder_.getInt32(indices),
                groupsSize, codegen_.builder_.getInt32(nameCount), pairSize, codegen_.builder_.getInt32(0)});
  auto *global =
      new llvm::GlobalVariable(*codegen_.module_, shapeTy, /*isConstant=*/true,
                               llvm::GlobalValue::PrivateLinkage, init, "tz.match.shape");
  global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
  shape = global;

  return shape;
}

llvm::Value *RegexLowerer::emitRegexCall(CallExpr *call) {
  llvm::Type *ptr = codegen_.builder_.getPtrTy(), *i32 = codegen_.builder_.getInt32Ty(), *i64 = codegen_.builder_.getInt64Ty();
  llvm::Type *voidTy = codegen_.builder_.getVoidTy();
  auto *member = dyn_cast<MemberExpr>(call->callee);
  // Whether everything after argument i leaves the variables alone, so that
  // argument i (or the receiver, for i = -1) can be borrowed.
  auto laterCannotAssign = [&](int i) {
    return std::all_of(call->args.begin() + (i + 1), call->args.end(), AssignLowerer::cannotAssign);
  };
  auto shapeFor = [&](const Type *result) {
    return matchShape(result->nonOptional(), call->regex);
  };

  switch (call->builtin) {
  case Builtin::RegExpNew: {
    CodeGen::Operand pattern = codegen_.refCounts().emitOperand(call->args[0], laterCannotAssign(0));
    llvm::Value *regex;
    if (call->args.size() > 1) {
      codegen_.throwLowerer().holdOperand(pattern, codegen_.mayThrow().exprThrows(call->args[1]));
      CodeGen::Operand flags = codegen_.refCounts().emitOperand(call->args[1], true);
      codegen_.throwLowerer().takeOperand(pattern);
      regex = codegen_.builder_.CreateCall(
          regexFunction("tz_re_from_string", ptr, {ptr, i64, ptr, i64, i32}),
          {codegen_.stringLowerer().stringObj(pattern.value), codegen_.stringLowerer().stringMeta(pattern.value), codegen_.stringLowerer().stringObj(flags.value),
           codegen_.stringLowerer().stringMeta(flags.value), codegen_.builder_.getInt32(1)},
          "regex");
      codegen_.refCounts().dropOperand(flags);
    } else {
      regex =
          codegen_.builder_.CreateCall(regexFunction("tz_re_from_string", ptr, {ptr, i64, ptr, i64, i32}),
                              {codegen_.stringLowerer().stringObj(pattern.value), codegen_.stringLowerer().stringMeta(pattern.value),
                               llvm::ConstantPointerNull::get(codegen_.builder_.getPtrTy()),
                               codegen_.builder_.getInt64(0), codegen_.builder_.getInt32(0)},
                              "regex");
    }
    codegen_.refCounts().dropOperand(pattern);
    codegen_.throwLowerer().emitThrowCheck(); // SyntaxError
    return regex;
  }
  case Builtin::RegExpEscape: {
    CodeGen::Operand text = codegen_.refCounts().emitOperand(call->args[0], true);
    llvm::Value *result =
        codegen_.stringLowerer().callReturningString(regexFunction("tz_re_escape", voidTy, {ptr, ptr, i64}),
                            {codegen_.stringLowerer().stringObj(text.value), codegen_.stringLowerer().stringMeta(text.value)});
    codegen_.refCounts().dropOperand(text);
    return result;
  }
  case Builtin::RegExpToString:
  case Builtin::RegExpSource:
  case Builtin::RegExpFlags:
  case Builtin::RegExpFlag: {
    Expr *object = call->builtin == Builtin::RegExpToString ? call->args[0] : member->object;
    CodeGen::Operand regex = codegen_.refCounts().emitOperand(object, true);
    llvm::Value *result;
    if (call->builtin == Builtin::RegExpFlag) {
      result = codegen_.builder_.CreateCall(regexFunction("tz_re_flag", i32, {ptr, i32}),
                                   {regex.value, codegen_.exprLowerer().emitExpr(call->args[0])});
      result = codegen_.builder_.CreateICmpNE(result, codegen_.builder_.getInt32(0));
    } else {
      llvm::StringRef name = call->builtin == Builtin::RegExpToString ? "tz_re_to_string"
                             : call->builtin == Builtin::RegExpSource ? "tz_re_source"
                                                                      : "tz_re_flags_text";
      result = codegen_.stringLowerer().callReturningString(regexFunction(name, voidTy, {ptr, ptr}), {regex.value});
    }
    codegen_.refCounts().dropOperand(regex);
    return result;
  }
  case Builtin::RegExpTest:
  case Builtin::RegExpExec: {
    CodeGen::Operand regex = codegen_.refCounts().emitOperand(member->object, laterCannotAssign(-1));
    codegen_.throwLowerer().holdOperand(regex, codegen_.mayThrow().exprThrows(call->args[0]));
    CodeGen::Operand text = codegen_.refCounts().emitOperand(call->args[0], true);
    codegen_.throwLowerer().takeOperand(regex);
    llvm::Value *result;
    if (call->builtin == Builtin::RegExpTest) {
      result = codegen_.builder_.CreateCall(regexFunction("tz_re_test", i32, {ptr, ptr, i64}),
                                   {regex.value, codegen_.stringLowerer().stringObj(text.value), codegen_.stringLowerer().stringMeta(text.value)});
      result = codegen_.builder_.CreateICmpNE(result, codegen_.builder_.getInt32(0));
    } else {
      result = codegen_.builder_.CreateCall(
          regexFunction("tz_re_exec", ptr, {ptr, ptr, i64, ptr}),
          {regex.value, codegen_.stringLowerer().stringObj(text.value), codegen_.stringLowerer().stringMeta(text.value), shapeFor(call->type)},
          "match");
    }
    codegen_.refCounts().dropOperand(text);
    codegen_.refCounts().dropOperand(regex);
    return result;
  }
  case Builtin::StrReplaceRegexFn:
    return emitReplaceWith(call);
  case Builtin::StrMatchAll: {
    // An iterator that holds the string and the regex, made where it is not
    // consumed at once (see CodeGen::prepare): a coroutine of its own. A
    // regex known only when the program runs has its g flag checked first.
    std::vector<llvm::Value *> operands = codegen_.throwLowerer().emitHeldValues({member->object, call->args[0]});
    llvm::Value *text = operands[0], *regex = operands[1];
    if (!call->regex) {
      codegen_.builder_.CreateCall(regexFunction("tz_re_check_match_all", voidTy, {ptr}), {regex});
      codegen_.throwLowerer().emitThrowCheckWith([&] {
        codegen_.refCounts().releaseString(text);
        codegen_.refCounts().releaseValue(regex, call->args[0]->type);
      });
    }
    llvm::Function *fn =
        codegen_.generatorLowerer().iteratorFunction(Builtin::StrMatchAll, Type::string(), call->args[0]->type, call->type);
    return codegen_.builder_.CreateCall(fn, {text, regex, shapeFor(call->type->yielded())}, "matches");
  }
  default:
    break;
  }

  // The methods of strings: the string, then the regex, then the rest.
  bool restThrows = call->args.size() > 1 && codegen_.mayThrow().exprThrows(call->args[1]);
  CodeGen::Operand text = codegen_.refCounts().emitOperand(member->object, laterCannotAssign(-1));
  codegen_.throwLowerer().holdOperand(text, codegen_.mayThrow().exprThrows(call->args[0]) || restThrows);
  CodeGen::Operand regex = codegen_.refCounts().emitOperand(call->args[0], laterCannotAssign(0));
  codegen_.throwLowerer().holdOperand(regex, restThrows);
  // Its replacement, or its limit.
  CodeGen::Operand replacement{nullptr, false, Type::string()};
  llvm::Value *limit = codegen_.builder_.getInt64(0), *hasLimit = codegen_.builder_.getInt32(0);
  if (call->builtin == Builtin::StrReplaceRegex) {
    replacement = codegen_.refCounts().emitOperand(call->args[1], true);
  } else if (call->builtin == Builtin::StrSplitRegex && call->args.size() > 1) {
    limit = codegen_.builder_.CreateSExt(codegen_.exprLowerer().emitExpr(call->args[1]), i64);
    hasLimit = codegen_.builder_.getInt32(1);
  }
  codegen_.throwLowerer().takeOperand(text);
  codegen_.throwLowerer().takeOperand(regex);
  llvm::Value *obj = codegen_.stringLowerer().stringObj(text.value), *meta = codegen_.stringLowerer().stringMeta(text.value);
  // replaceAll without the g flag, known only when the program runs.
  bool throws = false;
  llvm::Value *result = nullptr;
  switch (call->builtin) {
  case Builtin::StrMatch:
    result = codegen_.builder_.CreateCall(regexFunction("tz_re_exec", ptr, {ptr, ptr, i64, ptr}),
                                 {regex.value, obj, meta, shapeFor(call->type)}, "match");
    break;
  case Builtin::StrMatchGlobal:
    // NULL is undefined, which an optional array keeps apart.
    result = codegen_.builder_.CreateCall(regexFunction("tz_re_match_global", ptr, {ptr, ptr, i64, ptr}),
                                 {regex.value, obj, meta, codegen_.typeDescriptors().typeDescriptor(Type::string())}, "texts");
    result = codegen_.optionalLowerer().makeOptional(result, codegen_.builder_.CreateIsNotNull(result), call->type);
    break;
  case Builtin::StrSearch:
    result = codegen_.builder_.CreateCall(regexFunction("tz_re_search", i32, {ptr, ptr, i64}),
                                 {regex.value, obj, meta});
    break;
  case Builtin::StrReplaceRegex: {
    result = codegen_.stringLowerer().callReturningString(
        regexFunction("tz_re_replace", voidTy, {ptr, ptr, ptr, i64, ptr, i64, i32}),
        {regex.value, obj, meta, codegen_.stringLowerer().stringObj(replacement.value), codegen_.stringLowerer().stringMeta(replacement.value),
         codegen_.builder_.getInt32(member->name == "replaceAll")});
    codegen_.refCounts().dropOperand(replacement);
    throws = !call->regex && member->name == "replaceAll";
    break;
  }
  case Builtin::StrSplitRegex: {
    result = codegen_.builder_.CreateCall(
        regexFunction("tz_re_split", ptr, {ptr, ptr, i64, i64, i32, ptr}),
        {regex.value, obj, meta, limit, hasLimit, codegen_.typeDescriptors().typeDescriptor(call->type->inner())}, "split");
    break;
  }
  default:
    llvm_unreachable("not a method that takes a regex");
  }
  codegen_.refCounts().dropOperand(regex);
  codegen_.refCounts().dropOperand(text);
  codegen_.emitThrowCheckIf(throws);

  return result;
}

// replace(re, f): the runtime finds the matches, then for each the function
// gets the match, the groups, the offset, the string and the named groups
// (as many of those as it takes), and what it returns replaces the match. If
// the function throws, the error goes on from here, and the replacer drops
// what it found.
llvm::Value *RegexLowerer::emitReplaceWith(CallExpr *call) {
  llvm::Type *ptr = codegen_.builder_.getPtrTy(), *i32 = codegen_.builder_.getInt32Ty(), *i64 = codegen_.builder_.getInt64Ty();
  llvm::Type *voidTy = codegen_.builder_.getVoidTy();
  auto *member = cast<MemberExpr>(call->callee);
  // The function may change anything, so nothing is borrowed.
  std::vector<llvm::Value *> operands =
      codegen_.throwLowerer().emitHeldValues({member->object, call->args[0], call->args[1]});
  llvm::Value *text = operands[0], *regex = operands[1], *function = operands[2];
  const Type *functionType = call->args[1]->type;
  const Type *regexType = call->args[0]->type;

  auto *replacerTy = llvm::ArrayType::get(i64, kReplacerWords);
  llvm::Value *replacer = codegen_.functionFrame().createStackSlot(replacerTy, "replacer");
  auto dropAll = [&] {
    codegen_.builder_.CreateCall(regexFunction("tz_re_replacer_drop", voidTy, {ptr}), {replacer});
    codegen_.refCounts().releaseValue(function, functionType);
    codegen_.refCounts().releaseValue(regex, regexType);
    codegen_.refCounts().releaseString(text);
  };
  codegen_.builder_.CreateCall(regexFunction("tz_re_replacer_init", voidTy, {ptr, ptr, ptr, i64, i32}),
                      {replacer, regex, codegen_.stringLowerer().stringObj(text), codegen_.stringLowerer().stringMeta(text),
                       codegen_.builder_.getInt32(member->name == "replaceAll")});
  if (!call->regex && member->name == "replaceAll") {
    codegen_.throwLowerer().emitThrowCheckWith(dropAll); // without the g flag
  }
  llvm::FunctionCallee next = regexFunction("tz_re_replacer_next", ptr, {ptr});
  llvm::FunctionCallee capture = regexFunction("tz_re_replacer_capture", i32, {ptr, ptr, ptr, i32});

  llvm::BasicBlock *headBlock = codegen_.functionFrame().newBlock("replace.next");
  llvm::BasicBlock *bodyBlock = codegen_.functionFrame().newBlock("replace.match");
  llvm::BasicBlock *endBlock = codegen_.functionFrame().newBlock("replace.end");
  codegen_.functionFrame().emitBlock(headBlock);
  llvm::Value *caps = codegen_.builder_.CreateCall(next, {replacer}, "caps");
  codegen_.builder_.CreateCondBr(codegen_.builder_.CreateIsNotNull(caps), bodyBlock, endBlock);

  codegen_.functionFrame().switchTo(bodyBlock);
  // The arguments: with the groups of the regex known, the match, each
  // group, the offset, the string and the named groups; otherwise the match
  // alone.
  llvm::ArrayRef<const Type *> params = functionType->params();
  int groups = call->regex ? int(tz_re_captures(call->regex->program.data())) - 1 : 0;
  std::vector<llvm::Value *> args;
  for (size_t i = 0; i < params.size(); ++i) {
    const Type *param = params[i];
    int k = int(i);
    if (k <= groups) {
      llvm::AllocaInst *out = codegen_.functionFrame().createStackSlot(codegen_.strTy_, "capture");
      llvm::Value *has =
          codegen_.builder_.CreateCall(capture, {out, replacer, caps, codegen_.builder_.getInt32(uint32_t(k))});
      llvm::Value *value = codegen_.builder_.CreateLoad(codegen_.strTy_, out);
      args.push_back(
          param->isOptional()
              ? codegen_.optionalLowerer().makeOptional(value, codegen_.builder_.CreateICmpNE(has, codegen_.builder_.getInt32(0)), param)
              : value);
    } else if (k == groups + 1) {
      args.push_back(codegen_.builder_.CreateLoad(i32, caps, "offset"));
    } else if (k == groups + 2) {
      codegen_.refCounts().retainValue(text, Type::string());
      args.push_back(text);
    } else {
      args.push_back(
          codegen_.builder_.CreateCall(regexFunction("tz_re_replacer_groups", ptr, {ptr, ptr, ptr}),
                              {replacer, matchShape(param, call->regex), caps}, "groups"));
    }
  }
  llvm::Value *result = codegen_.closureLowerer().callFunctionValue(function, functionType, args);
  if (codegen_.typeThrows(functionType)) {
    codegen_.throwLowerer().emitThrowCheckWith(dropAll);
  }
  codegen_.builder_.CreateCall(regexFunction("tz_re_replacer_put", voidTy, {ptr, ptr, ptr, i64}),
                      {replacer, caps, codegen_.stringLowerer().stringObj(result), codegen_.stringLowerer().stringMeta(result)});
  codegen_.refCounts().releaseString(result);
  codegen_.builder_.CreateBr(headBlock);

  codegen_.functionFrame().switchTo(endBlock);
  llvm::Value *replaced =
      codegen_.stringLowerer().callReturningString(regexFunction("tz_re_replacer_finish", voidTy, {ptr, ptr}), {replacer});
  codegen_.refCounts().releaseValue(function, functionType);
  codegen_.refCounts().releaseValue(regex, regexType);
  codegen_.refCounts().releaseString(text);

  return replaced;
}

// The loop of `s.matchAll(re)`: each match, owned, until there is none.
CodeGen::Producer RegexLowerer::matchAllProducer(llvm::Value *textSlot, llvm::Value *regexSlot,
                                            llvm::Value *lastSlot, llvm::Value *shape) {
  CodeGen::Producer producer;
  producer.run = [this, textSlot, regexSlot, lastSlot, shape](const CodeGen::Consumer &consumer) {
    llvm::Type *ptr = codegen_.builder_.getPtrTy(), *i64 = codegen_.builder_.getInt64Ty();
    llvm::BasicBlock *headBlock = codegen_.functionFrame().newBlock("iter.match");
    llvm::BasicBlock *bodyBlock = codegen_.functionFrame().newBlock("iter.match.body");
    llvm::BasicBlock *endBlock = codegen_.functionFrame().newBlock("iter.match.end");
    codegen_.functionFrame().emitBlock(headBlock);
    llvm::Value *text = codegen_.builder_.CreateLoad(codegen_.strTy_, textSlot, "text");
    llvm::Value *regex = codegen_.builder_.CreateLoad(ptr, regexSlot, "regex");
    llvm::Value *match =
        codegen_.builder_.CreateCall(regexFunction("tz_re_match_next", ptr, {ptr, ptr, ptr, i64, ptr}),
                            {regex, lastSlot, codegen_.stringLowerer().stringObj(text), codegen_.stringLowerer().stringMeta(text), shape}, "match");
    codegen_.builder_.CreateCondBr(codegen_.builder_.CreateIsNotNull(match), bodyBlock, endBlock);
    codegen_.functionFrame().switchTo(bodyBlock);
    if (llvm::Value *sent = consumer.each(match)) {
      codegen_.refCounts().releaseValue(sent, consumer.sent);
    }
    codegen_.functionFrame().branchTo(headBlock);
    codegen_.functionFrame().switchTo(endBlock);
  };

  return producer;
}

} // namespace tz
