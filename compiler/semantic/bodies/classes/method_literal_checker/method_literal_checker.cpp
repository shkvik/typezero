#include "compiler/semantic/bodies/classes/method_literal_checker/method_literal_checker.h"
#include "compiler/semantic/bodies/classes/class_bases/class_bases.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/bodies/functions/function_checker/function_checker.h"
#include "compiler/semantic/declare/class_declarer/class_declarer.h"
#include "compiler/semantic/flow/predicate_checker/predicate_checker.h"
#include "compiler/semantic/infer/call_cycles/call_cycles.h"
#include "compiler/semantic/infer/return_inferrer/return_inferrer.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "llvm/ADT/STLExtras.h"

using llvm::cast;
using llvm::dyn_cast;

namespace tz {

MethodLiteralChecker::MethodLiteralChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->callCycles = &all.callCycles;
  this->classBases = &all.classBases;
  this->exprChecker = &all.exprChecker;
  this->classDeclarer = &all.classDeclarer;
  this->typeRelations = &all.typeRelations;
  this->returnInferrer = &all.returnInferrer;
  this->functionChecker = &all.functionChecker;
  this->predicateChecker = &all.predicateChecker;
}

Expr *MethodLiteralChecker::checkMethodLiteral(ObjectLitExpr *literal, const Type *target) {
  literal->type = Type::error();
  if (target != nullptr && target->isAnyObject()) {
    target = nullptr;
  }
  auto spreadless = this->noSpread(literal);
  if (spreadless.isErr()) {
    this->state.getDiagnostics().error(literal->loc(), spreadless.error().message);
    for (ObjectLitExpr::Property &property : literal->props) {
      property.value = this->exprChecker->checkExpr(property.value);
    }
    return literal;
  }
  auto *type = this->makeLiteralClass(literal);
  ClassDecl *classDecl = literal->cls;
  bool hasFieldsTyped = true;
  auto fields = std::vector<Field>();
  for (ObjectLitExpr::Property &property : literal->props) {
    if (property.method()) {
      this->addLiteralMethod(classDecl, property);
      continue;
    }
    hasFieldsTyped &= this->addLiteralField(classDecl, property, target, fields);
  }
  if (!hasFieldsTyped) {
    return literal;
  }
  type->defineFields(fields);
  type->rename(this->literalTypeName(fields, classDecl->methods));
  this->declareLiteralMethods(classDecl, target);
  this->walkLiteralMethods(classDecl);
  literal->type = type;
  if (target == nullptr) {
    return literal;
  }

  return this->matchLiteralTarget(literal, target, type);
}

Type *MethodLiteralChecker::makeLiteralClass(ObjectLitExpr *literal) {
  auto *classDecl = this->state.getAst().make<ClassDecl>(literal->loc(), "");
  classDecl->syntax = LiteralClass{.literal = literal};
  classDecl->id = this->state.getClassStore().takeClassId();
  literal->cls = classDecl;
  auto *type = Type::classType("", classDecl);
  classDecl->type = type;
  classDecl->stage = ClassStage::RESOLVED;
  classDecl->declared = true;
  this->state.getClassStore().addClass(classDecl);

  return type;
}

void MethodLiteralChecker::addLiteralMethod(ClassDecl *classDecl,
                                            const ObjectLitExpr::Property &property) {
  FunctionDecl *functionDecl = cast<FunctionExpr>(property.value)->fn;
  functionDecl->name = property.name;
  ClassMember *member = functionDecl->member();
  if (member != nullptr) {
    member->cls = classDecl;
  } else {
    functionDecl->role = ClassMember{.cls = classDecl};
  }
  auto simple = this->plainMethod(functionDecl);
  if (simple.isErr()) {
    this->state.getDiagnostics().error(functionDecl->loc(), simple.error().message);
    functionDecl->typeParams.clear();
  }
  classDecl->methods.push_back(functionDecl);
}

bool MethodLiteralChecker::addLiteralField(ClassDecl *classDecl, ObjectLitExpr::Property &property,
                                           const Type *target, std::vector<Field> &fields) {
  auto *functionExpr = dyn_cast<FunctionExpr>(property.value);
  if (functionExpr != nullptr && functionExpr->fn->name.empty()) {
    functionExpr->fn->name = property.name;
  }
  auto what = ErrorTextFieldValue::label(property.name);
  const Field *wanted = target != nullptr ? target->field(property.name) : nullptr;
  property.value =
      this->exprChecker->checkExpr(property.value, wanted != nullptr ? wanted->type : nullptr);
  if (wanted != nullptr) {
    property.value = this->typeRelations->coerce(property.value, wanted->type, what);
  }
  const Type *valueType =
      wanted != nullptr && !property.value->type->isError() ? wanted->type : property.value->type;
  auto typed = this->fieldValue(valueType, what);
  if (typed.isErr()) {
    this->state.getDiagnostics().error(property.value->loc(), typed.error().message);
  }
  ClassField field;
  field.name = property.name;
  field.loc = property.loc;
  field.resolved = valueType;
  classDecl->fields.push_back(field);
  fields.push_back({property.name.str(), valueType, false});

  return typed.isOk() && !valueType->isError();
}

std::string MethodLiteralChecker::literalTypeName(const std::vector<Field> &fields,
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

void MethodLiteralChecker::declareLiteralMethods(ClassDecl *classDecl, const Type *target) {
  auto *group = this->state.getAst().newGroup();
  group->owner = this->state.getFunctionStack().getCurrentDecl();
  group->scopeDepth = this->state.getScopeStack().getScopeDepth();
  group->functionDepth = this->state.getFunctionStack().getFunctionDepth();
  group->creationSeq = this->state.getReturnInference().takeSequenceNumber();
  group->finished = true;
  group->members = classDecl->methods;
  classDecl->group = group;
  for (FunctionDecl *functionDecl : classDecl->methods) {
    functionDecl->group = group;
    functionDecl->parent = this->state.getFunctionStack().getCurrentDecl();
    functionDecl->creationSeq = group->creationSeq;
    this->declareLiteralMethod(functionDecl, target);
  }
}

void MethodLiteralChecker::declareLiteralMethod(FunctionDecl *functionDecl, const Type *target) {
  const Field *wanted = target != nullptr ? target->field(functionDecl->name) : nullptr;
  if (wanted == nullptr || !wanted->type->isFunction()) {
    return this->functionChecker->declareSignature(functionDecl);
  }
  this->classDeclarer->declareTyped(functionDecl, wanted->type);
  if (functionDecl->returnType.has_value()) {
    functionDecl->sym->setReturnType(
        this->predicateChecker->resolveReturnType(functionDecl, functionDecl->sym));
  }
}

void MethodLiteralChecker::walkLiteralMethods(ClassDecl *classDecl) {
  this->callCycles->findMethodCycles(classDecl->methods);
  for (FunctionDecl *functionDecl : classDecl->methods) {
    if (!functionDecl->sym->hasReturnType()) {
      this->returnInferrer->inferReturnType(functionDecl->sym);
    }
    if (functionDecl->checked) {
      continue;
    }
    auto saved = this->returnInferrer->enterBody(functionDecl);
    this->functionChecker->walkFunctionBody(functionDecl);
    this->returnInferrer->leaveBody(saved);
  }
}

Expr *MethodLiteralChecker::matchLiteralTarget(ObjectLitExpr *literal, const Type *target,
                                               const Type *type) {
  for (const ObjectLitExpr::Property &property : literal->props) {
    auto fits = this->propertyFits(property, target);
    if (fits.isErr()) {
      this->state.getDiagnostics().error(property.loc, fits.error().message);
      literal->type = Type::error();
      return literal;
    }
  }
  auto matched = this->membersMatch(literal, target);
  if (matched.isErr()) {
    this->state.getDiagnostics().error(literal->loc(), matched.error().message);
    literal->type = Type::error();
    return literal;
  }
  this->classBases->addBase(type, target);

  return literal;
}

MethodLiteralChecker::ResultSpreadAbsent
MethodLiteralChecker::noSpread(const ObjectLitExpr *literal) const {
  if (literal->hasSpread()) {
    auto message = ErrorTextLiteralSpread::spread();

    return err(MethodLiteralCheckerNoSpreadError{
        .code = MethodLiteralCheckerNoSpreadErrorCode::SPREAD_IN_LITERAL,
        .message = message,
    });
  }

  return ok(literal);
}

MethodLiteralChecker::ResultMethodSimple
MethodLiteralChecker::plainMethod(const FunctionDecl *functionDecl) const {
  if (!functionDecl->typeParams.empty()) {
    auto message = ErrorTextPlainMethod::generic();

    return err(MethodLiteralCheckerPlainMethodError{
        .code = MethodLiteralCheckerPlainMethodErrorCode::GENERIC_METHOD,
        .message = message,
    });
  }

  return ok(functionDecl);
}

MethodLiteralChecker::ResultValueIsTyped
MethodLiteralChecker::fieldValue(const Type *valueType, llvm::StringRef label) const {
  if (valueType->isVoid()) {
    auto message = ErrorTextFieldValue::isVoid(label);

    return err(MethodLiteralCheckerFieldValueError{
        .code = MethodLiteralCheckerFieldValueErrorCode::VOID_VALUE,
        .message = message,
    });
  }
  if (valueType->isUndefined()) {
    auto message = ErrorTextFieldValue::undefined(label);

    return err(MethodLiteralCheckerFieldValueError{
        .code = MethodLiteralCheckerFieldValueErrorCode::UNDEFINED_VALUE,
        .message = message,
    });
  }

  return ok(valueType);
}

MethodLiteralChecker::ResultPropertyFits
MethodLiteralChecker::propertyFits(const ObjectLitExpr::Property &property,
                                   const Type *target) const {
  const auto *member = target->field(property.name);
  if (member == nullptr) {
    auto message = ErrorTextLiteralFits::noField(target->name(), property.name);

    return err(MethodLiteralCheckerPropertyFitsError{
        .code = MethodLiteralCheckerPropertyFitsErrorCode::NO_SUCH_FIELD,
        .message = message,
    });
  }
  if (!property.method()) {
    return ok(&property);
  }
  if (!member->type->isFunction()) {
    auto message =
        ErrorTextLiteralFits::notMethod(target->name(), property.name, member->type->name());

    return err(MethodLiteralCheckerPropertyFitsError{
        .code = MethodLiteralCheckerPropertyFitsErrorCode::NOT_A_METHOD,
        .message = message,
    });
  }
  if (!member->readonly) {
    auto message = ErrorTextLiteralFits::replaced(target->name(), property.name);

    return err(MethodLiteralCheckerPropertyFitsError{
        .code = MethodLiteralCheckerPropertyFitsErrorCode::REPLACEABLE_FIELD,
        .message = message,
    });
  }

  FunctionDecl *functionDecl = cast<FunctionExpr>(property.value)->fn;
  const auto *type = this->returnInferrer->functionType(functionDecl->sym);
  if (type != member->type && !type->isError()) {
    auto message = ErrorTextLiteralFits::otherType(target->name(), property.name, type->name(),
                                                   member->type->name());

    return err(MethodLiteralCheckerPropertyFitsError{
        .code = MethodLiteralCheckerPropertyFitsErrorCode::METHOD_TYPE_DIFFERS,
        .message = message,
    });
  }

  return ok(&property);
}

MethodLiteralChecker::ResultMembersMatch
MethodLiteralChecker::membersMatch(const ObjectLitExpr *literal, const Type *target) const {
  const ClassDecl *classDecl = literal->cls;
  for (const Field &member : target->fields()) {
    auto named = [&](const FunctionDecl *functionDecl) {
      return functionDecl->name == member.name;
    };
    if (classDecl->field(member.name) == nullptr && !llvm::any_of(classDecl->methods, named)) {
      auto message = ErrorTextLiteralMembers::missing(target->name(), member.name);

      return err(MethodLiteralCheckerMembersMatchError{
          .code = MethodLiteralCheckerMembersMatchErrorCode::MISSING_MEMBER,
          .message = message,
      });
    }
  }
  if (!target->overloads().empty()) {
    auto message = ErrorTextLiteralMembers::overloads(target->name(), target->overloads()[0].name);

    return err(MethodLiteralCheckerMembersMatchError{
        .code = MethodLiteralCheckerMembersMatchErrorCode::OVERLOADED_METHOD,
        .message = message,
    });
  }

  return ok(literal);
}

}
