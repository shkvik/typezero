#include "compiler/pipeline/check_program/topics/classes/decorator_checker/decorator_checker.h"
#include "compiler/pipeline/check_program/declare/class_declarer/class_declarer.h"
#include "compiler/pipeline/check_program/infer/return_inferrer/return_inferrer.h"
#include "compiler/pipeline/check_program/instances/late_classes/late_classes.h"
#include "compiler/pipeline/check_program/scopes/scopes.h"
#include "compiler/pipeline/check_program/topics/classes/class_value_checker/class_value_checker.h"
#include "compiler/pipeline/check_program/topics/expr/call_checker/call_checker.h"
#include "compiler/pipeline/check_program/topics/expr/expr_checker/expr_checker.h"
#include "compiler/pipeline/check_program/topics/reflect/metadata_checker/metadata_checker.h"
#include "compiler/pipeline/check_program/topics/stmt/stmt_checker/stmt_checker.h"
#include "compiler/pipeline/check_program/types/type_relations/type_relations.h"
#include "compiler/pipeline/check_program/whole_program/flows/flows.h"
#include "llvm/ADT/STLExtras.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

size_t DecoratorChecker::argumentsOf(Place place) {
  switch (place) {
  case Place::Class:
    return 1;
  case Place::Field:
    return 2;
  default:
    return 3;
  }
}

std::string DecoratorChecker::placeText(Place place) {
  switch (place) {
  case Place::Class:
    return ErrorTextDecoratorPlace::aClass();
  case Place::Field:
    return ErrorTextDecoratorPlace::aField();
  case Place::Method:
    return ErrorTextDecoratorPlace::aMethod();
  default:
    return ErrorTextDecoratorPlace::aParameter();
  }
}

std::string DecoratorChecker::argumentsText(Place place) {
  switch (place) {
  case Place::Class:
    return ErrorTextDecoratorPlace::classArgs();
  case Place::Field:
    return ErrorTextDecoratorPlace::fieldArgs();
  case Place::Method:
    return ErrorTextDecoratorPlace::methodArgs();
  default:
    return ErrorTextDecoratorPlace::paramArgs();
  }
}

std::string DecoratorChecker::decoratorName(const Expr *expr) {
  if (auto *call = dyn_cast<CallExpr>(expr)) {
    return DecoratorChecker::decoratorName(call->callee);
  }
  if (auto *name = dyn_cast<NameExpr>(expr)) {
    return name->name.starts_with("%") ? "" : name->name.str();
  }
  if (auto *member = dyn_cast<MemberExpr>(expr)) {
    std::string object = DecoratorChecker::decoratorName(member->object);
    return object.empty() ? "" : object + "." + member->name.str();
  }

  return "";
}

bool DecoratorChecker::isPlainReference(const Expr *expr) {
  if (isa<NameExpr>(expr)) {
    return true;
  }
  if (auto *member = dyn_cast<MemberExpr>(expr)) {
    return DecoratorChecker::isPlainReference(member->object);
  }

  return false;
}

bool DecoratorChecker::decoratedParams(const FunctionDecl *fn) {
  return llvm::any_of(fn->params, [](const Param &p) { return !p.decorators.empty(); });
}

DecoratorChecker::DecoratorChecker(CheckState &owner)
    : state(owner) {}

const Type *DecoratorChecker::methodDescriptorType(const Type *args, const Type *ret) {
  const Type *value = Type::function({Type::unknown(), args}, 1, true, ret, true);
  const Type *flag = Type::boolTy();
  const Type *type =
      Type::object({{"value", value, false},
                    {"writable", flag, false},
                    {"enumerable", flag, false},
                    {"configurable", flag, false}},
                   "MethodDescriptor<" + args->name().str() + ", " + ret->name().str() + ">");
  this->state.getAliasStore().rememberDescriptorOrigin(type, args, ret);

  return type;
}

bool DecoratorChecker::decoratorParams(Expr *callee, std::vector<const Type *> &params,
                                       size_t &required) {
  const FuncSymbol *sym = nullptr;
  if (auto *name = dyn_cast<NameExpr>(callee)) {
    ScopeEntry entry = Scopes(this->state).lookup(name->name);
    if (entry.var()) {
      const Type *type = entry.var()->type;
      if (!type || !type->isFunction() || type->hasRest()) {
        return false;
      }
      params.assign(type->params().begin(), type->params().end());
      required = type->minArgs();
      return true;
    }
    sym = entry.func();
  } else if (auto *member = dyn_cast<MemberExpr>(callee)) {
    auto *object = dyn_cast<NameExpr>(member->object);
    if (ClassDecl *cls = object ? Scopes(this->state).lookup(object->name).cls() : nullptr) {
      std::vector<FunctionDecl *> methods = ClassDeclarer::methodsNamed(cls, member->name, true);
      if (methods.size() == 1) {
        sym = methods[0]->sym;
      }
    }
  }
  if (!sym) {
    return false;
  }
  ReturnInferrer(this->state).resolveParams(const_cast<FuncSymbol *>(sym));
  if (sym->hasRest || llvm::is_contained(sym->params, nullptr)) {
    return false;
  }
  params = sym->params;
  required = sym->minArgs;

  return true;
}

DecoratorChecker::ResultNamesAreDistinct DecoratorChecker::namesDistinct(const DecoratedName &a,
                                                                         const DecoratedName &b,
                                                                         llvm::StringRef cls) {
  if (a.name == b.name) {
    std::string what = a.isStatic != b.isStatic ? ErrorTextDecoratedNames::bothKinds(a.name)
                                                : ErrorTextDecoratedNames::variants(a.name);
    std::string shared;
    for (const std::string &d : b.decorators) {
      if (!d.empty() && llvm::is_contained(a.decorators, d)) {
        shared = d;
      }
    }
    std::string records = shared.empty() ? ErrorTextDecoratedNames::design()
                                         : ErrorTextDecoratedNames::recordsOf(shared);
    std::string message = ErrorTextDecoratedNames::shared(what, cls, records);

    return err(DecoratorCheckerNamesDistinctError{
        .code = DecoratorCheckerNamesDistinctErrorCode::SAME_NAME,
        .message = message,
    });
  }

  return ok(&b);
}

DecoratorChecker::ResultFirstConstructor
DecoratorChecker::secondConstructor(const ClassDecl *cls, const FunctionDecl *first) {
  if (first) {
    std::string message = ErrorTextDecoratedNames::sameRecord(cls->name);

    return err(DecoratorCheckerSecondConstructorError{
        .code = DecoratorCheckerSecondConstructorErrorCode::TWO_DECORATED_CONSTRUCTORS,
        .message = message,
    });
  }

  return ok(cls);
}

void DecoratorChecker::checkDecoratedNames(ClassDecl *cls) {
  std::vector<DecoratedName> members;
  auto add = [&](llvm::StringRef name, llvm::SMLoc loc, bool isStatic,
                 llvm::ArrayRef<Decorator> decorators, const FunctionDecl *fn) {
    DecoratedName m{name, loc, isStatic, {}};
    for (const Decorator &d : decorators) {
      m.decorators.push_back(DecoratorChecker::decoratorName(d.expr));
    }
    if (fn) {
      for (const Param &p : fn->params) {
        for (const Decorator &d : p.decorators) {
          m.decorators.push_back(DecoratorChecker::decoratorName(d.expr));
        }
      }
    }
    if (!m.decorators.empty()) {
      members.push_back(std::move(m));
    }
  };
  for (const ClassField &f : cls->fields) {
    add(f.name, f.loc, f.isStatic(), f.decorators, nullptr);
  }
  for (const FunctionDecl *fn : cls->methods) {
    add(fn->name, fn->loc(), fn->isStatic(), fn->decorators(), fn);
  }
  for (size_t i = 0; i < members.size(); ++i) {
    for (size_t j = 0; j < i; ++j) {
      const DecoratedName &a = members[j];
      const DecoratedName &b = members[i];
      ResultNamesAreDistinct distinct = DecoratorChecker::namesDistinct(a, b, cls->name);
      if (distinct.isOk()) {
        continue;
      }
      this->state.getDiagnostics().error(b.loc, distinct.error().message);
      std::string other = ErrorTextDecoratedNames::other();
      this->state.getDiagnostics().note(a.loc, other);
      break;
    }
  }
  const FunctionDecl *first = nullptr;
  for (const FunctionDecl *ctor : cls->constructors) {
    if (!DecoratorChecker::decoratedParams(ctor)) {
      continue;
    }
    ResultFirstConstructor only = DecoratorChecker::secondConstructor(cls, first);
    if (only.isOk()) {
      first = ctor;
      continue;
    }
    this->state.getDiagnostics().error(ctor->loc(), only.error().message);
    std::string other = ErrorTextDecoratedNames::other();
    this->state.getDiagnostics().note(first->loc(), other);
    break;
  }
}

void DecoratorChecker::decorateClass(ClassDecl *cls) {
  this->checkDecoratedNames(cls);
  struct Member {
    ClassField *field;
    FunctionDecl *method;
    bool isStatic;
    const char *at;
  };
  std::vector<Member> members;
  for (ClassField &f : cls->fields) {
    if (!f.decorators.empty()) {
      members.push_back({&f, nullptr, f.isStatic(), f.loc.getPointer()});
    }
  }
  for (FunctionDecl *fn : cls->methods) {
    if (!fn->decorators().empty() || DecoratorChecker::decoratedParams(fn)) {
      members.push_back({nullptr, fn, fn->isStatic(), fn->loc().getPointer()});
    }
  }
  llvm::stable_sort(members, [](const Member &a, const Member &b) {
    return std::make_pair(a.isStatic, a.at) < std::make_pair(b.isStatic, b.at);
  });
  for (Member &m : members) {
    std::vector<DecoratorUse> uses;
    for (const Decorator &d : m.field ? m.field->decorators : m.method->decorators()) {
      uses.push_back({&d});
    }
    if (m.method) {
      for (size_t i = 0; i < m.method->params.size(); ++i) {
        for (const Decorator &d : m.method->params[i].decorators) {
          uses.push_back({&d, int(i), m.method});
        }
      }
    }
    this->decorate(cls, m.field, m.method, uses);
  }
  std::vector<DecoratorUse> uses;
  for (const Decorator &d : cls->decorators) {
    uses.push_back({&d});
  }
  for (const FunctionDecl *ctor : cls->constructors) {
    for (size_t i = 0; i < ctor->params.size(); ++i) {
      for (const Decorator &d : ctor->params[i].decorators) {
        uses.push_back({&d, int(i), ctor});
      }
    }
  }
  this->decorate(cls, nullptr, nullptr, uses);
}

DecoratorChecker::ResultPlainMemberFound
DecoratorChecker::memberDecoratable(const ClassField *field, const FunctionDecl *method) {
  if (field && field->accessor) {
    std::string message = ErrorTextDecorate::accessor();

    return err(DecoratorCheckerMemberDecoratableError{
        .code = DecoratorCheckerMemberDecoratableErrorCode::ACCESSOR_FIELD,
        .message = message,
    });
  }
  if (method && method->isAbstract()) {
    std::string message = ErrorTextDecorate::abstract();

    return err(DecoratorCheckerMemberDecoratableError{
        .code = DecoratorCheckerMemberDecoratableErrorCode::ABSTRACT_METHOD,
        .message = message,
    });
  }

  return ok(method);
}

DecoratorChecker::ResultDecoratorArgsFit DecoratorChecker::decoratorFits(llvm::StringRef label,
                                                                         Place place, size_t needs,
                                                                         size_t given,
                                                                         const Type *third) {
  if (needs > given) {
    std::string needed = std::to_string(needs);
    std::string gets = std::to_string(given);
    std::string where = DecoratorChecker::placeText(place);
    std::string args = DecoratorChecker::argumentsText(place);
    std::string message = ErrorTextDecorate::arguments(label, where, needed, gets, args);

    return err(DecoratorCheckerDecoratorFitsError{
        .code = DecoratorCheckerDecoratorFitsErrorCode::TOO_MANY_ARGUMENTS,
        .message = message,
    });
  }
  if (third && place == Place::Parameter && third->isObject() && third->field("value")) {
    std::string message = ErrorTextDecorate::descriptor(label);

    return err(DecoratorCheckerDecoratorFitsError{
        .code = DecoratorCheckerDecoratorFitsErrorCode::DESCRIPTOR_FOR_PARAMETER,
        .message = message,
    });
  }
  if (third && place == Place::Method && third->isNumeric()) {
    std::string message = ErrorTextDecorate::position(label);

    return err(DecoratorCheckerDecoratorFitsError{
        .code = DecoratorCheckerDecoratorFitsErrorCode::POSITION_FOR_METHOD,
        .message = message,
    });
  }

  return ok(needs);
}

DecoratorChecker::ResultDescriptorUsable DecoratorChecker::genericDescriptor(const ClassDecl *cls,
                                                                             bool descriptor) {
  if (descriptor && cls->isGeneric()) {
    std::string message = ErrorTextDecorate::generic(cls->name);

    return err(DecoratorCheckerGenericDescriptorError{
        .code = DecoratorCheckerGenericDescriptorErrorCode::GENERIC_CLASS_DESCRIPTOR,
        .message = message,
    });
  }

  return ok(cls);
}

DecoratorChecker::ResultMetadataArgsPair DecoratorChecker::metadataArgs(const CallExpr *metadata) {
  if (metadata->args.size() != 2 || metadata->hasSpread()) {
    std::string message = ErrorTextDecorate::metadata();

    return err(DecoratorCheckerMetadataArgsError{
        .code = DecoratorCheckerMetadataArgsErrorCode::WRONG_METADATA_ARGUMENTS,
        .message = message,
    });
  }

  return ok(metadata);
}

DecoratorChecker::ResultDecoratorReturns
DecoratorChecker::decoratorResult(llvm::StringRef label, const Type *result, Place place,
                                  bool descriptor, const ClassDecl *current) {
  bool nothing = result->isVoid() || result->isUndefined() || result->isError();
  if (!nothing && place == Place::Class && result->isConstructor()) {
    std::string message = ErrorTextDecoratorResult::runtime(label, result->name(), current->name);

    return err(DecoratorCheckerDecoratorResultError{
        .code = DecoratorCheckerDecoratorResultErrorCode::RUNTIME_CLASS,
        .message = message,
    });
  }
  if (!nothing && place == Place::Class) {
    std::string message = ErrorTextDecoratorResult::forClass(label, result->name());

    return err(DecoratorCheckerDecoratorResultError{
        .code = DecoratorCheckerDecoratorResultErrorCode::CLASS_RESULT,
        .message = message,
    });
  }
  if (!nothing && place == Place::Method && descriptor) {
    std::string message = ErrorTextDecoratorResult::forMethod(label, result->name());

    return err(DecoratorCheckerDecoratorResultError{
        .code = DecoratorCheckerDecoratorResultErrorCode::METHOD_RESULT,
        .message = message,
    });
  }
  if (!nothing) {
    std::string where = DecoratorChecker::placeText(place);
    std::string message = ErrorTextDecoratorResult::other(label, result->name(), where);

    return err(DecoratorCheckerDecoratorResultError{
        .code = DecoratorCheckerDecoratorResultErrorCode::OTHER_RESULT,
        .message = message,
    });
  }

  return ok(result);
}

void DecoratorChecker::decorate(ClassDecl *cls, ClassField *field, FunctionDecl *method,
                                llvm::ArrayRef<DecoratorUse> uses) {
  if (uses.empty()) {
    return;
  }
  std::vector<Stmt *> &out = cls->decorations;
  Place own = field ? Place::Field : method ? Place::Method : Place::Class;
  llvm::StringRef memberName = field ? field->name : method ? method->name : llvm::StringRef();
  llvm::SMLoc at = uses[0].decorator->loc;
  ResultPlainMemberFound plain = DecoratorChecker::memberDecoratable(field, method);
  if (plain.isErr()) {
    this->state.getDiagnostics().error(at, plain.error().message);
    return;
  }

  auto isMetadata = [&](const Expr *expr) {
    auto *call = dyn_cast<CallExpr>(expr);
    auto *member = call ? dyn_cast<MemberExpr>(call->callee) : nullptr;

    return member && member->name == "metadata" &&
           CallChecker(this->state).isGlobalName(member->object, "Reflect");
  };
  const Type *earlyDescriptor = nullptr;
  auto methodDescriptor = [&]() -> const Type * {
    if (earlyDescriptor || !method) {
      return earlyDescriptor;
    }
    FuncSymbol *sym = method->sym;
    ReturnInferrer(this->state).resolveParams(sym);
    const Type *ret = ReturnInferrer(this->state).returnType(sym);
    if (!ret || ret->isError() || llvm::is_contained(sym->params, nullptr)) {
      return nullptr;
    }
    earlyDescriptor = this->methodDescriptorType(Type::tuple(sym->params), ret);
    return earlyDescriptor;
  };
  auto factoryHint = [&](const Expr *expr) -> const Type * {
    auto *call = dyn_cast<CallExpr>(expr);
    auto *name = call ? dyn_cast<NameExpr>(call->callee) : nullptr;
    if (!name || !name->typeArgs.empty() || own != Place::Method || cls->isGeneric()) {
      return nullptr;
    }
    const FuncSymbol *factory = Scopes(this->state).lookup(name->name).func();
    if (!factory || !factory->decl || factory->decl->typeParams.empty()) {
      return nullptr;
    }
    const Type *descriptorType = methodDescriptor();
    if (!descriptorType) {
      return nullptr;
    }

    return Type::function({Type::classRef(cls, cls->name), Type::string(), descriptorType}, 3,
                          false, Type::voidTy());
  };
  std::vector<Expr *> callees;
  for (const DecoratorUse &use : uses) {
    Expr *expr = use.decorator->expr;
    if (isMetadata(expr)) {
      callees.push_back(nullptr);
      continue;
    }
    if (DecoratorChecker::isPlainReference(expr)) {
      callees.push_back(expr);
      continue;
    }
    llvm::StringRef hidden = this->state.getAst().hiddenName();
    auto *decl = this->state.getAst().make<VarDeclStmt>(use.decorator->loc, true);
    VarDeclarator d;
    d.name = hidden;
    d.loc = use.decorator->loc;
    d.init = expr;
    if (const Type *hint = use.param < 0 ? factoryHint(expr) : nullptr) {
      d.init = ExprChecker(this->state).checkExpr(expr, hint);
      d.sym = Scopes(this->state).declareVar(hidden, d.loc, d.init->type, true);
      decl->decls.push_back(std::move(d));
    } else {
      decl->decls.push_back(std::move(d));
      StmtChecker(this->state).checkStmt(decl);
    }
    out.push_back(decl);
    callees.push_back(this->state.getAst().make<NameExpr>(use.decorator->loc, hidden));
  }

  std::vector<size_t> takes;
  bool descriptor = false;
  bool fitting = true;
  for (size_t i = 0; i < uses.size(); ++i) {
    Place place = uses[i].param >= 0 ? Place::Parameter : own;
    size_t given = DecoratorChecker::argumentsOf(place);
    if (!callees[i]) {
      takes.push_back(0);
      continue;
    }
    std::vector<const Type *> params;
    size_t required = 0;
    bool known = this->decoratorParams(callees[i], params, required);
    size_t count = known ? params.size() : given;
    size_t needs = known ? std::min(required, count) : given;
    std::string name = DecoratorChecker::decoratorName(uses[i].decorator->expr);
    std::string label = ErrorTextDecoratorPlace::label(name);
    llvm::SMLoc loc = uses[i].decorator->loc;
    const Type *third = known && params.size() >= 3 ? params[2]->nonOptional() : nullptr;
    ResultDecoratorArgsFit fits =
        DecoratorChecker::decoratorFits(label, place, needs, given, third);
    if (fits.isErr()) {
      this->state.getDiagnostics().error(loc, fits.error().message);
      fitting = false;
    }
    takes.push_back(std::min(count, given));
    descriptor |= place == Place::Method && count >= 3;
  }
  if (!fitting) {
    return;
  }
  ResultDescriptorUsable usable = DecoratorChecker::genericDescriptor(cls, descriptor);
  if (usable.isErr()) {
    this->state.getDiagnostics().error(at, usable.error().message);
    return;
  }

  VarSymbol *descVar = nullptr;
  const Type *descType = nullptr;
  if (descriptor) {
    FuncSymbol *sym = method->sym;
    ReturnInferrer(this->state).resolveParams(sym);
    const Type *ret = ReturnInferrer(this->state).returnType(sym);
    if (!ret || ret->isError() || llvm::is_contained(sym->params, nullptr)) {
      return;
    }
    descType = earlyDescriptor ? earlyDescriptor
                               : this->methodDescriptorType(Type::tuple(sym->params), ret);
    VarSymbol *impl = this->state.getAst().newVar();
    impl->name = this->state.getAst().keepName("%" + cls->name.str() + "." + method->name.str() +
                                               "." + std::to_string(method->variant()));
    impl->loc = method->loc();
    impl->type = descType->field("value")->type;
    impl->place = GlobalVar{.usedInFunction = true};
    impl->reassigned = true;
    Scopes::ResultEntryDeclared added =
        Scopes(this->state).declare(impl->name, {VarEntry{.var = impl}});
    if (added.isErr()) {
      this->state.getDiagnostics().error(impl->loc, added.error().message);
    }
    if (added.isErr() && added.error().code != ScopesDeclareErrorCode::UNDEFINED_AS_NAME) {
      ScopeEntry previousEntry = Scopes(this->state).lookup(impl->name);
      std::string declaredHere = ErrorTextDeclare::previous();
      this->state.getDiagnostics().note(previousEntry.loc(), declaredHere);
    }
    method->member()->impl = impl;
    auto *lit = this->state.getAst().make<ObjectLitExpr>(at);
    auto prop = [&](llvm::StringRef name, Expr *value) {
      ObjectLitExpr::Property p;
      p.name = name;
      p.loc = at;
      p.value = value;
      lit->props.push_back(p);
    };
    prop("value", this->state.getAst().make<NameExpr>(at, impl->name));
    prop("writable", this->state.getAst().make<BoolLitExpr>(at, true));
    prop("enumerable", this->state.getAst().make<BoolLitExpr>(at, false));
    prop("configurable", this->state.getAst().make<BoolLitExpr>(at, true));
    Expr *init = TypeRelations(this->state)
                     .coerce(ExprChecker(this->state).checkExpr(lit, descType), descType);
    descVar =
        Scopes(this->state).declareVar(this->state.getAst().hiddenName(), at, descType, false);
    descVar->reassigned = true;
    auto *decl = this->state.getAst().make<VarDeclStmt>(at, false);
    VarDeclarator d;
    d.name = descVar->name;
    d.loc = at;
    d.init = init;
    d.sym = descVar;
    decl->decls.push_back(std::move(d));
    out.push_back(decl);
  }

  const FunctionDecl *ctor = nullptr;
  if (own == Place::Class) {
    for (const FunctionDecl *c : cls->constructors) {
      if (DecoratorChecker::decoratedParams(c) || cls->constructors.size() == 1) {
        ctor = ctor ? nullptr : c;
      }
    }
  }
  MetadataChecker(this->state).writeDesignRecords(cls, field, method, ctor, at);

  ClassDecl *current = cls;
  for (size_t i = uses.size(); i-- > 0;) {
    const DecoratorUse &use = uses[i];
    llvm::SMLoc loc = use.decorator->loc;
    Place place = use.param >= 0 ? Place::Parameter : own;
    if (!callees[i]) {
      auto *metadata = cast<CallExpr>(use.decorator->expr);
      ResultMetadataArgsPair pair = DecoratorChecker::metadataArgs(metadata);
      if (pair.isErr()) {
        this->state.getDiagnostics().error(loc, pair.error().message);
        continue;
      }
      auto *callee = this->state.getAst().make<MemberExpr>(
          loc, this->state.getAst().make<NameExpr>(loc, "Reflect"), "defineMetadata");
      std::vector<Expr *> args{metadata->args[0], metadata->args[1],
                               ClassValueChecker(this->state).classValueAt(cls, loc)};
      llvm::StringRef name = use.param >= 0 ? use.owner->name : memberName;
      if (place != Place::Class && !(use.param >= 0 && use.owner->isConstructor())) {
        args.push_back(this->state.getAst().make<StringLitExpr>(
            loc, std::u16string(name.begin(), name.end())));
      }
      out.push_back(this->state.getAst().make<ExprStmt>(
          loc, ExprChecker(this->state)
                   .checkExpr(this->state.getAst().make<CallExpr>(loc, callee, args))));
      continue;
    }
    std::vector<Expr *> args{
        ClassValueChecker(this->state).classValueAt(place == Place::Class ? current : cls, loc)};
    if (place != Place::Class) {
      llvm::StringRef name = use.param >= 0 ? use.owner->name : memberName;
      if (use.param >= 0 && use.owner->isConstructor()) {
        args.push_back(this->state.getAst().make<UndefinedExpr>(loc));
      } else {
        args.push_back(this->state.getAst().make<StringLitExpr>(
            loc, std::u16string(name.begin(), name.end())));
      }
    }
    if (place == Place::Parameter) {
      args.push_back(this->state.getAst().make<IntLitExpr>(loc, uint64_t(use.param)));
    } else if (place == Place::Method && descVar) {
      args.push_back(this->state.getAst().make<NameExpr>(loc, descVar->name));
    }
    args.resize(std::min(args.size(), takes[i]));
    auto *call = this->state.getAst().make<CallExpr>(loc, callees[i], std::move(args));
    Expr *applied = ExprChecker(this->state).checkExpr(call);
    const Type *result = applied->type;
    if (descVar && place == Place::Method && result == descType) {
      auto *target = this->state.getAst().make<NameExpr>(loc, descVar->name);
      target->binding = NamedVar{.var = descVar};
      target->type = descType;
      auto *assign = this->state.getAst().make<AssignExpr>(loc, std::nullopt, target, applied);
      assign->type = descType;
      applied = assign;
    } else if (place == Place::Class && result->classRef() &&
               result->classRef()->derivesFrom(current)) {
      if (ClassDecl *given = const_cast<ClassDecl *>(result->classRef()); given != current) {
        LateClasses(this->state).replaceClass(current, given, loc);
        current = given;
      }
    } else {
      std::string name = DecoratorChecker::decoratorName(use.decorator->expr);
      std::string label = ErrorTextDecoratorPlace::label(name);
      ResultDecoratorReturns returned =
          DecoratorChecker::decoratorResult(label, result, place, descVar != nullptr, current);
      if (returned.isErr()) {
        this->state.getDiagnostics().error(loc, returned.error().message);
      }
    }
    out.push_back(this->state.getAst().make<ExprStmt>(loc, applied));
  }
  if (descVar) {
    auto *target = this->state.getAst().make<NameExpr>(at, method->impl()->name);
    auto *value = this->state.getAst().make<MemberExpr>(
        at, this->state.getAst().make<NameExpr>(at, descVar->name), "value");
    auto *assign = this->state.getAst().make<AssignExpr>(at, std::nullopt, target, value);
    out.push_back(
        this->state.getAst().make<ExprStmt>(at, ExprChecker(this->state).checkExpr(assign)));
  }
}

}
