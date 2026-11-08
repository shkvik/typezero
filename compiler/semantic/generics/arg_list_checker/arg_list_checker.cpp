#include "compiler/semantic/generics/arg_list_checker/arg_list_checker.h"
#include "compiler/program/ast_walker/ast_walker.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/declare/class_declarer/class_declarer.h"
#include "compiler/semantic/flow/flow_checker/flow_checker.h"
#include "compiler/semantic/names/scopes/scopes.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "compiler/semantic/types/utility_types/utility_types.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

bool ArgListChecker::usedAsValue(const Stmt *body, llvm::StringRef name, bool known) {
  llvm::SmallPtrSet<const Expr *, 4> spreads;
  bool value = false;
  AstWalker::visitExprs(body, [&](const Expr *e) {
    if (auto *call = dyn_cast<CallExpr>(e)) {
      for (size_t i = 0; i < call->args.size(); ++i) {
        if (call->spreads(i)) {
          spreads.insert(call->args[i]);
        }
      }
    } else if (auto *lit = dyn_cast<ArrayLitExpr>(e)) {
      for (const ArrayLitExpr::Element &el : lit->elements) {
        if (el.spread) {
          spreads.insert(el.expr);
        }
      }
    } else if (auto *member = dyn_cast<MemberExpr>(e);
               member && known && member->name == "length") {
      spreads.insert(member->object);
    } else if (auto *index = dyn_cast<IndexExpr>(e);
               index && known && isa<IntLitExpr>(index->index)) {
      spreads.insert(index->object);
    }
  });
  AstWalker::visitExprs(body, [&](const Expr *e) {
    if (auto *n = dyn_cast<NameExpr>(e); n && n->name == name && !spreads.contains(n)) {
      value = true;
    }
  });

  return value;
}

bool ArgListChecker::spreadsLists(const Type *type) {
  return type->isTuple() &&
         llvm::any_of(type->fields(), [](const Field &f) { return f.type->isListParam(); });
}

ArgListChecker::ArgListChecker(SemanticState &owner)
    : state(owner) {}

const Type *ArgListChecker::listElement(const Type *list) {
  return Type::indexedAccess(list, Type::intTy());
}

const Type *ArgListChecker::memberType(const Type *object, const Type *key) {
  if (object->isTuple()) {
    llvm::ArrayRef<Field> fields = object->fields();
    if (key->isLiteral() && key->literalBase()->isNumeric()) {
      double k = key->literalNumber();
      if (k < 0 || k >= double(fields.size()) || k != double(size_t(k))) {
        return object->tupleRest() ? Type::optional(object->tupleRest()->inner()) : nullptr;
      }
      return fields[size_t(k)].type;
    }
    if (key->isNumeric()) {
      std::vector<const Type *> types;
      for (const Field &f : fields) {
        types.push_back(f.type);
      }
      if (object->tupleRest()) {
        types.push_back(object->tupleRest()->inner());
      }
      return types.empty() ? nullptr : Type::unionOf(types);
    }
    return nullptr;
  }
  if (object->isArray() &&
      (key->isNumeric() || (key->isLiteral() && key->literalBase()->isNumeric()))) {
    return object->inner();
  }
  if (object->isObject() && !object->isTuple()) {
    if (ClassDecl *cls = ClassDeclarer::classOf(object)) {
      ClassDeclarer(this->state).completeClass(cls);
    }
    llvm::ArrayRef<const Type *> names =
        key->isUnion() ? key->members() : llvm::ArrayRef<const Type *>(key);
    std::vector<const Type *> types;
    for (const Type *name : names) {
      if (!name->isStringLiteral()) {
        return nullptr;
      }
      const Field *field = object->field(ToUtf8::toUtf8(name->literalText()));
      if (!field) {
        return nullptr;
      }
      types.push_back(field->type);
    }
    return types.empty() ? nullptr : Type::unionOf(types);
  }
  if (object->isRecord() && key->isString()) {
    return Type::optional(object->valueType());
  }

  return nullptr;
}

void ArgListChecker::expandArgumentList(FunctionDecl *fn, FuncSymbol *sym) {
  if (!sym->hasRest || fn->params.empty() || !fn->params.back().rest() || !fn->listName.empty()) {
    return;
  }
  const Type *list = sym->params.back();
  std::vector<const Type *> flat;
  size_t minArgs = sym->minArgs;
  bool hasRest = sym->hasRest;
  if (!ArgumentLists::flattenArguments(sym->params, minArgs, hasRest, flat)) {
    return;
  }
  Param rest = fn->params.back();
  fn->params.pop_back();
  fn->listName = rest.name;
  fn->listStart = fn->params.size();
  fn->listType = list;
  for (size_t k = 0; k < list->fields().size(); ++k) {
    Param param;
    param.name = this->state.getAst().keepName("%" + rest.name.str() + "." + std::to_string(k));
    param.loc = rest.loc;
    if (k >= list->tupleMin()) {
      param.form = OptionalParam{};
    }
    param.declaredType = list->fields()[k].type;
    fn->params.push_back(param);
  }
  if (const Type *more = list->tupleRest()) {
    Param param;
    param.name = this->state.getAst().keepName("%" + rest.name.str() + ".rest");
    param.loc = rest.loc;
    param.form = RestParam{};
    param.declaredType = more;
    fn->params.push_back(param);
  }
  sym->params = std::move(flat);
  sym->minArgs = minArgs;
  sym->hasRest = hasRest;
}

void ArgListChecker::declareArgumentList(FunctionDecl *fn) {
  if (fn->listName.empty()) {
    return;
  }
  std::vector<llvm::StringRef> parts;
  std::vector<const Type *> elements;
  for (size_t k = fn->listStart; k < fn->params.size(); ++k) {
    parts.push_back(fn->params[k].name);
    if (!fn->params[k].rest()) {
      elements.push_back(fn->params[k].declaredType);
    }
  }
  llvm::SMLoc loc = fn->params.size() > fn->listStart ? fn->params[fn->listStart].loc : fn->loc();
  if (!ArgListChecker::usedAsValue(fn->body, fn->listName, !fn->listType->tupleRest())) {
    VarSymbol *var = Scopes(this->state).declareVar(fn->listName, loc, Type::tuple(elements), true);
    this->state.getExpressionMarks().rememberArgumentList(var, std::move(parts));
    return;
  }
  ResultListIsSpread spreadOnly = ArgListChecker::restListValue(fn);
  if (spreadOnly.isErr()) {
    this->state.getDiagnostics().error(loc, spreadOnly.error().message);
    Scopes(this->state).declareVar(fn->listName, loc, Type::error(), true);
    return;
  }
  auto *alias =
      this->state.getAst().make<TypeAliasDecl>(loc, this->state.getAst().hiddenName(), TypeRef());
  alias->resolved = Type::tuple(elements);
  this->state.getScopeStack().setType(alias->name, alias);
  auto *tuple = this->state.getAst().make<ArrayLitExpr>(loc);
  for (llvm::StringRef part : parts) {
    tuple->elements.push_back({this->state.getAst().make<NameExpr>(loc, part), false});
  }
  VarDeclarator d;
  d.name = fn->listName;
  d.loc = loc;
  TypeRef ref;
  ref.name = alias->name;
  ref.loc = loc;
  d.typeAnn = ref;
  d.init = tuple;
  auto *decl = this->state.getAst().make<VarDeclStmt>(loc, true);
  decl->decls.push_back(d);
  fn->body->body.insert(fn->body->body.begin(), decl);
}

void ArgListChecker::expandListSpreads(std::vector<Expr *> &args, std::vector<bool> &spread) {
  if (!llvm::is_contained(spread, true)) {
    return;
  }
  std::vector<Expr *> out;
  std::vector<bool> outSpread;
  bool changed = false;
  for (size_t i = 0; i < args.size(); ++i) {
    bool spreads = i < spread.size() && spread[i];
    auto *name = dyn_cast<NameExpr>(args[i]);
    VarSymbol *var = spreads && name ? Scopes(this->state).lookup(name->name).var() : nullptr;
    std::optional<std::vector<llvm::StringRef>> found =
        var ? this->state.getExpressionMarks().findArgumentList(var) : std::nullopt;
    if (found) {
      changed = true;
      for (llvm::StringRef part : *found) {
        out.push_back(this->state.getAst().make<NameExpr>(name->loc(), part));
        outSpread.push_back(part.ends_with(".rest"));
      }
      continue;
    }
    if (var && var->type && var->type->isTuple() && !var->type->tupleRest() &&
        llvm::none_of(var->type->fields(), [](const Field &f) { return f.type->isListParam(); })) {
      changed = true;
      for (size_t k = 0; k < var->type->fields().size(); ++k) {
        auto *index = this->state.getAst().make<IntLitExpr>(name->loc(), k);
        out.push_back(this->state.getAst().make<IndexExpr>(
            name->loc(), this->state.getAst().make<NameExpr>(name->loc(), name->name), index));
        outSpread.push_back(false);
      }
      continue;
    }
    out.push_back(args[i]);
    outSpread.push_back(spreads);
  }
  if (!changed) {
    return;
  }
  args = std::move(out);
  spread = llvm::is_contained(outSpread, true) ? std::move(outSpread) : std::vector<bool>();
}

Expr *ArgListChecker::listPart(Expr *expr) {
  auto *member = dyn_cast<MemberExpr>(expr);
  auto *index = dyn_cast<IndexExpr>(expr);
  auto *name = dyn_cast<NameExpr>(member ? member->object : index ? index->object : expr);
  VarSymbol *var =
      name && (member || index) ? Scopes(this->state).lookup(name->name).var() : nullptr;
  std::optional<std::vector<llvm::StringRef>> found =
      var ? this->state.getExpressionMarks().findArgumentList(var) : std::nullopt;
  if (!found) {
    return nullptr;
  }
  const std::vector<llvm::StringRef> &parts = *found;
  if (member && member->name == "length") {
    auto *count = this->state.getAst().make<IntLitExpr>(member->loc(), parts.size());
    count->type = Type::intTy();
    return count;
  }
  auto *k = index ? dyn_cast<IntLitExpr>(index->index) : nullptr;
  if (!k || k->value >= parts.size()) {
    return nullptr;
  }

  return ExprChecker(this->state)
      .checkExpr(this->state.getAst().make<NameExpr>(index->loc(), parts[size_t(k->value)]));
}

ArgListChecker::ResultListIsSpread ArgListChecker::restListValue(const FunctionDecl *fn) {
  if (fn->listType->tupleRest()) {
    std::string message = ErrorTextRestList::spreadOnly(fn->listName, fn->listType->name());

    return err(ArgListCheckerRestListValueError{
        .code = ArgListCheckerRestListValueErrorCode::REST_LIST_AS_VALUE,
        .message = message,
    });
  }

  return ok(fn);
}

ArgListChecker::ResultArgumentFits ArgListChecker::listArgument(const Type *list,
                                                                llvm::ArrayRef<const Type *> parts,
                                                                size_t k, const Expr *arg,
                                                                bool spreads) {
  if (k >= parts.size()) {
    std::string message = ErrorTextListArgument::tooMany(list->name());

    return err(ArgListCheckerListArgumentError{
        .code = ArgListCheckerListArgumentErrorCode::TOO_MANY_ARGUMENTS,
        .message = message,
    });
  }
  if (spreads && arg->type != parts[k]) {
    std::string message = ErrorTextListArgument::spreadWrong(
        arg->type->name(), list->name(), parts[k]->name(), parts[k]->isListParam());

    return err(ArgListCheckerListArgumentError{
        .code = ArgListCheckerListArgumentErrorCode::SPREAD_MISMATCH,
        .message = message,
    });
  }
  if (!spreads && parts[k]->isListParam()) {
    std::string message = ErrorTextListArgument::notSpread(parts[k]->name());

    return err(ArgListCheckerListArgumentError{
        .code = ArgListCheckerListArgumentErrorCode::LIST_NOT_SPREAD,
        .message = message,
    });
  }

  return ok(arg);
}

ArgListChecker::ResultListIsFilled
ArgListChecker::listFilled(const Type *list, llvm::ArrayRef<const Type *> parts, size_t k) {
  if (k < parts.size()) {
    std::string message =
        ErrorTextListFilled::missing(list->name(), parts[k]->name(), parts[k]->isListParam());

    return err(ArgListCheckerListFilledError{
        .code = ArgListCheckerListFilledErrorCode::MISSING_ARGUMENTS,
        .message = message,
    });
  }

  return ok(list);
}

bool ArgListChecker::checkListArguments(CallExpr *call, size_t fixed, const Type *list,
                                        llvm::SMLoc loc, const std::vector<bool> *checked) {
  std::vector<const Type *> parts;
  if (list->isListParam()) {
    parts.push_back(list);
  } else {
    for (const Field &f : list->fields()) {
      parts.push_back(f.type);
    }
  }
  size_t k = 0;
  bool typed = true;
  for (size_t i = fixed; i < call->args.size(); ++i) {
    Expr *&arg = call->args[i];
    if (!checked || i >= checked->size() || !(*checked)[i]) {
      arg = ExprChecker(this->state)
                .checkExpr(arg, !call->spreads(i) && k < parts.size() && !parts[k]->isListParam()
                                    ? parts[k]
                                    : nullptr);
    }
    if (arg->type->isError()) {
      typed = false;
      continue;
    }
    ResultArgumentFits fits = ArgListChecker::listArgument(list, parts, k, arg, call->spreads(i));
    if (fits.isErr()) {
      this->state.getDiagnostics().error(arg->loc(), fits.error().message);
      return false;
    }
    if (!call->spreads(i)) {
      arg = TypeRelations(this->state).coerce(arg, parts[k]);
    }
    ++k;
  }
  ResultListIsFilled filled = ArgListChecker::listFilled(list, parts, k);
  if (typed && filled.isErr()) {
    this->state.getDiagnostics().error(loc, filled.error().message);
    return false;
  }
  FlowChecker(this->state).calledFunction();

  return typed;
}

}
