add_library(tzrt STATIC tzrt.c)

set_target_properties(tzrt PROPERTIES ARCHIVE_OUTPUT_DIRECTORY ${TZ_OUTPUT_DIR})

if(MSVC)
  # /Z7 keeps debug info inside the objects so the library is self-contained.
  target_compile_options(tzrt PRIVATE /W4 /utf-8 /Z7)
else()
  target_compile_options(tzrt PRIVATE -Wall -Wextra)
endif()
