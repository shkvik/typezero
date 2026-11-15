#include "compiler/semantic/library_v2/async/library_async.h"
#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

namespace {

constexpr llvm::StringLiteral kPromisePlaceholder = "p";
constexpr size_t kMaxShownStatement = 60;

}

LibraryAsync::LibraryAsync(SemanticState &owner)
    : state(owner) {}

std::string LibraryAsync::statementText(llvm::SMLoc at) const {
  if (!at.isValid()) {
    return "";
  }
  const char *start = at.getPointer();
  const char *end = start;
  int depth = 0;
  for (; *end != '\0' && *end != '\n' && *end != '\r'; ++end) {
    bool isComment = end[0] == '/' && end[1] == '/';
    if (depth == 0 && (*end == ';' || isComment)) {
      break;
    }
    depth += this->depthChange(*end);
  }
  llvm::StringRef text = llvm::StringRef(start, size_t(end - start)).rtrim();
  if (depth != 0 || text.size() > kMaxShownStatement) {
    return "";
  }

  return text.str();
}

int LibraryAsync::depthChange(char symbol) const {
  if (symbol == '(' || symbol == '[' || symbol == '{') {
    return 1;
  }
  if (symbol == ')' || symbol == ']' || symbol == '}') {
    return -1;
  }

  return 0;
}

const Type *LibraryAsync::awaitedType(const Type *type) const {
  int optionals = 0;
  const Type *inner = type;
  while (inner->isOptional()) {
    optionals += 1;
    inner = inner->inner();
  }
  const auto *awaited = this->awaitedPlain(inner);
  for (int index = 0; index < optionals; ++index) {
    if (awaited == nullptr || awaited->isVoid()) {
      return awaited;
    }
    awaited = Type::optional(awaited);
  }

  return awaited;
}

const Type *LibraryAsync::awaitedPlain(const Type *type) const {
  if (type->isPromise()) {
    return type->promised();
  }
  if (!type->isUnion() || !this->hasPromiseMember(type)) {
    return type;
  }
  const Type *result = nullptr;
  for (const Type *member : type->members()) {
    const Type *candidate = member->isPromise() ? member->promised() : member;
    result =
        result != nullptr ? this->state.types.literals.unionUnify(result, candidate) : candidate;
    if (result == nullptr || result->isError()) {
      return nullptr;
    }
  }

  return result;
}

bool LibraryAsync::hasPromiseMember(const Type *type) const {
  for (const Type *member : type->members()) {
    if (member->isPromise()) {
      return true;
    }
  }

  return false;
}

LibraryAsync::ResultPromiseHandled
LibraryAsync::checkDiscardedPromise(const Expr *expr, bool isCallback, llvm::SMLoc at) const {
  if (isa<AssignExpr>(expr) || this->isTakingElement(expr)) {
    return ok(expr);
  }
  const Type *type = expr->type;
  bool isPromise = type->nonOptional()->isPromise();
  if (!isPromise && this->isMappingCall(expr)) {
    auto message = ErrorTextDiscarded::mapped();

    return err(LibraryAsyncDiscardedPromiseError{
        .code = LibraryAsyncDiscardedPromiseErrorCode::PROMISES_DROPPED,
        .message = message,
    });
  }
  if (!isPromise) {
    return ok(expr);
  }
  if (isCallback) {
    auto message = ErrorTextDiscarded::callback();

    return err(LibraryAsyncDiscardedPromiseError{
        .code = LibraryAsyncDiscardedPromiseErrorCode::CALLBACK_DROPS,
        .message = message,
    });
  }
  auto text = this->statementText(at);
  if (type->isPromise() && type->promised()->holdsResults()) {
    std::string shown = text.empty() ? kPromisePlaceholder.str() : text;
    auto message = ErrorTextDiscarded::result(shown);

    return err(LibraryAsyncDiscardedPromiseError{
        .code = LibraryAsyncDiscardedPromiseErrorCode::RESULT_UNCHECKED,
        .message = message,
    });
  }
  std::string message =
      text.empty() ? ErrorTextDiscarded::plain() : ErrorTextDiscarded::withText(text);

  return err(LibraryAsyncDiscardedPromiseError{
      .code = LibraryAsyncDiscardedPromiseErrorCode::NOT_AWAITED,
      .message = message,
  });
}

bool LibraryAsync::isTakingElement(const Expr *expr) const {
  auto *call = dyn_cast<CallExpr>(expr);
  if (call == nullptr) {
    return false;
  }
  Builtin builtin = call->builtin();

  return builtin == Builtin::ArrPop || builtin == Builtin::ArrShift ||
         builtin == Builtin::ArrSplice;
}

bool LibraryAsync::isMappingCall(const Expr *expr) const {
  auto *call = dyn_cast<CallExpr>(expr);
  if (call == nullptr) {
    return false;
  }
  Builtin builtin = call->builtin();

  return builtin == Builtin::ArrMap || builtin == Builtin::ArrFlatMap;
}

const Type *LibraryAsync::timerCallType(const Expr *init) const {
  auto *call = dyn_cast<CallExpr>(init);
  const Expr *callee = call != nullptr ? call->callee : nullptr;
  if (callee == nullptr) {
    return nullptr;
  }
  if (this->state.isGlobalName(callee, GlobalName::SET_TIMEOUT) ||
      this->state.isGlobalName(callee, GlobalName::SET_INTERVAL)) {
    return Type::timeout();
  }
  if (this->state.isGlobalName(callee, GlobalName::SET_IMMEDIATE)) {
    return Type::immediate();
  }

  return nullptr;
}

}
