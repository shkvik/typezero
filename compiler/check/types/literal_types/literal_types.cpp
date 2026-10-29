#include "compiler/check/types/literal_types/literal_types.h"
#include "compiler/check/scopes/scopes.h"
#include "compiler/check/types/conditional_types/conditional_types.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/check/types/union_check_helpers/union_check_helpers.h"
#include "compiler/whole_program/flows/flows.h"
#include "llvm/Support/ConvertUTF.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

namespace {

llvm::ArrayRef<const Type *> membersOf(const Type *&type) {
  return type->isUnion() ? type->members() : llvm::ArrayRef<const Type *>(type);
}

bool isLiteralSet(const Type *type) {
  if (type->isLiteral()) {
    return true;
  }

  return type->isUnion() &&
         llvm::all_of(type->members(), [](const Type *m) { return m->isLiteral(); });
}

const Type *literalPart(const Type *expected, UnionSlot slot) {
  if (!expected) {
    return nullptr;
  }
  const Type *part = expected->nonOptional()->slotPart(slot);

  return part && isLiteralSet(part) ? part : nullptr;
}

std::string utf8Text(const std::u16string &text) {
  std::string out;
  llvm::convertUTF16ToUTF8String(
      llvm::ArrayRef<llvm::UTF16>(reinterpret_cast<const llvm::UTF16 *>(text.data()), text.size()),
      out);

  return out;
}

std::optional<std::u16string> writtenText(const Expr *e) {
  if (auto *lit = dyn_cast<StringLitExpr>(e)) {
    return lit->value;
  }
  if (auto *tmpl = dyn_cast<TemplateExpr>(e); tmpl && tmpl->exprs.empty()) {
    return tmpl->quasis[0];
  }

  return std::nullopt;
}

bool choiceTexts(const Expr *e, std::vector<std::u16string> &texts) {
  while (auto *conv = dyn_cast<CastExpr>(e)) {
    if (!conv->isImplicit()) {
      return false;
    }
    e = conv->operand;
  }
  if (auto *choice = dyn_cast<ConditionalExpr>(e)) {
    return choiceTexts(choice->thenExpr, texts) && choiceTexts(choice->elseExpr, texts);
  }
  std::optional<std::u16string> text = writtenText(e);
  if (!text) {
    return false;
  }
  if (!llvm::is_contained(texts, *text)) {
    texts.push_back(*text);
  }

  return true;
}

}

LiteralTypes::LiteralTypes(Checker &checker) : checker_(checker) {}

const Type *LiteralTypes::constTextType(const Expr *checked, const Type *expected) {
  auto *name = dyn_cast<NameExpr>(checked);
  if (!name || !name->var || !name->var->literalConst || !checked->type->isString()) {
    return nullptr;
  }
  const Expr *written = checker_.constLiterals_.lookup(name->var);
  std::vector<std::u16string> texts;
  if (!written || !choiceTexts(written, texts)) {
    return nullptr;
  }
  std::vector<const Type *> literals;
  for (const std::u16string &text : texts) {
    const Type *literal = expected ? stringLiteralFor(expected, text) : Type::stringLiteral(text);
    if (!literal) {
      return nullptr;
    }
    literals.push_back(literal);
  }

  return Type::unionOf(literals);
}

Expr *LiteralTypes::constText(Expr *checked, const Type *expected) {
  if (!expected) {
    return checked;
  }
  const Type *type = constTextType(checked, expected);

  return type ? checker_.typeRelations().convert(checked, type) : checked;
}

const Type *LiteralTypes::unionMemberFor(const Type *from, const Type *to) {
  for (const Type *m : to->members()) {
    if (m == from) {
      return m;
    }
  }
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
  std::vector<const Type *> branches;
  for (const Type *branch :
       checker_.conditionalTypes().conditionalBranches(expected->nonOptional())) {
    branches.push_back(literalExpectation(branch, depth + 1));
  }

  return branches.empty() ? expected : Type::unionOf(branches);
}

const Type *LiteralTypes::stringLiteralFor(const Type *expected, const std::u16string &text) {
  expected = literalExpectation(expected);
  if (literalPart(expected, UnionSlot::Atom)) {
    return Type::stringLiteral(text);
  }
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

std::optional<std::pair<int64_t, int64_t>> LiteralTypes::intRange(const Type *type) {
  if (!type->isUnion()) {
    return std::nullopt;
  }
  int64_t lo = INT64_MAX, hi = INT64_MIN;
  for (const Type *m : type->members()) {
    if (!m->isLiteral() || !m->literalBase()->isInt()) {
      return std::nullopt;
    }
    int64_t value = int64_t(m->literalNumber());
    lo = std::min(lo, value);
    hi = std::max(hi, value);
  }
  if (hi - lo + 1 != int64_t(type->members().size())) {
    return std::nullopt;
  }

  return std::pair(lo, hi);
}

const Type *LiteralTypes::numberMemberOf(const Type *expected, bool fraction) {
  if (!expected) {
    return nullptr;
  }
  const Type *type = expected->nonOptional();
  if (!type->isUnion()) {
    return nullptr;
  }
  for (const Type *t : type->members()) {
    if ((t->isInteger() || t->isFloat() || t->isDecimal()) && (!fraction || !t->isInteger())) {
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
          (tag->literalBase()->isInt() &&
           TypeRelations::isImplicitlyConvertible(tag->literalBase(), field))) {
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

const Type *LiteralTypes::typeIn(const Checker::Flow &flow, const VarSymbol *var) const {
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

std::optional<std::vector<std::string>> LiteralTypes::knownTexts(const Expr *arg) {
  auto typeTexts = [](const Type *type) -> std::optional<std::vector<std::string>> {
    if (!type) {
      return std::nullopt;
    }
    type = type->nonOptional();
    llvm::ArrayRef<const Type *> members =
        type->isUnion() ? type->members() : llvm::ArrayRef<const Type *>(type);
    std::vector<std::string> texts;
    for (const Type *m : members) {
      if (!m->isStringLiteral()) {
        return std::nullopt;
      }
      texts.push_back(utf8Text(m->literalText()));
    }
    return texts;
  };
  const Expr *e = arg;
  while (true) {
    if (std::optional<std::vector<std::string>> texts = typeTexts(e->type)) {
      return texts;
    }
    auto *conv = dyn_cast<CastExpr>(e);
    if (!conv || !conv->isImplicit()) {
      break;
    }
    e = conv->operand;
  }
  if (auto *name = dyn_cast<NameExpr>(e)) {
    if (VarSymbol *var = checker_.scopes().lookup(name->name).var) {
      if (std::optional<std::vector<std::string>> texts = typeTexts(typeIn(checker_.flow_, var))) {
        return texts;
      }
      if (const Expr *lit = checker_.constLiterals_.lookup(var)) {
        e = lit;
      }
    }
  }
  if (auto *choice = dyn_cast<ConditionalExpr>(e)) {
    std::optional<std::vector<std::string>> a = knownTexts(choice->thenExpr);
    std::optional<std::vector<std::string>> b = knownTexts(choice->elseExpr);
    if (!a || !b) {
      return std::nullopt;
    }
    a->insert(a->end(), b->begin(), b->end());
    return a;
  }
  if (std::optional<std::u16string> text = writtenText(e)) {
    return std::vector<std::string>{utf8Text(*text)};
  }

  return typeTexts(e->type);
}

}
