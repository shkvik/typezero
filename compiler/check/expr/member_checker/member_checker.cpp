#include "compiler/check/expr/member_checker/member_checker.h"
#include "compiler/check/scopes/scopes.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/expr/call_checker/call_checker.h"
#include "compiler/check/generics/arg_list_checker/arg_list_checker.h"
#include "compiler/check/classes/class_member_checker/class_member_checker.h"
#include "compiler/check/classes/static_checker/static_checker.h"
#include "compiler/check/classes/overload_resolver/overload_resolver.h"
#include "compiler/declare/type_declarer/type_declarer.h"
#include "compiler/declare/constant_folder/constant_folder.h"
#include "compiler/check/objects/dict_checker/dict_checker.h"
#include "compiler/check/flow/index_proofs/index_proofs.h"
#include "compiler/check/generics/generic_checker/generic_checker.h"
#include "compiler/check/objects/keyed_field_checker/keyed_field_checker.h"
#include "compiler/check/objects/tuple_checker/tuple_checker.h"
#include "compiler/check/objects/object_checker/object_checker.h"
#include "compiler/check/types/utility_types/utility_types.h"
#include "compiler/check/types/union_checker/union_checker.h"
#include "compiler/check/types/unknown_checker/unknown_checker.h"
#include "compiler/library/bytes/bytes_checker/bytes_checker.h"
#include "compiler/library/crypto/crypto_checker/crypto_checker.h"
#include "compiler/library/crypto/web_crypto_checker/web_crypto_checker.h"
#include "compiler/library/date/date_checker/date_checker.h"
#include "compiler/library/tz_modules/library_calls/library_calls.h"
#include "compiler/library/intl/intl_checker/intl_checker.h"
#include "compiler/library/json/json_checker/json_checker.h"
#include "compiler/library/builtin_modules/builtin_module_checker/builtin_module_checker.h"
#include "compiler/library/builtin_modules/runtime_object_checker/runtime_object_checker.h"
#include "compiler/library/process/process_checker/process_checker.h"
#include "compiler/library/regex/regex_checker/regex_checker.h"
#include "compiler/library/regex/match_checker/match_checker.h"
#include "compiler/library/result/result_checker/result_checker.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/ErrorHandling.h"
#include <cmath>
#include <functional>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::dyn_cast_or_null;

namespace tz {

MemberChecker::MemberChecker(Sema &sema) : sema_(sema) {}

Expr *MemberChecker::checkMember(MemberExpr *member, const Type *expected) {
  member->type = Type::error();
  if (Expr *part = sema_.argListChecker().listPart(member)) {
    return part;
  }
  if (auto *name = dyn_cast<NameExpr>(member->object)) {
    Sema::ScopeEntry entry = sema_.scopes().lookup(name->name);
    if (entry.enumDecl) {
      return sema_.typeDeclarer().checkEnumMember(entry.enumDecl, member, expected);
    }
    if (entry.cls) {
      return sema_.staticChecker().checkStaticMember(member, entry.cls, expected);
    }
  }
  if (Expr *node = sema_.builtinModuleChecker().checkNodeMember(member)) {
    return node;
  }
  if (Expr *io = sema_.processChecker().checkProcessMember(member)) {
    return io;
  }
  if (sema_.callChecker().isGlobalName(member->object, "decimal")) {
    sema_.diags_.error(member->loc(), "'decimal." + member->name +
                                    "' can only be called, e.g. 'decimal.max(a, b)'; decimal "
                                    "has the functions max, min and sum");
    return member;
  }
  if (sema_.callChecker().isGlobalName(member->object, "RegExp")) {
    return sema_.regexChecker().checkRegExpStaticMember(member);
  }
  if (sema_.callChecker().isGlobalName(member->object, "crypto")) {
    return sema_.cryptoChecker().checkWebCryptoMember(member);
  }
  if (sema_.callChecker().isGlobalName(member->object, "globalThis")) {
    sema_.diags_.error(member->loc(), member->name == "crypto"
                                    ? std::string("'globalThis.crypto' is used as "
                                                  "'globalThis.crypto.subtle' and the like")
                                    : "'globalThis." + member->name +
                                          "' is not supported: tz has only 'globalThis.crypto'; "
                                          "write '" +
                                          member->name + "'");
    return member;
  }
  if (Expr *bytes = sema_.bytesChecker().checkBytesStaticMember(member)) {
    return bytes;
  }
  if (sema_.callChecker().isGlobalName(member->object, "Intl")) {
    sema_.diags_.error(member->loc(), "'Intl." + member->name +
                                    "' can only be called or made with 'new', e.g. 'new "
                                    "Intl.DateTimeFormat(\"ru\")'");
    return member;
  }
  for (llvm::StringRef global : {"console", "Math", "String", "Number"}) {
    if (sema_.callChecker().isGlobalName(member->object, global)) {
      if (Expr *constant = sema_.constantFolder().globalConstant(global, member->name, member->loc())) {
        return constant;
      }
      sema_.diags_.error(member->loc(), "'" + global + "." + member->name +
                                      "' can be used as a value only where a function type is "
                                      "expected, e.g. as a callback");
      return member;
    }
  }
  auto *object = dyn_cast<NameExpr>(member->object);
  if (object && object->name == "this") {
    sema_.thisMember_ = member->name;
  }
  if (object && object->name == "super") {
    // JavaScript reads a field through `super` from the prototype, where it
    // is not.
    sema_.diags_.error(member->loc(), "'super." + member->name + "' can only be called, 'super." +
                                    member->name +
                                    "(...)'; to read the field or the method, use "
                                    "'this." +
                                    member->name + "'");
    return member;
  }
  member->object = sema_.genericChecker().apparent(sema_.exprChecker().checkExpr(member->object));
  sema_.thisMember_ = {};
  if (member->optional) {
    member->object = sema_.exprChecker().chainLink(member->object);
  }
  const Type *type = member->object->type;
  if (type->isError()) {
    return member;
  }
  if (type->nonOptional()->isJson()) {
    return sema_.jsonChecker().checkJsonMember(member);
  }
  if (type->isUnknown() || type->isAnyObject() || type->isClassRef()) {
    return sema_.unknownChecker().checkUnknownMember(member);
  }
  // A member of every member of a union: through a type they all are, or
  // read from each (sema_union.cpp).
  if (type->isUnion()) {
    const Type *common = sema_.unionChecker().commonBaseWith(type, member->name);
    if (!common) {
      return sema_.unionChecker().checkUnionMember(member);
    }
    member->object = sema_.typeRelations().convert(member->object, common);
    type = common;
  }
  if (type->isResult()) {
    return sema_.resultChecker().checkResultMember(member);
  }
  if (type->isTuple()) {
    return sema_.tupleChecker().checkTupleMember(member);
  }
  // What a CryptoKey of Web Crypto is lies in its handle, which only tz:webcrypto reads.
  if (member->name == "handle" && sema_.webCryptoChecker().isSubtleClass(type, "CryptoKey") && !sema_.libraryCalls().inLibrary(member->loc())) {
    sema_.diags_.error(member->loc(), "property 'handle' does not exist on type 'CryptoKey'; it has "
                                "'type', 'extractable', 'algorithm' and 'usages'");
    return member;
  }
  if (type->isClass()) {
    return sema_.classMemberChecker().checkClassMember(member, expected);
  }
  if (type->isRegExp()) {
    return sema_.regexChecker().checkRegexMember(member);
  }
  if (type->isMatch()) {
    return sema_.matchChecker().checkMatchMember(member);
  }
  if (type->isDate()) {
    return sema_.dateChecker().checkDateMember(member);
  }
  if (type->isBytes()) {
    return sema_.bytesChecker().checkBytesMember(member);
  }
  if (type->isCrypto()) {
    return sema_.cryptoChecker().checkCryptoMember(member);
  }
  if (type->isIntl()) {
    return sema_.intlChecker().checkIntlMember(member, expected);
  }
  if (type->isWeakRef()) {
    sema_.diags_.error(member->loc(),
                 "property '" + member->name + "' of a WeakRef can only be " + "called: 'deref()'");
    return member;
  }
  if (type->isDict()) {
    return sema_.dictChecker().checkDictMember(member);
  }
  if (type->isRuntimeObject()) {
    return sema_.runtimeObjectChecker().checkRuntimeMember(member);
  }
  if (type->isObject() && type->overload(member->name)) {
    return sema_.overloadResolver().checkOverloadMember(member, expected);
  }
  if (type->isObject()) {
    return sema_.objectChecker().checkField(member);
  }
  // `args.length` of a list of types too, in generic code.
  if ((type->isString() || type->isArray() || type->isListParam()) && member->name == "length") {
    member->type = Type::intTy();
    return member;
  }
  if (type->isOptional()) {
    sema_.diags_.error(member->object->loc(),
                 "the value may be undefined; check it before reading '" + member->name + "'");
  } else if (type == Type::array(Type::string()) && member->name == "raw") {
    sema_.diags_.error(member->loc(), "'raw' is not supported: a tag gets the texts of its template as a "
                                "plain 'string[]', with the escapes already applied");
  } else if (type->isArray() || type->isString() || type->isIterator()) {
    sema_.diags_.error(member->loc(), "property '" + member->name + "' does not exist on type '" +
                                    type->name() + "'; methods must be called");
  } else {
    sema_.diags_.error(member->loc(),
                 "property '" + member->name + "' does not exist on type '" + type->name() + "'");
  }

  return member;
}

Expr *MemberChecker::checkPosition(Expr *expr) {
  expr = sema_.genericChecker().apparent(sema_.exprChecker().checkExpr(expr, Type::longTy()));
  const Type *type = expr->type;
  if (type->isError()) {
    return expr;
  }
  if (!type->isInteger()) {
    std::string message =
        "an index or position must be an integer, found '" + type->name().str() + "'";
    if (type->isFloat()) {
      message += "; round it with Math.floor(x) or drop the fraction with 'x | 0'";
    } else if (type->isOptional()) {
      message += "; the value may be undefined: check it first or give a default with '??'";
    }
    sema_.diags_.error(expr->loc(), message);
    return expr;
  }

  return sema_.typeRelations().convert(expr, Type::longTy());
}

Expr *MemberChecker::checkIndex(IndexExpr *index) {
  if (Expr *part = sema_.argListChecker().listPart(index)) {
    return part;
  }
  if (auto *name = dyn_cast<NameExpr>(index->object)) {
    if (EnumDecl *decl = sema_.scopes().lookup(name->name).enumDecl) {
      return sema_.typeDeclarer().checkEnumIndex(decl, index);
    }
  }
  index->object = sema_.builtinModuleChecker().processAlias(index->object); // `env[name]` of node:process
  if (Expr *variable = sema_.processChecker().checkProcessEnvIndex(index)) {
    return variable;
  }
  index->object = sema_.genericChecker().apparent(sema_.exprChecker().checkExpr(index->object));
  if (index->optional) {
    index->object = sema_.exprChecker().chainLink(index->object);
  }
  index->type = Type::error();
  if (Expr *generic = sema_.utilityTypes().checkGenericIndex(index)) {
    return generic;
  }
  if (index->object->type->isMatch()) {
    return sema_.matchChecker().checkMatchIndex(index);
  }
  if (index->object->type->nonOptional()->isJson()) {
    return sema_.jsonChecker().checkJsonIndex(index);
  }
  if (index->object->type->isBytes()) {
    return sema_.bytesChecker().checkBytesIndex(index);
  }
  if (const Type *type = index->object->type; type->isUnknown() || type->isAnyObject()) {
    index->index = sema_.exprChecker().checkExpr(index->index);
    sema_.unknownChecker().reportUnknownUse(index->object, "");
    return index;
  }
  // `o[key]` with a constant key reads the field it names.
  if (index->object->type->isObject()) {
    return sema_.keyedFieldChecker().checkKeyedField(index, /*write=*/false);
  }
  if (index->object->type->isDict()) {
    return sema_.dictChecker().checkDictIndex(index);
  }
  if (index->object->type->isOptional() && index->object->type->inner()->isObject()) {
    index->index = sema_.exprChecker().checkExpr(index->index);
    sema_.diags_.error(index->object->loc(), "the value may be undefined; check it before indexing");
    return index;
  }
  index->index = checkPosition(index->index);
  const Type *type = index->object->type;
  if (type->isError() || index->index->type->isError()) {
    return index;
  }
  // `args[i]` of a list of types, in generic code: `A[0]` for a number,
  // `A[number]` of any.
  if (type->isListParam()) {
    const Expr *at = index->index;
    while (isa<CastExpr>(at) && cast<CastExpr>(at)->isImplicit()) {
      at = cast<CastExpr>(at)->operand;
    }
    if (auto *lit = dyn_cast<IntLitExpr>(at)) {
      index->type =
          Type::indexedAccess(type, Type::numberLiteral(double(lit->value), Type::intTy()));
    } else {
      index->type = Type::optional(sema_.argListChecker().listElement(type));
    }
    return index;
  }
  const Type *element = type->isString() ? type : type->isArray() ? type->inner() : nullptr;
  if (!element) {
    if (type->isOptional() && (type->inner()->isArray() || type->inner()->isString())) {
      sema_.diags_.error(index->object->loc(), "the value may be undefined; check it before indexing");
    } else {
      sema_.diags_.error(index->loc(), "type '" + type->name() + "' cannot be indexed");
    }
    return index;
  }
  // Proved in range: a plain read. Otherwise JavaScript yields undefined.
  bool inRange = sema_.indexProofs().provesInBounds(index->object, index->index, /*allowEnd=*/false);
  index->mayBeOutOfRange = !inRange;
  index->type = inRange ? element : Type::optional(element);

  return index;
}

} // namespace tz
