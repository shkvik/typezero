#include "compiler/check/expr/new_checker/new_checker.h"
#include "compiler/check/scopes/scopes.h"
#include "compiler/check/types/type_resolver/type_resolver.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/expr/member_checker/member_checker.h"
#include "compiler/check/async/promise_checker/promise_checker.h"
#include "compiler/check/classes/method_call_checker/method_call_checker.h"
#include "compiler/check/classes/class_value_checker/class_value_checker.h"
#include "compiler/check/objects/dict_new_checker/dict_new_checker.h"
#include "compiler/check/flow/index_proofs/index_proofs.h"
#include "compiler/check/top_level_checker/top_level_checker.h"
#include "compiler/library/bytes/bytes_checker/bytes_checker.h"
#include "compiler/library/bytes/bytes_new_checker/bytes_new_checker.h"
#include "compiler/library/crypto/crypto_checker/crypto_checker.h"
#include "compiler/library/date/date_checker/date_checker.h"
#include "compiler/library/date/date_format_checker/date_format_checker.h"
#include "compiler/library/decimal/decimal_checker/decimal_checker.h"
#include "compiler/library/tz_modules/library_calls/library_calls.h"
#include "compiler/library/intl/intl_checker/intl_checker.h"
#include "compiler/library/builtin_modules/builtin_module_checker/builtin_module_checker.h"
#include "compiler/library/regex/regex_checker/regex_checker.h"
#include "compiler/library/weakref/weak_ref_checker/weak_ref_checker.h"
#include "llvm/ADT/STLExtras.h"
#include <cstring>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::dyn_cast_or_null;

namespace tz {

const char *whyUnsupported(llvm::StringRef name) {
  if (name == "eval") {
    return "a compiled program cannot run code from a string";
  }
  if (name == "Function") {
    return "a compiled program cannot run code from a string; write the function itself, e.g. "
           "'(a: int) => a'";
  }
  if (name == "Proxy") {
    return "the fields of an object are read and written straight in memory, so nothing can "
           "intercept that";
  }
  if (name == "require") {
    return "files are imported by name: 'import { a } from \"./x\"'";
  }
  if (name == "module" || name == "exports") {
    return "files export by name: 'export function f() {...}', or 'export { a, b }'";
  }
  if (name == "escape") {
    return "it is a legacy function whose '%uXXXX' escapes nothing decodes but 'unescape'; "
           "encode with 'encodeURIComponent(s)'";
  }
  if (name == "unescape") {
    return "it is a legacy function that decodes the '%uXXXX' escapes of 'escape'; decode with "
           "'decodeURIComponent(s)'";
  }

  return nullptr;
}

NewChecker::NewChecker(Sema &sema) : sema_(sema) {}

Expr *NewChecker::checkNew(NewExpr *expr, const Type *expected) {
  expr->type = Type::error();
  auto checkArgsLoosely = [&] {
    for (Expr *&arg : expr->args) {
      arg = sema_.exprChecker().checkExpr(arg);
    }
  };
  // `new cls()` and `new route.cls()` of a variable that holds a class
  // (docs/decorators.md, sections 5 and 6).
  if (!expr->callee && !expr->name.empty() && sema_.topLevelChecker().lookupValue(expr->name).var) {
    expr->callee = sema_.ctx_.make<NameExpr>(expr->loc(), expr->name);
    if (!expr->member.empty()) {
      expr->callee = sema_.ctx_.make<MemberExpr>(expr->loc(), expr->callee, expr->member);
    }
  }
  if (expr->callee) {
    return sema_.classValueChecker().checkValueNew(expr, expected);
  }
  if (!expr->member.empty()) {
    Sema::ScopeEntry intl = sema_.scopes().lookup("Intl");
    if (expr->name == "Intl" && !intl.var && !intl.func && !intl.cls) {
      return expr->member == "DateTimeFormat"
                 ? sema_.dateFormatChecker().checkDateFormatNew(expr->loc(), expr->args)
                 : sema_.intlChecker().checkIntlNew(expr->loc(), expr->member, expr->args, /*withNew=*/true);
    }
    sema_.diags_.error(expr->loc(), "'" + expr->name + "." + expr->member +
                                  "' is not supported; of namespaces, only the classes of 'Intl' "
                                  "can be made with 'new'");
    checkArgsLoosely();
    return expr;
  }
  if (ClassDecl *cls = sema_.topLevelChecker().lookupValue(expr->name).cls) {
    // As node's constructors of them throw 'Illegal constructor'.
    if (cls->library && !sema_.libraryCalls().inLibrary(expr->loc()) &&
        (cls == sema_.libraryClass("CryptoKey") || cls == sema_.libraryClass("SubtleCrypto"))) {
      sema_.diags_.error(expr->loc(), cls->name == "CryptoKey"
                                    ? std::string("a CryptoKey is made by crypto.subtle: "
                                                  "importKey, generateKey, deriveKey or "
                                                  "unwrapKey")
                                    : std::string("there is one SubtleCrypto: crypto.subtle"));
      checkArgsLoosely();
      return expr;
    }
    return sema_.methodCallChecker().checkClassNew(expr, ClassValueChecker::finalClass(cls), expected);
  }
  if (const Sema::NodeName *n = sema_.builtinModuleChecker().nodeName(expr->name); n && n->module == NodeModule::Crypto) {
    return sema_.cryptoChecker().checkCryptoNew(expr, n->name);
  }
  if (const char *why = whyUnsupported(expr->name)) {
    sema_.diags_.error(expr->loc(), "'new " + expr->name + "' is not supported: " + why);
    checkArgsLoosely();
    return expr;
  }
  if (expr->name == "RegExp" && !sema_.scopes().lookup("RegExp").var && !sema_.scopes().lookup("RegExp").func) {
    if (!expr->typeArgs.empty()) {
      sema_.diags_.error(expr->loc(), "'RegExp' takes no type arguments");
    }
    return sema_.regexChecker().checkRegExpConstructor(expr->loc(), expr->args);
  }
  if (expr->name == "Date" && !sema_.scopes().lookup("Date").var && !sema_.scopes().lookup("Date").func) {
    if (!expr->typeArgs.empty()) {
      sema_.diags_.error(expr->loc(), "'Date' takes no type arguments");
    }
    return sema_.dateChecker().checkDateNew(expr->loc(), expr->args);
  }
  if ((expr->name == "Map" || expr->name == "Set" || expr->name == "WeakMap" ||
       expr->name == "WeakSet") &&
      !sema_.scopes().lookup(expr->name).var && !sema_.scopes().lookup(expr->name).func) {
    return sema_.dictNewChecker().checkDictNew(expr, expected);
  }
  if (expr->name == "Promise" && !sema_.scopes().lookup("Promise").var && !sema_.scopes().lookup("Promise").func) {
    return sema_.promiseChecker().checkPromiseNew(expr, expected);
  }
  if (expr->name == "WeakRef" && !sema_.scopes().lookup("WeakRef").var && !sema_.scopes().lookup("WeakRef").func) {
    return sema_.weakRefChecker().checkWeakRefNew(expr, expected);
  }
  if (const Type *bytes = BytesChecker::bytesTypeNamed(expr->name);
      bytes && !sema_.scopes().lookup(expr->name).var && !sema_.scopes().lookup(expr->name).func) {
    return sema_.bytesNewChecker().checkBytesNew(expr, bytes);
  }
  if (expr->name == "BigInt64Array" || expr->name == "BigUint64Array") {
    sema_.decimalChecker().reportBigInt(expr->loc(), "'" + expr->name.str() + "'");
    checkArgsLoosely();
    return expr;
  }
  if (expr->name != "Array") {
    sema_.diags_.error(expr->loc(),
                 "'new " + expr->name + "' is not supported; only 'new Array<T>(length)' is");
    checkArgsLoosely();
    return expr;
  }

  const Type *element = nullptr;
  if (expr->typeArgs.size() == 1) {
    element = sema_.typeResolver().resolveType(expr->typeArgs[0], /*allowVoid=*/false);
  } else if (expr->typeArgs.size() > 1) {
    sema_.diags_.error(expr->loc(), "'Array' takes one type argument");
    element = Type::error();
  } else if (expected && expected->nonOptional()->isArray()) {
    element = expected->nonOptional()->inner();
  } else {
    sema_.diags_.error(expr->loc(), "'new Array' needs an element type, e.g. 'new Array<int>(n)'");
    element = Type::error();
  }

  if (expr->args.size() > 1) {
    sema_.diags_.error(expr->loc(), "to create an array from values, use a literal: [a, b, c]");
    checkArgsLoosely();
    return expr;
  }
  if (expr->args.size() == 1) {
    expr->args[0] = sema_.memberChecker().checkPosition(expr->args[0]);
    if (!element->isError() && !element->hasZeroValue()) {
      sema_.diags_.error(expr->loc(), "'new Array<T>(n)' fills the array with a default value, and '" +
                                    element->name() + "' has none; start from '[]' and use push()");
    }
    if (std::optional<Linear> length = sema_.indexProofs().linear(expr->args[0]);
        length && length->term.isZero() && length->offset < 0) {
      sema_.diags_.error(expr->args[0]->loc(), "an array length cannot be negative");
    }
  }
  if (!element->isError()) {
    expr->type = Type::array(element);
  }

  return expr;
}

} // namespace tz
