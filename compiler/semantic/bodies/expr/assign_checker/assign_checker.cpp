#include "compiler/semantic/bodies/expr/assign_checker/assign_checker.h"
#include "compiler/semantic/bodies/classes/class_checker/class_checker.h"
#include "compiler/semantic/bodies/expr/binary_checker/binary_checker.h"
#include "compiler/semantic/bodies/expr/concat_checker/concat_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/bodies/expr/target_checker/target_checker.h"
#include "compiler/semantic/bodies/expr/truth_checker/truth_checker.h"
#include "compiler/semantic/bodies/objects/dict_checker/dict_checker.h"
#include "compiler/semantic/bodies/shared_helpers/shared_helpers.h"
#include "compiler/semantic/bodies/stmt/var_checker/var_checker.h"
#include "compiler/semantic/bodies/top_level_checker/top_level_checker.h"
#include "compiler/semantic/declare/class_declarer/class_declarer.h"
#include "compiler/semantic/declare/constant_folder/constant_folder.h"
#include "compiler/semantic/flow/field_facts/field_facts.h"
#include "compiler/semantic/flow/flow_checker/flow_checker.h"
#include "compiler/semantic/flow/index_proofs/index_proofs.h"
#include "compiler/semantic/generics/generic_checker/generic_checker.h"
#include "compiler/semantic/library/builtin_modules/builtin_module_checker/builtin_module_checker.h"
#include "compiler/semantic/library/bytes/bytes_check_helpers/bytes_check_helpers.h"
#include "compiler/semantic/library/bytes/bytes_checker/bytes_checker.h"
#include "compiler/semantic/library/decimal/decimal_checker/decimal_checker.h"
#include "compiler/semantic/library/process/process_checker/process_checker.h"
#include "compiler/semantic/library/tz_modules/tz_modules_checker/tz_modules_checker.h"
#include "compiler/semantic/names/scopes/scopes.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "llvm/ADT/STLExtras.h"
#include <cmath>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

AssignChecker::AssignChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->scopes = &all.getScopes();
  this->fieldFacts = &all.getFieldFacts();
  this->varChecker = &all.getVarChecker();
  this->dictChecker = &all.getDictChecker();
  this->exprChecker = &all.getExprChecker();
  this->flowChecker = &all.getFlowChecker();
  this->indexProofs = &all.getIndexProofs();
  this->bytesChecker = &all.getBytesChecker();
  this->classChecker = &all.getClassChecker();
  this->truthChecker = &all.getTruthChecker();
  this->binaryChecker = &all.getBinaryChecker();
  this->concatChecker = &all.getConcatChecker();
  this->targetChecker = &all.getTargetChecker();
  this->typeRelations = &all.getTypeRelations();
  this->constantFolder = &all.getConstantFolder();
  this->decimalChecker = &all.getDecimalChecker();
  this->genericChecker = &all.getGenericChecker();
  this->processChecker = &all.getProcessChecker();
  this->topLevelChecker = &all.getTopLevelChecker();
  this->builtinModuleChecker = &all.getBuiltinModuleChecker();
}

std::pair<llvm::StringRef, llvm::StringRef>
AssignChecker::operandTexts(llvm::StringRef source, llvm::SMLoc op, size_t length) {
  const char *at = op.getPointer();
  if (source.empty() || at < source.begin() || at + length > source.end()) {
    return {};
  }

  return {llvm::StringRef(source.begin(), static_cast<size_t>(at - source.begin())).trim(),
          llvm::StringRef(at + length, static_cast<size_t>(source.end() - at - length)).trim()};
}

Expr *AssignChecker::copyChain(const Expr *e) {
  if (auto *name = dyn_cast<NameExpr>(e)) {
    return this->state.getAst().make<NameExpr>(name->loc(), name->name);
  }
  if (auto *lit = dyn_cast<IntLitExpr>(e)) {
    return this->state.getAst().make<IntLitExpr>(lit->loc(), lit->value);
  }
  if (auto *lit = dyn_cast<StringLitExpr>(e)) {
    return this->state.getAst().make<StringLitExpr>(lit->loc(), lit->value);
  }
  if (auto *member = dyn_cast<MemberExpr>(e); member && !member->optional) {
    if (Expr *object = this->copyChain(member->object)) {
      return this->state.getAst().make<MemberExpr>(member->loc(), object, member->name);
    }
  }

  return nullptr;
}

AssignChecker::ResultNameIsAssignable AssignChecker::notImported(const NameExpr *name) {
  if (this->builtinModuleChecker->nodeName(name->name)) {
    std::string message = ErrorTextAssign::imported(name->name);

    return err(AssignCheckerNotImportedError{
        .code = AssignCheckerNotImportedErrorCode::IMPORTED,
        .message = message,
    });
  }

  return ok(name);
}

AssignChecker::ResultTargetIsVariable AssignChecker::variableTarget(const Expr *target) {
  if (!isa<NameExpr>(target)) {
    std::string message = ErrorTextAssign::nullishVar();

    return err(AssignCheckerVariableTargetError{
        .code = AssignCheckerVariableTargetErrorCode::NOT_A_VARIABLE,
        .message = message,
    });
  }

  return ok(target);
}

AssignChecker::ResultVariableOptional AssignChecker::nullishOptional(const NameExpr *name,
                                                                     const Type *type) {
  if (!type->isOptional() && !type->isError()) {
    std::string message = ErrorTextAssign::notOptional(name->name, type->name());

    return err(AssignCheckerNullishOptionalError{
        .code = AssignCheckerNullishOptionalErrorCode::NOT_OPTIONAL,
        .message = message,
    });
  }

  return ok(type);
}

AssignChecker::ResultIndexWithinArray AssignChecker::indexProven(const IndexExpr *index) const {
  if (!this->indexProofs->provesInBounds(index->object, index->index, false)) {
    std::string message = ErrorTextAssign::indexRange();

    return err(AssignCheckerIndexProvenError{
        .code = AssignCheckerIndexProvenErrorCode::INDEX_NOT_PROVEN,
        .message = message,
    });
  }

  return ok(index);
}

AssignChecker::ResultFieldTypeIsFound AssignChecker::fieldTypeFrom(const MemberExpr *member,
                                                                   const Type *type) {
  if (type->isVoid() || type->isUndefined()) {
    std::string message = ErrorTextAssign::untyped(member->name);

    return err(AssignCheckerFieldTypeFromError{
        .code = AssignCheckerFieldTypeFromErrorCode::UNTYPED_VALUE,
        .message = message,
    });
  }

  return ok(type);
}

AssignChecker::ResultStringAssignable AssignChecker::stringAssign(BinaryOp op,
                                                                  llvm::StringRef spelling,
                                                                  const Type *type,
                                                                  const AssignExpr *assign) {
  const Type *valueType = assign->value->type;
  if (op != BinaryOp::Add && !valueType->isError()) {
    std::string message = BinaryChecker::operandTypesText(spelling, type, valueType);

    return err(AssignCheckerStringAssignError{
        .code = AssignCheckerStringAssignErrorCode::OPERAND_TYPES,
        .message = message,
    });
  }
  bool joins = valueType->nonOptional()->isNumeric() || valueType->nonOptional()->isBool();
  if (op == BinaryOp::Add && joins) {
    auto [target, value] = AssignChecker::operandTexts(assign->source, assign->loc(), 2);
    std::string hint =
        target.empty() ? ErrorTextCompound::convertTo()
                       : ErrorTextCompound::writeAs(target, SharedHelpers::withoutParens(value));
    std::string message = ErrorTextCompound::joinsText(valueType->name(), hint);

    return err(AssignCheckerStringAssignError{
        .code = AssignCheckerStringAssignErrorCode::JOINS_AS_TEXT,
        .message = message,
    });
  }

  return ok(type);
}

AssignChecker::ResultCompoundOperands
AssignChecker::compoundOperands(BinaryOp op, llvm::StringRef spelling, const Type *type,
                                const Expr *value, bool settled) {
  const Type *valueType = value->type;
  bool shift = op == BinaryOp::Shl || op == BinaryOp::Shr || op == BinaryOp::UShr;
  bool bitwise = op == BinaryOp::BitAnd || op == BinaryOp::BitOr || op == BinaryOp::BitXor;
  bool integerOnly = shift || bitwise;
  auto *zero = dyn_cast<IntLitExpr>(value);
  if (integerOnly && (type->isFloat() || valueType->isFloat())) {
    std::string message = ErrorTextCompound::bitsDouble(spelling);

    return err(AssignCheckerCompoundOperandsError{
        .code = AssignCheckerCompoundOperandsErrorCode::DOUBLE_BITS,
        .message = message,
    });
  }
  if (integerOnly ? !type->isInteger() : !type->isNumeric()) {
    std::string message = BinaryChecker::operandTypesText(spelling, type, valueType);

    return err(AssignCheckerCompoundOperandsError{
        .code = AssignCheckerCompoundOperandsErrorCode::OPERAND_TYPES,
        .message = message,
    });
  }
  bool orZero = (op == BinaryOp::BitOr || op == BinaryOp::UShr) && zero && zero->value == 0 &&
                !type->isWrapping();
  if (orZero) {
    std::string message = ErrorTextCompound::nothing(spelling);

    return err(AssignCheckerCompoundOperandsError{
        .code = AssignCheckerCompoundOperandsErrorCode::NOTHING_CHANGES,
        .message = message,
    });
  }
  if (op == BinaryOp::UShr && !type->isUnsigned()) {
    std::string message = ErrorTextCompound::signedTo(type->name());

    return err(AssignCheckerCompoundOperandsError{
        .code = AssignCheckerCompoundOperandsErrorCode::SIGNED_SHIFT,
        .message = message,
    });
  }
  if (!settled && bitwise && valueType->isInteger() && valueType != type) {
    std::string hint = TypeRelations::conversionHint(value, type);
    std::string message = ErrorTextCompound::mixed(spelling, type->name(), valueType->name(), hint);

    return err(AssignCheckerCompoundOperandsError{
        .code = AssignCheckerCompoundOperandsErrorCode::MIXED_NUMBERS,
        .message = message,
    });
  }

  return ok(type);
}

AssignChecker::ResultElementWriteSafe
AssignChecker::elementWritable(const IndexExpr *index, bool inRange, bool compound) const {
  bool unsafe = !inRange &&
                (compound || !this->indexProofs->provesInBounds(index->object, index->index, true));
  if (unsafe) {
    std::string message = ErrorTextAssign::outOfRange();

    return err(AssignCheckerElementWritableError{
        .code = AssignCheckerElementWritableErrorCode::OUT_OF_RANGE,
        .message = message,
    });
  }

  return ok(index);
}

AssignChecker::ResultLengthIsInBounds AssignChecker::lengthProven(const MemberExpr *member,
                                                                  const Expr *value) const {
  bool proven = this->indexProofs->provesInBounds(member->object, value, true) ||
                this->indexProofs->exempt(value->loc(), member->object);
  if (!proven) {
    std::string array = BytesCheckHelpers::valueText(member->object);
    std::string message =
        ErrorTextAssign::lengthRange(member->object->type->inner()->name(), array);

    return err(AssignCheckerLengthProvenError{
        .code = AssignCheckerLengthProvenErrorCode::LENGTH_NOT_PROVEN,
        .message = message,
    });
  }

  return ok(value);
}

Expr *AssignChecker::checkAssign(AssignExpr *assign) {
  if (auto *member = dyn_cast<MemberExpr>(assign->target)) {
    member->object = this->builtinModuleChecker->processAlias(member->object);
  } else if (auto *index = dyn_cast<IndexExpr>(assign->target)) {
    index->object = this->builtinModuleChecker->processAlias(index->object);
  } else if (auto *name = dyn_cast<NameExpr>(assign->target)) {
    ResultNameIsAssignable assignable = this->notImported(name);
    if (assignable.isErr()) {
      this->state.getDiagnostics().error(name->loc(), assignable.error().message);
      assign->value = this->exprChecker->checkExpr(assign->value);
      assign->type = Type::error();
      return assign;
    }
  }
  if (Expr *exitCode = this->processChecker->checkProcessAssign(assign)) {
    return exitCode;
  }
  bool compound = assign->op.has_value();
  BinaryOp op = compound ? *assign->op : BinaryOp::Add;

  if (compound && (op == BinaryOp::LogOr || op == BinaryOp::LogAnd)) {
    ResultLogicalTargetSet logical = this->checkLogicalAssign(assign);
    if (logical.isErr()) {
      this->state.getDiagnostics().error(assign->target->loc(), logical.error().message);
      return assign;
    }
    return logical.value();
  }

  if (compound && op == BinaryOp::Nullish) {
    if (auto *member = dyn_cast<MemberExpr>(assign->target); member && !member->optional) {
      auto *name = dyn_cast<NameExpr>(member->object);
      VarSymbol *var = name ? this->topLevelChecker->lookupValue(name->name).var() : nullptr;
      if ((var && var->type && var->type->isRecord()) ||
          this->processChecker->isProcessMember(member->object, "env")) {
        Expr *key = this->dictChecker->keyLiteral(member->loc(), member->name);
        auto *index = this->state.getAst().make<IndexExpr>(member->loc(), member->object, key);
        assign->target = index;
        if (Expr *set = this->dictChecker->checkRecordNullishAssign(assign, index)) {
          return set;
        }
      }
    }
    if (isa<MemberExpr>(assign->target)) {
      return this->checkFieldLogicalAssign(assign);
    }
    if (auto *index = dyn_cast<IndexExpr>(assign->target)) {
      if (Expr *set = this->dictChecker->checkRecordNullishAssign(assign, index)) {
        return set;
      }
    }
    auto *name = dyn_cast<NameExpr>(assign->target);
    VarSymbol *var = name ? this->targetChecker->checkVarTarget(name) : nullptr;
    ResultTargetIsVariable variable = AssignChecker::variableTarget(assign->target);
    if (variable.isErr()) {
      this->state.getDiagnostics().error(assign->target->loc(), variable.error().message);
    }
    const Type *type = var ? var->type : Type::error();
    assign->type = type;
    ResultVariableOptional optional = AssignChecker::nullishOptional(name, type);
    if (optional.isErr()) {
      this->state.getDiagnostics().error(assign->loc(), optional.error().message);
      type = Type::error();
    }
    Flow before = this->state.getFlowState().getFlow();
    assign->value =
        this->exprChecker->checkExpr(assign->value, type->isError() ? nullptr : type->inner());
    const Type *valueType = assign->value->type;
    this->state.getFlowState().replaceFlow(
        FlowChecker::join(before, this->state.getFlowState().getFlow()));
    if (type->isError()) {
      return assign;
    }
    bool definite = !valueType->isOptional() && !valueType->isUndefined();
    assign->value = this->typeRelations->coerce(assign->value, definite ? type->inner() : type);
    if (var) {
      if (definite) {
        this->flowChecker->setDefined(var, true);
      }
      assign->type = definite ? type->inner() : type;
    }
    return assign;
  }

  if (auto *target = dyn_cast<NameExpr>(assign->target); target && compound) {
    const VarSymbol *var = this->scopes->lookup(target->name).var();
    if (var && var->type && var->type->nonOptional()->isUnion() &&
        this->genericChecker->narrowedType(var)) {
      auto *read = this->state.getAst().make<NameExpr>(target->loc(), target->name);
      assign->value = this->state.getAst().make<BinaryExpr>(assign->loc(), op, read, assign->value);
      assign->op.reset();
      compound = false;
    }
  }

  const Type *type = this->targetChecker->checkAssignTarget(assign->target, compound);
  assign->type = type;
  auto *index = dyn_cast<IndexExpr>(assign->target);
  auto *member = dyn_cast<MemberExpr>(assign->target);
  auto *name = dyn_cast<NameExpr>(assign->target);
  const VarSymbol *var = name ? name->var() : nullptr;
  bool reportedRange = false;
  if (index && compound && index->object->type->isArray()) {
    ResultIndexWithinArray within = this->indexProven(index);
    if (within.isErr()) {
      this->state.getDiagnostics().error(index->loc(), within.error().message);
      reportedRange = true;
    }
  }

  bool definite = false;
  if (!type) {
    Expr *value = this->exprChecker->checkExpr(assign->value);
    type = value->type;
    ResultFieldTypeIsFound found = AssignChecker::fieldTypeFrom(member, type);
    if (found.isErr()) {
      this->state.getDiagnostics().error(value->loc(), found.error().message);
      type = Type::error();
    }
    ClassDecl *cls = ClassDeclarer::classOf(member->object->type);
    for (ClassField &f : cls->fields) {
      if (f.name == member->name && !f.isStatic()) {
        f.resolved = type;
      }
    }
    assign->value = value;
    assign->type = member->type = type;
    definite = !type->isOptional() && !type->isUndefined();
  } else if (!compound) {
    if (auto *fe = dyn_cast<FunctionExpr>(assign->value); fe && name && fe->fn->name.empty()) {
      fe->fn->name = name->name;
    }
    Expr *value = this->exprChecker->checkExpr(assign->value, type->isError() ? nullptr : type);
    assign->value = this->typeRelations->coerce(value, type);
    definite = !value->type->isOptional() && !value->type->isUndefined();
    if (var) {
      this->flowChecker->assigned(var, value);
    }
    if (var && type->isError() && this->state.getExpressionMarks().hasUntyped()) {
      this->varChecker->assignedUntyped(var, value->type);
    }
  } else {
    std::string spelling = std::string(Operators::opSpelling(op)) + "=";
    if (type->isString()) {
      assign->value = this->exprChecker->checkExpr(assign->value);
      ResultStringAssignable text = AssignChecker::stringAssign(op, spelling, type, assign);
      if (text.isErr()) {
        this->state.getDiagnostics().error(assign->loc(), text.error().message);
      } else if (op == BinaryOp::Add) {
        assign->value = this->concatChecker->toStringOperand(assign->value);
      }
      if (var) {
        this->state.getFlowState().getFacts().forget(var);
      }
    } else {
      bool shift = op == BinaryOp::Shl || op == BinaryOp::Shr || op == BinaryOp::UShr;
      bool bitwise = op == BinaryOp::BitAnd || op == BinaryOp::BitOr || op == BinaryOp::BitXor;
      bool integerOnly = shift || bitwise;
      bool decimalPow = type->isDecimal() && op == BinaryOp::Pow;
      const Type *valueHint = decimalPow ? nullptr : type;
      if (integerOnly && !type->isInteger()) {
        valueHint = nullptr;
      }
      assign->value = this->exprChecker->checkExpr(
          assign->value,
          shift && !TypeRelations::isIntLiteralTree(assign->value) ? nullptr : valueHint);
      const Type *valueType = assign->value->type;
      if (type->isDecimal() && !integerOnly && !valueType->isError()) {
        this->decimalChecker->checkDecimalAssign(assign, op);
      } else if (!type->isError() && !valueType->isError()) {
        bool adapts = !shift && valueType->isInteger() && valueType != type &&
                      this->typeRelations->literalAdapts(assign->value, type);
        bool wraps = !shift && type->isWrapping() && valueType->isInteger() &&
                     (!valueType->isWrapping() || valueType == type);
        ResultCompoundOperands operands =
            AssignChecker::compoundOperands(op, spelling, type, assign->value, adapts || wraps);
        if (operands.isErr()) {
          this->state.getDiagnostics().error(assign->loc(), operands.error().message);
        } else if (adapts) {
          assign->value = this->typeRelations->adaptLiteral(assign->value, type);
        } else if (wraps) {
          assign->value = this->typeRelations->convert(assign->value, type);
          ConstantFolder::ResultDivisorNotZero divisor =
              this->constantFolder->checkDivisor(op, assign->value);
          if (divisor.isErr()) {
            this->state.getDiagnostics().error(assign->value->loc(), divisor.error().message);
          }
        } else {
          assign->value = shift ? this->typeRelations->convert(assign->value, type)
                                : this->typeRelations->coerce(assign->value, type);
          if (type->isInteger()) {
            ConstantFolder::ResultDivisorNotZero divisor =
                this->constantFolder->checkDivisor(op, assign->value);
            if (divisor.isErr()) {
              this->state.getDiagnostics().error(assign->value->loc(), divisor.error().message);
            }
          }
        }
      }
      if (var) {
        std::optional<Linear> delta = this->indexProofs->linear(assign->value);
        if ((op == BinaryOp::Add || op == BinaryOp::Sub) && delta && delta->term.isZero()) {
          this->flowChecker->incremented(var, op == BinaryOp::Add ? delta->offset : -delta->offset);
        } else {
          this->state.getFlowState().getFacts().forget(var);
        }
      }
    }
  }

  if (Expr *set = this->dictChecker->recordAssign(assign)) {
    return set;
  }
  if (index && index->object->type->isTypedArray()) {
    this->bytesChecker->checkBytesStore(index, assign->value);
  }
  if (index && index->object->type->isArray()) {
    bool inRange = this->indexProofs->provesInBounds(index->object, index->index, false);
    ResultElementWriteSafe safe = this->elementWritable(index, inRange, compound);
    if (safe.isErr() && !reportedRange) {
      this->state.getDiagnostics().error(index->loc(), safe.error().message);
    }
    if (ElementIndex *element = index->form.as<ElementIndex>()) {
      element->mayAppend = !inRange;
    }
    if (!inRange) {
      this->flowChecker->arrayResized(index->object->type, true, false);
    }
  }
  if (member && member->object->type->isArray() && !assign->value->type->isError()) {
    const Type *element = member->object->type->inner();
    if (element->hasZeroValue()) {
      std::string what = ErrorTextAssign::length();
      IndexProofs::ResultBetweenProven between =
          this->indexProofs->requireBetween(assign->value, 0, std::nullopt, what);
      if (between.isErr() && between.error().code != IndexProofsRequireBetweenErrorCode::EXEMPT) {
        this->state.getDiagnostics().error(assign->value->loc(), between.error().message);
      }
    } else {
      ResultLengthIsInBounds length = this->lengthProven(member, assign->value);
      if (length.isErr()) {
        this->state.getDiagnostics().error(assign->value->loc(), length.error().message);
      }
    }
    this->flowChecker->arrayResized(member->object->type, true, true);
    if (std::optional<Linear> self = this->indexProofs->lengthOf(member->object);
        self && !self->term.isZero()) {
      if (std::optional<Linear> length = this->indexProofs->linear(assign->value)) {
        this->state.getFlowState().getFacts().addEqual(self->term, length->term, length->offset);
      }
    }
  }
  if (member && member->object->type->isObject()) {
    this->fieldFacts->fieldsAssigned(member->name);
    if (std::optional<FieldPath> path = this->fieldFacts->fieldPath(member); path && definite) {
      this->state.getFlowState().defineField(*path);
    }
    if (this->classChecker->isInitThis(member->object).isOk()) {
      this->state.getFlowState().addSetField(member->name);
    }
  }

  return assign;
}

AssignChecker::ResultLogicalTargetSet AssignChecker::checkLogicalAssign(AssignExpr *assign) {
  const char *op = assign->op == BinaryOp::LogOr ? "||=" : "&&=";
  Expr *read = nullptr;
  if (isa<NameExpr>(assign->target) || isa<MemberExpr>(assign->target)) {
    read = this->copyChain(assign->target);
  } else if (auto *index = dyn_cast<IndexExpr>(assign->target); index && !index->optional) {
    Expr *object = this->copyChain(index->object);
    Expr *position = this->copyChain(index->index);
    if (object && position) {
      read = this->state.getAst().make<IndexExpr>(index->loc(), object, position);
    }
  }
  if (!read) {
    assign->value = this->exprChecker->checkExpr(assign->value);
    assign->type = Type::error();
    std::string message = ErrorTextLogicalAssign::simple(op);

    return err(AssignCheckerCheckLogicalAssignError{
        .code = AssignCheckerCheckLogicalAssignErrorCode::NOT_A_SIMPLE_TARGET,
        .message = message,
    });
  }
  auto *set = this->state.getAst().make<AssignExpr>(assign->loc(), std::nullopt, assign->target,
                                                    assign->value);
  auto *bin = this->state.getAst().make<BinaryExpr>(assign->loc(), *assign->op, read, set);
  Flow whenTrue, whenFalse;
  this->truthChecker->checkLogical(bin, nullptr, whenTrue, whenFalse, false,
                                   bin->op == BinaryOp::LogAnd ? TruthUse::AndAssign
                                                               : TruthUse::OrAssign);
  this->state.getFlowState().replaceFlow(FlowChecker::join(whenTrue, whenFalse));

  return ok(bin);
}

AssignChecker::ResultFieldChainCopied AssignChecker::fieldChain(Expr *read) {
  if (!read) {
    std::string message = ErrorTextLogicalAssign::field();

    return err(AssignCheckerFieldChainError{
        .code = AssignCheckerFieldChainErrorCode::NOT_A_FIELD_CHAIN,
        .message = message,
    });
  }

  return ok(read);
}

AssignChecker::ResultFieldMayBeAbsent AssignChecker::fieldOptional(const MemberExpr *member,
                                                                   const Type *type) {
  if (type && !type->isOptional() && !type->isError()) {
    std::string message = ErrorTextLogicalAssign::fieldOpt(member->name, type->name());

    return err(AssignCheckerFieldOptionalError{
        .code = AssignCheckerFieldOptionalErrorCode::NOT_OPTIONAL,
        .message = message,
    });
  }

  return ok(type);
}

Expr *AssignChecker::checkFieldLogicalAssign(AssignExpr *assign) {
  ResultFieldChainCopied chain = AssignChecker::fieldChain(this->copyChain(assign->target));
  if (chain.isErr()) {
    this->state.getDiagnostics().error(assign->target->loc(), chain.error().message);
    assign->value = this->exprChecker->checkExpr(assign->value);
    assign->type = Type::error();
    return assign;
  }
  Expr *read = chain.value();
  auto *set = this->state.getAst().make<AssignExpr>(assign->loc(), std::nullopt, assign->target,
                                                    assign->value);
  auto *bin = this->state.getAst().make<BinaryExpr>(assign->loc(), BinaryOp::Nullish, read, set);
  BinaryChecker::ResultNullishResolved nullish = this->binaryChecker->checkNullish(bin, nullptr);
  if (nullish.isErr()) {
    this->state.getDiagnostics().error(bin->loc(), nullish.error().message);
  }
  Expr *result = nullish.isOk() ? nullish.value() : bin;
  auto *member = cast<MemberExpr>(set->target);
  ResultFieldMayBeAbsent absent = AssignChecker::fieldOptional(member, member->type);
  if (absent.isErr()) {
    this->state.getDiagnostics().error(assign->loc(), absent.error().message);
    return result;
  }
  const Type *valueType = set->value->type;
  if (auto *conv = dyn_cast<CastExpr>(set->value); conv && conv->isImplicit()) {
    valueType = conv->operand->type;
  }
  if (!valueType->isOptional() && !valueType->isUndefined()) {
    if (std::optional<FieldPath> path = this->fieldFacts->fieldPath(member)) {
      this->state.getFlowState().defineField(*path);
    }
  }

  return result;
}

AssignChecker::ResultIncrementedValue AssignChecker::updatable(const UpdateExpr *update,
                                                               const Type *type) {
  if (!type->isNumeric() && !type->isDecimal()) {
    const char *op = update->increment ? "++" : "--";
    std::string message = ErrorTextUpdate::notNumber(op, type->name());

    return err(AssignCheckerUpdatableError{
        .code = AssignCheckerUpdatableErrorCode::NOT_NUMERIC,
        .message = message,
    });
  }

  return ok(type);
}

Expr *AssignChecker::checkUpdate(UpdateExpr *update) {
  update->type = Type::error();
  const Type *type = this->targetChecker->checkAssignTarget(update->target, true);
  if (type->isError()) {
    return update;
  }
  ResultIncrementedValue numeric = AssignChecker::updatable(update, type);
  if (numeric.isErr()) {
    this->state.getDiagnostics().error(update->loc(), numeric.error().message);
    return update;
  }
  update->type = type;
  if (auto *name = dyn_cast<NameExpr>(update->target)) {
    if (name->var()) {
      this->flowChecker->incremented(name->var(), update->increment ? 1 : -1);
    }
  } else if (auto *index = dyn_cast<IndexExpr>(update->target); index && !index->keyed()) {
    if (index->object->type->isTypedArray()) {
      this->bytesChecker->checkBytesStore(index, nullptr);
    } else {
      ResultIndexWithinArray within = this->indexProven(index);
      if (within.isErr()) {
        this->state.getDiagnostics().error(index->loc(), within.error().message);
      }
    }
  } else if (auto *member = dyn_cast<MemberExpr>(update->target)) {
    this->fieldFacts->fieldsAssigned(member->name);
  }

  return update;
}

}
