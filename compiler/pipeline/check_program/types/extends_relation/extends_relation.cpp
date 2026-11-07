#include "compiler/pipeline/check_program/types/extends_relation/extends_relation.h"
#include "compiler/pipeline/check_program/declare/class_declarer/class_declarer.h"
#include "compiler/pipeline/check_program/generics/inference/inference.h"
#include "compiler/pipeline/check_program/infer/return_inferrer/return_inferrer.h"
#include "compiler/pipeline/check_program/types/conditional_check_helpers/conditional_check_helpers.h"
#include "compiler/pipeline/check_program/types/template_types/template_types.h"
#include "compiler/pipeline/check_program/types/type_relations/type_relations.h"
#include "compiler/pipeline/check_program/types/unknown_checker/unknown_checker.h"
#include "compiler/pipeline/check_program/whole_program/flows/flows.h"
#include "llvm/ADT/STLExtras.h"
#include <cmath>

namespace tz {


std::vector<const Type *> ExtendsRelation::distributed(const Type *type) {
  std::vector<const Type *> out;
  distribute(type, out);

  return out;
}

ExtendsRelation::ExtendsRelation(CheckState &owner)
    : state(owner) {}

bool ExtendsRelation::extendsType(const Type *type, const Type *pattern, InferMatch *inference) {
  if (inference) {
    auto found = llvm::find(inference->params, pattern);
    if (found != inference->params.end()) {
      if (const Type *constraint = pattern->constraint();
          constraint && !this->extendsType(type, constraint, inference)) {
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
  if (type->isUnion() || type->isOptional()) {
    return llvm::all_of(ExtendsRelation::distributed(type),
                        [&](const Type *m) { return this->extendsType(m, pattern, inference); });
  }
  if (pattern->isOptional()) {
    return type->isUndefined() || this->extendsType(type, pattern->inner(), inference);
  }
  if (type->isUndefined()) {
    return false;
  }
  if (pattern->isUnion()) {
    for (const Type *m : pattern->members()) {
      std::vector<const Type *> saved = inference ? inference->bound : std::vector<const Type *>();
      if (this->extendsType(type, m, inference)) {
        return true;
      }
      if (inference) {
        inference->bound = std::move(saved);
      }
    }
    return false;
  }
  if (pattern->typeOp() == TypeOp::Template) {
    return type->isStringLiteral() &&
           TemplateTypes(this->state)
               .matchTemplate(type->literalText(), pattern->operands(), inference);
  }
  if (pattern->isPattern()) {
    return type->isStringLiteral() && matchesPattern(type->literalText(), pattern);
  }
  if (type->isPattern()) {
    return pattern == Type::string();
  }
  if (type->isLiteral()) {
    return !pattern->isLiteral() && this->extendsType(type->literalBase(), pattern, inference);
  }
  if (pattern->isLiteral() || type->isParam() || pattern->isParam()) {
    return false;
  }
  if (pattern->isAnyObject()) {
    return UnknownChecker::isObjectReference(type);
  }
  if (type->isArray()) {
    return pattern->isArray() && this->extendsType(type->inner(), pattern->inner(), inference);
  }
  if (type->isTuple()) {
    if (pattern->isArray()) {
      for (const Field &f : type->fields()) {
        if (!this->extendsType(f.type, pattern->inner(), inference)) {
          return false;
        }
      }
      return !type->tupleRest() ||
             this->extendsType(type->tupleRest()->inner(), pattern->inner(), inference);
    }
    return pattern->isTuple() && this->extendsTuple(type, pattern, inference);
  }
  if (type->isFunction()) {
    return pattern->isFunction() && this->extendsFunction(type, pattern, inference);
  }
  if (type->isPromise()) {
    return pattern->isPromise() &&
           this->extendsType(type->promised(), pattern->promised(), inference);
  }
  if (type->isIterator()) {
    return pattern->isIterator() &&
           this->extendsType(type->yielded(), pattern->yielded(), inference) &&
           this->extendsType(type->returned(), pattern->returned(), inference) &&
           this->extendsType(type->sent(), pattern->sent(), inference);
  }
  if (type->isDict()) {
    return pattern->isDict() && type->dictKind() == pattern->dictKind() &&
           this->extendsType(type->keyType(), pattern->keyType(), inference) &&
           this->extendsType(type->valueType(), pattern->valueType(), inference);
  }
  if (type->isResult()) {
    return pattern->isResult() && this->extendsType(type->okType(), pattern->okType(), inference) &&
           this->extendsType(type->errType(), pattern->errType(), inference);
  }
  if (type->isObject() && pattern->isObject()) {
    return this->extendsObject(type, pattern, inference);
  }

  return !pattern->isObject() && !type->isObject() &&
         TypeRelations::isImplicitlyConvertible(type, pattern);
}

bool ExtendsRelation::extendsTuple(const Type *type, const Type *pattern, InferMatch *inference) {
  std::vector<const Type *> mine, theirs;
  for (const Field &f : type->fields()) {
    mine.push_back(f.type);
  }
  for (const Field &f : pattern->fields()) {
    theirs.push_back(f.type);
  }
  size_t spread = theirs.size();
  for (size_t i = 0; i < theirs.size(); ++i) {
    if (theirs[i]->isListParam()) {
      if (spread != theirs.size()) {
        return false;
      }
      spread = i;
    }
  }
  if (type->tupleRest() || type->tupleMin() < mine.size()) {
    return spread == 0 && theirs.size() == 1 && this->extendsType(type, theirs[0], inference);
  }
  if (spread == theirs.size()) {
    if (pattern->tupleRest()) {
      if (mine.size() < theirs.size()) {
        return false;
      }
      for (size_t i = 0; i < mine.size(); ++i) {
        if (!this->extendsType(mine[i],
                               i < theirs.size() ? theirs[i] : pattern->tupleRest()->inner(),
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
      if (!this->extendsType(mine[i], theirs[i], inference)) {
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
    if (!this->extendsType(mine[i], theirs[i], inference)) {
      return false;
    }
  }
  for (size_t k = 0; k < after; ++k) {
    if (!this->extendsType(mine[mine.size() - after + k], theirs[spread + 1 + k], inference)) {
      return false;
    }
  }
  std::vector<const Type *> middle(mine.begin() + long(spread), mine.end() - long(after));

  return this->extendsType(Type::tuple(middle), theirs[spread], inference);
}

bool ExtendsRelation::extendsFunction(const Type *type, const Type *pattern,
                                      InferMatch *inference) {
  if (!pattern->ret()->isVoid() && !this->extendsType(type->ret(), pattern->ret(), inference)) {
    return false;
  }
  llvm::ArrayRef<const Type *> mine = type->params();
  llvm::ArrayRef<const Type *> theirs = pattern->params();
  size_t myFixed = mine.size() - (type->hasRest() ? 1 : 0);
  size_t theirFixed = theirs.size() - (pattern->hasRest() ? 1 : 0);
  if (!pattern->hasRest() && type->minArgs() > theirs.size()) {
    return false;
  }
  auto mineAt = [&](size_t i) -> const Type * {
    if (i < myFixed) {
      return mine[i];
    }

    return type->hasRest() && mine.back()->isArray() ? mine.back()->inner() : nullptr;
  };
  auto fits = [&](const Type *my, const Type *their) {
    if (inference && mentionsAnyOf(their, inference->params)) {
      return this->extendsType(my, their, inference);
    }

    return TypeRelations::isImplicitlyConvertible(their, my) || this->extendsType(their, my);
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
    size_t from = std::min(theirFixed, myFixed);
    std::vector<const Type *> elements(mine.begin() + long(from), mine.begin() + long(myFixed));
    size_t min = type->minArgs() > from ? type->minArgs() - from : 0;
    const Type *list = Type::argumentList(elements, std::min(min, elements.size()),
                                          type->hasRest() ? mine.back() : nullptr);
    return this->extendsType(list, rest, inference);
  }
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

bool ExtendsRelation::extendsObject(const Type *type, const Type *pattern, InferMatch *inference) {
  if (type->isSubtypeOf(pattern)) {
    return true;
  }
  if (pattern->isClass()) {
    const ClassDecl *want = pattern->classDecl();
    want = want->generic() ? want->generic() : want;
    if (pattern->typeArgs().empty()) {
      return false;
    }
    std::vector<const Type *> todo{type};
    while (!todo.empty()) {
      const Type *t = todo.back();
      todo.pop_back();
      const ClassDecl *cls = t->classDecl();
      if (cls && (cls->generic() ? cls->generic() : cls) == want &&
          t->typeArgs().size() == pattern->typeArgs().size()) {
        bool all = true;
        for (size_t k = 0; all && k < t->typeArgs().size(); ++k) {
          all = this->extendsType(t->typeArgs()[k], pattern->typeArgs()[k], inference);
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
  ClassDecl *cls = ClassDeclarer::classOf(type);
  if (cls) {
    ClassDeclarer(this->state).completeClass(cls);
  }
  for (const Field &f : pattern->fields()) {
    const Type *own = nullptr;
    if (const Field *have = type->field(f.name)) {
      own = have->type;
    } else if (cls) {
      if (std::vector<FunctionDecl *> methods = ClassDeclarer::methodsNamed(cls, f.name, false);
          !methods.empty() && methods.back()->sym && methods.back()->typeParams.empty()) {
        own = ReturnInferrer(this->state).functionType(methods.back()->sym);
      }
    }
    if (!own) {
      if (f.type->isOptional()) {
        continue;
      }
      return false;
    }
    if (!this->extendsType(own, f.type, inference)) {
      return false;
    }
  }

  return true;
}

}
