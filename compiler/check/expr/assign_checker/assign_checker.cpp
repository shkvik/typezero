#include "compiler/check/expr/assign_checker/assign_checker.h"
#include "compiler/check/classes/class_checker/class_checker.h"
#include "compiler/check/expr/binary_checker/binary_checker.h"
#include "compiler/check/expr/concat_checker/concat_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/expr/target_checker/target_checker.h"
#include "compiler/check/expr/truth_checker/truth_checker.h"
#include "compiler/check/flow/field_facts/field_facts.h"
#include "compiler/check/flow/flow_checker/flow_checker.h"
#include "compiler/check/flow/index_proofs/index_proofs.h"
#include "compiler/check/generics/generic_checker/generic_checker.h"
#include "compiler/check/objects/dict_checker/dict_checker.h"
#include "compiler/check/scopes/scopes.h"
#include "compiler/check/shared_helpers/shared_helpers.h"
#include "compiler/check/stmt/var_checker/var_checker.h"
#include "compiler/check/top_level_checker/top_level_checker.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/declare/class_declarer/class_declarer.h"
#include "compiler/declare/constant_folder/constant_folder.h"
#include "compiler/library/builtin_modules/builtin_module_checker/builtin_module_checker.h"
#include "compiler/library/bytes/bytes_check_helpers/bytes_check_helpers.h"
#include "compiler/library/bytes/bytes_checker/bytes_checker.h"
#include "compiler/library/decimal/decimal_checker/decimal_checker.h"
#include "compiler/library/process/process_checker/process_checker.h"
#include "compiler/library/tz_modules/tz_modules_checker/tz_modules_checker.h"
#include "llvm/ADT/STLExtras.h"
#include <cmath>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

std::pair<llvm::StringRef, llvm::StringRef>
AssignChecker::operandTexts(llvm::StringRef source, llvm::SMLoc op, size_t length) {
  const char *at = op.getPointer();
  if (source.empty() || at < source.begin() || at + length > source.end()) {
    return {};
  }

  return {llvm::StringRef(source.begin(), static_cast<size_t>(at - source.begin())).trim(),
          llvm::StringRef(at + length, static_cast<size_t>(source.end() - at - length)).trim()};
}

static const char kWriteOutOfRange[] =
    "cannot prove that the index is within the array (0 <= index <= length); writing further "
    "would leave holes. Check the index first, e.g. 'if (i < a.length)', or use push()";

static Expr *copyChain(AstContext &ctx, const Expr *e) {
  if (auto *name = dyn_cast<NameExpr>(e)) {
    return ctx.make<NameExpr>(name->loc(), name->name);
  }
  if (auto *lit = dyn_cast<IntLitExpr>(e)) {
    return ctx.make<IntLitExpr>(lit->loc(), lit->value);
  }
  if (auto *lit = dyn_cast<StringLitExpr>(e)) {
    return ctx.make<StringLitExpr>(lit->loc(), lit->value);
  }
  if (auto *member = dyn_cast<MemberExpr>(e); member && !member->optional) {
    if (Expr *object = copyChain(ctx, member->object)) {
      return ctx.make<MemberExpr>(member->loc(), object, member->name);
    }
  }

  return nullptr;
}

AssignChecker::AssignChecker(Checker &owner)
    : checker(owner) {}

Expr *AssignChecker::checkAssign(AssignExpr *assign) {
  this->logicalAssign = false;
  if (auto *member = dyn_cast<MemberExpr>(assign->target)) {
    member->object = this->checker.builtinModuleChecker().processAlias(member->object);
  } else if (auto *index = dyn_cast<IndexExpr>(assign->target)) {
    index->object = this->checker.builtinModuleChecker().processAlias(index->object);
  } else if (auto *name = dyn_cast<NameExpr>(assign->target);
             name && this->checker.builtinModuleChecker().nodeName(name->name)) {
    this->checker.diags_.error(name->loc(),
                               "cannot assign to '" + name->name + "' because it is imported");
    assign->value = this->checker.exprChecker().checkExpr(assign->value);
    assign->type = Type::error();
    return assign;
  }
  if (Expr *exitCode = this->checker.processChecker().checkProcessAssign(assign)) {
    return exitCode;
  }
  bool compound = assign->op.has_value();
  BinaryOp op = compound ? *assign->op : BinaryOp::Add;

  if (compound && (op == BinaryOp::LogOr || op == BinaryOp::LogAnd)) {
    return this->checkLogicalAssign(assign);
  }

  if (compound && op == BinaryOp::Nullish) {
    if (auto *member = dyn_cast<MemberExpr>(assign->target); member && !member->optional) {
      auto *name = dyn_cast<NameExpr>(member->object);
      VarSymbol *var =
          name ? this->checker.topLevelChecker().lookupValue(name->name).var() : nullptr;
      if ((var && var->type && var->type->isRecord()) ||
          this->checker.processChecker().isProcessMember(member->object, "env")) {
        Expr *key = this->checker.dictChecker().keyLiteral(member->loc(), member->name);
        auto *index = this->checker.ctx_.make<IndexExpr>(member->loc(), member->object, key);
        assign->target = index;
        if (Expr *set = this->checker.dictChecker().checkRecordNullishAssign(assign, index)) {
          return set;
        }
      }
    }
    if (isa<MemberExpr>(assign->target)) {
      return this->checkFieldLogicalAssign(assign);
    }
    if (auto *index = dyn_cast<IndexExpr>(assign->target)) {
      if (Expr *set = this->checker.dictChecker().checkRecordNullishAssign(assign, index)) {
        return set;
      }
    }
    auto *name = dyn_cast<NameExpr>(assign->target);
    VarSymbol *var = name ? this->checker.targetChecker().checkVarTarget(name) : nullptr;
    if (!name) {
      this->checker.diags_.error(assign->target->loc(),
                                 "only variables can be the target of '??='");
    }
    const Type *type = var ? var->type : Type::error();
    assign->type = type;
    if (!type->isOptional() && !type->isError()) {
      this->checker.diags_.error(assign->loc(), "'??=' needs a variable that may be undefined; '" +
                                                    name->name + "' has type '" + type->name() +
                                                    "'");
      type = Type::error();
    }
    Checker::Flow before = this->checker.flow_;
    assign->value = this->checker.exprChecker().checkExpr(
        assign->value, type->isError() ? nullptr : type->inner());
    const Type *valueType = assign->value->type;
    this->checker.flow_ = FlowChecker::join(before, this->checker.flow_);
    if (type->isError()) {
      return assign;
    }
    bool definite = !valueType->isOptional() && !valueType->isUndefined();
    assign->value =
        this->checker.typeRelations().coerce(assign->value, definite ? type->inner() : type);
    if (var) {
      if (definite) {
        this->checker.flowChecker().setDefined(var, true);
      }
      assign->type = definite ? type->inner() : type;
    }
    return assign;
  }

  if (auto *target = dyn_cast<NameExpr>(assign->target); target && compound) {
    const VarSymbol *var = this->checker.scopes().lookup(target->name).var();
    if (var && var->type && var->type->nonOptional()->isUnion() &&
        this->checker.genericChecker().narrowedType(var)) {
      auto *read = this->checker.ctx_.make<NameExpr>(target->loc(), target->name);
      assign->value = this->checker.ctx_.make<BinaryExpr>(assign->loc(), op, read, assign->value);
      assign->op.reset();
      compound = false;
    }
  }

  const Type *type = this->checker.targetChecker().checkAssignTarget(assign->target, compound);
  assign->type = type;
  auto *index = dyn_cast<IndexExpr>(assign->target);
  auto *member = dyn_cast<MemberExpr>(assign->target);
  auto *name = dyn_cast<NameExpr>(assign->target);
  const VarSymbol *var = name ? name->var() : nullptr;
  bool reportedRange = false;
  if (index && compound && index->object->type->isArray() &&
      !this->checker.indexProofs().provesInBounds(index->object, index->index, false)) {
    this->checker.diags_.error(index->loc(),
                               "cannot prove that the index is within the array; check it first, "
                               "e.g. 'if (i < a.length)'");
    reportedRange = true;
  }

  bool definite = false;
  if (!type) {
    Expr *value = this->checker.exprChecker().checkExpr(assign->value);
    type = value->type;
    if (type->isVoid() || type->isUndefined()) {
      this->checker.diags_.error(value->loc(), "cannot infer the type of field '" + member->name +
                                                   "' from this value; declare it, e.g. '" +
                                                   member->name + ": int | undefined = undefined'");
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
    Expr *value =
        this->checker.exprChecker().checkExpr(assign->value, type->isError() ? nullptr : type);
    assign->value = this->checker.typeRelations().coerce(value, type);
    definite = !value->type->isOptional() && !value->type->isUndefined();
    if (var) {
      this->checker.flowChecker().assigned(var, value);
    }
    if (var && type->isError() && !this->checker.untyped_.empty()) {
      this->checker.varChecker().assignedUntyped(var, value->type);
    }
  } else {
    std::string spelling = std::string(opSpelling(op)) + "=";
    if (type->isString()) {
      assign->value = this->checker.exprChecker().checkExpr(assign->value);
      const Type *valueType = assign->value->type;
      if (op != BinaryOp::Add) {
        if (!valueType->isError()) {
          this->checker.binaryChecker().reportOperandTypes(assign->loc(), spelling.c_str(), type,
                                                           valueType);
        }
      } else if (valueType->nonOptional()->isNumeric() || valueType->nonOptional()->isBool()) {
        std::string message = "operator '+=' cannot be applied to types 'string' and '" +
                              valueType->name().str() + "' (JavaScript would join them as text)";
        auto [target, value] = AssignChecker::operandTexts(assign->source, assign->loc(), 2);
        if (target.empty()) {
          message += "; convert the value with String()";
        } else {
          message += ("; write '" + target + " += String(" + withoutParens(value) + ")'").str();
        }
        this->checker.diags_.error(assign->loc(), message);
      } else {
        assign->value = this->checker.concatChecker().toStringOperand(assign->value);
      }
      if (var) {
        this->checker.flow_.facts.forget(var);
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
      assign->value = this->checker.exprChecker().checkExpr(
          assign->value,
          shift && !TypeRelations::isIntLiteralTree(assign->value) ? nullptr : valueHint);
      const Type *valueType = assign->value->type;
      auto *zero = dyn_cast<IntLitExpr>(assign->value);
      if (type->isDecimal() && !integerOnly && !valueType->isError()) {
        this->checker.decimalChecker().checkDecimalAssign(assign, op);
      } else if (!type->isError() && !valueType->isError()) {
        if (integerOnly && (type->isFloat() || valueType->isFloat())) {
          this->checker.diags_.error(assign->loc(),
                                     "operator '" + spelling +
                                         "' works on the bits of integers, and this is a double; "
                                         "drop the fraction first, e.g. 'Math.trunc(x).toInt()'");
        } else if (integerOnly ? !type->isInteger() : !type->isNumeric()) {
          this->checker.binaryChecker().reportOperandTypes(assign->loc(), spelling.c_str(), type,
                                                           valueType);
        } else if ((op == BinaryOp::BitOr || op == BinaryOp::UShr) && zero && zero->value == 0 &&
                   !type->isWrapping()) {
          this->checker.diags_.error(assign->loc(), "'" + spelling +
                                                        " 0' changes nothing in tz: integers are "
                                                        "exact, and nothing wraps around here; to "
                                                        "count modulo 2^32, use 'Bit.Wrap32'");
        } else if (op == BinaryOp::UShr && !type->isUnsigned()) {
          this->checker.diags_.error(assign->loc(),
                                     "'>>>=' shifts in zeros, as a shift of an unsigned "
                                     "type does; '" +
                                         type->name().str() + "' is signed: use '>>='");
        } else if (!shift && valueType->isInteger() && valueType != type &&
                   this->checker.typeRelations().literalAdapts(assign->value, type)) {
          assign->value = this->checker.typeRelations().adaptLiteral(assign->value, type);
        } else if (!shift && type->isWrapping() && valueType->isInteger() &&
                   (!valueType->isWrapping() || valueType == type)) {
          assign->value = this->checker.typeRelations().convert(assign->value, type);
          ConstantFolder::ResultDivisorNotZero divisor =
              this->checker.constantFolder().checkDivisor(op, assign->value);
          if (divisor.isErr()) {
            this->checker.diags_.error(assign->value->loc(), divisor.error().message);
          }
        } else if (bitwise && valueType->isInteger() && valueType != type) {
          this->checker.diags_.error(assign->loc(),
                                     "operator '" + spelling + "' cannot be applied to types '" +
                                         type->name().str() + "' and '" + valueType->name().str() +
                                         "': a number changes its type only explicitly" +
                                         TypeRelations::conversionHint(assign->value, type));
        } else {
          assign->value = shift ? this->checker.typeRelations().convert(assign->value, type)
                                : this->checker.typeRelations().coerce(assign->value, type);
          if (type->isInteger()) {
            ConstantFolder::ResultDivisorNotZero divisor =
                this->checker.constantFolder().checkDivisor(op, assign->value);
            if (divisor.isErr()) {
              this->checker.diags_.error(assign->value->loc(), divisor.error().message);
            }
          }
        }
      }
      if (var) {
        std::optional<Linear> delta = this->checker.indexProofs().linear(assign->value);
        if ((op == BinaryOp::Add || op == BinaryOp::Sub) && delta && delta->term.isZero()) {
          this->checker.flowChecker().incremented(var, op == BinaryOp::Add ? delta->offset
                                                                           : -delta->offset);
        } else {
          this->checker.flow_.facts.forget(var);
        }
      }
    }
  }

  if (Expr *set = this->checker.dictChecker().recordAssign(assign)) {
    return set;
  }
  if (index && index->object->type->isTypedArray()) {
    this->checker.bytesChecker().checkBytesStore(index, assign->value);
  }
  if (index && index->object->type->isArray()) {
    bool inRange = this->checker.indexProofs().provesInBounds(index->object, index->index, false);
    if (!inRange && !reportedRange &&
        (compound ||
         !this->checker.indexProofs().provesInBounds(index->object, index->index, true))) {
      this->checker.diags_.error(index->loc(), kWriteOutOfRange);
    }
    if (ElementIndex *element = index->form.as<ElementIndex>()) {
      element->mayAppend = !inRange;
    }
    if (!inRange) {
      this->checker.flowChecker().arrayResized(index->object->type, true, false);
    }
  }
  if (member && member->object->type->isArray() && !assign->value->type->isError()) {
    const Type *element = member->object->type->inner();
    if (element->hasZeroValue()) {
      IndexProofs::ResultBetweenProven between = this->checker.indexProofs().requireBetween(
          assign->value, 0, std::nullopt, "an array length");
      if (between.isErr() && between.error().code != IndexProofsRequireBetweenErrorCode::EXEMPT) {
        this->checker.diags_.error(assign->value->loc(), between.error().message);
      }
    } else if (!this->checker.indexProofs().provesInBounds(member->object, assign->value, true) &&
               !this->checker.indexProofs().exempt(assign->value->loc(), member->object)) {
      this->checker.diags_.error(
          assign->value->loc(),
          "cannot prove that the new length is from 0 to the old one: an array "
          "of '" +
              element->name() +
              "' cannot grow by setting its length, which would leave elements "
              "without a value; check it first, e.g. 'if (n >= 0 && n <= " +
              valueText(member->object) + ".length)', or use push()");
    }
    this->checker.flowChecker().arrayResized(member->object->type, true, true);
    if (std::optional<Linear> self = this->checker.indexProofs().lengthOf(member->object);
        self && !self->term.isZero()) {
      if (std::optional<Linear> length = this->checker.indexProofs().linear(assign->value)) {
        this->checker.flow_.facts.addEqual(self->term, length->term, length->offset);
      }
    }
  }
  if (member && member->object->type->isObject()) {
    this->checker.fieldFacts().fieldsAssigned(member->name);
    if (std::optional<Checker::FieldPath> path = this->checker.fieldFacts().fieldPath(member);
        path && definite) {
      FieldFacts::defineField(this->checker.flow_, *path);
    }
    if (this->checker.classChecker().isInitThis(member->object).isOk() &&
        !llvm::is_contained(this->checker.flow_.setFields, member->name)) {
      this->checker.flow_.setFields.push_back(member->name);
    }
  }

  return assign;
}

Expr *AssignChecker::checkLogicalAssign(AssignExpr *assign) {
  const char *op = assign->op == BinaryOp::LogOr ? "||=" : "&&=";
  Expr *read = nullptr;
  if (isa<NameExpr>(assign->target) || isa<MemberExpr>(assign->target)) {
    read = copyChain(this->checker.ctx_, assign->target);
  } else if (auto *index = dyn_cast<IndexExpr>(assign->target); index && !index->optional) {
    Expr *object = copyChain(this->checker.ctx_, index->object),
         *position = copyChain(this->checker.ctx_, index->index);
    if (object && position) {
      read = this->checker.ctx_.make<IndexExpr>(index->loc(), object, position);
    }
  }
  if (!read) {
    this->checker.diags_.error(assign->target->loc(),
                               llvm::Twine("'") + op +
                                   "' needs a variable, a field or an array element at a "
                                   "simple index, such as 'a[i]'");
    assign->value = this->checker.exprChecker().checkExpr(assign->value);
    assign->type = Type::error();
    return assign;
  }
  auto *set = this->checker.ctx_.make<AssignExpr>(assign->loc(), std::nullopt, assign->target,
                                                  assign->value);
  auto *bin = this->checker.ctx_.make<BinaryExpr>(assign->loc(), *assign->op, read, set);
  Checker::Flow whenTrue, whenFalse;
  bool outer = std::exchange(this->logicalAssign, true);
  this->checker.truthChecker().checkLogical(
      bin, nullptr, whenTrue, whenFalse, false,
      bin->op == BinaryOp::LogAnd ? Checker::TruthUse::AndAssign : Checker::TruthUse::OrAssign);
  this->logicalAssign = outer;
  this->checker.flow_ = FlowChecker::join(whenTrue, whenFalse);

  return bin;
}

Expr *AssignChecker::checkFieldLogicalAssign(AssignExpr *assign) {
  Expr *read = copyChain(this->checker.ctx_, assign->target);
  if (!read) {
    this->checker.diags_.error(assign->target->loc(),
                               "'??=' needs a variable or a field of one, such as 'o.x'");
    assign->value = this->checker.exprChecker().checkExpr(assign->value);
    assign->type = Type::error();
    return assign;
  }
  auto *set = this->checker.ctx_.make<AssignExpr>(assign->loc(), std::nullopt, assign->target,
                                                  assign->value);
  auto *bin = this->checker.ctx_.make<BinaryExpr>(assign->loc(), BinaryOp::Nullish, read, set);
  bool outer = std::exchange(this->logicalAssign, true);
  Expr *result = this->checker.binaryChecker().checkNullish(bin, nullptr);
  this->logicalAssign = outer;
  auto *member = cast<MemberExpr>(set->target);
  const Type *type = member->type;
  if (type && !type->isOptional() && !type->isError()) {
    this->checker.diags_.error(assign->loc(), "'??=' needs a field that may be undefined; '" +
                                                  member->name + "' has type '" + type->name() +
                                                  "'");
    return result;
  }
  const Type *valueType = set->value->type;
  if (auto *conv = dyn_cast<CastExpr>(set->value); conv && conv->isImplicit()) {
    valueType = conv->operand->type;
  }
  if (!valueType->isOptional() && !valueType->isUndefined()) {
    if (std::optional<Checker::FieldPath> path = this->checker.fieldFacts().fieldPath(member)) {
      FieldFacts::defineField(this->checker.flow_, *path);
    }
  }

  return result;
}

Expr *AssignChecker::checkUpdate(UpdateExpr *update) {
  update->type = Type::error();
  const Type *type = this->checker.targetChecker().checkAssignTarget(update->target, true);
  if (type->isError()) {
    return update;
  }
  if (!type->isNumeric() && !type->isDecimal()) {
    this->checker.diags_.error(update->loc(),
                               llvm::Twine("operator '") + (update->increment ? "++" : "--") +
                                   "' cannot be applied to type '" + type->name() + "'");
    return update;
  }
  update->type = type;
  if (auto *name = dyn_cast<NameExpr>(update->target)) {
    if (name->var()) {
      this->checker.flowChecker().incremented(name->var(), update->increment ? 1 : -1);
    }
  } else if (auto *index = dyn_cast<IndexExpr>(update->target); index && !index->keyed()) {
    if (index->object->type->isTypedArray()) {
      this->checker.bytesChecker().checkBytesStore(index, nullptr);
    } else if (!this->checker.indexProofs().provesInBounds(index->object, index->index, false)) {
      this->checker.diags_.error(index->loc(),
                                 "cannot prove that the index is within the array; check it "
                                 "first, e.g. 'if (i < a.length)'");
    }
  } else if (auto *member = dyn_cast<MemberExpr>(update->target)) {
    this->checker.fieldFacts().fieldsAssigned(member->name);
  }

  return update;
}

}
