#include "compiler/check/expr/new_checker/new_checker.h"
#include "compiler/check/async/promise_checker/promise_checker.h"
#include "compiler/check/classes/class_value_checker/class_value_checker.h"
#include "compiler/check/classes/method_call_checker/method_call_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/expr/member_checker/member_checker.h"
#include "compiler/check/flow/index_proofs/index_proofs.h"
#include "compiler/check/objects/dict_new_checker/dict_new_checker.h"
#include "compiler/check/scopes/scopes.h"
#include "compiler/check/top_level_checker/top_level_checker.h"
#include "compiler/check/types/type_resolver/type_resolver.h"
#include "compiler/library/builtin_modules/builtin_module_checker/builtin_module_checker.h"
#include "compiler/library/bytes/bytes_checker/bytes_checker.h"
#include "compiler/library/bytes/bytes_new_checker/bytes_new_checker.h"
#include "compiler/library/crypto/crypto_checker/crypto_checker.h"
#include "compiler/library/date/date_checker/date_checker.h"
#include "compiler/library/date/date_format_checker/date_format_checker.h"
#include "compiler/library/decimal/decimal_checker/decimal_checker.h"
#include "compiler/library/intl/intl_checker/intl_checker.h"
#include "compiler/library/regex/regex_checker/regex_checker.h"
#include "compiler/library/tz_modules/http_checker/http_checker.h"
#include "compiler/library/tz_modules/library_calls/library_calls.h"
#include "compiler/library/tz_modules/tz_modules_checker/tz_modules_checker.h"
#include "compiler/library/tz_modules/url_checker/url_checker.h"
#include "compiler/library/weakref/weak_ref_checker/weak_ref_checker.h"
#include "llvm/ADT/STLExtras.h"
#include <cstring>

using llvm::dyn_cast;

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

NewChecker::NewChecker(Checker &owner)
    : checker(owner) {}

Expr *NewChecker::checkNew(NewExpr *expr, const Type *expected) {
  expr->type = Type::error();
  auto checkArgsLoosely = [&] {
    for (Expr *&arg : expr->args) {
      arg = this->checker.exprChecker().checkExpr(arg);
    }
  };
  if (!expr->callee && !expr->name.empty() &&
      this->checker.topLevelChecker().lookupValue(expr->name).var()) {
    expr->callee = this->checker.ctx_.make<NameExpr>(expr->loc(), expr->name);
    if (!expr->member.empty()) {
      expr->callee = this->checker.ctx_.make<MemberExpr>(expr->loc(), expr->callee, expr->member);
    }
  }
  if (expr->callee) {
    return this->checker.classValueChecker().checkValueNew(expr, expected);
  }
  if (!expr->member.empty()) {
    ScopeEntry intl = this->checker.scopes().lookup("Intl");
    if (expr->name == "Intl" && !intl.var() && !intl.func() && !intl.cls() &&
        expr->member == "Locale") {
      return this->checker.intlChecker().checkLocaleNew(expr, expected);
    }
    if (expr->name == "Intl" && !intl.var() && !intl.func() && !intl.cls()) {
      return expr->member == "DateTimeFormat"
                 ? this->checker.dateFormatChecker().checkDateFormatNew(expr->loc(), expr->args)
                 : this->checker.intlChecker().checkIntlNew(expr->loc(), expr->member, expr->args,
                                                            true);
    }
    this->checker.diags_.error(expr->loc(),
                               "'" + expr->name + "." + expr->member +
                                   "' is not supported; of namespaces, only the classes of 'Intl' "
                                   "can be made with 'new'");
    checkArgsLoosely();
    return expr;
  }
  if (ClassDecl *cls = this->checker.topLevelChecker().lookupValue(expr->name).cls()) {
    if (cls->library() && !this->checker.libraryCalls().inLibrary(expr->loc()) &&
        (cls == this->checker.libraryClass("CryptoKey") ||
         cls == this->checker.libraryClass("SubtleCrypto"))) {
      this->checker.diags_.error(expr->loc(),
                                 cls->name == "CryptoKey"
                                     ? std::string("a CryptoKey is made by crypto.subtle: "
                                                   "importKey, generateKey, deriveKey or "
                                                   "unwrapKey")
                                     : std::string("there is one SubtleCrypto: crypto.subtle"));
      checkArgsLoosely();
      return expr;
    }
    Expr *checked = this->checker.methodCallChecker().checkClassNew(
        expr, ClassValueChecker::finalClass(cls), expected);
    if (auto *made = dyn_cast<NewExpr>(checked); made && cls->library() && cls->name == "URL") {
      this->checker.urlChecker().checkUrlNew(made);
    }
    if (auto *made = dyn_cast<NewExpr>(checked); made && cls->library()) {
      if (cls->name == "Server" || cls->name == "Socket") {
        this->checker.tzModulesChecker().noteServer(made);
      }
      if (cls->name == "Response") {
        this->checker.httpChecker().checkResponseNew(made);
      } else if (cls->name == "Request") {
        this->checker.httpChecker().checkRequestNew(made);
      } else if (cls->name == "Headers") {
        this->checker.httpChecker().checkHeadersNew(made);
      }
    }
    return checked;
  }
  if (const Checker::NodeName *n = this->checker.builtinModuleChecker().nodeName(expr->name);
      n && n->module == NodeModule::Crypto) {
    return this->checker.cryptoChecker().checkCryptoNew(expr, n->name);
  }
  if (const char *why = whyUnsupported(expr->name)) {
    this->checker.diags_.error(expr->loc(), "'new " + expr->name + "' is not supported: " + why);
    checkArgsLoosely();
    return expr;
  }
  if (expr->name == "RegExp" && !this->checker.scopes().lookup("RegExp").var() &&
      !this->checker.scopes().lookup("RegExp").func()) {
    if (!expr->typeArgs.empty()) {
      this->checker.diags_.error(expr->loc(), "'RegExp' takes no type arguments");
    }
    return this->checker.regexChecker().checkRegExpConstructor(expr->loc(), expr->args);
  }
  if (expr->name == "Date" && !this->checker.scopes().lookup("Date").var() &&
      !this->checker.scopes().lookup("Date").func()) {
    if (!expr->typeArgs.empty()) {
      this->checker.diags_.error(expr->loc(), "'Date' takes no type arguments");
    }
    return this->checker.dateChecker().checkDateNew(expr->loc(), expr->args);
  }
  if ((expr->name == "Map" || expr->name == "Set" || expr->name == "WeakMap" ||
       expr->name == "WeakSet") &&
      !this->checker.scopes().lookup(expr->name).var() &&
      !this->checker.scopes().lookup(expr->name).func()) {
    return this->checker.dictNewChecker().checkDictNew(expr, expected);
  }
  if (expr->name == "Promise" && !this->checker.scopes().lookup("Promise").var() &&
      !this->checker.scopes().lookup("Promise").func()) {
    return this->checker.promiseChecker().checkPromiseNew(expr, expected);
  }
  if (expr->name == "WeakRef" && !this->checker.scopes().lookup("WeakRef").var() &&
      !this->checker.scopes().lookup("WeakRef").func()) {
    return this->checker.weakRefChecker().checkWeakRefNew(expr, expected);
  }
  if (const Type *bytes = BytesChecker::bytesTypeNamed(expr->name);
      bytes && !this->checker.scopes().lookup(expr->name).var() &&
      !this->checker.scopes().lookup(expr->name).func()) {
    return this->checker.bytesNewChecker().checkBytesNew(expr, bytes);
  }
  if (expr->name == "BigInt64Array" || expr->name == "BigUint64Array") {
    this->checker.decimalChecker().reportBigInt(expr->loc(), "'" + expr->name.str() + "'");
    checkArgsLoosely();
    return expr;
  }
  if (expr->name != "Array") {
    this->checker.diags_.error(expr->loc(), "cannot find class '" + expr->name + "'");
    checkArgsLoosely();
    return expr;
  }

  const Type *element = nullptr;
  if (expr->typeArgs.size() == 1) {
    element = this->checker.typeResolver().resolveType(expr->typeArgs[0], false);
  } else if (expr->typeArgs.size() > 1) {
    this->checker.diags_.error(expr->loc(), "'Array' takes one type argument");
    element = Type::error();
  } else if (expected && expected->nonOptional()->isArray()) {
    element = expected->nonOptional()->inner();
  } else {
    this->checker.diags_.error(expr->loc(),
                               "'new Array' needs an element type, e.g. 'new Array<int>(n)'");
    element = Type::error();
  }

  if (expr->args.size() > 1) {
    this->checker.diags_.error(expr->loc(),
                               "to create an array from values, use a literal: [a, b, c]");
    checkArgsLoosely();
    return expr;
  }
  if (expr->args.size() == 1) {
    expr->args[0] = this->checker.memberChecker().checkPosition(expr->args[0]);
    if (!element->isError() && !element->hasZeroValue()) {
      this->checker.diags_.error(
          expr->loc(), "'new Array<T>(n)' fills the array with a default value, and '" +
                           element->name() + "' has none; start from '[]' and use push()");
    }
    this->checker.indexProofs().requireBetween(expr->args[0], 0, std::nullopt, "an array length");
  }
  if (!element->isError()) {
    expr->type = Type::array(element);
  }

  return expr;
}

}
