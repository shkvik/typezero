# The runtime, built by clang of the same LLVM that tz is built with, in two
# forms that land next to tz.exe, where tz looks for them:
#
#   tzrt.bc   LLVM bitcode. tz copies the small functions a program calls
#             from it into the program before optimizing, so that their fast
#             paths are inlined into the program (see linkRuntime in
#             src/driver/driver.cpp).
#   tzrt.lib  the same code compiled, linked into every program: everything
#             that was not inlined is called here.
#
# CMake cannot build C with clang while tz itself is built with MSVC, so both
# come from custom commands.

find_program(TZ_CLANG clang PATHS ${LLVM_TOOLS_BINARY_DIR} NO_DEFAULT_PATH REQUIRED)
find_program(TZ_LLVM_LINK llvm-link PATHS ${LLVM_TOOLS_BINARY_DIR} NO_DEFAULT_PATH REQUIRED)
find_program(TZ_LLVM_LIB llvm-lib PATHS ${LLVM_TOOLS_BINARY_DIR} NO_DEFAULT_PATH REQUIRED)

set(sources alloc.c tzrt.c string.c array.c inspect.c number.c math.c unicode.c closure.c
            process.c object.c decimal.c regex.c regex_compile.c json.c error.c date.c
            date_parse.c date_zone.c date_format.c icu.c intl.c intl_number.c intl_text.c
            intl_names.c path.c fs.c child.c os.c map.c async.c io.c signal.c bytes.c buffer.c
            unknown.c meta.c library.c reflect.c net.c http.c decompress.c group.c cjk.c stream.c)
# What fetch decodes with (decompress.c): zlib's inflate and brotli's
# decoder, built as they come, without their warnings.
set(third_party ${PROJECT_SOURCE_DIR}/third_party)
set(third_party_sources
    zlib/adler32.c zlib/crc32.c zlib/inffast.c zlib/inflate.c zlib/inftrees.c zlib/zutil.c
    brotli/common/constants.c brotli/common/context.c brotli/common/dictionary.c
    brotli/common/platform.c brotli/common/shared_dictionary.c brotli/common/transform.c
    brotli/dec/bit_reader.c brotli/dec/decode.c brotli/dec/huffman.c brotli/dec/state.c)
set(includes -I${third_party}/zlib -I${third_party}/brotli/include)
set(flags -O2 -std=c17 -Wall -Wextra -D_CRT_SECURE_NO_WARNINGS ${includes})
if(WIN32)
  list(APPEND flags -fms-runtime-lib=static) # the static CRT, as tz links it
endif()

# Each file to bitcode. -flto=full stops before the optimizations that only
# pay off once everything is in one module (vectorization, unrolling).
set(dir ${CMAKE_CURRENT_BINARY_DIR}/bitcode)
file(MAKE_DIRECTORY ${dir})
set(bitcode_files)
foreach(source ${sources})
  get_filename_component(name ${source} NAME_WE)
  set(bitcode ${dir}/${name}.bc)
  add_custom_command(OUTPUT ${bitcode}
    COMMAND ${TZ_CLANG} ${flags} -flto=full -c -MD -MF ${bitcode}.d -o ${bitcode}
            ${CMAKE_CURRENT_SOURCE_DIR}/${source}
    DEPENDS ${source}
    DEPFILE ${bitcode}.d
    COMMENT "Compiling ${source} to bitcode"
    VERBATIM)
  list(APPEND bitcode_files ${bitcode})
endforeach()
foreach(source ${third_party_sources})
  string(REPLACE "/" "_" name ${source})
  string(REPLACE ".c" "" name ${name})
  set(bitcode ${dir}/third_party_${name}.bc)
  set(third_party_flags -O2 -std=c17 -w -D_CRT_SECURE_NO_WARNINGS ${includes})
  if(WIN32)
    list(APPEND third_party_flags -fms-runtime-lib=static)
  endif()
  add_custom_command(OUTPUT ${bitcode}
    COMMAND ${TZ_CLANG} ${third_party_flags} -flto=full -c -MD -MF ${bitcode}.d -o ${bitcode}
            ${third_party}/${source}
    DEPENDS ${third_party}/${source}
    DEPFILE ${bitcode}.d
    COMMENT "Compiling ${source} to bitcode"
    VERBATIM)
  list(APPEND bitcode_files ${bitcode})
endforeach()

# One module for the whole runtime: tzrt.bc, and compiled as a whole into
# tzrt.lib, so that the files' functions are inlined into each other too.
# Each function gets its own section, so the linker drops the unused ones.
add_custom_command(OUTPUT ${TZ_OUTPUT_DIR}/tzrt.bc
  COMMAND ${TZ_LLVM_LINK} -o ${TZ_OUTPUT_DIR}/tzrt.bc ${bitcode_files}
  DEPENDS ${bitcode_files}
  COMMENT "Linking the runtime bitcode"
  VERBATIM)
add_custom_command(OUTPUT ${TZ_OUTPUT_DIR}/tzrt.lib
  COMMAND ${TZ_CLANG} -O2 -ffunction-sections -fdata-sections -c -o ${dir}/tzrt.obj
          ${TZ_OUTPUT_DIR}/tzrt.bc
  COMMAND ${TZ_LLVM_LIB} /nologo /out:${TZ_OUTPUT_DIR}/tzrt.lib ${dir}/tzrt.obj
  DEPENDS ${TZ_OUTPUT_DIR}/tzrt.bc
  COMMENT "Compiling the runtime library"
  VERBATIM)

# node:crypto (docs/crypto.md, crypto.h): tzcrypto.lib, the glue between programs and OpenSSL,
# which tz links only into programs that use it. Each part is an object of its own, so that a
# program brings only the algorithms it names: crypto_md.c and crypto_aes.c are compiled once for
# each family of hashes and each mode. Not in tzrt.bc: nothing of it is hot enough to copy into
# programs.
set(crypto_parts
  "crypto crypto.c"
  "crypto_md5 crypto_md.c TZ_MD_MD5"
  "crypto_sha1 crypto_md.c TZ_MD_SHA1"
  "crypto_sha256 crypto_md.c TZ_MD_SHA256"
  "crypto_sha512 crypto_md.c TZ_MD_SHA512"
  "crypto_sha3 crypto_md.c TZ_MD_SHA3"
  "crypto_ripemd160 crypto_md.c TZ_MD_RIPEMD160"
  "crypto_sm3 crypto_md.c TZ_MD_SM3"
  "crypto_md5_sha1 crypto_md.c TZ_MD_MD5_SHA1"
  "crypto_blake2b crypto_md.c TZ_MD_BLAKE2B"
  "crypto_blake2s crypto_md.c TZ_MD_BLAKE2S"
  "crypto_aes_gcm crypto_aes.c TZ_AES_GCM"
  "crypto_aes_cbc crypto_aes.c TZ_AES_CBC"
  "crypto_aes_ctr crypto_aes.c TZ_AES_CTR"
  "crypto_chacha crypto_chacha.c"
  "crypto_scrypt crypto_scrypt.c"
  "crypto_argon2 crypto_argon2.c"
  "crypto_evp crypto_evp.c"
  "crypto_keys crypto_keys.c"
  "crypto_x509 crypto_x509.c"
  "crypto_subtle crypto_subtle.c"
  "crypto_subtle_kw crypto_subtle_kw.c"
  "crypto_subtle_keys crypto_subtle_keys.c")
set(crypto_dir ${CMAKE_CURRENT_BINARY_DIR}/crypto)
file(MAKE_DIRECTORY ${crypto_dir})
set(crypto_objects)
set(crypto_sources)
foreach(part ${crypto_parts})
  separate_arguments(part)
  list(GET part 0 name)
  list(GET part 1 source)
  set(defines)
  list(LENGTH part count)
  if(count GREATER 2)
    list(GET part 2 define)
    set(defines -D${define})
  endif()
  set(object ${crypto_dir}/${name}.obj)
  add_custom_command(OUTPUT ${object}
    COMMAND ${TZ_CLANG} ${flags} ${defines} -ffunction-sections -fdata-sections
            -I${TZ_OPENSSL_DIR}/include -I${TZ_OPENSSL_DIR}/private -c -MD -MF ${object}.d
            -o ${object} ${CMAKE_CURRENT_SOURCE_DIR}/${source}
    DEPENDS ${source} crypto.h
    DEPFILE ${object}.d
    COMMENT "Compiling ${name} of node:crypto"
    VERBATIM)
  list(APPEND crypto_objects ${object})
  list(APPEND crypto_sources ${source})
endforeach()
list(REMOVE_DUPLICATES crypto_sources)
add_custom_command(OUTPUT ${TZ_OUTPUT_DIR}/tzcrypto.lib
  COMMAND ${TZ_LLVM_LIB} /nologo /out:${TZ_OUTPUT_DIR}/tzcrypto.lib ${crypto_objects}
  DEPENDS ${crypto_objects}
  COMMENT "Linking the glue of node:crypto"
  VERBATIM)

add_custom_target(tzrt DEPENDS ${TZ_OUTPUT_DIR}/tzrt.bc ${TZ_OUTPUT_DIR}/tzrt.lib
                               ${TZ_OUTPUT_DIR}/tzcrypto.lib
  SOURCES ${sources} tzrt.h unicode_tables.h gen_unicode_tables.mjs regex.h regex_tables.h
          gen_regex_tables.mjs date.h icu.h intl.h encodings.h gen_encodings.mjs cjk_tables.h
          gen_cjk_tables.mjs
          number_tables.h gen_number_tables.mjs ${crypto_sources} crypto.h net.h)
