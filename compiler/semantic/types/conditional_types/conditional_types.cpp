#include "compiler/semantic/types/conditional_types/conditional_types.h"
#include "compiler/semantic/bodies/top_level_checker/top_level_checker.h"
#include "compiler/semantic/declare/class_declarer/class_declarer.h"
#include "compiler/semantic/declare/type_declarer/type_declarer.h"
#include "compiler/semantic/generics/type_params/type_params.h"
#include "compiler/semantic/infer/return_inferrer/return_inferrer.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/conditional_check_helpers/conditional_check_helpers.h"
#include "compiler/semantic/types/extends_relation/extends_relation.h"
#include "compiler/semantic/types/type_operators/type_operators.h"
#include "compiler/semantic/types/type_ref_parts/type_ref_parts.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "compiler/semantic/types/type_resolver/type_resolver.h"
#include "llvm/ADT/STLExtras.h"
#include <cmath>

namespace tz {

ConditionalTypes::ConditionalTypes(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->typeParams = &all.typeParams;
  this->typeDeclarer = &all.typeDeclarer;
  this->typeResolver = &all.typeResolver;
  this->classDeclarer = &all.classDeclarer;
  this->typeOperators = &all.typeOperators;
  this->returnInferrer = &all.returnInferrer;
  this->extendsRelation = &all.extendsRelation;
  this->topLevelChecker = &all.topLevelChecker;
}

bool ConditionalTypes::isBinding(const Stmt *decl) {
  auto *alias = llvm::dyn_cast_or_null<TypeAliasDecl>(decl);

  return alias && alias->resolved && alias->typeParams.empty() &&
         alias->type.kind() == TypeRefKind::NAME_REF && alias->type.name.empty();
}

std::vector<const Type *> ConditionalTypes::distributed(const Type *type) {
  std::vector<const Type *> out;
  ConditionalCheckHelpers::distribute(type, out);

  return out;
}

void ConditionalTypes::collectNames(const TypeRef &ref, std::vector<llvm::StringRef> &names) {
  for (const TypeRef *part : TypeRefParts::collect(ref)) {
    bool isNamed =
        part->kind() == TypeRefKind::NAME_REF || part->kind() == TypeRefKind::GENERIC_REF;
    if (isNamed && !part->name.empty() && !llvm::is_contained(names, part->name)) {
      names.push_back(part->name);
    }
  }
}

void ConditionalTypes::collectInfers(const TypeRef &ref, std::vector<const TypeRef *> &infers,
                                     std::vector<bool> &lists, bool spread) {
  std::vector<SpreadRef> stack = {SpreadRef{&ref, spread}};
  while (!stack.empty()) {
    SpreadRef next = stack.back();
    stack.pop_back();
    TypeRefKind kind = next.ref->kind();
    if (kind == TypeRefKind::INFER_REF) {
      ConditionalTypes::noteInfer(next, infers, lists);
    }
    if (kind != TypeRefKind::CONDITIONAL_REF) {
      ConditionalTypes::pushSpreadArgs(*next.ref, stack);
    }
  }
}

void ConditionalTypes::noteInfer(SpreadRef part, std::vector<const TypeRef *> &infers,
                                 std::vector<bool> &lists) {
  auto found = llvm::find_if(infers, [&](const TypeRef *r) { return r->name == part.ref->name; });
  if (found == infers.end()) {
    infers.push_back(part.ref);
    lists.push_back(part.spread);
  } else if (part.spread) {
    lists[size_t(found - infers.begin())] = true;
  }
}

void ConditionalTypes::pushSpreadArgs(const TypeRef &ref, std::vector<SpreadRef> &stack) {
  for (size_t i = ref.args.size(); i > 0; --i) {
    stack.push_back(SpreadRef{&ref.args[i - 1], ConditionalTypes::spreadsAt(ref, i - 1)});
  }
}

bool ConditionalTypes::spreadsAt(const TypeRef &ref, size_t i) {
  bool isTupleSpread =
      ref.kind() == TypeRefKind::TUPLE_REF && i < ref.spread().size() && ref.spread()[i];
  bool isRestParam =
      ref.kind() == TypeRefKind::FUNCTION_REF && ref.rest() && i + 2 == ref.args.size();

  return isTupleSpread || isRestParam;
}

bool ConditionalTypes::mentionsOtherParams(const Type *type, llvm::ArrayRef<const Type *> allowed,
                                           llvm::SmallPtrSetImpl<const Type *> &seen) {
  std::vector<const Type *> stack = {type};
  while (!stack.empty()) {
    const Type *next = stack.back();
    stack.pop_back();
    bool isSkipped = next == nullptr || !TypeQueries::mentionsParam(next);
    bool isPlainParam =
        !isSkipped && next->kind() == Type::Kind::Param && next->typeOp() == TypeOp::None;
    if (isPlainParam && !llvm::is_contained(allowed, next)) {
      return true;
    }
    if (!isSkipped && !isPlainParam) {
      std::vector<const Type *> parts = ConditionalTypes::otherParamParts(next, seen);
      stack.insert(stack.end(), parts.rbegin(), parts.rend());
    }
  }

  return false;
}

std::vector<const Type *>
ConditionalTypes::otherParamParts(const Type *type, llvm::SmallPtrSetImpl<const Type *> &seen) {
  switch (type->kind()) {
  case Type::Kind::Param:
    if (type->typeOp() == TypeOp::Conditional) {
      return std::vector<const Type *>(type->operands().begin() + 1, type->operands().end());
    }
    return std::vector<const Type *>(type->operands().begin(), type->operands().end());
  case Type::Kind::Optional:
  case Type::Kind::Array:
  case Type::Kind::Promise:
    return {type->inner()};
  case Type::Kind::Result:
    return {type->okType(), type->errType()};
  case Type::Kind::Function: {
    std::vector<const Type *> parts = {type->ret()};
    parts.insert(parts.end(), type->params().begin(), type->params().end());
    return parts;
  }
  case Type::Kind::Iterator:
    return {type->yielded(), type->returned(), type->sent()};
  case Type::Kind::Dict:
    return {type->keyType(), type->valueType()};
  case Type::Kind::Union:
    return std::vector<const Type *>(type->members().begin(), type->members().end());
  case Type::Kind::Object:
    return ConditionalTypes::objectParamParts(type, seen);
  default:
    return {};
  }
}

std::vector<const Type *>
ConditionalTypes::objectParamParts(const Type *type, llvm::SmallPtrSetImpl<const Type *> &seen) {
  if (type->isAsyncIterator()) {
    return {type->yielded(), type->returned(), type->sent()};
  }
  if (!type->typeArgs().empty()) {
    return std::vector<const Type *>(type->typeArgs().begin(), type->typeArgs().end());
  }
  auto [_, isFirstVisit] = seen.insert(type);
  if (!isFirstVisit) {
    return {};
  }
  std::vector<const Type *> parts = {type->tupleRest()};
  for (const Field &f : type->fields()) {
    parts.push_back(f.type);
  }

  return parts;
}

void ConditionalTypes::collectParams(const Type *type, std::vector<const Type *> &params,
                                     llvm::SmallPtrSetImpl<const Type *> &seen) {
  std::vector<const Type *> stack = {type};
  while (!stack.empty()) {
    const Type *next = stack.back();
    stack.pop_back();
    if (next == nullptr || !TypeQueries::mentionsParam(next)) {
      continue;
    }
    auto [_, isFresh] = seen.insert(next);
    if (!isFresh) {
      continue;
    }
    if (next->kind() == Type::Kind::Param && next->typeOp() == TypeOp::None) {
      params.push_back(next);
    } else {
      std::vector<const Type *> parts = ConditionalTypes::paramParts(next);
      stack.insert(stack.end(), parts.rbegin(), parts.rend());
    }
  }
}

std::vector<const Type *> ConditionalTypes::paramParts(const Type *type) {
  switch (type->kind()) {
  case Type::Kind::Optional:
  case Type::Kind::Array:
  case Type::Kind::Promise:
    return {type->inner()};
  case Type::Kind::Result:
    return {type->okType(), type->errType()};
  case Type::Kind::Function: {
    std::vector<const Type *> parts = {type->ret()};
    parts.insert(parts.end(), type->params().begin(), type->params().end());
    return parts;
  }
  case Type::Kind::Dict:
    return {type->keyType(), type->valueType()};
  case Type::Kind::Union:
    return std::vector<const Type *>(type->members().begin(), type->members().end());
  case Type::Kind::Object: {
    std::vector<const Type *> parts(type->typeArgs().begin(), type->typeArgs().end());
    for (const Field &f : type->fields()) {
      parts.push_back(f.type);
    }
    return parts;
  }
  default:
    return {};
  }
}

bool ConditionalTypes::mentionsParamBesides(const Type *type,
                                            llvm::ArrayRef<const Type *> allowed) {
  llvm::SmallPtrSet<const Type *, 4> seen;

  return ConditionalTypes::mentionsOtherParams(type, allowed, seen);
}

void ConditionalTypes::bindType(llvm::StringRef name, const Type *type, llvm::SMLoc loc) {
  auto *alias = this->state.getAst().make<TypeAliasDecl>(loc, name, TypeRef());
  alias->resolved = type;
  this->state.getScopeStack().setType(name, alias);
}

const Type *ConditionalTypes::resolveTypeArgument(const TypeRef &ref) {
  return this->typeResolver->resolveTypeInner(ref);
}

const Type *ConditionalTypes::resolveConditional(const TypeRef &ref) {
  bool isTail = this->state.getTypeContext().isTailCallAllowed();
  this->state.getTypeContext().forbidTailCall();
  PreparedConditional prepared;
  if (const Type *early = this->prepareConditional(ref, prepared)) {
    return early;
  }
  bool isBinding = ref.args[0].kind() == TypeRefKind::NAME_REF &&
                   ConditionalTypes::isBinding(this->typeDeclarer->lookupType(ref.args[0].name));
  std::vector<const Type *> members =
      isBinding ? ConditionalTypes::distributed(prepared.check) : std::vector<const Type *>();
  if (isBinding && members.size() != 1) {
    return this->distributeConditional(ref, members);
  }

  return this->chooseBranch(ref, prepared, isTail);
}

const Type *ConditionalTypes::distributeConditional(const TypeRef &ref,
                                                    llvm::ArrayRef<const Type *> members) {
  std::vector<const Type *> results;
  for (const Type *m : members) {
    this->state.getScopeStack().openTypeScope();
    this->bindType(ref.args[0].name, m, ref.loc);
    const Type *result = this->memberConditional(ref);
    this->state.getScopeStack().closeTypeScope();
    if (result->isError()) {
      return result;
    }
    results.push_back(result);
  }

  return results.empty() ? Type::never() : Type::unionOf(results);
}

const Type *ConditionalTypes::memberConditional(const TypeRef &ref) {
  this->state.getTypeContext().forbidTailCall();
  PreparedConditional prepared;
  if (const Type *early = this->prepareConditional(ref, prepared)) {
    return early;
  }

  return this->chooseBranch(ref, prepared, false);
}

const Type *ConditionalTypes::prepareConditional(const TypeRef &ref,
                                                 PreparedConditional &prepared) {
  if (ref.args.size() != 4) {
    return Type::error();
  }
  const Type *check = this->typeResolver->resolveTypeInner(ref.args[0]);
  if (check->isError()) {
    return check;
  }
  std::vector<const TypeRef *> inferRefs;
  std::vector<bool> lists;
  ConditionalTypes::collectInfers(ref.args[1], inferRefs, lists);
  std::vector<const Type *> infers;
  this->state.getScopeStack().openTypeScope();
  for (size_t i = 0; i < inferRefs.size(); ++i) {
    Type *param = Type::param(inferRefs[i]->name);
    if (lists[i]) {
      param->setList();
    }
    infers.push_back(param);
    this->bindType(inferRefs[i]->name, param, inferRefs[i]->loc);
  }
  for (size_t i = 0; i < inferRefs.size(); ++i) {
    if (inferRefs[i]->args.empty()) {
      continue;
    }
    const Type *constraint = this->typeResolver->resolveTypeInner(inferRefs[i]->args[0]);
    if (constraint->isError()) {
      this->state.getScopeStack().closeTypeScope();
      return constraint;
    }
    const_cast<Type *>(infers[i])->setConstraint(constraint, {});
  }
  this->state.getTypeContext().enterExtendsClause();
  const Type *pattern = this->typeResolver->resolveTypeInner(ref.args[1]);
  this->state.getTypeContext().leaveExtendsClause();
  this->state.getScopeStack().closeTypeScope();
  if (pattern->isError()) {
    return pattern;
  }
  if (TypeQueries::mentionsParam(check) ||
      ConditionalTypes::mentionsParamBesides(pattern, infers)) {
    return this->deferConditional(ref);
  }
  prepared = PreparedConditional{check, pattern, std::move(inferRefs), std::move(infers)};

  return nullptr;
}

const Type *ConditionalTypes::chooseBranch(const TypeRef &ref, const PreparedConditional &prepared,
                                           bool tail) {
  const Type *check = prepared.check;
  const Type *pattern = prepared.pattern;
  const std::vector<const TypeRef *> &inferRefs = prepared.inferRefs;
  const std::vector<const Type *> &infers = prepared.infers;
  InferMatch inference{infers, std::vector<const Type *>(infers.size(), nullptr)};
  bool isYes = this->extendsRelation->extendsType(check, pattern, &inference);
  this->state.getScopeStack().openTypeScope();
  for (size_t i = 0; isYes && i < infers.size(); ++i) {
    const Type *bound = inference.bound[i];
    if (!bound) {
      bound = infers[i]->constraint() ? infers[i]->constraint() : Type::unknown();
    }
    this->bindType(inferRefs[i]->name, bound, inferRefs[i]->loc);
  }
  const TypeRef &branch = ref.args[isYes ? 2 : 3];
  const Type *result = nullptr;
  if (tail && branch.kind() == TypeRefKind::CONDITIONAL_REF) {
    this->state.getTypeContext().allowTailCall();
    result = this->typeResolver->resolveTypeInner(branch);
  } else if (auto *alias = tail && branch.kind() == TypeRefKind::GENERIC_REF
                               ? llvm::dyn_cast_or_null<TypeAliasDecl>(
                                     this->typeDeclarer->lookupType(branch.name))
                               : nullptr;
             alias && !alias->typeParams.empty()) {
    std::vector<const Type *> args;
    for (const TypeRef &arg : branch.args) {
      args.push_back(this->resolveTypeArgument(arg));
      if (args.back()->isError()) {
        this->state.getScopeStack().closeTypeScope();

        return args.back();
      }
    }
    this->state.getTypeContext().rememberTailCall(alias, std::move(args));
  } else {
    result = this->typeResolver->resolveTypeInner(branch);
  }
  this->state.getScopeStack().closeTypeScope();

  return result;
}

const Type *ConditionalTypes::deferConditional(const TypeRef &ref) {
  const ConditionalInfo *known = this->state.getTypeContext().findConditional(&ref);
  if (!known) {
    ConditionalInfo info;
    info.ref = &ref;
    std::string spelling;
    for (char c : ref.source) {
      if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
        if (!spelling.empty() && spelling.back() != ' ') {
          spelling += ' ';
        }
      } else {
        spelling += c;
      }
    }
    while (!spelling.empty() && spelling.back() == ' ') {
      spelling.pop_back();
    }
    info.tag = Type::param(spelling.empty() ? "conditional type" : spelling);
    std::vector<llvm::StringRef> names;
    ConditionalTypes::collectNames(ref, names);
    for (llvm::StringRef name : names) {
      Stmt *decl = this->typeDeclarer->lookupType(name);
      if (!decl) {
        continue;
      }
      if (ConditionalTypes::isBinding(decl)) {
        info.bound.push_back(name);
      } else {
        info.decls.push_back({name, decl});
      }
    }
    known = this->state.getTypeContext().addConditional(std::move(info));
  }
  std::vector<const Type *> operands{known->tag};
  for (llvm::StringRef name : known->bound) {
    auto *alias = llvm::dyn_cast_or_null<TypeAliasDecl>(this->typeDeclarer->lookupType(name));
    operands.push_back(alias && alias->resolved ? alias->resolved : Type::error());
  }

  return Type::typeOp(TypeOp::Conditional, operands);
}

const Type *ConditionalTypes::evalConditional(llvm::ArrayRef<const Type *> operands) {
  const ConditionalInfo *info = this->state.getTypeContext().findConditionalByTag(operands[0]);
  if (!info) {
    return Type::error();
  }
  for (const Type *t : operands.drop_front()) {
    if (t->isError()) {
      return t;
    }
  }
  this->state.getScopeStack().openTypeScope();
  for (const auto &[name, decl] : info->decls) {
    this->state.getScopeStack().setType(name, decl);
  }
  for (size_t i = 0; i < info->bound.size() && i + 1 < operands.size(); ++i) {
    this->bindType(info->bound[i], operands[i + 1], info->ref->loc);
  }
  TypeContext &types = this->state.getTypeContext();
  bool isTail = types.isTailCallAllowed();
  types.forbidTailCall();
  const Type *result = this->resolveConditional(*info->ref);
  if (isTail) {
    types.allowTailCall();
  } else {
    types.forbidTailCall();
  }
  this->state.getScopeStack().closeTypeScope();

  return result ? result : Type::error();
}

std::vector<const Type *> ConditionalTypes::conditionalBranches(const Type *conditional) {
  if (conditional->typeOp() != TypeOp::Conditional) {
    return {};
  }
  llvm::ArrayRef<const Type *> operands = conditional->operands();
  const ConditionalInfo *info = this->state.getTypeContext().findConditionalByTag(operands[0]);
  if (!info) {
    return {};
  }
  this->state.getScopeStack().openTypeScope();
  for (const auto &[name, decl] : info->decls) {
    this->state.getScopeStack().setType(name, decl);
  }
  for (size_t i = 0; i < info->bound.size() && i + 1 < operands.size(); ++i) {
    this->bindType(info->bound[i], operands[i + 1], info->ref->loc);
  }
  std::vector<const TypeRef *> inferRefs;
  std::vector<bool> lists;
  ConditionalTypes::collectInfers(info->ref->args[1], inferRefs, lists);
  for (size_t i = 0; i < inferRefs.size(); ++i) {
    Type *param = Type::param(inferRefs[i]->name);
    if (lists[i]) {
      param->setList();
    }
    this->bindType(inferRefs[i]->name, param, inferRefs[i]->loc);
  }
  this->state.getDiagnostics().pushList(DiagnosticRoute::DISCARD);
  std::vector<const Type *> branches;
  for (size_t i : {2, 3}) {
    const Type *branch = this->typeResolver->resolveTypeInner(info->ref->args[i]);
    if (branch && !branch->isError()) {
      branches.push_back(branch);
    }
  }
  this->state.getDiagnostics().popList();
  this->state.getScopeStack().closeTypeScope();

  return branches;
}

bool ConditionalTypes::fitsConditional(const Type *from, const Type *to, unsigned depth) {
  std::vector<FitsStep> steps = {FitsGoal{from, to, depth}};
  while (!steps.empty()) {
    FitsStep step = std::move(steps.back());
    steps.pop_back();
    const FitsGoal *goal = step.as<FitsGoal>();
    bool isHolding = goal == nullptr || this->fitsStep(*goal, steps);
    if (!isHolding && !this->retryFits(steps)) {
      return false;
    }
  }

  return true;
}

bool ConditionalTypes::fitsStep(const FitsGoal &goal, std::vector<FitsStep> &steps) {
  if (goal.depth > 8 || goal.to->typeOp() != TypeOp::Conditional) {
    return false;
  }
  if (goal.from->isUnion()) {
    for (const Type *m : llvm::reverse(goal.from->members())) {
      steps.push_back(FitsGoal{m, goal.to, goal.depth + 1});
    }
    return true;
  }
  std::vector<const Type *> branches = this->conditionalBranches(goal.to);

  return this->tryFitsAlternative(FitsChoice{goal.from, goal.depth, std::move(branches), 0}, steps);
}

bool ConditionalTypes::retryFits(std::vector<FitsStep> &steps) {
  while (!steps.empty()) {
    FitsStep step = std::move(steps.back());
    steps.pop_back();
    FitsChoice *choice = step.as<FitsChoice>();
    if (choice != nullptr && this->tryFitsAlternative(std::move(*choice), steps)) {
      return true;
    }
  }

  return false;
}

bool ConditionalTypes::tryFitsAlternative(FitsChoice choice, std::vector<FitsStep> &steps) {
  while (choice.next < choice.branches.size() * 3) {
    const Type *branch = choice.branches[choice.next / 3];
    size_t way = choice.next % 3;
    choice.next += 1;
    if (way == 0 && TypeRelations::isImplicitlyConvertible(choice.from, branch)) {
      return true;
    }
    if (way == 1) {
      const Type *from = choice.from;
      unsigned depth = choice.depth;
      steps.push_back(std::move(choice));
      steps.push_back(FitsGoal{from, branch, depth + 1});
      return true;
    }
    if (way == 2 && this->extendsWithParams(choice.from, branch)) {
      return true;
    }
  }

  return false;
}

bool ConditionalTypes::extendsWithParams(const Type *from, const Type *branch) {
  std::vector<const Type *> params;
  llvm::SmallPtrSet<const Type *, 8> seen;
  ConditionalTypes::collectParams(branch, params, seen);
  InferMatch match{params, std::vector<const Type *>(params.size(), nullptr)};

  return !params.empty() && this->extendsRelation->extendsType(from, branch, &match);
}

ConditionalTypes::ResultIntersectionJoined
ConditionalTypes::resolveIntersection(const TypeRef &ref) {
  std::vector<const Type *> parts;
  for (const TypeRef &arg : ref.args) {
    parts.push_back(this->typeResolver->resolveTypeInner(arg));
    if (parts.back()->isError()) {
      return ok(parts.back());
    }
  }
  auto primitive = [](const Type *t) {
    return (t->isString() && !t->isPattern()) || t->isNumeric() || t->isBool();
  };
  auto emptyObject = [](const Type *t) {
    return t->isObject() && !t->isClass() && !t->isTuple() && !t->isAnyObject() &&
           t->fields().empty() && t->overloads().empty();
  };
  if (parts.size() == 2) {
    for (size_t i : {0, 1}) {
      const Type *other = parts[1 - i];
      if (primitive(parts[i])) {
        const Type *extracted = this->typeOperators->evalTypeOp(
            Type::typeOp(TypeOp::Extract, {other, parts[i]}), {other, parts[i]});
        return ok(extracted);
      }
      if (emptyObject(parts[i])) {
        const Type *defined =
            this->typeOperators->evalTypeOp(Type::typeOp(TypeOp::NonNullable, {other}), {other});
        return ok(defined);
      }
    }
  }
  auto plainObject = [](const Type *t) {
    return t->isObject() && !t->isClass() && !t->isTuple() && !t->isAnyObject() &&
           t->overloads().empty();
  };
  std::vector<std::vector<const Type *>> choices;
  bool hasObjects = true;
  for (const Type *part : parts) {
    llvm::ArrayRef<const Type *> members =
        part->isUnion() ? part->members() : llvm::ArrayRef<const Type *>(part);
    hasObjects &= llvm::all_of(members, plainObject);
    choices.emplace_back(members.begin(), members.end());
  }
  if (hasObjects) {
    std::vector<const Type *> merged{nullptr};
    for (const std::vector<const Type *> &choice : choices) {
      std::vector<const Type *> next;
      for (const Type *left : merged) {
        for (const Type *right : choice) {
          std::vector<Field> fields;
          if (left) {
            fields.assign(left->fields().begin(), left->fields().end());
          }
          for (const Field &field : right->fields()) {
            auto same = llvm::find_if(fields, [&](const Field &f) { return f.name == field.name; });
            if (same == fields.end()) {
              fields.push_back(field);
              continue;
            }
            const Type *a = same->type;
            const Type *b = field.type;
            const Type *x = a->nonOptional();
            const Type *y = b->nonOptional();
            const Type *both = nullptr;
            if (x == y) {
              both = x;
            } else if (x->isLiteral() && x->literalBase() == y) {
              both = x;
            } else if (y->isLiteral() && y->literalBase() == x) {
              both = y;
            }
            if (!both) {
              std::string message =
                  ErrorTextIntersection::fieldTypes(field.name, x->name(), y->name());

              return err(ConditionalTypesResolveIntersectionError{
                  .code = ConditionalTypesResolveIntersectionErrorCode::FIELD_TYPES_DIFFER,
                  .message = message,
              });
            }
            same->type = a->isOptional() && b->isOptional() ? Type::optional(both) : both;
            same->readonly |= field.readonly;
          }
          next.push_back(Type::object(std::move(fields)));
        }
      }
      merged = std::move(next);
    }
    return ok(Type::unionOf(merged));
  }
  const Type *other = nullptr;
  for (const std::vector<const Type *> &choice : choices) {
    for (const Type *t : choice) {
      if (!other && !plainObject(t)) {
        other = t;
      }
    }
  }
  std::string message = ErrorTextIntersection::notObject(other->name());

  return err(ConditionalTypesResolveIntersectionError{
      .code = ConditionalTypesResolveIntersectionErrorCode::NOT_AN_OBJECT,
      .message = message,
  });
}

const Type *ConditionalTypes::signatureForUtility(const TypeRef &ref) {
  if (ref.kind() != TypeRefKind::TYPE_OF_REF || ref.names().empty()) {
    return nullptr;
  }
  auto erased = [&](FuncSymbol *fn) {
    const Type *type = this->returnInferrer->functionType(fn);
    std::vector<const Type *> args;
    for (const Type *p : fn->typeParams) {
      if (p->constraint()) {
        args.push_back(p->constraint());
      } else if (!p->alternatives().empty()) {
        args.push_back(Type::unionOf(p->alternatives()));
      } else if (p->isListParam()) {
        args.push_back(Type::array(Type::unknown()));
      } else {
        args.push_back(Type::unknown());
      }
    }

    return this->typeParams->substitute(type, fn->typeParams, args);
  };
  if (ref.names().size() == 1) {
    ScopeEntry entry = this->topLevelChecker->lookupValue(ref.names().front());
    if (entry.func() && entry.func()->isGeneric()) {
      return erased(entry.func());
    }
    return nullptr;
  }
  if (ref.names().size() != 2) {
    return nullptr;
  }
  VarSymbol *var = this->topLevelChecker->lookupValue(ref.names().front()).var();
  if (!var || !var->type) {
    return nullptr;
  }
  const Type *object = var->type->nonOptional();
  if (const Overload *overload = object->isObject() ? object->overload(ref.names()[1]) : nullptr) {
    return overload->signatures.back();
  }
  ClassDecl *cls = ClassDeclarer::classOf(object);
  if (!cls || object->field(ref.names()[1])) {
    return nullptr;
  }
  this->classDeclarer->completeClass(cls);
  std::vector<FunctionDecl *> methods = ClassDeclarer::methodsNamed(cls, ref.names()[1], false);
  if (methods.empty() || !methods.back()->sym) {
    return nullptr;
  }

  return erased(methods.back()->sym);
}

}
