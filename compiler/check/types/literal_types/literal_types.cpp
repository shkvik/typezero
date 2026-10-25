#include "compiler/check/types/literal_types/literal_types.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/check/types/conditional_types/conditional_types.h"
#include "compiler/check/types/union_check_helpers/union_check_helpers.h"
#include "compiler/whole_program/flows/flows.h"
#include "llvm/Support/ConvertUTF.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::dyn_cast_or_null;

namespace tz {

namespace {

// The members of a type: those of a union, or the type itself (so `type`
// must outlive the result).
llvm::ArrayRef<const Type *> membersOf(const Type *&type) {
  return type->isUnion() ? type->members() : llvm::ArrayRef<const Type *>(type);
}

// Whether `type` is a literal type or a union of them.
bool isLiteralSet(const Type *type) {
  if (type->isLiteral()) {
    return true;
  }

  return type->isUnion() &&
         llvm::all_of(type->members(), [](const Type *m) { return m->isLiteral(); });
}

// The part of `expected` in `slot` if it is made of literal types only, as
// `0 | 1` is: a literal written there takes its literal type.
const Type *literalPart(const Type *expected, UnionSlot slot) {
  if (!expected) {
    return nullptr;
  }
  const Type *part = expected->nonOptional()->slotPart(slot);

  return part && isLiteralSet(part) ? part : nullptr;
}

} // namespace

LiteralTypes::LiteralTypes(Sema &sema) : sema_(sema) {}

const Type *LiteralTypes::unionMemberFor(const Type *from, const Type *to) {
  for (const Type *m : to->members()) {
    if (m == from) {
      return m;
    }
  }
  // A literal goes into its base: "circle" into string.
  if (from->isLiteral()) {
    for (const Type *m : to->members()) {
      if (m == from->literalBase()) {
        return m;
      }
    }
  }
  for (const Type *m : to->members()) {
    if (!m->isParam() && !from->isParam() && TypeRelations::isImplicitlyConvertible(from, m)) {
      return m;
    }
  }

  return nullptr;
}

const Type *LiteralTypes::unionUnify(const Type *a, const Type *b) {
  if (const Type *common = TypeRelations::unify(a, b)) {
    return common;
  }
  if (a->isError() || b->isError()) {
    return Type::error();
  }
  if (a->isVoid() || b->isVoid()) {
    return nullptr;
  }
  // A decimal and a double do not mix, even in a union (docs/decimal.md).
  auto number = [](const Type *t) {
    t = t->nonOptional();

    return t->isNumeric() || t->isDecimal();
  };
  if (number(a) && number(b)) {
    return nullptr;
  }
  const Type *joined = Type::unionOf({a, b});
  if (joined->isError() || !unionProblem(joined).empty()) {
    return nullptr;
  }

  return joined;
}

const Type *LiteralTypes::literalExpectation(const Type *expected, unsigned depth) {
  if (!expected || depth > 8 || expected->nonOptional()->typeOp() != TypeOp::Conditional) {
    return expected;
  }
  // A conditional type that waits: what its branches expect.
  std::vector<const Type *> branches;
  for (const Type *branch : sema_.conditionalTypes().conditionalBranches(expected->nonOptional())) {
    branches.push_back(literalExpectation(branch, depth + 1));
  }

  return branches.empty() ? expected : Type::unionOf(branches);
}

const Type *LiteralTypes::stringLiteralFor(const Type *expected, const std::u16string &text) {
  expected = literalExpectation(expected);
  if (literalPart(expected, UnionSlot::Atom)) {
    return Type::stringLiteral(text);
  }
  // A literal written where a string of a pattern is expected keeps its text,
  // which tells whether it is one.
  const Type *strings = expected ? expected->nonOptional()->slotPart(UnionSlot::String) : nullptr;
  if (strings &&
      llvm::any_of(strings->isUnion() ? strings->members() : llvm::ArrayRef<const Type *>(strings),
                   [](const Type *m) { return m->isPattern(); })) {
    return Type::stringLiteral(text);
  }

  return nullptr;
}

const Type *LiteralTypes::boolLiteralFor(const Type *expected, bool value) {
  expected = literalExpectation(expected);

  return literalPart(expected, UnionSlot::Bool) ? Type::boolLiteral(value) : nullptr;
}

const Type *LiteralTypes::numberLiteralFor(const Type *expected, double value, bool fraction) {
  expected = literalExpectation(expected);
  if (!fraction) {
    if (const Type *part = literalPart(expected, UnionSlot::Int)) {
      if (llvm::is_contained(membersOf(part), Type::numberLiteral(value, Type::intTy())) ||
          !literalPart(expected, UnionSlot::Double)) {
        return Type::numberLiteral(value, Type::intTy());
      }
    }
  }
  if (literalPart(expected, UnionSlot::Double)) {
    return Type::numberLiteral(value, Type::doubleTy());
  }

  return nullptr;
}

const Type *LiteralTypes::numberMemberOf(const Type *expected, bool fraction) {
  if (!expected) {
    return nullptr;
  }
  const Type *type = expected->nonOptional();
  if (!type->isUnion()) {
    return nullptr;
  }
  for (const Type *t : {Type::intTy(), Type::longTy(), Type::doubleTy(), Type::decimalTy()}) {
    if ((!fraction || !t->isInteger()) && llvm::is_contained(type->members(), t)) {
      return t;
    }
  }

  return nullptr;
}

const Type *LiteralTypes::literalMember(const ObjectLitExpr *lit, const Type *target) const {
  if (llvm::any_of(lit->props, [](const ObjectLitExpr::Property &p) { return p.spread; })) {
    return nullptr;
  }
  const Type *best = nullptr;
  int bestScore = -1;
  bool ambiguous = false;
  for (const Type *m : target->members()) {
    if (!m->isObject() || m->isClass() || m->isTuple() || m->isRegExp() || m->isMatch() ||
        m->isAnyObject() || m->isIncomplete()) {
      continue;
    }
    // Exactly the fields of the type.
    bool fits = llvm::all_of(lit->props, [&](const ObjectLitExpr::Property &p) {
      return m->field(p.name) || m->overload(p.name);
    });
    for (const Field &f : m->fields()) {
      fits &= f.type->isOptional() ||
              llvm::any_of(lit->props,
                           [&](const ObjectLitExpr::Property &p) { return p.name == f.name; });
    }
    if (!fits) {
      continue;
    }
    // The tags it gives tell the members apart: `kind: "circle"`.
    int score = 0;
    bool mismatch = false;
    for (const ObjectLitExpr::Property &p : lit->props) {
      const Field *f = m->field(p.name);
      const Type *tag = f && p.value ? writtenLiteral(p.value) : nullptr;
      if (!tag) {
        continue;
      }
      const Type *field = f->type->nonOptional();
      if (TypeRelations::isImplicitlyConvertible(tag, field) ||
          (tag->literalBase()->isInt() && TypeRelations::isImplicitlyConvertible(tag->literalBase(), field))) {
        score +=
            isLiteralSet(field) || (field->isUnion() && field->slotPart(UnionSlot::Atom)) ? 1 : 0;
      } else if (isLiteralSet(field) || field->isUnion()) {
        mismatch = true;
      }
    }
    if (mismatch) {
      continue;
    }
    if (score > bestScore) {
      best = m;
      bestScore = score;
      ambiguous = false;
    } else if (score == bestScore) {
      ambiguous = true;
    }
  }

  return ambiguous ? nullptr : best;
}

const Type *LiteralTypes::typeIn(const Sema::Flow &flow, const VarSymbol *var) const {
  for (const auto &[narrowed, type] : flow.narrowed) {
    if (narrowed == var) {
      return type;
    }
  }
  if (var->type->isOptional() && flow.defined.contains(var)) {
    return var->type->inner();
  }

  return var->type;
}

} // namespace tz
