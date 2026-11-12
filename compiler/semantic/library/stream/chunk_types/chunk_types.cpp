#include "compiler/semantic/library/stream/chunk_types/chunk_types.h"
#include "compiler/program/library/builtin_names/library_class_table/library_class_table.h"
#include "compiler/program/library/stream/stream_hook_table/stream_hook_table.h"
#include "compiler/program/library/stream/stream_kind_table/stream_kind_table.h"
#include "compiler/semantic/bodies/classes/class_bases/class_bases.h"
#include "compiler/semantic/declare/prelude/prelude.h"
#include "compiler/semantic/generics/alias_instantiator/alias_instantiator.h"
#include "compiler/semantic/generics/generic_class_instantiator/generic_class_instantiator.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

ChunkTypes::ChunkTypes(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->prelude = &all.prelude;
  this->classBases = &all.classBases;
  this->aliasInstantiator = &all.aliasInstantiator;
  this->genericClassInstantiator = &all.genericClassInstantiator;
}

bool ChunkTypes::isStreamClass(const ClassDecl *classDecl) const {
  auto *stream = this->state.getLibraryDeclarations().findLibraryClass(LibraryClass::STREAM);

  return classDecl != nullptr && stream != nullptr && classDecl->derivesFrom(stream);
}

StreamChunks ChunkTypes::streamChunksOf(const ClassDecl *classDecl) const {
  StreamChunks chunks;
  for (const ClassDecl *current = classDecl; current != nullptr; current = current->parent) {
    if (this->isTypedLibraryStream(current)) {
      this->addStreamChunk(chunks, current);
    }
  }

  return chunks;
}

bool ChunkTypes::isTypedLibraryStream(const ClassDecl *classDecl) const {
  return classDecl->library() && classDecl->generic() != nullptr && !classDecl->typeArgs.empty();
}

void ChunkTypes::addStreamChunk(StreamChunks &chunks, const ClassDecl *classDecl) const {
  llvm::StringRef name = classDecl->generic()->name;
  bool isReadable = LibraryClassTable::isName(name, LibraryClass::READABLE);
  bool isWritable = LibraryClassTable::isName(name, LibraryClass::WRITABLE);
  bool isDuplex = LibraryClassTable::isName(name, LibraryClass::DUPLEX);
  if (isReadable && chunks.readable == nullptr) {
    chunks.readable = classDecl->typeArgs[0];
  } else if (isWritable && chunks.writable == nullptr) {
    chunks.writable = classDecl->typeArgs[0];
  } else if (isDuplex && classDecl->typeArgs.size() >= 2 && chunks.writable == nullptr) {
    chunks.writable = classDecl->typeArgs[0];
  }
}

void ChunkTypes::addStreamBases(ClassDecl *instance, llvm::SMLoc loc) {
  ClassDecl *generic = instance->generic();
  auto *writable = this->state.getLibraryDeclarations().findLibraryClass(LibraryClass::WRITABLE);
  bool isDuplex =
      generic != nullptr && LibraryClassTable::isName(generic->name, LibraryClass::DUPLEX);
  if (!isDuplex || !generic->library() || writable == nullptr || instance->typeArgs.size() != 3) {
    return;
  }
  auto side = this->genericClassInstantiator->instantiateClass(
      writable, {instance->typeArgs[0], instance->typeArgs[2]}, loc);
  if (side.isErr() &&
      side.error().code != GenericClassInstantiatorInstantiateClassErrorCode::ALREADY_REPORTED) {
    llvm::SMLoc at =
        side.error().code == GenericClassInstantiatorInstantiateClassErrorCode::ENDLESS_INSTANCES
            ? writable->loc()
            : (loc.isValid() ? loc : writable->loc());
    this->state.getDiagnostics().error(at, side.error().message);
  }
  if (side.isOk()) {
    this->classBases->addBase(instance->type, side.value()->type);
    if (instance->isEmitted()) {
      this->state.getInventoryStore().addSubtype(instance->type);
    }
  }
}

std::optional<std::vector<const Type *>>
ChunkTypes::streamHookParams(const FunctionDecl *functionDecl) {
  if (!this->isStreamHook(functionDecl)) {
    return std::nullopt;
  }
  const auto *lib = this->libraryBase(functionDecl->cls()->parent);
  std::optional<StreamSides> sides = lib != nullptr ? this->streamSides(lib) : std::nullopt;
  if (!sides.has_value()) {
    return std::nullopt;
  }
  auto params = this->hookParamTypes(functionDecl, *sides);
  if (params.empty() || llvm::is_contained(params, nullptr)) {
    return std::nullopt;
  }

  return params;
}

const ClassDecl *ChunkTypes::libraryBase(const ClassDecl *classDecl) const {
  const ClassDecl *lib = classDecl;
  while (lib != nullptr && !lib->library()) {
    lib = lib->parent;
  }
  if (lib == nullptr || lib->generic() == nullptr) {
    return nullptr;
  }

  return lib;
}

std::optional<ChunkTypes::StreamSides> ChunkTypes::streamSides(const ClassDecl *lib) {
  const auto *streamError = this->prelude->builtinType(PreludeType::STREAM_ERROR);
  auto kind = StreamKindTable::find(lib->generic()->name);
  if (!kind.has_value()) {
    return std::nullopt;
  }
  const std::vector<const Type *> &args = lib->typeArgs;
  switch (*kind) {
    case StreamKind::READABLE:
      if (args.size() != 2) {
        return std::nullopt;
      }
      return StreamSides{
          .readable = args[0],
          .writable = nullptr,
          .error = args[1],
          .transform = false,
      };
    case StreamKind::WRITABLE:
      if (args.size() != 2) {
        return std::nullopt;
      }
      return StreamSides{
          .readable = nullptr,
          .writable = args[0],
          .error = args[1],
          .transform = false,
      };
    case StreamKind::DUPLEX:
    case StreamKind::TRANSFORM:
      if (args.size() != 3) {
        return std::nullopt;
      }
      return StreamSides{
          .readable = args[1],
          .writable = args[0],
          .error = args[2],
          .transform = *kind == StreamKind::TRANSFORM,
      };
    case StreamKind::PASS_THROUGH:
      if (args.size() != 2) {
        return std::nullopt;
      }
      return StreamSides{
          .readable = args[1],
          .writable = args[0],
          .error = streamError,
          .transform = true,
      };
  }

  return std::nullopt;
}

std::vector<const Type *> ChunkTypes::hookParamTypes(const FunctionDecl *functionDecl,
                                                     const StreamSides &sides) {
  const auto *callback =
      this->libraryAliasOf(LibraryType::HOOK_CALLBACK, sides.error, functionDecl->loc());
  const auto *transformCallback =
      this->libraryAliasOf(LibraryType::TRANSFORM_HOOK_CALLBACK, sides.error, functionDecl->loc());
  auto hook = StreamHookTable::find(functionDecl->name);
  if (!hook.has_value()) {
    return {};
  }
  switch (*hook) {
    case StreamHook::ON_READ:
      if (sides.readable == nullptr) {
        return {};
      }
      return {Type::intTy()};
    case StreamHook::ON_FINAL:
    case StreamHook::ON_CONSTRUCT:
      return {callback};
    case StreamHook::ON_DESTROY:
      return this->destroyParamTypes();
    case StreamHook::ON_FLUSH:
      if (!sides.transform) {
        return {};
      }
      return {transformCallback};
    case StreamHook::ON_TRANSFORM:
      if (!sides.transform) {
        return {};
      }
      return {sides.writable, Type::string(), transformCallback};
    case StreamHook::ON_WRITE:
      if (sides.writable == nullptr) {
        return {};
      }
      return {sides.writable, Type::string(), callback};
    case StreamHook::ON_WRITEV:
      return this->writevParamTypes(functionDecl, sides, callback);
  }

  return {};
}

std::vector<const Type *> ChunkTypes::destroyParamTypes() {
  const auto *error = this->prelude->builtinType(PreludeType::STREAM_ERROR);
  auto *aliasDecl =
      this->state.getLibraryDeclarations().findLibraryType(LibraryType::STREAM_CALLBACK);
  const Type *callback = aliasDecl != nullptr ? aliasDecl->resolved : nullptr;

  return {Type::optional(error), callback};
}

std::vector<const Type *> ChunkTypes::writevParamTypes(const FunctionDecl *functionDecl,
                                                       const StreamSides &sides,
                                                       const Type *callback) {
  if (sides.writable == nullptr) {
    return {};
  }
  const auto *chunk =
      this->libraryAliasOf(LibraryType::WRITE_CHUNK, sides.writable, functionDecl->loc());
  if (chunk == nullptr) {
    return {};
  }

  return {Type::array(chunk), callback};
}

const FuncSymbol *ChunkTypes::libraryHook(llvm::ArrayRef<FunctionDecl *> inherited) const {
  for (const FunctionDecl *method : llvm::reverse(inherited)) {
    if (method->sym != nullptr && method->cls() != nullptr && method->cls()->library()) {
      return method->sym;
    }
  }

  return nullptr;
}

bool ChunkTypes::isUntypedParam(const Type *type) const {
  return type == nullptr || type->isError();
}

const Type *ChunkTypes::libraryAliasOf(LibraryType libraryType, const Type *arg, llvm::SMLoc loc) {
  auto *aliasDecl = this->state.getLibraryDeclarations().findLibraryType(libraryType);
  if (aliasDecl == nullptr) {
    return nullptr;
  }

  return this->aliasInstantiator->instantiateAlias(aliasDecl, {arg}, loc);
}

bool ChunkTypes::isStreamHook(const FunctionDecl *functionDecl) const {
  const ClassDecl *classDecl = functionDecl->cls();
  bool isUserMethod = classDecl != nullptr && !functionDecl->isStatic() && !classDecl->library() &&
                      classDecl->literal() == nullptr && classDecl->parent != nullptr;
  auto hook = StreamHookTable::find(functionDecl->name);

  return isUserMethod && hook.has_value();
}

bool ChunkTypes::isLinkedStreamHook(const FunctionDecl *functionDecl) const {
  return this->isStreamHook(functionDecl) && this->isStreamClass(functionDecl->cls()->parent);
}

ChunkTypes::ResultHookParamsMatch
ChunkTypes::checkHookParams(const FunctionDecl *functionDecl,
                            llvm::ArrayRef<FunctionDecl *> inherited) const {
  const auto *hook = this->libraryHook(inherited);
  if (hook == nullptr) {
    return err(ChunkTypesHookParamsError{
        .code = ChunkTypesHookParamsErrorCode::NO_LIBRARY_HOOK,
        .message = "",
    });
  }
  auto isUntyped = std::bind_front(&ChunkTypes::isUntypedParam, this);
  if (llvm::any_of(functionDecl->sym->params, isUntyped)) {
    return err(ChunkTypesHookParamsError{
        .code = ChunkTypesHookParamsErrorCode::PARAMS_NOT_TYPED,
        .message = "",
    });
  }
  bool hasChunks = functionDecl->name == "onWrite" || functionDecl->name == "onWritev" ||
                   functionDecl->name == "onTransform";
  auto params = ErrorTextStreamHook::params(hook);
  auto message = ErrorTextStreamHook::other(functionDecl->name, params, hasChunks);

  return err(ChunkTypesHookParamsError{
      .code = ChunkTypesHookParamsErrorCode::OTHER_PARAMETERS,
      .message = message,
  });
}

}
