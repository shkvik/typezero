#include "compiler/check/expr/assign_checker/assign_checker.h"
#include "compiler/check/scopes/scopes.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/check/stmt/var_checker/var_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/expr/binary_checker/binary_checker.h"
#include "compiler/check/expr/concat_checker/concat_checker.h"
#include "compiler/check/expr/target_checker/target_checker.h"
#include "compiler/declare/class_declarer/class_declarer.h"
#include "compiler/check/classes/class_checker/class_checker.h"
#include "compiler/declare/constant_folder/constant_folder.h"
#include "compiler/check/objects/dict_checker/dict_checker.h"
#include "compiler/check/flow/flow_checker/flow_checker.h"
#include "compiler/check/flow/index_proofs/index_proofs.h"
#include "compiler/check/generics/generic_checker/generic_checker.h"
#include "compiler/check/top_level_checker/top_level_checker.h"
#include "compiler/check/flow/field_facts/field_facts.h"
#include "compiler/check/expr/truth_checker/truth_checker.h"
#include "compiler/library/bytes/bytes_checker/bytes_checker.h"
#include "compiler/library/decimal/decimal_checker/decimal_checker.h"
#include "compiler/library/events/event_target_checker/event_target_checker.h"
#include "compiler/library/builtin_modules/builtin_module_checker/builtin_module_checker.h"
#include "compiler/library/process/process_checker/process_checker.h"
#include "compiler/library/tz_modules/tz_modules_checker/tz_modules_checker.h"
#include "compiler/check/shared_helpers/shared_helpers.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/ErrorHandling.h"
#include <cmath>
#include <functional>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::dyn_cast_or_null;

namespace tz {

// Why an int's `x >>> n` may not go back into an int (see checkBinary).
static const char kUnsignedShift[] =
    "'>>>' is unsigned in JavaScript, and with a count of 0 a negative int becomes a number above "
    "int: keep the result in a long";

namespace {

// The source text of the two sides of `bin`, an operator `length` characters
// long; empty if the source is not known.
std::pair<llvm::StringRef, llvm::StringRef> operandTexts(llvm::StringRef source, llvm::SMLoc op,
                                                         size_t length) {
  const char *at = op.getPointer();
  if (source.empty() || at < source.begin() || at + length > source.end()) {
    return {};
  }

  return {llvm::StringRef(source.begin(), static_cast<size_t>(at - source.begin())).trim(),
          llvm::StringRef(at + length, static_cast<size_t>(source.end() - at - length)).trim()};
}

} // namespace

// After the value of `a[i] = v` is known: the store happens then, so the
// index must be in range at that point.
static const char kWriteOutOfRange[] =
    "cannot prove that the index is within the array (0 <= index <= length); writing further "
    "would leave holes. Check the index first, e.g. 'if (i < a.length)', or use push()";

// A copy of a name, a literal index or a chain of fields of a name (`o.a.b`),
// which reads the same value again; null for anything else.
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

AssignChecker::AssignChecker(Sema &sema) : sema_(sema) {}

Expr *AssignChecker::checkAssign(AssignExpr *assign) {
  // The `o.x = v` of `o.x ??= v` (and of `||=`, `&&=`), not one inside v.
  bool logical = std::exchange(logicalAssign_, false);
  // `env.NAME = v` of node:process is `process.env.NAME = v`; a name
  // imported from a built-in module cannot be assigned to.
  if (auto *member = dyn_cast<MemberExpr>(assign->target)) {
    member->object = sema_.builtinModuleChecker().processAlias(member->object);
  } else if (auto *index = dyn_cast<IndexExpr>(assign->target)) {
    index->object = sema_.builtinModuleChecker().processAlias(index->object);
  } else if (auto *name = dyn_cast<NameExpr>(assign->target); name && sema_.builtinModuleChecker().nodeName(name->name)) {
    sema_.diags_.error(name->loc(), "cannot assign to '" + name->name + "' because it is imported");
    assign->value = sema_.exprChecker().checkExpr(assign->value);
    assign->type = Type::error();
    return assign;
  }
  if (Expr *exitCode = sema_.processChecker().checkProcessAssign(assign)) {
    return exitCode;
  }
  bool compound = assign->op.has_value();
  BinaryOp op = compound ? *assign->op : BinaryOp::Add;

  if (compound && (op == BinaryOp::LogOr || op == BinaryOp::LogAnd)) {
    return checkLogicalAssign(assign);
  }

  if (compound && op == BinaryOp::Nullish) {
    // `r.key ??= v` of a Record (known by its variable, or process.env): a
    // key that may be missing.
    if (auto *member = dyn_cast<MemberExpr>(assign->target); member && !member->optional) {
      auto *name = dyn_cast<NameExpr>(member->object);
      VarSymbol *var = name ? sema_.topLevelChecker().lookupValue(name->name).var : nullptr;
      if ((var && var->type && var->type->isRecord()) || sema_.processChecker().isProcessMember(member->object, "env")) {
        Expr *key = sema_.dictChecker().keyLiteral(member->loc(), member->name);
        auto *index = sema_.ctx_.make<IndexExpr>(member->loc(), member->object, key);
        assign->target = index;
        if (Expr *set = sema_.dictChecker().checkRecordNullishAssign(assign, index)) {
          return set;
        }
      }
    }
    if (isa<MemberExpr>(assign->target)) {
      return checkFieldLogicalAssign(assign);
    }
    if (auto *index = dyn_cast<IndexExpr>(assign->target)) {
      if (Expr *set = sema_.dictChecker().checkRecordNullishAssign(assign, index)) {
        return set;
      }
    }
    // `x ??= v`: assigns only if x is undefined.
    auto *name = dyn_cast<NameExpr>(assign->target);
    VarSymbol *var = name ? sema_.targetChecker().checkVarTarget(name) : nullptr;
    if (!name) {
      sema_.diags_.error(assign->target->loc(), "only variables can be the target of '??='");
    }
    const Type *type = var ? var->type : Type::error();
    assign->type = type;
    if (!type->isOptional() && !type->isError()) {
      sema_.diags_.error(assign->loc(), "'??=' needs a variable that may be undefined; '" + name->name +
                                      "' has type '" + type->name() + "'");
      type = Type::error();
    }
    Sema::Flow before = sema_.flow_;
    assign->value = sema_.exprChecker().checkExpr(assign->value, type->isError() ? nullptr : type->inner());
    const Type *valueType = assign->value->type;
    sema_.flow_ = FlowChecker::join(before, sema_.flow_);
    if (type->isError()) {
      return assign;
    }
    bool definite = !valueType->isOptional() && !valueType->isUndefined();
    assign->value = sema_.typeRelations().coerce(assign->value, definite ? type->inner() : type);
    if (var) {
      if (definite) {
        sema_.flowChecker().setDefined(var, true);
      }
      assign->type = definite ? type->inner() : type;
    }
    return assign;
  }

  // `x += n` of a variable of a union that a check narrowed to a member:
  // `x = x + n`, the sum of the member, which goes back into the union.
  if (auto *target = dyn_cast<NameExpr>(assign->target); target && compound) {
    const VarSymbol *var = sema_.scopes().lookup(target->name).var;
    if (var && var->type && var->type->nonOptional()->isUnion() && sema_.genericChecker().narrowedType(var)) {
      auto *read = sema_.ctx_.make<NameExpr>(target->loc(), target->name);
      assign->value = sema_.ctx_.make<BinaryExpr>(assign->loc(), op, read, assign->value);
      assign->op.reset();
      compound = false;
    }
  }

  const Type *type = sema_.targetChecker().checkAssignTarget(assign->target, compound);
  // `o.x = v` of an accessor, a call of its setter (sema_library.cpp).
  if (sema_.pendingSetter_) {
    return sema_.tzModulesChecker().checkSetterCall(assign);
  }
  assign->type = type;
  auto *index = dyn_cast<IndexExpr>(assign->target);
  auto *member = dyn_cast<MemberExpr>(assign->target);
  auto *name = dyn_cast<NameExpr>(assign->target);
  const VarSymbol *var = name ? name->var : nullptr;
  // `a[i] op= v` reads the element before v is evaluated.
  bool reportedRange = false;
  if (index && compound && index->object->type->isArray() &&
      !sema_.indexProofs().provesInBounds(index->object, index->index, /*allowEnd=*/false)) {
    sema_.diags_.error(index->loc(), "cannot prove that the index is within the array; check it first, "
                               "e.g. 'if (i < a.length)'");
    reportedRange = true;
  }

  bool definite = false; // the value assigned is not undefined
  if (!type) {
    // `this.x = value` in the initializers, where x has the type of the
    // value, as a variable without a type does.
    Expr *value = sema_.exprChecker().checkExpr(assign->value);
    type = value->type;
    if (type->isVoid() || type->isUndefined()) {
      sema_.diags_.error(value->loc(), "cannot infer the type of field '" + member->name +
                                     "' from this value; declare it, e.g. '" + member->name +
                                     ": int | undefined = undefined'");
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
      fe->fn->name = name->name; // as JavaScript names it
    }
    Expr *value = sema_.exprChecker().checkExpr(assign->value, type->isError() ? nullptr : type);
    assign->value = sema_.typeRelations().coerce(value, type);
    definite = !value->type->isOptional() && !value->type->isUndefined();
    if (var) {
      sema_.flowChecker().assigned(var, value);
    }
    if (var && type->isError() && !sema_.untyped_.empty()) {
      sema_.varChecker().assignedUntyped(var, value->type);
    }
  } else {
    std::string spelling = std::string(opSpelling(op)) + "=";
    if (type->isString()) {
      assign->value = sema_.exprChecker().checkExpr(assign->value);
      const Type *valueType = assign->value->type;
      if (op != BinaryOp::Add) {
        if (!valueType->isError()) {
          sema_.binaryChecker().reportOperandTypes(assign->loc(), spelling.c_str(), type, valueType);
        }
      } else if (valueType->nonOptional()->isNumeric() || valueType->nonOptional()->isBool()) {
        // As with `s + 5` (see reportMixedConcat).
        std::string message = "operator '+=' cannot be applied to types 'string' and '" +
                              valueType->name().str() + "' (JavaScript would join them as text)";
        auto [target, value] = operandTexts(assign->source, assign->loc(), 2);
        if (target.empty()) {
          message += "; convert the value with String()";
        } else {
          message += ("; write '" + target + " += String(" + withoutParens(value) + ")'").str();
        }
        sema_.diags_.error(assign->loc(), message);
      } else {
        assign->value = sema_.concatChecker().toStringOperand(assign->value);
      }
      if (var) {
        sema_.flow_.facts.forget(var);
      }
    } else {
      bool shift = op == BinaryOp::Shl || op == BinaryOp::Shr || op == BinaryOp::UShr;
      bool bitwise = op == BinaryOp::BitAnd || op == BinaryOp::BitOr || op == BinaryOp::BitXor;
      bool integerOnly = shift || bitwise;
      // As with `x | v`, v takes no type from x (see checkBinary), and the
      // exponent of a decimal power is an integer.
      bool decimalPow = type->isDecimal() && op == BinaryOp::Pow;
      assign->value = sema_.exprChecker().checkExpr(assign->value, integerOnly || decimalPow ? nullptr : type,
                                /*wraps=*/integerOnly);
      const Type *valueType = assign->value->type;
      if (type->isDecimal() && !integerOnly && !valueType->isError()) {
        sema_.decimalChecker().checkDecimalAssign(assign, op);
      } else if (!type->isError() && !valueType->isError()) {
        if (integerOnly && valueType->isFloat()) {
          assign->value = sema_.binaryChecker().toInt32(assign->value); // as with `x | v`
        }
        if (integerOnly && type->isFloat() && valueType->isNumeric()) {
          // `x |= v` on a double works on 32-bit integers, as in JavaScript;
          // the result goes back into x as a double (of `>>>=`, unsigned).
          assign->value = sema_.binaryChecker().toInt32(assign->value);
        } else if (integerOnly ? !type->isInteger() : !type->isNumeric()) {
          sema_.binaryChecker().reportOperandTypes(assign->loc(), spelling.c_str(), type, valueType);
        } else if (bitwise && assign->value->type->isInteger() && assign->value->type != type) {
          // As with `x | v`: a long next to an int keeps its low 32 bits, and
          // the operator works on 32-bit integers; a long x gets the result
          // back as a long.
          assign->value = sema_.binaryChecker().toInt32(assign->value);
        } else {
          // `x op= v` must produce a value of x's type, so v converts to it.
          assign->value = shift ? sema_.typeRelations().convert(assign->value, type) : sema_.typeRelations().coerce(assign->value, type);
          if (type->isInteger()) {
            sema_.constantFolder().checkDivisor(op, assign->value);
          }
          // `x >>>= n` is `x = x >>> n`, a long unless the count is 1 to 31
          // (see checkBinary), which an int x cannot hold.
          if (op == BinaryOp::UShr && type->isInt()) {
            std::optional<int64_t> count = sema_.constantFolder().intConstant(assign->value);
            if (!count || (*count & 31) == 0) {
              auto [target, value] = operandTexts(assign->source, assign->loc(), 4);
              std::string message =
                  std::string("the result of '>>>=' may not fit in 'int'; ") + kUnsignedShift;
              if (!target.empty()) {
                message += (", e.g. 'const u = " + target + " >>> " + value + "'").str();
              }
              sema_.diags_.error(assign->loc(), message + ", or shift by 1 to 31 bits");
            }
          }
        }
      }
      if (var) {
        std::optional<Linear> delta = sema_.indexProofs().linear(assign->value);
        if ((op == BinaryOp::Add || op == BinaryOp::Sub) && delta && delta->term.isZero()) {
          sema_.flowChecker().incremented(var, op == BinaryOp::Add ? delta->offset : -delta->offset);
        } else {
          sema_.flow_.facts.forget(var);
        }
      }
    }
  }

  if (Expr *set = sema_.dictChecker().recordAssign(assign)) {
    return set;
  }
  if (index && index->object->type->isTypedArray()) {
    sema_.bytesChecker().checkBytesStore(index, assign->value, assign->op, assign->loc());
  }
  if (member && !type->isError()) {
    if (Expr *set = sema_.eventTargetChecker().onabortAssign(assign, member, logical)) {
      return set;
    }
  }
  if (index && index->object->type->isArray()) {
    // The store happens after the value is evaluated, so prove it here.
    bool inRange = sema_.indexProofs().provesInBounds(index->object, index->index, /*allowEnd=*/false);
    if (!inRange && !reportedRange &&
        (compound || !sema_.indexProofs().provesInBounds(index->object, index->index, /*allowEnd=*/true))) {
      sema_.diags_.error(index->loc(), kWriteOutOfRange);
    }
    index->mayAppend = !inRange;
    if (!inRange) {
      sema_.flowChecker().arrayResized(index->object->type, /*grew=*/true, /*shrank=*/false);
    }
  }
  if (member && member->object->type->isArray()) {
    if (std::optional<Linear> length = sema_.indexProofs().linear(assign->value);
        length && length->term.isZero() && length->offset < 0) {
      sema_.diags_.error(assign->value->loc(), "an array length cannot be negative");
    }
    sema_.flowChecker().arrayResized(member->object->type, /*grew=*/true, /*shrank=*/true);
    if (std::optional<Linear> self = sema_.indexProofs().lengthOf(member->object); self && !self->term.isZero()) {
      if (std::optional<Linear> length = sema_.indexProofs().linear(assign->value)) {
        sema_.flow_.facts.addEqual(self->term, length->term, length->offset);
      }
    }
  }
  if (member && member->object->type->isObject()) {
    // The store happens now; until here the field kept its old value.
    sema_.fieldFacts().fieldsAssigned(member->name);
    if (std::optional<Sema::FieldPath> path = sema_.fieldFacts().fieldPath(member); path && definite) {
      FieldFacts::defineField(sema_.flow_, *path);
    }
    if (sema_.classChecker().isInitThis(member->object) && !llvm::is_contained(sema_.flow_.setFields, member->name)) {
      sema_.flow_.setFields.push_back(member->name);
    }
  }

  return assign;
}

// `x ||= v` is `x || (x = v)` and `x &&= v` is `x && (x = v)`: v is evaluated
// and assigned only when x does not decide the result. x is read twice, so it
// must be a variable, a field of one or an element at a simple index. Its
// truth is tested as in a condition.
Expr *AssignChecker::checkLogicalAssign(AssignExpr *assign) {
  const char *op = assign->op == BinaryOp::LogOr ? "||=" : "&&=";
  Expr *read = nullptr;
  if (isa<NameExpr>(assign->target) || isa<MemberExpr>(assign->target)) {
    read = copyChain(sema_.ctx_, assign->target);
  } else if (auto *index = dyn_cast<IndexExpr>(assign->target); index && !index->optional) {
    Expr *object = copyChain(sema_.ctx_, index->object), *position = copyChain(sema_.ctx_, index->index);
    if (object && position) {
      read = sema_.ctx_.make<IndexExpr>(index->loc(), object, position);
    }
  }
  if (!read) {
    sema_.diags_.error(assign->target->loc(), llvm::Twine("'") + op +
                                            "' needs a variable, a field or an array element at a "
                                            "simple index, such as 'a[i]'");
    assign->value = sema_.exprChecker().checkExpr(assign->value);
    assign->type = Type::error();
    return assign;
  }
  auto *set = sema_.ctx_.make<AssignExpr>(assign->loc(), std::nullopt, assign->target, assign->value);
  auto *bin = sema_.ctx_.make<BinaryExpr>(assign->loc(), *assign->op, read, set);
  Sema::Flow whenTrue, whenFalse;
  bool outer = std::exchange(logicalAssign_, true);
  sema_.truthChecker().checkLogical(bin, nullptr, whenTrue, whenFalse, /*tested=*/false,
               bin->op == BinaryOp::LogAnd ? Sema::TruthUse::AndAssign : Sema::TruthUse::OrAssign);
  logicalAssign_ = outer;
  sema_.flow_ = FlowChecker::join(whenTrue, whenFalse);

  return bin;
}

// `o.f ??= v` is `o.f ?? (o.f = v)`: o is read twice, so it must be a
// variable or a field of one.
Expr *AssignChecker::checkFieldLogicalAssign(AssignExpr *assign) {
  Expr *read = copyChain(sema_.ctx_, assign->target);
  if (!read) {
    sema_.diags_.error(assign->target->loc(), "'??=' needs a variable or a field of one, such as 'o.x'");
    assign->value = sema_.exprChecker().checkExpr(assign->value);
    assign->type = Type::error();
    return assign;
  }
  auto *set = sema_.ctx_.make<AssignExpr>(assign->loc(), std::nullopt, assign->target, assign->value);
  auto *bin = sema_.ctx_.make<BinaryExpr>(assign->loc(), BinaryOp::Nullish, read, set);
  bool outer = std::exchange(logicalAssign_, true);
  Expr *result = sema_.binaryChecker().checkNullish(bin, nullptr);
  logicalAssign_ = outer;
  auto *member = cast<MemberExpr>(set->target);
  const Type *type = member->type;
  if (type && !type->isOptional() && !type->isError()) {
    sema_.diags_.error(assign->loc(), "'??=' needs a field that may be undefined; '" + member->name +
                                    "' has type '" + type->name() + "'");
    return result;
  }
  // The field holds a value now, unless the one assigned may be undefined.
  const Type *valueType = set->value->type;
  if (auto *conv = dyn_cast<CastExpr>(set->value); conv && conv->isImplicit()) {
    valueType = conv->operand->type;
  }
  if (!valueType->isOptional() && !valueType->isUndefined()) {
    if (std::optional<Sema::FieldPath> path = sema_.fieldFacts().fieldPath(member)) {
      FieldFacts::defineField(sema_.flow_, *path);
    }
  }

  return result;
}

Expr *AssignChecker::checkUpdate(UpdateExpr *update) {
  update->type = Type::error();
  const Type *type = sema_.targetChecker().checkAssignTarget(update->target, /*compound=*/true);
  if (type->isError()) {
    return update;
  }
  if (!type->isNumeric() && !type->isDecimal()) {
    sema_.diags_.error(update->loc(), llvm::Twine("operator '") + (update->increment ? "++" : "--") +
                                    "' cannot be applied to type '" + type->name() + "'");
    return update;
  }
  update->type = type;
  if (auto *name = dyn_cast<NameExpr>(update->target)) {
    if (name->var) {
      sema_.flowChecker().incremented(name->var, update->increment ? 1 : -1);
    }
  } else if (auto *index = dyn_cast<IndexExpr>(update->target); index && !index->keyed) {
    if (index->object->type->isTypedArray()) {
      sema_.bytesChecker().checkBytesStore(index, nullptr, update->increment ? BinaryOp::Add : BinaryOp::Sub,
                      update->loc());
    } else if (!sema_.indexProofs().provesInBounds(index->object, index->index, /*allowEnd=*/false)) {
      sema_.diags_.error(index->loc(), "cannot prove that the index is within the array; check it "
                                 "first, e.g. 'if (i < a.length)'");
    }
  } else if (auto *member = dyn_cast<MemberExpr>(update->target)) {
    sema_.fieldFacts().fieldsAssigned(member->name);
  }

  return update;
}

} // namespace tz
