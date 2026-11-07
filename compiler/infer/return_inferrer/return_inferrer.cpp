#include "compiler/infer/return_inferrer/return_inferrer.h"
#include "compiler/check/async/async_checker/async_checker.h"
#include "compiler/check/classes/class_checker/class_checker.h"
#include "compiler/check/classes/class_links/class_links.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/functions/function_checker/function_checker.h"
#include "compiler/check/generics/generic_class_checker/generic_class_checker.h"
#include "compiler/check/generics/type_params/type_params.h"
#include "compiler/check/iterators/generator_checker/generator_checker.h"
#include "compiler/check/scopes/scopes.h"
#include "compiler/check/stmt/return_checker/return_checker.h"
#include "compiler/check/types/literal_types/literal_types.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/declare/import_binder/import_binder.h"
#include "compiler/declare/module_declarer/module_declarer.h"
#include "compiler/instances/late_classes/late_classes.h"
#include "compiler/load/lexer/lexer.h"
#include "compiler/load/parser/parser.h"

using llvm::cast;

namespace tz {

ReturnInferrer::ReturnInferrer(Checker &owner)
    : checker(owner) {}

bool ReturnInferrer::returnsValue(const Stmt *stmt) {
  if (!stmt) {
    return false;
  }
  auto caseReturns = [](const SwitchCase &c) {
    return llvm::any_of(c.body, ReturnInferrer::returnsValue);
  };
  switch (stmt->kind()) {
    case Stmt::Kind::Return: {
      const Expr *value = cast<ReturnStmt>(stmt)->value;
      auto *unary = llvm::dyn_cast_or_null<UnaryExpr>(value);
      return value && !(unary && unary->op == UnaryOp::Void);
    }
    case Stmt::Kind::Block:
      return llvm::any_of(cast<BlockStmt>(stmt)->body, ReturnInferrer::returnsValue);
    case Stmt::Kind::If: {
      const auto *branch = cast<IfStmt>(stmt);
      return ReturnInferrer::returnsValue(branch->thenStmt) ||
             ReturnInferrer::returnsValue(branch->elseStmt);
    }
    case Stmt::Kind::While:
      return ReturnInferrer::returnsValue(cast<WhileStmt>(stmt)->body);
    case Stmt::Kind::DoWhile:
      return ReturnInferrer::returnsValue(cast<DoWhileStmt>(stmt)->body);
    case Stmt::Kind::For:
      return ReturnInferrer::returnsValue(cast<ForStmt>(stmt)->body);
    case Stmt::Kind::ForOf:
      return ReturnInferrer::returnsValue(cast<ForOfStmt>(stmt)->body);
    case Stmt::Kind::Switch:
      return llvm::any_of(cast<SwitchStmt>(stmt)->cases, caseReturns);
    case Stmt::Kind::Labeled:
      return ReturnInferrer::returnsValue(cast<LabeledStmt>(stmt)->body);
    default:
      return false;
  }
}

ReturnInferrer::ResultNotCalledEarly ReturnInferrer::earlyCall(const FuncSymbol *fn) const {
  bool calledWhileLoading = fn->inferring && this->checker.suspended_.contains(fn);
  if (calledWhileLoading) {
    std::string message = ErrorTextEarlyCall::importBack(fn->name);

    return err(ReturnInferrerEarlyCallError{
        .code = ReturnInferrerEarlyCallErrorCode::CALLED_WHILE_LOADING,
        .message = message,
    });
  }

  return ok(fn);
}

const Type *ReturnInferrer::returnType(FuncSymbol *fn) {
  if (fn->ret) {
    return fn->ret;
  }
  if (this->checker.lateClasses().isTemplate(fn)) {
    return Type::unknown();
  }
  ResultNotCalledEarly early = this->earlyCall(fn);
  if (early.isErr()) {
    this->checker.diags_.error(fn->loc, early.error().message);
    this->checker.suspended_.erase(fn);
    return Type::error();
  }
  if (fn->inferring) {
    return fn->assumed ? fn->assumed : Type::error();
  }
  this->inferReturnType(fn);

  return fn->ret;
}

const Type *ReturnInferrer::functionType(FuncSymbol *fn) {
  this->resolveParams(fn);
  const Type *ret = this->returnType(fn);
  const Type *returned = ret ? ret : Type::error();

  return Type::function(fn->params, fn->minArgs, fn->hasRest, returned, fn->hasThis);
}

Checker::BodyState ReturnInferrer::enterBody(const FunctionDecl *fn) {
  Checker::BodyState saved;
  saved.flow = std::move(this->checker.flow_);
  saved.scopes = this->checker.scopes_;
  saved.typeScopes = this->checker.typeScopes_;
  saved.functions = this->checker.functions_;
  saved.groups = this->checker.groups_;
  saved.blockNames = this->checker.blockNames_;
  saved.function = this->checker.currentFunction_;
  saved.targets = std::move(this->checker.targets_);
  saved.pendingLabels = std::move(this->checker.pendingLabels_);
  saved.otherCaseVars = this->checker.otherCaseVars_;
  saved.chainShortCircuits = this->checker.chainShortCircuits_;
  saved.returns = this->checker.returns_;
  saved.yields = this->checker.yields_;
  saved.trial = this->checker.trial_;
  saved.early = this->checker.early_;
  saved.module = this->checker.module_;
  saved.instance = this->checker.checkingInstance_;
  saved.capture = this->checker.diags_.capture();
  saved.scopeBranches = this->checker.scopeBranches_;
  saved.branchDepth = this->checker.branchDepth_;
  this->checker.flow_ = Checker::Flow();
  NestedGroup *group = fn ? fn->group : nullptr;
  if (group) {
    this->checker.scopes_.resize(group->scopeDepth);
    this->checker.typeScopes_.resize(group->scopeDepth);
    this->checker.functions_.resize(group->functionDepth);
    while (!this->checker.groups_.empty() && this->checker.groups_.back() != group) {
      this->checker.groups_.pop_back();
    }
    while (!this->checker.blockNames_.empty() &&
           this->checker.blockNames_.back().functions > group->functionDepth) {
      this->checker.blockNames_.pop_back();
    }
  } else if (!fn || !fn->isExpression()) {
    this->checker.scopes_.resize(1);
    this->checker.typeScopes_.resize(1);
    this->checker.functions_.clear();
    this->checker.groups_.clear();
    this->checker.blockNames_.clear();
    this->checker.otherCaseVars_.clear();
    this->checker.trial_ = false;
    Module *home = fn ? this->checker.declModules_.lookup(fn) : nullptr;
    if (home) {
      this->checker.moduleDeclarer().useModule(home);
      this->checker.importBinder().bindImports();
    }
    this->checker.checkingInstance_ = nullptr;
    this->checker.diags_.setCapture(nullptr);
    Checker::Instance *owner =
        fn && fn->cls() ? this->checker.lateOwners_.lookup(fn->cls()) : nullptr;
    if (owner) {
      this->checker.checkingInstance_ = owner;
      this->checker.diags_.setCapture(&this->checker.lateErrors_[owner]);
    }
  }
  this->checker.scopeBranches_.resize(this->checker.scopes_.size(), 0);
  this->checker.branchDepth_ =
      this->checker.scopeBranches_.empty() ? 0 : this->checker.scopeBranches_.back();
  this->checker.currentFunction_ = nullptr;
  this->checker.targets_.clear();
  this->checker.pendingLabels_.clear();
  this->checker.chainShortCircuits_ = false;
  this->checker.returns_ = nullptr;
  this->checker.yields_ = nullptr;

  return saved;
}

void ReturnInferrer::leaveBody(Checker::BodyState &saved) {
  this->checker.flow_ = std::move(saved.flow);
  this->checker.scopeBranches_ = std::move(saved.scopeBranches);
  this->checker.branchDepth_ = saved.branchDepth;
  this->checker.scopes_ = std::move(saved.scopes);
  this->checker.typeScopes_ = std::move(saved.typeScopes);
  this->checker.functions_ = std::move(saved.functions);
  this->checker.groups_ = std::move(saved.groups);
  this->checker.blockNames_ = std::move(saved.blockNames);
  this->checker.currentFunction_ = saved.function;
  this->checker.targets_ = std::move(saved.targets);
  this->checker.pendingLabels_ = std::move(saved.pendingLabels);
  this->checker.otherCaseVars_ = std::move(saved.otherCaseVars);
  this->checker.chainShortCircuits_ = saved.chainShortCircuits;
  this->checker.returns_ = saved.returns;
  this->checker.yields_ = saved.yields;
  this->checker.trial_ = saved.trial;
  this->checker.early_ = saved.early;
  this->checker.module_ = saved.module;
  this->checker.checkingInstance_ = saved.instance;
  this->checker.diags_.setCapture(saved.capture);
}

const Type *ReturnInferrer::templateReturn(FuncSymbol *fn) {
  FunctionDecl *decl = fn->decl;
  FuncSymbol *generic = decl->generic->sym;
  const Type *ret = this->returnType(generic);
  if (!ret) {
    return Type::error();
  }
  const Type *instance = this->checker.genericClassChecker().instanceType(decl->cls(), ret);

  return this->checker.typeParams().substitute(instance, generic->typeParams, fn->typeParams);
}

void ReturnInferrer::inferReturnType(FuncSymbol *fn) {
  FunctionDecl *decl = fn->decl;
  bool fromTemplate = decl && decl->generic && decl->cls() && decl->cls()->generic() &&
                      !this->checker.lateClasses().hasClassExpr(decl->generic);
  if (fromTemplate) {
    fn->ret = this->templateReturn(fn);
    return;
  }
  const Type *inherited = this->checker.classLinks().inheritedReturnType(fn);
  if (inherited) {
    fn->ret = inherited;
    return;
  }

  Checker::BodyState saved = this->enterBody(decl);
  bool trialClosure = decl->isClosure() && saved.trial;
  bool muted = this->checker.diags_.setMuted(trialClosure);
  bool topLevelEarly = !decl->isClosure() && !this->checker.topLevelDone_;
  bool groupEarly = decl->group && !decl->group->finished;
  if (topLevelEarly || groupEarly) {
    this->checker.early_ = fn;
  }

  auto cycle = this->checker.cycleOf_.find(fn);
  if (cycle != this->checker.cycleOf_.end()) {
    this->inferCycle(this->checker.cycles_[cycle->second]);
  } else {
    this->checkAndInfer(decl);
  }

  this->checker.diags_.setMuted(muted);
  this->leaveBody(saved);
}

void ReturnInferrer::checkAndInfer(FunctionDecl *decl) {
  FuncSymbol *fn = decl->sym;
  std::vector<ReturnStmt *> returns;
  std::vector<YieldExpr *> yields;
  std::vector<YieldExpr *> *outerYields = this->checker.yields_;
  std::vector<ReturnStmt *> *outerReturns = this->checker.returns_;
  this->checker.returns_ = &returns;
  this->checker.yields_ = &yields;
  fn->inferring = true;
  this->checker.inferring_.push_back(fn);
  this->checker.functionChecker().checkFunctionBody(decl);
  this->checker.inferring_.pop_back();
  fn->inferring = false;
  this->checker.returns_ = outerReturns;
  this->checker.yields_ = outerYields;

  bool fallsOff = !ReturnChecker::alwaysReturns(decl->body);
  const Type *type = nullptr;
  if (decl->isGenerator) {
    type = this->checker.generatorChecker().inferGeneratorType(fn, yields, returns, fallsOff, true);
  } else if (decl->isAsync) {
    type = this->checker.asyncChecker().inferAsyncType(fn, returns, fallsOff, true);
  } else {
    type = this->unifyReturns(fn, returns, fallsOff, true);
  }
  fn->ret = type ? type : Type::error();
  if (decl->isAsync && !decl->isGenerator) {
    this->checker.asyncChecker().finishAsyncReturns(fn, returns);
    return;
  }
  this->convertReturns(decl, returns, yields);
}

void ReturnInferrer::convertReturns(const FunctionDecl *decl, llvm::ArrayRef<ReturnStmt *> returns,
                                    llvm::ArrayRef<YieldExpr *> yields) {
  FuncSymbol *fn = decl->sym;
  bool async = decl->isAsync;
  bool iterates = fn->ret->isIterator() || fn->ret->isAsyncIterator();
  const Type *returned = decl->isGenerator && iterates ? fn->ret->returned() : fn->ret;
  if (!returned->isError() && !returned->isVoid()) {
    for (ReturnStmt *ret : returns) {
      bool typed = ret->value && !ret->value->type->isError();
      bool settled = typed && async &&
                     AsyncChecker::awaitedType(ret->value->type) == ret->value->type &&
                     !ret->value->type->isUndefined();
      if (typed && (!async || settled)) {
        ret->value = this->checker.typeRelations().convert(ret->value, returned);
      }
    }
  }
  if (iterates) {
    const Type *yielded = fn->ret->yielded();
    for (YieldExpr *yield : yields) {
      bool typed = !yield->delegate && yield->value && !yield->value->type->isError();
      bool settled =
          typed && async && AsyncChecker::awaitedType(yield->value->type) == yield->value->type;
      if (typed && (!async || settled)) {
        yield->value = this->checker.typeRelations().convert(yield->value, yielded);
      }
    }
  }
}

const Type *ReturnInferrer::trialReturn(FuncSymbol *fn) {
  FunctionDecl *copy = this->freshCopy(fn->decl);
  std::vector<ReturnStmt *> returns;
  std::vector<YieldExpr *> yields;
  this->checker.returns_ = &returns;
  this->checker.yields_ = &yields;
  this->checker.trial_ = true;
  this->checker.flow_ = Checker::Flow();
  this->checker.functionChecker().checkFunctionBody(copy);
  this->checker.trial_ = false;
  this->checker.returns_ = nullptr;
  this->checker.yields_ = nullptr;

  bool fallsOff = !ReturnChecker::alwaysReturns(copy->body);
  const Type *found = nullptr;
  if (fn->decl->isGenerator) {
    found =
        this->checker.generatorChecker().inferGeneratorType(fn, yields, returns, fallsOff, false);
  } else if (fn->decl->isAsync) {
    found = this->checker.asyncChecker().inferAsyncType(fn, returns, fallsOff, false);
  } else {
    found = this->unifyReturns(fn, returns, fallsOff, false);
  }

  return found;
}

ReturnInferrer::ResultCycleTypeKnown ReturnInferrer::cycleType(const FuncSymbol *fn, int cycleSize,
                                                               bool failed) const {
  bool unknown = fn->ret->isError();
  if (unknown && failed) {
    std::string message = ErrorTextCycleType::differ(fn->name);

    return err(ReturnInferrerCycleTypeError{
        .code = ReturnInferrerCycleTypeErrorCode::DIFFERENT_TYPES,
        .message = message,
    });
  }
  if (unknown && cycleSize == 1) {
    std::string message = ErrorTextCycleType::selfOnly(fn->name);

    return err(ReturnInferrerCycleTypeError{
        .code = ReturnInferrerCycleTypeErrorCode::ONLY_SELF_CALLS,
        .message = message,
    });
  }
  if (unknown) {
    std::string message = ErrorTextCycleType::calledBack(fn->name);

    return err(ReturnInferrerCycleTypeError{
        .code = ReturnInferrerCycleTypeErrorCode::ONLY_CALLED_BACK,
        .message = message,
    });
  }

  return ok(fn->ret);
}

void ReturnInferrer::inferCycle(llvm::ArrayRef<FuncSymbol *> cycle) {
  for (FuncSymbol *fn : cycle) {
    fn->inferring = true;
  }
  this->checker.inferring_.insert(this->checker.inferring_.end(), cycle.begin(), cycle.end());
  bool muted = this->checker.diags_.setMuted(true);
  bool failed = false;
  for (int round = 0; round < 8 && !failed; ++round) {
    bool changed = false;
    for (FuncSymbol *fn : cycle) {
      const Type *found = this->trialReturn(fn);
      if (!found) {
        continue;
      }
      const Type *next = fn->assumed ? LiteralTypes::unionUnify(fn->assumed, found) : found;
      if (!next || next->isError()) {
        failed = true;
        break;
      }
      if (next != fn->assumed) {
        fn->assumed = next;
        changed = true;
      }
    }
    if (!changed) {
      break;
    }
  }
  this->checker.diags_.setMuted(muted);
  this->checker.inferring_.resize(this->checker.inferring_.size() - cycle.size());

  for (FuncSymbol *fn : cycle) {
    fn->inferring = false;
    fn->ret = fn->assumed && !failed ? fn->assumed : Type::error();
    fn->assumed = nullptr;
  }
  int cycleSize = int(cycle.size());
  for (FuncSymbol *fn : cycle) {
    ResultCycleTypeKnown known = this->cycleType(fn, cycleSize, failed);
    if (known.isErr()) {
      this->checker.diags_.error(fn->loc, known.error().message);
    }
  }
}

std::vector<ReturnStmt *> ReturnInferrer::literalsLast(llvm::ArrayRef<ReturnStmt *> returns) {
  std::vector<ReturnStmt *> ordered;
  std::vector<ReturnStmt *> literals;
  for (ReturnStmt *ret : returns) {
    bool literal = ret->value && ret->value->type && ret->value->type->isInteger() &&
                   TypeRelations::isIntLiteralTree(ret->value);
    if (literal) {
      literals.push_back(ret);
    } else {
      ordered.push_back(ret);
    }
  }
  ordered.insert(ordered.end(), literals.begin(), literals.end());

  return ordered;
}

ReturnInferrer::ResultReturnsUnified ReturnInferrer::commonReturn(const FuncSymbol *fn,
                                                                  const Type *before,
                                                                  const ReturnStmt *ret) const {
  const Type *found = ret->value->type;
  const Type *common = LiteralTypes::unionUnify(before, found);
  if (!common || common->isError()) {
    std::string message = ErrorTextCommonReturn::differ(fn->name, found->name(), before->name());

    return err(ReturnInferrerCommonReturnError{
        .code = ReturnInferrerCommonReturnErrorCode::DIFFERENT_TYPES,
        .message = message,
    });
  }

  return ok(common);
}

ReturnInferrer::ResultReturnsDefined
ReturnInferrer::definedReturn(const FuncSymbol *fn, llvm::ArrayRef<ReturnStmt *> returns,
                              bool undefined) const {
  auto withValue = [](const ReturnStmt *ret) { return ret->value != nullptr; };
  bool onlyUndefined = undefined && llvm::any_of(returns, withValue);
  if (onlyUndefined) {
    std::string message = ErrorTextDefinedReturn::undefined(fn->name);

    return err(ReturnInferrerDefinedReturnError{
        .code = ReturnInferrerDefinedReturnErrorCode::ONLY_UNDEFINED,
        .message = message,
    });
  }

  return ok(fn);
}

const Type *ReturnInferrer::unifyReturns(const FuncSymbol *fn, llvm::ArrayRef<ReturnStmt *> returns,
                                         bool fallsOff, bool report) {
  const Type *type = nullptr;
  bool undefined = fallsOff;
  std::vector<ReturnStmt *> ordered = ReturnInferrer::literalsLast(returns);
  for (ReturnStmt *ret : ordered) {
    if (!ret->value) {
      undefined = true;
      continue;
    }
    const Type *found = ret->value->type;
    if (found->isError()) {
      continue;
    }
    if (found->isUndefined()) {
      undefined = true;
      continue;
    }
    if (!type) {
      type = found;
      continue;
    }
    bool adapts = type != found && this->checker.typeRelations().literalAdapts(ret->value, type);
    if (adapts) {
      continue;
    }
    ResultReturnsUnified common = this->commonReturn(fn, type, ret);
    if (common.isErr() && report) {
      this->checker.diags_.error(ret->value->loc(), common.error().message);
    }
    if (common.isErr()) {
      return Type::error();
    }
    type = common.value();
  }
  if (!type) {
    ResultReturnsDefined defined = this->definedReturn(fn, returns, undefined);
    if (defined.isErr() && report) {
      this->checker.diags_.error(fn->loc, defined.error().message);
    }
    return report ? Type::error() : nullptr;
  }
  if (undefined && !type->isVoid()) {
    type = Type::optional(type);
  }

  return type;
}

const Type *ReturnInferrer::instanceParam(FuncSymbol *fn, const Type *type) {
  if (!type) {
    return nullptr;
  }
  FunctionDecl *decl = fn->decl;
  FuncSymbol *generic = decl->generic->sym;
  const Type *instance = this->checker.genericClassChecker().instanceValueType(decl->cls(), type);

  return this->checker.typeParams().substituteValue(instance, generic->typeParams, fn->typeParams);
}

ReturnInferrer::ResultParamTypeKnown ReturnInferrer::defaultType(const Param &param,
                                                                 const Type *found) const {
  const Type *type = found ? found : Type::error();
  if (type->isVoid() || type->isUndefined()) {
    std::string message = ErrorTextDefaultType::unknown(param.label());

    return err(ReturnInferrerDefaultTypeError{
        .code = ReturnInferrerDefaultTypeErrorCode::UNKNOWN_FROM_DEFAULT,
        .message = message,
    });
  }

  return ok(type);
}

void ReturnInferrer::resolveParams(FuncSymbol *fn) {
  if (!fn->pendingParams) {
    return;
  }
  fn->pendingParams = false;
  FunctionDecl *decl = fn->decl;
  FunctionDecl *generic = decl->generic;
  if (generic && decl->cls() && decl->cls()->generic()) {
    this->resolveParams(generic->sym);
    for (size_t i = 0; i < fn->params.size() && i < generic->sym->params.size(); ++i) {
      fn->params[i] = this->instanceParam(fn, generic->sym->params[i]);
    }
    for (size_t i = 0; i < decl->params.size() && i < generic->params.size(); ++i) {
      decl->params[i].declaredType = this->instanceParam(fn, generic->params[i].declaredType);
    }
    return;
  }

  std::vector<const Type *> found(decl->params.size(), nullptr);
  auto failed = [](const Type *type) { return type && type->isError(); };
  for (bool report : {false, true}) {
    if (report && llvm::none_of(found, failed)) {
      break;
    }
    Checker::BodyState saved = this->enterBody(decl);
    bool muted = this->checker.diags_.setMuted(!report);
    FunctionDecl *copy = this->freshCopy(decl);
    this->checker.functions_.push_back(copy);
    if (copy->takesThis()) {
      ClassChecker::ResultNewThisSymbol declared = this->checker.classChecker().declareThis(copy);
      if (declared.isErr()) {
        this->checker.diags_.error(copy->loc(), declared.error().message);
      }
    }
    ClassDecl *outerClass = this->checker.currentClass_;
    if (!copy->isClosure()) {
      this->checker.currentClass_ = copy->cls();
    }
    this->checker.pushScope();
    for (size_t i = 0; i < decl->params.size() && i < copy->params.size(); ++i) {
      Param &param = copy->params[i];
      const Type *type = decl->params[i].declaredType;
      if (!type) {
        Expr *value = this->checker.exprChecker().checkExpr(param.defaultValue());
        found[i] = value->type;
        type = value->type;
      }
      this->checker.scopes().declareVar(param.name, param.loc, type, false);
    }
    this->checker.popScope();
    this->checker.functions_.pop_back();
    this->checker.currentClass_ = outerClass;
    this->checker.diags_.setMuted(muted);
    this->leaveBody(saved);
  }

  for (size_t i = 0; i < decl->params.size(); ++i) {
    Param &param = decl->params[i];
    if (param.declaredType) {
      continue;
    }
    ResultParamTypeKnown known = this->defaultType(param, found[i]);
    if (known.isErr()) {
      this->checker.diags_.error(param.loc, known.error().message);
    }
    const Type *type = known.isOk() ? known.value() : Type::error();
    param.declaredType = type;
    fn->params[i] = Type::optional(type);
  }
}

FunctionDecl *ReturnInferrer::freshCopy(const FunctionDecl *fn) {
  Lexer lexer(fn->source, this->checker.diags_);
  bool library = fn->cls() && fn->cls()->library();
  Parser parser(lexer, this->checker.ctx_, this->checker.diags_, library);
  bool expression = fn->isArrow() || ((fn->isExpression() || fn->lifted()) && !fn->cls());
  FunctionDecl *copy = nullptr;
  if (expression) {
    copy = parser.parseFunctionExprCopy();
  } else if (fn->cls()) {
    copy = parser.parseMethodCopy();
  } else {
    copy = parser.parseFunctionCopy();
  }
  copy->place = fn->place;
  copy->isAsync = fn->isAsync;
  const ClassMember *original = fn->member();
  if (original) {
    ClassMember copied{.cls = original->cls, .access = original->access};
    if (fn->isStatic()) {
      copied.method = StaticMethod{};
    } else if (fn->isConstructor()) {
      copied.method = Constructor{};
    }
    copy->role = std::move(copied);
  }
  copy->sym = fn->sym;
  copy->name = fn->name;
  copy->typeArgs = fn->typeArgs;
  copy->isInstance = fn->isInstance;
  copy->parent = fn->parent;
  copy->group = fn->group;
  copy->var = fn->var;
  copy->creationSeq = fn->creationSeq;
  if (!fn->listName.empty() && copy->params.size() > fn->listStart) {
    copy->params.resize(fn->listStart);
    copy->listName = fn->listName;
    copy->listStart = fn->listStart;
    copy->listType = fn->listType;
  }
  for (size_t i = 0; i < copy->params.size() && i < fn->params.size(); ++i) {
    copy->params[i].declaredType = fn->params[i].declaredType;
  }
  for (size_t i = copy->params.size(); i < fn->params.size(); ++i) {
    copy->params.push_back(fn->params[i]);
  }

  return copy;
}

}
