#include "compiler/semantic/types/extends_relation/extends_relation.h"
#include "compiler/semantic/declare/class_declarer/class_declarer.h"
#include "compiler/semantic/generics/generic_check_helpers/generic_check_helpers.h"
#include "compiler/semantic/generics/inference/inference.h"
#include "compiler/semantic/infer/return_inferrer/return_inferrer.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/conditional_check_helpers/conditional_check_helpers.h"
#include "compiler/semantic/types/template_types/template_types.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "compiler/semantic/types/unknown_checker/unknown_checker.h"
#include "llvm/ADT/STLExtras.h"
#include <cmath>

namespace tz {

ExtendsRelation::ExtendsRelation(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->classDeclarer = &all.getClassDeclarer();
  this->templateTypes = &all.getTemplateTypes();
  this->returnInferrer = &all.getReturnInferrer();
}

std::vector<const Type *> ExtendsRelation::distributed(const Type *type) {
  std::vector<const Type *> out;
  ConditionalCheckHelpers::distribute(type, out);

  return out;
}

bool ExtendsRelation::extendsType(const Type *type, const Type *pattern, InferMatch *inference) {
  std::vector<ExtendsStep> steps = {ExtendsGoal{type, pattern}};
  while (!steps.empty()) {
    ExtendsStep step = std::move(steps.back());
    steps.pop_back();
    bool holds = this->applyExtendsStep(step, inference, steps);
    if (!holds && !ExtendsRelation::retryChoice(steps, inference)) {
      return false;
    }
  }

  return true;
}

bool ExtendsRelation::applyExtendsStep(const ExtendsStep &step, InferMatch *inference,
                                       std::vector<ExtendsStep> &steps) {
  switch (step.kind()) {
    case ExtendsStepKind::GOAL:
      return this->extendsStep(step.as<ExtendsGoal>()->type, step.as<ExtendsGoal>()->pattern,
                               inference, steps);
    case ExtendsStepKind::BIND:
      ExtendsRelation::bindInfer(*step.as<BindInfer>(), inference);
      return true;
    case ExtendsStepKind::CHOICE:
      return true;
  }
  llvm_unreachable("unknown extends step");
}

bool ExtendsRelation::retryChoice(std::vector<ExtendsStep> &steps, InferMatch *inference) {
  while (!steps.empty()) {
    ExtendsStep step = std::move(steps.back());
    steps.pop_back();
    const ExtendsChoice *choice = step.as<ExtendsChoice>();
    if (choice == nullptr) {
      continue;
    }
    if (inference) {
      inference->bound = choice->saved;
    }
    bool tried = ExtendsRelation::tryAlternative(
        ExtendsChoice{choice->type, choice->pattern, choice->next + 1, choice->saved}, steps);
    if (tried) {
      return true;
    }
  }

  return false;
}

bool ExtendsRelation::tryAlternative(ExtendsChoice choice, std::vector<ExtendsStep> &steps) {
  llvm::ArrayRef<const Type *> members = choice.pattern->members();
  if (choice.next >= members.size()) {
    return false;
  }
  const Type *member = members[choice.next];
  const Type *type = choice.type;
  steps.push_back(std::move(choice));
  steps.push_back(ExtendsGoal{type, member});

  return true;
}

void ExtendsRelation::bindInfer(const BindInfer &bind, InferMatch *inference) {
  const Type *&bound = inference->bound[bind.index];
  bound = bound && bound != bind.type ? Type::unionOf({bound, bind.type}) : bind.type;
}

bool ExtendsRelation::extendsStep(const Type *type, const Type *pattern, InferMatch *inference,
                                  std::vector<ExtendsStep> &steps) {
  if (inference) {
    auto found = llvm::find(inference->params, pattern);
    if (found != inference->params.end()) {
      steps.push_back(BindInfer{size_t(found - inference->params.begin()), type});
      if (const Type *constraint = pattern->constraint()) {
        steps.push_back(ExtendsGoal{type, constraint});
      }
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
    ExtendsRelation::pushGoals(ExtendsRelation::distributed(type), pattern, steps);
    return true;
  }
  if (pattern->isOptional()) {
    if (!type->isUndefined()) {
      steps.push_back(ExtendsGoal{type, pattern->inner()});
    }
    return true;
  }
  if (type->isUndefined()) {
    return false;
  }
  if (pattern->isUnion()) {
    std::vector<const Type *> saved = inference ? inference->bound : std::vector<const Type *>();
    return ExtendsRelation::tryAlternative(ExtendsChoice{type, pattern, 0, std::move(saved)},
                                           steps);
  }
  if (pattern->typeOp() == TypeOp::Template) {
    return type->isStringLiteral() &&
           this->templateTypes->matchTemplate(type->literalText(), pattern->operands(), inference);
  }
  if (pattern->isPattern()) {
    return type->isStringLiteral() &&
           TemplatePatterns::matchesPattern(type->literalText(), pattern);
  }
  if (type->isPattern()) {
    return pattern == Type::string();
  }
  if (type->isLiteral()) {
    if (pattern->isLiteral()) {
      return false;
    }
    steps.push_back(ExtendsGoal{type->literalBase(), pattern});
    return true;
  }
  if (pattern->isLiteral() || type->isParam() || pattern->isParam()) {
    return false;
  }
  if (pattern->isAnyObject()) {
    return UnknownChecker::isObjectReference(type);
  }

  return this->structureStep(type, pattern, inference, steps);
}

bool ExtendsRelation::structureStep(const Type *type, const Type *pattern, InferMatch *inference,
                                    std::vector<ExtendsStep> &steps) {
  if (type->isArray()) {
    return pattern->isArray() &&
           ExtendsRelation::pushPairs({{type->inner(), pattern->inner()}}, steps);
  }
  if (type->isTuple()) {
    if (pattern->isArray()) {
      std::vector<ExtendsGoal> goals;
      for (const Field &f : type->fields()) {
        goals.push_back(ExtendsGoal{f.type, pattern->inner()});
      }
      if (type->tupleRest()) {
        goals.push_back(ExtendsGoal{type->tupleRest()->inner(), pattern->inner()});
      }
      return ExtendsRelation::pushPairs(goals, steps);
    }
    return pattern->isTuple() && this->extendsTuple(type, pattern, inference);
  }
  if (type->isFunction()) {
    return pattern->isFunction() && this->extendsFunction(type, pattern, inference);
  }
  if (type->isPromise()) {
    return pattern->isPromise() &&
           ExtendsRelation::pushPairs({{type->promised(), pattern->promised()}}, steps);
  }
  if (type->isIterator()) {
    return pattern->isIterator() &&
           ExtendsRelation::pushPairs({{type->yielded(), pattern->yielded()},
                                       {type->returned(), pattern->returned()},
                                       {type->sent(), pattern->sent()}},
                                      steps);
  }
  if (type->isDict()) {
    return pattern->isDict() && type->dictKind() == pattern->dictKind() &&
           ExtendsRelation::pushPairs(
               {{type->keyType(), pattern->keyType()}, {type->valueType(), pattern->valueType()}},
               steps);
  }
  if (type->isResult()) {
    return pattern->isResult() &&
           ExtendsRelation::pushPairs(
               {{type->okType(), pattern->okType()}, {type->errType(), pattern->errType()}}, steps);
  }
  if (type->isObject() && pattern->isObject()) {
    return this->extendsObject(type, pattern, inference);
  }

  return !pattern->isObject() && !type->isObject() &&
         TypeRelations::isImplicitlyConvertible(type, pattern);
}

void ExtendsRelation::pushGoals(llvm::ArrayRef<const Type *> types, const Type *pattern,
                                std::vector<ExtendsStep> &steps) {
  for (const Type *type : llvm::reverse(types)) {
    steps.push_back(ExtendsGoal{type, pattern});
  }
}

bool ExtendsRelation::pushPairs(llvm::ArrayRef<ExtendsGoal> goals,
                                std::vector<ExtendsStep> &steps) {
  for (const ExtendsGoal &goal : llvm::reverse(goals)) {
    steps.push_back(goal);
  }

  return true;
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
    if (inference && GenericCheckHelpers::mentionsAnyOf(their, inference->params)) {
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
    this->classDeclarer->completeClass(cls);
  }
  for (const Field &f : pattern->fields()) {
    const Type *own = nullptr;
    if (const Field *have = type->field(f.name)) {
      own = have->type;
    } else if (cls) {
      if (std::vector<FunctionDecl *> methods = ClassDeclarer::methodsNamed(cls, f.name, false);
          !methods.empty() && methods.back()->sym && methods.back()->typeParams.empty()) {
        own = this->returnInferrer->functionType(methods.back()->sym);
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
