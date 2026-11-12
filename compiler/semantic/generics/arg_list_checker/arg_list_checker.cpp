#include "compiler/semantic/generics/arg_list_checker/arg_list_checker.h"
#include "compiler/program/library/builtin_names/builtin_property_table/builtin_property_table.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/declare/class_declarer/class_declarer.h"
#include "compiler/semantic/flow/flow_checker/flow_checker.h"
#include "compiler/semantic/flow/placed_nodes/placed_nodes.h"
#include "compiler/semantic/names/scopes/scopes.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "compiler/semantic/types/utility_types/utility_types.h"
#include "llvm/ADT/STLExtras.h"

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

namespace {

constexpr llvm::StringLiteral kHiddenPrefix = "%";
constexpr llvm::StringLiteral kRestPartSuffix = ".rest";

}

ArgListChecker::ArgListChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->scopes = &all.scopes;
  this->exprChecker = &all.exprChecker;
  this->flowChecker = &all.flowChecker;
  this->classDeclarer = &all.classDeclarer;
  this->typeRelations = &all.typeRelations;
}

bool ArgListChecker::isUsedAsValue(const Stmt *body, llvm::StringRef name, bool isFixed) const {
  auto nodes = PlacedNodes::collect({body}, {});
  auto listUses = this->listUses(nodes, isFixed);
  for (const PlacedExpr &placed : nodes.getExprs()) {
    auto *named = dyn_cast<NameExpr>(placed.expr);
    if (named != nullptr && named->name == name && !listUses.contains(named)) {
      return true;
    }
  }

  return false;
}

llvm::SmallPtrSet<const Expr *, 4> ArgListChecker::listUses(const PlacedNodes &nodes,
                                                            bool isFixed) const {
  auto uses = llvm::SmallPtrSet<const Expr *, 4>();
  for (const PlacedExpr &placed : nodes.getExprs()) {
    this->addSpreadUses(placed.expr, uses);
    if (isFixed) {
      this->addFixedUses(placed.expr, uses);
    }
  }

  return uses;
}

void ArgListChecker::addSpreadUses(const Expr *expr,
                                   llvm::SmallPtrSetImpl<const Expr *> &uses) const {
  auto *call = dyn_cast<CallExpr>(expr);
  auto *literal = dyn_cast<ArrayLitExpr>(expr);
  if (call != nullptr) {
    for (size_t position = 0; position < call->args.size(); ++position) {
      if (call->spreads(position)) {
        uses.insert(call->args[position]);
      }
    }
  }
  if (literal != nullptr) {
    for (const ArrayLitExpr::Element &element : literal->elements) {
      if (element.spread) {
        uses.insert(element.expr);
      }
    }
  }
}

void ArgListChecker::addFixedUses(const Expr *expr,
                                  llvm::SmallPtrSetImpl<const Expr *> &uses) const {
  auto *member = dyn_cast<MemberExpr>(expr);
  auto *index = dyn_cast<IndexExpr>(expr);
  if (member != nullptr && BuiltinPropertyTable::isName(member->name, BuiltinProperty::LENGTH)) {
    uses.insert(member->object);
  }
  if (index != nullptr && isa<IntLitExpr>(index->index)) {
    uses.insert(index->object);
  }
}

bool ArgListChecker::isSpreadingLists(const Type *type) {
  if (!type->isTuple()) {
    return false;
  }
  for (const Field &field : type->fields()) {
    if (field.type->isListParam()) {
      return true;
    }
  }

  return false;
}

const Type *ArgListChecker::listElement(const Type *list) {
  return Type::indexedAccess(list, Type::intTy());
}

bool ArgListChecker::isNumericKey(const Type *key) const {
  return key->isNumeric() || (key->isLiteral() && key->literalBase()->isNumeric());
}

const Type *ArgListChecker::memberType(const Type *object, const Type *key) {
  if (object->isTuple()) {
    return this->tupleMemberType(object, key);
  }
  if (object->isArray() && this->isNumericKey(key)) {
    return object->inner();
  }
  if (object->isObject()) {
    return this->objectMemberType(object, key);
  }
  if (object->isRecord() && key->isString()) {
    return Type::optional(object->valueType());
  }

  return nullptr;
}

const Type *ArgListChecker::tupleMemberType(const Type *tuple, const Type *key) const {
  llvm::ArrayRef<Field> fields = tuple->fields();
  if (key->isLiteral() && key->literalBase()->isNumeric()) {
    return this->tupleFieldAt(tuple, key->literalNumber());
  }
  if (!key->isNumeric()) {
    return nullptr;
  }
  auto types = std::vector<const Type *>();
  for (const Field &field : fields) {
    types.push_back(field.type);
  }
  if (tuple->tupleRest() != nullptr) {
    types.push_back(tuple->tupleRest()->inner());
  }
  if (types.empty()) {
    return nullptr;
  }

  return Type::unionOf(types);
}

const Type *ArgListChecker::tupleFieldAt(const Type *tuple, double position) const {
  llvm::ArrayRef<Field> fields = tuple->fields();
  bool isInside =
      position >= 0 && position < double(fields.size()) && position == double(size_t(position));
  if (isInside) {
    return fields[size_t(position)].type;
  }
  if (tuple->tupleRest() == nullptr) {
    return nullptr;
  }

  return Type::optional(tuple->tupleRest()->inner());
}

const Type *ArgListChecker::objectMemberType(const Type *object, const Type *key) {
  auto *classDecl = this->classDeclarer->classOf(object);
  if (classDecl != nullptr) {
    this->classDeclarer->completeClass(classDecl);
  }
  llvm::ArrayRef<const Type *> names =
      key->isUnion() ? key->members() : llvm::ArrayRef<const Type *>(key);
  auto types = std::vector<const Type *>();
  for (const Type *name : names) {
    if (!name->isStringLiteral()) {
      return nullptr;
    }
    auto fieldName = ToUtf8::toUtf8(name->literalText());
    const auto *field = object->field(fieldName);
    if (field == nullptr) {
      return nullptr;
    }
    types.push_back(field->type);
  }
  if (types.empty()) {
    return nullptr;
  }

  return Type::unionOf(types);
}

bool ArgListChecker::isExpandableRest(const FunctionDecl *functionDecl,
                                      const FuncSymbol *function) const {
  if (!function->hasRest || functionDecl->params.empty()) {
    return false;
  }

  return functionDecl->params.back().rest() && functionDecl->listName.empty();
}

void ArgListChecker::expandArgumentList(FunctionDecl *functionDecl, FuncSymbol *function) {
  if (!this->isExpandableRest(functionDecl, function)) {
    return;
  }
  const Type *list = function->params.back();
  auto flat = std::vector<const Type *>();
  size_t minArgs = function->minArgs;
  bool hasRest = function->hasRest;
  if (!ArgumentLists::flattenArguments(function->params, minArgs, hasRest, flat)) {
    return;
  }
  Param rest = functionDecl->params.back();
  functionDecl->params.pop_back();
  functionDecl->listName = rest.name;
  functionDecl->listStart = functionDecl->params.size();
  functionDecl->listType = list;
  for (size_t index = 0; index < list->fields().size(); ++index) {
    auto param = this->listPartParam(rest, list, index);
    functionDecl->params.push_back(param);
  }
  const Type *more = list->tupleRest();
  if (more != nullptr) {
    auto param = this->listRestParam(rest, more);
    functionDecl->params.push_back(param);
  }
  function->params = std::move(flat);
  function->minArgs = minArgs;
  function->hasRest = hasRest;
}

Param ArgListChecker::listPartParam(const Param &rest, const Type *list, size_t index) {
  std::string name = kHiddenPrefix.str() + rest.name.str() + "." + std::to_string(index);
  Param param;
  param.name = this->state.getAst().keepName(name);
  param.loc = rest.loc;
  if (index >= list->tupleMin()) {
    param.form = OptionalParam{};
  }
  param.declaredType = list->fields()[index].type;

  return param;
}

Param ArgListChecker::listRestParam(const Param &rest, const Type *more) {
  std::string name = kHiddenPrefix.str() + rest.name.str() + kRestPartSuffix.str();
  Param param;
  param.name = this->state.getAst().keepName(name);
  param.loc = rest.loc;
  param.form = RestParam{};
  param.declaredType = more;

  return param;
}

void ArgListChecker::declareArgumentList(FunctionDecl *functionDecl) {
  if (functionDecl->listName.empty()) {
    return;
  }
  auto listParams = llvm::ArrayRef<Param>(functionDecl->params).drop_front(functionDecl->listStart);
  auto parts = std::vector<llvm::StringRef>();
  auto elements = std::vector<const Type *>();
  for (const Param &param : listParams) {
    parts.push_back(param.name);
    if (!param.rest()) {
      elements.push_back(param.declaredType);
    }
  }
  llvm::SMLoc loc = listParams.empty() ? functionDecl->loc() : listParams.front().loc;
  bool isFixed = functionDecl->listType->tupleRest() == nullptr;
  if (!this->isUsedAsValue(functionDecl->body, functionDecl->listName, isFixed)) {
    const auto *tuple = Type::tuple(elements);
    auto *variable = this->scopes->declareConst(functionDecl->listName, loc, tuple);
    this->state.getExpressionMarks().rememberArgumentList(variable, std::move(parts));
    return;
  }
  auto spreadOnly = this->restListValue(functionDecl);
  if (spreadOnly.isErr()) {
    this->state.getDiagnostics().error(loc, spreadOnly.error().message);
    this->scopes->declareConst(functionDecl->listName, loc, Type::error());
    return;
  }
  this->declareListValue(functionDecl, loc, parts, elements);
}

void ArgListChecker::declareListValue(FunctionDecl *functionDecl, llvm::SMLoc loc,
                                      llvm::ArrayRef<llvm::StringRef> parts,
                                      llvm::ArrayRef<const Type *> elements) {
  auto &ast = this->state.getAst();
  auto *alias = ast.make<TypeAliasDecl>(loc, ast.hiddenName(), TypeRef());
  alias->resolved = Type::tuple(elements);
  this->state.getScopeStack().setType(alias->name, alias);
  auto *tuple = ast.make<ArrayLitExpr>(loc);
  for (llvm::StringRef part : parts) {
    auto *partName = ast.make<NameExpr>(loc, part);
    auto element = ArrayLitExpr::Element{.expr = partName, .spread = false};
    tuple->elements.push_back(element);
  }
  TypeRef ref;
  ref.name = alias->name;
  ref.loc = loc;
  VarDeclarator declarator;
  declarator.name = functionDecl->listName;
  declarator.loc = loc;
  declarator.typeAnn = ref;
  declarator.init = tuple;
  auto *varDecl = ast.make<VarDeclStmt>(loc, true);
  varDecl->decls.push_back(declarator);
  functionDecl->body->body.insert(functionDecl->body->body.begin(), varDecl);
}

void ArgListChecker::expandListSpreads(std::vector<Expr *> &args, std::vector<bool> &spread) {
  if (!llvm::is_contained(spread, true)) {
    return;
  }
  auto expanded = ExpandedArgs{.args = {}, .spread = {}, .hasChanged = false};
  for (size_t position = 0; position < args.size(); ++position) {
    bool isSpreading = position < spread.size() && spread[position];
    this->addExpandedArgument(args[position], isSpreading, expanded);
  }
  if (!expanded.hasChanged) {
    return;
  }
  args = std::move(expanded.args);
  bool hasSpread = llvm::is_contained(expanded.spread, true);
  spread = hasSpread ? std::move(expanded.spread) : std::vector<bool>();
}

void ArgListChecker::addExpandedArgument(Expr *arg, bool isSpreading, ExpandedArgs &expanded) {
  auto *name = dyn_cast<NameExpr>(arg);
  VarSymbol *variable =
      isSpreading && name != nullptr ? this->scopes->lookup(name->name).var() : nullptr;
  auto parts = variable != nullptr ? this->state.getExpressionMarks().findArgumentList(variable)
                                   : std::nullopt;
  if (parts.has_value()) {
    return this->addListParts(*parts, name->loc(), expanded);
  }
  if (variable != nullptr && this->isFixedTuple(variable->type)) {
    return this->addTupleElements(name, variable->type, expanded);
  }
  expanded.args.push_back(arg);
  expanded.spread.push_back(isSpreading);
}

bool ArgListChecker::isFixedTuple(const Type *type) const {
  if (type == nullptr || !type->isTuple() || type->tupleRest() != nullptr) {
    return false;
  }

  return !ArgListChecker::isSpreadingLists(type);
}

void ArgListChecker::addListParts(llvm::ArrayRef<llvm::StringRef> parts, llvm::SMLoc loc,
                                  ExpandedArgs &expanded) {
  expanded.hasChanged = true;
  for (llvm::StringRef part : parts) {
    auto *partName = this->state.getAst().make<NameExpr>(loc, part);
    expanded.args.push_back(partName);
    expanded.spread.push_back(part.ends_with(kRestPartSuffix));
  }
}

void ArgListChecker::addTupleElements(const NameExpr *name, const Type *tuple,
                                      ExpandedArgs &expanded) {
  auto &ast = this->state.getAst();
  llvm::SMLoc loc = name->loc();
  expanded.hasChanged = true;
  for (size_t position = 0; position < tuple->fields().size(); ++position) {
    auto *index = ast.make<IntLitExpr>(loc, position);
    auto *root = ast.make<NameExpr>(loc, name->name);
    auto *element = ast.make<IndexExpr>(loc, root, index);
    expanded.args.push_back(element);
    expanded.spread.push_back(false);
  }
}

std::optional<std::vector<llvm::StringRef>>
ArgListChecker::argumentListOf(const Expr *object) const {
  auto *name = dyn_cast<NameExpr>(object);
  if (name == nullptr) {
    return std::nullopt;
  }
  VarSymbol *variable = this->scopes->lookup(name->name).var();
  if (variable == nullptr) {
    return std::nullopt;
  }

  return this->state.getExpressionMarks().findArgumentList(variable);
}

Expr *ArgListChecker::listPart(Expr *expr) {
  auto *member = dyn_cast<MemberExpr>(expr);
  auto *index = dyn_cast<IndexExpr>(expr);
  if (member == nullptr && index == nullptr) {
    return nullptr;
  }
  const Expr *object = member != nullptr ? member->object : index->object;
  auto parts = this->argumentListOf(object);
  if (!parts.has_value()) {
    return nullptr;
  }
  if (member != nullptr && BuiltinPropertyTable::isName(member->name, BuiltinProperty::LENGTH)) {
    auto *count = this->state.getAst().make<IntLitExpr>(member->loc(), parts->size());
    count->type = Type::intTy();

    return count;
  }
  auto *position = index != nullptr ? dyn_cast<IntLitExpr>(index->index) : nullptr;
  if (position == nullptr || position->value >= parts->size()) {
    return nullptr;
  }
  llvm::StringRef part = (*parts)[size_t(position->value)];
  auto *partName = this->state.getAst().make<NameExpr>(index->loc(), part);

  return this->exprChecker->checkExpr(partName);
}

ArgListChecker::ResultListIsSpread
ArgListChecker::restListValue(const FunctionDecl *functionDecl) const {
  if (functionDecl->listType->tupleRest() != nullptr) {
    auto message =
        ErrorTextRestList::spreadOnly(functionDecl->listName, functionDecl->listType->name());

    return err(ArgListCheckerRestListValueError{
        .code = ArgListCheckerRestListValueErrorCode::REST_LIST_AS_VALUE,
        .message = message,
    });
  }

  return ok(functionDecl);
}

ArgListChecker::ResultArgumentFits ArgListChecker::listArgument(const Type *list,
                                                                llvm::ArrayRef<const Type *> parts,
                                                                size_t used, const Expr *arg,
                                                                bool isSpread) const {
  if (used >= parts.size()) {
    auto message = ErrorTextListArgument::tooMany(list->name());

    return err(ArgListCheckerListArgumentError{
        .code = ArgListCheckerListArgumentErrorCode::TOO_MANY_ARGUMENTS,
        .message = message,
    });
  }
  const Type *part = parts[used];
  if (isSpread && arg->type != part) {
    auto message = ErrorTextListArgument::spreadWrong(arg->type->name(), list->name(), part->name(),
                                                      part->isListParam());

    return err(ArgListCheckerListArgumentError{
        .code = ArgListCheckerListArgumentErrorCode::SPREAD_MISMATCH,
        .message = message,
    });
  }
  if (!isSpread && part->isListParam()) {
    auto message = ErrorTextListArgument::notSpread(part->name());

    return err(ArgListCheckerListArgumentError{
        .code = ArgListCheckerListArgumentErrorCode::LIST_NOT_SPREAD,
        .message = message,
    });
  }

  return ok(arg);
}

ArgListChecker::ResultListIsFilled ArgListChecker::listFilled(const Type *list,
                                                              llvm::ArrayRef<const Type *> parts,
                                                              size_t used) const {
  if (used < parts.size()) {
    const Type *part = parts[used];
    auto message = ErrorTextListFilled::missing(list->name(), part->name(), part->isListParam());

    return err(ArgListCheckerListFilledError{
        .code = ArgListCheckerListFilledErrorCode::MISSING_ARGUMENTS,
        .message = message,
    });
  }

  return ok(list);
}

std::vector<const Type *> ArgListChecker::listParts(const Type *list) const {
  if (list->isListParam()) {
    return {list};
  }
  auto parts = std::vector<const Type *>();
  for (const Field &field : list->fields()) {
    parts.push_back(field.type);
  }

  return parts;
}

bool ArgListChecker::isChecked(const std::vector<bool> *checked, size_t index) const {
  return checked != nullptr && index < checked->size() && (*checked)[index];
}

const Type *ArgListChecker::expectedPart(llvm::ArrayRef<const Type *> parts, size_t used,
                                         bool isSpread) const {
  if (isSpread || used >= parts.size() || parts[used]->isListParam()) {
    return nullptr;
  }

  return parts[used];
}

bool ArgListChecker::checkListArguments(CallExpr *call, size_t fixed, const Type *list,
                                        llvm::SMLoc loc, const std::vector<bool> *checked) {
  auto parts = this->listParts(list);
  size_t used = 0;
  bool isTyped = true;
  for (size_t index = fixed; index < call->args.size(); ++index) {
    auto fit = this->fitListArgument(call, index, list, parts, used, checked);
    if (fit == ListFit::REPORTED) {
      return false;
    }
    if (fit == ListFit::UNTYPED) {
      isTyped = false;
    }
    if (fit == ListFit::FITS) {
      used += 1;
    }
  }
  auto filled = this->listFilled(list, parts, used);
  if (isTyped && filled.isErr()) {
    this->state.getDiagnostics().error(loc, filled.error().message);
    return false;
  }
  this->flowChecker->calledFunction();

  return isTyped;
}

ArgListChecker::ListFit ArgListChecker::fitListArgument(CallExpr *call, size_t index,
                                                        const Type *list,
                                                        llvm::ArrayRef<const Type *> parts,
                                                        size_t used,
                                                        const std::vector<bool> *checked) {
  Expr *&arg = call->args[index];
  bool isSpread = call->spreads(index);
  if (!this->isChecked(checked, index)) {
    const auto *expected = this->expectedPart(parts, used, isSpread);
    arg = this->exprChecker->checkExpr(arg, expected);
  }
  if (arg->type->isError()) {
    return ListFit::UNTYPED;
  }
  auto fits = this->listArgument(list, parts, used, arg, isSpread);
  if (fits.isErr()) {
    this->state.getDiagnostics().error(arg->loc(), fits.error().message);

    return ListFit::REPORTED;
  }
  if (!isSpread) {
    arg = this->typeRelations->coerce(arg, parts[used]);
  }

  return ListFit::FITS;
}

}
