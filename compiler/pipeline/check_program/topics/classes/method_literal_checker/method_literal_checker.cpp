#include "compiler/pipeline/check_program/topics/classes/method_literal_checker/method_literal_checker.h"
#include "compiler/pipeline/check_program/declare/class_declarer/class_declarer.h"
#include "compiler/pipeline/check_program/flow/predicate_checker/predicate_checker.h"
#include "compiler/pipeline/check_program/infer/call_cycles/call_cycles.h"
#include "compiler/pipeline/check_program/infer/return_inferrer/return_inferrer.h"
#include "compiler/pipeline/check_program/topics/classes/class_bases/class_bases.h"
#include "compiler/pipeline/check_program/topics/expr/expr_checker/expr_checker.h"
#include "compiler/pipeline/check_program/topics/functions/function_checker/function_checker.h"
#include "compiler/pipeline/check_program/types/type_relations/type_relations.h"
#include "compiler/pipeline/check_program/whole_program/flows/flows.h"
#include "llvm/ADT/STLExtras.h"

using llvm::cast;
using llvm::dyn_cast;

namespace tz {

MethodLiteralChecker::MethodLiteralChecker(CheckState &owner)
    : state(owner) {}

Expr *MethodLiteralChecker::checkMethodLiteral(ObjectLitExpr *lit, const Type *target) {
  lit->type = Type::error();
  if (target && target->isAnyObject()) {
    target = nullptr;
  }
  ResultSpreadAbsent spreadless = MethodLiteralChecker::noSpread(lit);
  if (spreadless.isErr()) {
    this->state.getDiagnostics().error(lit->loc(), spreadless.error().message);
    for (ObjectLitExpr::Property &prop : lit->props) {
      prop.value = ExprChecker(this->state).checkExpr(prop.value);
    }
    return lit;
  }
  auto *cls = this->state.getAst().make<ClassDecl>(lit->loc(), "");
  cls->syntax = LiteralClass{.literal = lit};
  cls->id = this->state.getClassStore().takeClassId();
  lit->cls = cls;
  Type *type = Type::classType("", cls);
  cls->type = type;
  cls->stage = ClassStage::RESOLVED;
  cls->declared = true;
  this->state.getClassStore().addClass(cls);

  bool fieldsTyped = true;
  std::vector<Field> fields;
  for (ObjectLitExpr::Property &prop : lit->props) {
    if (prop.method()) {
      FunctionDecl *fn = cast<FunctionExpr>(prop.value)->fn;
      fn->name = prop.name;
      if (ClassMember *member = fn->member()) {
        member->cls = cls;
      } else {
        fn->role = ClassMember{.cls = cls};
      }
      ResultMethodSimple simple = MethodLiteralChecker::plainMethod(fn);
      if (simple.isErr()) {
        this->state.getDiagnostics().error(fn->loc(), simple.error().message);
        fn->typeParams.clear();
      }
      cls->methods.push_back(fn);
      continue;
    }
    if (auto *fe = dyn_cast<FunctionExpr>(prop.value); fe && fe->fn->name.empty()) {
      fe->fn->name = prop.name;
    }
    std::string what = ErrorTextFieldValue::label(prop.name);
    const Field *wanted = target ? target->field(prop.name) : nullptr;
    prop.value = ExprChecker(this->state).checkExpr(prop.value, wanted ? wanted->type : nullptr);
    if (wanted) {
      prop.value = TypeRelations(this->state).coerce(prop.value, wanted->type, what);
    }
    const Type *valueType =
        wanted && !prop.value->type->isError() ? wanted->type : prop.value->type;
    ResultValueIsTyped typed = MethodLiteralChecker::fieldValue(valueType, what);
    if (typed.isErr()) {
      this->state.getDiagnostics().error(prop.value->loc(), typed.error().message);
      fieldsTyped = false;
    }
    fieldsTyped &= !valueType->isError();
    ClassField field;
    field.name = prop.name;
    field.loc = prop.loc;
    field.resolved = valueType;
    cls->fields.push_back(field);
    fields.push_back({prop.name.str(), valueType, false});
  }
  if (!fieldsTyped) {
    return lit;
  }
  type->defineFields(fields);
  std::string name = objectTypeSpelling(fields);
  std::string methods;
  for (FunctionDecl *fn : cls->methods) {
    methods += (methods.empty() ? "" : "; ") + fn->name.str() + "()";
  }
  name = fields.empty() ? "{ " + methods + " }"
                        : name.substr(0, name.size() - 2) + "; " + methods + " }";
  type->rename(name);

  NestedGroup *group = this->state.getAst().newGroup();
  group->owner = this->state.getFunctionStack().getCurrentDecl();
  group->scopeDepth = this->state.getScopeStack().getScopeDepth();
  group->functionDepth = this->state.getFunctionStack().getFunctionDepth();
  group->creationSeq = this->state.getReturnInference().takeSequenceNumber();
  group->finished = true;
  group->members = cls->methods;
  cls->group = group;
  for (FunctionDecl *fn : cls->methods) {
    fn->group = group;
    fn->parent = this->state.getFunctionStack().getCurrentDecl();
    fn->creationSeq = group->creationSeq;
    const Field *wanted = target ? target->field(fn->name) : nullptr;
    if (wanted && wanted->type->isFunction()) {
      ClassDeclarer(this->state).declareTyped(fn, wanted->type);
      if (fn->returnType) {
        fn->sym->ret = PredicateChecker(this->state).resolveReturnType(fn, fn->sym);
      }
    } else {
      FunctionChecker(this->state).declareFunction(fn, nullptr, false);
    }
  }
  CallCycles(this->state)
      .findCycles(cls->methods,
                  [](FunctionDecl *, llvm::StringRef) -> FunctionDecl * { return nullptr; });
  for (FunctionDecl *fn : cls->methods) {
    if (!fn->sym->ret) {
      ReturnInferrer(this->state).inferReturnType(fn->sym);
    }
    if (fn->checked) {
      continue;
    }
    BodyState saved = ReturnInferrer(this->state).enterBody(fn);
    FunctionChecker(this->state).checkFunctionBody(fn);
    ReturnInferrer(this->state).leaveBody(saved);
  }
  lit->type = type;
  if (!target) {
    return lit;
  }
  for (const ObjectLitExpr::Property &prop : lit->props) {
    ResultPropertyFits fits = this->propertyFits(prop, target);
    if (fits.isErr()) {
      this->state.getDiagnostics().error(prop.loc, fits.error().message);
      lit->type = Type::error();
      return lit;
    }
  }
  ResultMembersMatch matched = MethodLiteralChecker::membersMatch(lit, target);
  if (matched.isErr()) {
    this->state.getDiagnostics().error(lit->loc(), matched.error().message);
    lit->type = Type::error();
    return lit;
  }
  ClassBases(this->state).addBase(type, target);

  return lit;
}

MethodLiteralChecker::ResultSpreadAbsent MethodLiteralChecker::noSpread(const ObjectLitExpr *lit) {
  if (lit->hasSpread()) {
    std::string message = ErrorTextLiteralSpread::spread();

    return err(MethodLiteralCheckerNoSpreadError{
        .code = MethodLiteralCheckerNoSpreadErrorCode::SPREAD_IN_LITERAL,
        .message = message,
    });
  }

  return ok(lit);
}

MethodLiteralChecker::ResultMethodSimple MethodLiteralChecker::plainMethod(const FunctionDecl *fn) {
  if (!fn->typeParams.empty()) {
    std::string message = ErrorTextPlainMethod::generic();

    return err(MethodLiteralCheckerPlainMethodError{
        .code = MethodLiteralCheckerPlainMethodErrorCode::GENERIC_METHOD,
        .message = message,
    });
  }

  return ok(fn);
}

MethodLiteralChecker::ResultValueIsTyped MethodLiteralChecker::fieldValue(const Type *valueType,
                                                                          llvm::StringRef label) {
  if (valueType->isVoid()) {
    std::string message = ErrorTextFieldValue::isVoid(label);

    return err(MethodLiteralCheckerFieldValueError{
        .code = MethodLiteralCheckerFieldValueErrorCode::VOID_VALUE,
        .message = message,
    });
  }
  if (valueType->isUndefined()) {
    std::string message = ErrorTextFieldValue::undefined(label);

    return err(MethodLiteralCheckerFieldValueError{
        .code = MethodLiteralCheckerFieldValueErrorCode::UNDEFINED_VALUE,
        .message = message,
    });
  }

  return ok(valueType);
}

MethodLiteralChecker::ResultPropertyFits
MethodLiteralChecker::propertyFits(const ObjectLitExpr::Property &prop, const Type *target) const {
  const Field *member = target->field(prop.name);
  if (!member) {
    std::string message = ErrorTextLiteralFits::noField(target->name(), prop.name);

    return err(MethodLiteralCheckerPropertyFitsError{
        .code = MethodLiteralCheckerPropertyFitsErrorCode::NO_SUCH_FIELD,
        .message = message,
    });
  }
  if (!prop.method()) {
    return ok(&prop);
  }
  if (!member->type->isFunction()) {
    std::string message =
        ErrorTextLiteralFits::notMethod(target->name(), prop.name, member->type->name());

    return err(MethodLiteralCheckerPropertyFitsError{
        .code = MethodLiteralCheckerPropertyFitsErrorCode::NOT_A_METHOD,
        .message = message,
    });
  }
  if (!member->readonly) {
    std::string message = ErrorTextLiteralFits::replaced(target->name(), prop.name);

    return err(MethodLiteralCheckerPropertyFitsError{
        .code = MethodLiteralCheckerPropertyFitsErrorCode::REPLACEABLE_FIELD,
        .message = message,
    });
  }

  FunctionDecl *fn = cast<FunctionExpr>(prop.value)->fn;
  const Type *type = ReturnInferrer(this->state).functionType(fn->sym);
  if (type != member->type && !type->isError()) {
    std::string message = ErrorTextLiteralFits::otherType(target->name(), prop.name, type->name(),
                                                          member->type->name());

    return err(MethodLiteralCheckerPropertyFitsError{
        .code = MethodLiteralCheckerPropertyFitsErrorCode::METHOD_TYPE_DIFFERS,
        .message = message,
    });
  }

  return ok(&prop);
}

MethodLiteralChecker::ResultMembersMatch
MethodLiteralChecker::membersMatch(const ObjectLitExpr *lit, const Type *target) {
  const ClassDecl *cls = lit->cls;
  for (const Field &member : target->fields()) {
    auto named = [&](const FunctionDecl *fn) { return fn->name == member.name; };
    if (!cls->field(member.name) && !llvm::any_of(cls->methods, named)) {
      std::string message = ErrorTextLiteralMembers::missing(target->name(), member.name);

      return err(MethodLiteralCheckerMembersMatchError{
          .code = MethodLiteralCheckerMembersMatchErrorCode::MISSING_MEMBER,
          .message = message,
      });
    }
  }
  if (!target->overloads().empty()) {
    std::string message =
        ErrorTextLiteralMembers::overloads(target->name(), target->overloads()[0].name);

    return err(MethodLiteralCheckerMembersMatchError{
        .code = MethodLiteralCheckerMembersMatchErrorCode::OVERLOADED_METHOD,
        .message = message,
    });
  }

  return ok(lit);
}

}
