#include "compiler/semantic/bodies/objects/object_checker/object_checker.h"
#include "compiler/program/library/builtin_names/builtin_property_table/builtin_property_table.h"
#include "compiler/program/library/builtin_names/global_member_table/global_member_table.h"
#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/program/library/object/object_member_table/object_member_table.h"
#include "compiler/program/to_utf16/to_utf16.h"
#include "compiler/semantic/bodies/classes/class_member_checker/class_member_checker.h"
#include "compiler/semantic/bodies/classes/overload_resolver/overload_resolver.h"
#include "compiler/semantic/bodies/expr/argument_checker/argument_checker.h"
#include "compiler/semantic/bodies/expr/call_checker/call_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/bodies/objects/dict_methods/dict_methods.h"
#include "compiler/semantic/bodies/objects/object_check_helpers/object_check_helpers.h"
#include "compiler/semantic/declare/class_declarer/class_declarer.h"
#include "compiler/semantic/declare/type_declarer/type_declarer.h"
#include "compiler/semantic/flow/field_facts/field_facts.h"
#include "compiler/semantic/library/events/event_maps/event_maps.h"
#include "compiler/semantic/library/process/process_checker/process_checker.h"
#include "compiler/semantic/names/scopes/scopes.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ConvertUTF.h"

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

ObjectChecker::ObjectChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->scopes = &all.scopes;
  this->eventMaps = &all.eventMaps;
  this->fieldFacts = &all.fieldFacts;
  this->callChecker = &all.callChecker;
  this->dictMethods = &all.dictMethods;
  this->exprChecker = &all.exprChecker;
  this->typeDeclarer = &all.typeDeclarer;
  this->classDeclarer = &all.classDeclarer;
  this->typeRelations = &all.typeRelations;
  this->processChecker = &all.processChecker;
  this->argumentChecker = &all.argumentChecker;
  this->overloadResolver = &all.overloadResolver;
  this->classMemberChecker = &all.classMemberChecker;
}

std::optional<std::string> ObjectChecker::chainText(const Expr *expr) const {
  auto names = std::vector<llvm::StringRef>();
  const auto *link = ObjectCheckHelpers::unnarrowed(expr);
  while (auto *member = dyn_cast<MemberExpr>(link)) {
    if (member->optional) {
      return std::nullopt;
    }
    names.push_back(member->name);
    link = ObjectCheckHelpers::unnarrowed(member->object);
  }
  auto *root = dyn_cast<NameExpr>(link);
  if (root == nullptr) {
    return std::nullopt;
  }
  std::string text = root->name.str();
  for (llvm::StringRef name : llvm::reverse(names)) {
    text += "." + name.str();
  }

  return text;
}

std::string ObjectChecker::fieldText(const Expr *object, llvm::StringRef field) const {
  auto chain = this->chainText(object);
  std::string text = chain.has_value() ? *chain : "o";
  bool isIdentifier = !field.empty() && !llvm::isDigit(field[0]) && llvm::all_of(field, [](char c) {
    return llvm::isAlnum(c) || c == '_' || c == '$';
  });

  return isIdentifier ? text + "." + field.str() : text + "[\"" + field.str() + "\"]";
}

bool ObjectChecker::hasMethodFields(const ClassDecl *classDecl) const {
  if (classDecl->literal() != nullptr) {
    return true;
  }
  for (const ClassDecl *c = classDecl; c != nullptr; c = c->parent) {
    if (llvm::any_of(c->fields, [](const ClassField &field) { return field.method(); })) {
      return true;
    }
  }

  return false;
}

bool ObjectChecker::isInherited(llvm::StringRef name) const {
  auto member = ObjectMemberTable::find(name);

  return member.has_value();
}

std::string ObjectChecker::noFieldText(const Type *type, llvm::StringRef name) const {
  bool isInherited = this->isInherited(name);

  return ErrorTextObjectField::noField(name, type->name(), isInherited);
}

ObjectChecker::ResultObjectStaticCall
ObjectChecker::checkObjectCall(CallExpr *call, MemberExpr *member, const Type *expected) {
  llvm::StringRef functionName = member->name;
  auto objectMember = GlobalMemberTable::find(GlobalName::OBJECT, functionName);
  call->type = Type::error();
  if (objectMember == GlobalMember::OBJECT_GROUP_BY) {
    auto grouped = this->dictMethods->checkGroupBy(call, false);
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
  if (objectMember == GlobalMember::OBJECT_FROM_ENTRIES) {
    auto entries = this->dictMethods->checkFromEntries(call, expected);
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
  if (objectMember != GlobalMember::OBJECT_KEYS && objectMember != GlobalMember::OBJECT_VALUES &&
      objectMember != GlobalMember::OBJECT_ENTRIES) {
    this->argumentChecker->walkArgsLoosely(call);
    auto message = ErrorTextObjectCall::noSupport(functionName);

    return err(ObjectCheckerCheckObjectCallError{
        .code = ObjectCheckerCheckObjectCallErrorCode::NOT_SUPPORTED,
        .message = message,
    });
  }
  if (this->callChecker->rejectSpread(call)) {
    return err(ObjectCheckerCheckObjectCallError{
        .code = ObjectCheckerCheckObjectCallErrorCode::ALREADY_REPORTED,
        .message = "",
    });
  }
  auto arity = this->argumentChecker->checkArity(call, 1, 1);
  if (arity.isErr()) {
    return err(ObjectCheckerCheckObjectCallError{
        .code = ObjectCheckerCheckObjectCallErrorCode::WRONG_ARITY,
        .message = arity.error().message,
    });
  }
  Expr *&arg = call->args[0];
  auto *name = dyn_cast<NameExpr>(arg);
  if (name != nullptr) {
    EnumDecl *enumDecl = this->scopes->lookup(name->name).enumDecl();
    if (enumDecl != nullptr) {
      auto members = this->enumMembers(call, enumDecl, functionName);
      if (members.isErr()) {
        return err(ObjectCheckerCheckObjectCallError{
            .code = ObjectCheckerCheckObjectCallErrorCode::NUMERIC_ENUM,
            .message = members.error().message,
        });
      }
      return ok(members.value());
    }
  }
  arg = this->exprChecker->checkExpr(arg);
  const Type *type = arg->type;
  if (type->isError()) {
    return ok(call);
  }
  if (type->nonOptional()->isJson()) {
    call->target = BuiltinCall{
        .builtin = objectMember == GlobalMember::OBJECT_KEYS     ? Builtin::ObjectKeys
                   : objectMember == GlobalMember::OBJECT_VALUES ? Builtin::ObjectValues
                                                                 : Builtin::ObjectEntries};
    const auto *json = Type::json();
    call->type = objectMember == GlobalMember::OBJECT_KEYS ? Type::array(Type::string())
                 : objectMember == GlobalMember::OBJECT_VALUES
                     ? Type::array(json)
                     : Type::array(Type::tuple({Type::string(), json}));
    return ok(call);
  }
  if (type->isOptional() && (type->inner()->isObject() || type->inner()->isDict())) {
    auto message = ErrorTextObjectCall::undefined();

    return err(ObjectCheckerCheckObjectCallError{
        .code = ObjectCheckerCheckObjectCallErrorCode::MAY_BE_UNDEFINED,
        .message = message,
    });
  }
  if (type->isDict()) {
    auto listed = this->dictMethods->checkDictObjectCall(call, functionName);
    if (listed.isErr()) {
      return err(ObjectCheckerCheckObjectCallError{
          .code = ObjectCheckerCheckObjectCallErrorCode::DICT_ALWAYS_EMPTY,
          .message = listed.error().message,
      });
    }
    return ok(listed.value());
  }
  bool isNotObject = !type->isObject() || type->isTuple() || type->isRegExp() || type->isMatch() ||
                     type->isDate() || type->isIntl() || type->isBytes() || type->isCrypto();
  if (isNotObject) {
    auto message = ErrorTextObjectCall::needObject(functionName, type->name());

    return err(ObjectCheckerCheckObjectCallError{
        .code = ObjectCheckerCheckObjectCallErrorCode::NOT_AN_OBJECT,
        .message = message,
    });
  }
  if (objectMember == GlobalMember::OBJECT_KEYS) {
    call->target = BuiltinCall{.builtin = Builtin::ObjectKeys};
    call->type = Type::array(Type::string());
    return ok(call);
  }
  auto *classDecl = this->classDeclarer->classOf(type);
  if (classDecl != nullptr) {
    this->classDeclarer->completeClass(classDecl);
    if (this->eventMaps->isEmitterClass(classDecl)) {
      auto message = ErrorTextObjectCall::emitter(functionName);

      return err(ObjectCheckerCheckObjectCallError{
          .code = ObjectCheckerCheckObjectCallErrorCode::EMITTER_FIELDS,
          .message = message,
      });
    }
    if (this->hasMethodFields(classDecl)) {
      auto message = ErrorTextObjectCall::methods(functionName);

      return err(ObjectCheckerCheckObjectCallError{
          .code = ObjectCheckerCheckObjectCallErrorCode::METHOD_FIELDS,
          .message = message,
      });
    }
  }
  const Type *element = nullptr;
  auto fields = std::vector<std::pair<const Type *, const Field *>>();
  for (const Field &f : type->fields()) {
    fields.push_back({type, &f});
  }
  for (const Type *candidate : this->state.getInventoryStore().getSubtypes()) {
    if (candidate == type || !candidate->isSubtypeOf(type)) {
      continue;
    }
    auto *sub = this->classDeclarer->classOf(candidate);
    if (sub != nullptr) {
      this->classDeclarer->completeClass(sub);
    }
    for (const Field &f : candidate->fields()) {
      fields.push_back({candidate, &f});
    }
  }
  for (auto [t0, field] : fields) {
    const Field &f = *field;
    const Type *candidate = f.type->nonOptional();
    const Type *both = element != nullptr ? Type::unionOf({element, candidate}) : candidate;
    auto problem = UnionSlots::unionProblem(both);
    if (!problem.empty()) {
      auto message = ErrorTextObjectCall::noUnion(functionName, t0->name(), problem);

      return err(ObjectCheckerCheckObjectCallError{
          .code = ObjectCheckerCheckObjectCallErrorCode::NO_COMMON_TYPE,
          .message = message,
      });
    }
    element = both;
  }
  if (element == nullptr) {
    auto message = ErrorTextObjectCall::noFields(type->name(), functionName);

    return err(ObjectCheckerCheckObjectCallError{
        .code = ObjectCheckerCheckObjectCallErrorCode::NO_FIELDS,
        .message = message,
    });
  }
  if (objectMember == GlobalMember::OBJECT_VALUES) {
    call->target = BuiltinCall{.builtin = Builtin::ObjectValues};
    call->type = Type::array(element);
  } else {
    call->target = BuiltinCall{.builtin = Builtin::ObjectEntries};
    call->type = Type::array(Type::tuple({Type::string(), element}));
  }

  return ok(call);
}

ObjectChecker::ResultEnumMemberValues ObjectChecker::enumMembers(CallExpr *call, EnumDecl *enumDecl,
                                                                 llvm::StringRef functionName) {
  auto objectMember = GlobalMemberTable::find(GlobalName::OBJECT, functionName);
  const auto *values = this->typeDeclarer->enumType(enumDecl);
  if (!enumDecl->isString && objectMember != GlobalMember::OBJECT_KEYS) {
    llvm::StringRef first = enumDecl->members.empty() ? "A" : enumDecl->members.front().name;
    auto message = ErrorTextObjectCall::numberEnum(functionName, enumDecl->name, first);

    return err(ObjectCheckerEnumMembersError{
        .code = ObjectCheckerEnumMembersErrorCode::NUMERIC_ENUM,
        .message = message,
    });
  }
  auto *list = this->state.getAst().make<ArrayLitExpr>(call->loc());
  for (const EnumMember &enumMember : enumDecl->members) {
    auto *name =
        this->state.getAst().make<StringLitExpr>(call->loc(), ToUtf16::toUtf16(enumMember.name));
    auto *member = this->state.getAst().make<MemberExpr>(
        call->loc(), this->state.getAst().make<NameExpr>(call->loc(), enumDecl->name),
        enumMember.name);
    Expr *element = objectMember == GlobalMember::OBJECT_KEYS ? static_cast<Expr *>(name) : member;
    if (objectMember == GlobalMember::OBJECT_ENTRIES) {
      auto *pair = this->state.getAst().make<ArrayLitExpr>(call->loc());
      pair->elements.push_back({name, false});
      pair->elements.push_back({member, false});
      element = pair;
    }
    list->elements.push_back({element, false});
  }
  const Type *element = objectMember == GlobalMember::OBJECT_KEYS ? Type::string()
                        : objectMember == GlobalMember::OBJECT_VALUES
                            ? values
                            : Type::tuple({Type::string(), values});
  auto *checked = this->exprChecker->checkExpr(list, Type::array(element));
  this->state.getInventoryStore().rememberNamedUse(checked, {enumDecl, call->args[0]->loc(), ""});

  return ok(checked);
}

Expr *ObjectChecker::enumObject(NameExpr *name, const Type *expected) {
  const Type *target = expected != nullptr ? expected->nonOptional() : nullptr;
  if (target == nullptr || !target->isDict()) {
    return nullptr;
  }
  EnumDecl *enumDecl = this->scopes->lookup(name->name).enumDecl();
  if (enumDecl == nullptr) {
    return nullptr;
  }
  this->typeDeclarer->resolveEnum(enumDecl);
  auto *object = this->state.getAst().make<ObjectLitExpr>(name->loc());
  for (const EnumMember &member : enumDecl->members) {
    ObjectLitExpr::Property property;
    property.name = member.name;
    property.loc = name->loc();
    property.value = this->state.getAst().make<MemberExpr>(
        name->loc(), this->state.getAst().make<NameExpr>(name->loc(), enumDecl->name), member.name);
    object->props.push_back(property);
  }
  auto *checked = this->exprChecker->checkExpr(object, expected);
  this->state.getInventoryStore().rememberNamedUse(checked, {enumDecl, name->loc(), ""});

  return checked;
}

ObjectChecker::ResultForInKeysAllowed ObjectChecker::walkable(const Type *type,
                                                              llvm::StringRef text) const {
  if (type->isArray() || type->isString() || type->isTuple()) {
    std::string what = type->isArray()    ? ErrorTextForIn::anArray()
                       : type->isString() ? ErrorTextForIn::aString()
                                          : ErrorTextForIn::aTuple();
    auto message = ErrorTextForIn::indices(what, text, type->isArray());

    return err(ObjectCheckerWalkableError{
        .code = ObjectCheckerWalkableErrorCode::INDICES_AS_STRINGS,
        .message = message,
    });
  }
  if (type->isOptional() && type->inner()->isObject()) {
    auto message = ErrorTextForIn::undefined();

    return err(ObjectCheckerWalkableError{
        .code = ObjectCheckerWalkableErrorCode::MAY_BE_UNDEFINED,
        .message = message,
    });
  }
  if (type->nonOptional()->isJson() || type->isRecord()) {
    return ok(type);
  }
  if (type->isDict()) {
    auto message = ErrorTextForIn::dict(text, type->isMap());

    return err(ObjectCheckerWalkableError{
        .code = ObjectCheckerWalkableErrorCode::DICT_PROPERTIES,
        .message = message,
    });
  }
  if (type->isOptional() && type->inner()->isRecord()) {
    auto message = ErrorTextForIn::undefined();

    return err(ObjectCheckerWalkableError{
        .code = ObjectCheckerWalkableErrorCode::MAY_BE_UNDEFINED,
        .message = message,
    });
  }
  bool isNotObject = (!type->isObject() || type->isRegExp() || type->isMatch() || type->isDate() ||
                      type->isIntl() || type->isBytes() || type->isCrypto()) &&
                     !type->isError();
  if (isNotObject) {
    auto message = ErrorTextForIn::notObject(type->name());

    return err(ObjectCheckerWalkableError{
        .code = ObjectCheckerWalkableErrorCode::NOT_AN_OBJECT,
        .message = message,
    });
  }

  return ok(type);
}

void ObjectChecker::walkForIn(ForOfStmt *stmt) {
  auto *variable = dyn_cast<NameExpr>(stmt->iterable);
  bool isEnv = this->processChecker->isProcessMember(stmt->iterable, ProcessMember::ENV);
  auto *object = this->exprChecker->checkExpr(stmt->iterable);
  const Type *type = object->type;
  if (type->isRecord() && (variable != nullptr || isEnv) && !isa<VarDeclStmt>(stmt->body) &&
      !isa<FunctionDecl>(stmt->body)) {
    llvm::SMLoc loc = stmt->body->loc();
    Expr *again =
        variable != nullptr
            ? static_cast<Expr *>(this->state.getAst().make<NameExpr>(loc, variable->name))
            : this->state.getAst().make<MemberExpr>(
                  loc,
                  this->state.getAst().make<NameExpr>(
                      loc, GlobalNameTable::getName(GlobalName::PROCESS)),
                  "env");
    auto *has = this->state.getAst().make<BinaryExpr>(
        loc, BinaryOp::In, this->state.getAst().make<NameExpr>(loc, stmt->binding.name), again);
    auto *skip = this->state.getAst().make<IfStmt>(
        loc, this->state.getAst().make<UnaryExpr>(loc, UnaryOp::Not, has),
        this->state.getAst().make<ContinueStmt>(loc, ""), nullptr);
    auto *block = dyn_cast<BlockStmt>(stmt->body);
    if (block == nullptr) {
      block = this->state.getAst().make<BlockStmt>(loc);
      block->body.push_back(stmt->body);
      stmt->body = block;
    }
    block->body.insert(block->body.begin(), skip);
  }
  std::string text = type->isString() ? "s" : "a";
  auto *name = dyn_cast<NameExpr>(ObjectCheckHelpers::unnarrowed(object));
  if (name != nullptr) {
    text = name->name.str();
  }
  auto allowed = this->walkable(type, text);
  if (allowed.isErr()) {
    this->state.getDiagnostics().error(object->loc(), allowed.error().message);
  }
  bool isWalking = allowed.isOk() && !type->isError();
  llvm::SMLoc loc = object->loc();
  auto objectName = GlobalNameTable::getName(GlobalName::OBJECT);
  auto keysName = GlobalMemberTable::getName(GlobalMember::OBJECT_KEYS);
  auto *callee = this->state.getAst().make<MemberExpr>(
      loc, this->state.getAst().make<NameExpr>(loc, objectName), keysName);
  auto *keys = this->state.getAst().make<CallExpr>(loc, callee, std::vector<Expr *>{object});
  keys->target = BuiltinCall{.builtin = Builtin::ObjectKeys};
  keys->type = isWalking ? Type::array(Type::string()) : Type::error();
  keys->forIn = true;
  stmt->iterable = keys;
}

ObjectChecker::ResultObjectFieldValue ObjectChecker::checkField(MemberExpr *member) {
  const Type *type = member->object->type;
  if (type->isClass()) {
    auto *field = this->classMemberChecker->checkClassMember(member, nullptr);

    return ok(field);
  }
  const auto *field = type->field(member->name);
  if (field == nullptr) {
    member->type = Type::error();
    auto message = this->noFieldText(type, member->name);

    return err(ObjectCheckerCheckFieldError{
        .code = ObjectCheckerCheckFieldErrorCode::NO_SUCH_FIELD,
        .message = message,
    });
  }
  member->type = field->type;
  auto *narrowed = this->fieldFacts->narrowedField(member);
  if (narrowed != member) {
    return ok(narrowed);
  }
  if (field->type->isOptional()) {
    auto path = this->fieldFacts->fieldPath(member);
    if (path && this->fieldFacts->isFieldDefined(*path)) {
      auto *defined = this->typeRelations->convert(member, field->type->inner());

      return ok(defined);
    }
  }

  return ok(member);
}

ObjectChecker::ResultFieldCallIsTyped ObjectChecker::checkFieldCall(CallExpr *call,
                                                                    MemberExpr *member) {
  if (member->object->type->overload(member->name) != nullptr) {
    auto *overload = this->overloadResolver->checkOverloadCall(call, member);

    return ok(overload);
  }
  auto field = this->checkField(member);
  if (field.isErr()) {
    call->callee = member;
    this->argumentChecker->walkArgsLoosely(call);
    call->type = Type::error();
    return err(ObjectCheckerCheckFieldCallError{
        .code = ObjectCheckerCheckFieldCallErrorCode::NO_SUCH_FIELD,
        .message = field.error().message,
    });
  }
  call->callee = field.value();
  const Type *type = call->callee->type;
  if (type->isFunction()) {
    auto valued = this->callChecker->checkValueCall(call, type);
    if (valued.isErr()) {
      return err(ObjectCheckerCheckFieldCallError{
          .code = ObjectCheckerCheckFieldCallErrorCode::TAKES_THIS,
          .message = valued.error().message,
      });
    }
    return ok(valued.value());
  }
  this->argumentChecker->walkArgsLoosely(call);
  call->type = Type::error();
  if (type->isOptional() && type->inner()->isFunction()) {
    auto message = ErrorTextObjectField::maybeUndef();

    return err(ObjectCheckerCheckFieldCallError{
        .code = ObjectCheckerCheckFieldCallErrorCode::MAY_BE_UNDEFINED,
        .message = message,
    });
  }
  if (!type->isError()) {
    auto message = ErrorTextObjectField::notFunc(member->name, type->name());

    return err(ObjectCheckerCheckFieldCallError{
        .code = ObjectCheckerCheckFieldCallErrorCode::NOT_A_FUNCTION,
        .message = message,
    });
  }

  return ok(call);
}

ObjectChecker::ResultTargetObjectType ObjectChecker::objectDefined(const Type *type,
                                                                   const MemberExpr *member) const {
  if (type->isOptional()) {
    auto message = ErrorTextObjectField::setUndef(member->name);

    return err(ObjectCheckerObjectDefinedError{
        .code = ObjectCheckerObjectDefinedErrorCode::MAY_BE_UNDEFINED,
        .message = message,
    });
  }

  return ok(type);
}

ObjectChecker::ResultLengthIsNotFixed ObjectChecker::fixedLength(const Type *type,
                                                                 const MemberExpr *member) const {
  if (type->isTuple() && BuiltinPropertyTable::isName(member->name, BuiltinProperty::LENGTH)) {
    auto message = ErrorTextObjectField::tupleLen();

    return err(ObjectCheckerFixedLengthError{
        .code = ObjectCheckerFixedLengthErrorCode::TUPLE_LENGTH,
        .message = message,
    });
  }

  return ok(type);
}

ObjectChecker::ResultTargetFieldFound ObjectChecker::fieldExists(const Type *type,
                                                                 const MemberExpr *member) const {
  const auto *field = type->field(member->name);
  if (field == nullptr && type->overload(member->name) != nullptr) {
    auto message = ErrorTextObjectField::overloaded(member->name, type->name());

    return err(ObjectCheckerFieldExistsError{
        .code = ObjectCheckerFieldExistsErrorCode::OVERLOADED_METHOD,
        .message = message,
    });
  }
  if (field == nullptr) {
    auto message = this->noFieldText(type, member->name);

    return err(ObjectCheckerFieldExistsError{
        .code = ObjectCheckerFieldExistsErrorCode::NO_SUCH_FIELD,
        .message = message,
    });
  }

  return ok(field);
}

ObjectChecker::ResultTargetIsWritable
ObjectChecker::fieldWritable(const Field *field, const Type *type, const MemberExpr *member) const {
  if (field->readonly) {
    auto message = ErrorTextObjectField::readOnly(member->name, type->name());

    return err(ObjectCheckerFieldWritableError{
        .code = ObjectCheckerFieldWritableErrorCode::READ_ONLY,
        .message = message,
    });
  }

  return ok(field);
}

const Type *ObjectChecker::checkObjectFieldTarget(MemberExpr *member) {
  const Type *type = member->object->type;
  auto defined = this->objectDefined(type, member);
  if (defined.isErr()) {
    this->state.getDiagnostics().error(member->object->loc(), defined.error().message);
    return Type::error();
  }
  auto length = this->fixedLength(type, member);
  if (length.isErr()) {
    this->state.getDiagnostics().error(member->loc(), length.error().message);
    return Type::error();
  }
  auto found = this->fieldExists(type, member);
  if (found.isErr()) {
    this->state.getDiagnostics().error(member->loc(), found.error().message);
    return Type::error();
  }
  const Field *field = found.value();
  auto writable = this->fieldWritable(field, type, member);
  if (writable.isErr()) {
    this->state.getDiagnostics().error(member->loc(), writable.error().message);
  }
  member->type = field->type;

  return field->type;
}

std::string ObjectChecker::objectMismatch(const Type *from, const Type *to, bool conflict) const {
  for (const Field &field : from->fields()) {
    if (to->field(field.name) == nullptr) {
      return ErrorTextMismatch::extraField(field.name);
    }
  }
  for (const Field &t : to->fields()) {
    if (from->field(t.name) == nullptr) {
      return ErrorTextMismatch::noField(t.name);
    }
  }
  for (const Field &field : from->fields()) {
    const auto *t = to->field(field.name);
    if (t->type != field.type) {
      auto message =
          ErrorTextMismatch::fieldType(field.name, field.type->name(), t->type->name(), to->name());
      if (!conflict && field.type->nonOptional()->isNumeric() &&
          t->type->nonOptional()->isNumeric()) {
        message += ErrorTextMismatch::typeHint(to->name());
      }
      return message;
    }
    if (field.readonly && !t->readonly) {
      return ErrorTextMismatch::readonly(field.name, to->name());
    }
  }

  return "";
}

}
