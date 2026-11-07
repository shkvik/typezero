#include "compiler/check/classes/method_literal_checker/method_literal_checker.h"
#include "compiler/check/classes/class_bases/class_bases.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/flow/predicate_checker/predicate_checker.h"
#include "compiler/check/functions/function_checker/function_checker.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/declare/class_declarer/class_declarer.h"
#include "compiler/infer/call_cycles/call_cycles.h"
#include "compiler/infer/return_inferrer/return_inferrer.h"
#include "compiler/whole_program/flows/flows.h"
#include "llvm/ADT/STLExtras.h"

using llvm::cast;
using llvm::dyn_cast;

namespace tz {

MethodLiteralChecker::MethodLiteralChecker(Checker &owner)
    : checker(owner) {}

Expr *MethodLiteralChecker::checkMethodLiteral(ObjectLitExpr *lit, const Type *target) {
  lit->type = Type::error();
  if (target && target->isAnyObject()) {
    target = nullptr;
  }
  ResultSpreadAbsent spreadless = MethodLiteralChecker::noSpread(lit);
  if (spreadless.isErr()) {
    this->checker.diags_.error(lit->loc(), spreadless.error().message);
    for (ObjectLitExpr::Property &prop : lit->props) {
      prop.value = this->checker.exprChecker().checkExpr(prop.value);
    }
    return lit;
  }
  auto *cls = this->checker.ctx_.make<ClassDecl>(lit->loc(), "");
  cls->syntax = LiteralClass{.literal = lit};
  cls->id = ++this->checker.classCount_;
  lit->cls = cls;
  Type *type = Type::classType("", cls);
  cls->type = type;
  cls->stage = ClassStage::RESOLVED;
  cls->declared = true;
  this->checker.classes_.push_back(cls);

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
        this->checker.diags_.error(fn->loc(), simple.error().message);
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
    prop.value = this->checker.exprChecker().checkExpr(prop.value, wanted ? wanted->type : nullptr);
    if (wanted) {
      prop.value = this->checker.typeRelations().coerce(prop.value, wanted->type, what);
    }
    const Type *valueType =
        wanted && !prop.value->type->isError() ? wanted->type : prop.value->type;
    ResultValueIsTyped typed = MethodLiteralChecker::fieldValue(valueType, what);
    if (typed.isErr()) {
      this->checker.diags_.error(prop.value->loc(), typed.error().message);
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

  NestedGroup *group = this->checker.ctx_.newGroup();
  group->owner = this->checker.currentDecl();
  group->scopeDepth = this->checker.scopes_.size();
  group->functionDepth = this->checker.functions_.size();
  group->creationSeq = ++this->checker.seq_;
  group->finished = true;
  group->members = cls->methods;
  cls->group = group;
  for (FunctionDecl *fn : cls->methods) {
    fn->group = group;
    fn->parent = this->checker.currentDecl();
    fn->creationSeq = group->creationSeq;
    const Field *wanted = target ? target->field(fn->name) : nullptr;
    if (wanted && wanted->type->isFunction()) {
      this->checker.classDeclarer().declareTyped(fn, wanted->type);
      if (fn->returnType) {
        fn->sym->ret = this->checker.predicateChecker().resolveReturnType(fn, fn->sym);
      }
    } else {
      this->checker.functionChecker().declareFunction(fn, nullptr, false);
    }
  }
  this->checker.callCycles().findCycles(
      cls->methods, [](FunctionDecl *, llvm::StringRef) -> FunctionDecl * { return nullptr; });
  for (FunctionDecl *fn : cls->methods) {
    if (!fn->sym->ret) {
      this->checker.returnInferrer().inferReturnType(fn->sym);
    }
    if (fn->checked) {
      continue;
    }
    Checker::BodyState saved = this->checker.returnInferrer().enterBody(fn);
    this->checker.functionChecker().checkFunctionBody(fn);
    this->checker.returnInferrer().leaveBody(saved);
  }
  lit->type = type;
  if (!target) {
    return lit;
  }
  for (const ObjectLitExpr::Property &prop : lit->props) {
    ResultPropertyFits fits = this->propertyFits(prop, target);
    if (fits.isErr()) {
      this->checker.diags_.error(prop.loc, fits.error().message);
      lit->type = Type::error();
      return lit;
    }
  }
  ResultMembersMatch matched = MethodLiteralChecker::membersMatch(lit, target);
  if (matched.isErr()) {
    this->checker.diags_.error(lit->loc(), matched.error().message);
    lit->type = Type::error();
    return lit;
  }
  this->checker.classBases().addBase(type, target);

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
  const Type *type = this->checker.returnInferrer().functionType(fn->sym);
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
