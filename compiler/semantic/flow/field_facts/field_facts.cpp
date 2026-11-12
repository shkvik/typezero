#include "compiler/semantic/flow/field_facts/field_facts.h"
#include "compiler/program/library/builtin_names/builtin_property_table/builtin_property_table.h"
#include "compiler/semantic/bodies/objects/object_check_helpers/object_check_helpers.h"
#include "compiler/semantic/library/stream/set_encoding_checker/set_encoding_checker.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include <functional>

using llvm::dyn_cast;

namespace tz {

FieldFacts::FieldFacts(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->typeRelations = &all.typeRelations;
}

std::optional<FieldPath> FieldFacts::fieldPath(const Expr *expr) const {
  auto names = std::vector<llvm::StringRef>();
  const auto *link = ObjectCheckHelpers::unnarrowed(expr);
  auto *member = dyn_cast<MemberExpr>(link);
  if (member == nullptr) {
    return std::nullopt;
  }
  while (member != nullptr) {
    if (!this->isHoldingField(member)) {
      return std::nullopt;
    }
    names.push_back(member->name);
    link = ObjectCheckHelpers::unnarrowed(member->object);
    member = dyn_cast<MemberExpr>(link);
  }
  auto *root = dyn_cast<NameExpr>(link);
  if (root == nullptr || root->var() == nullptr) {
    return std::nullopt;
  }
  auto path = FieldPath{.var = root->var()};
  for (llvm::StringRef name : llvm::reverse(names)) {
    path.names.push_back(name);
  }

  return path;
}

bool FieldFacts::isHoldingField(const MemberExpr *member) const {
  if (member->optional || member->object->type == nullptr) {
    return false;
  }
  const Type *holder = member->object->type;
  bool isValue = BuiltinPropertyTable::isName(member->name, BuiltinProperty::RESULT_VALUE);
  bool isError = BuiltinPropertyTable::isName(member->name, BuiltinProperty::RESULT_ERROR);
  bool isResultHalf = holder->isResult() && (isValue || isError);

  return holder->isObject() || holder->isUnion() || holder->nonOptional()->isJson() || isResultHalf;
}

bool FieldFacts::isFieldDefined(const FieldPath &path) const {
  return this->state.getFlowState().isFieldDefined(path);
}

void FieldFacts::defineField(Flow &flow, const FieldPath &path) const {
  if (!llvm::is_contained(flow.definedFields, path)) {
    flow.definedFields.push_back(path);
  }
}

void FieldFacts::fieldsAssigned(llvm::StringRef name) {
  this->state.getFlowState().forgetFieldsNamed(name);
}

bool FieldFacts::isPathOf(const VarSymbol *variable, const FieldPath &path) const {
  return path.var == variable;
}

bool FieldFacts::isNarrowedPathOf(const VarSymbol *variable,
                                  const std::pair<FieldPath, const Type *> &entry) const {
  return entry.first.var == variable;
}

void FieldFacts::forgetFieldsOf(Flow &flow, const VarSymbol *variable) const {
  auto isOwned = std::bind_front(&FieldFacts::isPathOf, this, variable);
  auto isNarrowedOwned = std::bind_front(&FieldFacts::isNarrowedPathOf, this, variable);
  llvm::erase_if(flow.definedFields, isOwned);
  llvm::erase_if(flow.narrowedFields, isNarrowedOwned);
}

Expr *FieldFacts::narrowedField(MemberExpr *member) {
  const auto *view = this->streamView(member);
  if (view != nullptr) {
    return this->typeRelations->convert(member, view);
  }
  if (!this->state.getFlowState().hasNarrowedFields()) {
    return member;
  }
  auto path = this->fieldPath(member);
  if (!path.has_value()) {
    return member;
  }
  const auto *type = this->state.getFlowState().findNarrowedField(*path);
  if (type == nullptr) {
    return member;
  }

  return this->typeRelations->convert(member, type);
}

const Type *FieldFacts::streamView(const MemberExpr *member) const {
  const auto &streams = this->state.getStreamStore();
  if (!streams.hasStreamViews()) {
    return nullptr;
  }
  auto path = this->fieldPath(member);
  if (!path.has_value()) {
    return nullptr;
  }

  return streams.findStreamView(*path, member->loc());
}

}
