#include "compiler/semantic/library/regex/match_checker/match_checker.h"
#include "compiler/program/library/builtin_names/builtin_property_table/builtin_property_table.h"
#include "compiler/program/library/regex/match_member_table/match_member_table.h"
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
  this->exprChecker = &all.exprChecker;
  this->memberChecker = &all.memberChecker;
  this->objectChecker = &all.objectChecker;
  this->typeRelations = &all.typeRelations;
  this->constantFolder = &all.constantFolder;
}

Expr *MatchChecker::matchItems(Expr *match) {
  auto optionals = std::vector<Expr *>();
  Expr *link = match;
  while (link->type->isOptional()) {
    this->state.getFunctionStack().enterOptionalChain();
    optionals.push_back(link);
    link = this->exprChecker->chainLink(link);
  }
  auto *items = this->itemsMember(link);
  for (Expr *optional : llvm::reverse(optionals)) {
    items = this->optionalItems(optional, items);
  }

  return items;
}

Expr *MatchChecker::itemsMember(Expr *match) {
  auto *items = this->state.getAst().make<MemberExpr>(match->loc(), match, "%items");
  items->type = this->itemsType();

  return items;
}

Expr *MatchChecker::optionalItems(Expr *match, Expr *items) {
  auto *chain = this->state.getAst().make<OptionalChainExpr>(match->loc(), items);
  chain->mayShortCircuit = this->state.getFunctionStack().isShortCircuiting();
  chain->type = Type::optional(this->itemsType());
  this->state.getFunctionStack().leaveOptionalChain();

  return chain;
}

const Type *MatchChecker::itemsType() const {
  return Type::array(Type::optional(Type::string()));
}

Expr *MatchChecker::checkMatchMember(MemberExpr *member) {
  const Type *type = member->object->type;
  llvm::StringRef name = member->name;
  if (BuiltinPropertyTable::isName(name, BuiltinProperty::LENGTH)) {
    member->object = this->matchItems(member->object);
    member->type = Type::intTy();
    return member;
  }
  auto known = MatchMemberTable::find(name);
  if (known == MatchMember::INDEX || known == MatchMember::INPUT) {
    auto field = this->objectChecker->checkField(member);
    if (field.isErr()) {
      this->state.getDiagnostics().error(member->loc(), field.error().message);
      return member;
    }
    return field.value();
  }
  member->type = Type::error();
  if (known == MatchMember::GROUPS) {
    auto groupsName = MatchMemberTable::getName(MatchMember::GROUPS);
    const Type *groups = type->field(groupsName)->type;
    if (!groups->isOptional()) {
      auto field = this->objectChecker->checkField(member);
      if (field.isErr()) {
        this->state.getDiagnostics().error(member->loc(), field.error().message);
        return member;
      }
      return field.value();
    }
    auto message = ErrorTextMatch::groups(type->matchGroups() >= 0);
    this->state.getDiagnostics().error(member->loc(), message);
    return member;
  }
  if (known == MatchMember::INDICES) {
    auto indicesName = MatchMemberTable::getName(MatchMember::INDICES);
    if (type->field(indicesName) != nullptr) {
      auto field = this->objectChecker->checkField(member);
      if (field.isErr()) {
        this->state.getDiagnostics().error(member->loc(), field.error().message);
        return member;
      }
      return field.value();
    }
    auto message = ErrorTextMatch::indices(type->matchGroups() >= 0);
    this->state.getDiagnostics().error(member->loc(), message);
    return member;
  }
  auto message = ErrorTextMatch::noMember(name, type->name());
  this->state.getDiagnostics().error(member->loc(), message);

  return member;
}

MatchChecker::ResultGroupIndex MatchChecker::checkGroupIndex(int64_t index, int groups) const {
  if (index < 0 || index > groups) {
    auto message = ErrorTextMatch::noGroup(groups, index);

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
  const auto *item = Type::optional(Type::string());
  auto position = this->constantFolder->intConstant(index->index);
  int groups = type->matchGroups();
  if (position.has_value() && *position == 0) {
    index->form = ElementIndex{.mayBeOutOfRange = false};
    index->type = item;
    return this->typeRelations->convert(index, Type::string());
  }
  if (position.has_value() && groups >= 0) {
    auto group = this->checkGroupIndex(*position, groups);
    if (group.isErr()) {
      this->state.getDiagnostics().error(index->index->loc(), group.error().message);
      return index;
    }
    index->form = ElementIndex{.mayBeOutOfRange = false};
    index->type = item;
    if (type->captureMandatory(unsigned(*position))) {
      return this->typeRelations->convert(index, Type::string());
    }
    return index;
  }
  index->form = ElementIndex{.mayBeOutOfRange = true};
  index->type = item;

  return index;
}

}
