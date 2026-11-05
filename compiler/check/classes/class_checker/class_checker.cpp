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
    return err(ClassCheckerCheckClassStmtError{
        .code = ClassCheckerCheckClassStmtErrorCode::NOT_TOP_LEVEL,
        .message = "a class can be declared only at the top level of a file; here, "
                   "make it with a class expression, 'const " +
                   cls->name.str() + " = class { ... }'",
    });
  }

  if (cls->baseExpr && !cls->resolved) {
    this->checker.lateClasses().makeLateClass(cls);
  }
  this->checker.lateClasses().checkStaticFields(cls);
  ClassDecl *outer = std::exchange(this->checker.currentClass_, cls);
  if (!cls->generic && hasDecorators(cls).isOk()) {
    this->checker.decoratorChecker().decorateClass(cls);
  }
  this->checker.currentClass_ = outer;

  return ok(cls);
}

ClassChecker::ResultHasDecorators ClassChecker::hasDecorators(const ClassDecl *cls) {
  auto decorated = [](const FunctionDecl *fn) {
    return !fn->decorators.empty() ||
           llvm::any_of(fn->params, [](const Param &p) { return !p.decorators.empty(); });
  };

  bool decoratedAnywhere =
      !cls->decorators.empty() || llvm::any_of(cls->methods, decorated) ||
      llvm::any_of(cls->constructors, decorated) ||
      llvm::any_of(cls->fields, [](const ClassField &f) { return !f.decorators.empty(); });

  if (!decoratedAnywhere) {
    return err(ClassCheckerHasDecoratorsError{
        .code = ClassCheckerHasDecoratorsErrorCode::NO_DECORATORS,
        .message = "class '" + cls->name.str() + "' has no decorators",
    });
  }

  return ok(cls);
}

ClassChecker::ResultFieldHasValue ClassChecker::fieldStartValue(const ClassDecl *cls,
                                                                const ClassField &field) const {
  bool needsValue = !field.isStatic && !field.method && !field.optional && !field.init &&
                    !field.definite && field.resolved && !field.resolved->isError();
  if (needsValue && ClassLinks::constructorsOf(cls).empty()) {
    std::string name = field.name.str();
    std::string type = field.resolved->name().str();
    return err(ClassCheckerFieldStartValueError{
        .code = ClassCheckerFieldStartValueErrorCode::NO_START_VALUE,
        .message = "field '" + name + "' has no value to start with: '" + type +
                   "' has no empty value, like \"\" or 0; give it one here, e.g. '" + name + ": " +
                   type + " = ...', set it in a constructor, or make it optional: '" + name +
                   "?: " + type + "'",
    });
  }
  if (needsValue) {
    std::string name = field.name.str();
    std::string type = field.resolved->name().str();
    return err(ClassCheckerFieldStartValueError{
        .code = ClassCheckerFieldStartValueErrorCode::NEVER_SET,
        .message = "field '" + name + "' never gets a value: give it one here, e.g. '" + name +
                   ": " + type + " = ...', set it in a constructor, or make it optional: '" + name +
                   "?: " + type + "'",
    });
  }

  return ok(&field);
}

ClassChecker::ResultReturnMatches ClassChecker::overrideReturn(const ClassDecl *cls,
                                                               const FunctionDecl *method) {
  const FunctionDecl *overridden = method->overrides;
  const Type *mine = overridden ? this->checker.returnInferrer().returnType(method->sym) : nullptr;
  const Type *theirs =
      overridden ? this->checker.returnInferrer().returnType(overridden->sym) : nullptr;

  bool known = mine && theirs && !mine->isError() && !theirs->isError();
  bool narrowerObject = known && mine->nonOptional()->isObject() &&
                        theirs->nonOptional()->isObject() &&
                        TypeRelations::isImplicitlyConvertible(mine, theirs);

  if (known && mine != theirs && !narrowerObject) {
    return err(ClassCheckerOverrideReturnError{
        .code = ClassCheckerOverrideReturnErrorCode::RETURNS_OTHER_TYPE,
        .message = "'" + method->name.str() + "' returns '" + mine->name().str() + "' in '" +
                   cls->name.str() + "', and '" + theirs->name().str() + "' in '" +
                   overridden->cls->name.str() +
                   "', which it overrides; an override returns what the method it "
                   "overrides returns",
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
      ResultFieldHasValue started = fieldStartValue(cls, f);
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
    if (fn->checked || fn->isAbstract) {
      continue;
    }
    this->checker.flow_ = Checker::Flow();
    this->checker.functionChecker().checkFunctionBody(fn);
  }
  this->checker.inheritChecker().checkAbstract(cls);
  this->checker.inheritChecker().checkImplements(cls);
  for (FunctionDecl *fn : ClassLinks::ownMethods(cls)) {
    ResultReturnMatches returned = overrideReturn(cls, fn);
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
    return err(ClassCheckerDeclareThisError{
        .code = ClassCheckerDeclareThisErrorCode::NOT_A_METHOD,
        .message = "internal error: 'this' is declared for '" + fn->name.str() +
                   "', which is not a method",
    });
  }

  VarSymbol *self = this->checker.ctx_.newVar();
  self->name = "this";
  self->loc = fn->loc();
  self->type = fn->cls->type;
  self->isConst = true;
  self->owner = fn;
  self->declSeq = ++this->checker.seq_;
  fn->thisVar = self;

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
    return err(ClassCheckerIsInitThisError{
        .code = ClassCheckerIsInitThisErrorCode::NOT_IN_CONSTRUCTOR,
        .message = "the code is not in the body of the constructor being checked",
    });
  }
  if (!name || name->var != this->checker.initThis_) {
    return err(ClassCheckerIsInitThisError{
        .code = ClassCheckerIsInitThisErrorCode::NOT_THIS,
        .message = "the expression is not the 'this' of the constructor",
    });
  }

  return ok(name);
}

ClassChecker::ResultObjectIsReady ClassChecker::thisReady(const Checker::Flow &flow) const {
  ResultFieldUnsetYet unset = firstUnset(flow);
  if (flow.beforeSuper) {
    return err(ClassCheckerThisReadyError{
        .code = ClassCheckerThisReadyErrorCode::BEFORE_SUPER,
        .message = "'super(...)' is not called yet",
    });
  }
  if (unset.isOk()) {
    return err(ClassCheckerThisReadyError{
        .code = ClassCheckerThisReadyErrorCode::FIELD_NOT_SET,
        .message = "field '" + unset.value() + "' is not set yet",
    });
  }

  return ok(&flow);
}

ClassChecker::ResultFieldUnsetYet ClassChecker::firstUnset(const Checker::Flow &flow) const {
  if (!this->checker.initFn_) {
    return err(ClassCheckerFirstUnsetError{
        .code = ClassCheckerFirstUnsetErrorCode::NOT_IN_CONSTRUCTOR,
        .message = "the code is not in a constructor",
    });
  }

  const std::vector<ClassField> &fields = this->checker.initFn_->cls->fields;
  auto unset = llvm::find_if(fields, [&](const ClassField &f) {
    return !f.isStatic && !f.method && !f.optional && !llvm::is_contained(flow.setFields, f.name);
  });
  if (unset == fields.end()) {
    return err(ClassCheckerFirstUnsetError{
        .code = ClassCheckerFirstUnsetErrorCode::ALL_FIELDS_SET,
        .message = "every field of '" + this->checker.initFn_->cls->name.str() + "' is set",
    });
  }

  return ok(unset->name.str());
}

ClassChecker::ResultEveryFieldSet ClassChecker::checkFieldsSet() const {
  ResultFieldUnsetYet unset = firstUnset(this->checker.flow_);
  if (!this->checker.flow_.unreachable && unset.isOk()) {
    return err(ClassCheckerCheckFieldsSetError{
        .code = ClassCheckerCheckFieldsSetErrorCode::FIELD_NOT_SET_ON_EVERY_PATH,
        .message = "the constructor of '" + this->checker.initFn_->cls->name.str() +
                   "' does not set field '" + unset.value() +
                   "' on every path; set it, give it a value where it is declared, or "
                   "make it optional: '" +
                   unset.value() + "?: T'",
    });
  }

  return ok(this->checker.initFn_);
}

ClassChecker::ResultThisComesFrom ClassChecker::thisOf(const FunctionDecl *current) const {
  std::string member = this->checker.thisMember_.empty() ? "x" : this->checker.thisMember_.str();
  const FunctionDecl *source = current ? current->thisSource() : nullptr;
  if (!source && this->checker.staticInit_ && this->checker.currentClass_) {
    return err(ClassCheckerThisOfError{
        .code = ClassCheckerThisOfErrorCode::IN_STATIC_FIELD,
        .message = "in a static field 'this' would be the class itself; use the "
                   "class's name: '" +
                   this->checker.currentClass_->name.str() + "." + member + "'",
    });
  }
  if (!source) {
    return err(ClassCheckerThisOfError{
        .code = ClassCheckerThisOfErrorCode::OUTSIDE_METHOD,
        .message = "'this' is used outside of a method; it exists in the methods "
                   "of classes and object literals",
    });
  }

  bool hasThis = source->takesThis() || source->hasThisParam();
  if (!hasThis && source->cls && source->isStatic) {
    return err(ClassCheckerThisOfError{
        .code = ClassCheckerThisOfErrorCode::IN_STATIC_METHOD,
        .message = "in a static method 'this' would be the class itself; use the "
                   "class's name: '" +
                   source->cls->name.str() + "." + member + "'",
    });
  }
  if (!hasThis) {
    return err(ClassCheckerThisOfError{
        .code = ClassCheckerThisOfErrorCode::IN_PLAIN_FUNCTION,
        .message = "a 'function' has no 'this' of its own; use an arrow function, "
                   "which sees the 'this' of the method around it",
    });
  }

  return ok(source);
}

ClassChecker::ResultThisReadiness ClassChecker::readyThis(VarSymbol *var) const {
  bool inInit = var == this->checker.initThis_;
  bool inConstructor = inInit && this->checker.currentDecl() == this->checker.initFn_;
  bool inClosure = inInit && this->checker.currentDecl() != this->checker.initFn_;
  if (inClosure && !this->checker.closureThisReady_) {
    return err(ClassCheckerReadyThisError{
        .code = ClassCheckerReadyThisErrorCode::IN_CLOSURE_BEFORE_FIELDS_SET,
        .message = "this function uses 'this', and it is created before every field "
                   "is set; create it after the constructor sets them",
    });
  }
  if (inConstructor && this->checker.flow_.beforeSuper) {
    return err(ClassCheckerReadyThisError{
        .code = ClassCheckerReadyThisErrorCode::BEFORE_SUPER,
        .message = "'this' cannot be used before 'super(...)': the object exists once "
                   "the constructor of '" +
                   this->checker.initFn_->cls->parent->name.str() + "' has run",
    });
  }

  bool fieldsMatter = inConstructor && this->checker.thisMember_.empty();
  ResultFieldUnsetYet unset = firstUnset(this->checker.flow_);
  if (fieldsMatter && unset.isOk()) {
    return err(ClassCheckerReadyThisError{
        .code = ClassCheckerReadyThisErrorCode::BEFORE_FIELDS_SET,
        .message = "'this' is used before every field is set: field '" + unset.value() +
                   "' is not set yet; until then 'this' can only set fields "
                   "and read the ones set",
    });
  }

  return ok(var);
}

ClassChecker::ResultThisReference ClassChecker::checkThis(NameExpr *name) {
  name->type = Type::error();
  ResultThisComesFrom found = thisOf(this->checker.currentDecl());
  if (found.isErr()) {
    return err(ClassCheckerCheckThisError{
        .code = ClassCheckerCheckThisErrorCode::THIS_NOT_FOUND,
        .message = found.error().message,
    });
  }

  const FunctionDecl *source = found.value();
  VarSymbol *var = source->takesThis() ? source->thisVar : source->params[0].sym;
  if (!var) {
    return err(ClassCheckerCheckThisError{
        .code = ClassCheckerCheckThisErrorCode::THIS_NOT_DECLARED,
        .message = "internal error: 'this' of '" + source->name.str() +
                   "' is used before the method is checked",
    });
  }

  name->var = var;
  this->checker.nestedFunctions().noteUse(var, name->loc());
  name->type = var->type;
  ResultThisReadiness ready = readyThis(var);
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
