#include "compiler/semantic/library/os/os_checker/os_checker.h"
#include "compiler/program/library/os/endianness_table/endianness_table.h"
#include "compiler/program/library/os/os_function_table/os_function_table.h"
#include "compiler/program/library/os/os_shape_field_table/os_shape_field_table.h"
#include "compiler/program/library/os/os_shape_name_table/os_shape_name_table.h"
#include "compiler/program/library/os/os_value_table/os_value_table.h"
#include "compiler/program/to_utf16/to_utf16.h"
#include "compiler/semantic/bodies/expr/argument_checker/argument_checker.h"
#include "compiler/semantic/bodies/expr/call_checker/call_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/library/builtin_modules/node_check_helpers/node_check_helpers.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"

namespace tz {

OsChecker::OsChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->callChecker = &all.callChecker;
  this->exprChecker = &all.exprChecker;
  this->argumentChecker = &all.argumentChecker;
}

const Type *OsChecker::cpuInfoType() const {
  const auto *doubleType = Type::doubleTy();
  auto field = std::bind_front(&OsChecker::shapeFieldName, this);
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

std::string OsChecker::shapeFieldName(OsShapeField field) const {
  return OsShapeFieldTable::getName(field).str();
}

const Type *OsChecker::userInfoType() const {
  auto field = std::bind_front(&OsChecker::shapeFieldName, this);
  auto typeName = OsShapeNameTable::getName(OsShapeName::USER_INFO);

  return Type::object({{field(OsShapeField::USER_ID), Type::intTy(), false},
                       {field(OsShapeField::GROUP_ID), Type::intTy(), false},
                       {field(OsShapeField::USER_NAME), Type::string(), false},
                       {field(OsShapeField::HOME_DIRECTORY), Type::string(), false},
                       {field(OsShapeField::SHELL), Type::optional(Type::string()), false}},
                      typeName);
}

Expr *OsChecker::checkOsValue(llvm::StringRef name, Expr *expr) {
  auto value = OsValueTable::find(name);
  if (!value.has_value()) {
    return nullptr;
  }
  auto text = this->osValueText(value.value());

  return NodeCheckHelpers::stringLiteral(this->state.getAst(), expr->loc(), text);
}

llvm::StringRef OsChecker::osValueText(OsValue value) const {
  bool isWindows = NodeCheckHelpers::isTargetWindows();
  switch (value) {
    case OsValue::END_OF_LINE:
      return isWindows ? "\r\n" : "\n";
    case OsValue::DEV_NULL:
      return isWindows ? "\\\\.\\nul" : "/dev/null";
  }

  return "";
}

OsChecker::ResultOsFunction OsChecker::checkKnownOsFunction(llvm::StringRef functionName) const {
  auto value = OsValueTable::find(functionName);
  if (value.has_value()) {
    auto message = ErrorTextOs::notCalled(functionName);

    return err(OsCheckerOsFunctionError{
        .code = OsCheckerOsFunctionErrorCode::STRING_CALLED,
        .message = message,
    });
  }

  return ok(functionName);
}

Expr *OsChecker::checkOsCall(CallExpr *call, llvm::StringRef functionName, llvm::SMLoc loc) {
  call->type = Type::error();
  auto function = this->checkKnownOsFunction(functionName);
  if (function.isErr()) {
    this->state.getDiagnostics().error(loc, function.error().message);
    this->argumentChecker->walkArgsLoosely(call);
    return call;
  }
  if (this->hasRejectedArgs(call, loc)) {
    return call;
  }
  auto found = OsFunctionTable::find(functionName);
  if (!found.has_value()) {
    return this->reportNotYet(call, functionName, loc);
  }

  return this->checkOsFunction(call, *found, loc);
}

bool OsChecker::hasRejectedArgs(CallExpr *call, llvm::SMLoc loc) {
  bool isRejected = this->callChecker->rejectSpread(call);
  auto arity = isRejected ? ok(call) : this->argumentChecker->checkArity(call, 0, 0);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(loc, arity.error().message);
  }

  return isRejected || arity.isErr();
}

Expr *OsChecker::checkOsFunction(CallExpr *call, OsFunction function, llvm::SMLoc loc) {
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

Expr *OsChecker::osStringCall(CallExpr *call, llvm::SMLoc loc, int which) {
  this->setWhichArg(call, loc, which);

  return this->builtinOsCall(call, Builtin::OsString, Type::string());
}

Expr *OsChecker::osNumberCall(CallExpr *call, llvm::SMLoc loc, int which) {
  this->setWhichArg(call, loc, which);

  return this->builtinOsCall(call, Builtin::OsNumber, Type::doubleTy());
}

void OsChecker::setWhichArg(CallExpr *call, llvm::SMLoc loc, int which) {
  auto *literal = this->state.getAst().make<IntLitExpr>(loc, uint64_t(which));
  literal->type = Type::intTy();
  call->args = {literal};
}

Expr *OsChecker::builtinOsCall(CallExpr *call, Builtin builtin, const Type *result) const {
  call->target = BuiltinCall{.builtin = builtin};
  call->type = result;

  return call;
}

Expr *OsChecker::endiannessLiteral(llvm::SMLoc loc) {
  auto little = ToUtf16::toUtf16(EndiannessTable::getName(Endianness::LITTLE));
  auto *literal = this->state.getAst().make<StringLitExpr>(loc, little);
  literal->type = Type::string();

  return literal;
}

Expr *OsChecker::reportNotYet(CallExpr *call, llvm::StringRef functionName, llvm::SMLoc loc) {
  auto message = ErrorTextOs::notYet(functionName);
  this->state.getDiagnostics().error(loc, message);

  return call;
}

}
