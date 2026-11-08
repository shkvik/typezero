#include "compiler/semantic/bodies/expr/member_checker/member_checker.h"
#include "compiler/semantic/bodies/classes/class_member_checker/class_member_checker.h"
#include "compiler/semantic/bodies/classes/overload_resolver/overload_resolver.h"
#include "compiler/semantic/bodies/classes/static_checker/static_checker.h"
#include "compiler/semantic/bodies/expr/call_checker/call_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/bodies/objects/dict_checker/dict_checker.h"
#include "compiler/semantic/bodies/objects/keyed_field_checker/keyed_field_checker.h"
#include "compiler/semantic/bodies/objects/object_checker/object_checker.h"
#include "compiler/semantic/bodies/objects/tuple_checker/tuple_checker.h"
#include "compiler/semantic/declare/constant_folder/constant_folder.h"
#include "compiler/semantic/declare/type_declarer/type_declarer.h"
#include "compiler/semantic/flow/index_proofs/index_proofs.h"
#include "compiler/semantic/generics/arg_list_checker/arg_list_checker.h"
#include "compiler/semantic/generics/generic_checker/generic_checker.h"
#include "compiler/semantic/library/builtin_modules/builtin_module_checker/builtin_module_checker.h"
#include "compiler/semantic/library/builtin_modules/runtime_object_checker/runtime_object_checker.h"
#include "compiler/semantic/library/bytes/bytes_checker/bytes_checker.h"
#include "compiler/semantic/library/crypto/crypto_checker/crypto_checker.h"
#include "compiler/semantic/library/date/date_checker/date_checker.h"
#include "compiler/semantic/library/intl/intl_checker/intl_checker.h"
#include "compiler/semantic/library/json/json_checker/json_checker.h"
#include "compiler/semantic/library/process/process_checker/process_checker.h"
#include "compiler/semantic/library/regex/match_checker/match_checker.h"
#include "compiler/semantic/library/regex/regex_checker/regex_checker.h"
#include "compiler/semantic/library/result/result_checker/result_checker.h"
#include "compiler/semantic/names/scopes/scopes.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "compiler/semantic/types/union_checker/union_checker.h"
#include "compiler/semantic/types/unknown_checker/unknown_checker.h"
#include "compiler/semantic/types/utility_types/utility_types.h"
#include "llvm/ADT/STLExtras.h"
#include <cmath>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

MemberChecker::MemberChecker(SemanticState &owner)
    : state(owner) {}

MemberChecker::ResultMemberReadable MemberChecker::checkMember(MemberExpr *member,
                                                               const Type *expected) {
  member->type = Type::error();
  if (Expr *part = ArgListChecker(this->state).listPart(member)) {
    return ok(part);
  }
  if (auto *name = dyn_cast<NameExpr>(member->object)) {
    ScopeEntry entry = Scopes(this->state).lookup(name->name);
    if (entry.enumDecl()) {
      TypeDeclarer::ResultEnumMemberRead read =
          TypeDeclarer(this->state).checkEnumMember(entry.enumDecl(), member, expected);
      if (read.isOk()) {
        return ok(read.value());
      }
      member->type = Type::error();
      return err(MemberCheckerCheckMemberError{
          .code = MemberCheckerCheckMemberErrorCode::ENUM_MEMBER,
          .message = read.error().message,
      });
    }
    if (entry.cls()) {
      Expr *read = StaticChecker(this->state).checkStaticMember(member, entry.cls(), expected);

      return ok(read);
    }
  }
  if (Expr *node = BuiltinModuleChecker(this->state).checkNodeMember(member)) {
    return ok(node);
  }
  if (Expr *io = ProcessChecker(this->state).checkProcessMember(member)) {
    return ok(io);
  }
  if (CallChecker(this->state).isGlobalName(member->object, "decimal")) {
    std::string message = ErrorTextMemberRead::decimal(member->name);

    return err(MemberCheckerCheckMemberError{
        .code = MemberCheckerCheckMemberErrorCode::DECIMAL_FUNCTION,
        .message = message,
    });
  }
  if (CallChecker(this->state).isGlobalName(member->object, "RegExp")) {
    Expr *regex = RegexChecker(this->state).checkRegExpStaticMember(member);

    return ok(regex);
  }
  if (CallChecker(this->state).isGlobalName(member->object, "crypto")) {
    Expr *crypto = CryptoChecker(this->state).checkWebCryptoMember(member);

    return ok(crypto);
  }
  if (CallChecker(this->state).isGlobalName(member->object, "globalThis")) {
    std::string message = member->name == "crypto" ? ErrorTextMemberRead::crypto()
                                                   : ErrorTextMemberRead::globalThis(member->name);

    return err(MemberCheckerCheckMemberError{
        .code = MemberCheckerCheckMemberErrorCode::GLOBAL_THIS,
        .message = message,
    });
  }
  if (Expr *bytes = BytesChecker(this->state).checkBytesStaticMember(member)) {
    return ok(bytes);
  }
  if (CallChecker(this->state).isGlobalName(member->object, "Intl")) {
    std::string message = ErrorTextMemberRead::intl(member->name);

    return err(MemberCheckerCheckMemberError{
        .code = MemberCheckerCheckMemberErrorCode::INTL_VALUE,
        .message = message,
    });
  }
  for (llvm::StringRef global : {"console", "Math", "String", "Number"}) {
    if (CallChecker(this->state).isGlobalName(member->object, global)) {
      if (Expr *constant =
              ConstantFolder(this->state).globalConstant(global, member->name, member->loc())) {
        return ok(constant);
      }
      static const llvm::StringLiteral numberFunctions[] = {"parseInt", "parseFloat", "isInteger",
                                                            "isSafeInteger"};
      if (global == "Number" && !llvm::is_contained(numberFunctions, member->name)) {
        std::string message = ErrorTextMemberRead::numberProp(member->name);

        return err(MemberCheckerCheckMemberError{
            .code = MemberCheckerCheckMemberErrorCode::NUMBER_PROPERTY,
            .message = message,
        });
      }
      std::string message = ErrorTextMemberRead::asValue(global, member->name);

      return err(MemberCheckerCheckMemberError{
          .code = MemberCheckerCheckMemberErrorCode::GLOBAL_FUNCTION,
          .message = message,
      });
    }
  }
  auto *object = dyn_cast<NameExpr>(member->object);
  if (object && object->name == "this") {
    this->state.getClassContext().rememberThisMember(member->name);
  }
  if (object && object->name == "super") {
    std::string message = ErrorTextMemberRead::superField(member->name);

    return err(MemberCheckerCheckMemberError{
        .code = MemberCheckerCheckMemberErrorCode::SUPER_MEMBER,
        .message = message,
    });
  }
  member->object =
      GenericChecker(this->state).apparent(ExprChecker(this->state).checkExpr(member->object));
  this->state.getClassContext().forgetThisMember();
  if (member->optional) {
    member->object = ExprChecker(this->state).chainLink(member->object);
  }
  const Type *type = member->object->type;
  if (type->isError()) {
    return ok(member);
  }
  if (type->nonOptional()->isJson()) {
    Expr *json = JsonChecker(this->state).checkJsonMember(member);

    return ok(json);
  }
  if (type->isUnknown() || type->isAnyObject() || type->isClassRef()) {
    UnknownChecker::ResultMemberChecked checked =
        UnknownChecker(this->state).checkUnknownMember(member);
    if (checked.isOk()) {
      return ok(checked.value());
    }
    bool narrow = checked.error().code == UnknownCheckerCheckUnknownMemberErrorCode::NOT_NARROWED;

    return err(MemberCheckerCheckMemberError{
        .code = narrow ? MemberCheckerCheckMemberErrorCode::UNKNOWN_NOT_NARROWED
                       : MemberCheckerCheckMemberErrorCode::UNKNOWN_MEMBER,
        .message = checked.error().message,
    });
  }
  if (type->isUnion()) {
    const Type *common = UnionChecker(this->state).commonBaseWith(type, member->name);
    if (!common) {
      UnionChecker::ResultUnionMemberTyped fieldRead =
          UnionChecker(this->state).checkUnionMember(member);
      if (fieldRead.isOk()) {
        return ok(fieldRead.value());
      }
      member->type = Type::error();
      return err(MemberCheckerCheckMemberError{
          .code = MemberCheckerCheckMemberErrorCode::UNION_MEMBER,
          .message = fieldRead.error().message,
      });
    }
    member->object = TypeRelations(this->state).convert(member->object, common);
    type = common;
  }
  if (type->isResult()) {
    Expr *result = ResultChecker(this->state).checkResultMember(member);

    return ok(result);
  }
  if (type->isTuple()) {
    TupleChecker::ResultTupleMember tupleMember =
        TupleChecker(this->state).checkTupleMember(member);
    if (tupleMember.isErr()) {
      return err(MemberCheckerCheckMemberError{
          .code = MemberCheckerCheckMemberErrorCode::TUPLE_MEMBER,
          .message = tupleMember.error().message,
      });
    }
    return ok(tupleMember.value());
  }
  if (type->isClass()) {
    Expr *field = ClassMemberChecker(this->state).checkClassMember(member, expected);

    return ok(field);
  }
  if (type->isRegExp()) {
    Expr *regex = RegexChecker(this->state).checkRegexMember(member);

    return ok(regex);
  }
  if (type->isMatch()) {
    Expr *match = MatchChecker(this->state).checkMatchMember(member);

    return ok(match);
  }
  if (type->isDate()) {
    Expr *date = DateChecker(this->state).checkDateMember(member);

    return ok(date);
  }
  if (type->isBytes()) {
    Expr *bytes = BytesChecker(this->state).checkBytesMember(member);

    return ok(bytes);
  }
  if (type->isCrypto()) {
    Expr *crypto = CryptoChecker(this->state).checkCryptoMember(member);

    return ok(crypto);
  }
  if (type->isIntl()) {
    Expr *intl = IntlChecker(this->state).checkIntlMember(member, expected);

    return ok(intl);
  }
  if (type->isWeakRef()) {
    std::string message = ErrorTextMemberRead::weakRef(member->name);

    return err(MemberCheckerCheckMemberError{
        .code = MemberCheckerCheckMemberErrorCode::WEAK_REF_MEMBER,
        .message = message,
    });
  }
  if (type->isDict()) {
    DictChecker::ResultDictMemberAccess dict = DictChecker(this->state).checkDictMember(member);
    if (dict.isErr()) {
      return err(MemberCheckerCheckMemberError{
          .code = MemberCheckerCheckMemberErrorCode::DICT_MEMBER,
          .message = dict.error().message,
      });
    }
    return ok(dict.value());
  }
  if (type->isRuntimeObject()) {
    Expr *runtime = RuntimeObjectChecker(this->state).checkRuntimeMember(member);

    return ok(runtime);
  }
  if (type->isObject() && type->overload(member->name)) {
    Expr *overload = OverloadResolver(this->state).checkOverloadMember(member, expected);

    return ok(overload);
  }
  if (type->isObject()) {
    ObjectChecker::ResultObjectFieldValue field = ObjectChecker(this->state).checkField(member);
    if (field.isErr()) {
      return err(MemberCheckerCheckMemberError{
          .code = MemberCheckerCheckMemberErrorCode::OBJECT_FIELD,
          .message = field.error().message,
      });
    }
    return ok(field.value());
  }
  if ((type->isString() || type->isArray() || type->isListParam()) && member->name == "length") {
    member->type = Type::intTy();
    return ok(member);
  }
  if (type->isInteger() && type->isWrapping() && member->name == "value") {
    Expr *value =
        TypeRelations(this->state).convert(member->object, Type::integer(type->bitWidth(), true));

    return ok(value);
  }
  if (type->isOptional()) {
    std::string message = ErrorTextMemberRead::undefined(member->name);

    return err(MemberCheckerCheckMemberError{
        .code = MemberCheckerCheckMemberErrorCode::MAY_BE_UNDEFINED,
        .message = message,
    });
  }
  if (type == Type::array(Type::string()) && member->name == "raw") {
    std::string message = ErrorTextMemberRead::raw();

    return err(MemberCheckerCheckMemberError{
        .code = MemberCheckerCheckMemberErrorCode::RAW_TEXTS,
        .message = message,
    });
  }
  if (type->isArray() || type->isString() || type->isIterator()) {
    std::string message = ErrorTextMemberRead::mustCall(member->name, type->name());

    return err(MemberCheckerCheckMemberError{
        .code = MemberCheckerCheckMemberErrorCode::METHOD_NOT_CALLED,
        .message = message,
    });
  }
  std::string message = ErrorTextMemberRead::noProperty(member->name, type->name());

  return err(MemberCheckerCheckMemberError{
      .code = MemberCheckerCheckMemberErrorCode::NO_SUCH_PROPERTY,
      .message = message,
  });
}

MemberChecker::ResultIndexIsInteger MemberChecker::positionType(const Type *type) {
  if (!type->isInteger()) {
    std::string message =
        ErrorTextPosition::notInt(type->name(), type->isFloat(), type->isOptional());

    return err(MemberCheckerPositionTypeError{
        .code = MemberCheckerPositionTypeErrorCode::NOT_AN_INTEGER,
        .message = message,
    });
  }

  return ok(type);
}

Expr *MemberChecker::checkPosition(Expr *expr) {
  expr =
      GenericChecker(this->state).apparent(ExprChecker(this->state).checkExpr(expr, Type::intTy()));
  const Type *type = expr->type;
  if (type->isError()) {
    return expr;
  }
  ResultIndexIsInteger integer = MemberChecker::positionType(type);
  if (integer.isErr()) {
    this->state.getDiagnostics().error(expr->loc(), integer.error().message);
    return expr;
  }

  return TypeRelations(this->state).convert(expr, Type::intTy());
}

MemberChecker::ResultIndexedElement MemberChecker::checkIndex(IndexExpr *index) {
  if (Expr *part = ArgListChecker(this->state).listPart(index)) {
    return ok(part);
  }
  if (auto *name = dyn_cast<NameExpr>(index->object)) {
    if (EnumDecl *decl = Scopes(this->state).lookup(name->name).enumDecl()) {
      TypeDeclarer::ResultEnumIndexValue read =
          TypeDeclarer(this->state).checkEnumIndex(decl, index);
      if (read.isOk()) {
        return ok(read.value());
      }
      MemberCheckerCheckIndexErrorCode code = MemberCheckerCheckIndexErrorCode::ALREADY_REPORTED;
      switch (read.error().code) {
        case TypeDeclarerCheckEnumIndexErrorCode::ALREADY_REPORTED:
          code = MemberCheckerCheckIndexErrorCode::ALREADY_REPORTED;
          break;
        case TypeDeclarerCheckEnumIndexErrorCode::STRING_ENUM:
          code = MemberCheckerCheckIndexErrorCode::ENUM_STRING;
          break;
        case TypeDeclarerCheckEnumIndexErrorCode::NOT_AN_INTEGER:
          code = MemberCheckerCheckIndexErrorCode::ENUM_NOT_INTEGER;
          break;
      }

      return err(MemberCheckerCheckIndexError{
          .code = code,
          .message = read.error().message,
      });
    }
  }
  index->object = BuiltinModuleChecker(this->state).processAlias(index->object);
  if (Expr *variable = ProcessChecker(this->state).checkProcessEnvIndex(index)) {
    return ok(variable);
  }
  index->object =
      GenericChecker(this->state).apparent(ExprChecker(this->state).checkExpr(index->object));
  if (index->optional) {
    index->object = ExprChecker(this->state).chainLink(index->object);
  }
  index->type = Type::error();
  UtilityTypes::ResultGenericIndexed generic = UtilityTypes(this->state).checkGenericIndex(index);
  bool genericIndex =
      generic.isOk() ||
      generic.error().code != UtilityTypesCheckGenericIndexErrorCode::NOT_A_GENERIC_INDEX;
  if (generic.isErr() && genericIndex) {
    return err(MemberCheckerCheckIndexError{
        .code = MemberCheckerCheckIndexErrorCode::GENERIC_INDEX,
        .message = generic.error().message,
    });
  }
  if (genericIndex) {
    return ok(generic.value());
  }
  if (index->object->type->isMatch()) {
    Expr *match = MatchChecker(this->state).checkMatchIndex(index);

    return ok(match);
  }
  if (index->object->type->nonOptional()->isJson()) {
    Expr *json = JsonChecker(this->state).checkJsonIndex(index);

    return ok(json);
  }
  if (index->object->type->isBytes()) {
    Expr *bytes = BytesChecker(this->state).checkBytesIndex(index);

    return ok(bytes);
  }
  if (const Type *type = index->object->type; type->isUnknown() || type->isAnyObject()) {
    index->index = ExprChecker(this->state).checkExpr(index->index);
    UnknownChecker::ResultValueNarrowed narrowed = UnknownChecker::unknownUse(index->object, "");
    if (narrowed.isErr()) {
      return err(MemberCheckerCheckIndexError{
          .code = MemberCheckerCheckIndexErrorCode::UNKNOWN_NOT_NARROWED,
          .message = narrowed.error().message,
      });
    }
    return ok(index);
  }
  if (index->object->type->isObject()) {
    KeyedFieldChecker::ResultKeyedFieldRead keyed =
        KeyedFieldChecker(this->state).checkKeyedField(index, false);
    if (keyed.isOk()) {
      return ok(keyed.value());
    }
    KeyedFieldCheckerCheckKeyedFieldErrorCode keyedCode = keyed.error().code;
    bool reported = keyedCode == KeyedFieldCheckerCheckKeyedFieldErrorCode::ALREADY_REPORTED;
    MemberCheckerCheckIndexErrorCode code =
        reported ? MemberCheckerCheckIndexErrorCode::ALREADY_REPORTED
        : KeyedFieldChecker::wrongKey(keyedCode) ? MemberCheckerCheckIndexErrorCode::KEY_REJECTED
                                                 : MemberCheckerCheckIndexErrorCode::FIELD_REJECTED;

    return err(MemberCheckerCheckIndexError{
        .code = code,
        .message = keyed.error().message,
    });
  }
  if (index->object->type->isDict()) {
    DictChecker::ResultDictIndexedValue dict = DictChecker(this->state).checkDictIndex(index);
    if (dict.isErr()) {
      return err(MemberCheckerCheckIndexError{
          .code = MemberCheckerCheckIndexErrorCode::DICT_INDEX,
          .message = dict.error().message,
      });
    }
    return ok(dict.value());
  }
  if (index->object->type->isOptional() && index->object->type->inner()->isObject()) {
    index->index = ExprChecker(this->state).checkExpr(index->index);
    std::string message = ErrorTextIndexRead::undefined();

    return err(MemberCheckerCheckIndexError{
        .code = MemberCheckerCheckIndexErrorCode::VALUE_MAY_BE_UNDEFINED,
        .message = message,
    });
  }
  index->index = this->checkPosition(index->index);
  const Type *type = index->object->type;
  if (type->isError() || index->index->type->isError()) {
    return ok(index);
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
      index->type = Type::optional(ArgListChecker(this->state).listElement(type));
    }
    return ok(index);
  }
  const Type *element = type->isString() ? type : type->isArray() ? type->inner() : nullptr;
  bool optionalList = type->isOptional() && (type->inner()->isArray() || type->inner()->isString());
  if (!element && optionalList) {
    std::string message = ErrorTextIndexRead::undefined();

    return err(MemberCheckerCheckIndexError{
        .code = MemberCheckerCheckIndexErrorCode::VALUE_MAY_BE_UNDEFINED,
        .message = message,
    });
  }
  if (!element) {
    std::string message = ErrorTextIndexRead::notIndexed(type->name());

    return err(MemberCheckerCheckIndexError{
        .code = MemberCheckerCheckIndexErrorCode::NOT_INDEXABLE,
        .message = message,
    });
  }
  bool inRange = IndexProofs(this->state).provesInBounds(index->object, index->index, false);
  if (ElementIndex *access = index->form.as<ElementIndex>()) {
    access->mayBeOutOfRange = !inRange;
  }
  index->type = inRange ? element : Type::optional(element);

  return ok(index);
}

}
