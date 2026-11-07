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
#include "compiler/library/date/date_checker/date_checker.h"
#include "compiler/library/intl/intl_checker/intl_checker.h"
#include "compiler/library/json/json_checker/json_checker.h"
#include "compiler/library/process/process_checker/process_checker.h"
#include "compiler/library/regex/match_checker/match_checker.h"
#include "compiler/library/regex/regex_checker/regex_checker.h"
#include "compiler/library/result/result_checker/result_checker.h"
#include "llvm/ADT/STLExtras.h"
#include <cmath>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

MemberChecker::MemberChecker(Checker &owner)
    : checker(owner) {}

Expr *MemberChecker::checkMember(MemberExpr *member, const Type *expected) {
  member->type = Type::error();
  if (Expr *part = this->checker.argListChecker().listPart(member)) {
    return part;
  }
  if (auto *name = dyn_cast<NameExpr>(member->object)) {
    ScopeEntry entry = this->checker.scopes().lookup(name->name);
    if (entry.enumDecl()) {
      TypeDeclarer::ResultEnumMemberRead read =
          this->checker.typeDeclarer().checkEnumMember(entry.enumDecl(), member, expected);
      if (read.isOk()) {
        return read.value();
      }
      this->checker.diags_.error(member->loc(), read.error().message);
      member->type = Type::error();
      return member;
    }
    if (entry.cls()) {
      return this->checker.staticChecker().checkStaticMember(member, entry.cls(), expected);
    }
  }
  if (Expr *node = this->checker.builtinModuleChecker().checkNodeMember(member)) {
    return node;
  }
  if (Expr *io = this->checker.processChecker().checkProcessMember(member)) {
    return io;
  }
  if (this->checker.callChecker().isGlobalName(member->object, "decimal")) {
    this->checker.diags_.error(member->loc(),
                               "'decimal." + member->name +
                                   "' can only be called, e.g. 'decimal.max(a, b)'; decimal "
                                   "has the functions max, min and sum");
    return member;
  }
  if (this->checker.callChecker().isGlobalName(member->object, "RegExp")) {
    return this->checker.regexChecker().checkRegExpStaticMember(member);
  }
  if (this->checker.callChecker().isGlobalName(member->object, "crypto")) {
    return this->checker.cryptoChecker().checkWebCryptoMember(member);
  }
  if (this->checker.callChecker().isGlobalName(member->object, "globalThis")) {
    this->checker.diags_.error(member->loc(),
                               member->name == "crypto"
                                   ? std::string("'globalThis.crypto' is used as "
                                                 "'globalThis.crypto.subtle' and the like")
                                   : "'globalThis." + member->name +
                                         "' is not supported: tz has only 'globalThis.crypto'; "
                                         "write '" +
                                         member->name + "'");
    return member;
  }
  if (Expr *bytes = this->checker.bytesChecker().checkBytesStaticMember(member)) {
    return bytes;
  }
  if (this->checker.callChecker().isGlobalName(member->object, "Intl")) {
    this->checker.diags_.error(member->loc(),
                               "'Intl." + member->name +
                                   "' can only be called or made with 'new', e.g. 'new "
                                   "Intl.DateTimeFormat(\"ru\")'");
    return member;
  }
  for (llvm::StringRef global : {"console", "Math", "String", "Number"}) {
    if (this->checker.callChecker().isGlobalName(member->object, global)) {
      if (Expr *constant =
              this->checker.constantFolder().globalConstant(global, member->name, member->loc())) {
        return constant;
      }
      static const llvm::StringLiteral numberFunctions[] = {"parseInt", "parseFloat", "isInteger",
                                                            "isSafeInteger"};
      if (global == "Number" && !llvm::is_contained(numberFunctions, member->name)) {
        this->checker.diags_.error(member->loc(),
                                   "property '" + member->name + "' does not exist on 'Number'");
        return member;
      }
      this->checker.diags_.error(member->loc(),
                                 "'" + global + "." + member->name +
                                     "' can be used as a value only where a function type is "
                                     "expected, e.g. as a callback");
      return member;
    }
  }
  auto *object = dyn_cast<NameExpr>(member->object);
  if (object && object->name == "this") {
    this->checker.thisMember_ = member->name;
  }
  if (object && object->name == "super") {
    this->checker.diags_.error(member->loc(), "'super." + member->name +
                                                  "' can only be called, 'super." + member->name +
                                                  "(...)'; to read the field or the method, use "
                                                  "'this." +
                                                  member->name + "'");
    return member;
  }
  member->object = this->checker.genericChecker().apparent(
      this->checker.exprChecker().checkExpr(member->object));
  this->checker.thisMember_ = {};
  if (member->optional) {
    member->object = this->checker.exprChecker().chainLink(member->object);
  }
  const Type *type = member->object->type;
  if (type->isError()) {
    return member;
  }
  if (type->nonOptional()->isJson()) {
    return this->checker.jsonChecker().checkJsonMember(member);
  }
  if (type->isUnknown() || type->isAnyObject() || type->isClassRef()) {
    return this->checker.unknownChecker().checkUnknownMember(member);
  }
  if (type->isUnion()) {
    const Type *common = this->checker.unionChecker().commonBaseWith(type, member->name);
    if (!common) {
      return this->checker.unionChecker().checkUnionMember(member);
    }
    member->object = this->checker.typeRelations().convert(member->object, common);
    type = common;
  }
  if (type->isResult()) {
    return this->checker.resultChecker().checkResultMember(member);
  }
  if (type->isTuple()) {
    return this->checker.tupleChecker().checkTupleMember(member);
  }
  if (type->isClass()) {
    return this->checker.classMemberChecker().checkClassMember(member, expected);
  }
  if (type->isRegExp()) {
    return this->checker.regexChecker().checkRegexMember(member);
  }
  if (type->isMatch()) {
    return this->checker.matchChecker().checkMatchMember(member);
  }
  if (type->isDate()) {
    return this->checker.dateChecker().checkDateMember(member);
  }
  if (type->isBytes()) {
    return this->checker.bytesChecker().checkBytesMember(member);
  }
  if (type->isCrypto()) {
    return this->checker.cryptoChecker().checkCryptoMember(member);
  }
  if (type->isIntl()) {
    return this->checker.intlChecker().checkIntlMember(member, expected);
  }
  if (type->isWeakRef()) {
    this->checker.diags_.error(member->loc(), "property '" + member->name +
                                                  "' of a WeakRef can only be " +
                                                  "called: 'deref()'");
    return member;
  }
  if (type->isDict()) {
    return this->checker.dictChecker().checkDictMember(member);
  }
  if (type->isRuntimeObject()) {
    return this->checker.runtimeObjectChecker().checkRuntimeMember(member);
  }
  if (type->isObject() && type->overload(member->name)) {
    return this->checker.overloadResolver().checkOverloadMember(member, expected);
  }
  if (type->isObject()) {
    return this->checker.objectChecker().checkField(member);
  }
  if ((type->isString() || type->isArray() || type->isListParam()) && member->name == "length") {
    member->type = Type::intTy();
    return member;
  }
  if (type->isInteger() && type->isWrapping() && member->name == "value") {
    return this->checker.typeRelations().convert(member->object,
                                                 Type::integer(type->bitWidth(), true));
  }
  if (type->isOptional()) {
    this->checker.diags_.error(member->object->loc(),
                               "the value may be undefined; check it before reading '" +
                                   member->name + "'");
  } else if (type == Type::array(Type::string()) && member->name == "raw") {
    this->checker.diags_.error(member->loc(),
                               "'raw' is not supported: a tag gets the texts of its template as a "
                               "plain 'string[]', with the escapes already applied");
  } else if (type->isArray() || type->isString() || type->isIterator()) {
    this->checker.diags_.error(member->loc(), "property '" + member->name +
                                                  "' does not exist on type '" + type->name() +
                                                  "'; methods must be called");
  } else {
    this->checker.diags_.error(member->loc(), "property '" + member->name +
                                                  "' does not exist on type '" + type->name() +
                                                  "'");
  }

  return member;
}

Expr *MemberChecker::checkPosition(Expr *expr) {
  expr = this->checker.genericChecker().apparent(
      this->checker.exprChecker().checkExpr(expr, Type::intTy()));
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
    this->checker.diags_.error(expr->loc(), message);
    return expr;
  }

  return this->checker.typeRelations().convert(expr, Type::intTy());
}

Expr *MemberChecker::checkIndex(IndexExpr *index) {
  if (Expr *part = this->checker.argListChecker().listPart(index)) {
    return part;
  }
  if (auto *name = dyn_cast<NameExpr>(index->object)) {
    if (EnumDecl *decl = this->checker.scopes().lookup(name->name).enumDecl()) {
      TypeDeclarer::ResultEnumIndexValue read =
          this->checker.typeDeclarer().checkEnumIndex(decl, index);
      if (read.isOk()) {
        return read.value();
      }
      llvm::SMLoc at;
      switch (read.error().code) {
        case TypeDeclarerCheckEnumIndexErrorCode::ALREADY_REPORTED:
          return index;
        case TypeDeclarerCheckEnumIndexErrorCode::STRING_ENUM:
          at = index->loc();
          break;
        case TypeDeclarerCheckEnumIndexErrorCode::NOT_AN_INTEGER:
          at = index->index->loc();
          break;
      }
      this->checker.diags_.error(at, read.error().message);
      return index;
    }
  }
  index->object = this->checker.builtinModuleChecker().processAlias(index->object);
  if (Expr *variable = this->checker.processChecker().checkProcessEnvIndex(index)) {
    return variable;
  }
  index->object =
      this->checker.genericChecker().apparent(this->checker.exprChecker().checkExpr(index->object));
  if (index->optional) {
    index->object = this->checker.exprChecker().chainLink(index->object);
  }
  index->type = Type::error();
  if (Expr *generic = this->checker.utilityTypes().checkGenericIndex(index)) {
    return generic;
  }
  if (index->object->type->isMatch()) {
    return this->checker.matchChecker().checkMatchIndex(index);
  }
  if (index->object->type->nonOptional()->isJson()) {
    return this->checker.jsonChecker().checkJsonIndex(index);
  }
  if (index->object->type->isBytes()) {
    return this->checker.bytesChecker().checkBytesIndex(index);
  }
  if (const Type *type = index->object->type; type->isUnknown() || type->isAnyObject()) {
    index->index = this->checker.exprChecker().checkExpr(index->index);
    this->checker.unknownChecker().reportUnknownUse(index->object, "");
    return index;
  }
  if (index->object->type->isObject()) {
    return this->checker.keyedFieldChecker().checkKeyedField(index, false);
  }
  if (index->object->type->isDict()) {
    return this->checker.dictChecker().checkDictIndex(index);
  }
  if (index->object->type->isOptional() && index->object->type->inner()->isObject()) {
    index->index = this->checker.exprChecker().checkExpr(index->index);
    this->checker.diags_.error(index->object->loc(),
                               "the value may be undefined; check it before indexing");
    return index;
  }
  index->index = this->checkPosition(index->index);
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
      index->type = Type::optional(this->checker.argListChecker().listElement(type));
    }
    return index;
  }
  const Type *element = type->isString() ? type : type->isArray() ? type->inner() : nullptr;
  if (!element) {
    if (type->isOptional() && (type->inner()->isArray() || type->inner()->isString())) {
      this->checker.diags_.error(index->object->loc(),
                                 "the value may be undefined; check it before indexing");
    } else {
      this->checker.diags_.error(index->loc(), "type '" + type->name() + "' cannot be indexed");
    }
    return index;
  }
  bool inRange = this->checker.indexProofs().provesInBounds(index->object, index->index, false);
  if (ElementIndex *access = index->form.as<ElementIndex>()) {
    access->mayBeOutOfRange = !inRange;
  }
  index->type = inRange ? element : Type::optional(element);

  return index;
}

}
