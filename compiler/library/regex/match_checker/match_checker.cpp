#include "compiler/library/regex/match_checker/match_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/expr/member_checker/member_checker.h"
#include "compiler/check/objects/object_checker/object_checker.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/declare/constant_folder/constant_folder.h"
#include "runtime/library/regex/regex/regex.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include <cstring>

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

namespace {

const char kUnknownRegex[] =
    "the regex is known only when the program runs (it is not a literal, nor a variable that "
    "holds one all along)";

} // namespace

MatchChecker::MatchChecker(Checker &checker) : checker_(checker) {}

Expr *MatchChecker::matchItems(Expr *match) {
  const Type *array = Type::array(Type::optional(Type::string()));
  if (match->type->isOptional()) {
    // `m?.%items`
    bool outer = std::exchange(checker_.chainShortCircuits_, false);
    Expr *items = matchItems(checker_.exprChecker().chainLink(match));
    auto *chain = checker_.ctx_.make<OptionalChainExpr>(match->loc(), items);
    chain->mayShortCircuit = checker_.chainShortCircuits_;
    chain->type = Type::optional(array);
    checker_.chainShortCircuits_ = outer;
    return chain;
  }
  auto *items = checker_.ctx_.make<MemberExpr>(match->loc(), match, "%items");
  items->type = array;

  return items;
}

Expr *MatchChecker::checkMatchMember(MemberExpr *member) {
  const Type *type = member->object->type;
  llvm::StringRef name = member->name;
  if (name == "length") {
    member->object = matchItems(member->object);
    member->type = Type::intTy();
    return member;
  }
  if (name == "index" || name == "input") {
    return checker_.objectChecker().checkField(member);
  }
  member->type = Type::error();
  if (name == "groups") {
    const Type *groups = type->field("groups")->type;
    if (!groups->isOptional()) {
      return checker_.objectChecker().checkField(member);
    }
    if (type->matchGroups() < 0) {
      checker_.diags_.error(member->loc(),
                            std::string("the named groups of a match are not supported "
                                        "yet when ") +
                                kUnknownRegex + "; read the groups by number: 'm[1]'");
    } else {
      checker_.diags_.error(member->loc(),
                            "the regex has no named groups; name one with '(?<name>...)'");
    }
    return member;
  }
  if (name == "indices") {
    if (type->field("indices")) {
      return checker_.objectChecker().checkField(member);
    }
    checker_.diags_.error(member->loc(),
                          type->matchGroups() < 0
                              ? std::string("'indices' of a match needs the d flag, and ") +
                                    kUnknownRegex
                              : "a match has 'indices' only with the d flag: add 'd' to "
                                "the flags of the regex");
    return member;
  }
  checker_.diags_.error(member->loc(),
                        "property '" + name + "' does not exist on type '" + type->name() + "'");

  return member;
}

Expr *MatchChecker::checkMatchIndex(IndexExpr *index) {
  const Type *type = index->object->type;
  index->index = checker_.memberChecker().checkPosition(index->index);
  index->type = Type::error();
  if (index->index->type->isError()) {
    return index;
  }
  index->object = matchItems(index->object);
  const Type *item = Type::optional(Type::string());
  std::optional<int64_t> k = checker_.constantFolder().intConstant(index->index);
  int groups = type->matchGroups();
  // The match itself is m[0] of any regex.
  if (k && *k == 0) {
    index->mayBeOutOfRange = false;
    index->type = item;
    return checker_.typeRelations().convert(index, Type::string());
  }
  if (k && groups >= 0) {
    if (*k < 0 || *k > groups) {
      checker_.diags_.error(
          index->index->loc(),
          "the regex has " + std::to_string(groups) + " group" + (groups == 1 ? "" : "s") +
              (groups ? " (m[1] to m[" + std::to_string(groups) + "]; m[0] is the match)"
                      : " (m[0] is the match)") +
              ", so there is no m[" + std::to_string(*k) + "]");
      return index;
    }
    index->mayBeOutOfRange = false;
    index->type = item;
    // A group that takes part in every match holds a value.
    if (type->captureMandatory(unsigned(*k))) {
      return checker_.typeRelations().convert(index, Type::string());
    }
    return index;
  }
  // Otherwise as an element of an array: undefined past the end.
  index->mayBeOutOfRange = true;
  index->type = item;

  return index;
}

} // namespace tz
