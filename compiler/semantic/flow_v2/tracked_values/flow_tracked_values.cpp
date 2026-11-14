#include "compiler/semantic/flow_v2/tracked_values/flow_tracked_values.h"
#include "compiler/program/library/builtin_names/builtin_property_table/builtin_property_table.h"
#include "compiler/semantic/bodies/objects/object_check_helpers/object_check_helpers.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include "llvm/ADT/STLExtras.h"

using llvm::dyn_cast;

namespace tz {

FlowTrackedValues::FlowTrackedValues(SemanticState &owner)
    : state(owner) {}

std::optional<FieldPath> FlowTrackedValues::fieldPath(const Expr *expr) const {
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

bool FlowTrackedValues::isHoldingField(const MemberExpr *member) const {
  if (member->optional || member->object->type == nullptr) {
    return false;
  }
  const Type *holder = member->object->type;
  bool isValue = BuiltinPropertyTable::isName(member->name, BuiltinProperty::RESULT_VALUE);
  bool isError = BuiltinPropertyTable::isName(member->name, BuiltinProperty::RESULT_ERROR);
  bool isResultHalf = holder->isResult() && (isValue || isError);

  return holder->isObject() || holder->isUnion() || holder->nonOptional()->isJson() || isResultHalf;
}

void FlowTrackedValues::defineField(Flow &flow, const FieldPath &path) const {
  if (!llvm::is_contained(flow.definedFields, path)) {
    flow.definedFields.push_back(path);
  }
}

const Type *FlowTrackedValues::narrowedFieldType(const MemberExpr *member) const {
  const auto *view = this->streamView(member);
  if (view != nullptr) {
    return view;
  }
  if (!this->state.walk.flow.hasNarrowedFields()) {
    return nullptr;
  }
  auto path = this->fieldPath(member);
  if (!path.has_value()) {
    return nullptr;
  }

  return this->state.walk.flow.findNarrowedField(*path);
}

const Type *FlowTrackedValues::streamView(const MemberExpr *member) const {
  const auto &streams = this->state.library.streams;
  if (!streams.hasStreamViews()) {
    return nullptr;
  }
  auto path = this->fieldPath(member);
  if (!path.has_value()) {
    return nullptr;
  }

  return streams.findStreamView(*path, member->loc());
}

const Type *FlowTrackedValues::typeIn(const Flow &flow, const VarSymbol *variable) const {
  for (const auto &[narrowed, type] : flow.narrowed) {
    if (narrowed == variable) {
      return type;
    }
  }
  if (variable->type->isOptional() && flow.defined.contains(variable)) {
    return variable->type->inner();
  }

  return variable->type;
}

}
