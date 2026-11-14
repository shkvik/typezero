#include "compiler/semantic/code/assignments/assignment_rules/assignment_rules.h"
#include "compiler/program/library/builtin_names/keyword_table/keyword_table.h"
#include "compiler/program/to_utf16/to_utf16.h"
#include "llvm/ADT/STLExtras.h"

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

AssignmentRules::AssignmentRules(SemanticState &owner)
    : state(owner) {}

void AssignmentRules::startNullishByTarget(AssignExpr *assign) {
  if (isa<MemberExpr>(assign->target)) {
    return this->startFieldNullish(assign);
  }
  auto *index = dyn_cast<IndexExpr>(assign->target);
  if (index == nullptr) {
    return this->startVariableNullish(assign);
  }
  this->state.walk.tasks.pushTask(AfterIndexNullish{.assign = assign});

  return this->state.walk.tasks.pushTask(
      CheckRecordNullishAssign{.assign = assign, .index = index});
}

void AssignmentRules::startFieldNullish(AssignExpr *assign) {
  auto *copied = this->copyChain(assign->target);
  auto chain = this->checkFieldChain(copied);
  if (chain.isErr()) {
    this->state.diagnostics.error(assign->target->loc(), chain.error().message);
    assign->type = Type::error();
    this->state.walk.tasks.pushChecked(assign);
    return this->state.walk.tasks.pushTask(
        CheckExprSlot{.slot = &assign->value, .expected = nullptr});
  }
  Expr *read = chain.value();
  auto *set =
      this->state.ast.make<AssignExpr>(assign->loc(), std::nullopt, assign->target, assign->value);
  auto *binary = this->state.ast.make<BinaryExpr>(assign->loc(), BinaryOp::Nullish, read, set);
  this->state.walk.tasks.pushTask(AfterFieldNullish{.assign = assign, .set = set});

  return this->state.walk.tasks.pushTask(StartNullish{.binary = binary, .expected = nullptr});
}

void AssignmentRules::startVariableNullish(AssignExpr *assign) {
  auto *name = dyn_cast<NameExpr>(assign->target);
  if (name == nullptr) {
    return this->state.walk.tasks.pushTask(AfterNullishVariable{.assign = assign});
  }
  this->state.walk.tasks.pushTask(AfterNullishVariable{.assign = assign});

  return this->startVarTarget(name);
}

void AssignmentRules::startVarTarget(NameExpr *name) {
  auto thisTarget = this->checkNotThis(name);
  if (thisTarget.isErr()) {
    return this->state.diagnostics.error(name->loc(), thisTarget.error().message);
  }
  auto load = this->state.findEarlyLoad(name->name);
  if (!load.has_value()) {
    return this->state.walk.tasks.pushTask(FinishVarTarget{.name = name});
  }
  this->state.walk.tasks.pushTask(FinishVarTarget{.name = name});

  return this->state.walk.tasks.pushTask(*load);
}

void AssignmentRules::startAssignTarget(Expr **target, AssignForm form) {
  auto *name = dyn_cast<NameExpr>(*target);
  auto *index = dyn_cast<IndexExpr>(*target);
  auto *member = dyn_cast<MemberExpr>(*target);
  if (name != nullptr) {
    this->state.walk.tasks.pushTask(AnswerVarTarget{.name = name});
    return this->startVarTarget(name);
  }
  if (index != nullptr) {
    this->state.walk.tasks.pushTask(
        AfterIndexTargetObject{.target = target, .index = index, .form = form});
    return this->state.walk.tasks.pushTask(
        CheckExprSlot{.slot = &index->object, .expected = nullptr});
  }
  if (member != nullptr) {
    return this->startMemberTarget(target, member, form);
  }
  auto message = this->wrongTargetMessage(*target);
  this->state.diagnostics.error((*target)->loc(), message);

  return this->state.walk.tasks.pushAnswer(Type::error());
}

void AssignmentRules::startMemberTarget(Expr **target, MemberExpr *member, AssignForm form) {
  auto *name = dyn_cast<NameExpr>(member->object);
  ClassDecl *named = name != nullptr ? this->state.walk.scopes.findName(name->name).cls() : nullptr;
  if (named != nullptr) {
    return this->state.walk.tasks.pushTask(CheckStaticTarget{.target = target, .classDecl = named});
  }
  if (name != nullptr && name->name == KeywordTable::getName(Keyword::THIS)) {
    this->state.walk.self.rememberThisMember(member->name);
  }
  this->state.walk.tasks.pushTask(
      AfterMemberTargetObject{.target = target, .member = member, .form = form});

  return this->state.walk.tasks.pushTask(
      CheckExprSlot{.slot = &member->object, .expected = nullptr});
}

Expr *AssignmentRules::keyLiteral(llvm::SMLoc loc, llvm::StringRef name) {
  auto *key = this->state.ast.make<StringLitExpr>(loc, ToUtf16::toUtf16(name));
  key->type = Type::string();

  return key;
}

Expr *AssignmentRules::copyChain(const Expr *expr) {
  auto members = std::vector<const MemberExpr *>();
  const Expr *link = expr;
  auto *member = dyn_cast<MemberExpr>(link);
  while (member != nullptr) {
    if (member->optional) {
      return nullptr;
    }
    members.push_back(member);
    link = member->object;
    member = dyn_cast<MemberExpr>(link);
  }
  auto *copy = this->copyRoot(link);
  if (copy == nullptr) {
    return nullptr;
  }
  for (const MemberExpr *step : llvm::reverse(members)) {
    copy = this->state.ast.make<MemberExpr>(step->loc(), copy, step->name);
  }

  return copy;
}

Expr *AssignmentRules::copyRoot(const Expr *expr) {
  auto *name = dyn_cast<NameExpr>(expr);
  auto *integer = dyn_cast<IntLitExpr>(expr);
  auto *text = dyn_cast<StringLitExpr>(expr);
  if (name != nullptr) {
    return this->state.ast.make<NameExpr>(name->loc(), name->name);
  }
  if (integer != nullptr) {
    return this->state.ast.make<IntLitExpr>(integer->loc(), integer->value);
  }
  if (text != nullptr) {
    return this->state.ast.make<StringLitExpr>(text->loc(), text->value);
  }

  return nullptr;
}

AssignmentRules::ResultIndexWithinArray
AssignmentRules::checkIndexProven(const IndexExpr *index) const {
  if (!this->state.flow.proofs.canProveInBounds(index->object, index->index)) {
    auto message = ErrorTextAssign::indexRange();

    return err(CodeAssignmentsRegistryIndexProvenError{
        .code = CodeAssignmentsRegistryIndexProvenErrorCode::INDEX_NOT_PROVEN,
        .message = message,
    });
  }

  return ok(index);
}

AssignmentRules::ResultFieldChainCopied AssignmentRules::checkFieldChain(Expr *read) const {
  if (read == nullptr) {
    auto message = ErrorTextLogicalAssign::field();

    return err(CodeAssignmentsRegistryFieldChainError{
        .code = CodeAssignmentsRegistryFieldChainErrorCode::NOT_A_FIELD_CHAIN,
        .message = message,
    });
  }

  return ok(read);
}

AssignmentRules::ResultTargetIsNotThis AssignmentRules::checkNotThis(const NameExpr *name) const {
  if (name->name == KeywordTable::getName(Keyword::THIS)) {
    auto message = ErrorTextVarTarget::toThis();

    return err(CodeAssignmentsRegistryNotThisError{
        .code = CodeAssignmentsRegistryNotThisErrorCode::ASSIGN_TO_THIS,
        .message = message,
    });
  }

  return ok(name);
}

std::string AssignmentRules::wrongTargetMessage(const Expr *target) const {
  if (isa<ArrayLitExpr>(target)) {
    return ErrorTextAssignTarget::arrayForm();
  }
  if (isa<ObjectLitExpr>(target)) {
    return ErrorTextAssignTarget::objectForm();
  }

  return ErrorTextAssignTarget::notTarget();
}

}
