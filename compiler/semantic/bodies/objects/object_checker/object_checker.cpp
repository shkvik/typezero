#include "compiler/semantic/bodies/objects/object_checker/object_checker.h"
#include "compiler/program/library/builtin_names/builtin_property_table/builtin_property_table.h"
#include "compiler/program/library/builtin_names/global_member_table/global_member_table.h"
#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/program/library/object/object_member_table/object_member_table.h"
#include "compiler/program/library/process/process_member_table/process_member_table.h"
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

namespace {

constexpr const char *kObjectPlaceholder = "o";
constexpr const char *kStringPlaceholder = "s";
constexpr const char *kArrayPlaceholder = "a";
constexpr const char *kEnumMemberPlaceholder = "A";

}

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
  auto *member = dyn_cast<MemberExpr>(link);
  while (member != nullptr) {
    if (member->optional) {
      return std::nullopt;
    }
    names.push_back(member->name);
    link = ObjectCheckHelpers::unnarrowed(member->object);
    member = dyn_cast<MemberExpr>(link);
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
  std::string text = chain.has_value() ? *chain : kObjectPlaceholder;
  bool isIdentifier = !field.empty() && !llvm::isDigit(field[0]) &&
                      llvm::all_of(field, std::bind_front(&ObjectChecker::isIdentifierChar, this));

  return isIdentifier ? text + "." + field.str() : text + "[\"" + field.str() + "\"]";
}

bool ObjectChecker::isIdentifierChar(char letter) const {
  return llvm::isAlnum(letter) || letter == '_' || letter == '$';
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
    return this->checkObjectGroupBy(call);
  }
  if (objectMember == GlobalMember::OBJECT_FROM_ENTRIES) {
    return this->checkObjectFromEntries(call, expected);
  }
  bool isListing = objectMember == GlobalMember::OBJECT_KEYS ||
                   objectMember == GlobalMember::OBJECT_VALUES ||
                   objectMember == GlobalMember::OBJECT_ENTRIES;
  if (!isListing) {
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

  return this->checkObjectListing(call, *objectMember, functionName);
}

ObjectChecker::ResultObjectStaticCall ObjectChecker::checkObjectGroupBy(CallExpr *call) {
  auto grouped = this->dictMethods->checkGroupBy(call, GroupByOwner::OBJECT);
  if (grouped.isOk()) {
    return ok(grouped.value());
  }
  auto code = this->groupByCode(grouped.error().code);

  return err(ObjectCheckerCheckObjectCallError{
      .code = code,
      .message = grouped.error().message,
  });
}

ObjectCheckerCheckObjectCallErrorCode
ObjectChecker::groupByCode(DictMethodsCheckGroupByErrorCode code) const {
  switch (code) {
    case DictMethodsCheckGroupByErrorCode::ALREADY_REPORTED:
      return ObjectCheckerCheckObjectCallErrorCode::ALREADY_REPORTED;
    case DictMethodsCheckGroupByErrorCode::NO_GROUP_KEY:
    case DictMethodsCheckGroupByErrorCode::KEYS_NOT_STRINGS:
    case DictMethodsCheckGroupByErrorCode::BAD_KEY_TYPE:
      return ObjectCheckerCheckObjectCallErrorCode::GROUP_BY_KEY;
    case DictMethodsCheckGroupByErrorCode::WRONG_ARITY:
      return ObjectCheckerCheckObjectCallErrorCode::GROUP_BY_ARITY;
  }

  return ObjectCheckerCheckObjectCallErrorCode::ALREADY_REPORTED;
}

ObjectChecker::ResultObjectStaticCall ObjectChecker::checkObjectFromEntries(CallExpr *call,
                                                                            const Type *expected) {
  auto entries = this->dictMethods->checkFromEntries(call, expected);
  if (entries.isOk()) {
    return ok(entries.value());
  }
  auto code = this->fromEntriesCode(entries.error().code);

  return err(ObjectCheckerCheckObjectCallError{
      .code = code,
      .message = entries.error().message,
  });
}

ObjectCheckerCheckObjectCallErrorCode
ObjectChecker::fromEntriesCode(DictMethodsCheckFromEntriesErrorCode code) const {
  switch (code) {
    case DictMethodsCheckFromEntriesErrorCode::ALREADY_REPORTED:
      return ObjectCheckerCheckObjectCallErrorCode::ALREADY_REPORTED;
    case DictMethodsCheckFromEntriesErrorCode::KEYS_NOT_STRINGS:
    case DictMethodsCheckFromEntriesErrorCode::VALUES_UNDEFINED:
      return ObjectCheckerCheckObjectCallErrorCode::FROM_ENTRIES_PAIRS;
    case DictMethodsCheckFromEntriesErrorCode::WRONG_ARITY:
      return ObjectCheckerCheckObjectCallErrorCode::FROM_ENTRIES_ARITY;
  }

  return ObjectCheckerCheckObjectCallErrorCode::ALREADY_REPORTED;
}

ObjectChecker::ResultObjectStaticCall
ObjectChecker::checkObjectListing(CallExpr *call, GlobalMember listing,
                                  llvm::StringRef functionName) {
  auto *name = dyn_cast<NameExpr>(call->args[0]);
  EnumDecl *enumDecl = name != nullptr ? this->scopes->lookup(name->name).enumDecl() : nullptr;
  if (enumDecl != nullptr) {
    return this->checkEnumListing(call, enumDecl, listing, functionName);
  }
  call->args[0] = this->exprChecker->checkExpr(call->args[0]);
  const Type *type = call->args[0]->type;
  if (type->isError()) {
    return ok(call);
  }
  if (type->nonOptional()->isJson()) {
    auto *listed = this->typedListing(call, listing, Type::json());
    return ok(listed);
  }
  if (type->isOptional() && (type->inner()->isObject() || type->inner()->isDict())) {
    auto message = ErrorTextObjectCall::undefined();

    return err(ObjectCheckerCheckObjectCallError{
        .code = ObjectCheckerCheckObjectCallErrorCode::MAY_BE_UNDEFINED,
        .message = message,
    });
  }
  if (type->isDict()) {
    return this->checkDictListing(call, functionName);
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
  if (listing == GlobalMember::OBJECT_KEYS) {
    auto *listed = this->typedListing(call, listing, nullptr);
    return ok(listed);
  }
  auto listedClass = this->checkListedClass(type, functionName);
  if (listedClass.isErr()) {
    return err(listedClass.error());
  }
  auto element = this->checkListedElement(type, functionName);
  if (element.isErr()) {
    return err(element.error());
  }
  auto *listed = this->typedListing(call, listing, element.value());

  return ok(listed);
}

Expr *ObjectChecker::typedListing(CallExpr *call, GlobalMember listing, const Type *value) const {
  call->target = BuiltinCall{.builtin = this->dictMethods->objectCallBuiltin(listing)};
  call->type = this->dictMethods->objectCallType(listing, value);

  return call;
}

ObjectChecker::ResultObjectStaticCall
ObjectChecker::checkDictListing(CallExpr *call, llvm::StringRef functionName) {
  auto listed = this->dictMethods->checkDictObjectCall(call, functionName);
  if (listed.isErr()) {
    return err(ObjectCheckerCheckObjectCallError{
        .code = ObjectCheckerCheckObjectCallErrorCode::DICT_ALWAYS_EMPTY,
        .message = listed.error().message,
    });
  }

  return ok(listed.value());
}

ObjectChecker::ResultListedType ObjectChecker::checkListedClass(const Type *type,
                                                                llvm::StringRef functionName) {
  auto *classDecl = this->classDeclarer->classOf(type);
  if (classDecl == nullptr) {
    return ok(type);
  }
  this->classDeclarer->completeClass(classDecl);
  if (this->eventMaps->isEmitterClass(classDecl)) {
    auto message = ErrorTextObjectCall::emitter(functionName);

    return err(ObjectCheckerCheckObjectCallError{
        .code = ObjectCheckerCheckObjectCallErrorCode::EMITTER_FIELDS,
        .message = message,
    });
  }
  if (this->classDeclarer->hasMethodFields(classDecl)) {
    auto message = ErrorTextObjectCall::methods(functionName);

    return err(ObjectCheckerCheckObjectCallError{
        .code = ObjectCheckerCheckObjectCallErrorCode::METHOD_FIELDS,
        .message = message,
    });
  }

  return ok(type);
}

ObjectChecker::ResultListedType ObjectChecker::checkListedElement(const Type *type,
                                                                  llvm::StringRef functionName) {
  auto fields = this->listedFields(type);
  const Type *element = nullptr;
  for (const OwnedField &owned : fields) {
    const Type *candidate = owned.field->type->nonOptional();
    const Type *both = element != nullptr ? Type::unionOf({element, candidate}) : candidate;
    auto problem = UnionSlots::unionProblem(both);
    if (!problem.empty()) {
      auto message = ErrorTextObjectCall::noUnion(functionName, owned.owner->name(), problem);

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

  return ok(element);
}

std::vector<ObjectChecker::OwnedField> ObjectChecker::listedFields(const Type *type) {
  auto fields = std::vector<OwnedField>();
  this->addOwnedFields(type, fields);
  for (const Type *candidate : this->state.getInventoryStore().getSubtypes()) {
    if (candidate != type && candidate->isSubtypeOf(type)) {
      this->addSubtypeFields(candidate, fields);
    }
  }

  return fields;
}

void ObjectChecker::addSubtypeFields(const Type *subtype, std::vector<OwnedField> &fields) {
  auto *subclass = this->classDeclarer->classOf(subtype);
  if (subclass != nullptr) {
    this->classDeclarer->completeClass(subclass);
  }
  this->addOwnedFields(subtype, fields);
}

void ObjectChecker::addOwnedFields(const Type *owner, std::vector<OwnedField> &fields) const {
  for (const Field &field : owner->fields()) {
    fields.push_back(OwnedField{.owner = owner, .field = &field});
  }
}

ObjectChecker::ResultObjectStaticCall
ObjectChecker::checkEnumListing(CallExpr *call, EnumDecl *enumDecl, GlobalMember listing,
                                llvm::StringRef functionName) {
  auto members = this->checkEnumMembers(call, enumDecl, listing, functionName);
  if (members.isErr()) {
    return err(ObjectCheckerCheckObjectCallError{
        .code = ObjectCheckerCheckObjectCallErrorCode::NUMERIC_ENUM,
        .message = members.error().message,
    });
  }

  return ok(members.value());
}

ObjectChecker::ResultEnumMemberValues
ObjectChecker::checkEnumMembers(CallExpr *call, EnumDecl *enumDecl, GlobalMember listing,
                                llvm::StringRef functionName) {
  const auto *values = this->typeDeclarer->enumType(enumDecl);
  if (!enumDecl->isString && listing != GlobalMember::OBJECT_KEYS) {
    llvm::StringRef first =
        enumDecl->members.empty() ? kEnumMemberPlaceholder : enumDecl->members.front().name;
    auto message = ErrorTextObjectCall::numberEnum(functionName, enumDecl->name, first);

    return err(ObjectCheckerEnumMembersError{
        .code = ObjectCheckerEnumMembersErrorCode::NUMERIC_ENUM,
        .message = message,
    });
  }
  auto *list = this->state.getAst().make<ArrayLitExpr>(call->loc());
  for (const EnumMember &enumMember : enumDecl->members) {
    auto *element = this->enumListElement(call, enumDecl, enumMember, listing);
    list->elements.push_back(ArrayLitExpr::Element{.expr = element, .spread = false});
  }
  const auto *listType = this->dictMethods->objectCallType(listing, values);
  auto *checked = this->exprChecker->checkExpr(list, listType);
  this->state.getInventoryStore().rememberNamedUse(checked, {enumDecl, call->args[0]->loc(), ""});

  return ok(checked);
}

Expr *ObjectChecker::enumListElement(const CallExpr *call, const EnumDecl *enumDecl,
                                     const EnumMember &enumMember, GlobalMember listing) {
  auto text = ToUtf16::toUtf16(enumMember.name);
  auto *name = this->state.getAst().make<StringLitExpr>(call->loc(), text);
  auto *owner = this->state.getAst().make<NameExpr>(call->loc(), enumDecl->name);
  auto *member = this->state.getAst().make<MemberExpr>(call->loc(), owner, enumMember.name);
  if (listing == GlobalMember::OBJECT_KEYS) {
    return name;
  }
  if (listing != GlobalMember::OBJECT_ENTRIES) {
    return member;
  }
  auto *pair = this->state.getAst().make<ArrayLitExpr>(call->loc());
  pair->elements.push_back(ArrayLitExpr::Element{.expr = name, .spread = false});
  pair->elements.push_back(ArrayLitExpr::Element{.expr = member, .spread = false});

  return pair;
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
    auto *owner = this->state.getAst().make<NameExpr>(name->loc(), enumDecl->name);
    auto *value = this->state.getAst().make<MemberExpr>(name->loc(), owner, member.name);
    auto property = ObjectLitExpr::Property{
        .name = member.name,
        .loc = name->loc(),
        .value = value,
    };
    object->props.push_back(property);
  }
  auto *checked = this->exprChecker->checkExpr(object, expected);
  this->state.getInventoryStore().rememberNamedUse(checked, {enumDecl, name->loc(), ""});

  return checked;
}

ObjectChecker::ResultForInKeysAllowed ObjectChecker::checkWalkable(const Type *type,
                                                                   llvm::StringRef text) const {
  if (type->isArray() || type->isString() || type->isTuple()) {
    auto what = this->indexedKindText(type);
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

std::string ObjectChecker::indexedKindText(const Type *type) const {
  if (type->isArray()) {
    return ErrorTextForIn::anArray();
  }
  if (type->isString()) {
    return ErrorTextForIn::aString();
  }

  return ErrorTextForIn::aTuple();
}

void ObjectChecker::walkForIn(ForOfStmt *stmt) {
  auto *variable = dyn_cast<NameExpr>(stmt->iterable);
  bool isEnv = this->processChecker->isProcessMember(stmt->iterable, ProcessMember::ENV);
  auto *object = this->exprChecker->checkExpr(stmt->iterable);
  const Type *type = object->type;
  bool isRecordLoop = type->isRecord() && (variable != nullptr || isEnv) &&
                      !isa<VarDeclStmt>(stmt->body) && !isa<FunctionDecl>(stmt->body);
  if (isRecordLoop) {
    this->addKeyGuard(stmt, variable);
  }
  auto text = this->forInText(object, type);
  auto allowed = this->checkWalkable(type, text);
  if (allowed.isErr()) {
    this->state.getDiagnostics().error(object->loc(), allowed.error().message);
  }
  bool isWalking = allowed.isOk() && !type->isError();
  stmt->iterable = this->forInKeys(object, isWalking);
}

void ObjectChecker::addKeyGuard(ForOfStmt *stmt, const NameExpr *variable) {
  llvm::SMLoc loc = stmt->body->loc();
  auto *again = this->recordAgain(loc, variable);
  auto *key = this->state.getAst().make<NameExpr>(loc, stmt->binding.name);
  auto *has = this->state.getAst().make<BinaryExpr>(loc, BinaryOp::In, key, again);
  auto *missing = this->state.getAst().make<UnaryExpr>(loc, UnaryOp::Not, has);
  auto *next = this->state.getAst().make<ContinueStmt>(loc, "");
  auto *skip = this->state.getAst().make<IfStmt>(loc, missing, next, nullptr);
  auto *block = this->bodyBlock(stmt);
  block->body.insert(block->body.begin(), skip);
}

Expr *ObjectChecker::recordAgain(llvm::SMLoc loc, const NameExpr *variable) {
  if (variable != nullptr) {
    return this->state.getAst().make<NameExpr>(loc, variable->name);
  }
  auto processName = GlobalNameTable::getName(GlobalName::PROCESS);
  auto *process = this->state.getAst().make<NameExpr>(loc, processName);
  auto envName = ProcessMemberTable::getName(ProcessMember::ENV);

  return this->state.getAst().make<MemberExpr>(loc, process, envName);
}

BlockStmt *ObjectChecker::bodyBlock(ForOfStmt *stmt) {
  auto *block = dyn_cast<BlockStmt>(stmt->body);
  if (block != nullptr) {
    return block;
  }
  auto *wrapped = this->state.getAst().make<BlockStmt>(stmt->body->loc());
  wrapped->body.push_back(stmt->body);
  stmt->body = wrapped;

  return wrapped;
}

std::string ObjectChecker::forInText(const Expr *object, const Type *type) const {
  const Expr *unnarrowed = ObjectCheckHelpers::unnarrowed(object);
  auto *name = dyn_cast<NameExpr>(unnarrowed);
  if (name != nullptr) {
    return name->name.str();
  }

  return type->isString() ? kStringPlaceholder : kArrayPlaceholder;
}

Expr *ObjectChecker::forInKeys(Expr *object, bool isWalking) {
  llvm::SMLoc loc = object->loc();
  auto objectName = GlobalNameTable::getName(GlobalName::OBJECT);
  auto keysName = GlobalMemberTable::getName(GlobalMember::OBJECT_KEYS);
  auto *owner = this->state.getAst().make<NameExpr>(loc, objectName);
  auto *callee = this->state.getAst().make<MemberExpr>(loc, owner, keysName);
  auto *keys = this->state.getAst().make<CallExpr>(loc, callee, std::vector<Expr *>{object});
  keys->target = BuiltinCall{.builtin = Builtin::ObjectKeys};
  keys->type = isWalking ? Type::array(Type::string()) : Type::error();
  keys->forIn = true;

  return keys;
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
  if (!field->type->isOptional()) {
    return ok(member);
  }
  auto path = this->fieldFacts->fieldPath(member);
  if (!path.has_value() || !this->fieldFacts->isFieldDefined(*path)) {
    return ok(member);
  }
  auto *defined = this->typeRelations->convert(member, field->type->inner());

  return ok(defined);
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
    return this->checkFieldValueCall(call, type);
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

ObjectChecker::ResultFieldCallIsTyped ObjectChecker::checkFieldValueCall(CallExpr *call,
                                                                         const Type *type) {
  auto valued = this->callChecker->checkValueCall(call, type);
  if (valued.isErr()) {
    return err(ObjectCheckerCheckFieldCallError{
        .code = ObjectCheckerCheckFieldCallErrorCode::TAKES_THIS,
        .message = valued.error().message,
    });
  }

  return ok(valued.value());
}

ObjectChecker::ResultTargetObjectType
ObjectChecker::checkObjectDefined(const Type *type, const MemberExpr *member) const {
  if (type->isOptional()) {
    auto message = ErrorTextObjectField::setUndef(member->name);

    return err(ObjectCheckerObjectDefinedError{
        .code = ObjectCheckerObjectDefinedErrorCode::MAY_BE_UNDEFINED,
        .message = message,
    });
  }

  return ok(type);
}

ObjectChecker::ResultLengthIsNotFixed
ObjectChecker::checkFixedLength(const Type *type, const MemberExpr *member) const {
  if (type->isTuple() && BuiltinPropertyTable::isName(member->name, BuiltinProperty::LENGTH)) {
    auto message = ErrorTextObjectField::tupleLen();

    return err(ObjectCheckerFixedLengthError{
        .code = ObjectCheckerFixedLengthErrorCode::TUPLE_LENGTH,
        .message = message,
    });
  }

  return ok(type);
}

ObjectChecker::ResultTargetFieldFound
ObjectChecker::checkFieldExists(const Type *type, const MemberExpr *member) const {
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
ObjectChecker::checkFieldWritable(const Field *field, const Type *type,
                                  const MemberExpr *member) const {
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
  auto defined = this->checkObjectDefined(type, member);
  if (defined.isErr()) {
    this->state.getDiagnostics().error(member->object->loc(), defined.error().message);
    return Type::error();
  }
  auto length = this->checkFixedLength(type, member);
  if (length.isErr()) {
    this->state.getDiagnostics().error(member->loc(), length.error().message);
    return Type::error();
  }
  auto found = this->checkFieldExists(type, member);
  if (found.isErr()) {
    this->state.getDiagnostics().error(member->loc(), found.error().message);
    return Type::error();
  }
  const Field *field = found.value();
  auto writable = this->checkFieldWritable(field, type, member);
  if (writable.isErr()) {
    this->state.getDiagnostics().error(member->loc(), writable.error().message);
  }
  member->type = field->type;

  return field->type;
}

std::string ObjectChecker::objectMismatch(const Type *from, const Type *to, bool isConflict) const {
  for (const Field &field : from->fields()) {
    if (to->field(field.name) == nullptr) {
      return ErrorTextMismatch::extraField(field.name);
    }
  }
  for (const Field &target : to->fields()) {
    if (from->field(target.name) == nullptr) {
      return ErrorTextMismatch::noField(target.name);
    }
  }
  for (const Field &field : from->fields()) {
    const auto *target = to->field(field.name);
    if (target->type != field.type) {
      return this->fieldTypeMismatch(field, *target, to, isConflict);
    }
    if (field.readonly && !target->readonly) {
      return ErrorTextMismatch::readonly(field.name, to->name());
    }
  }

  return "";
}

std::string ObjectChecker::fieldTypeMismatch(const Field &field, const Field &target,
                                             const Type *to, bool isConflict) const {
  auto message =
      ErrorTextMismatch::fieldType(field.name, field.type->name(), target.type->name(), to->name());
  bool isNumberPair =
      field.type->nonOptional()->isNumeric() && target.type->nonOptional()->isNumeric();
  if (!isConflict && isNumberPair) {
    message += ErrorTextMismatch::typeHint(to->name());
  }

  return message;
}

}
