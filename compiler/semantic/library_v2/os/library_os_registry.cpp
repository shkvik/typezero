#include "compiler/semantic/library_v2/os/library_os_registry.h"
#include "compiler/program/library/os/endianness_table/endianness_table.h"
#include "compiler/program/library/os/os_function_table/os_function_table.h"
#include "compiler/program/library/os/os_shape_field_table/os_shape_field_table.h"
#include "compiler/program/library/os/os_shape_name_table/os_shape_name_table.h"
#include "compiler/program/library/os/os_value_table/os_value_table.h"
#include "compiler/program/to_utf16/to_utf16.h"
#include "compiler/semantic/library/builtin_modules/node_check_helpers/node_check_helpers.h"
#include <functional>

namespace tz {

LibraryOsRegistry::LibraryOsRegistry(SemanticState &owner)
    : state(owner) {}

const Type *LibraryOsRegistry::cpuInfoType() const {
  const auto *doubleType = Type::doubleTy();
  auto field = std::bind_front(&LibraryOsRegistry::shapeFieldName, this);
  const auto *times = Type::object({{field(OsShapeField::CPU_USER), doubleType, false},
                                    {field(OsShapeField::CPU_NICE), doubleType, false},
                                    {field(OsShapeField::CPU_SYS), doubleType, false},
                                    {field(OsShapeField::CPU_IDLE), doubleType, false},
                                    {field(OsShapeField::CPU_IRQ), doubleType, false}});
  auto typeName = OsShapeNameTable::getName(OsShapeName::CPU_INFO);

  return Type::object({{field(OsShapeField::CPU_MODEL), Type::string(), false},
                       {field(OsShapeField::CPU_SPEED), doubleType, false},
                       {field(OsShapeField::CPU_TIMES), times, false}},
                      typeName);
}

std::string LibraryOsRegistry::shapeFieldName(OsShapeField field) const {
  return OsShapeFieldTable::getName(field).str();
}

const Type *LibraryOsRegistry::userInfoType() const {
  auto field = std::bind_front(&LibraryOsRegistry::shapeFieldName, this);
  auto typeName = OsShapeNameTable::getName(OsShapeName::USER_INFO);

  return Type::object({{field(OsShapeField::USER_ID), Type::intTy(), false},
                       {field(OsShapeField::GROUP_ID), Type::intTy(), false},
                       {field(OsShapeField::USER_NAME), Type::string(), false},
                       {field(OsShapeField::HOME_DIRECTORY), Type::string(), false},
                       {field(OsShapeField::SHELL), Type::optional(Type::string()), false}},
                      typeName);
}

void LibraryOsRegistry::checkOsValue(const CheckOsValue &task) {
  auto value = OsValueTable::find(task.name);
  if (!value.has_value()) {
    return this->state.walk.tasks.pushChecked(nullptr);
  }
  auto text = this->osValueText(value.value());
  auto *literal = NodeCheckHelpers::stringLiteral(this->state.ast, task.expr->loc(), text);

  return this->state.walk.tasks.pushChecked(literal);
}

llvm::StringRef LibraryOsRegistry::osValueText(OsValue value) const {
  bool isWindows = NodeCheckHelpers::isTargetWindows();
  switch (value) {
    case OsValue::END_OF_LINE:
      return isWindows ? "\r\n" : "\n";
    case OsValue::DEV_NULL:
      return isWindows ? "\\\\.\\nul" : "/dev/null";
  }

  return "";
}

LibraryOsRegistry::ResultOsFunction LibraryOsRegistry::checkKnownOsFunction(llvm::StringRef functionName) const {
  auto value = OsValueTable::find(functionName);
  if (value.has_value()) {
    auto message = ErrorTextOs::notCalled(functionName);

    return err(LibraryOsRegistryOsFunctionError{
        .code = LibraryOsRegistryOsFunctionErrorCode::STRING_CALLED,
        .message = message,
    });
  }

  return ok(functionName);
}

void LibraryOsRegistry::checkOsCall(const CheckOsCall &task) {
  CallExpr *call = task.call;
  llvm::StringRef functionName = task.functionName;
  llvm::SMLoc loc = task.loc;
  auto &tasks = this->state.walk.tasks;
  call->type = Type::error();
  auto function = this->checkKnownOsFunction(functionName);
  if (function.isErr()) {
    this->state.diagnostics.error(loc, function.error().message);
    tasks.pushChecked(call);

    return tasks.pushTask(WalkArgsLoosely{.call = call});
  }
  if (this->state.library.common.hasSpread(call)) {
    tasks.pushChecked(call);

    return tasks.pushTask(RejectSpreadCall{.call = call});
  }
  auto arity = this->state.library.common.checkArity(call, 0, 0);
  if (arity.isErr()) {
    this->state.diagnostics.error(loc, arity.error().message);

    return tasks.pushChecked(call);
  }
  auto found = OsFunctionTable::find(functionName);
  if (!found.has_value()) {
    auto *reported = this->reportNotYet(call, functionName, loc);

    return tasks.pushChecked(reported);
  }
  auto *checked = this->checkOsFunction(call, *found, loc);

  return tasks.pushChecked(checked);
}

Expr *LibraryOsRegistry::checkOsFunction(CallExpr *call, OsFunction function, llvm::SMLoc loc) {
  switch (function) {
    case OsFunction::HOMEDIR:
      return this->osStringCall(call, loc, 0);
    case OsFunction::TMPDIR:
      return this->osStringCall(call, loc, 1);
    case OsFunction::HOSTNAME:
      return this->osStringCall(call, loc, 2);
    case OsFunction::ARCH:
      return this->osStringCall(call, loc, 3);
    case OsFunction::TYPE:
      return this->osStringCall(call, loc, 4);
    case OsFunction::RELEASE:
      return this->osStringCall(call, loc, 5);
    case OsFunction::VERSION:
      return this->osStringCall(call, loc, 6);
    case OsFunction::MACHINE:
      return this->osStringCall(call, loc, 7);
    case OsFunction::TOTALMEM:
      return this->osNumberCall(call, loc, 0);
    case OsFunction::FREEMEM:
      return this->osNumberCall(call, loc, 1);
    case OsFunction::UPTIME:
      return this->osNumberCall(call, loc, 2);
    case OsFunction::PLATFORM:
      return this->builtinOsCall(call, Builtin::ProcessPlatform, Type::string());
    case OsFunction::ENDIANNESS:
      return this->endiannessLiteral(loc);
    case OsFunction::AVAILABLE_PARALLELISM:
      return this->builtinOsCall(call, Builtin::OsAvailableParallelism, Type::intTy());
    case OsFunction::CPUS:
      return this->builtinOsCall(call, Builtin::OsCpus, Type::array(this->cpuInfoType()));
    case OsFunction::LOADAVG:
      return this->builtinOsCall(call, Builtin::OsLoadavg, Type::array(Type::doubleTy()));
    case OsFunction::USER_INFO:
      return this->builtinOsCall(call, Builtin::OsUserInfo, this->userInfoType());
  }

  return call;
}

Expr *LibraryOsRegistry::osStringCall(CallExpr *call, llvm::SMLoc loc, int which) {
  this->setWhichArg(call, loc, which);

  return this->builtinOsCall(call, Builtin::OsString, Type::string());
}

Expr *LibraryOsRegistry::osNumberCall(CallExpr *call, llvm::SMLoc loc, int which) {
  this->setWhichArg(call, loc, which);

  return this->builtinOsCall(call, Builtin::OsNumber, Type::doubleTy());
}

void LibraryOsRegistry::setWhichArg(CallExpr *call, llvm::SMLoc loc, int which) {
  auto *literal = this->state.ast.make<IntLitExpr>(loc, uint64_t(which));
  literal->type = Type::intTy();
  call->args = {literal};
}

Expr *LibraryOsRegistry::builtinOsCall(CallExpr *call, Builtin builtin, const Type *result) const {
  call->target = BuiltinCall{.builtin = builtin};
  call->type = result;

  return call;
}

Expr *LibraryOsRegistry::endiannessLiteral(llvm::SMLoc loc) {
  auto little = ToUtf16::toUtf16(EndiannessTable::getName(Endianness::LITTLE));
  auto *literal = this->state.ast.make<StringLitExpr>(loc, little);
  literal->type = Type::string();

  return literal;
}

Expr *LibraryOsRegistry::reportNotYet(CallExpr *call, llvm::StringRef functionName, llvm::SMLoc loc) {
  auto message = ErrorTextOs::notYet(functionName);
  this->state.diagnostics.error(loc, message);

  return call;
}

}
