#include "compiler/check/expr/member_checker/member_checker.h"
#include "compiler/check/classes/class_member_checker/class_member_checker.h"
#include "compiler/check/classes/overload_resolver/overload_resolver.h"
#include "compiler/check/classes/static_checker/static_checker.h"
#include "compiler/check/expr/call_checker/call_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/flow/index_proofs/index_proofs.h"
#include "compiler/check/generics/arg_list_checker/arg_list_checker.h"
#include "compiler/check/generics/generic_checker/generic_checker.h"
#include "compiler/check/objects/dict_checker/dict_checker.h"
#include "compiler/check/objects/keyed_field_checker/keyed_field_checker.h"
#include "compiler/check/objects/object_checker/object_checker.h"
#include "compiler/check/objects/tuple_checker/tuple_checker.h"
#include "compiler/check/scopes/scopes.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/check/types/union_checker/union_checker.h"
#include "compiler/check/types/unknown_checker/unknown_checker.h"
#include "compiler/check/types/utility_types/utility_types.h"
#include "compiler/declare/constant_folder/constant_folder.h"
#include "compiler/declare/type_declarer/type_declarer.h"
#include "compiler/library/builtin_modules/builtin_module_checker/builtin_module_checker.h"
#include "compiler/library/builtin_modules/runtime_object_checker/runtime_object_checker.h"
#include "compiler/library/bytes/bytes_checker/bytes_checker.h"
#include "compiler/library/crypto/crypto_checker/crypto_checker.h"
#include "compiler/library/crypto/web_crypto_checker/web_crypto_checker.h"
#include "compiler/library/date/date_checker/date_checker.h"
#include "compiler/library/intl/intl_checker/intl_checker.h"
#include "compiler/library/json/json_checker/json_checker.h"
#include "compiler/library/process/process_checker/process_checker.h"
#include "compiler/library/regex/match_checker/match_checker.h"
#include "compiler/library/regex/regex_checker/regex_checker.h"
#include "compiler/library/result/result_checker/result_checker.h"
#include "compiler/library/tz_modules/library_calls/library_calls.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/ErrorHandling.h"
#include <cmath>
#include <functional>

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

MemberChecker::MemberChecker(Checker &checker) : checker_(checker) {}

Expr *MemberChecker::checkMember(MemberExpr *member, const Type *expected) {
  member->type = Type::error();
  if (Expr *part = checker_.argListChecker().listPart(member)) {
    return part;
  }
  if (auto *name = dyn_cast<NameExpr>(member->object)) {
    Checker::ScopeEntry entry = checker_.scopes().lookup(name->name);
    if (entry.enumDecl) {
      return checker_.typeDeclarer().checkEnumMember(entry.enumDecl, member, expected);
    }
    if (entry.cls) {
      return checker_.staticChecker().checkStaticMember(member, entry.cls, expected);
    }
  }
  if (Expr *node = checker_.builtinModuleChecker().checkNodeMember(member)) {
    return node;
  }
  if (Expr *io = checker_.processChecker().checkProcessMember(member)) {
    return io;
  }
  if (checker_.callChecker().isGlobalName(member->object, "decimal")) {
    checker_.diags_.error(member->loc(),
                          "'decimal." + member->name +
                              "' can only be called, e.g. 'decimal.max(a, b)'; decimal "
                              "has the functions max, min and sum");
    return member;
  }
  if (checker_.callChecker().isGlobalName(member->object, "RegExp")) {
    return checker_.regexChecker().checkRegExpStaticMember(member);
  }
  if (checker_.callChecker().isGlobalName(member->object, "crypto")) {
    return checker_.cryptoChecker().checkWebCryptoMember(member);
  }
  if (checker_.callChecker().isGlobalName(member->object, "globalThis")) {
    checker_.diags_.error(member->loc(),
                          member->name == "crypto"
                              ? std::string("'globalThis.crypto' is used as "
                                            "'globalThis.crypto.subtle' and the like")
                              : "'globalThis." + member->name +
                                    "' is not supported: tz has only 'globalThis.crypto'; "
                                    "write '" +
                                    member->name + "'");
    return member;
  }
  if (Expr *bytes = checker_.bytesChecker().checkBytesStaticMember(member)) {
    return bytes;
  }
  if (checker_.callChecker().isGlobalName(member->object, "Intl")) {
    checker_.diags_.error(member->loc(), "'Intl." + member->name +
                                             "' can only be called or made with 'new', e.g. 'new "
                                             "Intl.DateTimeFormat(\"ru\")'");
    return member;
  }
  for (llvm::StringRef global : {"console", "Math", "String", "Number"}) {
    if (checker_.callChecker().isGlobalName(member->object, global)) {
      if (Expr *constant =
              checker_.constantFolder().globalConstant(global, member->name, member->loc())) {
        return constant;
      }
      checker_.diags_.error(member->loc(),
                            "'" + global + "." + member->name +
                                "' can be used as a value only where a function type is "
                                "expected, e.g. as a callback");
      return member;
    }
  }
  auto *object = dyn_cast<NameExpr>(member->object);
  if (object && object->name == "this") {
    checker_.thisMember_ = member->name;
  }
  if (object && object->name == "super") {
    checker_.diags_.error(member->loc(), "'super." + member->name +
                                             "' can only be called, 'super." + member->name +
                                             "(...)'; to read the field or the method, use "
                                             "'this." +
                                             member->name + "'");
    return member;
  }
  member->object =
      checker_.genericChecker().apparent(checker_.exprChecker().checkExpr(member->object));
  checker_.thisMember_ = {};
  if (member->optional) {
    member->object = checker_.exprChecker().chainLink(member->object);
  }
  const Type *type = member->object->type;
  if (type->isError()) {
    return member;
  }
  if (type->nonOptional()->isJson()) {
    return checker_.jsonChecker().checkJsonMember(member);
  }
  if (type->isUnknown() || type->isAnyObject() || type->isClassRef()) {
    return checker_.unknownChecker().checkUnknownMember(member);
  }
  if (type->isUnion()) {
    const Type *common = checker_.unionChecker().commonBaseWith(type, member->name);
    if (!common) {
      return checker_.unionChecker().checkUnionMember(member);
    }
    member->object = checker_.typeRelations().convert(member->object, common);
    type = common;
  }
  if (type->isResult()) {
    return checker_.resultChecker().checkResultMember(member);
  }
  if (type->isTuple()) {
    return checker_.tupleChecker().checkTupleMember(member);
  }
  if (member->name == "handle" && checker_.webCryptoChecker().isSubtleClass(type, "CryptoKey") &&
      !checker_.libraryCalls().inLibrary(member->loc())) {
    checker_.diags_.error(member->loc(),
                          "property 'handle' does not exist on type 'CryptoKey'; it has "
                          "'type', 'extractable', 'algorithm' and 'usages'");
    return member;
  }
  if (type->isClass()) {
    return checker_.classMemberChecker().checkClassMember(member, expected);
  }
  if (type->isRegExp()) {
    return checker_.regexChecker().checkRegexMember(member);
  }
  if (type->isMatch()) {
    return checker_.matchChecker().checkMatchMember(member);
  }
  if (type->isDate()) {
    return checker_.dateChecker().checkDateMember(member);
  }
  if (type->isBytes()) {
    return checker_.bytesChecker().checkBytesMember(member);
  }
  if (type->isCrypto()) {
    return checker_.cryptoChecker().checkCryptoMember(member);
  }
  if (type->isIntl()) {
    return checker_.intlChecker().checkIntlMember(member, expected);
  }
  if (type->isWeakRef()) {
    checker_.diags_.error(member->loc(), "property '" + member->name +
                                             "' of a WeakRef can only be " + "called: 'deref()'");
    return member;
  }
  if (type->isDict()) {
    return checker_.dictChecker().checkDictMember(member);
  }
  if (type->isRuntimeObject()) {
    return checker_.runtimeObjectChecker().checkRuntimeMember(member);
  }
  if (type->isObject() && type->overload(member->name)) {
    return checker_.overloadResolver().checkOverloadMember(member, expected);
  }
  if (type->isObject()) {
    return checker_.objectChecker().checkField(member);
  }
  if ((type->isString() || type->isArray() || type->isListParam()) && member->name == "length") {
    member->type = Type::intTy();
    return member;
  }
  if (type->isOptional()) {
    checker_.diags_.error(member->object->loc(),
                          "the value may be undefined; check it before reading '" + member->name +
                              "'");
  } else if (type == Type::array(Type::string()) && member->name == "raw") {
    checker_.diags_.error(member->loc(),
                          "'raw' is not supported: a tag gets the texts of its template as a "
                          "plain 'string[]', with the escapes already applied");
  } else if (type->isArray() || type->isString() || type->isIterator()) {
    checker_.diags_.error(member->loc(), "property '" + member->name +
                                             "' does not exist on type '" + type->name() +
                                             "'; methods must be called");
  } else {
    checker_.diags_.error(member->loc(), "property '" + member->name +
                                             "' does not exist on type '" + type->name() + "'");
  }

  return member;
}

Expr *MemberChecker::checkPosition(Expr *expr) {
  expr = checker_.genericChecker().apparent(checker_.exprChecker().checkExpr(expr, Type::intTy()));
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
    checker_.diags_.error(expr->loc(), message);
    return expr;
  }

  return checker_.typeRelations().convert(expr, Type::intTy());
}

Expr *MemberChecker::checkIndex(IndexExpr *index) {
  if (Expr *part = checker_.argListChecker().listPart(index)) {
    return part;
  }
  if (auto *name = dyn_cast<NameExpr>(index->object)) {
    if (EnumDecl *decl = checker_.scopes().lookup(name->name).enumDecl) {
      return checker_.typeDeclarer().checkEnumIndex(decl, index);
    }
  }
  index->object = checker_.builtinModuleChecker().processAlias(index->object);
  if (Expr *variable = checker_.processChecker().checkProcessEnvIndex(index)) {
    return variable;
  }
  index->object =
      checker_.genericChecker().apparent(checker_.exprChecker().checkExpr(index->object));
  if (index->optional) {
    index->object = checker_.exprChecker().chainLink(index->object);
  }
  index->type = Type::error();
  if (Expr *generic = checker_.utilityTypes().checkGenericIndex(index)) {
    return generic;
  }
  if (index->object->type->isMatch()) {
    return checker_.matchChecker().checkMatchIndex(index);
  }
  if (index->object->type->nonOptional()->isJson()) {
    return checker_.jsonChecker().checkJsonIndex(index);
  }
  if (index->object->type->isBytes()) {
    return checker_.bytesChecker().checkBytesIndex(index);
  }
  if (const Type *type = index->object->type; type->isUnknown() || type->isAnyObject()) {
    index->index = checker_.exprChecker().checkExpr(index->index);
    checker_.unknownChecker().reportUnknownUse(index->object, "");
    return index;
  }
  if (index->object->type->isObject()) {
    return checker_.keyedFieldChecker().checkKeyedField(index, false);
  }
  if (index->object->type->isDict()) {
    return checker_.dictChecker().checkDictIndex(index);
  }
  if (index->object->type->isOptional() && index->object->type->inner()->isObject()) {
    index->index = checker_.exprChecker().checkExpr(index->index);
    checker_.diags_.error(index->object->loc(),
                          "the value may be undefined; check it before indexing");
    return index;
  }
  index->index = checkPosition(index->index);
  const Type *type = index->object->type;
  if (type->isError() || index->index->type->isError()) {
    return index;
  }
  if (type->isListParam()) {
    const Expr *at = index->index;
    while (isa<CastExpr>(at) && cast<CastExpr>(at)->isImplicit()) {
      at = cast<CastExpr>(at)->operand;
    }
    if (auto *lit = dyn_cast<IntLitExpr>(at)) {
      index->type =
          Type::indexedAccess(type, Type::numberLiteral(double(lit->value), Type::intTy()));
    } else {
      index->type = Type::optional(checker_.argListChecker().listElement(type));
    }
    return index;
  }
  const Type *element = type->isString() ? type : type->isArray() ? type->inner() : nullptr;
  if (!element) {
    if (type->isOptional() && (type->inner()->isArray() || type->inner()->isString())) {
      checker_.diags_.error(index->object->loc(),
                            "the value may be undefined; check it before indexing");
    } else {
      checker_.diags_.error(index->loc(), "type '" + type->name() + "' cannot be indexed");
    }
    return index;
  }
  bool inRange = checker_.indexProofs().provesInBounds(index->object, index->index, false);
  index->mayBeOutOfRange = !inRange;
  index->type = inRange ? element : Type::optional(element);

  return index;
}

}
