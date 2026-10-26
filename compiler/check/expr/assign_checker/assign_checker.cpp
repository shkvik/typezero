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
#include "compiler/library/bytes/bytes_checker/bytes_checker.h"
#include "compiler/library/decimal/decimal_checker/decimal_checker.h"
#include "compiler/library/events/event_target_checker/event_target_checker.h"
#include "compiler/library/process/process_checker/process_checker.h"
#include "compiler/library/tz_modules/tz_modules_checker/tz_modules_checker.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/ErrorHandling.h"
#include <cmath>
#include <functional>

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

static const char kUnsignedShift[] =
    "'>>>' is unsigned in JavaScript, and with a count of 0 a negative int becomes a number above "
    "int: keep the result in a long";

namespace {

std::pair<llvm::StringRef, llvm::StringRef> operandTexts(llvm::StringRef source, llvm::SMLoc op,
                                                         size_t length) {
  const char *at = op.getPointer();
  if (source.empty() || at < source.begin() || at + length > source.end()) {
    return {};
  }

  return {llvm::StringRef(source.begin(), static_cast<size_t>(at - source.begin())).trim(),
          llvm::StringRef(at + length, static_cast<size_t>(source.end() - at - length)).trim()};
}

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

AssignChecker::AssignChecker(Checker &checker) : checker_(checker) {}

Expr *AssignChecker::checkAssign(AssignExpr *assign) {
  bool logical = std::exchange(logicalAssign_, false);
  if (auto *member = dyn_cast<MemberExpr>(assign->target)) {
    member->object = checker_.builtinModuleChecker().processAlias(member->object);
  } else if (auto *index = dyn_cast<IndexExpr>(assign->target)) {
    index->object = checker_.builtinModuleChecker().processAlias(index->object);
  } else if (auto *name = dyn_cast<NameExpr>(assign->target);
             name && checker_.builtinModuleChecker().nodeName(name->name)) {
    checker_.diags_.error(name->loc(),
                          "cannot assign to '" + name->name + "' because it is imported");
    assign->value = checker_.exprChecker().checkExpr(assign->value);
    assign->type = Type::error();
    return assign;
  }
  if (Expr *exitCode = checker_.processChecker().checkProcessAssign(assign)) {
    return exitCode;
  }
  bool compound = assign->op.has_value();
  BinaryOp op = compound ? *assign->op : BinaryOp::Add;

  if (compound && (op == BinaryOp::LogOr || op == BinaryOp::LogAnd)) {
    return checkLogicalAssign(assign);
  }

  if (compound && op == BinaryOp::Nullish) {
    if (auto *member = dyn_cast<MemberExpr>(assign->target); member && !member->optional) {
      auto *name = dyn_cast<NameExpr>(member->object);
      VarSymbol *var = name ? checker_.topLevelChecker().lookupValue(name->name).var : nullptr;
      if ((var && var->type && var->type->isRecord()) ||
          checker_.processChecker().isProcessMember(member->object, "env")) {
        Expr *key = checker_.dictChecker().keyLiteral(member->loc(), member->name);
        auto *index = checker_.ctx_.make<IndexExpr>(member->loc(), member->object, key);
        assign->target = index;
        if (Expr *set = checker_.dictChecker().checkRecordNullishAssign(assign, index)) {
          return set;
        }
      }
    }
    if (isa<MemberExpr>(assign->target)) {
      return checkFieldLogicalAssign(assign);
    }
    if (auto *index = dyn_cast<IndexExpr>(assign->target)) {
      if (Expr *set = checker_.dictChecker().checkRecordNullishAssign(assign, index)) {
        return set;
      }
    }
    auto *name = dyn_cast<NameExpr>(assign->target);
    VarSymbol *var = name ? checker_.targetChecker().checkVarTarget(name) : nullptr;
    if (!name) {
      checker_.diags_.error(assign->target->loc(), "only variables can be the target of '??='");
    }
    const Type *type = var ? var->type : Type::error();
    assign->type = type;
    if (!type->isOptional() && !type->isError()) {
      checker_.diags_.error(assign->loc(), "'??=' needs a variable that may be undefined; '" +
                                               name->name + "' has type '" + type->name() + "'");
      type = Type::error();
    }
    Checker::Flow before = checker_.flow_;
    assign->value =
        checker_.exprChecker().checkExpr(assign->value, type->isError() ? nullptr : type->inner());
    const Type *valueType = assign->value->type;
    checker_.flow_ = FlowChecker::join(before, checker_.flow_);
    if (type->isError()) {
      return assign;
    }
    bool definite = !valueType->isOptional() && !valueType->isUndefined();
    assign->value = checker_.typeRelations().coerce(assign->value, definite ? type->inner() : type);
    if (var) {
      if (definite) {
        checker_.flowChecker().setDefined(var, true);
      }
      assign->type = definite ? type->inner() : type;
    }
    return assign;
  }

  if (auto *target = dyn_cast<NameExpr>(assign->target); target && compound) {
    const VarSymbol *var = checker_.scopes().lookup(target->name).var;
    if (var && var->type && var->type->nonOptional()->isUnion() &&
        checker_.genericChecker().narrowedType(var)) {
      auto *read = checker_.ctx_.make<NameExpr>(target->loc(), target->name);
      assign->value = checker_.ctx_.make<BinaryExpr>(assign->loc(), op, read, assign->value);
      assign->op.reset();
      compound = false;
    }
  }

  const Type *type = checker_.targetChecker().checkAssignTarget(assign->target, compound);
  if (checker_.pendingSetter_) {
    return checker_.tzModulesChecker().checkSetterCall(assign);
  }
  assign->type = type;
  auto *index = dyn_cast<IndexExpr>(assign->target);
  auto *member = dyn_cast<MemberExpr>(assign->target);
  auto *name = dyn_cast<NameExpr>(assign->target);
  const VarSymbol *var = name ? name->var : nullptr;
  bool reportedRange = false;
  if (index && compound && index->object->type->isArray() &&
      !checker_.indexProofs().provesInBounds(index->object, index->index, false)) {
    checker_.diags_.error(index->loc(),
                          "cannot prove that the index is within the array; check it first, "
                          "e.g. 'if (i < a.length)'");
    reportedRange = true;
  }

  bool definite = false;
  if (!type) {
    Expr *value = checker_.exprChecker().checkExpr(assign->value);
    type = value->type;
    if (type->isVoid() || type->isUndefined()) {
      checker_.diags_.error(value->loc(), "cannot infer the type of field '" + member->name +
                                              "' from this value; declare it, e.g. '" +
                                              member->name + ": int | undefined = undefined'");
      type = Type::error();
    }
    ClassDecl *cls = ClassDeclarer::classOf(member->object->type);
    for (ClassField &f : cls->fields) {
      if (f.name == member->name && !f.isStatic) {
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
    Expr *value = checker_.exprChecker().checkExpr(assign->value, type->isError() ? nullptr : type);
    assign->value = checker_.typeRelations().coerce(value, type);
    definite = !value->type->isOptional() && !value->type->isUndefined();
    if (var) {
      checker_.flowChecker().assigned(var, value);
    }
    if (var && type->isError() && !checker_.untyped_.empty()) {
      checker_.varChecker().assignedUntyped(var, value->type);
    }
  } else {
    std::string spelling = std::string(opSpelling(op)) + "=";
    if (type->isString()) {
      assign->value = checker_.exprChecker().checkExpr(assign->value);
      const Type *valueType = assign->value->type;
      if (op != BinaryOp::Add) {
        if (!valueType->isError()) {
          checker_.binaryChecker().reportOperandTypes(assign->loc(), spelling.c_str(), type,
                                                      valueType);
        }
      } else if (valueType->nonOptional()->isNumeric() || valueType->nonOptional()->isBool()) {
        std::string message = "operator '+=' cannot be applied to types 'string' and '" +
                              valueType->name().str() + "' (JavaScript would join them as text)";
        auto [target, value] = operandTexts(assign->source, assign->loc(), 2);
        if (target.empty()) {
          message += "; convert the value with String()";
        } else {
          message += ("; write '" + target + " += String(" + withoutParens(value) + ")'").str();
        }
        checker_.diags_.error(assign->loc(), message);
      } else {
        assign->value = checker_.concatChecker().toStringOperand(assign->value);
      }
      if (var) {
        checker_.flow_.facts.forget(var);
      }
    } else {
      bool shift = op == BinaryOp::Shl || op == BinaryOp::Shr || op == BinaryOp::UShr;
      bool bitwise = op == BinaryOp::BitAnd || op == BinaryOp::BitOr || op == BinaryOp::BitXor;
      bool integerOnly = shift || bitwise;
      bool decimalPow = type->isDecimal() && op == BinaryOp::Pow;
      assign->value = checker_.exprChecker().checkExpr(
          assign->value, integerOnly || decimalPow ? nullptr : type, integerOnly);
      const Type *valueType = assign->value->type;
      if (type->isDecimal() && !integerOnly && !valueType->isError()) {
        checker_.decimalChecker().checkDecimalAssign(assign, op);
      } else if (!type->isError() && !valueType->isError()) {
        if (integerOnly && valueType->isFloat()) {
          assign->value = checker_.binaryChecker().toInt32(assign->value);
        }
        if (integerOnly && type->isFloat() && valueType->isNumeric()) {
          assign->value = checker_.binaryChecker().toInt32(assign->value);
        } else if (integerOnly ? !type->isInteger() : !type->isNumeric()) {
          checker_.binaryChecker().reportOperandTypes(assign->loc(), spelling.c_str(), type,
                                                      valueType);
        } else if (bitwise && assign->value->type->isInteger() && assign->value->type != type) {
          assign->value = checker_.binaryChecker().toInt32(assign->value);
        } else {
          assign->value = shift ? checker_.typeRelations().convert(assign->value, type)
                                : checker_.typeRelations().coerce(assign->value, type);
          if (type->isInteger()) {
            checker_.constantFolder().checkDivisor(op, assign->value);
          }
          if (op == BinaryOp::UShr && type->isInt()) {
            std::optional<int64_t> count = checker_.constantFolder().intConstant(assign->value);
            if (!count || (*count & 31) == 0) {
              auto [target, value] = operandTexts(assign->source, assign->loc(), 4);
              std::string message =
                  std::string("the result of '>>>=' may not fit in 'int'; ") + kUnsignedShift;
              if (!target.empty()) {
                message += (", e.g. 'const u = " + target + " >>> " + value + "'").str();
              }
              checker_.diags_.error(assign->loc(), message + ", or shift by 1 to 31 bits");
            }
          }
        }
      }
      if (var) {
        std::optional<Linear> delta = checker_.indexProofs().linear(assign->value);
        if ((op == BinaryOp::Add || op == BinaryOp::Sub) && delta && delta->term.isZero()) {
          checker_.flowChecker().incremented(var,
                                             op == BinaryOp::Add ? delta->offset : -delta->offset);
        } else {
          checker_.flow_.facts.forget(var);
        }
      }
    }
  }

  if (Expr *set = checker_.dictChecker().recordAssign(assign)) {
    return set;
  }
  if (index && index->object->type->isTypedArray()) {
    checker_.bytesChecker().checkBytesStore(index, assign->value, assign->op, assign->loc());
  }
  if (member && !type->isError()) {
    if (Expr *set = checker_.eventTargetChecker().onabortAssign(assign, member, logical)) {
      return set;
    }
  }
  if (index && index->object->type->isArray()) {
    bool inRange = checker_.indexProofs().provesInBounds(index->object, index->index, false);
    if (!inRange && !reportedRange &&
        (compound || !checker_.indexProofs().provesInBounds(index->object, index->index, true))) {
      checker_.diags_.error(index->loc(), kWriteOutOfRange);
    }
    index->mayAppend = !inRange;
    if (!inRange) {
      checker_.flowChecker().arrayResized(index->object->type, true, false);
    }
  }
  if (member && member->object->type->isArray()) {
    if (std::optional<Linear> length = checker_.indexProofs().linear(assign->value);
        length && length->term.isZero() && length->offset < 0) {
      checker_.diags_.error(assign->value->loc(), "an array length cannot be negative");
    }
    checker_.flowChecker().arrayResized(member->object->type, true, true);
    if (std::optional<Linear> self = checker_.indexProofs().lengthOf(member->object);
        self && !self->term.isZero()) {
      if (std::optional<Linear> length = checker_.indexProofs().linear(assign->value)) {
        checker_.flow_.facts.addEqual(self->term, length->term, length->offset);
      }
    }
  }
  if (member && member->object->type->isObject()) {
    checker_.fieldFacts().fieldsAssigned(member->name);
    if (std::optional<Checker::FieldPath> path = checker_.fieldFacts().fieldPath(member);
        path && definite) {
      FieldFacts::defineField(checker_.flow_, *path);
    }
    if (checker_.classChecker().isInitThis(member->object) &&
        !llvm::is_contained(checker_.flow_.setFields, member->name)) {
      checker_.flow_.setFields.push_back(member->name);
    }
  }

  return assign;
}

Expr *AssignChecker::checkLogicalAssign(AssignExpr *assign) {
  const char *op = assign->op == BinaryOp::LogOr ? "||=" : "&&=";
  Expr *read = nullptr;
  if (isa<NameExpr>(assign->target) || isa<MemberExpr>(assign->target)) {
    read = copyChain(checker_.ctx_, assign->target);
  } else if (auto *index = dyn_cast<IndexExpr>(assign->target); index && !index->optional) {
    Expr *object = copyChain(checker_.ctx_, index->object),
         *position = copyChain(checker_.ctx_, index->index);
    if (object && position) {
      read = checker_.ctx_.make<IndexExpr>(index->loc(), object, position);
    }
  }
  if (!read) {
    checker_.diags_.error(assign->target->loc(),
                          llvm::Twine("'") + op +
                              "' needs a variable, a field or an array element at a "
                              "simple index, such as 'a[i]'");
    assign->value = checker_.exprChecker().checkExpr(assign->value);
    assign->type = Type::error();
    return assign;
  }
  auto *set =
      checker_.ctx_.make<AssignExpr>(assign->loc(), std::nullopt, assign->target, assign->value);
  auto *bin = checker_.ctx_.make<BinaryExpr>(assign->loc(), *assign->op, read, set);
  Checker::Flow whenTrue, whenFalse;
  bool outer = std::exchange(logicalAssign_, true);
  checker_.truthChecker().checkLogical(bin, nullptr, whenTrue, whenFalse, false,
                                       bin->op == BinaryOp::LogAnd ? Checker::TruthUse::AndAssign
                                                                   : Checker::TruthUse::OrAssign);
  logicalAssign_ = outer;
  checker_.flow_ = FlowChecker::join(whenTrue, whenFalse);

  return bin;
}

Expr *AssignChecker::checkFieldLogicalAssign(AssignExpr *assign) {
  Expr *read = copyChain(checker_.ctx_, assign->target);
  if (!read) {
    checker_.diags_.error(assign->target->loc(),
                          "'??=' needs a variable or a field of one, such as 'o.x'");
    assign->value = checker_.exprChecker().checkExpr(assign->value);
    assign->type = Type::error();
    return assign;
  }
  auto *set =
      checker_.ctx_.make<AssignExpr>(assign->loc(), std::nullopt, assign->target, assign->value);
  auto *bin = checker_.ctx_.make<BinaryExpr>(assign->loc(), BinaryOp::Nullish, read, set);
  bool outer = std::exchange(logicalAssign_, true);
  Expr *result = checker_.binaryChecker().checkNullish(bin, nullptr);
  logicalAssign_ = outer;
  auto *member = cast<MemberExpr>(set->target);
  const Type *type = member->type;
  if (type && !type->isOptional() && !type->isError()) {
    checker_.diags_.error(assign->loc(), "'??=' needs a field that may be undefined; '" +
                                             member->name + "' has type '" + type->name() + "'");
    return result;
  }
  const Type *valueType = set->value->type;
  if (auto *conv = dyn_cast<CastExpr>(set->value); conv && conv->isImplicit()) {
    valueType = conv->operand->type;
  }
  if (!valueType->isOptional() && !valueType->isUndefined()) {
    if (std::optional<Checker::FieldPath> path = checker_.fieldFacts().fieldPath(member)) {
      FieldFacts::defineField(checker_.flow_, *path);
    }
  }

  return result;
}

Expr *AssignChecker::checkUpdate(UpdateExpr *update) {
  update->type = Type::error();
  const Type *type = checker_.targetChecker().checkAssignTarget(update->target, true);
  if (type->isError()) {
    return update;
  }
  if (!type->isNumeric() && !type->isDecimal()) {
    checker_.diags_.error(update->loc(), llvm::Twine("operator '") +
                                             (update->increment ? "++" : "--") +
                                             "' cannot be applied to type '" + type->name() + "'");
    return update;
  }
  update->type = type;
  if (auto *name = dyn_cast<NameExpr>(update->target)) {
    if (name->var) {
      checker_.flowChecker().incremented(name->var, update->increment ? 1 : -1);
    }
  } else if (auto *index = dyn_cast<IndexExpr>(update->target); index && !index->keyed) {
    if (index->object->type->isTypedArray()) {
      checker_.bytesChecker().checkBytesStore(
          index, nullptr, update->increment ? BinaryOp::Add : BinaryOp::Sub, update->loc());
    } else if (!checker_.indexProofs().provesInBounds(index->object, index->index, false)) {
      checker_.diags_.error(index->loc(),
                            "cannot prove that the index is within the array; check it "
                            "first, e.g. 'if (i < a.length)'");
    }
  } else if (auto *member = dyn_cast<MemberExpr>(update->target)) {
    checker_.fieldFacts().fieldsAssigned(member->name);
  }

  return update;
}

}
