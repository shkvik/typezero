#include "compiler/semantic/library/stream/set_encoding_checker/set_encoding_checker.h"
#include "compiler/program/ast_walker/ast_walker.h"
#include "compiler/program/library/builtin_names/library_class_table/library_class_table.h"
#include "compiler/program/library/builtin_names/library_method_table/library_method_table.h"
#include "compiler/program/library/events/emitter_member_table/emitter_member_table.h"
#include "compiler/program/library/process/process_function_table/process_function_table.h"
#include "compiler/program/library/stream/stream_method_table/stream_method_table.h"
#include "compiler/semantic/bodies/expr/argument_checker/argument_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/declare/class_declarer/class_declarer.h"
#include "compiler/semantic/flow/field_facts/field_facts.h"
#include "compiler/semantic/generics/generic_class_instantiator/generic_class_instantiator.h"
#include "compiler/semantic/library/events/emitter_checker/emitter_checker.h"
#include "compiler/semantic/library/process/process_checker/process_checker.h"
#include "compiler/semantic/library/stream/chunk_types/chunk_types.h"
#include "compiler/semantic/library/stream/stream_pipeline/stream_pipeline.h"
#include "compiler/semantic/library/stream/stream_sources/stream_sources.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "llvm/ADT/STLExtras.h"

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

SetEncodingChecker::SetEncodingChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->chunkTypes = &all.chunkTypes;
  this->fieldFacts = &all.fieldFacts;
  this->streamSources = &all.streamSources;
  this->emitterChecker = &all.emitterChecker;
  this->processChecker = &all.processChecker;
  this->genericClassInstantiator = &all.genericClassInstantiator;
}

const Type *SetEncodingChecker::encodedStreamType(ClassDecl *classDecl, llvm::SMLoc loc) {
  for (ClassDecl *current = classDecl; current != nullptr; current = current->parent) {
    auto args = this->encodedArgs(current);
    if (!args.empty()) {
      return this->instantiateEncoded(current->generic(), args, loc);
    }
  }

  return nullptr;
}

std::vector<const Type *> SetEncodingChecker::encodedArgs(const ClassDecl *classDecl) const {
  if (!classDecl->library() || classDecl->generic() == nullptr || classDecl->typeArgs.empty()) {
    return {};
  }
  llvm::StringRef name = classDecl->generic()->name;
  if (LibraryClassTable::isName(name, LibraryClass::READABLE)) {
    return {Type::string()};
  }
  bool isDuplex = LibraryClassTable::isName(name, LibraryClass::DUPLEX) ||
                  LibraryClassTable::isName(name, LibraryClass::TRANSFORM) ||
                  LibraryClassTable::isName(name, LibraryClass::PASS_THROUGH);
  if (isDuplex) {
    return {classDecl->typeArgs[0], Type::string()};
  }

  return {};
}

const Type *SetEncodingChecker::instantiateEncoded(ClassDecl *generic,
                                                   const std::vector<const Type *> &args,
                                                   llvm::SMLoc loc) {
  auto made = this->genericClassInstantiator->instantiateClass(generic, args, loc);
  if (made.isErr() &&
      made.error().code != GenericClassInstantiatorInstantiateClassErrorCode::ALREADY_REPORTED) {
    llvm::SMLoc at =
        made.error().code == GenericClassInstantiatorInstantiateClassErrorCode::ENDLESS_INSTANCES
            ? generic->loc()
            : (loc.isValid() ? loc : generic->loc());
    this->state.getDiagnostics().error(at, made.error().message);
  }

  return made.isOk() ? made.value()->type : nullptr;
}

SetEncodingChecker::ResultDecodedChunks
SetEncodingChecker::checkDecodedChunks(const Type *readable, const Type *receiver) const {
  if (readable == nullptr) {
    auto message = ErrorTextSetEncoding::unreadable(receiver->name());

    return err(SetEncodingCheckerDecodedChunksError{
        .code = SetEncodingCheckerDecodedChunksErrorCode::NOT_READABLE,
        .message = message,
    });
  }
  if (!readable->isString() && !readable->isBuffer()) {
    auto message = ErrorTextSetEncoding::objectMode(readable->name());

    return err(SetEncodingCheckerDecodedChunksError{
        .code = SetEncodingCheckerDecodedChunksErrorCode::OBJECT_MODE,
        .message = message,
    });
  }

  return ok(readable);
}

SetEncodingChecker::ResultStdinIsPlaced SetEncodingChecker::checkStdinPlace() const {
  const auto *module = this->state.getModuleStates().getCurrentModule();
  bool isTopLevel = module != nullptr && module->isEntry &&
                    !this->state.getFunctionStack().isInsideFunction() &&
                    this->state.getScopeStack().isTopLevel();
  if (!isTopLevel) {
    auto message = ErrorTextSetEncoding::stdinPlace();

    return err(SetEncodingCheckerStdinPlaceError{
        .code = SetEncodingCheckerStdinPlaceErrorCode::NOT_TOP_LEVEL,
        .message = message,
    });
  }

  return ok(module);
}

Expr *SetEncodingChecker::checkSetEncoding(CallExpr *call, MemberExpr *member,
                                           ClassDecl *classDecl) {
  const Type *receiver = member->object->type;
  auto chunks = this->chunkTypes->streamChunksOf(classDecl);
  llvm::SMLoc loc = member->loc();
  auto decoded = this->checkDecodedChunks(chunks.readable, receiver);
  if (decoded.isErr()) {
    this->state.getDiagnostics().error(loc, decoded.error().message);
    return call;
  }
  if (chunks.readable->isString()) {
    return this->plainSetEncoding(call, member, classDecl, receiver);
  }
  const auto *object = this->withoutImplicit(member->object);
  bool isStdinStream = this->isStdinCall(object);
  if (isStdinStream) {
    auto placed = this->checkStdinPlace();
    if (placed.isErr()) {
      this->state.getDiagnostics().error(loc, placed.error().message);
      return call;
    }
  }
  auto path = this->encodingPath(object, isStdinStream);
  auto why = this->encodingPathProblem(path, isStdinStream);
  if (why.has_value()) {
    auto message = ErrorTextSetEncoding::changes(*why);
    this->state.getDiagnostics().error(loc, message);
    return call;
  }
  EncodingRun run{
      .member = member,
      .stdinStream = isStdinStream,
      .path = *path,
      .here = nullptr,
      .start = nullptr,
      .escapes = {},
      .safe = {},
  };
  this->markEscapeWindow(run);
  this->collectEscapes(run);
  if (!run.escapes.empty()) {
    this->reportFirstEscape(run, loc);
    return call;
  }
  const auto *view = this->encodedStreamType(classDecl, loc);
  this->plainSetEncoding(call, member, classDecl, receiver);
  if (view == nullptr || call->type->isError()) {
    return call;
  }
  call->type = view;
  llvm::SMLoc here = llvm::SMLoc::getFromPointer(run.here);
  this->state.getStreamStore().rememberStreamView(run.path, here, view);

  return call;
}

Expr *SetEncodingChecker::plainSetEncoding(CallExpr *call, MemberExpr *member, ClassDecl *classDecl,
                                           const Type *receiver) {
  auto setEncoding = LibraryMethodTable::getName(LibraryMethod::SET_ENCODING);
  this->emitterChecker->callLibraryMethod(call, member, classDecl, setEncoding, {}, nullptr,
                                          CallReceiver::OWN);
  if (!call->type->isError()) {
    call->type = receiver;
  }

  return call;
}

const Expr *SetEncodingChecker::withoutImplicit(const Expr *expr) const {
  const Expr *bare = expr;
  auto *conversion = dyn_cast<CastExpr>(bare);
  while (conversion != nullptr && conversion->isImplicit()) {
    bare = conversion->operand;
    conversion = dyn_cast<CastExpr>(bare);
  }

  return bare;
}

bool SetEncodingChecker::isStdinCall(const Expr *expr) const {
  auto *call = dyn_cast<CallExpr>(expr);

  auto stdinName = ProcessFunctionTable::getName(ProcessFunction::STDIN);

  return call != nullptr && call->func() != nullptr && call->func()->name == stdinName;
}

std::optional<FieldPath> SetEncodingChecker::encodingPath(const Expr *object, bool stdinStream) {
  if (stdinStream) {
    FieldPath stdinPath;
    stdinPath.var = this->streamSources->processStdinVar();
    return stdinPath;
  }
  auto *name = dyn_cast<NameExpr>(object);
  if (name != nullptr && name->var() != nullptr) {
    FieldPath namePath;
    namePath.var = name->var();
    return namePath;
  }

  return this->fieldFacts->fieldPath(object);
}

std::optional<std::string>
SetEncodingChecker::encodingPathProblem(const std::optional<FieldPath> &path,
                                        bool stdinStream) const {
  if (!path.has_value() || path->var == nullptr) {
    return ErrorTextEncodingWhy::noConstant();
  }
  const VarSymbol *root = path->var;
  if (!root->isConst) {
    return ErrorTextEncodingWhy::notConst(root->name);
  }
  auto field = this->writableField(*path);
  if (field.has_value()) {
    return ErrorTextEncodingWhy::fieldSet(*field);
  }
  if (!this->isOwnBlock(root, stdinStream)) {
    return ErrorTextEncodingWhy::ownBlock(root->name);
  }

  return std::nullopt;
}

std::optional<llvm::StringRef> SetEncodingChecker::writableField(const FieldPath &path) const {
  const Type *current = path.var->type;
  for (llvm::StringRef field : path.names) {
    const Field *found = current != nullptr ? current->nonOptional()->field(field) : nullptr;
    if (found == nullptr || !found->readonly) {
      return field;
    }
    current = found->type;
  }

  return std::nullopt;
}

bool SetEncodingChecker::isOwnBlock(const VarSymbol *root, bool stdinStream) const {
  auto &scopes = this->state.getScopeStack();
  size_t declared = stdinStream ? 0 : scopes.getScopeDepth();
  if (!stdinStream) {
    auto found = scopes.findDeclaringScope(root);
    if (found.has_value()) {
      declared = *found;
    }
  }
  bool isBranched = stdinStream ? scopes.isInsideBranch() : scopes.isBranchedSince(declared);

  return declared + 1 == scopes.getScopeDepth() && !isBranched;
}

void SetEncodingChecker::markEscapeWindow(EncodingRun &run) const {
  const Expr *base = run.member->object;
  for (;;) {
    auto *conversion = dyn_cast<CastExpr>(base);
    auto *member = dyn_cast<MemberExpr>(base);
    if (conversion != nullptr) {
      base = conversion->operand;
    } else if (member != nullptr) {
      base = member->object;
    } else {
      break;
    }
  }
  run.here = base->loc().getPointer();
  run.start = run.stdinStream ? nullptr : run.path.var->loc.getPointer();
}

bool SetEncodingChecker::isBefore(const EncodingRun &run, const Expr *expr) const {
  const char *at = expr->loc().getPointer();

  return at > run.start && at < run.here;
}

bool SetEncodingChecker::isEncodingPath(const EncodingRun &run, const Expr *expr) const {
  const auto *bare = this->withoutImplicit(expr);
  if (run.stdinStream) {
    if (isa<CallExpr>(bare)) {
      return this->isStdinCall(bare);
    }
    return this->processChecker->isProcessMember(bare, ProcessMember::STDIN);
  }
  const VarSymbol *root = run.path.var;
  if (run.path.names.empty()) {
    auto *name = dyn_cast<NameExpr>(bare);
    return name != nullptr &&
           (name->var() == root || (name->var() == nullptr && name->name == root->name));
  }
  auto path = this->fieldFacts->fieldPath(bare);

  return path.has_value() && *path == run.path;
}

void SetEncodingChecker::collectEscapes(EncodingRun &run) {
  auto body = this->encodingBody(run);
  for (const Stmt *stmt : body) {
    for (const Expr *expr : AstWalker::allExprsIn(stmt)) {
      this->classifyUse(run, expr);
    }
  }
  auto direct = llvm::SmallPtrSet<const Expr *, 32>();
  for (const Stmt *stmt : body) {
    auto exprs = AstWalker::ownExprsIn(stmt);
    direct.insert(exprs.begin(), exprs.end());
  }
  for (const Stmt *stmt : body) {
    for (const Expr *expr : AstWalker::allExprsIn(stmt)) {
      this->noteEscape(run, expr, direct);
    }
  }
}

std::vector<const Stmt *> SetEncodingChecker::encodingBody(const EncodingRun &run) const {
  auto body = std::vector<const Stmt *>();
  FunctionDecl *owner = run.stdinStream ? nullptr : run.path.var->owner();
  if (owner != nullptr && owner->body != nullptr) {
    body.push_back(owner->body);
    return body;
  }
  auto *module = this->state.getModuleStates().getCurrentModule();
  if (module != nullptr) {
    for (const Stmt *item : module->file.items) {
      body.push_back(item);
    }
  }

  return body;
}

void SetEncodingChecker::classifyUse(EncodingRun &run, const Expr *expr) const {
  if (!this->isBefore(run, expr)) {
    return;
  }
  auto *call = dyn_cast<CallExpr>(expr);
  if (call != nullptr) {
    auto *member = dyn_cast<MemberExpr>(call->callee);
    if (member == nullptr || !this->isEncodingPath(run, member->object)) {
      return;
    }
    if (this->isListeningMethod(member->name)) {
      run.escapes.push_back({member->loc(), ErrorTextEncodingUse::listeners(member->name)});
    } else {
      run.safe.insert(this->withoutImplicit(member->object));
    }
    return;
  }
  auto *member = dyn_cast<MemberExpr>(expr);
  if (member != nullptr && this->isEncodingPath(run, member->object)) {
    run.safe.insert(this->withoutImplicit(member->object));
  }
}

void SetEncodingChecker::noteEscape(EncodingRun &run, const Expr *expr,
                                    const llvm::SmallPtrSetImpl<const Expr *> &direct) const {
  bool isCandidate =
      this->isBefore(run, expr) && !isa<CastExpr>(expr) && this->isEncodingPath(run, expr);
  if (!isCandidate) {
    return;
  }
  if (!direct.contains(expr)) {
    run.escapes.push_back({expr->loc(), ErrorTextEncodingUse::function()});
  } else if (!run.safe.contains(expr)) {
    run.escapes.push_back({expr->loc(), ErrorTextEncodingUse::elsewhere()});
  }
}

void SetEncodingChecker::reportFirstEscape(const EncodingRun &run, llvm::SMLoc loc) {
  const StreamEscape *first = &run.escapes[0];
  for (const StreamEscape &escape : run.escapes) {
    if (escape.loc.getPointer() < first->loc.getPointer()) {
      first = &escape;
    }
  }
  auto message = ErrorTextSetEncoding::usedBefore(first->how);
  auto usedHere = ErrorTextSetEncoding::usedHere();
  this->state.getDiagnostics().error(loc, message);
  this->state.getDiagnostics().note(first->loc, usedHere);
}

bool SetEncodingChecker::isListeningMethod(llvm::StringRef name) const {
  auto member = EmitterMemberTable::find(name);
  bool isListen = member == EmitterMember::ON || member == EmitterMember::ONCE ||
                  member == EmitterMember::ADD_LISTENER ||
                  member == EmitterMember::PREPEND_LISTENER ||
                  member == EmitterMember::PREPEND_ONCE_LISTENER;

  return isListen || StreamMethodTable::find(name) == StreamMethod::PIPE;
}

}
