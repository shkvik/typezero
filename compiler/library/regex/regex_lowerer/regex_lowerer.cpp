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

using llvm::cast;
using llvm::dyn_cast;

namespace tz {

namespace {

constexpr int64_t kConstantRefCount = int64_t(1) << 62;

constexpr unsigned kReplacerWords = 16;

}

RegexLowerer::RegexLowerer(Lowering &owner)
    : lowering(owner) {}

llvm::FunctionCallee RegexLowerer::regexFunction(llvm::StringRef name, llvm::Type *ret,
                                                 llvm::ArrayRef<llvm::Type *> params) {
  llvm::FunctionCallee callee = this->lowering.module_->getOrInsertFunction(
      name, llvm::FunctionType::get(ret, params, false));
  cast<llvm::Function>(callee.getCallee())->addFnAttr(llvm::Attribute::NoUnwind);

  return callee;
}

llvm::Constant *RegexLowerer::regexProgram(const RegexLitExpr *lit) {
  llvm::Constant *&program = this->regexPrograms[lit->program];
  if (program) {
    return program;
  }
  auto *data = llvm::ConstantDataArray::get(this->lowering.ctx_, lit->program);
  auto *global =
      new llvm::GlobalVariable(*this->lowering.module_, data->getType(), true,
                               llvm::GlobalValue::PrivateLinkage, data, "tz.regex.program");
  global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
  global->setAlignment(llvm::Align(8));
  program = global;

  return program;
}

llvm::Value *RegexLowerer::emitRegexLiteral(RegexLitExpr *lit) {
  llvm::Type *ptr = this->lowering.builder_.getPtrTy();
  llvm::Type *i64 = this->lowering.builder_.getInt64Ty();
  llvm::Constant *program = this->regexProgram(lit);
  llvm::Constant *match = this->lowering.regexNativeLowerer().regexMatcher(lit);
  if (tz_re_flags(lit->program.data()) & (TZ_RE_GLOBAL | TZ_RE_STICKY)) {
    return this->lowering.builder_.CreateCall(this->regexFunction("tz_re_new", ptr, {ptr, ptr}),
                                              {program, match}, "regex");
  }
  llvm::GlobalVariable *layout = this->lowering.module_->getNamedGlobal("tz_re_layout");
  if (!layout) {
    layout = new llvm::GlobalVariable(*this->lowering.module_, this->lowering.layoutDescTy_, true,
                                      llvm::GlobalValue::ExternalLinkage, nullptr, "tz_re_layout");
  }
  auto *type = llvm::StructType::get(this->lowering.ctx_, {i64, ptr, i64, ptr, ptr, ptr});
  auto *init = llvm::ConstantStruct::get(
      type, {this->lowering.builder_.getInt64(kConstantRefCount), layout,
             this->lowering.builder_.getInt64(0), program,
             llvm::ConstantPointerNull::get(this->lowering.builder_.getPtrTy()), match});
  auto *global = new llvm::GlobalVariable(*this->lowering.module_, type, false,
                                          llvm::GlobalValue::PrivateLinkage, init, "tz.regex");
  global->setAlignment(llvm::Align(8));

  return global;
}

llvm::Constant *RegexLowerer::matchShape(const Type *type, const RegexLitExpr *regex) {
  std::vector<uint32_t> program = regex ? regex->program : std::vector<uint32_t>();
  llvm::Constant *&shape = this->matchShapes[{type, program}];
  if (shape) {
    return shape;
  }
  llvm::Type *ptr = this->lowering.builder_.getPtrTy();
  llvm::Type *i32 = this->lowering.builder_.getInt32Ty();
  const llvm::DataLayout &data = this->lowering.module_->getDataLayout();
  llvm::Constant *null = llvm::ConstantPointerNull::get(this->lowering.builder_.getPtrTy());
  auto offsetOf = [&](const Type *object, llvm::StringRef name) -> uint32_t {
    const Lowering::ObjectLayout &layout = this->lowering.objectLayouts().objectLayout(object);
    llvm::ArrayRef<Field> fields = object->fields();
    for (unsigned i = 0; i < fields.size(); ++i) {
      if (fields[i].name == name) {
        return uint32_t(data.getStructLayout(layout.type)->getElementOffset(layout.elements[i]));
      }
    }

    return 0;
  };
  auto sizeOf = [&](const Type *object) {
    return this->lowering.builder_.getInt32(
        uint32_t(data.getTypeAllocSize(this->lowering.objectLayouts().objectLayout(object).type)));
  };
  auto keysOf = [](const Type *object) {
    std::vector<unsigned> keys(object->fields().size());
    std::iota(keys.begin(), keys.end(), 0u);

    return keys;
  };

  llvm::Constant *layout = null;
  llvm::Constant *size = this->lowering.builder_.getInt32(0);
  uint32_t items = 0;
  uint32_t index = 0;
  uint32_t input = 0;
  uint32_t groupsAt = 0;
  uint32_t indices = 0;
  llvm::Constant *pairLayout = null;
  llvm::Constant *pairSize = this->lowering.builder_.getInt32(0);
  const Type *groups = nullptr;
  if (type->isMatch()) {
    layout = this->lowering.objectLayouts().layoutDescriptor(type, keysOf(type));
    size = sizeOf(type);
    items = offsetOf(type, "%items");
    index = offsetOf(type, "index");
    input = offsetOf(type, "input");
    groupsAt = offsetOf(type, "groups");
    groups = type->field("groups")->type;
    if (const Field *field = type->field("indices")) {
      indices = offsetOf(type, "indices");
      const Type *pair = field->type->inner()->inner();
      pairLayout = this->lowering.objectLayouts().layoutDescriptor(pair, keysOf(pair));
      pairSize = sizeOf(pair);
    }
  } else {
    groups = type;
  }

  llvm::Constant *groupsLayout = null;
  llvm::Constant *names = null;
  llvm::Constant *groupsSize = this->lowering.builder_.getInt32(0);
  uint32_t nameCount = 0;
  if (groups && !groups->isOptional() && regex) {
    const uint32_t *p = regex->program.data();
    groupsLayout = this->lowering.objectLayouts().layoutDescriptor(groups, keysOf(groups), 128);
    groupsSize = sizeOf(groups);
    auto *nameTy = llvm::StructType::get(this->lowering.ctx_, {i32, i32, i32, i32});
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
            nameTy, {this->lowering.builder_.getInt32(offsetOf(groups, f.name)),
                     this->lowering.builder_.getInt32(k),
                     this->lowering.builder_.getInt32(f.type->isOptional() ? 1 : 0),
                     this->lowering.builder_.getInt32(0)}));
      }
    }
    nameCount = uint32_t(entries.size());
    auto *arrayType = llvm::ArrayType::get(nameTy, entries.size());
    auto *global = new llvm::GlobalVariable(
        *this->lowering.module_, arrayType, true, llvm::GlobalValue::PrivateLinkage,
        llvm::ConstantArray::get(arrayType, entries), "tz.groups");
    global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
    names = global;
  }

  auto *shapeTy =
      llvm::StructType::get(this->lowering.ctx_, {ptr, ptr, ptr, ptr, ptr, i32, i32, i32, i32, i32,
                                                  i32, i32, i32, i32, i32});
  llvm::Constant *init = llvm::ConstantStruct::get(
      shapeTy,
      {layout, this->lowering.typeDescriptors().typeDescriptor(Type::optional(Type::string())),
       groupsLayout, names, pairLayout, size, this->lowering.builder_.getInt32(items),
       this->lowering.builder_.getInt32(index), this->lowering.builder_.getInt32(input),
       this->lowering.builder_.getInt32(groupsAt), this->lowering.builder_.getInt32(indices),
       groupsSize, this->lowering.builder_.getInt32(nameCount), pairSize,
       this->lowering.builder_.getInt32(0)});
  auto *global =
      new llvm::GlobalVariable(*this->lowering.module_, shapeTy, true,
                               llvm::GlobalValue::PrivateLinkage, init, "tz.match.shape");
  global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
  shape = global;

  return shape;
}

llvm::Value *RegexLowerer::emitRegexCall(CallExpr *call) {
  llvm::Type *ptr = this->lowering.builder_.getPtrTy(), *i32 = this->lowering.builder_.getInt32Ty(),
             *i64 = this->lowering.builder_.getInt64Ty();
  llvm::Type *voidTy = this->lowering.builder_.getVoidTy();
  auto *member = dyn_cast<MemberExpr>(call->callee);
  auto laterCannotAssign = [&](int i) {
    return std::all_of(call->args.begin() + (i + 1), call->args.end(), AssignLowerer::cannotAssign);
  };
  auto shapeFor = [&](const Type *result) {
    return this->matchShape(result->nonOptional(), call->regex);
  };

  switch (call->builtin()) {
    case Builtin::RegExpNew: {
      Lowering::Operand pattern =
          this->lowering.refCounts().emitOperand(call->args[0], laterCannotAssign(0));
      llvm::Value *regex;
      if (call->args.size() > 1) {
        Lowering::Operand flags = this->lowering.refCounts().emitOperand(call->args[1], true);
        regex = this->lowering.builder_.CreateCall(
            this->regexFunction("tz_re_from_string", ptr, {ptr, i64, ptr, i64, i32}),
            {this->lowering.stringLowerer().stringObj(pattern.value),
             this->lowering.stringLowerer().stringMeta(pattern.value),
             this->lowering.stringLowerer().stringObj(flags.value),
             this->lowering.stringLowerer().stringMeta(flags.value),
             this->lowering.builder_.getInt32(1)},
            "regex");
        this->lowering.refCounts().dropOperand(flags);
      } else {
        regex = this->lowering.builder_.CreateCall(
            this->regexFunction("tz_re_from_string", ptr, {ptr, i64, ptr, i64, i32}),
            {this->lowering.stringLowerer().stringObj(pattern.value),
             this->lowering.stringLowerer().stringMeta(pattern.value),
             llvm::ConstantPointerNull::get(this->lowering.builder_.getPtrTy()),
             this->lowering.builder_.getInt64(0), this->lowering.builder_.getInt32(0)},
            "regex");
      }
      this->lowering.refCounts().dropOperand(pattern);
      return regex;
    }
    case Builtin::RegExpParse: {
      Lowering::Operand pattern =
          this->lowering.refCounts().emitOperand(call->args[0], laterCannotAssign(0));
      bool given = call->args.size() > 1;
      Lowering::Operand flags{nullptr, false, nullptr};
      if (given) {
        flags = this->lowering.refCounts().emitOperand(call->args[1], true);
      }
      llvm::Value *null = llvm::ConstantPointerNull::get(ptr);
      llvm::Value *regex = this->lowering.builder_.CreateCall(
          this->regexFunction("tz_re_parse", ptr, {ptr, i64, ptr, i64, i32, ptr}),
          {this->lowering.stringLowerer().stringObj(pattern.value),
           this->lowering.stringLowerer().stringMeta(pattern.value),
           given ? this->lowering.stringLowerer().stringObj(flags.value) : null,
           given ? this->lowering.stringLowerer().stringMeta(flags.value)
                 : this->lowering.builder_.getInt64(0),
           this->lowering.builder_.getInt32(given ? 1 : 0),
           this->lowering.resultLowerer().failureMaker(call->type->errType())},
          "regex");
      if (given) {
        this->lowering.refCounts().dropOperand(flags);
      }
      this->lowering.refCounts().dropOperand(pattern);
      return this->lowering.resultLowerer().catchFailure(regex, call->type, call->loc());
    }
    case Builtin::RegExpEscape: {
      Lowering::Operand text = this->lowering.refCounts().emitOperand(call->args[0], true);
      llvm::Value *result = this->lowering.stringLowerer().callReturningString(
          this->regexFunction("tz_re_escape", voidTy, {ptr, ptr, i64}),
          {this->lowering.stringLowerer().stringObj(text.value),
           this->lowering.stringLowerer().stringMeta(text.value)});
      this->lowering.refCounts().dropOperand(text);
      return result;
    }
    case Builtin::RegExpToString:
    case Builtin::RegExpSource:
    case Builtin::RegExpFlags:
    case Builtin::RegExpFlag: {
      Expr *object = call->builtin() == Builtin::RegExpToString ? call->args[0] : member->object;
      Lowering::Operand regex = this->lowering.refCounts().emitOperand(object, true);
      llvm::Value *result;
      if (call->builtin() == Builtin::RegExpFlag) {
        result = this->lowering.builder_.CreateCall(
            this->regexFunction("tz_re_flag", i32, {ptr, i64}),
            {regex.value, this->lowering.exprLowerer().emitExpr(call->args[0])});
        result = this->lowering.builder_.CreateICmpNE(result, this->lowering.builder_.getInt32(0));
      } else {
        llvm::StringRef name = call->builtin() == Builtin::RegExpToString ? "tz_re_to_string"
                               : call->builtin() == Builtin::RegExpSource ? "tz_re_source"
                                                                          : "tz_re_flags_text";
        result = this->lowering.stringLowerer().callReturningString(
            this->regexFunction(name, voidTy, {ptr, ptr}), {regex.value});
      }
      this->lowering.refCounts().dropOperand(regex);
      return result;
    }
    case Builtin::RegExpTest:
    case Builtin::RegExpExec: {
      Lowering::Operand regex =
          this->lowering.refCounts().emitOperand(member->object, laterCannotAssign(-1));
      Lowering::Operand text = this->lowering.refCounts().emitOperand(call->args[0], true);
      llvm::Value *result;
      if (call->builtin() == Builtin::RegExpTest) {
        result = this->lowering.builder_.CreateCall(
            this->regexFunction("tz_re_test", i32, {ptr, ptr, i64}),
            {regex.value, this->lowering.stringLowerer().stringObj(text.value),
             this->lowering.stringLowerer().stringMeta(text.value)});
        result = this->lowering.builder_.CreateICmpNE(result, this->lowering.builder_.getInt32(0));
      } else {
        result = this->lowering.builder_.CreateCall(
            this->regexFunction("tz_re_exec", ptr, {ptr, ptr, i64, ptr}),
            {regex.value, this->lowering.stringLowerer().stringObj(text.value),
             this->lowering.stringLowerer().stringMeta(text.value), shapeFor(call->type)},
            "match");
      }
      this->lowering.refCounts().dropOperand(text);
      this->lowering.refCounts().dropOperand(regex);
      return result;
    }
    case Builtin::StrReplaceRegexFn:
      return this->emitReplaceWith(call);
    case Builtin::StrMatchAll: {
      std::vector<llvm::Value *> operands =
          this->lowering.exprLowerer().emitExprs({member->object, call->args[0]});
      llvm::Value *text = operands[0];
      llvm::Value *regex = operands[1];
      if (!call->regex) {
        this->lowering.builder_.CreateCall(
            this->regexFunction("tz_re_check_match_all", voidTy, {ptr}), {regex});
      }
      llvm::Function *fn = this->lowering.generatorLowerer().iteratorFunction(
          Builtin::StrMatchAll, Type::string(), call->args[0]->type, call->type);
      return this->lowering.builder_.CreateCall(fn, {text, regex, shapeFor(call->type->yielded())},
                                                "matches");
    }
    default:
      break;
  }

  Lowering::Operand text =
      this->lowering.refCounts().emitOperand(member->object, laterCannotAssign(-1));
  Lowering::Operand regex =
      this->lowering.refCounts().emitOperand(call->args[0], laterCannotAssign(0));
  Lowering::Operand replacement{nullptr, false, Type::string()};
  llvm::Value *limit = this->lowering.builder_.getInt64(0);
  llvm::Value *hasLimit = this->lowering.builder_.getInt32(0);
  if (call->builtin() == Builtin::StrReplaceRegex) {
    replacement = this->lowering.refCounts().emitOperand(call->args[1], true);
  } else if (call->builtin() == Builtin::StrSplitRegex && call->args.size() > 1) {
    limit = this->lowering.builder_.CreateSExt(this->lowering.exprLowerer().emitExpr(call->args[1]),
                                               i64);
    hasLimit = this->lowering.builder_.getInt32(1);
  }
  llvm::Value *obj = this->lowering.stringLowerer().stringObj(text.value),
              *meta = this->lowering.stringLowerer().stringMeta(text.value);
  llvm::Value *result = nullptr;
  switch (call->builtin()) {
    case Builtin::StrMatch:
      result = this->lowering.builder_.CreateCall(
          this->regexFunction("tz_re_exec", ptr, {ptr, ptr, i64, ptr}),
          {regex.value, obj, meta, shapeFor(call->type)}, "match");
      break;
    case Builtin::StrMatchGlobal:
      result = this->lowering.builder_.CreateCall(
          this->regexFunction("tz_re_match_global", ptr, {ptr, ptr, i64, ptr}),
          {regex.value, obj, meta, this->lowering.typeDescriptors().typeDescriptor(Type::string())},
          "texts");
      result = this->lowering.optionalLowerer().makeOptional(
          result, this->lowering.builder_.CreateIsNotNull(result), call->type);
      break;
    case Builtin::StrSearch:
      result = this->lowering.builder_.CreateSExt(
          this->lowering.builder_.CreateCall(
              this->regexFunction("tz_re_search", i32, {ptr, ptr, i64}), {regex.value, obj, meta}),
          this->lowering.builder_.getInt64Ty());
      break;
    case Builtin::StrReplaceRegex: {
      result = this->lowering.stringLowerer().callReturningString(
          this->regexFunction("tz_re_replace", voidTy, {ptr, ptr, ptr, i64, ptr, i64, i32}),
          {regex.value, obj, meta, this->lowering.stringLowerer().stringObj(replacement.value),
           this->lowering.stringLowerer().stringMeta(replacement.value),
           this->lowering.builder_.getInt32(member->name == "replaceAll")});
      this->lowering.refCounts().dropOperand(replacement);
      break;
    }
    case Builtin::StrSplitRegex: {
      result = this->lowering.builder_.CreateCall(
          this->regexFunction("tz_re_split", ptr, {ptr, ptr, i64, i64, i32, ptr}),
          {regex.value, obj, meta, limit, hasLimit,
           this->lowering.typeDescriptors().typeDescriptor(call->type->inner())},
          "split");
      break;
    }
    default:
      llvm_unreachable("not a method that takes a regex");
  }
  this->lowering.refCounts().dropOperand(regex);
  this->lowering.refCounts().dropOperand(text);

  return result;
}

llvm::Value *RegexLowerer::emitReplaceWith(CallExpr *call) {
  llvm::Type *ptr = this->lowering.builder_.getPtrTy(), *i32 = this->lowering.builder_.getInt32Ty(),
             *i64 = this->lowering.builder_.getInt64Ty();
  llvm::Type *voidTy = this->lowering.builder_.getVoidTy();
  auto *member = cast<MemberExpr>(call->callee);
  std::vector<llvm::Value *> operands =
      this->lowering.exprLowerer().emitExprs({member->object, call->args[0], call->args[1]});
  llvm::Value *text = operands[0];
  llvm::Value *regex = operands[1];
  llvm::Value *function = operands[2];
  const Type *functionType = call->args[1]->type;
  const Type *regexType = call->args[0]->type;

  auto *replacerTy = llvm::ArrayType::get(i64, kReplacerWords);
  llvm::Value *replacer = this->lowering.functionFrame().createStackSlot(replacerTy, "replacer");
  auto dropAll = [&] {
    this->lowering.builder_.CreateCall(this->regexFunction("tz_re_replacer_drop", voidTy, {ptr}),
                                       {replacer});
    this->lowering.refCounts().releaseValue(function, functionType);
    this->lowering.refCounts().releaseValue(regex, regexType);
    this->lowering.refCounts().releaseString(text);
  };
  this->lowering.builder_.CreateCall(
      this->regexFunction("tz_re_replacer_init", voidTy, {ptr, ptr, ptr, i64, i32}),
      {replacer, regex, this->lowering.stringLowerer().stringObj(text),
       this->lowering.stringLowerer().stringMeta(text),
       this->lowering.builder_.getInt32(member->name == "replaceAll")});
  llvm::FunctionCallee next = this->regexFunction("tz_re_replacer_next", ptr, {ptr});
  llvm::FunctionCallee capture =
      this->regexFunction("tz_re_replacer_capture", i32, {ptr, ptr, ptr, i32});

  llvm::BasicBlock *headBlock = this->lowering.functionFrame().newBlock("replace.next");
  llvm::BasicBlock *bodyBlock = this->lowering.functionFrame().newBlock("replace.match");
  llvm::BasicBlock *endBlock = this->lowering.functionFrame().newBlock("replace.end");
  this->lowering.functionFrame().emitBlock(headBlock);
  llvm::Value *caps = this->lowering.builder_.CreateCall(next, {replacer}, "caps");
  this->lowering.builder_.CreateCondBr(this->lowering.builder_.CreateIsNotNull(caps), bodyBlock,
                                       endBlock);

  this->lowering.functionFrame().switchTo(bodyBlock);
  llvm::ArrayRef<const Type *> params = functionType->params();
  int groups = call->regex ? int(tz_re_captures(call->regex->program.data())) - 1 : 0;
  std::vector<llvm::Value *> args;
  for (size_t i = 0; i < params.size(); ++i) {
    const Type *param = params[i];
    int k = int(i);
    if (k <= groups) {
      llvm::AllocaInst *out =
          this->lowering.functionFrame().createStackSlot(this->lowering.strTy_, "capture");
      llvm::Value *has = this->lowering.builder_.CreateCall(
          capture, {out, replacer, caps, this->lowering.builder_.getInt32(uint32_t(k))});
      llvm::Value *value = this->lowering.builder_.CreateLoad(this->lowering.strTy_, out);
      args.push_back(param->isOptional() ? this->lowering.optionalLowerer().makeOptional(
                                               value,
                                               this->lowering.builder_.CreateICmpNE(
                                                   has, this->lowering.builder_.getInt32(0)),
                                               param)
                                         : value);
    } else if (k == groups + 1) {
      args.push_back(this->lowering.builder_.CreateSExt(
          this->lowering.builder_.CreateLoad(i32, caps, "offset"), i64));
    } else if (k == groups + 2) {
      this->lowering.refCounts().retainValue(text, Type::string());
      args.push_back(text);
    } else {
      args.push_back(this->lowering.builder_.CreateCall(
          this->regexFunction("tz_re_replacer_groups", ptr, {ptr, ptr, ptr}),
          {replacer, this->matchShape(param, call->regex), caps}, "groups"));
    }
  }
  llvm::Value *result =
      this->lowering.closureLowerer().callFunctionValue(function, functionType, args);
  this->lowering.builder_.CreateCall(
      this->regexFunction("tz_re_replacer_put", voidTy, {ptr, ptr, ptr, i64}),
      {replacer, caps, this->lowering.stringLowerer().stringObj(result),
       this->lowering.stringLowerer().stringMeta(result)});
  this->lowering.refCounts().releaseString(result);
  this->lowering.builder_.CreateBr(headBlock);

  this->lowering.functionFrame().switchTo(endBlock);
  llvm::Value *replaced = this->lowering.stringLowerer().callReturningString(
      this->regexFunction("tz_re_replacer_finish", voidTy, {ptr, ptr}), {replacer});
  this->lowering.refCounts().releaseValue(function, functionType);
  this->lowering.refCounts().releaseValue(regex, regexType);
  this->lowering.refCounts().releaseString(text);

  return replaced;
}

Lowering::Producer RegexLowerer::matchAllProducer(llvm::Value *textSlot, llvm::Value *regexSlot,
                                                  llvm::Value *lastSlot, llvm::Value *shape) {
  Lowering::Producer producer;
  producer.run = [this, textSlot, regexSlot, lastSlot, shape](const Lowering::Consumer &consumer) {
    llvm::Type *ptr = this->lowering.builder_.getPtrTy();
    llvm::Type *i64 = this->lowering.builder_.getInt64Ty();
    llvm::BasicBlock *headBlock = this->lowering.functionFrame().newBlock("iter.match");
    llvm::BasicBlock *bodyBlock = this->lowering.functionFrame().newBlock("iter.match.body");
    llvm::BasicBlock *endBlock = this->lowering.functionFrame().newBlock("iter.match.end");
    this->lowering.functionFrame().emitBlock(headBlock);
    llvm::Value *text = this->lowering.builder_.CreateLoad(this->lowering.strTy_, textSlot, "text");
    llvm::Value *regex = this->lowering.builder_.CreateLoad(ptr, regexSlot, "regex");
    llvm::Value *match = this->lowering.builder_.CreateCall(
        this->regexFunction("tz_re_match_next", ptr, {ptr, ptr, ptr, i64, ptr}),
        {regex, lastSlot, this->lowering.stringLowerer().stringObj(text),
         this->lowering.stringLowerer().stringMeta(text), shape},
        "match");
    this->lowering.builder_.CreateCondBr(this->lowering.builder_.CreateIsNotNull(match), bodyBlock,
                                         endBlock);
    this->lowering.functionFrame().switchTo(bodyBlock);
    if (llvm::Value *sent = consumer.each(match)) {
      this->lowering.refCounts().releaseValue(sent, consumer.sent);
    }
    this->lowering.functionFrame().branchTo(headBlock);
    this->lowering.functionFrame().switchTo(endBlock);
  };

  return producer;
}

}
