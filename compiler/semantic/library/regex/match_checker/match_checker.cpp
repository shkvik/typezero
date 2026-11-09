#include "compiler/semantic/library/regex/match_checker/match_checker.h"
#include "compiler/program/library/builtin_names/builtin_property_table/builtin_property_table.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/bodies/expr/member_checker/member_checker.h"
#include "compiler/semantic/bodies/objects/object_checker/object_checker.h"
#include "compiler/semantic/declare/constant_folder/constant_folder.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "runtime/library/regex/regex/regex.h"
#include "llvm/ADT/STLExtras.h"
#include <cstring>

namespace tz {

MatchChecker::MatchChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->exprChecker = &all.getExprChecker();
  this->memberChecker = &all.getMemberChecker();
  this->objectChecker = &all.getObjectChecker();
  this->typeRelations = &all.getTypeRelations();
  this->constantFolder = &all.getConstantFolder();
}

Expr *MatchChecker::matchItems(Expr *match) {
  const Type *array = Type::array(Type::optional(Type::string()));
  if (match->type->isOptional()) {
    this->state.getFunctionStack().enterOptionalChain();
    Expr *items = this->matchItems(this->exprChecker->chainLink(match));
    auto *chain = this->state.getAst().make<OptionalChainExpr>(match->loc(), items);
    chain->mayShortCircuit = this->state.getFunctionStack().isShortCircuiting();
    chain->type = Type::optional(array);
    this->state.getFunctionStack().leaveOptionalChain();
    return chain;
  }
  auto *items = this->state.getAst().make<MemberExpr>(match->loc(), match, "%items");
  items->type = array;

  return items;
}

Expr *MatchChecker::checkMatchMember(MemberExpr *member) {
  const Type *type = member->object->type;
  llvm::StringRef name = member->name;
  if (BuiltinPropertyTable::isName(name, BuiltinProperty::LENGTH)) {
    member->object = this->matchItems(member->object);
    member->type = Type::intTy();
    return member;
  }
  if (name == "index" || name == "input") {
    ObjectChecker::ResultObjectFieldValue field = this->objectChecker->checkField(member);
    if (field.isErr()) {
      this->state.getDiagnostics().error(member->loc(), field.error().message);
      return member;
    }
    return field.value();
  }
  member->type = Type::error();
  if (name == "groups") {
    const Type *groups = type->field("groups")->type;
    if (!groups->isOptional()) {
      ObjectChecker::ResultObjectFieldValue field = this->objectChecker->checkField(member);
      if (field.isErr()) {
        this->state.getDiagnostics().error(member->loc(), field.error().message);
        return member;
      }
      return field.value();
    }
    std::string message = ErrorTextMatch::groups(type->matchGroups() >= 0);
    this->state.getDiagnostics().error(member->loc(), message);
    return member;
  }
  if (name == "indices") {
    if (type->field("indices")) {
      ObjectChecker::ResultObjectFieldValue field = this->objectChecker->checkField(member);
      if (field.isErr()) {
        this->state.getDiagnostics().error(member->loc(), field.error().message);
        return member;
      }
      return field.value();
    }
    std::string message = ErrorTextMatch::indices(type->matchGroups() >= 0);
    this->state.getDiagnostics().error(member->loc(), message);
    return member;
  }
  std::string message = ErrorTextMatch::noMember(name, type->name());
  this->state.getDiagnostics().error(member->loc(), message);

  return member;
}

MatchChecker::ResultGroupIndex MatchChecker::groupIndex(int64_t index, int groups) {
  if (index < 0 || index > groups) {
    std::string message = ErrorTextMatch::noGroup(groups, index);

    return err(MatchCheckerGroupIndexError{
        .code = MatchCheckerGroupIndexErrorCode::NO_SUCH_GROUP,
        .message = message,
    });
  }

  return ok(index);
}

Expr *MatchChecker::checkMatchIndex(IndexExpr *index) {
  const Type *type = index->object->type;
  index->index = this->memberChecker->checkPosition(index->index);
  index->type = Type::error();
  if (index->index->type->isError()) {
    return index;
  }
  index->object = this->matchItems(index->object);
  const Type *item = Type::optional(Type::string());
  std::optional<int64_t> k = this->constantFolder->intConstant(index->index);
  int groups = type->matchGroups();
  if (k && *k == 0) {
    index->form = ElementIndex{.mayBeOutOfRange = false};
    index->type = item;
    return this->typeRelations->convert(index, Type::string());
  }
  if (k && groups >= 0) {
    ResultGroupIndex group = MatchChecker::groupIndex(*k, groups);
    if (group.isErr()) {
      this->state.getDiagnostics().error(index->index->loc(), group.error().message);
      return index;
    }
    index->form = ElementIndex{.mayBeOutOfRange = false};
    index->type = item;
    if (type->captureMandatory(unsigned(*k))) {
      return this->typeRelations->convert(index, Type::string());
    }
    return index;
  }
  index->form = ElementIndex{.mayBeOutOfRange = true};
  index->type = item;

  return index;
}

}
