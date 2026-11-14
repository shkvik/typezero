#include "compiler/semantic/code/literals/method_literals/method_literals.h"
#include "llvm/ADT/STLExtras.h"

using llvm::cast;
using llvm::dyn_cast;

namespace tz {

MethodLiterals::MethodLiterals(SemanticState &owner)
    : state(owner) {}

void MethodLiterals::nextLiteralMember(const NextLiteralMember &task) {
  auto draft = task.draft;
  ObjectLitExpr *literal = draft.literal;
  ClassDecl *classDecl = literal->cls;
  while (draft.index < literal->props.size() && literal->props[draft.index].method()) {
    this->addLiteralMethod(classDecl, literal->props[draft.index]);
    draft.index += 1;
  }
  if (draft.index == literal->props.size()) {
    return this->afterLiteralMembers(std::move(draft));
  }
  ObjectLitExpr::Property &property = literal->props[draft.index];
  auto *functionExpr = dyn_cast<FunctionExpr>(property.value);
  if (functionExpr != nullptr && functionExpr->fn->name.empty()) {
    functionExpr->fn->name = property.name;
  }
  const Field *wanted = draft.wanted != nullptr ? draft.wanted->field(property.name) : nullptr;
  const Type *hint = wanted != nullptr ? wanted->type : nullptr;
  auto what = ErrorTextFieldValue::label(property.name);
  this->state.walk.tasks.pushTask(AfterLiteralField{.draft = std::move(draft)});
  if (wanted != nullptr) {
    this->state.walk.tasks.pushTask(
        CoerceSlot{.slot = &property.value, .to = wanted->type, .what = what});
  }

  return this->state.walk.tasks.pushTask(CheckExprSlot{.slot = &property.value, .expected = hint});
}

void MethodLiterals::afterLiteralField(const AfterLiteralField &task) {
  auto draft = task.draft;
  ObjectLitExpr::Property &property = draft.literal->props[draft.index];
  draft.index += 1;
  const Field *wanted = draft.wanted != nullptr ? draft.wanted->field(property.name) : nullptr;
  auto what = ErrorTextFieldValue::label(property.name);
  const Type *valueType =
      wanted != nullptr && !property.value->type->isError() ? wanted->type : property.value->type;
  auto typed = this->checkFieldValue(valueType, what);
  if (typed.isErr()) {
    this->state.diagnostics.error(property.value->loc(), typed.error().message);
  }
  auto field = ClassField{
      .name = property.name,
      .loc = property.loc,
      .resolved = valueType,
  };
  draft.literal->cls->fields.push_back(field);
  draft.fields.push_back({property.name.str(), valueType, false});
  draft.hasFieldsTyped = draft.hasFieldsTyped && typed.isOk() && !valueType->isError();

  return this->nextLiteralMember(NextLiteralMember{.draft = std::move(draft)});
}

void MethodLiterals::afterLiteralMembers(MethodLiteralDraft draft) {
  ObjectLitExpr *literal = draft.literal;
  ClassDecl *classDecl = literal->cls;
  auto &tasks = this->state.walk.tasks;
  if (!draft.hasFieldsTyped) {
    return tasks.pushChecked(literal);
  }
  draft.type->defineFields(draft.fields);
  auto typeName = this->literalTypeName(draft.fields, classDecl->methods);
  draft.type->rename(typeName);
  tasks.pushTask(
      AfterLiteralMethods{.literal = literal, .wanted = draft.wanted, .type = draft.type});
  tasks.pushTask(WalkLiteralMethods{.classDecl = classDecl});

  return this->declareLiteralMethods(classDecl, draft.wanted);
}

void MethodLiterals::declareLiteralMethods(ClassDecl *classDecl, const Type *target) {
  auto *group = this->state.ast.newGroup();
  group->owner = this->state.walk.functions.getCurrentDecl();
  group->scopeDepth = this->state.walk.scopes.getScopeDepth();
  group->functionDepth = this->state.walk.functions.getFunctionDepth();
  group->creationSeq = this->state.declaration.order.takeSequenceNumber();
  group->finished = true;
  group->members = classDecl->methods;
  classDecl->group = group;
  for (FunctionDecl *functionDecl : llvm::reverse(classDecl->methods)) {
    functionDecl->group = group;
    functionDecl->parent = this->state.walk.functions.getCurrentDecl();
    functionDecl->creationSeq = group->creationSeq;
    this->pushLiteralMethodDeclaration(functionDecl, target);
  }
}

void MethodLiterals::pushLiteralMethodDeclaration(FunctionDecl *functionDecl, const Type *target) {
  const Field *wanted = target != nullptr ? target->field(functionDecl->name) : nullptr;
  auto &tasks = this->state.walk.tasks;
  if (wanted == nullptr || !wanted->type->isFunction()) {
    return tasks.pushTask(
        StartFunctionSymbol{.functionDecl = functionDecl, .variable = nullptr, .isNamed = false});
  }
  if (functionDecl->returnType.has_value()) {
    tasks.pushTask(DeclareMethodReturn{.functionDecl = functionDecl});
  }

  return tasks.pushTask(StartTypedMethod{.functionDecl = functionDecl, .declared = wanted->type});
}

void MethodLiterals::walkLiteralMethods(const WalkLiteralMethods &task) {
  ClassDecl *classDecl = task.classDecl;
  auto &tasks = this->state.walk.tasks;
  for (FunctionDecl *functionDecl : llvm::reverse(classDecl->methods)) {
    tasks.pushTask(WalkLiteralMethod{.functionDecl = functionDecl});
  }

  return tasks.pushTask(FindMethodCycles{.methods = classDecl->methods});
}

void MethodLiterals::walkLiteralMethod(const WalkLiteralMethod &task) {
  FunctionDecl *functionDecl = task.functionDecl;
  this->state.walk.tasks.pushTask(AfterLiteralMethodReturn{.functionDecl = functionDecl});
  if (!functionDecl->sym->hasReturnType()) {
    return this->state.walk.tasks.pushTask(InferReturnType{.function = functionDecl->sym});
  }
}

void MethodLiterals::afterLiteralMethodReturn(const AfterLiteralMethodReturn &task) {
  if (!task.functionDecl->checked) {
    return this->state.walk.tasks.pushTask(WalkInferredBody{.functionDecl = task.functionDecl});
  }
}

void MethodLiterals::afterLiteralMethods(const AfterLiteralMethods &task) {
  ObjectLitExpr *literal = task.literal;
  literal->type = task.type;
  if (task.wanted == nullptr) {
    return this->state.walk.tasks.pushChecked(literal);
  }
  this->state.walk.tasks.pushTask(
      MatchLiteralTarget{.literal = literal, .wanted = task.wanted, .type = task.type});
  for (const ObjectLitExpr::Property &property : llvm::reverse(literal->props)) {
    const Field *member = task.wanted->field(property.name);
    bool isTypedMethod =
        member != nullptr && property.method() && member->type->isFunction() && member->readonly;
    if (isTypedMethod) {
      FunctionDecl *functionDecl = cast<FunctionExpr>(property.value)->fn;
      this->state.walk.tasks.pushTask(FunctionTypeOf{.function = functionDecl->sym});
    }
  }
}

void MethodLiterals::matchLiteralTarget(const MatchLiteralTarget &task) {
  ObjectLitExpr *literal = task.literal;
  const Type *target = task.wanted;
  auto methodTypes = std::vector<const Type *>();
  for (const ObjectLitExpr::Property &property : literal->props) {
    const Field *member = target->field(property.name);
    bool isTypedMethod =
        member != nullptr && property.method() && member->type->isFunction() && member->readonly;
    if (isTypedMethod) {
      methodTypes.push_back(this->state.walk.tasks.takeAnswer());
    }
  }
  std::reverse(methodTypes.begin(), methodTypes.end());
  this->state.walk.tasks.pushChecked(literal);
  size_t next = 0;
  for (const ObjectLitExpr::Property &property : literal->props) {
    const Field *member = target->field(property.name);
    bool isTypedMethod =
        member != nullptr && property.method() && member->type->isFunction() && member->readonly;
    const Type *methodType = isTypedMethod ? methodTypes[next++] : nullptr;
    auto fits = this->checkPropertyFits(property, target, methodType);
    if (fits.isErr()) {
      this->state.diagnostics.error(property.loc, fits.error().message);
      literal->type = Type::error();
      return;
    }
  }
  auto matched = this->checkMembersMatch(literal, target);
  if (matched.isErr()) {
    this->state.diagnostics.error(literal->loc(), matched.error().message);
    literal->type = Type::error();
    return;
  }

  return this->state.walk.tasks.pushTask(AddBase{.type = task.type, .base = target});
}

void MethodLiterals::addLiteralMethod(ClassDecl *classDecl,
                                      const ObjectLitExpr::Property &property) {
  FunctionDecl *functionDecl = cast<FunctionExpr>(property.value)->fn;
  functionDecl->name = property.name;
  ClassMember *member = functionDecl->member();
  if (member != nullptr) {
    member->cls = classDecl;
  } else {
    functionDecl->role = ClassMember{.cls = classDecl};
  }
  auto simple = this->checkPlainMethod(functionDecl);
  if (simple.isErr()) {
    this->state.diagnostics.error(functionDecl->loc(), simple.error().message);
    functionDecl->typeParams.clear();
  }
  classDecl->methods.push_back(functionDecl);
}

std::string MethodLiterals::literalTypeName(const std::vector<Field> &fields,
                                            llvm::ArrayRef<FunctionDecl *> methods) const {
  auto name = TypeSpelling::objectTypeSpelling(fields);
  auto calls = std::string();
  for (const FunctionDecl *functionDecl : methods) {
    calls += (calls.empty() ? "" : "; ") + functionDecl->name.str() + "()";
  }
  if (fields.empty()) {
    return "{ " + calls + " }";
  }

  return name.substr(0, name.size() - 2) + "; " + calls + " }";
}

MethodLiterals::ResultMethodSimple
MethodLiterals::checkPlainMethod(const FunctionDecl *functionDecl) const {
  if (!functionDecl->typeParams.empty()) {
    auto message = ErrorTextPlainMethod::generic();

    return err(CodeLiteralsRegistryPlainMethodError{
        .code = CodeLiteralsRegistryPlainMethodErrorCode::GENERIC_METHOD,
        .message = message,
    });
  }

  return ok(functionDecl);
}

MethodLiterals::ResultValueIsTyped MethodLiterals::checkFieldValue(const Type *valueType,
                                                                   llvm::StringRef label) const {
  if (valueType->isVoid()) {
    auto message = ErrorTextFieldValue::isVoid(label);

    return err(CodeLiteralsRegistryFieldValueError{
        .code = CodeLiteralsRegistryFieldValueErrorCode::VOID_VALUE,
        .message = message,
    });
  }
  if (valueType->isUndefined()) {
    auto message = ErrorTextFieldValue::undefined(label);

    return err(CodeLiteralsRegistryFieldValueError{
        .code = CodeLiteralsRegistryFieldValueErrorCode::UNDEFINED_VALUE,
        .message = message,
    });
  }

  return ok(valueType);
}

MethodLiterals::ResultPropertyFits
MethodLiterals::checkPropertyFits(const ObjectLitExpr::Property &property, const Type *target,
                                  const Type *methodType) const {
  const auto *member = target->field(property.name);
  if (member == nullptr) {
    auto message = ErrorTextLiteralFits::noField(target->name(), property.name);

    return err(CodeLiteralsRegistryPropertyFitsError{
        .code = CodeLiteralsRegistryPropertyFitsErrorCode::NO_SUCH_FIELD,
        .message = message,
    });
  }
  if (!property.method()) {
    return ok(&property);
  }
  if (!member->type->isFunction()) {
    auto message =
        ErrorTextLiteralFits::notMethod(target->name(), property.name, member->type->name());

    return err(CodeLiteralsRegistryPropertyFitsError{
        .code = CodeLiteralsRegistryPropertyFitsErrorCode::NOT_A_METHOD,
        .message = message,
    });
  }
  if (!member->readonly) {
    auto message = ErrorTextLiteralFits::replaced(target->name(), property.name);

    return err(CodeLiteralsRegistryPropertyFitsError{
        .code = CodeLiteralsRegistryPropertyFitsErrorCode::REPLACEABLE_FIELD,
        .message = message,
    });
  }

  const Type *type = methodType;
  if (type != member->type && !type->isError()) {
    auto message = ErrorTextLiteralFits::otherType(target->name(), property.name, type->name(),
                                                   member->type->name());

    return err(CodeLiteralsRegistryPropertyFitsError{
        .code = CodeLiteralsRegistryPropertyFitsErrorCode::METHOD_TYPE_DIFFERS,
        .message = message,
    });
  }

  return ok(&property);
}

MethodLiterals::ResultMembersMatch MethodLiterals::checkMembersMatch(const ObjectLitExpr *literal,
                                                                     const Type *target) const {
  const ClassDecl *classDecl = literal->cls;
  for (const Field &member : target->fields()) {
    bool isGiven =
        classDecl->field(member.name) != nullptr || this->hasMethodNamed(classDecl, member.name);
    if (!isGiven) {
      auto message = ErrorTextLiteralMembers::missing(target->name(), member.name);

      return err(CodeLiteralsRegistryMembersMatchError{
          .code = CodeLiteralsRegistryMembersMatchErrorCode::MISSING_MEMBER,
          .message = message,
      });
    }
  }
  if (!target->overloads().empty()) {
    auto message = ErrorTextLiteralMembers::overloads(target->name(), target->overloads()[0].name);

    return err(CodeLiteralsRegistryMembersMatchError{
        .code = CodeLiteralsRegistryMembersMatchErrorCode::OVERLOADED_METHOD,
        .message = message,
    });
  }

  return ok(literal);
}

bool MethodLiterals::hasMethodNamed(const ClassDecl *classDecl, llvm::StringRef name) const {
  for (const FunctionDecl *functionDecl : classDecl->methods) {
    if (functionDecl->name == name) {
      return true;
    }
  }

  return false;
}

}
