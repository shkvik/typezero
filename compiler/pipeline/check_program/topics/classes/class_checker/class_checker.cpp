#include "compiler/pipeline/check_program/topics/classes/class_checker/class_checker.h"
#include "compiler/pipeline/check_program/declare/class_declarer/class_declarer.h"
#include "compiler/pipeline/check_program/generics/generic_checker/generic_checker.h"
#include "compiler/pipeline/check_program/infer/return_inferrer/return_inferrer.h"
#include "compiler/pipeline/check_program/instances/late_classes/late_classes.h"
#include "compiler/pipeline/check_program/topics/classes/class_links/class_links.h"
#include "compiler/pipeline/check_program/topics/classes/decorator_checker/decorator_checker.h"
#include "compiler/pipeline/check_program/topics/classes/inherit_checker/inherit_checker.h"
#include "compiler/pipeline/check_program/topics/functions/function_checker/function_checker.h"
#include "compiler/pipeline/check_program/topics/functions/nested_functions/nested_functions.h"
#include "compiler/pipeline/check_program/types/type_relations/type_relations.h"
#include "llvm/ADT/STLExtras.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

ClassChecker::ClassChecker(CheckState &owner)
    : state(owner) {}

ClassChecker::ResultClassDeclared ClassChecker::checkClassStmt(ClassDecl *cls) {
  bool topLevel = this->state.getScopeStack().isTopLevel() &&
                  !this->state.getFunctionStack().isInsideFunction() && cls->type;
  if (!topLevel) {
    std::string message = ErrorTextClassStmt::nested(cls->name);

    return err(ClassCheckerCheckClassStmtError{
        .code = ClassCheckerCheckClassStmtErrorCode::NOT_TOP_LEVEL,
        .message = message,
    });
  }

  if (cls->baseExpr && cls->stage == ClassStage::UNRESOLVED) {
    LateClasses(this->state).makeLateClass(cls);
  }
  LateClasses(this->state).checkStaticFields(cls);
  this->state.getClassContext().enterClass(cls);
  if (!cls->generic() && this->hasDecorators(cls).isOk()) {
    DecoratorChecker(this->state).decorateClass(cls);
  }
  this->state.getClassContext().leaveClass();

  return ok(cls);
}

ClassChecker::ResultHasDecorators ClassChecker::hasDecorators(const ClassDecl *cls) const {
  auto decorated = [](const FunctionDecl *fn) {
    return !fn->decorators().empty() ||
           llvm::any_of(fn->params, [](const Param &p) { return !p.decorators.empty(); });
  };
  auto decoratedField = [](const ClassField &f) { return !f.decorators.empty(); };

  bool decoratedAnywhere = !cls->decorators.empty() || llvm::any_of(cls->methods, decorated) ||
                           llvm::any_of(cls->constructors, decorated) ||
                           llvm::any_of(cls->fields, decoratedField);

  if (!decoratedAnywhere) {
    std::string message = ErrorTextDecorators::none(cls->name);

    return err(ClassCheckerHasDecoratorsError{
        .code = ClassCheckerHasDecoratorsErrorCode::NO_DECORATORS,
        .message = message,
    });
  }

  return ok(cls);
}

ClassChecker::ResultFieldHasValue ClassChecker::fieldStartValue(const ClassDecl *cls,
                                                                const ClassField &field) const {
  bool needsValue = !field.isStatic() && !field.method() && !field.optional() && !field.init &&
                    !field.definite() && field.resolved && !field.resolved->isError();
  if (needsValue && ClassLinks::constructorsOf(cls).empty()) {
    std::string message = ErrorTextStartValue::noStart(field.name, field.resolved->name());

    return err(ClassCheckerFieldStartValueError{
        .code = ClassCheckerFieldStartValueErrorCode::NO_START_VALUE,
        .message = message,
    });
  }
  if (needsValue) {
    std::string message = ErrorTextStartValue::neverSet(field.name, field.resolved->name());

    return err(ClassCheckerFieldStartValueError{
        .code = ClassCheckerFieldStartValueErrorCode::NEVER_SET,
        .message = message,
    });
  }

  return ok(&field);
}

ClassChecker::ResultReturnMatches ClassChecker::overrideReturn(const ClassDecl *cls,
                                                               const FunctionDecl *method) {
  const FunctionDecl *overridden = method->overrides();
  const Type *mine = overridden ? ReturnInferrer(this->state).returnType(method->sym) : nullptr;
  const Type *theirs =
      overridden ? ReturnInferrer(this->state).returnType(overridden->sym) : nullptr;

  bool known = mine && theirs && !mine->isError() && !theirs->isError();
  bool narrowerObject = known && mine->nonOptional()->isObject() &&
                        theirs->nonOptional()->isObject() &&
                        TypeRelations::isImplicitlyConvertible(mine, theirs);

  if (known && mine != theirs && !narrowerObject) {
    std::string message = ErrorTextOverride::otherType(method->name, mine->name(), cls->name,
                                                       theirs->name(), overridden->cls()->name);

    return err(ClassCheckerOverrideReturnError{
        .code = ClassCheckerOverrideReturnErrorCode::RETURNS_OTHER_TYPE,
        .message = message,
    });
  }

  return ok(method);
}

void ClassChecker::checkClassBodies(ClassDecl *cls) {
  ClassDeclarer(this->state).completeClass(cls);
  if (cls->init && !cls->init->checked) {
    this->state.getFlowState().replaceFlow(Flow());
    FunctionChecker(this->state).checkFunctionBody(cls->init);
  }
  if (cls->constructors.empty()) {
    for (const ClassField &f : cls->fields) {
      ResultFieldHasValue started = this->fieldStartValue(cls, f);
      if (started.isErr()) {
        this->state.getDiagnostics().error(f.loc, started.error().message);
      }
    }
  }
  for (FunctionDecl *ctor : cls->constructors) {
    if (!ctor->checked) {
      this->state.getFlowState().replaceFlow(Flow());
      FunctionChecker(this->state).checkFunctionBody(ctor);
    }
  }
  std::vector<FunctionDecl *> methods = cls->methods;
  for (const ClassField &f : cls->fields) {
    if (f.method()) {
      methods.push_back(f.method());
    }
  }
  for (FunctionDecl *fn : methods) {
    if (LateClasses(this->state).isTemplate(fn->sym)) {
      continue;
    }
    if (!fn->sym->ret) {
      ReturnInferrer(this->state).inferReturnType(fn->sym);
    }
    if (fn->checked || fn->isAbstract()) {
      continue;
    }
    this->state.getFlowState().replaceFlow(Flow());
    FunctionChecker(this->state).checkFunctionBody(fn);
  }
  InheritChecker(this->state).checkAbstract(cls);
  InheritChecker(this->state).checkImplements(cls);
  for (FunctionDecl *fn : ClassLinks::ownMethods(cls)) {
    ResultReturnMatches returned = this->overrideReturn(cls, fn);
    if (returned.isErr()) {
      this->state.getDiagnostics().error(fn->loc(), returned.error().message);
    }
  }
}

ClassChecker::ResultNewThisSymbol ClassChecker::declareThis(FunctionDecl *fn) {
  if (!fn->takesThis()) {
    std::string message = ErrorTextDeclareThis::notMethod(fn->name);

    return err(ClassCheckerDeclareThisError{
        .code = ClassCheckerDeclareThisErrorCode::NOT_A_METHOD,
        .message = message,
    });
  }

  VarSymbol *self = this->state.getAst().newVar();
  self->name = "this";
  self->loc = fn->loc();
  self->type = fn->cls()->type;
  self->isConst = true;
  unsigned declSeq = this->state.getReturnInference().takeSequenceNumber();
  self->place = FunctionLocalVar{.owner = fn, .state = {.declSeq = declSeq}};
  ClassMember *member = fn->member();

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
  while (auto *conv = dyn_cast<CastExpr>(expr)) {
    if (conv->isImplicit()) {
      expr = conv->operand;
    } else {
      break;
    }
  }
  auto *name = dyn_cast<NameExpr>(expr);
  bool inConstructor = this->state.getClassContext().getInitializerThis() &&
                       this->state.getFunctionStack().getCurrentDecl() ==
                           this->state.getClassContext().getInitializer();
  if (!inConstructor) {
    std::string message = ErrorTextInitThis::outside();

    return err(ClassCheckerIsInitThisError{
        .code = ClassCheckerIsInitThisErrorCode::NOT_IN_CONSTRUCTOR,
        .message = message,
    });
  }
  if (!name || name->var() != this->state.getClassContext().getInitializerThis()) {
    std::string message = ErrorTextInitThis::notThis();

    return err(ClassCheckerIsInitThisError{
        .code = ClassCheckerIsInitThisErrorCode::NOT_THIS,
        .message = message,
    });
  }

  return ok(name);
}

ClassChecker::ResultObjectIsReady ClassChecker::thisReady(const Flow &flow) const {
  ResultFieldUnsetYet unset = this->firstUnset(flow);
  if (flow.beforeSuper) {
    std::string message = ErrorTextThisReady::noSuper();

    return err(ClassCheckerThisReadyError{
        .code = ClassCheckerThisReadyErrorCode::BEFORE_SUPER,
        .message = message,
    });
  }
  if (unset.isOk()) {
    std::string message = ErrorTextThisReady::unset(unset.value());

    return err(ClassCheckerThisReadyError{
        .code = ClassCheckerThisReadyErrorCode::FIELD_NOT_SET,
        .message = message,
    });
  }

  return ok(&flow);
}

ClassChecker::ResultFieldUnsetYet ClassChecker::firstUnset(const Flow &flow) const {
  if (!this->state.getClassContext().getInitializer()) {
    std::string message = ErrorTextFirstUnset::outside();

    return err(ClassCheckerFirstUnsetError{
        .code = ClassCheckerFirstUnsetErrorCode::NOT_IN_CONSTRUCTOR,
        .message = message,
    });
  }

  const std::vector<ClassField> &fields =
      this->state.getClassContext().getInitializer()->cls()->fields;
  auto unset = llvm::find_if(fields, [&](const ClassField &f) {
    return !f.isStatic() && !f.method() && !f.optional() &&
           !llvm::is_contained(flow.setFields, f.name);
  });
  if (unset == fields.end()) {
    std::string message =
        ErrorTextFirstUnset::allSet(this->state.getClassContext().getInitializer()->cls()->name);

    return err(ClassCheckerFirstUnsetError{
        .code = ClassCheckerFirstUnsetErrorCode::ALL_FIELDS_SET,
        .message = message,
    });
  }

  return ok(unset->name.str());
}

ClassChecker::ResultEveryFieldSet ClassChecker::checkFieldsSet() const {
  ResultFieldUnsetYet unset = this->firstUnset(this->state.getFlowState().getFlow());
  if (!this->state.getFlowState().isUnreachable() && unset.isOk()) {
    std::string message = ErrorTextFieldsSet::pathUnset(
        this->state.getClassContext().getInitializer()->cls()->name, unset.value());

    return err(ClassCheckerCheckFieldsSetError{
        .code = ClassCheckerCheckFieldsSetErrorCode::FIELD_NOT_SET_ON_EVERY_PATH,
        .message = message,
    });
  }

  return ok(this->state.getClassContext().getInitializer());
}

ClassChecker::ResultThisComesFrom ClassChecker::thisOf(const FunctionDecl *current) const {
  std::string member = this->state.getClassContext().getThisMember().empty()
                           ? "x"
                           : this->state.getClassContext().getThisMember().str();
  const FunctionDecl *source = current ? current->thisSource() : nullptr;
  if (!source && this->state.getClassContext().isInsideStaticInit() &&
      this->state.getClassContext().getCurrentClass()) {
    std::string message =
        ErrorTextThisOf::inField(this->state.getClassContext().getCurrentClass()->name, member);

    return err(ClassCheckerThisOfError{
        .code = ClassCheckerThisOfErrorCode::IN_STATIC_FIELD,
        .message = message,
    });
  }
  if (!source) {
    std::string message = ErrorTextThisOf::outside();

    return err(ClassCheckerThisOfError{
        .code = ClassCheckerThisOfErrorCode::OUTSIDE_METHOD,
        .message = message,
    });
  }

  bool hasThis = source->takesThis() || source->hasThisParam();
  if (!hasThis && source->cls() && source->isStatic()) {
    std::string message = ErrorTextThisOf::inStatic(source->cls()->name, member);

    return err(ClassCheckerThisOfError{
        .code = ClassCheckerThisOfErrorCode::IN_STATIC_METHOD,
        .message = message,
    });
  }
  if (!hasThis) {
    std::string message = ErrorTextThisOf::inFunction();

    return err(ClassCheckerThisOfError{
        .code = ClassCheckerThisOfErrorCode::IN_PLAIN_FUNCTION,
        .message = message,
    });
  }

  return ok(source);
}

ClassChecker::ResultThisReadiness ClassChecker::readyThis(VarSymbol *var) const {
  bool inInit = var == this->state.getClassContext().getInitializerThis();
  bool inConstructor = inInit && this->state.getFunctionStack().getCurrentDecl() ==
                                     this->state.getClassContext().getInitializer();
  bool inClosure = inInit && this->state.getFunctionStack().getCurrentDecl() !=
                                 this->state.getClassContext().getInitializer();
  if (inClosure && !this->state.getClassContext().isClosureThisReady()) {
    std::string message = ErrorTextReadyThis::inClosure();

    return err(ClassCheckerReadyThisError{
        .code = ClassCheckerReadyThisErrorCode::IN_CLOSURE_BEFORE_FIELDS_SET,
        .message = message,
    });
  }
  if (inConstructor && this->state.getFlowState().isBeforeSuper()) {
    std::string message = ErrorTextReadyThis::noSuper(
        this->state.getClassContext().getInitializer()->cls()->parent->name);

    return err(ClassCheckerReadyThisError{
        .code = ClassCheckerReadyThisErrorCode::BEFORE_SUPER,
        .message = message,
    });
  }

  bool fieldsMatter = inConstructor && this->state.getClassContext().getThisMember().empty();
  ResultFieldUnsetYet unset = this->firstUnset(this->state.getFlowState().getFlow());
  if (fieldsMatter && unset.isOk()) {
    std::string message = ErrorTextReadyThis::unset(unset.value());

    return err(ClassCheckerReadyThisError{
        .code = ClassCheckerReadyThisErrorCode::BEFORE_FIELDS_SET,
        .message = message,
    });
  }

  return ok(var);
}

ClassChecker::ResultThisReference ClassChecker::checkThis(NameExpr *name) {
  name->type = Type::error();
  ResultThisComesFrom found = this->thisOf(this->state.getFunctionStack().getCurrentDecl());
  if (found.isErr()) {
    return err(ClassCheckerCheckThisError{
        .code = ClassCheckerCheckThisErrorCode::THIS_NOT_FOUND,
        .message = found.error().message,
    });
  }

  const FunctionDecl *source = found.value();
  VarSymbol *var = source->takesThis() ? source->thisVar() : source->params[0].sym;
  if (!var) {
    std::string message = ErrorTextThis::undeclared(source->name);

    return err(ClassCheckerCheckThisError{
        .code = ClassCheckerCheckThisErrorCode::THIS_NOT_DECLARED,
        .message = message,
    });
  }

  name->binding = NamedVar{.var = var};
  NestedFunctions(this->state).noteUse(var, name->loc());
  name->type = var->type;
  ResultThisReadiness ready = this->readyThis(var);
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
      source->takesThis() ? nullptr : GenericChecker(this->state).narrowedType(var);
  Expr *reference = narrowed ? TypeRelations(this->state).convert(name, narrowed) : name;

  return ok(reference);
}

}
