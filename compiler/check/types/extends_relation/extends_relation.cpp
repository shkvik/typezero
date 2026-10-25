#include "compiler/check/types/extends_relation/extends_relation.h"
#include "compiler/check/types/conditional_check_helpers/conditional_check_helpers.h"
#include "compiler/check/types/template_types/template_types.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/check/types/unknown_checker/unknown_checker.h"
#include "compiler/declare/class_declarer/class_declarer.h"
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

bool typeMentions(const Type *type, llvm::ArrayRef<const Type *> params);

namespace {

std::vector<const Type *> distributed(const Type *type) {
  std::vector<const Type *> out;
  distribute(type, out);

  return out;
}

} // namespace

ExtendsRelation::ExtendsRelation(Checker &checker) : checker_(checker) {}

bool ExtendsRelation::extendsType(const Type *type, const Type *pattern,
                                  Checker::InferMatch *inference) {
  // `infer U`: what stands in its place, which must meet its constraint.
  if (inference) {
    auto found = llvm::find(inference->params, pattern);
    if (found != inference->params.end()) {
      if (const Type *constraint = pattern->constraint();
          constraint && !extendsType(type, constraint, inference)) {
        return false;
      }
      const Type *&bound = inference->bound[size_t(found - inference->params.begin())];
      bound = bound && bound != type ? Type::unionOf({bound, type}) : type;
      return true;
    }
  }
  if (type == pattern || type->isError() || pattern->isError() || type->isNever()) {
    return true;
  }
  if (pattern->isUnknown()) {
    return true;
  }
  if (pattern->isNever()) {
    return false;
  }
  // A union extends what each of its members does.
  if (type->isUnion() || type->isOptional()) {
    return llvm::all_of(distributed(type),
                        [&](const Type *m) { return extendsType(m, pattern, inference); });
  }
  if (pattern->isOptional()) {
    return type->isUndefined() || extendsType(type, pattern->inner(), inference);
  }
  if (type->isUndefined()) {
    return false;
  }
  if (pattern->isUnion()) {
    for (const Type *m : pattern->members()) {
      std::vector<const Type *> saved = inference ? inference->bound : std::vector<const Type *>();
      if (extendsType(type, m, inference)) {
        return true;
      }
      if (inference) {
        inference->bound = std::move(saved);
      }
    }
    return false;
  }
  // Strings and their patterns.
  if (pattern->typeOp() == TypeOp::Template) {
    return type->isStringLiteral() && checker_.templateTypes().matchTemplate(
                                          type->literalText(), pattern->operands(), inference);
  }
  if (pattern->isPattern()) {
    return type->isStringLiteral() && matchesPattern(type->literalText(), pattern);
  }
  if (type->isPattern()) {
    return pattern == Type::string();
  }
  // A literal extends what its base does: `"a" extends string`.
  if (type->isLiteral()) {
    return !pattern->isLiteral() && extendsType(type->literalBase(), pattern, inference);
  }
  if (pattern->isLiteral() || type->isParam() || pattern->isParam()) {
    return false;
  }
  if (pattern->isAnyObject()) {
    return UnknownChecker::isObjectReference(type);
  }
  if (type->isArray()) {
    return pattern->isArray() && extendsType(type->inner(), pattern->inner(), inference);
  }
  if (type->isTuple()) {
    if (pattern->isArray()) {
      for (const Field &f : type->fields()) {
        if (!extendsType(f.type, pattern->inner(), inference)) {
          return false;
        }
      }
      return !type->tupleRest() ||
             extendsType(type->tupleRest()->inner(), pattern->inner(), inference);
    }
    return pattern->isTuple() && extendsTuple(type, pattern, inference);
  }
  if (type->isFunction()) {
    return pattern->isFunction() && extendsFunction(type, pattern, inference);
  }
  if (type->isPromise()) {
    return pattern->isPromise() && extendsType(type->promised(), pattern->promised(), inference);
  }
  if (type->isIterator()) {
    return pattern->isIterator() && extendsType(type->yielded(), pattern->yielded(), inference) &&
           extendsType(type->returned(), pattern->returned(), inference) &&
           extendsType(type->sent(), pattern->sent(), inference);
  }
  if (type->isDict()) {
    return pattern->isDict() && type->dictKind() == pattern->dictKind() &&
           extendsType(type->keyType(), pattern->keyType(), inference) &&
           extendsType(type->valueType(), pattern->valueType(), inference);
  }
  if (type->isResult()) {
    return pattern->isResult() && extendsType(type->okType(), pattern->okType(), inference) &&
           extendsType(type->errType(), pattern->errType(), inference);
  }
  if (type->isObject() && pattern->isObject()) {
    return extendsObject(type, pattern, inference);
  }

  // Numbers and the other values: as they convert, `int extends number`.
  return !pattern->isObject() && !type->isObject() &&
         TypeRelations::isImplicitlyConvertible(type, pattern);
}

bool ExtendsRelation::extendsTuple(const Type *type, const Type *pattern,
                                   Checker::InferMatch *inference) {
  std::vector<const Type *> mine, theirs;
  for (const Field &f : type->fields()) {
    mine.push_back(f.type);
  }
  for (const Field &f : pattern->fields()) {
    theirs.push_back(f.type);
  }
  // A list of types in the pattern, `[infer H, ...infer R]`, takes the
  // elements the others leave.
  size_t spread = theirs.size();
  for (size_t i = 0; i < theirs.size(); ++i) {
    if (theirs[i]->isListParam()) {
      if (spread != theirs.size()) {
        return false;
      }
      spread = i;
    }
  }
  // An argument list, with elements that may be left out: only as a whole.
  if (type->tupleRest() || type->tupleMin() < mine.size()) {
    return spread == 0 && theirs.size() == 1 && extendsType(type, theirs[0], inference);
  }
  if (spread == theirs.size()) {
    if (pattern->tupleRest()) {
      if (mine.size() < theirs.size()) {
        return false;
      }
      for (size_t i = 0; i < mine.size(); ++i) {
        if (!extendsType(mine[i], i < theirs.size() ? theirs[i] : pattern->tupleRest()->inner(),
                         inference)) {
          return false;
        }
      }
      return true;
    }
    if (mine.size() != theirs.size() || pattern->tupleMin() < theirs.size()) {
      return false;
    }
    for (size_t i = 0; i < mine.size(); ++i) {
      if (!extendsType(mine[i], theirs[i], inference)) {
        return false;
      }
    }
    return true;
  }
  size_t after = theirs.size() - spread - 1;
  if (mine.size() < spread + after) {
    return false;
  }
  for (size_t i = 0; i < spread; ++i) {
    if (!extendsType(mine[i], theirs[i], inference)) {
      return false;
    }
  }
  for (size_t k = 0; k < after; ++k) {
    if (!extendsType(mine[mine.size() - after + k], theirs[spread + 1 + k], inference)) {
      return false;
    }
  }
  std::vector<const Type *> middle(mine.begin() + long(spread), mine.end() - long(after));

  return extendsType(Type::tuple(middle), theirs[spread], inference);
}

bool ExtendsRelation::extendsFunction(const Type *type, const Type *pattern,
                                      Checker::InferMatch *inference) {
  // What the function returns; anything goes where void is expected.
  if (!pattern->ret()->isVoid() && !extendsType(type->ret(), pattern->ret(), inference)) {
    return false;
  }
  llvm::ArrayRef<const Type *> mine = type->params(), theirs = pattern->params();
  size_t myFixed = mine.size() - (type->hasRest() ? 1 : 0);
  size_t theirFixed = theirs.size() - (pattern->hasRest() ? 1 : 0);
  // A function that needs more arguments than the pattern gives does not
  // fit; one that takes fewer does.
  if (!pattern->hasRest() && type->minArgs() > theirs.size()) {
    return false;
  }
  auto mineAt = [&](size_t i) -> const Type * {
    if (i < myFixed) {
      return mine[i];
    }

    return type->hasRest() && mine.back()->isArray() ? mine.back()->inner() : nullptr;
  };
  // A parameter of the pattern: one with `infer` takes mine; another must go
  // where mine is expected, as parameters are contravariant.
  auto fits = [&](const Type *my, const Type *their) {
    if (inference && typeMentions(their, inference->params)) {
      return extendsType(my, their, inference);
    }

    return TypeRelations::isImplicitlyConvertible(their, my) || extendsType(their, my);
  };
  for (size_t i = 0; i < theirFixed; ++i) {
    if (const Type *my = mineAt(i); my && !fits(my, theirs[i])) {
      return false;
    }
  }
  if (!pattern->hasRest()) {
    return true;
  }
  const Type *rest = theirs.back();
  if (inference && llvm::is_contained(inference->params, rest)) {
    // `(...args: infer A) => R`: my parameters from there on, as a list.
    size_t from = std::min(theirFixed, myFixed);
    std::vector<const Type *> elements(mine.begin() + long(from), mine.begin() + long(myFixed));
    size_t min = type->minArgs() > from ? type->minArgs() - from : 0;
    const Type *list = Type::argumentList(elements, std::min(min, elements.size()),
                                          type->hasRest() ? mine.back() : nullptr);
    return extendsType(list, rest, inference);
  }
  // `...args: unknown[]` or `never[]`: any parameters, as `any[]` is in
  // TypeScript.
  const Type *element = rest->isArray() ? rest->inner() : nullptr;
  if (!element || element->isUnknown() || element->isNever()) {
    return true;
  }
  for (size_t i = theirFixed; i < myFixed; ++i) {
    if (!fits(mine[i], element)) {
      return false;
    }
  }

  return true;
}

bool ExtendsRelation::extendsObject(const Type *type, const Type *pattern,
                                    Checker::InferMatch *inference) {
  if (type->isSubtypeOf(pattern)) {
    return true;
  }
  if (pattern->isClass()) {
    // `Box<infer U>`: an instance of the same generic class, up the chain.
    const ClassDecl *want = pattern->classDecl();
    want = want->generic ? want->generic : want;
    if (pattern->typeArgs().empty()) {
      return false;
    }
    std::vector<const Type *> todo{type};
    while (!todo.empty()) {
      const Type *t = todo.back();
      todo.pop_back();
      const ClassDecl *cls = t->classDecl();
      if (cls && (cls->generic ? cls->generic : cls) == want &&
          t->typeArgs().size() == pattern->typeArgs().size()) {
        bool all = true;
        for (size_t k = 0; all && k < t->typeArgs().size(); ++k) {
          all = extendsType(t->typeArgs()[k], pattern->typeArgs()[k], inference);
        }
        if (all) {
          return true;
        }
      }
      todo.insert(todo.end(), t->bases().begin(), t->bases().end());
    }
    return false;
  }
  if (type->isTuple() || pattern->isTuple() || type->isAnyObject() ||
      !pattern->overloads().empty()) {
    return false;
  }
  // At least the fields of the pattern, of types that extend theirs; a method
  // of a class is a field that holds a function, and a getter (of a class of
  // a built-in module, `status` of a Response) one of what it gives.
  ClassDecl *cls = ClassDeclarer::classOf(type);
  if (cls) {
    checker_.classDeclarer().completeClass(cls);
  }
  for (const Field &f : pattern->fields()) {
    const Type *own = nullptr;
    if (const Field *have = type->field(f.name)) {
      own = have->type;
    } else if (cls) {
      if (std::vector<FunctionDecl *> methods =
              ClassDeclarer::methodsNamed(cls, f.name, /*isStatic=*/false);
          !methods.empty() && methods.back()->sym && methods.back()->typeParams.empty()) {
        own = checker_.returnInferrer().functionType(methods.back()->sym);
        if (methods.back()->accessor == FunctionDecl::Accessor::Getter) {
          own = own->ret();
        } else if (methods.back()->accessor == FunctionDecl::Accessor::Setter) {
          own = nullptr;
        }
      }
    }
    if (!own) {
      if (f.type->isOptional()) {
        continue;
      }
      return false;
    }
    if (!extendsType(own, f.type, inference)) {
      return false;
    }
  }

  return true;
}

} // namespace tz
