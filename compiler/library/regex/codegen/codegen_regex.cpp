// Regular expressions (docs/regex.md): literals, the methods of RegExp and
// of the strings that take one, which call the runtime
// (runtime/library/regex/regex/regex.c), and replace() with a function, whose
// loop is emitted here so that the function is called like any other.
//
// A literal's program, compiled by Sema, is a constant in the executable. A
// literal without g and y has no state that matters (lastIndex is used only
// with those flags), so it is a constant object as well, which no count ever
// frees; one with g or y is a new object each time it is evaluated, as in
// JavaScript, where each has its own lastIndex.

#include "compiler/lower/codegen/codegen.h"

#include "runtime/library/regex/regex/regex.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"

#include <numeric>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

namespace {

// Never reaches zero: the constant regexes are never freed.
constexpr int64_t kConstantRefCount = int64_t(1) << 62;
// The room the compiled code reserves for a TzReplacer (TZ_REPLACER_SIZE).
constexpr unsigned kReplacerWords = 16;

} // namespace

llvm::FunctionCallee CodeGen::regexFunction(llvm::StringRef name, llvm::Type *ret,
                                            llvm::ArrayRef<llvm::Type *> params) {
  llvm::FunctionCallee callee =
      module_->getOrInsertFunction(name, llvm::FunctionType::get(ret, params, false));
  cast<llvm::Function>(callee.getCallee())->addFnAttr(llvm::Attribute::NoUnwind);
  return callee;
}

llvm::Constant *CodeGen::regexProgram(const RegexLitExpr *lit) {
  llvm::Constant *&program = regexPrograms_[lit->program];
  if (program)
    return program;
  auto *data = llvm::ConstantDataArray::get(ctx_, lit->program);
  auto *global =
      new llvm::GlobalVariable(*module_, data->getType(), /*isConstant=*/true,
                               llvm::GlobalValue::PrivateLinkage, data, "tz.regex.program");
  global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
  global->setAlignment(llvm::Align(8));
  program = global;
  return program;
}

llvm::Value *CodeGen::emitRegexLiteral(RegexLitExpr *lit) {
  llvm::Type *ptr = builder_.getPtrTy(), *i32 = builder_.getInt32Ty(), *i64 = builder_.getInt64Ty();
  llvm::Constant *program = regexProgram(lit);
  llvm::Constant *match = regexMatcher(lit);
  if (tz_re_flags(lit->program.data()) & (TZ_RE_GLOBAL | TZ_RE_STICKY))
    return builder_.CreateCall(regexFunction("tz_re_new", ptr, {ptr, ptr}), {program, match},
                               "regex");
  // { rc, layout, lastIndex, reserved, program, owned, match }, as TzRegex;
  // written to (lastIndex, the count), so not in read-only memory.
  llvm::GlobalVariable *layout = module_->getNamedGlobal("tz_re_layout");
  if (!layout)
    layout = new llvm::GlobalVariable(*module_, layoutDescTy_, /*isConstant=*/true,
                                      llvm::GlobalValue::ExternalLinkage, nullptr, "tz_re_layout");
  auto *type = llvm::StructType::get(ctx_, {i64, ptr, i32, i32, ptr, ptr, ptr});
  auto *init = llvm::ConstantStruct::get(
      type, {builder_.getInt64(kConstantRefCount), layout, builder_.getInt32(0),
             builder_.getInt32(0), program, llvm::ConstantPointerNull::get(builder_.getPtrTy()),
             match});
  auto *global = new llvm::GlobalVariable(*module_, type, /*isConstant=*/false,
                                          llvm::GlobalValue::PrivateLinkage, init, "tz.regex");
  global->setAlignment(llvm::Align(8));
  return global;
}

// TzMatchShape: { ptr layout, ptr item_type, ptr groups_layout, ptr names, ptr pair_layout,
// i32 size, items, index, input, groups, indices, groups_size, name_count, pair_size,
// reserved }.
llvm::Constant *CodeGen::matchShape(const Type *type, const RegexLitExpr *regex) {
  std::vector<uint32_t> program = regex ? regex->program : std::vector<uint32_t>();
  llvm::Constant *&shape = matchShapes_[{type, program}];
  if (shape)
    return shape;
  llvm::Type *ptr = builder_.getPtrTy(), *i32 = builder_.getInt32Ty();
  const llvm::DataLayout &data = module_->getDataLayout();
  llvm::Constant *null = llvm::ConstantPointerNull::get(builder_.getPtrTy());
  auto offsetOf = [&](const Type *object, llvm::StringRef name) -> uint32_t {
    const ObjectLayout &layout = objectLayout(object);
    llvm::ArrayRef<Field> fields = object->fields();
    for (unsigned i = 0; i < fields.size(); ++i)
      if (fields[i].name == name)
        return uint32_t(data.getStructLayout(layout.type)->getElementOffset(layout.elements[i]));
    return 0;
  };
  auto sizeOf = [&](const Type *object) {
    return builder_.getInt32(uint32_t(data.getTypeAllocSize(objectLayout(object).type)));
  };
  auto keysOf = [](const Type *object) {
    std::vector<unsigned> keys(object->fields().size());
    std::iota(keys.begin(), keys.end(), 0u);
    return keys;
  };

  llvm::Constant *layout = null, *size = builder_.getInt32(0);
  uint32_t items = 0, index = 0, input = 0, groupsAt = 0, indices = 0;
  llvm::Constant *pairLayout = null, *pairSize = builder_.getInt32(0);
  const Type *groups = nullptr;
  if (type->isMatch()) {
    // Printed in the order of the fields: the captures, index, input,
    // groups, indices.
    layout = layoutDescriptor(type, keysOf(type));
    size = sizeOf(type);
    items = offsetOf(type, "%items");
    index = offsetOf(type, "index");
    input = offsetOf(type, "input");
    groupsAt = offsetOf(type, "groups");
    groups = type->field("groups")->type;
    if (const Field *field = type->field("indices")) {
      indices = offsetOf(type, "indices");
      const Type *pair = field->type->inner()->inner();
      pairLayout = layoutDescriptor(pair, keysOf(pair));
      pairSize = sizeOf(pair);
    }
  } else {
    groups = type; // only the groups, for replace() with a function
  }

  // The named groups: a field each, filled from the captures of that name.
  llvm::Constant *groupsLayout = null, *names = null, *groupsSize = builder_.getInt32(0);
  uint32_t nameCount = 0;
  if (groups && !groups->isOptional() && regex) {
    const uint32_t *p = regex->program.data();
    groupsLayout = layoutDescriptor(groups, keysOf(groups), 128); // TZ_LAYOUT_GROUPS
    groupsSize = sizeOf(groups);
    auto *nameTy = llvm::StructType::get(ctx_, {i32, i32, i32, i32});
    std::vector<llvm::Constant *> entries;
    for (const Field &f : groups->fields()) {
      for (uint32_t k = 1; k < tz_re_captures(p); ++k) {
        uint32_t start, length = tz_re_name(p, k, &start);
        std::vector<llvm::UTF16> name;
        for (uint32_t i = 0; i < length; ++i)
          name.push_back(tz_re_name_unit(p, start, i));
        std::string utf8;
        llvm::convertUTF16ToUTF8String(name, utf8);
        if (!length || utf8 != f.name)
          continue;
        entries.push_back(llvm::ConstantStruct::get(
            nameTy, {builder_.getInt32(offsetOf(groups, f.name)), builder_.getInt32(k),
                     builder_.getInt32(f.type->isOptional() ? 1 : 0), builder_.getInt32(0)}));
      }
    }
    nameCount = uint32_t(entries.size());
    auto *arrayType = llvm::ArrayType::get(nameTy, entries.size());
    auto *global = new llvm::GlobalVariable(
        *module_, arrayType, /*isConstant=*/true, llvm::GlobalValue::PrivateLinkage,
        llvm::ConstantArray::get(arrayType, entries), "tz.groups");
    global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
    names = global;
  }

  auto *shapeTy = llvm::StructType::get(
      ctx_, {ptr, ptr, ptr, ptr, ptr, i32, i32, i32, i32, i32, i32, i32, i32, i32, i32});
  llvm::Constant *init = llvm::ConstantStruct::get(
      shapeTy, {layout, typeDescriptor(Type::optional(Type::string())), groupsLayout, names,
                pairLayout, size, builder_.getInt32(items), builder_.getInt32(index),
                builder_.getInt32(input), builder_.getInt32(groupsAt), builder_.getInt32(indices),
                groupsSize, builder_.getInt32(nameCount), pairSize, builder_.getInt32(0)});
  auto *global =
      new llvm::GlobalVariable(*module_, shapeTy, /*isConstant=*/true,
                               llvm::GlobalValue::PrivateLinkage, init, "tz.match.shape");
  global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
  shape = global;
  return shape;
}

llvm::Value *CodeGen::emitRegexCall(CallExpr *call) {
  llvm::Type *ptr = builder_.getPtrTy(), *i32 = builder_.getInt32Ty(), *i64 = builder_.getInt64Ty();
  llvm::Type *voidTy = builder_.getVoidTy();
  auto *member = dyn_cast<MemberExpr>(call->callee);
  // Whether everything after argument i leaves the variables alone, so that
  // argument i (or the receiver, for i = -1) can be borrowed.
  auto laterCannotAssign = [&](int i) {
    return std::all_of(call->args.begin() + (i + 1), call->args.end(), cannotAssign);
  };
  auto shapeFor = [&](const Type *result) {
    return matchShape(result->nonOptional(), call->regex);
  };

  switch (call->builtin) {
  case Builtin::RegExpNew: {
    Operand pattern = emitOperand(call->args[0], laterCannotAssign(0));
    llvm::Value *regex;
    if (call->args.size() > 1) {
      holdOperand(pattern, exprThrows(call->args[1]));
      Operand flags = emitOperand(call->args[1], true);
      takeOperand(pattern);
      regex = builder_.CreateCall(
          regexFunction("tz_re_from_string", ptr, {ptr, i64, ptr, i64, i32}),
          {stringObj(pattern.value), stringMeta(pattern.value), stringObj(flags.value),
           stringMeta(flags.value), builder_.getInt32(1)},
          "regex");
      dropOperand(flags);
    } else {
      regex =
          builder_.CreateCall(regexFunction("tz_re_from_string", ptr, {ptr, i64, ptr, i64, i32}),
                              {stringObj(pattern.value), stringMeta(pattern.value),
                               llvm::ConstantPointerNull::get(builder_.getPtrTy()),
                               builder_.getInt64(0), builder_.getInt32(0)},
                              "regex");
    }
    dropOperand(pattern);
    emitThrowCheck(); // SyntaxError
    return regex;
  }
  case Builtin::RegExpEscape: {
    Operand text = emitOperand(call->args[0], true);
    llvm::Value *result =
        callReturningString(regexFunction("tz_re_escape", voidTy, {ptr, ptr, i64}),
                            {stringObj(text.value), stringMeta(text.value)});
    dropOperand(text);
    return result;
  }
  case Builtin::RegExpToString:
  case Builtin::RegExpSource:
  case Builtin::RegExpFlags:
  case Builtin::RegExpFlag: {
    Expr *object = call->builtin == Builtin::RegExpToString ? call->args[0] : member->object;
    Operand regex = emitOperand(object, true);
    llvm::Value *result;
    if (call->builtin == Builtin::RegExpFlag) {
      result = builder_.CreateCall(regexFunction("tz_re_flag", i32, {ptr, i32}),
                                   {regex.value, emitExpr(call->args[0])});
      result = builder_.CreateICmpNE(result, builder_.getInt32(0));
    } else {
      llvm::StringRef name = call->builtin == Builtin::RegExpToString ? "tz_re_to_string"
                             : call->builtin == Builtin::RegExpSource ? "tz_re_source"
                                                                      : "tz_re_flags_text";
      result = callReturningString(regexFunction(name, voidTy, {ptr, ptr}), {regex.value});
    }
    dropOperand(regex);
    return result;
  }
  case Builtin::RegExpTest:
  case Builtin::RegExpExec: {
    Operand regex = emitOperand(member->object, laterCannotAssign(-1));
    holdOperand(regex, exprThrows(call->args[0]));
    Operand text = emitOperand(call->args[0], true);
    takeOperand(regex);
    llvm::Value *result;
    if (call->builtin == Builtin::RegExpTest) {
      result = builder_.CreateCall(regexFunction("tz_re_test", i32, {ptr, ptr, i64}),
                                   {regex.value, stringObj(text.value), stringMeta(text.value)});
      result = builder_.CreateICmpNE(result, builder_.getInt32(0));
    } else {
      result = builder_.CreateCall(
          regexFunction("tz_re_exec", ptr, {ptr, ptr, i64, ptr}),
          {regex.value, stringObj(text.value), stringMeta(text.value), shapeFor(call->type)},
          "match");
    }
    dropOperand(text);
    dropOperand(regex);
    return result;
  }
  case Builtin::StrReplaceRegexFn:
    return emitReplaceWith(call);
  case Builtin::StrMatchAll: {
    // An iterator that holds the string and the regex, made where it is not
    // consumed at once (see CodeGen::prepare): a coroutine of its own. A
    // regex known only when the program runs has its g flag checked first.
    std::vector<llvm::Value *> operands = emitHeldValues({member->object, call->args[0]});
    llvm::Value *text = operands[0], *regex = operands[1];
    if (!call->regex) {
      builder_.CreateCall(regexFunction("tz_re_check_match_all", voidTy, {ptr}), {regex});
      emitThrowCheckWith([&] {
        releaseString(text);
        releaseValue(regex, call->args[0]->type);
      });
    }
    llvm::Function *fn =
        iteratorFunction(Builtin::StrMatchAll, Type::string(), call->args[0]->type, call->type);
    return builder_.CreateCall(fn, {text, regex, shapeFor(call->type->yielded())}, "matches");
  }
  default:
    break;
  }

  // The methods of strings: the string, then the regex, then the rest.
  bool restThrows = call->args.size() > 1 && exprThrows(call->args[1]);
  Operand text = emitOperand(member->object, laterCannotAssign(-1));
  holdOperand(text, exprThrows(call->args[0]) || restThrows);
  Operand regex = emitOperand(call->args[0], laterCannotAssign(0));
  holdOperand(regex, restThrows);
  // Its replacement, or its limit.
  Operand replacement{nullptr, false, Type::string()};
  llvm::Value *limit = builder_.getInt64(0), *hasLimit = builder_.getInt32(0);
  if (call->builtin == Builtin::StrReplaceRegex) {
    replacement = emitOperand(call->args[1], true);
  } else if (call->builtin == Builtin::StrSplitRegex && call->args.size() > 1) {
    limit = builder_.CreateSExt(emitExpr(call->args[1]), i64);
    hasLimit = builder_.getInt32(1);
  }
  takeOperand(text);
  takeOperand(regex);
  llvm::Value *obj = stringObj(text.value), *meta = stringMeta(text.value);
  // replaceAll without the g flag, known only when the program runs.
  bool throws = false;
  llvm::Value *result = nullptr;
  switch (call->builtin) {
  case Builtin::StrMatch:
    result = builder_.CreateCall(regexFunction("tz_re_exec", ptr, {ptr, ptr, i64, ptr}),
                                 {regex.value, obj, meta, shapeFor(call->type)}, "match");
    break;
  case Builtin::StrMatchGlobal:
    // NULL is undefined, which an optional array keeps apart.
    result = builder_.CreateCall(regexFunction("tz_re_match_global", ptr, {ptr, ptr, i64, ptr}),
                                 {regex.value, obj, meta, typeDescriptor(Type::string())}, "texts");
    result = makeOptional(result, builder_.CreateIsNotNull(result), call->type);
    break;
  case Builtin::StrSearch:
    result = builder_.CreateCall(regexFunction("tz_re_search", i32, {ptr, ptr, i64}),
                                 {regex.value, obj, meta});
    break;
  case Builtin::StrReplaceRegex: {
    result = callReturningString(
        regexFunction("tz_re_replace", voidTy, {ptr, ptr, ptr, i64, ptr, i64, i32}),
        {regex.value, obj, meta, stringObj(replacement.value), stringMeta(replacement.value),
         builder_.getInt32(member->name == "replaceAll")});
    dropOperand(replacement);
    throws = !call->regex && member->name == "replaceAll";
    break;
  }
  case Builtin::StrSplitRegex: {
    result = builder_.CreateCall(
        regexFunction("tz_re_split", ptr, {ptr, ptr, i64, i64, i32, ptr}),
        {regex.value, obj, meta, limit, hasLimit, typeDescriptor(call->type->inner())}, "split");
    break;
  }
  default:
    llvm_unreachable("not a method that takes a regex");
  }
  dropOperand(regex);
  dropOperand(text);
  emitThrowCheckIf(throws);
  return result;
}

// replace(re, f): the runtime finds the matches, then for each the function
// gets the match, the groups, the offset, the string and the named groups
// (as many of those as it takes), and what it returns replaces the match. If
// the function throws, the error goes on from here, and the replacer drops
// what it found.
llvm::Value *CodeGen::emitReplaceWith(CallExpr *call) {
  llvm::Type *ptr = builder_.getPtrTy(), *i32 = builder_.getInt32Ty(), *i64 = builder_.getInt64Ty();
  llvm::Type *voidTy = builder_.getVoidTy();
  auto *member = cast<MemberExpr>(call->callee);
  // The function may change anything, so nothing is borrowed.
  std::vector<llvm::Value *> operands =
      emitHeldValues({member->object, call->args[0], call->args[1]});
  llvm::Value *text = operands[0], *regex = operands[1], *function = operands[2];
  const Type *functionType = call->args[1]->type;
  const Type *regexType = call->args[0]->type;

  auto *replacerTy = llvm::ArrayType::get(i64, kReplacerWords);
  llvm::Value *replacer = createStackSlot(replacerTy, "replacer");
  auto dropAll = [&] {
    builder_.CreateCall(regexFunction("tz_re_replacer_drop", voidTy, {ptr}), {replacer});
    releaseValue(function, functionType);
    releaseValue(regex, regexType);
    releaseString(text);
  };
  builder_.CreateCall(regexFunction("tz_re_replacer_init", voidTy, {ptr, ptr, ptr, i64, i32}),
                      {replacer, regex, stringObj(text), stringMeta(text),
                       builder_.getInt32(member->name == "replaceAll")});
  if (!call->regex && member->name == "replaceAll")
    emitThrowCheckWith(dropAll); // without the g flag
  llvm::FunctionCallee next = regexFunction("tz_re_replacer_next", ptr, {ptr});
  llvm::FunctionCallee capture = regexFunction("tz_re_replacer_capture", i32, {ptr, ptr, ptr, i32});

  llvm::BasicBlock *headBlock = newBlock("replace.next");
  llvm::BasicBlock *bodyBlock = newBlock("replace.match");
  llvm::BasicBlock *endBlock = newBlock("replace.end");
  emitBlock(headBlock);
  llvm::Value *caps = builder_.CreateCall(next, {replacer}, "caps");
  builder_.CreateCondBr(builder_.CreateIsNotNull(caps), bodyBlock, endBlock);

  switchTo(bodyBlock);
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
      llvm::AllocaInst *out = createStackSlot(strTy_, "capture");
      llvm::Value *has =
          builder_.CreateCall(capture, {out, replacer, caps, builder_.getInt32(uint32_t(k))});
      llvm::Value *value = builder_.CreateLoad(strTy_, out);
      args.push_back(
          param->isOptional()
              ? makeOptional(value, builder_.CreateICmpNE(has, builder_.getInt32(0)), param)
              : value);
    } else if (k == groups + 1) {
      args.push_back(builder_.CreateLoad(i32, caps, "offset"));
    } else if (k == groups + 2) {
      retainValue(text, Type::string());
      args.push_back(text);
    } else {
      args.push_back(
          builder_.CreateCall(regexFunction("tz_re_replacer_groups", ptr, {ptr, ptr, ptr}),
                              {replacer, matchShape(param, call->regex), caps}, "groups"));
    }
  }
  llvm::Value *result = callFunctionValue(function, functionType, args);
  if (typeThrows(functionType))
    emitThrowCheckWith(dropAll);
  builder_.CreateCall(regexFunction("tz_re_replacer_put", voidTy, {ptr, ptr, ptr, i64}),
                      {replacer, caps, stringObj(result), stringMeta(result)});
  releaseString(result);
  builder_.CreateBr(headBlock);

  switchTo(endBlock);
  llvm::Value *replaced =
      callReturningString(regexFunction("tz_re_replacer_finish", voidTy, {ptr, ptr}), {replacer});
  releaseValue(function, functionType);
  releaseValue(regex, regexType);
  releaseString(text);
  return replaced;
}

// The loop of `s.matchAll(re)`: each match, owned, until there is none.
CodeGen::Producer CodeGen::matchAllProducer(llvm::Value *textSlot, llvm::Value *regexSlot,
                                            llvm::Value *lastSlot, llvm::Value *shape) {
  Producer producer;
  producer.run = [this, textSlot, regexSlot, lastSlot, shape](const Consumer &consumer) {
    llvm::Type *ptr = builder_.getPtrTy(), *i64 = builder_.getInt64Ty();
    llvm::BasicBlock *headBlock = newBlock("iter.match");
    llvm::BasicBlock *bodyBlock = newBlock("iter.match.body");
    llvm::BasicBlock *endBlock = newBlock("iter.match.end");
    emitBlock(headBlock);
    llvm::Value *text = builder_.CreateLoad(strTy_, textSlot, "text");
    llvm::Value *regex = builder_.CreateLoad(ptr, regexSlot, "regex");
    llvm::Value *match = builder_.CreateCall(
        regexFunction("tz_re_match_next", ptr, {ptr, ptr, ptr, i64, ptr}),
        {regex, lastSlot, stringObj(text), stringMeta(text), shape}, "match");
    builder_.CreateCondBr(builder_.CreateIsNotNull(match), bodyBlock, endBlock);
    switchTo(bodyBlock);
    if (llvm::Value *sent = consumer.each(match))
      releaseValue(sent, consumer.sent);
    branchTo(headBlock);
    switchTo(endBlock);
  };
  return producer;
}

} // namespace tz
