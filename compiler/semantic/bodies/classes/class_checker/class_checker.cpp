#include "compiler/semantic/bodies/classes/class_checker/class_checker.h"
#include "compiler/program/library/builtin_names/keyword_table/keyword_table.h"
#include "compiler/semantic/bodies/classes/class_links/class_links.h"
#include "compiler/semantic/bodies/classes/decorator_checker/decorator_checker.h"
#include "compiler/semantic/bodies/classes/inherit_checker/inherit_checker.h"
#include "compiler/semantic/bodies/functions/function_checker/function_checker.h"
#include "compiler/semantic/bodies/functions/nested_functions/nested_functions.h"
#include "compiler/semantic/declare/class_declarer/class_declarer.h"
#include "compiler/semantic/flow/expr_parts/expr_parts.h"
#include "compiler/semantic/generics/generic_checker/generic_checker.h"
#include "compiler/semantic/infer/late_classes/late_classes.h"
#include "compiler/semantic/infer/return_inferrer/return_inferrer.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "llvm/ADT/STLExtras.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

namespace {

constexpr const char *kMemberPlaceholder = "x";

}

ClassChecker::ClassChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->classLinks = &all.classLinks;
  this->lateClasses = &all.lateClasses;
  this->classDeclarer = &all.classDeclarer;
  this->typeRelations = &all.typeRelations;
  this->genericChecker = &all.genericChecker;
  this->inheritChecker = &all.inheritChecker;
  this->returnInferrer = &all.returnInferrer;
  this->functionChecker = &all.functionChecker;
  this->nestedFunctions = &all.nestedFunctions;
  this->decoratorChecker = &all.decoratorChecker;
}

ClassChecker::ResultClassDeclared ClassChecker::checkClassStmt(ClassDecl *classDecl) {
  bool isTopLevel = this->state.getScopeStack().isTopLevel() &&
                    !this->state.getFunctionStack().isInsideFunction() &&
                    classDecl->type != nullptr;
  if (!isTopLevel) {
    auto message = ErrorTextClassStmt::nested(classDecl->name);

    return err(ClassCheckerCheckClassStmtError{
        .code = ClassCheckerCheckClassStmtErrorCode::NOT_TOP_LEVEL,
        .message = message,
    });
  }

  if (classDecl->baseExpr != nullptr && classDecl->stage == ClassStage::UNRESOLVED) {
    this->lateClasses->makeLateClass(classDecl);
  }
  this->lateClasses->walkStaticFields(classDecl);
  this->state.getClassContext().enterClass(classDecl);
  if (classDecl->generic() == nullptr && this->hasDecorators(classDecl).isOk()) {
    this->decoratorChecker->decorateClass(classDecl);
  }
  this->state.getClassContext().leaveClass();

  return ok(classDecl);
}

ClassChecker::ResultHasDecorators ClassChecker::hasDecorators(const ClassDecl *classDecl) const {
  auto functionTest = std::bind_front(&ClassChecker::isDecoratedFunction, this);
  auto fieldTest = std::bind_front(&ClassChecker::isDecoratedField, this);
  bool isDecoratedAnywhere = !classDecl->decorators.empty() ||
                             llvm::any_of(classDecl->methods, functionTest) ||
                             llvm::any_of(classDecl->constructors, functionTest) ||
                             llvm::any_of(classDecl->fields, fieldTest);
  if (!isDecoratedAnywhere) {
    auto message = ErrorTextDecorators::none(classDecl->name);

    return err(ClassCheckerHasDecoratorsError{
        .code = ClassCheckerHasDecoratorsErrorCode::NO_DECORATORS,
        .message = message,
    });
  }

  return ok(classDecl);
}

bool ClassChecker::isDecoratedFunction(const FunctionDecl *functionDecl) const {
  if (!functionDecl->decorators().empty()) {
    return true;
  }

  return llvm::any_of(functionDecl->params, std::bind_front(&ClassChecker::isDecoratedParam, this));
}

bool ClassChecker::isDecoratedParam(const Param &param) const {
  return !param.decorators.empty();
}

bool ClassChecker::isDecoratedField(const ClassField &field) const {
  return !field.decorators.empty();
}

ClassChecker::ResultFieldHasValue
ClassChecker::checkFieldStartValue(const ClassDecl *classDecl, const ClassField &field) const {
  bool isNeedingValue = !field.isStatic() && field.method() == nullptr && !field.optional() &&
                        field.init == nullptr && !field.definite() && field.resolved != nullptr &&
                        !field.resolved->isError();
  if (isNeedingValue && this->classLinks->constructorsOf(classDecl).empty()) {
    auto message = ErrorTextStartValue::noStart(field.name, field.resolved->name());

    return err(ClassCheckerFieldStartValueError{
        .code = ClassCheckerFieldStartValueErrorCode::NO_START_VALUE,
        .message = message,
    });
  }
  if (isNeedingValue) {
    auto message = ErrorTextStartValue::neverSet(field.name, field.resolved->name());

    return err(ClassCheckerFieldStartValueError{
        .code = ClassCheckerFieldStartValueErrorCode::NEVER_SET,
        .message = message,
    });
  }

  return ok(&field);
}

ClassChecker::ResultReturnMatches ClassChecker::checkOverrideReturn(const ClassDecl *classDecl,
                                                                    const FunctionDecl *method) {
  const FunctionDecl *overridden = method->overrides();
  const Type *mine = overridden != nullptr ? method->sym->getReturnType() : nullptr;
  const Type *theirs = overridden != nullptr ? overridden->sym->getReturnType() : nullptr;

  bool isKnown = mine != nullptr && theirs != nullptr && !mine->isError() && !theirs->isError();
  bool isNarrowerObject = isKnown && mine->nonOptional()->isObject() &&
                          theirs->nonOptional()->isObject() &&
                          TypeRelations::isImplicitlyConvertible(mine, theirs);

  if (isKnown && mine != theirs && !isNarrowerObject) {
    auto message = ErrorTextOverride::otherType(method->name, mine->name(), classDecl->name,
                                                theirs->name(), overridden->cls()->name);

    return err(ClassCheckerOverrideReturnError{
        .code = ClassCheckerOverrideReturnErrorCode::RETURNS_OTHER_TYPE,
        .message = message,
    });
  }

  return ok(method);
}

void ClassChecker::walkClassBodies(ClassDecl *classDecl) {
  this->classDeclarer->completeClass(classDecl);
  if (classDecl->init != nullptr && !classDecl->init->checked) {
    this->functionChecker->walkFunctionFromScratch(classDecl->init);
  }
  if (classDecl->constructors.empty()) {
    this->walkFieldStarts(classDecl);
  }
  for (FunctionDecl *constructor : classDecl->constructors) {
    if (!constructor->checked) {
      this->functionChecker->walkFunctionFromScratch(constructor);
    }
  }
  for (FunctionDecl *functionDecl : this->classMethods(classDecl)) {
    this->walkClassMethod(functionDecl);
  }
  this->inheritChecker->walkAbstract(classDecl);
  this->inheritChecker->walkImplements(classDecl);
  this->walkOverrideReturns(classDecl);
}

void ClassChecker::walkFieldStarts(const ClassDecl *classDecl) {
  for (const ClassField &field : classDecl->fields) {
    auto started = this->checkFieldStartValue(classDecl, field);
    if (started.isErr()) {
      this->state.getDiagnostics().error(field.loc, started.error().message);
    }
  }
}

std::vector<FunctionDecl *> ClassChecker::classMethods(const ClassDecl *classDecl) const {
  std::vector<FunctionDecl *> methods = classDecl->methods;
  for (const ClassField &field : classDecl->fields) {
    if (field.method() != nullptr) {
      methods.push_back(field.method());
    }
  }

  return methods;
}

void ClassChecker::walkClassMethod(FunctionDecl *functionDecl) {
  if (this->lateClasses->isTemplate(functionDecl->sym)) {
    return;
  }
  if (!functionDecl->sym->hasReturnType()) {
    this->returnInferrer->inferReturnType(functionDecl->sym);
  }
  if (functionDecl->checked || functionDecl->isAbstract()) {
    return;
  }
  this->functionChecker->walkFunctionFromScratch(functionDecl);
}

void ClassChecker::walkOverrideReturns(const ClassDecl *classDecl) {
  for (FunctionDecl *functionDecl : this->classLinks->ownMethods(classDecl)) {
    auto returned = this->checkOverrideReturn(classDecl, functionDecl);
    if (returned.isErr()) {
      this->state.getDiagnostics().error(functionDecl->loc(), returned.error().message);
    }
  }
}

ClassChecker::ResultNewThisSymbol ClassChecker::declareThis(FunctionDecl *functionDecl) {
  if (!functionDecl->takesThis()) {
    auto message = ErrorTextDeclareThis::notMethod(functionDecl->name);

    return err(ClassCheckerDeclareThisError{
        .code = ClassCheckerDeclareThisErrorCode::NOT_A_METHOD,
        .message = message,
    });
  }
  auto *self = this->state.getAst().newVar();
  self->name = KeywordTable::getName(Keyword::THIS);
  self->loc = functionDecl->loc();
  self->type = functionDecl->cls()->type;
  self->isConst = true;
  unsigned declSeq = this->state.getReturnInference().takeSequenceNumber();
  self->place = FunctionLocalVar{.owner = functionDecl, .state = {.declSeq = declSeq}};
  ClassMember *member = functionDecl->member();
  switch (member->method.kind()) {
    case MethodKind::INSTANCE_METHOD:
      member->method.as<InstanceMethod>()->thisVar = self;
      break;
    case MethodKind::STATIC_METHOD:
      break;
    case MethodKind::CONSTRUCTOR:
      member->method.as<Constructor>()->thisVar = self;
      break;
    case MethodKind::ABSTRACT_METHOD:
      member->method.as<AbstractMethod>()->thisVar = self;
      break;
  }

  return ok(self);
}

ClassChecker::ResultInitThisFound ClassChecker::isInitThis(const Expr *expr) const {
  const Expr *plain = ExprParts::withoutImplicitCasts(expr);
  auto *name = dyn_cast<NameExpr>(plain);
  bool isInConstructor = this->state.getClassContext().getInitializerThis() != nullptr &&
                         this->state.getFunctionStack().getCurrentDecl() ==
                             this->state.getClassContext().getInitializer();
  if (!isInConstructor) {
    auto message = ErrorTextInitThis::outside();

    return err(ClassCheckerIsInitThisError{
        .code = ClassCheckerIsInitThisErrorCode::NOT_IN_CONSTRUCTOR,
        .message = message,
    });
  }
  if (name == nullptr || name->var() != this->state.getClassContext().getInitializerThis()) {
    auto message = ErrorTextInitThis::notThis();

    return err(ClassCheckerIsInitThisError{
        .code = ClassCheckerIsInitThisErrorCode::NOT_THIS,
        .message = message,
    });
  }

  return ok(name);
}

ClassChecker::ResultObjectIsReady ClassChecker::checkThisReady(const Flow &flow) const {
  auto unset = this->firstUnset(flow);
  if (flow.beforeSuper) {
    auto message = ErrorTextThisReady::noSuper();

    return err(ClassCheckerThisReadyError{
        .code = ClassCheckerThisReadyErrorCode::BEFORE_SUPER,
        .message = message,
    });
  }
  if (unset.isOk()) {
    auto message = ErrorTextThisReady::unset(unset.value());

    return err(ClassCheckerThisReadyError{
        .code = ClassCheckerThisReadyErrorCode::FIELD_NOT_SET,
        .message = message,
    });
  }

  return ok(&flow);
}

ClassChecker::ResultFieldUnsetYet ClassChecker::firstUnset(const Flow &flow) const {
  if (this->state.getClassContext().getInitializer() == nullptr) {
    auto message = ErrorTextFirstUnset::outside();

    return err(ClassCheckerFirstUnsetError{
        .code = ClassCheckerFirstUnsetErrorCode::NOT_IN_CONSTRUCTOR,
        .message = message,
    });
  }
  const ClassDecl *initialized = this->state.getClassContext().getInitializer()->cls();
  const std::vector<ClassField> &fields = initialized->fields;
  auto unset =
      llvm::find_if(fields, std::bind_front(&ClassChecker::isUnsetField, this, std::cref(flow)));
  if (unset == fields.end()) {
    auto message = ErrorTextFirstUnset::allSet(initialized->name);

    return err(ClassCheckerFirstUnsetError{
        .code = ClassCheckerFirstUnsetErrorCode::ALL_FIELDS_SET,
        .message = message,
    });
  }

  return ok(unset->name.str());
}

bool ClassChecker::isUnsetField(const Flow &flow, const ClassField &field) const {
  return !field.isStatic() && field.method() == nullptr && !field.optional() &&
         !llvm::is_contained(flow.setFields, field.name);
}

ClassChecker::ResultEveryFieldSet ClassChecker::checkFieldsSet() const {
  const Flow &flow = this->state.getFlowState().getFlow();
  auto unset = this->firstUnset(flow);
  const FunctionDecl *initializer = this->state.getClassContext().getInitializer();
  if (!this->state.getFlowState().isUnreachable() && unset.isOk()) {
    auto message = ErrorTextFieldsSet::pathUnset(initializer->cls()->name, unset.value());

    return err(ClassCheckerCheckFieldsSetError{
        .code = ClassCheckerCheckFieldsSetErrorCode::FIELD_NOT_SET_ON_EVERY_PATH,
        .message = message,
    });
  }

  return ok(initializer);
}

ClassChecker::ResultThisComesFrom ClassChecker::checkThisOf(const FunctionDecl *current) const {
  auto member = this->thisMemberText();
  const FunctionDecl *source = current != nullptr ? current->thisSource() : nullptr;
  const ClassDecl *currentClass = this->state.getClassContext().getCurrentClass();
  bool isInStaticField = source == nullptr && this->state.getClassContext().isInsideStaticInit() &&
                         currentClass != nullptr;
  if (isInStaticField) {
    auto message = ErrorTextThisOf::inField(currentClass->name, member);

    return err(ClassCheckerThisOfError{
        .code = ClassCheckerThisOfErrorCode::IN_STATIC_FIELD,
        .message = message,
    });
  }
  if (source == nullptr) {
    auto message = ErrorTextThisOf::outside();

    return err(ClassCheckerThisOfError{
        .code = ClassCheckerThisOfErrorCode::OUTSIDE_METHOD,
        .message = message,
    });
  }
  bool hasThis = source->takesThis() || source->hasThisParam();
  if (!hasThis && source->cls() != nullptr && source->isStatic()) {
    auto message = ErrorTextThisOf::inStatic(source->cls()->name, member);

    return err(ClassCheckerThisOfError{
        .code = ClassCheckerThisOfErrorCode::IN_STATIC_METHOD,
        .message = message,
    });
  }
  if (!hasThis) {
    auto message = ErrorTextThisOf::inFunction();

    return err(ClassCheckerThisOfError{
        .code = ClassCheckerThisOfErrorCode::IN_PLAIN_FUNCTION,
        .message = message,
    });
  }

  return ok(source);
}

std::string ClassChecker::thisMemberText() const {
  llvm::StringRef member = this->state.getClassContext().getThisMember();
  if (member.empty()) {
    return kMemberPlaceholder;
  }

  return member.str();
}

ClassChecker::ResultThisReadiness ClassChecker::checkReadyThis(VarSymbol *variable) const {
  const FunctionDecl *initializer = this->state.getClassContext().getInitializer();
  const FunctionDecl *currentDecl = this->state.getFunctionStack().getCurrentDecl();
  bool isInInit = variable == this->state.getClassContext().getInitializerThis();
  bool isInConstructor = isInInit && currentDecl == initializer;
  bool isInClosure = isInInit && currentDecl != initializer;
  if (isInClosure && !this->state.getClassContext().isClosureThisReady()) {
    auto message = ErrorTextReadyThis::inClosure();

    return err(ClassCheckerReadyThisError{
        .code = ClassCheckerReadyThisErrorCode::IN_CLOSURE_BEFORE_FIELDS_SET,
        .message = message,
    });
  }
  if (isInConstructor && this->state.getFlowState().isBeforeSuper()) {
    auto message = ErrorTextReadyThis::noSuper(initializer->cls()->parent->name);

    return err(ClassCheckerReadyThisError{
        .code = ClassCheckerReadyThisErrorCode::BEFORE_SUPER,
        .message = message,
    });
  }
  bool hasFieldsMatter = isInConstructor && this->state.getClassContext().getThisMember().empty();
  const Flow &flow = this->state.getFlowState().getFlow();
  auto unset = this->firstUnset(flow);
  if (hasFieldsMatter && unset.isOk()) {
    auto message = ErrorTextReadyThis::unset(unset.value());

    return err(ClassCheckerReadyThisError{
        .code = ClassCheckerReadyThisErrorCode::BEFORE_FIELDS_SET,
        .message = message,
    });
  }

  return ok(variable);
}

ClassChecker::ResultThisReference ClassChecker::checkThis(NameExpr *name) {
  name->type = Type::error();
  const FunctionDecl *currentDecl = this->state.getFunctionStack().getCurrentDecl();
  auto found = this->checkThisOf(currentDecl);
  if (found.isErr()) {
    return err(ClassCheckerCheckThisError{
        .code = ClassCheckerCheckThisErrorCode::THIS_NOT_FOUND,
        .message = found.error().message,
    });
  }
  const FunctionDecl *source = found.value();
  VarSymbol *variable = source->takesThis() ? source->thisVar() : source->params[0].sym;
  if (variable == nullptr) {
    auto message = ErrorTextThis::undeclared(source->name);

    return err(ClassCheckerCheckThisError{
        .code = ClassCheckerCheckThisErrorCode::THIS_NOT_DECLARED,
        .message = message,
    });
  }
  name->binding = NamedVar{.var = variable};
  this->nestedFunctions->noteUse(variable, name->loc());
  name->type = variable->type;
  auto ready = this->checkReadyThis(variable);
  if (ready.isErr() && ready.error().code == ClassCheckerReadyThisErrorCode::BEFORE_SUPER) {
    name->type = Type::error();
  }
  if (ready.isErr()) {
    return err(ClassCheckerCheckThisError{
        .code = ClassCheckerCheckThisErrorCode::THIS_NOT_READY,
        .message = ready.error().message,
    });
  }
  const Type *narrowed =
      source->takesThis() ? nullptr : this->genericChecker->narrowedType(variable);
  Expr *reference = narrowed != nullptr ? this->typeRelations->convert(name, narrowed) : name;

  return ok(reference);
}

}
