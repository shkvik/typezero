#include "compiler/check/classes/class_checker/class_checker.h"
#include "compiler/check/classes/class_links/class_links.h"
#include "compiler/check/classes/decorator_checker/decorator_checker.h"
#include "compiler/check/classes/inherit_checker/inherit_checker.h"
#include "compiler/check/functions/function_checker/function_checker.h"
#include "compiler/check/functions/nested_functions/nested_functions.h"
#include "compiler/check/generics/generic_checker/generic_checker.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/declare/class_declarer/class_declarer.h"
#include "compiler/infer/return_inferrer/return_inferrer.h"
#include "compiler/instances/late_classes/late_classes.h"
#include "llvm/ADT/STLExtras.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

void collectMemberCalls(const Stmt *body, llvm::StringSet<> &names) {
  visitExprs(body, [&](const Expr *e) {
    if (auto *call = dyn_cast<CallExpr>(e)) {
      if (auto *member = dyn_cast<MemberExpr>(call->callee)) {
        names.insert(member->name);
      }
    }
  });
}

ClassChecker::ClassChecker(Checker &owner)
    : checker(owner) {}

ClassChecker::ResultClassDeclared ClassChecker::checkClassStmt(ClassDecl *cls) {
  bool topLevel =
      this->checker.scopes_.size() == 1 && this->checker.functions_.empty() && cls->type;
  if (!topLevel) {
    std::string message = ErrorTextClassStmt::nested(cls->name);

    return err(ClassCheckerCheckClassStmtError{
        .code = ClassCheckerCheckClassStmtErrorCode::NOT_TOP_LEVEL,
        .message = message,
    });
  }

  if (cls->baseExpr && !cls->resolved) {
    this->checker.lateClasses().makeLateClass(cls);
  }
  this->checker.lateClasses().checkStaticFields(cls);
  ClassDecl *outer = std::exchange(this->checker.currentClass_, cls);
  if (!cls->generic && this->hasDecorators(cls).isOk()) {
    this->checker.decoratorChecker().decorateClass(cls);
  }
  this->checker.currentClass_ = outer;

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
  bool needsValue = !field.isStatic && !field.method && !field.optional && !field.init &&
                    !field.definite && field.resolved && !field.resolved->isError();
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
  const Type *mine = overridden ? this->checker.returnInferrer().returnType(method->sym) : nullptr;
  const Type *theirs =
      overridden ? this->checker.returnInferrer().returnType(overridden->sym) : nullptr;

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

ClassChecker::ResultBodiesChecked ClassChecker::checkClassBodies(ClassDecl *cls) {
  std::vector<ClassCheckerCheckClassBodiesError> errors;
  this->checker.classDeclarer().completeClass(cls);
  if (cls->init && !cls->init->checked) {
    this->checker.flow_ = Checker::Flow();
    this->checker.functionChecker().checkFunctionBody(cls->init);
  }
  if (cls->constructors.empty()) {
    for (const ClassField &f : cls->fields) {
      ResultFieldHasValue started = this->fieldStartValue(cls, f);
      if (started.isErr()) {
        errors.push_back(ClassCheckerCheckClassBodiesError{
            .code = ClassCheckerCheckClassBodiesErrorCode::FIELD_WITHOUT_VALUE,
            .message = started.error().message,
            .loc = f.loc,
        });
      }
    }
  }
  for (FunctionDecl *ctor : cls->constructors) {
    if (!ctor->checked) {
      this->checker.flow_ = Checker::Flow();
      this->checker.functionChecker().checkFunctionBody(ctor);
    }
  }
  std::vector<FunctionDecl *> methods = cls->methods;
  for (const ClassField &f : cls->fields) {
    if (f.method) {
      methods.push_back(f.method);
    }
  }
  for (FunctionDecl *fn : methods) {
    if (this->checker.lateClasses().isTemplate(fn->sym)) {
      continue;
    }
    if (!fn->sym->ret) {
      this->checker.returnInferrer().inferReturnType(fn->sym);
    }
    if (fn->checked || fn->isAbstract()) {
      continue;
    }
    this->checker.flow_ = Checker::Flow();
    this->checker.functionChecker().checkFunctionBody(fn);
  }
  this->checker.inheritChecker().checkAbstract(cls);
  this->checker.inheritChecker().checkImplements(cls);
  for (FunctionDecl *fn : ClassLinks::ownMethods(cls)) {
    ResultReturnMatches returned = this->overrideReturn(cls, fn);
    if (returned.isErr()) {
      errors.push_back(ClassCheckerCheckClassBodiesError{
          .code = ClassCheckerCheckClassBodiesErrorCode::OVERRIDE_RETURNS_OTHER_TYPE,
          .message = returned.error().message,
          .loc = fn->loc(),
      });
    }
  }
  if (!errors.empty()) {
    return err(std::move(errors));
  }

  return ok(cls);
}

ClassChecker::ResultNewThisSymbol ClassChecker::declareThis(FunctionDecl *fn) {
  if (!fn->takesThis()) {
    std::string message = ErrorTextDeclareThis::notMethod(fn->name);

    return err(ClassCheckerDeclareThisError{
        .code = ClassCheckerDeclareThisErrorCode::NOT_A_METHOD,
        .message = message,
    });
  }

  VarSymbol *self = this->checker.ctx_.newVar();
  self->name = "this";
  self->loc = fn->loc();
  self->type = fn->cls()->type;
  self->isConst = true;
  unsigned declSeq = ++this->checker.seq_;
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
  bool inConstructor =
      this->checker.initThis_ && this->checker.currentDecl() == this->checker.initFn_;
  if (!inConstructor) {
    std::string message = ErrorTextInitThis::outside();

    return err(ClassCheckerIsInitThisError{
        .code = ClassCheckerIsInitThisErrorCode::NOT_IN_CONSTRUCTOR,
        .message = message,
    });
  }
  if (!name || name->var != this->checker.initThis_) {
    std::string message = ErrorTextInitThis::notThis();

    return err(ClassCheckerIsInitThisError{
        .code = ClassCheckerIsInitThisErrorCode::NOT_THIS,
        .message = message,
    });
  }

  return ok(name);
}

ClassChecker::ResultObjectIsReady ClassChecker::thisReady(const Checker::Flow &flow) const {
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

ClassChecker::ResultFieldUnsetYet ClassChecker::firstUnset(const Checker::Flow &flow) const {
  if (!this->checker.initFn_) {
    std::string message = ErrorTextFirstUnset::outside();

    return err(ClassCheckerFirstUnsetError{
        .code = ClassCheckerFirstUnsetErrorCode::NOT_IN_CONSTRUCTOR,
        .message = message,
    });
  }

  const std::vector<ClassField> &fields = this->checker.initFn_->cls()->fields;
  auto unset = llvm::find_if(fields, [&](const ClassField &f) {
    return !f.isStatic && !f.method && !f.optional && !llvm::is_contained(flow.setFields, f.name);
  });
  if (unset == fields.end()) {
    std::string message = ErrorTextFirstUnset::allSet(this->checker.initFn_->cls()->name);

    return err(ClassCheckerFirstUnsetError{
        .code = ClassCheckerFirstUnsetErrorCode::ALL_FIELDS_SET,
        .message = message,
    });
  }

  return ok(unset->name.str());
}

ClassChecker::ResultEveryFieldSet ClassChecker::checkFieldsSet() const {
  ResultFieldUnsetYet unset = this->firstUnset(this->checker.flow_);
  if (!this->checker.flow_.unreachable && unset.isOk()) {
    std::string message =
        ErrorTextFieldsSet::pathUnset(this->checker.initFn_->cls()->name, unset.value());

    return err(ClassCheckerCheckFieldsSetError{
        .code = ClassCheckerCheckFieldsSetErrorCode::FIELD_NOT_SET_ON_EVERY_PATH,
        .message = message,
    });
  }

  return ok(this->checker.initFn_);
}

ClassChecker::ResultThisComesFrom ClassChecker::thisOf(const FunctionDecl *current) const {
  std::string member = this->checker.thisMember_.empty() ? "x" : this->checker.thisMember_.str();
  const FunctionDecl *source = current ? current->thisSource() : nullptr;
  if (!source && this->checker.staticInit_ && this->checker.currentClass_) {
    std::string message = ErrorTextThisOf::inField(this->checker.currentClass_->name, member);

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
  bool inInit = var == this->checker.initThis_;
  bool inConstructor = inInit && this->checker.currentDecl() == this->checker.initFn_;
  bool inClosure = inInit && this->checker.currentDecl() != this->checker.initFn_;
  if (inClosure && !this->checker.closureThisReady_) {
    std::string message = ErrorTextReadyThis::inClosure();

    return err(ClassCheckerReadyThisError{
        .code = ClassCheckerReadyThisErrorCode::IN_CLOSURE_BEFORE_FIELDS_SET,
        .message = message,
    });
  }
  if (inConstructor && this->checker.flow_.beforeSuper) {
    std::string message = ErrorTextReadyThis::noSuper(this->checker.initFn_->cls()->parent->name);

    return err(ClassCheckerReadyThisError{
        .code = ClassCheckerReadyThisErrorCode::BEFORE_SUPER,
        .message = message,
    });
  }

  bool fieldsMatter = inConstructor && this->checker.thisMember_.empty();
  ResultFieldUnsetYet unset = this->firstUnset(this->checker.flow_);
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
  ResultThisComesFrom found = this->thisOf(this->checker.currentDecl());
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

  name->var = var;
  this->checker.nestedFunctions().noteUse(var, name->loc());
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
      source->takesThis() ? nullptr : this->checker.genericChecker().narrowedType(var);
  Expr *reference = narrowed ? this->checker.typeRelations().convert(name, narrowed) : name;

  return ok(reference);
}

}
