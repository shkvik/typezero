#include "compiler/semantic/bodies/stmt/var_checker/var_checker.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/bodies/functions/closure_checker/closure_checker.h"
#include "compiler/semantic/bodies/functions/function_checker/function_checker.h"
#include "compiler/semantic/bodies/iterators/generator_checker/generator_checker.h"
#include "compiler/semantic/bodies/iterators/iterator_methods/iterator_methods.h"
#include "compiler/semantic/bodies/objects/dict_checker/dict_checker.h"
#include "compiler/semantic/bodies/objects/keyed_field_checker/keyed_field_checker.h"
#include "compiler/semantic/declare/constant_folder/constant_folder.h"
#include "compiler/semantic/flow/flow_checker/flow_checker.h"
#include "compiler/semantic/flow/index_proofs/index_proofs.h"
#include "compiler/semantic/generics/generic_checker/generic_checker.h"
#include "compiler/semantic/infer/call_cycles/call_cycles.h"
#include "compiler/semantic/library/async/timer_checker/timer_checker.h"
#include "compiler/semantic/library/regex/regex_checker/regex_checker.h"
#include "compiler/semantic/library/tz_modules/tz_modules_checker/tz_modules_checker.h"
#include "compiler/semantic/names/scopes/scopes.h"
#include "compiler/semantic/types/destination/destination.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "compiler/semantic/types/type_resolver/type_resolver.h"
#include "llvm/ADT/STLExtras.h"
#include <cmath>

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

bool VarChecker::isTextChoice(const Expr *expr) {
  while (auto *conv = dyn_cast<CastExpr>(expr)) {
    if (!conv->isImplicit()) {
      return false;
    }
    expr = conv->operand;
  }
  if (auto *choice = dyn_cast<ConditionalExpr>(expr)) {
    return VarChecker::isTextChoice(choice->thenExpr) && VarChecker::isTextChoice(choice->elseExpr);
  }

  return isa<StringLitExpr>(expr);
}

std::vector<const Stmt *> VarChecker::followingStatements(llvm::ArrayRef<Stmt *> stmts, size_t i) {
  std::vector<const Stmt *> following;
  for (size_t j = 0; j < i; ++j) {
    if (llvm::isa<FunctionDecl>(stmts[j])) {
      following.push_back(stmts[j]);
    }
  }
  following.insert(following.end(), stmts.begin() + i + 1, stmts.end());

  return following;
}

VarChecker::VarChecker(SemanticState &owner)
    : state(owner) {}

void VarChecker::checkVarDecl(VarDeclStmt *decl) {
  llvm::ArrayRef<Stmt *> block = this->state.getBlockPosition().getBlock();
  size_t index = this->state.getBlockPosition().getBlockIndex();
  this->state.getBlockPosition().forgetBlock();
  for (size_t k = 0; k < decl->decls.size(); ++k) {
    VarDeclarator &d = decl->decls[k];
    const Type *declared =
        d.typeAnn ? TypeResolver(this->state).resolveType(*d.typeAnn, false) : nullptr;
    if (auto *name = llvm::dyn_cast_or_null<NameExpr>(d.init);
        name && decl->isConst && !declared && !d.destructured() && name->typeArgs.empty()) {
      if (FuncSymbol *generic = GenericChecker(this->state).genericNamed(name)) {
        Scopes::ResultEntryDeclared added =
            Scopes(this->state).declare(d.name, {FunctionEntry{.func = generic}});
        if (added.isErr()) {
          this->state.getDiagnostics().error(d.loc, added.error().message);
        }
        if (added.isErr() && added.error().code != ScopesDeclareErrorCode::UNDEFINED_AS_NAME) {
          ScopeEntry previousEntry = Scopes(this->state).lookup(d.name);
          std::string declaredHere = ErrorTextDeclare::previous();
          this->state.getDiagnostics().note(previousEntry.loc(), declaredHere);
        }
        d.alias = true;
        continue;
      }
    }
    if (auto *fe = llvm::dyn_cast_or_null<FunctionExpr>(d.init);
        fe && !fe->fn->typeParams.empty() && decl->isConst && !declared && !d.destructured() &&
        this->state.getScopeStack().isTopLevel()) {
      FunctionDecl *fn = fe->fn;
      fn->place = FunctionPlace::LIFTED;
      fn->name = d.name;
      if (this->state.getModuleStates().getCurrentModule()) {
        this->state.getModuleStates().rememberDeclModule(
            fn, this->state.getModuleStates().getCurrentModule());
      }
      FunctionChecker(this->state).declareFunction(fn);
      CallCycles(this->state).findCycles({fn});
      this->state.getReturnInference().addLiftedFunction(fn);
      d.alias = true;
      continue;
    }
    if (auto *fe = llvm::dyn_cast_or_null<FunctionExpr>(d.init)) {
      if (fe->fn->name.empty()) {
        fe->fn->name = d.name;
      }
      if (decl->isConst && !d.destructured()) {
        VarSymbol *var = Scopes(this->state).declareVar(d.name, d.loc, nullptr, true);
        var->holds = Lambda{.lambda = fe->fn};
        d.sym = var;
        const Type *function =
            declared && declared->nonOptional()->isFunction() ? declared->nonOptional() : nullptr;
        Signature sig = function ? ClosureChecker::signatureOf(function) : Signature();
        d.init = ClosureChecker(this->state).checkFunctionExpr(fe, function ? &sig : nullptr);
        if (declared) {
          d.init = TypeRelations(this->state).coerce(d.init, declared);
        }
        if (auto *cast = dyn_cast<CastExpr>(d.init); cast && cast->adaptsResult) {
          var->holds = OrdinaryValue{};
        }
        var->type = declared ? declared : d.init->type;
        continue;
      }
    }
    const Type *type = declared;
    Expr *value = nullptr;
    bool untyped = false;
    const Type *hint = declared;
    std::vector<std::pair<const Type *, llvm::SMLoc>> places;
    if (!declared && d.init && !d.destructured() && Destination::literalShaped(d.init)) {
      this->state.getBlockPosition().rememberFollowing(
          block.empty() ? std::vector<const Stmt *>()
                        : VarChecker::followingStatements(block, index));
      hint = Destination(this->state)
                 .literalDestination(d, llvm::ArrayRef(decl->decls).drop_front(k + 1));
      places = this->state.getTypeContext().takeDestinationConflict();
    }
    VarSymbol *early = nullptr;
    if (const Type *timer = d.init && !d.destructured()
                                ? TimerChecker(this->state).timerCallType(d.init)
                                : nullptr) {
      early =
          Scopes(this->state).declareVar(d.name, d.loc, declared ? declared : timer, decl->isConst);
      if (LocalVarState *localState = early->local()) {
        localState->declSeq = ~0u;
      }
    }
    if (d.init) {
      if (!early) {
        this->state.getExpressionMarks().enterVariableInit(d.name);
      }
      if (d.destructured() && !d.objectPattern()) {
        IteratorMethods(this->state).markFused(d.init);
      }
      d.init = value = ExprChecker(this->state).checkExpr(d.init, hint);
      if (d.destructured() && !d.objectPattern()) {
        d.init = value = DictChecker(this->state).iterableOf(d.init);
      }
      if (!early) {
        this->state.getExpressionMarks().leaveVariableInit();
      }
      if (declared) {
        d.init = TypeRelations(this->state).coerce(d.init, declared);
      } else {
        type = d.init->type;
        ResultInitHasType typed = VarChecker::initHasType(decl, d, type);
        if (typed.isErr()) {
          this->state.getDiagnostics().error(d.init->loc(), typed.error().message);
          type = Type::error();
        } else if (type->isUndefined()) {
          untyped = true;
          type = Type::error();
        }
      }
    } else if (decl->isConst) {
      ResultConstValued valued = VarChecker::constValue(decl, d);
      if (valued.isErr()) {
        this->state.getDiagnostics().error(d.loc, valued.error().message);
      }
      if (!type) {
        type = Type::error();
      }
    } else if (!declared) {
      untyped = true;
      type = Type::error();
    }

    if (d.destructured() && d.objectPattern()) {
    } else if (d.destructured() && (type->isString() || type->isTypedArray()) && !declared) {
      auto *chars = this->state.getAst().make<ArrayLitExpr>(d.init->loc());
      chars->elements.push_back({d.init, true});
      chars->type = Type::array(type->isString() ? Type::string() : type->elementType());
      d.init = value = chars;
      type = chars->type;
    } else if (d.destructured() && type->isIterator() && !declared) {
      d.init = value = GeneratorChecker(this->state).destructuredIterator(d.init, d);
      type = d.init->type;
    } else {
      ResultDestructure shaped = VarChecker::destructurable(d, type);
      if (shaped.isErr()) {
        this->state.getDiagnostics().error(d.init ? d.init->loc() : d.loc, shaped.error().message);
        type = Type::error();
      }
    }

    if (early) {
      if (LocalVarState *localState = early->local()) {
        localState->declSeq = this->state.getReturnInference().takeSequenceNumber();
      }
      d.sym = early;
    } else {
      d.sym = Scopes(this->state).declareVar(d.name, d.loc, type, decl->isConst);
    }
    RegexChecker(this->state).noteRegexVariable(d.sym, value, decl->isConst, block, index);
    if (decl->isConst && !d.destructured()) {
      if (auto *got = llvm::dyn_cast_or_null<CallExpr>(value);
          got && got->builtin() == Builtin::ReflectMethod &&
          !this->state.getExpressionMarks().hasSeveralVariants(got)) {
        d.sym->holds = ReflectedMethod{.method = got->method()->decl};
      }
      if (auto *got = llvm::dyn_cast_or_null<CallExpr>(value);
          got && got->builtin() == Builtin::ReflectGet && got->args.size() == 2) {
        const Expr *key = got->args[1];
        while (auto *conv = llvm::dyn_cast<CastExpr>(key)) {
          if (!conv->isImplicit()) {
            break;
          }
          key = conv->operand;
        }
        std::vector<std::u16string> names;
        if (auto *lit = llvm::dyn_cast<StringLitExpr>(key)) {
          names.push_back(lit->value);
        } else if (const Type *keyType = key->type ? key->type->nonOptional() : nullptr) {
          std::vector<const Type *> members;
          if (keyType->isUnion()) {
            members.assign(keyType->members().begin(), keyType->members().end());
          } else {
            members.push_back(keyType);
          }
          for (const Type *m : members) {
            if (!m->isLiteral() || !m->literalBase()->isString()) {
              names.clear();
              break;
            }
            names.push_back(m->literalText());
          }
        }
        std::vector<llvm::StringRef> kept;
        for (const std::u16string &name : names) {
          kept.push_back(this->state.getAst().keepName(ToUtf8::toUtf8(name)));
        }
        if (!kept.empty()) {
          d.sym->holds = ReflectedNames{.names = std::move(kept)};
        }
      }
      auto *made = llvm::dyn_cast_or_null<CallExpr>(value);
      bool fresh =
          llvm::isa_and_nonnull<ArrayLitExpr>(value) ||
          (made &&
           (made->builtin() == Builtin::ArrMap || made->builtin() == Builtin::ArrFilter ||
            made->builtin() == Builtin::ArrSlice || made->builtin() == Builtin::ArrConcat ||
            made->builtin() == Builtin::ArrFlatMap || made->builtin() == Builtin::ArrayFrom));
      if (LocalVarState *localState = d.sym->local()) {
        localState->unshared =
            fresh && type->isArray() && FlowChecker::staysLocal(d.sym->owner(), d.name);
      }
    }
    if (decl->isConst && value &&
        (isa<ObjectLitExpr>(value) || isa<StringLitExpr>(value) ||
         (isa<ConditionalExpr>(value) && VarChecker::isTextChoice(value)))) {
      this->state.getConstantStore().rememberConstLiteral(d.sym, value);
      d.sym->literalConst = !declared && type->isString();
    }
    if (auto *alias = dyn_cast_or_null<NameExpr>(value); decl->isConst && alias && alias->var() &&
                                                         alias->var()->literalConst && !declared &&
                                                         type->isString()) {
      const Expr *literal = this->state.getConstantStore().findConstLiteral(alias->var());
      this->state.getConstantStore().rememberConstLiteral(d.sym, literal);
      d.sym->literalConst = true;
    }
    if (!places.empty()) {
      this->state.getTypeContext().rememberDestinations(d.sym, std::move(places));
    }
    if (const Type *held = type->nonOptional();
        !d.destructured() && !this->state.getGenericStore().isCheckingInstance() &&
        (held->isResult() || (held->isPromise() && held->promised()->isResult())) &&
        !this->exported(d.name)) {
      this->state.getExpressionMarks().addResultVar(d.sym);
    }
    if (untyped) {
      this->noteUntyped(d.sym, d.init != nullptr);
    }
    if (value) {
      FlowChecker(this->state).assigned(d.sym, value);
    }
    if (const Type *base = type->nonOptional();
        decl->isConst && value && !d.destructured() &&
        (base->isByteView() || base->bytes() == Type::Bytes::ArrayBuffer)) {
      if (std::optional<Linear> length = IndexProofs(this->state).knownLength(value);
          length && length->term.isZero()) {
        this->state.getFlowState().rememberFixedLength(d.sym, length->offset);
      }
    }
    if (decl->isConst && d.init && type->isInteger()) {
      ConstantFolder(this->state).noteUnsignedVar(d.sym, d.init);
      if (std::optional<int64_t> constant = ConstantFolder(this->state).intConstant(d.init)) {
        this->state.getConstantStore().rememberConstVar(d.sym, *constant);
        d.sym->literalConst = !declared && type->isInt() && TypeRelations::isIntLiteralTree(d.init);
      }
    }
    if (decl->isConst && d.init && type->isString()) {
      if (std::optional<std::string> key = KeyedFieldChecker(this->state).constantKey(d.init)) {
        this->state.getConstantStore().rememberConstKey(d.sym, std::move(*key));
      }
      if (std::string prefix = TzModulesChecker(this->state).knownPrefix(d.init); !prefix.empty()) {
        this->state.getConstantStore().rememberConstPrefix(d.sym, std::move(prefix));
      }
    }
  }
}

VarChecker::ResultInitHasType VarChecker::initHasType(const VarDeclStmt *decl,
                                                      const VarDeclarator &d, const Type *type) {
  if (type->isVoid()) {
    std::string message = ErrorTextVarInit::isVoid();

    return err(VarCheckerInitHasTypeError{
        .code = VarCheckerInitHasTypeErrorCode::VOID_INITIALIZER,
        .message = message,
    });
  }
  if (type->isUndefined() && (decl->isConst || d.destructured())) {
    std::string message = ErrorTextVarInit::undefined(decl->isConst, d.name);

    return err(VarCheckerInitHasTypeError{
        .code = VarCheckerInitHasTypeErrorCode::UNDEFINED_INITIALIZER,
        .message = message,
    });
  }

  return ok(type);
}

VarChecker::ResultConstValued VarChecker::constValue(const VarDeclStmt *decl,
                                                     const VarDeclarator &d) {
  if (decl->isConst && !d.init) {
    std::string message = ErrorTextConstValue::missing();

    return err(VarCheckerConstValueError{
        .code = VarCheckerConstValueErrorCode::CONST_WITHOUT_VALUE,
        .message = message,
    });
  }

  return ok(&d);
}

VarChecker::ResultDestructure VarChecker::destructurable(const VarDeclarator &d, const Type *type) {
  bool fits = !d.destructured() || type->isArray() || type->isTuple() || type->isMatch() ||
              type->isError() || type->nonOptional()->isJson();
  if (!fits && type->isOptional() && type->inner()->isArray()) {
    std::string message = ErrorTextVarShape::mayBeUndef(type->name());

    return err(VarCheckerDestructurableError{
        .code = VarCheckerDestructurableErrorCode::ARRAY_MAY_BE_UNDEFINED,
        .message = message,
    });
  }
  if (!fits) {
    std::string message = ErrorTextVarShape::notArray(type->name());

    return err(VarCheckerDestructurableError{
        .code = VarCheckerDestructurableErrorCode::NOT_AN_ARRAY,
        .message = message,
    });
  }

  return ok(type);
}

VarChecker::ResultResultCheck VarChecker::resultChecked(const VarSymbol *var) {
  const Type *held = var->type->nonOptional();
  if (held->isPromise()) {
    std::string message = ErrorTextResultUse::promise(var->name);

    return err(VarCheckerResultCheckedError{
        .code = VarCheckerResultCheckedErrorCode::PROMISE_NEVER_AWAITED,
        .message = message,
    });
  }

  std::string message = ErrorTextResultUse::unchecked(var->name);

  return err(VarCheckerResultCheckedError{
      .code = VarCheckerResultCheckedErrorCode::RESULT_NEVER_CHECKED,
      .message = message,
  });
}

VarChecker::ResultVarHasTypes VarChecker::varTyped(const Untyped &u, const Type *type) {
  llvm::StringRef name = u.var->name;
  std::string hint;
  if (type && !type->isError() && !type->isUndefined()) {
    hint = ErrorTextVarTyped::writeType(name, type->name());
  } else if (u.initUndefined) {
    hint = ErrorTextVarTyped::addOne(name);
  } else {
    hint = ErrorTextVarTyped::example(name);
  }
  if (u.initUndefined) {
    std::string message = ErrorTextVarTyped::undefined(hint);

    return err(VarCheckerVarTypedError{
        .code = VarCheckerVarTypedErrorCode::UNDEFINED_ONLY,
        .message = message,
    });
  }

  std::string message = ErrorTextVarTyped::needsType(name, hint);

  return err(VarCheckerVarTypedError{
      .code = VarCheckerVarTypedErrorCode::NEEDS_TYPE,
      .message = message,
  });
}

bool VarChecker::exported(llvm::StringRef name) const {
  if (!this->state.getScopeStack().isTopLevel() ||
      !this->state.getModuleStates().getCurrentModule()) {
    return false;
  }

  return llvm::any_of(
      this->state.getModuleStates().getCurrentModule()->file.exports, [&](const ExportDecl &decl) {
        const LocalExport *local = decl.form.as<LocalExport>();

        return local &&
               llvm::any_of(local->names, [&](const ModuleName &n) { return n.name == name; });
      });
}

void VarChecker::reportUncheckedResults() {
  llvm::DenseMap<const char *, bool> readAt;
  std::vector<VarSymbol *> resultVars = this->state.getExpressionMarks().getResultVars();
  for (const VarSymbol *var : resultVars) {
    readAt[var->loc.getPointer()] |= var->read;
  }
  for (const VarSymbol *var : resultVars) {
    bool &done = readAt[var->loc.getPointer()];
    if (done) {
      continue;
    }
    done = true;
    ResultResultCheck checked = VarChecker::resultChecked(var);
    if (checked.isErr()) {
      this->state.getDiagnostics().error(var->loc, checked.error().message);
    }
  }
  this->state.getExpressionMarks().forgetResultVars();
}

void VarChecker::noteUntyped(VarSymbol *var, bool initUndefined) {
  this->state.getExpressionMarks().addUntyped(var, initUndefined);
}

void VarChecker::assignedUntyped(const VarSymbol *var, const Type *type) {
  if (!type->isError() && !type->isVoid()) {
    this->state.getExpressionMarks().addUntypedAssignment(var, type);
  }
}

void VarChecker::reportUntyped() {
  ExpressionMarks &marks = this->state.getExpressionMarks();
  for (const Untyped &u : marks.getUntyped()) {
    std::optional<ScopeEntry> found = this->state.getScopeStack().findInCurrentScope(u.var->name);
    if (!found || found->var() != u.var) {
      continue;
    }
    const Type *type = u.initUndefined ? Type::undefinedTy() : nullptr;
    for (const Type *t : u.assigned) {
      const Type *common = type ? TypeRelations::unify(type, t) : t;
      if (!common && type && type->nonOptional()->isNumeric() && t->isNumeric()) {
        common = type->nonOptional()->isFloat() ? type : t->isFloat() ? t : nullptr;
      }
      type = common ? common : Type::error();
    }
    ResultVarHasTypes typed = VarChecker::varTyped(u, type);
    if (typed.isErr()) {
      this->state.getDiagnostics().error(u.var->loc, typed.error().message);
    }
    marks.forgetUntyped(u.var);
  }
}

}
