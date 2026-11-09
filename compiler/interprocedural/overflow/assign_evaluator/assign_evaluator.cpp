#include "compiler/interprocedural/overflow/assign_evaluator/assign_evaluator.h"
#include "compiler/interprocedural/overflow/call_ranges/call_ranges.h"
#include "compiler/interprocedural/overflow/evaluator/evaluator.h"
#include "compiler/interprocedural/overflow/executor/executor.h"
#include "compiler/interprocedural/overflow/loop_ranges/loop_ranges.h"
#include "compiler/interprocedural/overflow/narrowing/narrowing.h"
#include "compiler/interprocedural/overflow/overflow_helpers/overflow_helpers.h"
#include "compiler/interprocedural/overflow/overflow_warnings/overflow_warnings.h"
#include "compiler/interprocedural/overflow/program_scan/program_scan.h"
#include "compiler/interprocedural/overflow/slots/slots.h"
#include "compiler/interprocedural/overflow/value_range/value_range.h"
#include "compiler/program/library/builtin_names/builtin_property_table/builtin_property_table.h"
#include "compiler/program/types/types.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Path.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace tz {

using llvm::cast;

using llvm::dyn_cast;

AssignEvaluator::AssignEvaluator(OverflowCheck &check)
    : check(check) {}

OverflowCheck::Place AssignEvaluator::place(const Expr *target, State &st) {
  OverflowCheck::Place p;
  p.type = target->type;
  if (auto *name = dyn_cast<NameExpr>(target)) {
    if (name->var()) {
      p.kind = OverflowCheck::Place::Kind::Var;
      p.var = name->var();
      p.type = name->var()->type;
    }
    return p;
  }
  if (auto *member = dyn_cast<MemberExpr>(target)) {
    this->check.evaluator().eval(member->object, st);
    const Type *objectType = member->object->type ? member->object->type->nonOptional() : nullptr;
    if (objectType && BuiltinPropertyTable::isName(member->name, BuiltinProperty::LENGTH) &&
        objectType->isArray()) {
      p.kind = OverflowCheck::Place::Kind::Length;
      if (auto *name = dyn_cast<NameExpr>(OverflowHelpers::stripCasts(member->object))) {
        p.var = name->var();
      }
      return p;
    }
    if (objectType && objectType->isObject()) {
      p.kind = OverflowCheck::Place::Kind::Field;
      p.objectType = objectType;
      p.field = member->name.str();
    }
    return p;
  }
  if (auto *index = dyn_cast<IndexExpr>(target)) {
    this->check.evaluator().eval(index->object, st);
    this->check.evaluator().eval(index->index, st);
    const Type *objectType = index->object->type ? index->object->type->nonOptional() : nullptr;
    if (objectType && objectType->isArray()) {
      p.kind = OverflowCheck::Place::Kind::Element;
      p.objectType = objectType;
      p.append = index->mayAppend();
      if (auto *name = dyn_cast<NameExpr>(OverflowHelpers::stripCasts(index->object))) {
        p.var = name->var();
      }
    } else if (objectType && objectType->isObject() && !index->field().empty()) {
      p.kind = OverflowCheck::Place::Kind::Field;
      p.objectType = objectType;
      p.field = index->field().str();
    }
    return p;
  }

  return p;
}

Val AssignEvaluator::read(const OverflowCheck::Place &p, State &st, const Expr *target) {
  switch (p.kind) {
  case OverflowCheck::Place::Kind::Var:
    return this->check.slots().readVar(p.var, st);
  case OverflowCheck::Place::Kind::Field:
    return this->check.slots().readField(p.objectType, p.field, p.type);
  case OverflowCheck::Place::Kind::Element: {
    const Expr *array = cast<IndexExpr>(target)->object;
    Val arr = p.var ? this->check.slots().readVar(p.var, st) : Val();
    if (!p.var) {
      return this->check.overflowWarnings().trusted(p.type);
    }
    return this->check.slots().elementsOf(array, arr, p.objectType);
  }
  case OverflowCheck::Place::Kind::Length: {
    if (!p.var) {
      return ValueRange::numVal(ValueRange::someLength());
    }
    return ValueRange::numVal(this->check.slots().readVar(p.var, st).len);
  }
  case OverflowCheck::Place::Kind::None:
    return this->check.overflowWarnings().trusted(p.type);
  }

  return Val();
}

void AssignEvaluator::write(const OverflowCheck::Place &p, const Val &v, State &st) {
  switch (p.kind) {
  case OverflowCheck::Place::Kind::Var:
    this->check.slots().writeVar(p.var, v, st);
    return;
  case OverflowCheck::Place::Kind::Field:
    this->check.slots().storeField(p.objectType, p.field, p.type, v);
    return;
  case OverflowCheck::Place::Kind::Element: {
    const Type *element = p.objectType->inner();
    this->check.contribute(this->check.next.elements[element],
                           this->check.overflowWarnings().fitVal(v, element));
    if (p.var) {
      auto found = st.vars.find(p.var);
      if (found != st.vars.end()) {
        Val &array = found->second;
        if (array.elem) {
          array.elem = ValueRange::share(
              ValueRange::join(*array.elem, this->check.overflowWarnings().fitVal(v, element)));
        }
        if (p.append && !array.len.empty()) {
          array.len = ValueRange::narrowTo(
              ValueRange::join(array.len, ValueRange::add(array.len, ValueRange::exactly(1))), 0,
              kLengthMax);
        }
      }
    }
    return;
  }
  case OverflowCheck::Place::Kind::Length:
    if (p.var) {
      auto found = st.vars.find(p.var);
      if (found != st.vars.end() && !v.num.empty()) {
        found->second.len = ValueRange::narrowTo(v.num, 0, kLengthMax);
      }
    }
    return;
  case OverflowCheck::Place::Kind::None:
    return;
  }
}

Val AssignEvaluator::evalAssign(const AssignExpr *assign, State &st) {
  if (!assign->op) {
    OverflowCheck::Place p = place(assign->target, st);
    Val v = this->check.evaluator().eval(assign->value, st);
    write(p, v, st);
    return v;
  }
  BinaryOp op = *assign->op;
  OverflowCheck::Place p = place(assign->target, st);
  Val old = read(p, st, assign->target);
  if (op == BinaryOp::LogAnd || op == BinaryOp::LogOr || op == BinaryOp::Nullish) {
    State other = st;
    Val v = this->check.evaluator().eval(assign->value, other);
    st = ValueRange::join(st, other);
    Val result = ValueRange::join(old, v);
    write(p, result, st);
    return result;
  }
  bool bits = op == BinaryOp::BitAnd || op == BinaryOp::BitOr || op == BinaryOp::BitXor ||
              op == BinaryOp::Shl || op == BinaryOp::Shr || op == BinaryOp::UShr;
  Val v = this->check.evaluator().eval(assign->value, st, bits);
  const Type *type = assign->target->type;
  Val result;
  if (type && type->nonOptional()->isString()) {
    result = ValueRange::lengthVal(
        old.len.empty() || v.len.empty()
            ? ValueRange::someLength()
            : ValueRange::narrowTo(ValueRange::add(old.len, v.len), 0, kLengthMax));
  } else if (ValueRange::isNumberType(type)) {
    if (bits) {
      result = ValueRange::numVal(this->check.evaluator().bitwise(op, old.num, v.num, type));
    } else {
      const VarSymbol *accum = p.kind == OverflowCheck::Place::Kind::Var ? p.var : nullptr;
      result = ValueRange::numVal(this->check.evaluator().arithmetic(
          assign, op, old.num, v.num, type, false, assign->target, assign->value, accum));
    }
  }
  write(p, result, st);

  return result;
}

Val AssignEvaluator::evalUpdate(const UpdateExpr *update, State &st) {
  OverflowCheck::Place p = place(update->target, st);
  Val old = read(p, st, update->target);
  const Type *type = update->target->type;
  if (!ValueRange::isNumberType(type)) {
    return Val();
  }
  Num result = this->check.evaluator().arithmetic(
      update, update->increment ? BinaryOp::Add : BinaryOp::Sub, old.num, ValueRange::exactly(1),
      type, false, update->target, nullptr,
      p.kind == OverflowCheck::Place::Kind::Var ? p.var : nullptr);
  write(p, ValueRange::numVal(result), st);

  return update->prefix ? ValueRange::numVal(result) : old;
}

Val AssignEvaluator::evalObjectLit(const ObjectLitExpr *lit, State &st) {
  const Type *type = lit->type ? lit->type->nonOptional() : nullptr;
  for (const ObjectLitExpr::Property &p : lit->props) {
    if (p.key()) {
      this->check.evaluator().eval(p.key(), st);
    }
    Val v = this->check.evaluator().eval(p.value, st);
    if (!type || !type->isObject() || p.method()) {
      continue;
    }
    if (p.spread()) {
      const Type *from = p.value->type ? p.value->type->nonOptional() : nullptr;
      if (!from || !from->isObject()) {
        continue;
      }
      for (const Field &f : from->fields()) {
        if (type->field(f.name) && ValueRange::tracked(f.type) &&
            !llvm::is_contained(p.replacedFields(), f.name)) {
          this->check.slots().storeField(type, f.name, f.type,
                                         this->check.slots().readField(from, f.name, f.type));
        }
      }
      continue;
    }
    if (const Field *f = p.name.empty() ? nullptr : type->field(p.name)) {
      this->check.slots().storeField(type, p.name, f->type, v);
    }
  }

  return Val();
}

}
