#pragma once

#include "compiler/semantic/semantic_state/v2/library/bytes_context/bytes_context.h"
#include "compiler/semantic/semantic_state/v2/library/event_context/event_context.h"
#include "compiler/semantic/semantic_state/v2/library/file_system_context/file_system_context.h"
#include "compiler/semantic/semantic_state/v2/library/library_declaration_context/library_declaration_context.h"
#include "compiler/semantic/semantic_state/v2/library/reflection_context/reflection_context.h"
#include "compiler/semantic/semantic_state/v2/library/stream_context/stream_context.h"

namespace tz {

struct LibraryStorage {
  BytesContext bytes;
  EventContext events;
  StreamContext streams;
  FileSystemContext filesystem;
  ReflectionContext reflection;
  LibraryDeclarationContext declarations;
};

}
