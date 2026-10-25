#include "compiler/lower/errors/throw_calls/throw_calls.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include <functional>
#include <map>
#include <optional>

using llvm::BasicBlock;
using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::Value;

namespace tz {

ThrowCalls::ThrowCalls(ThrowScanner &scanner) : scanner_(scanner) {}

void ThrowCalls::intrinsic(bool caught) {
  if (!caught) {
    scanner_.out_.intrinsic = true;
  }
}

void ThrowCalls::resumes(bool caught) {
  if (!caught) {
    scanner_.out_.resumes = true;
  }
}

void ThrowCalls::callee(const FunctionDecl *fn, bool caught, const CallExpr *site) {
  if (!fn) {
    return;
  }
  if (!caught) {
    scanner_.out_.calls.push_back(fn);
  }
  scanner_.out_.allCalls.push_back({fn, site});
}

void ThrowCalls::valueCall(const Type *type, bool caught) {
  if (!type) {
    return;
  }
  type = type->nonOptional();
  if (!type->isFunction()) {
    return;
  }
  if (!caught) {
    scanner_.out_.valueCalls.push_back(type);
  }
  scanner_.out_.allValueCalls.push_back(type);
}

void ThrowCalls::value(const Type *type, const FunctionDecl *fn) {
  if (type && fn) {
    scanner_.out_.values.push_back({type->nonOptional(), fn});
  }
}

void ThrowCalls::callback(const Expr *arg, bool caught) {
  scanner_.expr(arg, caught);
  if (auto *fe = llvm::dyn_cast<FunctionExpr>(arg)) {
    callee(fe->fn, caught, nullptr);
  } else if (arg->type) {
    valueCall(arg->type, caught);
  }
}

void ThrowCalls::target(const Expr *target, bool caught) {
  if (auto *index = llvm::dyn_cast<IndexExpr>(target)) {
    scanner_.expr(index->object, caught);
    scanner_.expr(index->index, caught);
  } else if (auto *member = llvm::dyn_cast<MemberExpr>(target)) {
    scanner_.expr(member->object, caught);
  }
}

bool ThrowCalls::operatorThrows(BinaryOp op, const Type *type, const Expr *rhs) {
  if (!type) {
    return false;
  }
  if (type->isDecimal()) {
    return op == BinaryOp::Add || op == BinaryOp::Sub || op == BinaryOp::Mul ||
           op == BinaryOp::Div || op == BinaryOp::Rem || op == BinaryOp::Pow;
  }
  if (!type->isInteger()) {
    return false;
  }
  if (op == BinaryOp::Pow) {
    return true;
  }
  if (op != BinaryOp::Div && op != BinaryOp::Rem) {
    return false;
  }
  std::optional<int64_t> divisor = constantInt(rhs);

  return !divisor || *divisor == 0;
}

void ThrowCalls::call(const CallExpr *c, bool caught) {
  auto *member = llvm::dyn_cast<MemberExpr>(c->callee);
  Builtin builtin = c->builtin;
  if (builtin == Builtin::None) {
    if (member) {
      scanner_.expr(member->object, caught);
    }
    for (const Expr *arg : c->args) {
      scanner_.expr(arg, caught);
    }
    // A method of a union: that of any member may run.
    for (const CallExpr::UnionMethod &m : c->unionMethods) {
      if (m.method && m.virtualCall) {
        forEachImplementation(m.method->decl,
                              [&](const FunctionDecl *fn) { callee(fn, caught, c); });
      } else if (m.method) {
        callee(m.method->decl, caught, c);
      } else {
        valueCall(llvm::cast<MemberExpr>(c->callee)->type, caught);
      }
    }
    if (!c->unionMethods.empty()) {
      return;
    }
    if (c->method && c->virtualCall) {
      forEachImplementation(c->method->decl,
                            [&](const FunctionDecl *fn) { callee(fn, caught, c); });
    } else if (c->method) {
      callee(c->method->decl, caught, c);
    } else if (c->func) {
      callee(c->func->decl, caught, c);
    } else {
      scanner_.expr(c->callee, caught);
      valueCall(c->callee->type, caught);
    }
    return;
  }
  if (member) {
    scanner_.expr(member->object, caught);
  }
  // asyncMap and asyncAndThen of a Result call their function now.
  bool ofResult = (builtin == Builtin::ResultAsyncMap || builtin == Builtin::ResultAsyncAndThen) &&
                  member && member->object->type && member->object->type->isResult();
  bool callbacks = (builtin >= Builtin::ArrForEach && builtin <= Builtin::ArrSortWith) ||
                   isResultCallback(builtin) || isIteratorSink(builtin) ||
                   (isIteratorStage(builtin) && c->fused) ||
                   builtin == Builtin::StrReplaceRegexFn || builtin == Builtin::MapForEach ||
                   builtin == Builtin::DictGroupBy || ofResult;
  for (size_t i = 0; i < c->args.size(); ++i) {
    const Expr *arg = c->args[i];
    bool function = arg->type && arg->type->nonOptional()->isFunction();
    if ((callbacks && function) || (builtin == Builtin::ArrayFrom && i == 1)) {
      callback(arg, caught);
    } else {
      scanner_.expr(arg, caught);
    }
  }
  switch (builtin) {
  case Builtin::ArrReduce:
  case Builtin::ArrReduceRight:
  case Builtin::IterReduce:
    if (c->args.size() < 2) {
      intrinsic(caught); // of nothing, without an initial value
    }
    break;
  case Builtin::IterTake:
  case Builtin::IterDrop: {
    std::optional<int64_t> count = c->args.empty() ? std::nullopt : constantInt(c->args[0]);
    if (!count || *count < 0) {
      intrinsic(caught);
    }
    break;
  }
  case Builtin::IterNext:
    resumes(caught);
    break;
  case Builtin::SafeTry:
    // Its generator runs once, now.
    if (!c->args.empty()) {
      if (auto *fe = llvm::dyn_cast<FunctionExpr>(c->args[0])) {
        callee(fe->fn, caught, nullptr);
      }
    }
    break;
  case Builtin::ResultFromThrowable:
    scanner_.out_.fromThrowables.push_back({c->type, c->args.size() > 1 ? c->args[1] : nullptr});
    break;
  case Builtin::ParseInt:
  case Builtin::ParseFloat:
    if (c->type && c->type->isInteger()) {
      intrinsic(caught); // NaN or a number that does not fit
    }
    break;
  case Builtin::StrRepeat:
  case Builtin::StrPadStart:
  case Builtin::StrPadEnd:
  case Builtin::FromCodePoint:
  case Builtin::UriEncodeComponent: // a lone surrogate
  case Builtin::UriEncode:
  case Builtin::UriDecodeComponent: // an escape that is not UTF-8
  case Builtin::UriDecode:
  case Builtin::ResultUnsafeUnwrap:
  case Builtin::ResultUnsafeUnwrapErr:
    intrinsic(caught);
    break;
  case Builtin::NumToFixed:
    if (!c->args.empty()) {
      intrinsic(caught);
    }
    break;
  case Builtin::NumToRadix:
    intrinsic(caught);
    break;
  case Builtin::RegExpNew: // a SyntaxError
  case Builtin::JsonParse:
  case Builtin::JsonParseAs:
  case Builtin::FromJson:
  // Reflection checks its values when the program runs, and runs the code
  // of the class.
  case Builtin::ReflectGet:
  case Builtin::ReflectApply:
  case Builtin::ReflectConstruct:
  case Builtin::FromJsonClass:
    intrinsic(caught);
    break;
  case Builtin::StrMatchAll:
    // Without the g flag, a TypeError, where the regex is known only when
    // the program runs.
    if (!c->regex) {
      intrinsic(caught);
    }
    break;
  case Builtin::StrReplaceRegex:
  case Builtin::StrReplaceRegexFn:
    if (!c->regex && member && member->name == "replaceAll") {
      intrinsic(caught);
    }
    break;
  case Builtin::JsonStringify:
    if (!c->args.empty() && c->args[0]->type && stringifyMayThrow(c->args[0]->type)) {
      intrinsic(caught); // a value inside itself
    }
    break;
  case Builtin::DateToISOString: // of an invalid date
    intrinsic(caught);
    break;
  case Builtin::DtfFormat:
  case Builtin::DtfFormatToParts:
    if (!c->args.empty()) { // a date that may be invalid
      intrinsic(caught);
    }
    break;
  case Builtin::DateToLocale:
    if (dateOptionsMayThrow(llvm::ArrayRef<Expr *>(c->args).drop_front())) {
      intrinsic(caught);
    }
    break;
  case Builtin::DtfNew:
    if (dateOptionsMayThrow(c->args)) {
      intrinsic(caught);
    }
    break;
  case Builtin::MetaDefine:
    // A key `design:*` that the program wrote, which only the compiler
    // writes (runtime/library/reflect/meta/meta.c).
    if (!c->metaKey && !c->designRecord && !c->args.empty() &&
        !llvm::isa<StringLitExpr>(c->args[0])) {
      intrinsic(caught);
    }
    break;
  case Builtin::ObjectKeys:
  case Builtin::ObjectValues:
  case Builtin::ObjectEntries:
    // Of a Json that is null or undefined, but for `for...in`.
    if (!c->forIn && !c->args.empty() && c->args[0]->type &&
        c->args[0]->type->nonOptional()->isJson()) {
      intrinsic(caught);
    }
    break;
  default:
    if (isRoundingBuiltin(builtin) && c->type && c->type->isInteger() && !c->args.empty() &&
        c->args[0]->type != c->type) {
      intrinsic(caught); // a number that does not fit in the integer
    }
    if (isDecimalBuiltin(builtin)) {
      intrinsic(caught);
    }
    if (isFsBuiltin(builtin) || isChildBuiltin(builtin)) { // an error of the system
      intrinsic(caught);
    }
    if (isCryptoBuiltin(builtin)) { // the errors of node:crypto
      intrinsic(caught);
    }
    if (isBytesBuiltin(builtin) && bytesMayThrow(c)) {
      intrinsic(caught);
    }
    if (isIntlBuiltin(builtin) && intlCallMayThrow(c)) {
      intrinsic(caught);
    }
    break;
  }
}

} // namespace tz
