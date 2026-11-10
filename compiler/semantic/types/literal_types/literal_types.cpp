#include "compiler/semantic/types/literal_types/literal_types.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "compiler/semantic/declare/type_declarer/type_declarer.h"
#include "compiler/semantic/flow/children_first/children_first.h"
#include "compiler/semantic/flow/expr_parts/expr_parts.h"
#include "compiler/semantic/names/scopes/scopes.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/conditional_types/conditional_types.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "compiler/semantic/types/union_check_helpers/union_check_helpers.h"
#include "llvm/Support/ConvertUTF.h"

using llvm::dyn_cast;
using llvm::dyn_cast_or_null;

namespace tz {

LiteralTypes::LiteralTypes(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->scopes = &all.getScopes();
  this->typeDeclarer = &all.getTypeDeclarer();
  this->typeRelations = &all.getTypeRelations();
  this->conditionalTypes = &all.getConditionalTypes();
}

llvm::ArrayRef<const Type *> LiteralTypes::membersOf(const Type *&type) {
  return type->isUnion() ? type->members() : llvm::ArrayRef<const Type *>(type);
}

bool LiteralTypes::isLiteralSet(const Type *type) {
  if (type->isLiteral()) {
    return true;
  }

  return type->isUnion() &&
         llvm::all_of(type->members(), [](const Type *m) { return m->isLiteral(); });
}

const Type *LiteralTypes::literalPart(const Type *expected, UnionSlot slot) {
  if (!expected) {
    return nullptr;
  }
  const Type *part = expected->nonOptional()->slotPart(slot);

  return part && LiteralTypes::isLiteralSet(part) ? part : nullptr;
}

std::optional<std::u16string> LiteralTypes::writtenText(const Expr *e) {
  if (auto *lit = dyn_cast<StringLitExpr>(e)) {
    return lit->value;
  }
  if (auto *tmpl = dyn_cast<TemplateExpr>(e); tmpl && tmpl->exprs.empty()) {
    return tmpl->quasis[0];
  }

  return std::nullopt;
}

bool LiteralTypes::choiceTexts(const Expr *e, std::vector<std::u16string> &texts) {
  for (const Expr *branch : ExprParts::choiceBranches(e)) {
    std::optional<std::u16string> text = LiteralTypes::writtenText(branch);
    if (!text.has_value()) {
      return false;
    }
    if (!llvm::is_contained(texts, *text)) {
      texts.push_back(*text);
    }
  }

  return true;
}

const Type *LiteralTypes::enumTag(const Expr *e) const {
  auto *member = dyn_cast<MemberExpr>(e);
  auto *owner = member ? dyn_cast<NameExpr>(member->object) : nullptr;
  EnumDecl *decl = owner ? this->scopes->lookup(owner->name).enumDecl() : nullptr;
  if (!decl) {
    return nullptr;
  }
  this->typeDeclarer->resolveEnum(decl);
  const EnumMember *m = decl->find(member->name);
  if (!m) {
    return nullptr;
  }

  return m->isString ? Type::stringLiteral(m->text)
                     : Type::numberLiteral(double(m->number), Type::intTy());
}

const Type *LiteralTypes::constTextType(const Expr *checked, const Type *expected) {
  auto *name = dyn_cast<NameExpr>(checked);
  if (!name || !name->var() || !name->var()->literalConst || !checked->type->isString()) {
    return nullptr;
  }
  const Expr *written = this->state.getConstantStore().findConstLiteral(name->var());
  std::vector<std::u16string> texts;
  if (!written || !LiteralTypes::choiceTexts(written, texts)) {
    return nullptr;
  }
  std::vector<const Type *> literals;
  for (const std::u16string &text : texts) {
    const Type *literal =
        expected ? this->stringLiteralFor(expected, text) : Type::stringLiteral(text);
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
  const Type *type = this->constTextType(checked, expected);

  return type ? this->typeRelations->convert(checked, type) : checked;
}

const Type *LiteralTypes::exactMemberFor(const Type *from, const Type *to) {
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

  return nullptr;
}

const Type *LiteralTypes::unionUnify(const Type *a, const Type *b) {
  return TypeRelations::unifyAs(UnifyForm::UNION, a, b);
}

const Type *LiteralTypes::unionFallback(const Type *a, const Type *b) {
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
  if (joined->isError() || !UnionSlots::unionProblem(joined).empty()) {
    return nullptr;
  }

  return joined;
}

const Type *LiteralTypes::literalExpectation(const Type *expected, unsigned depth) {
  std::vector<ExpectationStep> steps = {EnterExpectation{expected, depth}};
  std::vector<const Type *> answers;
  while (!steps.empty()) {
    ExpectationStep step = steps.back();
    steps.pop_back();
    if (const EnterExpectation *enter = step.as<EnterExpectation>()) {
      this->enterExpectation(*enter, steps, answers);
    } else {
      LiteralTypes::joinBranches(*step.as<JoinBranches>(), answers);
    }
  }

  return answers.back();
}

void LiteralTypes::enterExpectation(const EnterExpectation &enter,
                                    std::vector<ExpectationStep> &steps,
                                    std::vector<const Type *> &answers) {
  const Type *expected = enter.type;
  bool conditional =
      expected && enter.depth <= 8 && expected->nonOptional()->typeOp() == TypeOp::Conditional;
  std::vector<const Type *> branches =
      conditional ? this->conditionalTypes->conditionalBranches(expected->nonOptional())
                  : std::vector<const Type *>();
  if (branches.empty()) {
    answers.push_back(expected);
  } else {
    steps.push_back(JoinBranches{branches.size()});
    for (const Type *branch : llvm::reverse(branches)) {
      steps.push_back(EnterExpectation{branch, enter.depth + 1});
    }
  }
}

void LiteralTypes::joinBranches(const JoinBranches &join, std::vector<const Type *> &answers) {
  std::vector<const Type *> members = ChildrenFirst::takeAnswers(answers, join.count);
  answers.push_back(Type::unionOf(members));
}

const Type *LiteralTypes::stringLiteralFor(const Type *expected, const std::u16string &text) {
  expected = this->literalExpectation(expected);
  if (LiteralTypes::literalPart(expected, UnionSlot::Atom)) {
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
  expected = this->literalExpectation(expected);

  return LiteralTypes::literalPart(expected, UnionSlot::Bool) ? Type::boolLiteral(value) : nullptr;
}

const Type *LiteralTypes::numberLiteralFor(const Type *expected, double value, bool fraction) {
  expected = this->literalExpectation(expected);
  if (!fraction) {
    if (const Type *part = LiteralTypes::literalPart(expected, UnionSlot::Int)) {
      if (llvm::is_contained(LiteralTypes::membersOf(part),
                             Type::numberLiteral(value, Type::intTy())) ||
          !LiteralTypes::literalPart(expected, UnionSlot::Double)) {
        return Type::numberLiteral(value, Type::intTy());
      }
    }
  }
  if (LiteralTypes::literalPart(expected, UnionSlot::Double)) {
    return Type::numberLiteral(value, Type::doubleTy());
  }

  return nullptr;
}

std::optional<std::pair<int64_t, int64_t>> LiteralTypes::intRange(const Type *type) {
  if (!type->isUnion()) {
    return std::nullopt;
  }
  int64_t lo = INT64_MAX;
  int64_t hi = INT64_MIN;
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

std::vector<std::pair<int64_t, int64_t>> LiteralTypes::intIntervals(const Type *type) {
  std::vector<std::pair<int64_t, int64_t>> intervals;
  if (!type->isUnion()) {
    return intervals;
  }
  std::vector<int64_t> values;
  for (const Type *m : type->members()) {
    if (!m->isLiteral() || !m->literalBase()->isInt()) {
      return intervals;
    }
    values.push_back(int64_t(m->literalNumber()));
  }
  llvm::sort(values);
  for (int64_t value : values) {
    if (!intervals.empty() && intervals.back().second + 1 >= value) {
      intervals.back().second = std::max(intervals.back().second, value);
      continue;
    }
    intervals.emplace_back(value, value);
  }

  return intervals;
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
  if (llvm::any_of(lit->props, [](const ObjectLitExpr::Property &p) { return p.spread(); })) {
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
      const Type *tag = f && p.value ? UnionCheckHelpers::writtenLiteral(p.value) : nullptr;
      if (!tag && f && p.value) {
        tag = this->enumTag(p.value);
      }
      if (auto *named = !tag && f ? dyn_cast_or_null<NameExpr>(p.value) : nullptr) {
        const VarSymbol *var = this->scopes->lookup(named->name).var();
        const Type *known =
            var && var->type ? this->typeIn(this->state.getFlowState().getFlow(), var) : nullptr;
        if (known && LiteralTypes::isLiteralSet(known)) {
          tag = known;
        }
      }
      if (!tag) {
        continue;
      }
      const Type *field = f->type->nonOptional();
      if (TypeRelations::isImplicitlyConvertible(tag, field) ||
          (tag->isLiteral() && tag->literalBase()->isInt() &&
           TypeRelations::isImplicitlyConvertible(tag->literalBase(), field))) {
        score += LiteralTypes::isLiteralSet(field) ||
                         (field->isUnion() && field->slotPart(UnionSlot::Atom))
                     ? 1
                     : 0;
      } else if (LiteralTypes::isLiteralSet(field) || field->isUnion()) {
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

const Type *LiteralTypes::typeIn(const Flow &flow, const VarSymbol *var) const {
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
  std::vector<std::string> texts;
  std::vector<const Expr *> stack = {arg};
  while (!stack.empty()) {
    const Expr *next = stack.back();
    stack.pop_back();
    TextLookup lookup = this->lookupTexts(next);
    if (lookup.choice != nullptr) {
      stack.push_back(lookup.choice->elseExpr);
      stack.push_back(lookup.choice->thenExpr);
    } else if (lookup.texts.has_value()) {
      texts.insert(texts.end(), lookup.texts->begin(), lookup.texts->end());
    } else {
      return std::nullopt;
    }
  }

  return texts;
}

TextLookup LiteralTypes::lookupTexts(const Expr *arg) {
  const Expr *e = arg;
  while (true) {
    if (std::optional<std::vector<std::string>> texts = LiteralTypes::typeTexts(e->type)) {
      return TextLookup{texts, nullptr};
    }
    auto *conv = dyn_cast<CastExpr>(e);
    if (!conv || !conv->isImplicit()) {
      break;
    }
    e = conv->operand;
  }
  if (auto *name = dyn_cast<NameExpr>(e)) {
    if (VarSymbol *var = this->scopes->lookup(name->name).var()) {
      const Type *flowType = this->typeIn(this->state.getFlowState().getFlow(), var);
      if (std::optional<std::vector<std::string>> texts = LiteralTypes::typeTexts(flowType)) {
        return TextLookup{texts, nullptr};
      }
      if (const Expr *lit = this->state.getConstantStore().findConstLiteral(var)) {
        e = lit;
      }
    }
  }
  if (auto *choice = dyn_cast<ConditionalExpr>(e)) {
    return TextLookup{std::nullopt, choice};
  }
  if (std::optional<std::u16string> text = LiteralTypes::writtenText(e)) {
    return TextLookup{std::vector<std::string>{ToUtf8::toUtf8(*text)}, nullptr};
  }

  return TextLookup{LiteralTypes::typeTexts(e->type), nullptr};
}

std::optional<std::vector<std::string>> LiteralTypes::typeTexts(const Type *type) {
  if (!type) {
    return std::nullopt;
  }
  const Type *plain = type->nonOptional();
  llvm::ArrayRef<const Type *> members =
      plain->isUnion() ? plain->members() : llvm::ArrayRef<const Type *>(plain);
  std::vector<std::string> texts;
  for (const Type *m : members) {
    if (!m->isStringLiteral()) {
      return std::nullopt;
    }
    texts.push_back(ToUtf8::toUtf8(m->literalText()));
  }

  return texts;
}

}
