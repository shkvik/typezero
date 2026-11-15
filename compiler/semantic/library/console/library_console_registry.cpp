#include "compiler/semantic/library/console/library_console_registry.h"
#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"

namespace tz {

LibraryConsoleRegistry::LibraryConsoleRegistry(SemanticState &owner)
    : state(owner) {}

void LibraryConsoleRegistry::checkConsoleCall(const CheckConsoleCall &task) {
  CallExpr *call = task.call;
  MemberExpr *member = task.member;
  auto &tasks = this->state.walk.tasks;
  if (!this->state.isGlobalName(member->object, GlobalName::CONSOLE)) {
    return tasks.pushChecked(nullptr);
  }
  tasks.pushChecked(call);
  auto method = this->checkConsoleMethod(member);
  if (method.isErr()) {
    this->state.diagnostics.error(member->loc(), method.error().message);

    return tasks.pushTask(WalkArgsLoosely{.call = call});
  }
  bool isToStderr = ConsoleMethodTable::isToStderr(method.value());
  call->target = BuiltinCall{.builtin = isToStderr ? Builtin::ConsoleError : Builtin::ConsoleLog};
  call->type = Type::voidTy();

  return this->nextConsoleArgument(NextConsoleArgument{.call = call, .index = 0});
}

void LibraryConsoleRegistry::nextConsoleArgument(const NextConsoleArgument &task) {
  CallExpr *call = task.call;
  auto &tasks = this->state.walk.tasks;
  if (task.index == call->args.size()) {
    return;
  }
  tasks.pushTask(AfterConsoleArgument{.call = call, .index = task.index});

  return tasks.pushTask(CheckExprSlot{.slot = &call->args[task.index], .expected = nullptr});
}

void LibraryConsoleRegistry::afterConsoleArgument(const AfterConsoleArgument &task) {
  CallExpr *call = task.call;
  const Expr *arg = call->args[task.index];
  auto printable = this->checkPrintable(arg);
  if (printable.isErr()) {
    this->state.diagnostics.error(arg->loc(), printable.error().message);
  }
  auto spreadable = this->checkSpreadable(call, task.index);
  if (spreadable.isErr()) {
    this->state.diagnostics.error(arg->loc(), spreadable.error().message);
  }

  return this->state.walk.tasks.pushTask(
      NextConsoleArgument{.call = call, .index = task.index + 1});
}

LibraryConsoleRegistry::ResultPrintableItem
LibraryConsoleRegistry::checkPrintable(const Expr *arg) const {
  if (arg->type->isVoid()) {
    auto message = ErrorTextConsole::voidValue();

    return err(LibraryConsoleRegistryPrintableError{
        .code = LibraryConsoleRegistryPrintableErrorCode::VOID_VALUE,
        .message = message,
    });
  }

  return ok(arg);
}

LibraryConsoleRegistry::ResultSpreadableArg
LibraryConsoleRegistry::checkSpreadable(const CallExpr *call, size_t index) const {
  const Type *type = call->args[index]->type;
  bool isSpreadingArray = !call->spreads(index) || type->isArray() || type->isListParam() ||
                          this->isSpreadingLists(type) || type->isError();
  if (!isSpreadingArray) {
    auto message = ErrorTextConsole::spread(type->name());

    return err(LibraryConsoleRegistrySpreadableError{
        .code = LibraryConsoleRegistrySpreadableErrorCode::NOT_AN_ARRAY,
        .message = message,
    });
  }

  return ok(call->args[index]);
}

LibraryConsoleRegistry::ResultConsoleMethod
LibraryConsoleRegistry::checkConsoleMethod(const MemberExpr *member) const {
  auto method = ConsoleMethodTable::find(member->name);
  if (!method.has_value()) {
    auto message = ErrorTextConsole::notYet(member->name);

    return err(LibraryConsoleRegistryConsoleMethodError{
        .code = LibraryConsoleRegistryConsoleMethodErrorCode::NOT_SUPPORTED_YET,
        .message = message,
    });
  }

  return ok(method.value());
}

bool LibraryConsoleRegistry::isSpreadingLists(const Type *type) const {
  if (!type->isTuple()) {
    return false;
  }
  for (const Field &field : type->fields()) {
    if (field.type->isListParam()) {
      return true;
    }
  }

  return false;
}

}
