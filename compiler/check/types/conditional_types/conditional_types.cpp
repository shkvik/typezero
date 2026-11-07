#include "compiler/check/types/conditional_types/conditional_types.h"
#include "compiler/check/generics/type_params/type_params.h"
#include "compiler/check/top_level_checker/top_level_checker.h"
#include "compiler/check/types/conditional_check_helpers/conditional_check_helpers.h"
#include "compiler/check/types/extends_relation/extends_relation.h"
#include "compiler/check/types/type_operators/type_operators.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/check/types/type_resolver/type_resolver.h"
#include "compiler/declare/class_declarer/class_declarer.h"
#include "compiler/declare/type_declarer/type_declarer.h"
#include "compiler/infer/return_inferrer/return_inferrer.h"
#include "compiler/whole_program/flows/flows.h"
#include "llvm/ADT/STLExtras.h"
#include <cmath>


namespace tz {

namespace {

bool isBinding(const Stmt *decl) {
  auto *alias = llvm::dyn_cast_or_null<TypeAliasDecl>(decl);

  return alias && alias->resolved && alias->typeParams.empty() &&
         alias->type.kind() == TypeRefKind::NAME_REF && alias->type.name.empty();
}

std::vector<const Type *> distributed(const Type *type) {
  std::vector<const Type *> out;
  distribute(type, out);

  return out;
}

void collectNames(const TypeRef &ref, std::vector<llvm::StringRef> &names) {
  if ((ref.kind() == TypeRefKind::NAME_REF || ref.kind() == TypeRefKind::GENERIC_REF) &&
      !ref.name.empty() && !llvm::is_contained(names, ref.name)) {
    names.push_back(ref.name);
  }
  for (const TypeRef &arg : ref.args) {
    collectNames(arg, names);
  }
}

void collectInfers(const TypeRef &ref, std::vector<const TypeRef *> &infers,
                   std::vector<bool> &lists, bool spread = false) {
  if (ref.kind() == TypeRefKind::CONDITIONAL_REF) {
    return;
  }
  if (ref.kind() == TypeRefKind::INFER_REF) {
    auto found = llvm::find_if(infers, [&](const TypeRef *r) { return r->name == ref.name; });
    if (found == infers.end()) {
      infers.push_back(&ref);
      lists.push_back(spread);
    } else if (spread) {
      lists[size_t(found - infers.begin())] = true;
    }
  }
  for (size_t i = 0; i < ref.args.size(); ++i) {
    bool spreadHere =
        (ref.kind() == TypeRefKind::TUPLE_REF && i < ref.spread().size() && ref.spread()[i]) ||
        (ref.kind() == TypeRefKind::FUNCTION_REF && ref.rest() && i + 2 == ref.args.size());
    collectInfers(ref.args[i], infers, lists, spreadHere);
  }
}

bool mentionsOtherParams(const Type *type, llvm::ArrayRef<const Type *> allowed,
                         llvm::SmallPtrSetImpl<const Type *> &seen) {
  if (!mentionsParam(type)) {
    return false;
  }
  auto other = [&](const Type *t) { return t && mentionsOtherParams(t, allowed, seen); };
  switch (type->kind()) {
  case Type::Kind::Param:
    if (type->typeOp() == TypeOp::Conditional) {
      return llvm::any_of(type->operands().drop_front(), other);
    }
    if (type->typeOp() != TypeOp::None) {
      return llvm::any_of(type->operands(), other);
    }
    return !llvm::is_contained(allowed, type);
  case Type::Kind::Optional:
  case Type::Kind::Array:
  case Type::Kind::Promise:
    return other(type->inner());
  case Type::Kind::Result:
    return other(type->okType()) || other(type->errType());
  case Type::Kind::Function:
    return other(type->ret()) || llvm::any_of(type->params(), other);
  case Type::Kind::Iterator:
    return other(type->yielded()) || other(type->returned()) || other(type->sent());
  case Type::Kind::Dict:
    return other(type->keyType()) || other(type->valueType());
  case Type::Kind::Union:
    return llvm::any_of(type->members(), other);
  case Type::Kind::Object:
    if (type->isAsyncIterator()) {
      return other(type->yielded()) || other(type->returned()) || other(type->sent());
    }
    if (!type->typeArgs().empty()) {
      return llvm::any_of(type->typeArgs(), other);
    }
    if (!seen.insert(type).second) {
      return false;
    }
    if (other(type->tupleRest())) {
      return true;
    }
    return llvm::any_of(type->fields(), [&](const Field &f) { return other(f.type); });
  default:
    return false;
  }
}

void collectParams(const Type *type, std::vector<const Type *> &params,
                   llvm::SmallPtrSetImpl<const Type *> &seen) {
  if (!mentionsParam(type) || !seen.insert(type).second) {
    return;
  }
  auto add = [&](const Type *t) {
    if (t) {
      collectParams(t, params, seen);
    }
  };
  switch (type->kind()) {
  case Type::Kind::Param:
    if (type->typeOp() == TypeOp::None) {
      params.push_back(type);
    }
    break;
  case Type::Kind::Optional:
  case Type::Kind::Array:
  case Type::Kind::Promise:
    add(type->inner());
    break;
  case Type::Kind::Result:
    add(type->okType());
    add(type->errType());
    break;
  case Type::Kind::Function:
    add(type->ret());
    for (const Type *p : type->params()) {
      add(p);
    }
    break;
  case Type::Kind::Dict:
    add(type->keyType());
    add(type->valueType());
    break;
  case Type::Kind::Union:
    for (const Type *m : type->members()) {
      add(m);
    }
    break;
  case Type::Kind::Object:
    for (const Type *t : type->typeArgs()) {
      add(t);
    }
    for (const Field &f : type->fields()) {
      add(f.type);
    }
    break;
  default:
    break;
  }
}

}

bool mentionsParamBesides(const Type *type, llvm::ArrayRef<const Type *> allowed) {
  llvm::SmallPtrSet<const Type *, 4> seen;

  return mentionsOtherParams(type, allowed, seen);
}

ConditionalTypes::ConditionalTypes(Checker &owner)
    : checker(owner) {}

void ConditionalTypes::bindType(llvm::StringRef name, const Type *type, llvm::SMLoc loc) {
  auto *alias = this->checker.ctx_.make<TypeAliasDecl>(loc, name, TypeRef());
  alias->resolved = type;
  this->checker.typeScopes_.back()[name] = alias;
}

const Type *ConditionalTypes::resolveTypeArgument(const TypeRef &ref) {
  return this->checker.typeResolver().resolveTypeInner(ref);
}

const Type *ConditionalTypes::resolveConditional(const TypeRef &ref) {
  bool tail = std::exchange(this->checker.tailAllowed_, false);
  if (ref.args.size() != 4) {
    return Type::error();
  }
  const Type *check = this->checker.typeResolver().resolveTypeInner(ref.args[0]);
  if (check->isError()) {
    return check;
  }
  std::vector<const TypeRef *> inferRefs;
  std::vector<bool> lists;
  collectInfers(ref.args[1], inferRefs, lists);
  std::vector<const Type *> infers;
  this->checker.typeScopes_.emplace_back();
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
    const Type *constraint = this->checker.typeResolver().resolveTypeInner(inferRefs[i]->args[0]);
    if (constraint->isError()) {
      this->checker.typeScopes_.pop_back();
      return constraint;
    }
    const_cast<Type *>(infers[i])->setConstraint(constraint, {});
  }
  ++this->checker.extendsClause_;
  const Type *pattern = this->checker.typeResolver().resolveTypeInner(ref.args[1]);
  --this->checker.extendsClause_;
  this->checker.typeScopes_.pop_back();
  if (pattern->isError()) {
    return pattern;
  }
  if (mentionsParam(check) || mentionsParamBesides(pattern, infers)) {
    return this->deferConditional(ref);
  }
  if (ref.args[0].kind() == TypeRefKind::NAME_REF &&
      isBinding(this->checker.typeDeclarer().lookupType(ref.args[0].name))) {
    std::vector<const Type *> members = distributed(check);
    if (members.size() != 1) {
      std::vector<const Type *> results;
      for (const Type *m : members) {
        this->checker.typeScopes_.emplace_back();
        this->bindType(ref.args[0].name, m, ref.loc);
        const Type *result = this->resolveConditional(ref);
        this->checker.typeScopes_.pop_back();
        if (result->isError()) {
          return result;
        }
        results.push_back(result);
      }
      return results.empty() ? Type::never() : Type::unionOf(results);
    }
  }
  Checker::InferMatch inference{infers, std::vector<const Type *>(infers.size(), nullptr)};
  bool yes = this->checker.extendsRelation().extendsType(check, pattern, &inference);
  this->checker.typeScopes_.emplace_back();
  for (size_t i = 0; yes && i < infers.size(); ++i) {
    const Type *bound = inference.bound[i];
    if (!bound) {
      bound = infers[i]->constraint() ? infers[i]->constraint() : Type::unknown();
    }
    this->bindType(inferRefs[i]->name, bound, inferRefs[i]->loc);
  }
  const TypeRef &branch = ref.args[yes ? 2 : 3];
  const Type *result = nullptr;
  if (tail && branch.kind() == TypeRefKind::CONDITIONAL_REF) {
    this->checker.tailAllowed_ = true;
    result = this->checker.typeResolver().resolveTypeInner(branch);
  } else if (auto *alias = tail && branch.kind() == TypeRefKind::GENERIC_REF
                               ? llvm::dyn_cast_or_null<TypeAliasDecl>(
                                     this->checker.typeDeclarer().lookupType(branch.name))
                               : nullptr;
             alias && !alias->typeParams.empty()) {
    std::vector<const Type *> args;
    for (const TypeRef &arg : branch.args) {
      args.push_back(this->resolveTypeArgument(arg));
      if (args.back()->isError()) {
        this->checker.typeScopes_.pop_back();

        return args.back();
      }
    }
    this->checker.tailCall_ = Checker::TailCall{alias, std::move(args)};
  } else {
    result = this->checker.typeResolver().resolveTypeInner(branch);
  }
  this->checker.typeScopes_.pop_back();

  return result;
}

const Type *ConditionalTypes::deferConditional(const TypeRef &ref) {
  Checker::ConditionalInfo &info = this->conditionals[&ref];
  if (!info.tag) {
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
    this->conditionalTags[info.tag] = &info;
    std::vector<llvm::StringRef> names;
    collectNames(ref, names);
    for (llvm::StringRef name : names) {
      Stmt *decl = this->checker.typeDeclarer().lookupType(name);
      if (!decl) {
        continue;
      }
      if (isBinding(decl)) {
        info.bound.push_back(name);
      } else {
        info.decls.push_back({name, decl});
      }
    }
  }
  std::vector<const Type *> operands{info.tag};
  for (llvm::StringRef name : info.bound) {
    auto *alias =
        llvm::dyn_cast_or_null<TypeAliasDecl>(this->checker.typeDeclarer().lookupType(name));
    operands.push_back(alias && alias->resolved ? alias->resolved : Type::error());
  }

  return Type::typeOp(TypeOp::Conditional, operands);
}

const Type *ConditionalTypes::evalConditional(llvm::ArrayRef<const Type *> operands) {
  const Checker::ConditionalInfo *info = this->conditionalTags.lookup(operands[0]);
  if (!info) {
    return Type::error();
  }
  for (const Type *t : operands.drop_front()) {
    if (t->isError()) {
      return t;
    }
  }
  this->checker.typeScopes_.emplace_back();
  for (const auto &[name, decl] : info->decls) {
    this->checker.typeScopes_.back()[name] = decl;
  }
  for (size_t i = 0; i < info->bound.size() && i + 1 < operands.size(); ++i) {
    this->bindType(info->bound[i], operands[i + 1], info->ref->loc);
  }
  bool tail = std::exchange(this->checker.tailAllowed_, false);
  const Type *result = this->resolveConditional(*info->ref);
  this->checker.tailAllowed_ = tail;
  this->checker.typeScopes_.pop_back();

  return result ? result : Type::error();
}

std::vector<const Type *> ConditionalTypes::conditionalBranches(const Type *conditional) {
  if (conditional->typeOp() != TypeOp::Conditional) {
    return {};
  }
  llvm::ArrayRef<const Type *> operands = conditional->operands();
  const Checker::ConditionalInfo *info = this->conditionalTags.lookup(operands[0]);
  if (!info) {
    return {};
  }
  this->checker.typeScopes_.emplace_back();
  for (const auto &[name, decl] : info->decls) {
    this->checker.typeScopes_.back()[name] = decl;
  }
  for (size_t i = 0; i < info->bound.size() && i + 1 < operands.size(); ++i) {
    this->bindType(info->bound[i], operands[i + 1], info->ref->loc);
  }
  std::vector<const TypeRef *> inferRefs;
  std::vector<bool> lists;
  collectInfers(info->ref->args[1], inferRefs, lists);
  for (size_t i = 0; i < inferRefs.size(); ++i) {
    Type *param = Type::param(inferRefs[i]->name);
    if (lists[i]) {
      param->setList();
    }
    this->bindType(inferRefs[i]->name, param, inferRefs[i]->loc);
  }
  bool muted = this->checker.diags_.setMuted(true);
  std::vector<const Type *> branches;
  for (size_t i : {2, 3}) {
    const Type *branch = this->checker.typeResolver().resolveTypeInner(info->ref->args[i]);
    if (branch && !branch->isError()) {
      branches.push_back(branch);
    }
  }
  this->checker.diags_.setMuted(muted);
  this->checker.typeScopes_.pop_back();

  return branches;
}

bool ConditionalTypes::fitsConditional(const Type *from, const Type *to, unsigned depth) {
  if (depth > 8 || to->typeOp() != TypeOp::Conditional) {
    return false;
  }
  if (from->isUnion()) {
    return llvm::all_of(from->members(),
                        [&](const Type *m) { return this->fitsConditional(m, to, depth + 1); });
  }
  for (const Type *branch : this->conditionalBranches(to)) {
    if (TypeRelations::isImplicitlyConvertible(from, branch) ||
        this->fitsConditional(from, branch, depth + 1)) {
      return true;
    }
    std::vector<const Type *> params;
    llvm::SmallPtrSet<const Type *, 8> seen;
    collectParams(branch, params, seen);
    Checker::InferMatch match{params, std::vector<const Type *>(params.size(), nullptr)};
    if (!params.empty() && this->checker.extendsRelation().extendsType(from, branch, &match)) {
      return true;
    }
  }

  return false;
}

ConditionalTypes::ResultIntersectionJoined
ConditionalTypes::resolveIntersection(const TypeRef &ref) {
  std::vector<const Type *> parts;
  for (const TypeRef &arg : ref.args) {
    parts.push_back(this->checker.typeResolver().resolveTypeInner(arg));
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
        const Type *extracted = this->checker.typeOperators().evalTypeOp(
            Type::typeOp(TypeOp::Extract, {other, parts[i]}), {other, parts[i]});
        return ok(extracted);
      }
      if (emptyObject(parts[i])) {
        const Type *defined = this->checker.typeOperators().evalTypeOp(
            Type::typeOp(TypeOp::NonNullable, {other}), {other});
        return ok(defined);
      }
    }
  }
  auto plainObject = [](const Type *t) {
    return t->isObject() && !t->isClass() && !t->isTuple() && !t->isAnyObject() &&
           t->overloads().empty();
  };
  std::vector<std::vector<const Type *>> choices;
  bool objects = true;
  for (const Type *part : parts) {
    llvm::ArrayRef<const Type *> members =
        part->isUnion() ? part->members() : llvm::ArrayRef<const Type *>(part);
    objects &= llvm::all_of(members, plainObject);
    choices.emplace_back(members.begin(), members.end());
  }
  if (objects) {
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
    const Type *type = this->checker.returnInferrer().functionType(fn);
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

    return this->checker.typeParams().substitute(type, fn->typeParams, args);
  };
  if (ref.names().size() == 1) {
    ScopeEntry entry = this->checker.topLevelChecker().lookupValue(ref.names().front());
    if (entry.func() && entry.func()->isGeneric()) {
      return erased(entry.func());
    }
    return nullptr;
  }
  if (ref.names().size() != 2) {
    return nullptr;
  }
  VarSymbol *var = this->checker.topLevelChecker().lookupValue(ref.names().front()).var();
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
  this->checker.classDeclarer().completeClass(cls);
  std::vector<FunctionDecl *> methods = ClassDeclarer::methodsNamed(cls, ref.names()[1], false);
  if (methods.empty() || !methods.back()->sym) {
    return nullptr;
  }

  return erased(methods.back()->sym);
}

}
