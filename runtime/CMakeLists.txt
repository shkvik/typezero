# The runtime is on every program's hot path: optimize it fully even in
# RelWithDebInfo, which otherwise inlines only functions marked inline.
string(REPLACE "/Ob1" "/Ob2" CMAKE_C_FLAGS_RELWITHDEBINFO "${CMAKE_C_FLAGS_RELWITHDEBINFO}")

add_library(tzrt STATIC alloc.c tzrt.c string.c array.c inspect.c)

set_target_properties(tzrt PROPERTIES ARCHIVE_OUTPUT_DIRECTORY ${TZ_OUTPUT_DIR})

if(MSVC)
  # Debug info inside the objects (/Z7) keeps the library self-contained.
  set_target_properties(tzrt PROPERTIES MSVC_DEBUG_INFORMATION_FORMAT Embedded)
  target_compile_options(tzrt PRIVATE /W4 /utf-8)
  target_compile_definitions(tzrt PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(tzrt PRIVATE -Wall -Wextra)
endif()
