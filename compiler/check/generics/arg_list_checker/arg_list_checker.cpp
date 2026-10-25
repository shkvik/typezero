#include "compiler/check/generics/arg_list_checker/arg_list_checker.h"
#include "compiler/check/scopes/scopes.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/declare/class_declarer/class_declarer.h"
#include "compiler/check/flow/flow_checker/flow_checker.h"
#include "compiler/check/types/utility_types/utility_types.h"
#include "compiler/whole_program/flows/flows.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::dyn_cast_or_null;

namespace tz {

namespace {

std::string utf8(const std::u16string &text) {
  std::string out;
  llvm::convertUTF16ToUTF8String(
      llvm::ArrayRef<llvm::UTF16>(reinterpret_cast<const llvm::UTF16 *>(text.data()), text.size()),
      out);

  return out;
}

// Whether the body uses the variable `name` other than spread, `...name`,
// or (with `known`, for a list of a known length) as `name.length` and
// `name[0]`: then the tuple of the list is made.
bool usedAsValue(const Stmt *body, llvm::StringRef name, bool known) {
  llvm::SmallPtrSet<const Expr *, 4> spreads;
  bool value = false;
  visitExprs(body, [&](const Expr *e) {
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
  visitExprs(body, [&](const Expr *e) {
    if (auto *n = dyn_cast<NameExpr>(e); n && n->name == name && !spreads.contains(n)) {
      value = true;
    }
  });

  return value;
}

} // namespace

bool spreadsLists(const Type *type) {
  return type->isTuple() &&
         llvm::any_of(type->fields(), [](const Field &f) { return f.type->isListParam(); });
}

ArgListChecker::ArgListChecker(Sema &sema) : sema_(sema) {}

const Type *ArgListChecker::listElement(const Type *list) { return Type::indexedAccess(list, Type::intTy()); }

const Type *ArgListChecker::memberType(const Type *object, const Type *key) {
  // Of a tuple, an element by its number, or any of them.
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
  // Of an object, a field by its name, or the fields a union of names
  // names.
  if (object->isObject() && !object->isTuple()) {
    if (ClassDecl *cls = ClassDeclarer::classOf(object)) {
      sema_.classDeclarer().completeClass(cls);
    }
    llvm::ArrayRef<const Type *> names =
        key->isUnion() ? key->members() : llvm::ArrayRef<const Type *>(key);
    std::vector<const Type *> types;
    for (const Type *name : names) {
      if (!name->isStringLiteral()) {
        return nullptr;
      }
      const Field *field = object->field(utf8(name->literalText()));
      const Type *got =
          field ? field->type : sema_.utilityTypes().getterType(ClassDeclarer::classOf(object), utf8(name->literalText()));
      if (!got) {
        return nullptr;
      }
      types.push_back(got);
    }
    return types.empty() ? nullptr : Type::unionOf(types);
  }
  if (object->isRecord() && key->isString()) {
    return Type::optional(object->valueType());
  }

  return nullptr;
}

void ArgListChecker::expandArgumentList(FunctionDecl *fn, FuncSymbol *sym) {
  if (!sym->hasRest || fn->params.empty() || !fn->params.back().rest || !fn->listName.empty()) {
    return;
  }
  const Type *list = sym->params.back();
  std::vector<const Type *> flat;
  size_t minArgs = sym->minArgs;
  bool hasRest = sym->hasRest;
  if (!flattenArguments(sym->params, minArgs, hasRest, flat)) {
    return;
  }
  Param rest = fn->params.back();
  fn->params.pop_back();
  fn->listName = rest.name;
  fn->listStart = fn->params.size();
  fn->listType = list;
  for (size_t k = 0; k < list->fields().size(); ++k) {
    Param param;
    param.name = sema_.ctx_.keepName("%" + rest.name.str() + "." + std::to_string(k));
    param.loc = rest.loc;
    param.optional = k >= list->tupleMin();
    param.declaredType = list->fields()[k].type;
    fn->params.push_back(param);
  }
  // The rest of the list stays a rest parameter, of its array.
  if (const Type *more = list->tupleRest()) {
    Param param;
    param.name = sema_.ctx_.keepName("%" + rest.name.str() + ".rest");
    param.loc = rest.loc;
    param.rest = true;
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
    if (!fn->params[k].rest) {
      elements.push_back(fn->params[k].declaredType);
    }
  }
  llvm::SMLoc loc = fn->params.size() > fn->listStart ? fn->params[fn->listStart].loc : fn->loc();
  if (!usedAsValue(fn->body, fn->listName, !fn->listType->tupleRest())) {
    // Every use spreads it: no tuple, only its parameters (see
    // expandListSpreads).
    VarSymbol *var = sema_.scopes().declareVar(fn->listName, loc, Type::tuple(elements), /*isConst=*/true);
    argumentLists_[var] = std::move(parts);
    return;
  }
  if (fn->listType->tupleRest()) {
    sema_.diags_.error(loc, "'" + fn->listName + "' of the parameters '" + fn->listType->name() +
                          "', which end with a rest parameter, can only be spread, '..." +
                          fn->listName + "'");
    sema_.scopes().declareVar(fn->listName, loc, Type::error(), /*isConst=*/true);
    return;
  }
  // `const args: [int, int] = [a0, a1]` before the body, by a name of its
  // type that no program can write.
  auto *alias = sema_.ctx_.make<TypeAliasDecl>(loc, sema_.ctx_.hiddenName(), TypeRef());
  alias->resolved = Type::tuple(elements);
  sema_.typeScopes_.back()[alias->name] = alias;
  auto *tuple = sema_.ctx_.make<ArrayLitExpr>(loc);
  for (llvm::StringRef part : parts) {
    tuple->elements.push_back({sema_.ctx_.make<NameExpr>(loc, part), /*spread=*/false});
  }
  VarDeclarator d;
  d.name = fn->listName;
  d.loc = loc;
  TypeRef ref;
  ref.name = alias->name;
  ref.loc = loc;
  d.typeAnn = ref;
  d.init = tuple;
  auto *decl = sema_.ctx_.make<VarDeclStmt>(loc, /*isConst=*/true);
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
    VarSymbol *var = spreads && name ? sema_.scopes().lookup(name->name).var : nullptr;
    auto found = var ? argumentLists_.find(var) : argumentLists_.end();
    if (found != argumentLists_.end()) {
      // `...args`: the parameters of the list, one by one.
      changed = true;
      for (llvm::StringRef part : found->second) {
        out.push_back(sema_.ctx_.make<NameExpr>(name->loc(), part));
        outSpread.push_back(part.ends_with(".rest"));
      }
      continue;
    }
    // `...t` of a tuple in a variable: its elements, `t[0], t[1]`.
    if (var && var->type && var->type->isTuple() && !var->type->tupleRest() &&
        llvm::none_of(var->type->fields(), [](const Field &f) { return f.type->isListParam(); })) {
      changed = true;
      for (size_t k = 0; k < var->type->fields().size(); ++k) {
        auto *index = sema_.ctx_.make<IntLitExpr>(name->loc(), k);
        out.push_back(
            sema_.ctx_.make<IndexExpr>(name->loc(), sema_.ctx_.make<NameExpr>(name->loc(), name->name), index));
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
  VarSymbol *var = name && (member || index) ? sema_.scopes().lookup(name->name).var : nullptr;
  auto found = var ? argumentLists_.find(var) : argumentLists_.end();
  if (found == argumentLists_.end()) {
    return nullptr;
  }
  const std::vector<llvm::StringRef> &parts = found->second;
  // `args.length`: how many parameters the list has, which is known.
  if (member && member->name == "length") {
    auto *count = sema_.ctx_.make<IntLitExpr>(member->loc(), parts.size());
    count->type = Type::intTy();
    return count;
  }
  // `args[1]`: that parameter.
  auto *k = index ? dyn_cast<IntLitExpr>(index->index) : nullptr;
  if (!k || k->value >= parts.size()) {
    return nullptr;
  }

  return sema_.exprChecker().checkExpr(sema_.ctx_.make<NameExpr>(index->loc(), parts[size_t(k->value)]));
}

bool ArgListChecker::checkListArguments(CallExpr *call, size_t fixed, const Type *list, llvm::SMLoc loc,
                              const std::vector<bool> *checked) {
  // What the list is made of: itself, or the elements of a tuple of lists.
  std::vector<const Type *> parts;
  if (list->isListParam()) {
    parts.push_back(list);
  } else {
    for (const Field &f : list->fields()) {
      parts.push_back(f.type);
    }
  }
  size_t k = 0;
  bool ok = true;
  for (size_t i = fixed; i < call->args.size(); ++i) {
    Expr *&arg = call->args[i];
    if (!checked || i >= checked->size() || !(*checked)[i]) {
      arg = sema_.exprChecker().checkExpr(arg, !call->spreads(i) && k < parts.size() && !parts[k]->isListParam()
                               ? parts[k]
                               : nullptr);
    }
    if (arg->type->isError()) {
      ok = false;
      continue;
    }
    if (k >= parts.size()) {
      sema_.diags_.error(arg->loc(), "too many arguments for the parameters '" + list->name() + "'");
      return false;
    }
    if (call->spreads(i)) {
      if (arg->type != parts[k]) {
        sema_.diags_.error(arg->loc(), "'..." + arg->type->name() + "' is spread where the parameters '" +
                                     list->name() + "' take " +
                                     (parts[k]->isListParam() ? "'..." : "'") + parts[k]->name() +
                                     "'");
        return false;
      }
      ++k;
      continue;
    }
    if (parts[k]->isListParam()) {
      sema_.diags_.error(arg->loc(), "the arguments for the parameters '" + parts[k]->name() +
                                   "' of some function are spread from a list of them, e.g. "
                                   "'...args'");

      return false;
    }
    arg = sema_.typeRelations().coerce(arg, parts[k]);
    ++k;
  }
  if (ok && k < parts.size()) {
    sema_.diags_.error(loc, "the parameters '" + list->name() + "' take " +
                          (parts[k]->isListParam() ? "'..." : "'") + parts[k]->name() +
                          "' too, after these arguments");
    return false;
  }
  sema_.flowChecker().calledFunction();

  return ok;
}

} // namespace tz
