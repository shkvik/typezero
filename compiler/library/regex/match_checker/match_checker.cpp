#include "compiler/library/regex/match_checker/match_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/expr/member_checker/member_checker.h"
#include "compiler/check/objects/object_checker/object_checker.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/declare/constant_folder/constant_folder.h"
#include "runtime/library/regex/regex/regex.h"
#include "llvm/ADT/STLExtras.h"
#include <cstring>


namespace tz {

namespace {

const char kUnknownRegex[] =
    "the regex is known only when the program runs (it is not a literal, nor a variable that "
    "holds one all along)";

}

MatchChecker::MatchChecker(Checker &owner)
    : checker(owner) {}

Expr *MatchChecker::matchItems(Expr *match) {
  const Type *array = Type::array(Type::optional(Type::string()));
  if (match->type->isOptional()) {
    bool outer = std::exchange(this->checker.chainShortCircuits_, false);
    Expr *items = this->matchItems(this->checker.exprChecker().chainLink(match));
    auto *chain = this->checker.ctx_.make<OptionalChainExpr>(match->loc(), items);
    chain->mayShortCircuit = this->checker.chainShortCircuits_;
    chain->type = Type::optional(array);
    this->checker.chainShortCircuits_ = outer;
    return chain;
  }
  auto *items = this->checker.ctx_.make<MemberExpr>(match->loc(), match, "%items");
  items->type = array;

  return items;
}

Expr *MatchChecker::checkMatchMember(MemberExpr *member) {
  const Type *type = member->object->type;
  llvm::StringRef name = member->name;
  if (name == "length") {
    member->object = this->matchItems(member->object);
    member->type = Type::intTy();
    return member;
  }
  if (name == "index" || name == "input") {
    ObjectChecker::ResultObjectFieldValue field = this->checker.objectChecker().checkField(member);
    if (field.isErr()) {
      this->checker.diags_.error(member->loc(), field.error().message);
      return member;
    }
    return field.value();
  }
  member->type = Type::error();
  if (name == "groups") {
    const Type *groups = type->field("groups")->type;
    if (!groups->isOptional()) {
      ObjectChecker::ResultObjectFieldValue field =
          this->checker.objectChecker().checkField(member);
      if (field.isErr()) {
        this->checker.diags_.error(member->loc(), field.error().message);
        return member;
      }
      return field.value();
    }
    if (type->matchGroups() < 0) {
      this->checker.diags_.error(member->loc(),
                                 std::string("the named groups of a match are not supported "
                                             "yet when ") +
                                     kUnknownRegex + "; read the groups by number: 'm[1]'");
    } else {
      this->checker.diags_.error(member->loc(),
                                 "the regex has no named groups; name one with '(?<name>...)'");
    }
    return member;
  }
  if (name == "indices") {
    if (type->field("indices")) {
      ObjectChecker::ResultObjectFieldValue field =
          this->checker.objectChecker().checkField(member);
      if (field.isErr()) {
        this->checker.diags_.error(member->loc(), field.error().message);
        return member;
      }
      return field.value();
    }
    this->checker.diags_.error(member->loc(),
                               type->matchGroups() < 0
                                   ? std::string("'indices' of a match needs the d flag, and ") +
                                         kUnknownRegex
                                   : "a match has 'indices' only with the d flag: add 'd' to "
                                     "the flags of the regex");
    return member;
  }
  this->checker.diags_.error(member->loc(), "property '" + name + "' does not exist on type '" +
                                                type->name() + "'");

  return member;
}

Expr *MatchChecker::checkMatchIndex(IndexExpr *index) {
  const Type *type = index->object->type;
  index->index = this->checker.memberChecker().checkPosition(index->index);
  index->type = Type::error();
  if (index->index->type->isError()) {
    return index;
  }
  index->object = this->matchItems(index->object);
  const Type *item = Type::optional(Type::string());
  std::optional<int64_t> k = this->checker.constantFolder().intConstant(index->index);
  int groups = type->matchGroups();
  if (k && *k == 0) {
    index->form = ElementIndex{.mayBeOutOfRange = false};
    index->type = item;
    return this->checker.typeRelations().convert(index, Type::string());
  }
  if (k && groups >= 0) {
    if (*k < 0 || *k > groups) {
      this->checker.diags_.error(
          index->index->loc(),
          "the regex has " + std::to_string(groups) + " group" + (groups == 1 ? "" : "s") +
              (groups ? " (m[1] to m[" + std::to_string(groups) + "]; m[0] is the match)"
                      : " (m[0] is the match)") +
              ", so there is no m[" + std::to_string(*k) + "]");
      return index;
    }
    index->form = ElementIndex{.mayBeOutOfRange = false};
    index->type = item;
    if (type->captureMandatory(unsigned(*k))) {
      return this->checker.typeRelations().convert(index, Type::string());
    }
    return index;
  }
  index->form = ElementIndex{.mayBeOutOfRange = true};
  index->type = item;

  return index;
}

}
