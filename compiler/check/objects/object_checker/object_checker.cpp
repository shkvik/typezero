#include "compiler/check/objects/object_checker/object_checker.h"
#include "compiler/check/classes/class_member_checker/class_member_checker.h"
#include "compiler/check/classes/overload_resolver/overload_resolver.h"
#include "compiler/check/expr/argument_checker/argument_checker.h"
#include "compiler/check/expr/call_checker/call_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/flow/field_facts/field_facts.h"
#include "compiler/check/objects/dict_methods/dict_methods.h"
#include "compiler/check/objects/object_check_helpers/object_check_helpers.h"
#include "compiler/check/scopes/scopes.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/declare/class_declarer/class_declarer.h"
#include "compiler/declare/type_declarer/type_declarer.h"
#include "compiler/library/events/event_maps/event_maps.h"
#include "compiler/library/process/process_checker/process_checker.h"
#include "compiler/whole_program/flows/flows.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ConvertUTF.h"

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

namespace {

std::string chainText(const Expr *expr) {
  expr = unnarrowed(expr);
  if (auto *name = dyn_cast<NameExpr>(expr)) {
    return name->name.str();
  }
  auto *member = dyn_cast<MemberExpr>(expr);
  if (!member || member->optional) {
    return "";
  }
  std::string outer = chainText(member->object);

  return outer.empty() ? "" : outer + "." + member->name.str();
}

}

std::string fieldText(const Expr *object, llvm::StringRef field) {
  std::string text = chainText(object);
  if (text.empty()) {
    text = "o";
  }
  bool identifier = !field.empty() && !llvm::isDigit(field[0]) && llvm::all_of(field, [](char c) {
    return llvm::isAlnum(c) || c == '_' || c == '$';
  });

  return identifier ? text + "." + field.str() : text + "[\"" + field.str() + "\"]";
}

std::u16string ObjectChecker::utf16(llvm::StringRef text) {
  llvm::SmallVector<llvm::UTF16, 32> units;
  llvm::convertUTF8ToUTF16String(text, units);

  return std::u16string(units.begin(), units.end());
}

bool ObjectChecker::hasMethodFields(const ClassDecl *cls) {
  if (cls->literal()) {
    return true;
  }
  for (const ClassDecl *c = cls; c; c = c->parent) {
    if (llvm::any_of(c->fields, [](const ClassField &f) { return f.method(); })) {
      return true;
    }
  }

  return false;
}

bool ObjectChecker::isInherited(llvm::StringRef name) {
  static const char *const names[] = {"toString",      "valueOf",        "hasOwnProperty",
                                      "isPrototypeOf", "toLocaleString", "propertyIsEnumerable",
                                      "constructor",   "__proto__"};

  return llvm::is_contained(names, name);
}

std::string ObjectChecker::noFieldText(const Type *type, llvm::StringRef name) {
  bool inherited = ObjectChecker::isInherited(name);

  return ErrorTextObjectField::noField(name, type->name(), inherited);
}

ObjectChecker::ObjectChecker(Checker &owner)
    : checker(owner) {}

ObjectChecker::ResultObjectStaticCall
ObjectChecker::checkObjectCall(CallExpr *call, MemberExpr *member, const Type *expected) {
  llvm::StringRef fn = member->name;
  call->type = Type::error();
  if (fn == "groupBy") {
    DictMethods::ResultGroupByCallTyped grouped =
        this->checker.dictMethods().checkGroupBy(call, false);
    if (grouped.isOk()) {
      return ok(grouped.value());
    }
    ObjectCheckerCheckObjectCallErrorCode code =
        ObjectCheckerCheckObjectCallErrorCode::ALREADY_REPORTED;
    switch (grouped.error().code) {
      case DictMethodsCheckGroupByErrorCode::ALREADY_REPORTED:
        code = ObjectCheckerCheckObjectCallErrorCode::ALREADY_REPORTED;
        break;
      case DictMethodsCheckGroupByErrorCode::NO_GROUP_KEY:
      case DictMethodsCheckGroupByErrorCode::KEYS_NOT_STRINGS:
      case DictMethodsCheckGroupByErrorCode::BAD_KEY_TYPE:
        code = ObjectCheckerCheckObjectCallErrorCode::GROUP_BY_KEY;
        break;
      case DictMethodsCheckGroupByErrorCode::WRONG_ARITY:
        code = ObjectCheckerCheckObjectCallErrorCode::GROUP_BY_ARITY;
        break;
    }

    return err(ObjectCheckerCheckObjectCallError{
        .code = code,
        .message = grouped.error().message,
    });
  }
  if (fn == "fromEntries") {
    DictMethods::ResultFromEntriesTyped entries =
        this->checker.dictMethods().checkFromEntries(call, expected);
    if (entries.isOk()) {
      return ok(entries.value());
    }
    ObjectCheckerCheckObjectCallErrorCode code =
        ObjectCheckerCheckObjectCallErrorCode::ALREADY_REPORTED;
    switch (entries.error().code) {
      case DictMethodsCheckFromEntriesErrorCode::ALREADY_REPORTED:
        code = ObjectCheckerCheckObjectCallErrorCode::ALREADY_REPORTED;
        break;
      case DictMethodsCheckFromEntriesErrorCode::KEYS_NOT_STRINGS:
      case DictMethodsCheckFromEntriesErrorCode::VALUES_UNDEFINED:
        code = ObjectCheckerCheckObjectCallErrorCode::FROM_ENTRIES_PAIRS;
        break;
      case DictMethodsCheckFromEntriesErrorCode::WRONG_ARITY:
        code = ObjectCheckerCheckObjectCallErrorCode::FROM_ENTRIES_ARITY;
        break;
    }

    return err(ObjectCheckerCheckObjectCallError{
        .code = code,
        .message = entries.error().message,
    });
  }
  if (fn != "keys" && fn != "values" && fn != "entries") {
    this->checker.argumentChecker().checkArgsLoosely(call);
    std::string message = ErrorTextObjectCall::noSupport(fn);

    return err(ObjectCheckerCheckObjectCallError{
        .code = ObjectCheckerCheckObjectCallErrorCode::NOT_SUPPORTED,
        .message = message,
    });
  }
  if (this->checker.callChecker().rejectSpread(call)) {
    return err(ObjectCheckerCheckObjectCallError{
        .code = ObjectCheckerCheckObjectCallErrorCode::ALREADY_REPORTED,
        .message = "",
    });
  }
  ArgumentChecker::ResultArityMatches arity =
      this->checker.argumentChecker().checkArity(call, 1, 1);
  if (arity.isErr()) {
    return err(ObjectCheckerCheckObjectCallError{
        .code = ObjectCheckerCheckObjectCallErrorCode::WRONG_ARITY,
        .message = arity.error().message,
    });
  }
  Expr *&arg = call->args[0];
  if (auto *name = dyn_cast<NameExpr>(arg)) {
    if (EnumDecl *decl = this->checker.scopes().lookup(name->name).enumDecl()) {
      ResultEnumMemberValues members = this->enumMembers(call, decl, fn);
      if (members.isErr()) {
        return err(ObjectCheckerCheckObjectCallError{
            .code = ObjectCheckerCheckObjectCallErrorCode::NUMERIC_ENUM,
            .message = members.error().message,
        });
      }
      return ok(members.value());
    }
  }
  arg = this->checker.exprChecker().checkExpr(arg);
  const Type *type = arg->type;
  if (type->isError()) {
    return ok(call);
  }
  if (type->nonOptional()->isJson()) {
    call->target = BuiltinCall{.builtin = fn == "keys"     ? Builtin::ObjectKeys
                                          : fn == "values" ? Builtin::ObjectValues
                                                           : Builtin::ObjectEntries};
    const Type *json = Type::json();
    call->type = fn == "keys"     ? Type::array(Type::string())
                 : fn == "values" ? Type::array(json)
                                  : Type::array(Type::tuple({Type::string(), json}));
    return ok(call);
  }
  if (type->isOptional() && (type->inner()->isObject() || type->inner()->isDict())) {
    std::string message = ErrorTextObjectCall::undefined();

    return err(ObjectCheckerCheckObjectCallError{
        .code = ObjectCheckerCheckObjectCallErrorCode::MAY_BE_UNDEFINED,
        .message = message,
    });
  }
  if (type->isDict()) {
    DictMethods::ResultDictObjectCalled listed =
        this->checker.dictMethods().checkDictObjectCall(call, fn);
    if (listed.isErr()) {
      return err(ObjectCheckerCheckObjectCallError{
          .code = ObjectCheckerCheckObjectCallErrorCode::DICT_ALWAYS_EMPTY,
          .message = listed.error().message,
      });
    }
    return ok(listed.value());
  }
  bool notObject = !type->isObject() || type->isTuple() || type->isRegExp() || type->isMatch() ||
                   type->isDate() || type->isIntl() || type->isBytes() || type->isCrypto();
  if (notObject) {
    std::string message = ErrorTextObjectCall::needObject(fn, type->name());

    return err(ObjectCheckerCheckObjectCallError{
        .code = ObjectCheckerCheckObjectCallErrorCode::NOT_AN_OBJECT,
        .message = message,
    });
  }
  if (fn == "keys") {
    call->target = BuiltinCall{.builtin = Builtin::ObjectKeys};
    call->type = Type::array(Type::string());
    return ok(call);
  }
  if (ClassDecl *cls = ClassDeclarer::classOf(type)) {
    this->checker.classDeclarer().completeClass(cls);
    if (this->checker.eventMaps().isEmitterClass(cls)) {
      std::string message = ErrorTextObjectCall::emitter(fn);

      return err(ObjectCheckerCheckObjectCallError{
          .code = ObjectCheckerCheckObjectCallErrorCode::EMITTER_FIELDS,
          .message = message,
      });
    }
    if (ObjectChecker::hasMethodFields(cls)) {
      std::string message = ErrorTextObjectCall::methods(fn);

      return err(ObjectCheckerCheckObjectCallError{
          .code = ObjectCheckerCheckObjectCallErrorCode::METHOD_FIELDS,
          .message = message,
      });
    }
  }
  const Type *element = nullptr;
  std::vector<std::pair<const Type *, const Field *>> fields;
  for (const Field &f : type->fields()) {
    fields.push_back({type, &f});
  }
  for (const Type *t : this->checker.file_->subtypes) {
    if (t == type || !t->isSubtypeOf(type)) {
      continue;
    }
    if (ClassDecl *sub = ClassDeclarer::classOf(t)) {
      this->checker.classDeclarer().completeClass(sub);
    }
    for (const Field &f : t->fields()) {
      fields.push_back({t, &f});
    }
  }
  for (auto [t0, field] : fields) {
    const Field &f = *field;
    const Type *t = f.type->nonOptional();
    const Type *both = element ? Type::unionOf({element, t}) : t;
    std::string problem = unionProblem(both);
    if (!problem.empty()) {
      std::string message = ErrorTextObjectCall::noUnion(fn, t0->name(), problem);

      return err(ObjectCheckerCheckObjectCallError{
          .code = ObjectCheckerCheckObjectCallErrorCode::NO_COMMON_TYPE,
          .message = message,
      });
    }
    element = both;
  }
  if (!element) {
    std::string message = ErrorTextObjectCall::noFields(type->name(), fn);

    return err(ObjectCheckerCheckObjectCallError{
        .code = ObjectCheckerCheckObjectCallErrorCode::NO_FIELDS,
        .message = message,
    });
  }
  if (fn == "values") {
    call->target = BuiltinCall{.builtin = Builtin::ObjectValues};
    call->type = Type::array(element);
  } else {
    call->target = BuiltinCall{.builtin = Builtin::ObjectEntries};
    call->type = Type::array(Type::tuple({Type::string(), element}));
  }

  return ok(call);
}

ObjectChecker::ResultEnumMemberValues ObjectChecker::enumMembers(CallExpr *call, EnumDecl *decl,
                                                                 llvm::StringRef fn) {
  const Type *values = this->checker.typeDeclarer().enumType(decl);
  if (!decl->isString && fn != "keys") {
    llvm::StringRef first = decl->members.empty() ? "A" : decl->members.front().name;
    std::string message = ErrorTextObjectCall::numberEnum(fn, decl->name, first);

    return err(ObjectCheckerEnumMembersError{
        .code = ObjectCheckerEnumMembersErrorCode::NUMERIC_ENUM,
        .message = message,
    });
  }
  auto *list = this->checker.ctx_.make<ArrayLitExpr>(call->loc());
  for (const EnumMember &m : decl->members) {
    auto *name = this->checker.ctx_.make<StringLitExpr>(call->loc(), ObjectChecker::utf16(m.name));
    auto *member = this->checker.ctx_.make<MemberExpr>(
        call->loc(), this->checker.ctx_.make<NameExpr>(call->loc(), decl->name), m.name);
    Expr *element = fn == "keys" ? static_cast<Expr *>(name) : member;
    if (fn == "entries") {
      auto *pair = this->checker.ctx_.make<ArrayLitExpr>(call->loc());
      pair->elements.push_back({name, false});
      pair->elements.push_back({member, false});
      element = pair;
    }
    list->elements.push_back({element, false});
  }
  const Type *element = fn == "keys"     ? Type::string()
                        : fn == "values" ? values
                                         : Type::tuple({Type::string(), values});
  Expr *checked = this->checker.exprChecker().checkExpr(list, Type::array(element));
  this->checker.file_->namedUses[checked] = {decl, call->args[0]->loc(), ""};

  return ok(checked);
}

Expr *ObjectChecker::enumObject(NameExpr *name, const Type *expected) {
  const Type *target = expected ? expected->nonOptional() : nullptr;
  if (!target || !target->isDict()) {
    return nullptr;
  }
  EnumDecl *decl = this->checker.scopes().lookup(name->name).enumDecl();
  if (!decl) {
    return nullptr;
  }
  this->checker.typeDeclarer().resolveEnum(decl);
  auto *object = this->checker.ctx_.make<ObjectLitExpr>(name->loc());
  for (const EnumMember &m : decl->members) {
    ObjectLitExpr::Property p;
    p.name = m.name;
    p.loc = name->loc();
    p.value = this->checker.ctx_.make<MemberExpr>(
        name->loc(), this->checker.ctx_.make<NameExpr>(name->loc(), decl->name), m.name);
    object->props.push_back(p);
  }
  Expr *checked = this->checker.exprChecker().checkExpr(object, expected);
  this->checker.file_->namedUses[checked] = {decl, name->loc(), ""};

  return checked;
}

ObjectChecker::ResultForInKeysAllowed ObjectChecker::walkable(const Type *type,
                                                              llvm::StringRef text) {
  if (type->isArray() || type->isString() || type->isTuple()) {
    std::string what = type->isArray()    ? ErrorTextForIn::anArray()
                       : type->isString() ? ErrorTextForIn::aString()
                                          : ErrorTextForIn::aTuple();
    std::string message = ErrorTextForIn::indices(what, text, type->isArray());

    return err(ObjectCheckerWalkableError{
        .code = ObjectCheckerWalkableErrorCode::INDICES_AS_STRINGS,
        .message = message,
    });
  }
  if (type->isOptional() && type->inner()->isObject()) {
    std::string message = ErrorTextForIn::undefined();

    return err(ObjectCheckerWalkableError{
        .code = ObjectCheckerWalkableErrorCode::MAY_BE_UNDEFINED,
        .message = message,
    });
  }
  if (type->nonOptional()->isJson() || type->isRecord()) {
    return ok(type);
  }
  if (type->isDict()) {
    std::string message = ErrorTextForIn::dict(text, type->isMap());

    return err(ObjectCheckerWalkableError{
        .code = ObjectCheckerWalkableErrorCode::DICT_PROPERTIES,
        .message = message,
    });
  }
  if (type->isOptional() && type->inner()->isRecord()) {
    std::string message = ErrorTextForIn::undefined();

    return err(ObjectCheckerWalkableError{
        .code = ObjectCheckerWalkableErrorCode::MAY_BE_UNDEFINED,
        .message = message,
    });
  }
  bool notObject = (!type->isObject() || type->isRegExp() || type->isMatch() || type->isDate() ||
                    type->isIntl() || type->isBytes() || type->isCrypto()) &&
                   !type->isError();
  if (notObject) {
    std::string message = ErrorTextForIn::notObject(type->name());

    return err(ObjectCheckerWalkableError{
        .code = ObjectCheckerWalkableErrorCode::NOT_AN_OBJECT,
        .message = message,
    });
  }

  return ok(type);
}

void ObjectChecker::checkForIn(ForOfStmt *stmt) {
  auto *variable = dyn_cast<NameExpr>(stmt->iterable);
  bool env = this->checker.processChecker().isProcessMember(stmt->iterable, "env");
  Expr *object = this->checker.exprChecker().checkExpr(stmt->iterable);
  const Type *type = object->type;
  if (type->isRecord() && (variable || env) && !isa<VarDeclStmt>(stmt->body) &&
      !isa<FunctionDecl>(stmt->body)) {
    llvm::SMLoc loc = stmt->body->loc();
    Expr *again = variable
                      ? static_cast<Expr *>(this->checker.ctx_.make<NameExpr>(loc, variable->name))
                      : this->checker.ctx_.make<MemberExpr>(
                            loc, this->checker.ctx_.make<NameExpr>(loc, "process"), "env");
    auto *has = this->checker.ctx_.make<BinaryExpr>(
        loc, BinaryOp::In, this->checker.ctx_.make<NameExpr>(loc, stmt->binding.name), again);
    auto *skip = this->checker.ctx_.make<IfStmt>(
        loc, this->checker.ctx_.make<UnaryExpr>(loc, UnaryOp::Not, has),
        this->checker.ctx_.make<ContinueStmt>(loc, ""), nullptr);
    auto *block = dyn_cast<BlockStmt>(stmt->body);
    if (!block) {
      block = this->checker.ctx_.make<BlockStmt>(loc);
      block->body.push_back(stmt->body);
      stmt->body = block;
    }
    block->body.insert(block->body.begin(), skip);
  }
  std::string text = type->isString() ? "s" : "a";
  if (auto *name = dyn_cast<NameExpr>(unnarrowed(object))) {
    text = name->name.str();
  }
  ResultForInKeysAllowed allowed = ObjectChecker::walkable(type, text);
  if (allowed.isErr()) {
    this->checker.diags_.error(object->loc(), allowed.error().message);
  }
  bool walks = allowed.isOk() && !type->isError();
  llvm::SMLoc loc = object->loc();
  auto *callee = this->checker.ctx_.make<MemberExpr>(
      loc, this->checker.ctx_.make<NameExpr>(loc, "Object"), "keys");
  auto *keys = this->checker.ctx_.make<CallExpr>(loc, callee, std::vector<Expr *>{object});
  keys->target = BuiltinCall{.builtin = Builtin::ObjectKeys};
  keys->type = walks ? Type::array(Type::string()) : Type::error();
  keys->forIn = true;
  stmt->iterable = keys;
}

ObjectChecker::ResultObjectFieldValue ObjectChecker::checkField(MemberExpr *member) {
  const Type *type = member->object->type;
  if (type->isClass()) {
    Expr *field = this->checker.classMemberChecker().checkClassMember(member, nullptr);

    return ok(field);
  }
  const Field *field = type->field(member->name);
  if (!field) {
    member->type = Type::error();
    std::string message = ObjectChecker::noFieldText(type, member->name);

    return err(ObjectCheckerCheckFieldError{
        .code = ObjectCheckerCheckFieldErrorCode::NO_SUCH_FIELD,
        .message = message,
    });
  }
  member->type = field->type;
  if (Expr *narrowed = this->checker.fieldFacts().narrowedField(member); narrowed != member) {
    return ok(narrowed);
  }
  if (field->type->isOptional()) {
    if (std::optional<Checker::FieldPath> path = this->checker.fieldFacts().fieldPath(member);
        path && this->checker.fieldFacts().isFieldDefined(*path)) {
      Expr *defined = this->checker.typeRelations().convert(member, field->type->inner());

      return ok(defined);
    }
  }

  return ok(member);
}

ObjectChecker::ResultFieldCallIsTyped ObjectChecker::checkFieldCall(CallExpr *call,
                                                                    MemberExpr *member) {
  if (member->object->type->overload(member->name)) {
    Expr *overload = this->checker.overloadResolver().checkOverloadCall(call, member);

    return ok(overload);
  }
  ResultObjectFieldValue field = this->checkField(member);
  if (field.isErr()) {
    call->callee = member;
    this->checker.argumentChecker().checkArgsLoosely(call);
    call->type = Type::error();
    return err(ObjectCheckerCheckFieldCallError{
        .code = ObjectCheckerCheckFieldCallErrorCode::NO_SUCH_FIELD,
        .message = field.error().message,
    });
  }
  call->callee = field.value();
  const Type *type = call->callee->type;
  if (type->isFunction()) {
    CallChecker::ResultValueCallTyped valued =
        this->checker.callChecker().checkValueCall(call, type);
    if (valued.isErr()) {
      return err(ObjectCheckerCheckFieldCallError{
          .code = ObjectCheckerCheckFieldCallErrorCode::TAKES_THIS,
          .message = valued.error().message,
      });
    }
    return ok(valued.value());
  }
  this->checker.argumentChecker().checkArgsLoosely(call);
  call->type = Type::error();
  if (type->isOptional() && type->inner()->isFunction()) {
    std::string message = ErrorTextObjectField::maybeUndef();

    return err(ObjectCheckerCheckFieldCallError{
        .code = ObjectCheckerCheckFieldCallErrorCode::MAY_BE_UNDEFINED,
        .message = message,
    });
  }
  if (!type->isError()) {
    std::string message = ErrorTextObjectField::notFunc(member->name, type->name());

    return err(ObjectCheckerCheckFieldCallError{
        .code = ObjectCheckerCheckFieldCallErrorCode::NOT_A_FUNCTION,
        .message = message,
    });
  }

  return ok(call);
}

ObjectChecker::ResultTargetObjectType ObjectChecker::objectDefined(const Type *type,
                                                                   const MemberExpr *member) {
  if (type->isOptional()) {
    std::string message = ErrorTextObjectField::setUndef(member->name);

    return err(ObjectCheckerObjectDefinedError{
        .code = ObjectCheckerObjectDefinedErrorCode::MAY_BE_UNDEFINED,
        .message = message,
    });
  }

  return ok(type);
}

ObjectChecker::ResultLengthIsNotFixed ObjectChecker::fixedLength(const Type *type,
                                                                 const MemberExpr *member) {
  if (type->isTuple() && member->name == "length") {
    std::string message = ErrorTextObjectField::tupleLen();

    return err(ObjectCheckerFixedLengthError{
        .code = ObjectCheckerFixedLengthErrorCode::TUPLE_LENGTH,
        .message = message,
    });
  }

  return ok(type);
}

ObjectChecker::ResultTargetFieldFound ObjectChecker::fieldExists(const Type *type,
                                                                 const MemberExpr *member) {
  const Field *field = type->field(member->name);
  if (!field && type->overload(member->name)) {
    std::string message = ErrorTextObjectField::overloaded(member->name, type->name());

    return err(ObjectCheckerFieldExistsError{
        .code = ObjectCheckerFieldExistsErrorCode::OVERLOADED_METHOD,
        .message = message,
    });
  }
  if (!field) {
    std::string message = ObjectChecker::noFieldText(type, member->name);

    return err(ObjectCheckerFieldExistsError{
        .code = ObjectCheckerFieldExistsErrorCode::NO_SUCH_FIELD,
        .message = message,
    });
  }

  return ok(field);
}

ObjectChecker::ResultTargetIsWritable
ObjectChecker::fieldWritable(const Field *field, const Type *type, const MemberExpr *member) {
  if (field->readonly) {
    std::string message = ErrorTextObjectField::readOnly(member->name, type->name());

    return err(ObjectCheckerFieldWritableError{
        .code = ObjectCheckerFieldWritableErrorCode::READ_ONLY,
        .message = message,
    });
  }

  return ok(field);
}

const Type *ObjectChecker::checkObjectFieldTarget(MemberExpr *member) {
  const Type *type = member->object->type;
  ResultTargetObjectType defined = ObjectChecker::objectDefined(type, member);
  if (defined.isErr()) {
    this->checker.diags_.error(member->object->loc(), defined.error().message);
    return Type::error();
  }
  ResultLengthIsNotFixed length = ObjectChecker::fixedLength(type, member);
  if (length.isErr()) {
    this->checker.diags_.error(member->loc(), length.error().message);
    return Type::error();
  }
  ResultTargetFieldFound found = ObjectChecker::fieldExists(type, member);
  if (found.isErr()) {
    this->checker.diags_.error(member->loc(), found.error().message);
    return Type::error();
  }
  const Field *field = found.value();
  ResultTargetIsWritable writable = ObjectChecker::fieldWritable(field, type, member);
  if (writable.isErr()) {
    this->checker.diags_.error(member->loc(), writable.error().message);
  }
  member->type = field->type;

  return field->type;
}

std::string ObjectChecker::objectMismatch(const Type *from, const Type *to, bool conflict) const {
  for (const Field &f : from->fields()) {
    if (!to->field(f.name)) {
      return ErrorTextMismatch::extraField(f.name);
    }
  }
  for (const Field &t : to->fields()) {
    if (!from->field(t.name)) {
      return ErrorTextMismatch::noField(t.name);
    }
  }
  for (const Field &f : from->fields()) {
    const Field *t = to->field(f.name);
    if (t->type != f.type) {
      std::string message =
          ErrorTextMismatch::fieldType(f.name, f.type->name(), t->type->name(), to->name());
      if (!conflict && f.type->nonOptional()->isNumeric() && t->type->nonOptional()->isNumeric()) {
        message += ErrorTextMismatch::typeHint(to->name());
      }
      return message;
    }
    if (f.readonly && !t->readonly) {
      return ErrorTextMismatch::readonly(f.name, to->name());
    }
  }

  return "";
}

}
