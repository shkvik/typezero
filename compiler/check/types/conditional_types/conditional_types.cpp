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
#include "llvm/Support/ConvertUTF.h"
#include <cmath>

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

namespace {

// A name bound to a type, as a type parameter is (Checker::bindTypeParams),
// rather than declared.
bool isBinding(const Stmt *decl) {
  auto *alias = llvm::dyn_cast_or_null<TypeAliasDecl>(decl);

  return alias && alias->resolved && alias->typeParams.empty() &&
         alias->type.kind == TypeRef::Kind::Name && alias->type.name.empty();
}

std::vector<const Type *> distributed(const Type *type) {
  std::vector<const Type *> out;
  distribute(type, out);

  return out;
}

// The names of types that a type written uses, but `infer` and the keys of
// mapped types, which it declares.
void collectNames(const TypeRef &ref, std::vector<llvm::StringRef> &names) {
  if ((ref.kind == TypeRef::Kind::Name || ref.kind == TypeRef::Kind::Generic) &&
      !ref.name.empty() && !llvm::is_contained(names, ref.name)) {
    names.push_back(ref.name);
  }
  for (const TypeRef &arg : ref.args) {
    collectNames(arg, names);
  }
}

// The types `infer` declares in the pattern of a conditional type (not in
// one nested there, which has its own), and whether each is spread into a
// tuple or into the parameters of a function: `[infer H, ...infer R]` makes R
// a list of types.
void collectInfers(const TypeRef &ref, std::vector<const TypeRef *> &infers,
                   std::vector<bool> &lists, bool spread = false) {
  if (ref.kind == TypeRef::Kind::Conditional) {
    return;
  }
  if (ref.kind == TypeRef::Kind::Infer) {
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
        (ref.kind == TypeRef::Kind::Tuple && i < ref.spread.size() && ref.spread[i]) ||
        (ref.kind == TypeRef::Kind::Function && ref.rest && i + 2 == ref.args.size());
    collectInfers(ref.args[i], infers, lists, spreadHere);
  }
}

// See mentionsParamBesides.
bool mentionsOtherParams(const Type *type, llvm::ArrayRef<const Type *> allowed,
                         llvm::SmallPtrSetImpl<const Type *> &seen) {
  if (!mentionsParam(type)) {
    return false;
  }
  auto other = [&](const Type *t) { return t && mentionsOtherParams(t, allowed, seen); };
  switch (type->kind()) {
  case Type::Kind::Param:
    // A conditional type that waits, by what its names stand for.
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

// The type parameters `type` mentions, but those of the computations in it.
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

} // namespace

bool mentionsParamBesides(const Type *type, llvm::ArrayRef<const Type *> allowed) {
  llvm::SmallPtrSet<const Type *, 4> seen;

  return mentionsOtherParams(type, allowed, seen);
}

ConditionalTypes::ConditionalTypes(Checker &checker) : checker_(checker) {}

void ConditionalTypes::bindType(llvm::StringRef name, const Type *type, llvm::SMLoc loc) {
  auto *alias = checker_.ctx_.make<TypeAliasDecl>(loc, name, TypeRef());
  alias->resolved = type;
  checker_.typeScopes_.back()[name] = alias;
}

const Type *ConditionalTypes::resolveTypeArgument(const TypeRef &ref) {
  return checker_.typeResolver().resolveTypeInner(ref);
}

const Type *ConditionalTypes::resolveConditional(const TypeRef &ref) {
  bool tail = std::exchange(checker_.tailAllowed_, false);
  if (ref.args.size() != 4) {
    return Type::error(); // the parser reported it
  }
  const Type *check = checker_.typeResolver().resolveTypeInner(ref.args[0]);
  if (check->isError()) {
    return check;
  }
  // The types `infer` declares: type parameters of the pattern, which the
  // match gives types.
  std::vector<const TypeRef *> inferRefs;
  std::vector<bool> lists;
  collectInfers(ref.args[1], inferRefs, lists);
  std::vector<const Type *> infers;
  checker_.typeScopes_.emplace_back();
  for (size_t i = 0; i < inferRefs.size(); ++i) {
    Type *param = Type::param(inferRefs[i]->name);
    if (lists[i]) {
      param->setList();
    }
    infers.push_back(param);
    bindType(inferRefs[i]->name, param, inferRefs[i]->loc);
  }
  for (size_t i = 0; i < inferRefs.size(); ++i) {
    if (inferRefs[i]->args.empty()) {
      continue;
    }
    const Type *constraint = checker_.typeResolver().resolveTypeInner(inferRefs[i]->args[0]);
    if (constraint->isError()) {
      checker_.typeScopes_.pop_back();
      return constraint;
    }
    const_cast<Type *>(infers[i])->setConstraint(constraint, {});
  }
  ++checker_.extendsClause_;
  const Type *pattern = checker_.typeResolver().resolveTypeInner(ref.args[1]);
  --checker_.extendsClause_;
  checker_.typeScopes_.pop_back();
  if (pattern->isError()) {
    return pattern;
  }
  // Over type parameters it waits for a copy.
  if (mentionsParam(check) || mentionsParamBesides(pattern, infers)) {
    return deferConditional(ref);
  }
  // A type parameter that stands for a union gives the union of what each
  // of its members gives, as in TypeScript; the check of a type parameter
  // that stands for never gives never.
  if (ref.args[0].kind == TypeRef::Kind::Name &&
      isBinding(checker_.typeDeclarer().lookupType(ref.args[0].name))) {
    std::vector<const Type *> members = distributed(check);
    if (members.size() != 1) {
      std::vector<const Type *> results;
      for (const Type *m : members) {
        checker_.typeScopes_.emplace_back();
        bindType(ref.args[0].name, m, ref.loc);
        const Type *result = resolveConditional(ref);
        checker_.typeScopes_.pop_back();
        if (result->isError()) {
          return result;
        }
        results.push_back(result);
      }
      return results.empty() ? Type::never() : Type::unionOf(results);
    }
  }
  Checker::InferMatch inference{infers, std::vector<const Type *>(infers.size(), nullptr)};
  bool yes = checker_.extendsRelation().extendsType(check, pattern, &inference);
  checker_.typeScopes_.emplace_back();
  // What `infer` matched; one that matched nothing is its constraint, or
  // unknown.
  for (size_t i = 0; yes && i < infers.size(); ++i) {
    const Type *bound = inference.bound[i];
    if (!bound) {
      bound = infers[i]->constraint() ? infers[i]->constraint() : Type::unknown();
    }
    bindType(inferRefs[i]->name, bound, inferRefs[i]->loc);
  }
  const TypeRef &branch = ref.args[yes ? 2 : 3];
  const Type *result = nullptr;
  if (tail && branch.kind == TypeRef::Kind::Conditional) {
    checker_.tailAllowed_ = true;
    result = checker_.typeResolver().resolveTypeInner(branch);
  } else if (auto *alias = tail && branch.kind == TypeRef::Kind::Generic
                               ? llvm::dyn_cast_or_null<TypeAliasDecl>(
                                     checker_.typeDeclarer().lookupType(branch.name))
                               : nullptr;
             alias && !alias->typeParams.empty()) {
    // A tail call: instantiateAlias goes on with it.
    std::vector<const Type *> args;
    for (const TypeRef &arg : branch.args) {
      args.push_back(resolveTypeArgument(arg));
      if (args.back()->isError()) {
        checker_.typeScopes_.pop_back();

        return args.back();
      }
    }
    checker_.tailCall_ = Checker::TailCall{alias, std::move(args)};
  } else {
    result = checker_.typeResolver().resolveTypeInner(branch);
  }
  checker_.typeScopes_.pop_back();

  return result;
}

const Type *ConditionalTypes::deferConditional(const TypeRef &ref) {
  Checker::ConditionalInfo &info = conditionals_[&ref];
  if (!info.tag) {
    info.ref = &ref;
    // Named by its text, the spaces in it collapsed.
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
    conditionalTags_[info.tag] = &info;
    std::vector<llvm::StringRef> names;
    collectNames(ref, names);
    for (llvm::StringRef name : names) {
      Stmt *decl = checker_.typeDeclarer().lookupType(name);
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
    auto *alias = llvm::dyn_cast_or_null<TypeAliasDecl>(checker_.typeDeclarer().lookupType(name));
    operands.push_back(alias && alias->resolved ? alias->resolved : Type::error());
  }

  return Type::typeOp(TypeOp::Conditional, operands);
}

const Type *ConditionalTypes::evalConditional(llvm::ArrayRef<const Type *> operands) {
  const Checker::ConditionalInfo *info = conditionalTags_.lookup(operands[0]);
  if (!info) {
    return Type::error();
  }
  for (const Type *t : operands.drop_front()) {
    if (t->isError()) {
      return t;
    }
  }
  checker_.typeScopes_.emplace_back();
  for (const auto &[name, decl] : info->decls) {
    checker_.typeScopes_.back()[name] = decl;
  }
  for (size_t i = 0; i < info->bound.size() && i + 1 < operands.size(); ++i) {
    bindType(info->bound[i], operands[i + 1], info->ref->loc);
  }
  bool tail = std::exchange(checker_.tailAllowed_, false);
  const Type *result = resolveConditional(*info->ref);
  checker_.tailAllowed_ = tail;
  checker_.typeScopes_.pop_back();

  return result ? result : Type::error();
}

std::vector<const Type *> ConditionalTypes::conditionalBranches(const Type *conditional) {
  if (conditional->typeOp() != TypeOp::Conditional) {
    return {};
  }
  llvm::ArrayRef<const Type *> operands = conditional->operands();
  const Checker::ConditionalInfo *info = conditionalTags_.lookup(operands[0]);
  if (!info) {
    return {};
  }
  checker_.typeScopes_.emplace_back();
  for (const auto &[name, decl] : info->decls) {
    checker_.typeScopes_.back()[name] = decl;
  }
  for (size_t i = 0; i < info->bound.size() && i + 1 < operands.size(); ++i) {
    bindType(info->bound[i], operands[i + 1], info->ref->loc);
  }
  // What `infer` gives is not known: type parameters of their own.
  std::vector<const TypeRef *> inferRefs;
  std::vector<bool> lists;
  collectInfers(info->ref->args[1], inferRefs, lists);
  for (size_t i = 0; i < inferRefs.size(); ++i) {
    Type *param = Type::param(inferRefs[i]->name);
    if (lists[i]) {
      param->setList();
    }
    bindType(inferRefs[i]->name, param, inferRefs[i]->loc);
  }
  bool muted = checker_.diags_.setMuted(true);
  std::vector<const Type *> branches;
  for (size_t i : {2, 3}) {
    const Type *branch = checker_.typeResolver().resolveTypeInner(info->ref->args[i]);
    if (branch && !branch->isError()) {
      branches.push_back(branch);
    }
  }
  checker_.diags_.setMuted(muted);
  checker_.typeScopes_.pop_back();

  return branches;
}

bool ConditionalTypes::fitsConditional(const Type *from, const Type *to, unsigned depth) {
  if (depth > 8 || to->typeOp() != TypeOp::Conditional) {
    return false;
  }
  // A value of a union, `flag ? "text" : "other"`: each member of either.
  if (from->isUnion()) {
    return llvm::all_of(from->members(),
                        [&](const Type *m) { return fitsConditional(m, to, depth + 1); });
  }
  for (const Type *branch : conditionalBranches(to)) {
    if (TypeRelations::isImplicitlyConvertible(from, branch) ||
        fitsConditional(from, branch, depth + 1)) {
      return true;
    }
    // Or it fits for some types of the type parameters the branch mentions:
    // `{ text: x }` with x narrowed to a string, for `{ text: T }`.
    std::vector<const Type *> params;
    llvm::SmallPtrSet<const Type *, 8> seen;
    collectParams(branch, params, seen);
    Checker::InferMatch match{params, std::vector<const Type *>(params.size(), nullptr)};
    if (!params.empty() && checker_.extendsRelation().extendsType(from, branch, &match)) {
      return true;
    }
  }

  return false;
}

const Type *ConditionalTypes::resolveIntersection(const TypeRef &ref) {
  std::vector<const Type *> parts;
  for (const TypeRef &arg : ref.args) {
    parts.push_back(checker_.typeResolver().resolveTypeInner(arg));
    if (parts.back()->isError()) {
      return parts.back();
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
      // `K & string`: what of K is a string, as `Extract<K, string>`.
      if (primitive(parts[i])) {
        return checker_.typeOperators().evalTypeOp(Type::typeOp(TypeOp::Extract, {other, parts[i]}),
                                                   {other, parts[i]});
      }
      // `T & {}`: T without undefined, as `NonNullable<T>`.
      if (emptyObject(parts[i])) {
        return checker_.typeOperators().evalTypeOp(Type::typeOp(TypeOp::NonNullable, {other}),
                                                   {other});
      }
    }
  }
  checker_.diags_.error(
      ref.loc, "intersection types are not supported: a value has exactly the fields of its one "
               "type; of TypeScript's idioms with '&', 'K & string' (what of K is a string) and "
               "'T & {}' (T without undefined) work");

  return Type::error();
}

const Type *ConditionalTypes::signatureForUtility(const TypeRef &ref) {
  if (ref.kind != TypeRef::Kind::TypeOf || ref.names.empty()) {
    return nullptr;
  }
  // Each type parameter as its constraint, or unknown, as TypeScript
  // instantiates a generic signature here.
  auto erased = [&](FuncSymbol *fn) {
    const Type *type = checker_.returnInferrer().functionType(fn);
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

    return checker_.typeParams().substitute(type, fn->typeParams, args);
  };
  if (ref.names.size() == 1) {
    Checker::ScopeEntry entry = checker_.topLevelChecker().lookupValue(ref.names.front());
    if (entry.func && entry.func->isGeneric()) {
      return erased(entry.func);
    }
    return nullptr;
  }
  // `typeof obj.method`: a method of the class of obj; of several, the last
  // one.
  if (ref.names.size() != 2) {
    return nullptr;
  }
  VarSymbol *var = checker_.topLevelChecker().lookupValue(ref.names.front()).var;
  if (!var || !var->type) {
    return nullptr;
  }
  const Type *object = var->type->nonOptional();
  if (const Overload *overload = object->isObject() ? object->overload(ref.names[1]) : nullptr) {
    return overload->signatures.back();
  }
  ClassDecl *cls = ClassDeclarer::classOf(object);
  if (!cls || object->field(ref.names[1])) {
    return nullptr;
  }
  checker_.classDeclarer().completeClass(cls);
  std::vector<FunctionDecl *> methods =
      ClassDeclarer::methodsNamed(cls, ref.names[1], /*isStatic=*/false);
  if (methods.empty() || !methods.back()->sym) {
    return nullptr;
  }

  return erased(methods.back()->sym);
}

} // namespace tz
