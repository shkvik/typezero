#include "compiler/semantic/declare/constant_folder/constant_folder.h"
#include "compiler/program/library/builtin_names/global_constant_table/global_constant_table.h"
#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/semantic/flow/children_first/children_first.h"
#include "compiler/semantic/flow/expr_parts/expr_parts.h"
#include "compiler/semantic/names/scopes/scopes.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/CheckedArithmetic.h"
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>

using llvm::dyn_cast;

namespace tz {

namespace {

constexpr double kTwoTo32 = 4294967296.0;
constexpr double kNoExactBelow = 1e21;
constexpr unsigned kWideBits = 128;
constexpr int kShiftMask = 31;

}

ConstantFolder::ConstantFolder(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->scopes = &all.scopes;
}

int32_t ConstantFolder::jsToInt32(double number) const {
  if (!std::isfinite(number)) {
    return 0;
  }
  double modulo = std::fmod(std::trunc(number), kTwoTo32);
  if (modulo < 0) {
    modulo += kTwoTo32;
  }

  return static_cast<int32_t>(static_cast<uint32_t>(modulo));
}

double ConstantFolder::power(double base, double exponent) const {
  if (std::isnan(exponent) || (std::fabs(base) == 1 && std::isinf(exponent))) {
    return std::numeric_limits<double>::quiet_NaN();
  }

  return std::pow(base, exponent);
}

std::string ConstantFolder::formatNumber(double value) const {
  char text[32];
  std::snprintf(text, sizeof text, "%.15g", value);

  return text;
}

int64_t ConstantFolder::wrapTo(const Type *type, int64_t value) const {
  int bits = int(type->bitWidth());
  if (bits == 64) {
    return value;
  }
  uint64_t low = static_cast<uint64_t>(value) & ((uint64_t(1) << bits) - 1);
  if (type->isUnsigned()) {
    return static_cast<int64_t>(low);
  }
  uint64_t sign = uint64_t(1) << (bits - 1);

  return static_cast<int64_t>((low ^ sign) - sign);
}

bool ConstantFolder::isFitting(const Type *type, int64_t value) const {
  if (value < type->intMin()) {
    return false;
  }

  return value < 0 || static_cast<uint64_t>(value) <= type->intMax();
}

std::optional<int64_t> ConstantFolder::checkedProduct(std::optional<int64_t> left,
                                                      std::optional<int64_t> right) const {
  if (!left.has_value() || !right.has_value()) {
    return std::nullopt;
  }

  return llvm::checkedMul(*left, *right);
}

ConstantFolder::IntPower ConstantFolder::intPower(int64_t base, int64_t exponent) const {
  uint64_t wrapped = 1;
  auto factor = static_cast<uint64_t>(base);
  std::optional<int64_t> exact = 1;
  std::optional<int64_t> exactFactor = base;
  for (int64_t rest = exponent; rest > 0; rest >>= 1) {
    if ((rest & 1) != 0) {
      wrapped *= factor;
      exact = this->checkedProduct(exact, exactFactor);
    }
    factor *= factor;
    if (rest > 1) {
      exactFactor = this->checkedProduct(exactFactor, exactFactor);
    }
  }

  return IntPower{.wrapped = static_cast<int64_t>(wrapped), .exact = exact};
}

llvm::APInt ConstantFolder::wideResult(BinaryOp op, const llvm::APInt &left,
                                       const llvm::APInt &right) const {
  if (op == BinaryOp::Add) {
    return left + right;
  }
  if (op == BinaryOp::Sub) {
    return left - right;
  }

  return left * right;
}

std::string ConstantFolder::wideText(int64_t left, int64_t right, BinaryOp op) const {
  llvm::APInt wideLeft(kWideBits, static_cast<uint64_t>(left), true);
  llvm::APInt wideRight(kWideBits, static_cast<uint64_t>(right), true);
  auto wide = this->wideResult(op, wideLeft, wideRight);

  return this->decimalText(wide);
}

std::string ConstantFolder::decimalText(const llvm::APInt &wide) const {
  auto digits = llvm::SmallString<48>();
  wide.toString(digits, 10, true);

  return std::string(digits);
}

std::string ConstantFolder::constantText(std::optional<int64_t> exact, double value) const {
  if (exact.has_value()) {
    return std::to_string(*exact);
  }
  if (std::fabs(value) >= kNoExactBelow) {
    return this->formatNumber(value);
  }
  char text[32];
  std::snprintf(text, sizeof text, "%.0f", value);

  return text;
}

std::optional<ConstantValue> ConstantFolder::evalConstant(const Expr *expr, const EnumDecl *self,
                                                          size_t defined) {
  auto answers = std::vector<std::optional<ConstantValue>>();
  for (const Expr *node : ChildrenFirst::order(expr, this, &ConstantFolder::constantParts)) {
    auto value = this->nodeConstant(node, self, defined, answers);
    answers.push_back(std::move(value));
  }

  return answers.back();
}

NodeChildren<const Expr *> ConstantFolder::constantParts(const Expr *expr) const {
  auto *unary = dyn_cast<UnaryExpr>(expr);
  auto *binary = dyn_cast<BinaryExpr>(expr);
  if (unary != nullptr) {
    return {unary->operand};
  }
  if (binary != nullptr) {
    return {binary->lhs, binary->rhs};
  }

  return {};
}

std::vector<EnumDecl *> ConstantFolder::enumsNeeded(const Expr *init, const EnumDecl *self) const {
  auto needed = std::vector<EnumDecl *>();
  if (init == nullptr) {
    return needed;
  }
  for (const Expr *part : ExprParts::operatorTree(init)) {
    auto *other = this->enumNamedBy(part);
    if (other != nullptr && other != self) {
      needed.push_back(other);
    }
  }

  return needed;
}

EnumDecl *ConstantFolder::enumNamedBy(const Expr *expr) const {
  auto *access = dyn_cast<MemberExpr>(expr);
  if (access == nullptr) {
    return nullptr;
  }
  auto *object = dyn_cast<NameExpr>(access->object);
  if (object == nullptr) {
    return nullptr;
  }

  return this->scopes->lookup(object->name).enumDecl();
}

ConstantValue ConstantFolder::numberValue(double number) const {
  return ConstantValue{.isString = false, .number = number, .text = u""};
}

ConstantValue ConstantFolder::textValue(std::u16string text) const {
  return ConstantValue{.isString = true, .number = 0, .text = std::move(text)};
}

ConstantValue ConstantFolder::memberValue(const EnumMember &member) const {
  if (member.isString) {
    return this->textValue(member.text);
  }

  return this->numberValue(static_cast<double>(member.number));
}

std::optional<ConstantValue>
ConstantFolder::nodeConstant(const Expr *expr, const EnumDecl *self, size_t defined,
                             std::vector<std::optional<ConstantValue>> &answers) {
  auto *unary = dyn_cast<UnaryExpr>(expr);
  auto *binary = dyn_cast<BinaryExpr>(expr);
  if (unary != nullptr) {
    auto operand = ChildrenFirst::takeAnswer(answers);

    return this->unaryConstant(unary->op, operand);
  }
  if (binary != nullptr) {
    auto right = ChildrenFirst::takeAnswer(answers);
    auto left = ChildrenFirst::takeAnswer(answers);

    return this->binaryConstant(binary->op, left, right);
  }

  return this->leafConstant(expr, self, defined);
}

std::optional<ConstantValue> ConstantFolder::leafConstant(const Expr *expr, const EnumDecl *self,
                                                          size_t defined) const {
  auto *intLiteral = dyn_cast<IntLitExpr>(expr);
  auto *floatLiteral = dyn_cast<FloatLitExpr>(expr);
  auto *stringLiteral = dyn_cast<StringLitExpr>(expr);
  auto *templateExpr = dyn_cast<TemplateExpr>(expr);
  auto *name = dyn_cast<NameExpr>(expr);
  auto *member = dyn_cast<MemberExpr>(expr);
  if (intLiteral != nullptr) {
    return this->numberValue(static_cast<double>(intLiteral->value));
  }
  if (floatLiteral != nullptr) {
    return this->numberValue(floatLiteral->value);
  }
  if (stringLiteral != nullptr) {
    return this->textValue(stringLiteral->value);
  }
  if (templateExpr != nullptr && templateExpr->exprs.empty()) {
    return this->textValue(templateExpr->quasis[0]);
  }
  if (name != nullptr) {
    return this->ownMemberConstant(self, defined, name->name);
  }
  if (member != nullptr) {
    return this->enumMemberConstant(member, self, defined);
  }

  return std::nullopt;
}

std::optional<ConstantValue> ConstantFolder::ownMemberConstant(const EnumDecl *self, size_t defined,
                                                               llvm::StringRef name) const {
  auto earlier = llvm::ArrayRef<EnumMember>(self->members).take_front(defined);
  for (const EnumMember &member : earlier) {
    if (member.name == name) {
      return this->memberValue(member);
    }
  }

  return std::nullopt;
}

std::optional<ConstantValue> ConstantFolder::enumMemberConstant(const MemberExpr *member,
                                                                const EnumDecl *self,
                                                                size_t defined) const {
  const EnumDecl *enumDecl = this->enumNamedBy(member);
  if (enumDecl == nullptr) {
    return std::nullopt;
  }
  if (enumDecl == self) {
    return this->ownMemberConstant(self, defined, member->name);
  }
  const EnumMember *found = enumDecl->find(member->name);
  if (found == nullptr) {
    return std::nullopt;
  }

  return this->memberValue(*found);
}

std::optional<ConstantValue>
ConstantFolder::unaryConstant(UnaryOp op, const std::optional<ConstantValue> &operand) const {
  if (!operand.has_value() || operand->isString) {
    return std::nullopt;
  }
  double number = operand->number;
  switch (op) {
    case UnaryOp::Neg:
      return this->numberValue(-number);
    case UnaryOp::Plus:
      return operand;
    case UnaryOp::BitNot:
      return this->numberValue(~this->jsToInt32(number));
    case UnaryOp::Not:
    case UnaryOp::TypeOf:
    case UnaryOp::Void:
    case UnaryOp::Delete:
    case UnaryOp::NonNull:
      return std::nullopt;
  }

  return std::nullopt;
}

std::optional<ConstantValue>
ConstantFolder::binaryConstant(BinaryOp op, const std::optional<ConstantValue> &left,
                               const std::optional<ConstantValue> &right) const {
  if (!left.has_value() || !right.has_value()) {
    return std::nullopt;
  }
  if (left->isString || right->isString) {
    return this->joinedText(op, *left, *right);
  }

  return this->numberResult(op, left->number, right->number);
}

std::optional<ConstantValue> ConstantFolder::joinedText(BinaryOp op, const ConstantValue &left,
                                                        const ConstantValue &right) const {
  if (op != BinaryOp::Add || !left.isString || !right.isString) {
    return std::nullopt;
  }

  return this->textValue(left.text + right.text);
}

std::optional<ConstantValue> ConstantFolder::numberResult(BinaryOp op, double left,
                                                          double right) const {
  switch (op) {
    case BinaryOp::Add:
      return this->numberValue(left + right);
    case BinaryOp::Sub:
      return this->numberValue(left - right);
    case BinaryOp::Mul:
      return this->numberValue(left * right);
    case BinaryOp::Div:
      return this->numberValue(left / right);
    case BinaryOp::Rem:
      return this->numberValue(std::fmod(left, right));
    case BinaryOp::Pow:
      return this->numberValue(this->power(left, right));
    case BinaryOp::BitAnd:
    case BinaryOp::BitOr:
    case BinaryOp::BitXor:
    case BinaryOp::Shl:
    case BinaryOp::Shr:
    case BinaryOp::UShr:
      return this->int32Result(op, this->jsToInt32(left), this->jsToInt32(right));
    case BinaryOp::Lt:
    case BinaryOp::Le:
    case BinaryOp::Gt:
    case BinaryOp::Ge:
    case BinaryOp::Eq:
    case BinaryOp::Ne:
    case BinaryOp::LogAnd:
    case BinaryOp::LogOr:
    case BinaryOp::Nullish:
    case BinaryOp::In:
    case BinaryOp::InstanceOf:
      return std::nullopt;
  }

  return std::nullopt;
}

ConstantValue ConstantFolder::int32Result(BinaryOp op, int32_t left, int32_t right) const {
  int shift = right & kShiftMask;
  if (op == BinaryOp::BitAnd) {
    return this->numberValue(left & right);
  }
  if (op == BinaryOp::BitOr) {
    return this->numberValue(left | right);
  }
  if (op == BinaryOp::BitXor) {
    return this->numberValue(left ^ right);
  }
  if (op == BinaryOp::Shl) {
    uint32_t moved = static_cast<uint32_t>(left) << shift;

    return this->numberValue(static_cast<int32_t>(moved));
  }
  if (op == BinaryOp::Shr) {
    return this->numberValue(left >> shift);
  }
  uint32_t moved = static_cast<uint32_t>(left) >> shift;

  return this->numberValue(moved);
}

std::optional<int64_t> ConstantFolder::intConstant(const Expr *expr) const {
  auto casts = std::vector<const CastExpr *>();
  const Expr *inner = expr;
  auto *conversion = dyn_cast<CastExpr>(inner);
  while (conversion != nullptr) {
    bool isInteger = conversion->type != nullptr && conversion->type->isInteger();
    if (!isInteger || !conversion->isImplicit()) {
      return std::nullopt;
    }
    casts.push_back(conversion);
    inner = conversion->operand;
    conversion = dyn_cast<CastExpr>(inner);
  }
  auto value = this->plainIntConstant(inner);
  if (!value.has_value()) {
    return std::nullopt;
  }
  for (const CastExpr *outer : casts) {
    if (!this->isFitting(outer->type, *value)) {
      return std::nullopt;
    }
  }

  return value;
}

std::optional<int64_t> ConstantFolder::plainIntConstant(const Expr *expr) const {
  if (expr->type == nullptr || !expr->type->isInteger()) {
    return std::nullopt;
  }
  auto *literal = dyn_cast<IntLitExpr>(expr);
  auto *name = dyn_cast<NameExpr>(expr);
  if (literal != nullptr) {
    return this->literalConstant(literal);
  }
  if (name != nullptr) {
    return this->state.getConstantStore().findConstVar(name->var());
  }

  return this->state.getConstantStore().findIntConstant(expr);
}

std::optional<int64_t> ConstantFolder::literalConstant(const IntLitExpr *literal) const {
  uint64_t value = literal->value;
  if (value > uint64_t(std::numeric_limits<int64_t>::max())) {
    return std::nullopt;
  }
  auto signedValue = static_cast<int64_t>(value);
  if (!this->isFitting(literal->type, signedValue)) {
    return std::nullopt;
  }

  return signedValue;
}

std::optional<uint64_t> ConstantFolder::nonNegative(std::optional<int64_t> value) const {
  if (!value.has_value() || *value < 0) {
    return std::nullopt;
  }

  return uint64_t(*value);
}

bool ConstantFolder::isUnsigned64(const Type *type) const {
  return type != nullptr && type->isUnsigned() && type->bitWidth() == 64;
}

std::optional<uint64_t> ConstantFolder::unsignedConstant(const Expr *expr) const {
  if (!this->isUnsigned64(expr->type)) {
    auto value = this->intConstant(expr);

    return this->nonNegative(value);
  }
  auto *literal = dyn_cast<IntLitExpr>(expr);
  auto *name = dyn_cast<NameExpr>(expr);
  auto &constants = this->state.getConstantStore();
  if (literal != nullptr) {
    return literal->value;
  }
  std::optional<uint64_t> bigVar =
      name != nullptr ? constants.findBigUnsignedVar(name->var()) : std::nullopt;
  if (bigVar.has_value()) {
    return bigVar;
  }
  auto big = constants.findBigUnsigned(expr);
  if (big.has_value()) {
    return big;
  }
  auto value = this->intConstant(expr);

  return this->nonNegative(value);
}

void ConstantFolder::noteUnsignedVar(const VarSymbol *variable, const Expr *init) {
  if (!this->isUnsigned64(variable->type)) {
    return;
  }
  auto value = this->unsignedConstant(init);
  if (value.has_value() && *value > uint64_t(std::numeric_limits<int64_t>::max())) {
    this->state.getConstantStore().rememberBigUnsignedVar(variable, *value);
  }
}

void ConstantFolder::foldUnary(UnaryExpr *unary) {
  if (unary->type->isWrapping()) {
    return;
  }
  auto *literal = dyn_cast<IntLitExpr>(unary->operand);
  if (literal != nullptr && unary->op == UnaryOp::Neg) {
    return this->foldNegativeLiteral(unary, literal);
  }
  auto value = this->intConstant(unary->operand);
  if (!value.has_value()) {
    return;
  }
  switch (unary->op) {
    case UnaryOp::Plus:
      return this->state.getConstantStore().rememberIntConstant(unary, *value);
    case UnaryOp::BitNot:
      return this->foldBitNot(unary, *value);
    case UnaryOp::Neg:
      return this->foldNegation(unary, *value);
    case UnaryOp::Not:
    case UnaryOp::TypeOf:
    case UnaryOp::Void:
    case UnaryOp::Delete:
    case UnaryOp::NonNull:
      return;
  }
}

void ConstantFolder::foldNegativeLiteral(UnaryExpr *unary, const IntLitExpr *literal) {
  uint64_t magnitude = literal->value;
  if (magnitude > uint64_t(1) << 63) {
    return;
  }
  auto negative = static_cast<int64_t>(0 - magnitude);
  if (this->isFitting(unary->type, negative)) {
    this->state.getConstantStore().rememberIntConstant(unary, negative);
  }
}

void ConstantFolder::foldBitNot(UnaryExpr *unary, int64_t value) {
  const Type *type = unary->type;
  int64_t wrapped = this->wrapTo(type, value);
  int64_t inverted = this->wrapTo(type, ~wrapped);
  this->state.getConstantStore().rememberIntConstant(unary, inverted);
}

void ConstantFolder::foldNegation(UnaryExpr *unary, int64_t value) {
  const Type *type = unary->type;
  std::optional<int64_t> exact = llvm::checkedSub(int64_t(0), value);
  if (exact.has_value() && this->isFitting(type, *exact)) {
    this->state.getConstantStore().rememberIntConstant(unary, *exact);
    return;
  }
  std::string shown =
      exact.has_value() ? std::to_string(*exact) : this->wideText(0, value, BinaryOp::Sub);
  auto negated = static_cast<int64_t>(0 - static_cast<uint64_t>(value));
  int64_t wrapped = this->wrapTo(type, negated);
  auto message = this->overflowText(type, shown, wrapped);
  this->state.getDiagnostics().error(unary->loc(), message);
}

void ConstantFolder::foldBinary(BinaryExpr *binary) {
  const Type *type = binary->type;
  if (type->isWrapping() || binary->lhs->type->isWrapping()) {
    return;
  }
  if (this->isUnsigned64(type)) {
    return this->foldUnsigned64(binary);
  }
  auto left = this->intConstant(binary->lhs);
  auto right = this->intConstant(binary->rhs);
  if (!left.has_value() || !right.has_value()) {
    return;
  }
  auto folded = this->foldIntOp(binary, *left, *right);
  if (folded.has_value()) {
    this->rememberFolded(binary, *folded);
  }
}

void ConstantFolder::foldUnsigned64(BinaryExpr *binary) {
  auto left = this->unsignedConstant(binary->lhs);
  auto right = this->unsignedConstant(binary->rhs);
  if (!left.has_value() || !right.has_value()) {
    return;
  }
  bool isSupported =
      binary->op == BinaryOp::Add || binary->op == BinaryOp::Sub || binary->op == BinaryOp::Mul;
  if (!isSupported) {
    return;
  }
  auto result = this->unsignedResult(binary->op, *left, *right);
  if (!result.has_value()) {
    return this->reportUnsignedOverflow(binary, *left, *right);
  }
  if (*result <= uint64_t(std::numeric_limits<int64_t>::max())) {
    this->state.getConstantStore().rememberIntConstant(binary, static_cast<int64_t>(*result));
    return;
  }
  this->state.getConstantStore().rememberBigUnsigned(binary, *result);
}

void ConstantFolder::reportUnsignedOverflow(const BinaryExpr *binary, uint64_t left,
                                            uint64_t right) {
  llvm::APInt wideLeft(kWideBits, left);
  llvm::APInt wideRight(kWideBits, right);
  auto wide = this->wideResult(binary->op, wideLeft, wideRight);
  auto shown = this->decimalText(wide);
  auto message = this->overflowText(binary->type, shown, 0);
  this->state.getDiagnostics().error(binary->loc(), message);
}

std::optional<uint64_t> ConstantFolder::unsignedResult(BinaryOp op, uint64_t left,
                                                       uint64_t right) const {
  if (op == BinaryOp::Add) {
    return llvm::checkedAddUnsigned(left, right);
  }
  if (op == BinaryOp::Sub && left < right) {
    return std::nullopt;
  }
  if (op == BinaryOp::Sub) {
    return left - right;
  }

  return llvm::checkedMulUnsigned(left, right);
}

std::optional<ConstantFolder::IntFold> ConstantFolder::foldIntOp(BinaryExpr *binary, int64_t left,
                                                                 int64_t right) {
  switch (binary->op) {
    case BinaryOp::Add:
    case BinaryOp::Sub:
    case BinaryOp::Mul:
      return this->foldArithmetic(binary->op, left, right);
    case BinaryOp::Div:
    case BinaryOp::Rem:
      return this->foldDivision(binary->op, binary->type, left, right);
    case BinaryOp::Pow:
      return this->foldPower(left, right);
    case BinaryOp::BitAnd:
    case BinaryOp::BitOr:
    case BinaryOp::BitXor:
      return this->foldBits(binary->op, binary->type, left, right);
    case BinaryOp::Shl:
    case BinaryOp::Shr:
    case BinaryOp::UShr:
      return this->foldShift(binary, left, right);
    case BinaryOp::Lt:
    case BinaryOp::Le:
    case BinaryOp::Gt:
    case BinaryOp::Ge:
    case BinaryOp::Eq:
    case BinaryOp::Ne:
    case BinaryOp::LogAnd:
    case BinaryOp::LogOr:
    case BinaryOp::Nullish:
    case BinaryOp::In:
    case BinaryOp::InstanceOf:
      return std::nullopt;
  }

  return std::nullopt;
}

ConstantFolder::IntFold ConstantFolder::exactFold(int64_t value) const {
  return IntFold{.exact = value, .wrapped = 0, .value = 0, .text = ""};
}

ConstantFolder::IntFold ConstantFolder::foldArithmetic(BinaryOp op, int64_t left,
                                                       int64_t right) const {
  auto unsignedLeft = static_cast<uint64_t>(left);
  auto unsignedRight = static_cast<uint64_t>(right);
  auto text = this->wideText(left, right, op);
  if (op == BinaryOp::Add) {
    return IntFold{
        .exact = llvm::checkedAdd(left, right),
        .wrapped = static_cast<int64_t>(unsignedLeft + unsignedRight),
        .value = 0,
        .text = text,
    };
  }
  if (op == BinaryOp::Sub) {
    return IntFold{
        .exact = llvm::checkedSub(left, right),
        .wrapped = static_cast<int64_t>(unsignedLeft - unsignedRight),
        .value = 0,
        .text = text,
    };
  }

  return IntFold{
      .exact = llvm::checkedMul(left, right),
      .wrapped = static_cast<int64_t>(unsignedLeft * unsignedRight),
      .value = 0,
      .text = text,
  };
}

std::optional<ConstantFolder::IntFold>
ConstantFolder::foldDivision(BinaryOp op, const Type *type, int64_t left, int64_t right) const {
  if (right == 0) {
    return std::nullopt;
  }
  if (right == -1 && !type->isUnsigned()) {
    return this->divisionByMinusOne(op, type, left);
  }
  int64_t quotient = op == BinaryOp::Rem ? left % right : left / right;

  return IntFold{
      .exact = quotient,
      .wrapped = quotient,
      .value = 0,
      .text = "",
  };
}

ConstantFolder::IntFold ConstantFolder::divisionByMinusOne(BinaryOp op, const Type *type,
                                                           int64_t left) const {
  auto text = this->wideText(0, left, BinaryOp::Sub);
  if (op == BinaryOp::Rem) {
    return IntFold{
        .exact = 0,
        .wrapped = 0,
        .value = 0,
        .text = text,
    };
  }
  int64_t min = type->intMin();
  int64_t wrapped = left == min ? min : -left;

  return IntFold{
      .exact = llvm::checkedSub(int64_t(0), left),
      .wrapped = wrapped,
      .value = 0,
      .text = text,
  };
}

std::optional<ConstantFolder::IntFold> ConstantFolder::foldPower(int64_t base,
                                                                 int64_t exponent) const {
  if (exponent < 0) {
    return std::nullopt;
  }
  auto product = this->intPower(base, exponent);
  double value = std::pow(static_cast<double>(base), static_cast<double>(exponent));

  return IntFold{
      .exact = product.exact,
      .wrapped = product.wrapped,
      .value = value,
      .text = "",
  };
}

ConstantFolder::IntFold ConstantFolder::foldBits(BinaryOp op, const Type *type, int64_t left,
                                                 int64_t right) const {
  int64_t wrappedLeft = this->wrapTo(type, left);
  int64_t wrappedRight = this->wrapTo(type, right);
  if (op == BinaryOp::BitAnd) {
    return this->exactFold(wrappedLeft & wrappedRight);
  }
  if (op == BinaryOp::BitOr) {
    return this->exactFold(wrappedLeft | wrappedRight);
  }

  return this->exactFold(wrappedLeft ^ wrappedRight);
}

uint64_t ConstantFolder::bitPattern(int64_t value, int width) const {
  auto bits = static_cast<uint64_t>(value);
  if (width == 64) {
    return bits;
  }

  return bits & ((uint64_t(1) << width) - 1);
}

std::optional<ConstantFolder::IntFold> ConstantFolder::foldShift(BinaryExpr *binary, int64_t value,
                                                                 int64_t count) {
  const Type *shifted = binary->lhs->type;
  int width = int(shifted->bitWidth());
  auto counted = this->shiftCount(shifted, count);
  if (counted.isErr()) {
    this->state.getDiagnostics().error(binary->rhs->loc(), counted.error().message);
  }
  if (count < 0 || count >= int64_t(width)) {
    return std::nullopt;
  }
  int bits = int(count);
  int64_t wrapped = this->wrapTo(shifted, value);
  uint64_t pattern = this->bitPattern(wrapped, width);
  if (binary->op == BinaryOp::Shl) {
    return this->foldShiftLeft(binary, value, bits, pattern);
  }
  if (binary->op == BinaryOp::Shr && !shifted->isUnsigned()) {
    return this->exactFold(wrapped >> bits);
  }
  uint64_t moved = pattern >> bits;
  if (moved > uint64_t(std::numeric_limits<int64_t>::max())) {
    return std::nullopt;
  }

  return this->exactFold(static_cast<int64_t>(moved));
}

std::optional<ConstantFolder::IntFold>
ConstantFolder::foldShiftLeft(BinaryExpr *binary, int64_t value, int count, uint64_t pattern) {
  const Type *shifted = binary->lhs->type;
  int width = int(shifted->bitWidth());
  llvm::APInt start(kWideBits, uint64_t(value), !shifted->isUnsigned());
  llvm::APInt wide = start.shl(unsigned(count));
  int bitsUsed = shifted->isUnsigned() ? int(wide.getActiveBits()) : int(wide.getSignificantBits());
  if (bitsUsed > width && !shifted->isWrapping()) {
    auto shown = this->decimalText(wide);
    auto message = this->overflowText(binary->type, shown, 0);
    this->state.getDiagnostics().error(binary->loc(), message);

    return std::nullopt;
  }
  auto moved = static_cast<int64_t>(pattern << count);

  return this->exactFold(this->wrapTo(shifted, moved));
}

void ConstantFolder::rememberFolded(BinaryExpr *binary, const IntFold &fold) {
  const Type *type = binary->type;
  if (fold.exact.has_value() && this->isFitting(type, *fold.exact)) {
    this->state.getConstantStore().rememberIntConstant(binary, *fold.exact);
    return;
  }
  std::string shown = fold.text.empty() ? this->constantText(fold.exact, fold.value) : fold.text;
  int64_t wrappedValue = this->wrapTo(type, fold.wrapped);
  auto message = this->overflowText(type, shown, wrappedValue);
  this->state.getDiagnostics().error(binary->loc(), message);
}

ConstantFolder::ResultDivisorNotZero ConstantFolder::checkDivisor(BinaryOp op,
                                                                  const Expr *divisor) const {
  bool isDividing = op == BinaryOp::Div || op == BinaryOp::Rem;
  if (isDividing && this->intConstant(divisor) == 0) {
    auto message = ErrorTextDivisor::zero();

    return err(ConstantFolderCheckDivisorError{
        .code = ConstantFolderCheckDivisorErrorCode::DIVISION_BY_ZERO,
        .message = message,
    });
  }

  return ok(divisor);
}

ConstantFolder::ResultShiftCountFits ConstantFolder::shiftCount(const Type *shifted,
                                                                int64_t count) const {
  int width = int(shifted->bitWidth());
  bool isOutside = count < 0 || count >= int64_t(width);
  if (isOutside && !shifted->isWrapping()) {
    auto message = ErrorTextShiftCount::outside(count, width - 1, shifted->name());

    return err(ConstantFolderShiftCountError{
        .code = ConstantFolderShiftCountErrorCode::OUTSIDE_BITS,
        .message = message,
    });
  }

  return ok(shifted);
}

std::string ConstantFolder::overflowText(const Type *type, const std::string &value,
                                         int64_t wrapped) const {
  if (type->bitWidth() < 64) {
    return ErrorTextOverflow::inInt(value, type->name(), wrapped);
  }

  return ErrorTextOverflow::inDouble(value, type->name(), wrapped);
}

Expr *ConstantFolder::globalConstant(GlobalName object, llvm::StringRef name, llvm::SMLoc loc) {
  auto value = GlobalConstantTable::find(object, name);
  if (!value.has_value()) {
    return nullptr;
  }
  auto *literal = this->state.getAst().make<FloatLitExpr>(loc, *value);
  literal->type = Type::doubleTy();

  return literal;
}
}
