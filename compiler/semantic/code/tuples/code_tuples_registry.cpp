#include "compiler/semantic/code/tuples/code_tuples_registry.h"
#include "compiler/program/library/array/array_method_table/array_method_table.h"
#include "compiler/program/library/builtin_names/builtin_property_table/builtin_property_table.h"
#include "compiler/semantic/bodies/objects/object_check_helpers/object_check_helpers.h"
#include "compiler/semantic/code/calls/code_calls_registry_errors.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

namespace {

constexpr const char *kTuplePlaceholder = "t";

}

CodeTuplesRegistry::CodeTuplesRegistry(SemanticState &owner)
    : state(owner) {}

void CodeTuplesRegistry::checkTupleMember(const CheckTupleMember &task) {
  MemberExpr *member = task.member;
  const Type *type = member->object->type;
  if (!BuiltinPropertyTable::isName(member->name, BuiltinProperty::LENGTH)) {
    member->type = Type::error();
    auto message = ErrorTextTupleMember::noProperty(member->name, type->name());
    this->state.diagnostics.error(member->loc(), message);
    return this->state.walk.tasks.pushChecked(member);
  }
  auto *length = this->state.ast.make<IntLitExpr>(member->loc(), type->fields().size());
  length->type = Type::intTy();
  if (isa<NameExpr>(member->object)) {
    return this->state.walk.tasks.pushChecked(length);
  }
  auto *sequence = this->state.ast.make<SequenceExpr>(member->loc(),
                                                      std::vector<Expr *>{member->object, length});
  sequence->type = Type::intTy();

  return this->state.walk.tasks.pushChecked(sequence);
}

void CodeTuplesRegistry::checkTupleMethod(const CheckTupleMethod &task) {
  CallExpr *call = task.call;
  MemberExpr *member = task.member;
  const auto *method = ArrayMethodTable::find(member->name);
  bool isJoining = method != nullptr && method->method == ArrayMethod::JOIN;
  if (isJoining) {
    return this->startTupleJoin(call, member);
  }
  bool isResizing = method != nullptr && method->form.is<ResizingArrayMethod>();
  this->state.walk.tasks.pushTask(FinishTupleMethod{
      .call = call,
      .member = member,
      .isResizing = isResizing,
  });

  return this->state.walk.tasks.pushTask(WalkArgsLoosely{.call = call});
}

void CodeTuplesRegistry::finishTupleMethod(const FinishTupleMethod &task) {
  CallExpr *call = task.call;
  MemberExpr *member = task.member;
  llvm::StringRef name = member->name;
  auto text = this->tupleText(member);
  call->type = Type::error();
  this->state.walk.tasks.pushChecked(call);
  if (task.isResizing) {
    auto message = ErrorTextTupleMethod::fixedLen(name, text);
    return this->state.diagnostics.error(member->loc(), message);
  }
  if (this->tupleElement(member->object->type) != nullptr) {
    auto message = ErrorTextTupleMethod::copyFirst(name, text);
    return this->state.diagnostics.error(member->loc(), message);
  }
  auto message = ErrorTextTupleMethod::destruct(name, text);

  return this->state.diagnostics.error(member->loc(), message);
}

void CodeTuplesRegistry::startTupleJoin(CallExpr *call, MemberExpr *member) {
  call->type = Type::error();
  for (size_t index = 0; index < call->args.size(); ++index) {
    if (call->spreads(index)) {
      auto message = ErrorTextSpread::notRest();
      this->state.diagnostics.error(call->args[index]->loc(), message);
      this->state.walk.tasks.pushChecked(call);
      return this->state.walk.tasks.pushTask(WalkArgsLoosely{.call = call});
    }
  }
  size_t count = call->args.size();
  if (count > 1) {
    auto message =
        call->tagged ? ErrorTextArity::tagged(0, 1, count) : ErrorTextArity::count(0, 1, count);
    this->state.walk.tasks.pushTask(ReportTupleJoin{
        .call = call,
        .member = member,
        .message = message,
    });
    return this->state.walk.tasks.pushTask(WalkArgsLoosely{.call = call});
  }
  this->state.walk.tasks.pushTask(FinishTupleJoin{.call = call, .member = member});
  if (count == 1) {
    this->state.walk.tasks.pushTask(CheckArgSlot{.slot = &call->args[0], .to = Type::string()});
  }
}

void CodeTuplesRegistry::reportTupleJoin(const ReportTupleJoin &task) {
  this->state.diagnostics.error(task.member->loc(), task.message);

  return this->state.walk.tasks.pushChecked(task.call);
}

void CodeTuplesRegistry::finishTupleJoin(const FinishTupleJoin &task) {
  CallExpr *call = task.call;
  const Type *tuple = task.member->object->type;
  this->state.walk.tasks.pushChecked(call);
  for (size_t index = 0; index < tuple->fields().size(); ++index) {
    const Type *element = tuple->fields()[index].type->nonOptional();
    if (!this->isJoinableElement(element)) {
      auto message = ErrorTextTupleJoin::notText(index, tuple->name(), element->name());
      return this->state.diagnostics.error(task.member->loc(), message);
    }
  }
  call->target = BuiltinCall{.builtin = Builtin::TupleJoin};
  call->type = Type::string();
}

const Type *CodeTuplesRegistry::tupleElement(const Type *tuple) const {
  const Type *element = nullptr;
  for (const Field &field : tuple->fields()) {
    element = element != nullptr ? this->state.types.literals.unionUnify(element, field.type)
                                 : field.type;
    if (element == nullptr || element->isError()) {
      return nullptr;
    }
  }

  return element;
}

bool CodeTuplesRegistry::isJoinableElement(const Type *element) const {
  return element->isNumeric() || element->isDecimal() || element->isBool() || element->isString() ||
         element->isUndefined();
}

std::string CodeTuplesRegistry::tupleText(const MemberExpr *member) const {
  const Expr *unnarrowed = ObjectCheckHelpers::unnarrowed(member->object);
  auto *object = dyn_cast<NameExpr>(unnarrowed);
  if (object == nullptr) {
    return kTuplePlaceholder;
  }

  return object->name.str();
}

}
