# OpenSSL for node:crypto (docs/crypto.md) and https (docs/net.md): built from pinned sources by
# build.ps1 when the build tree is configured, once per recipe (the result is kept in
# TZ_DEPS_CACHE, so another build tree copies it). tz links it into a program only when the
# program uses node:crypto or https, from next to tz.exe as it does the runtime:
#
#   libcrypto.lib         all of OpenSSL's libcrypto: the full layer
#   libcrypto_direct.lib  the objects of the algorithms that are called directly
#   libssl.lib            TLS, for https (docs/net.md, section 4), with all of libcrypto
#   licenses/OpenSSL.txt  what `tz licenses` prints for it

set(TZ_DEPS_CACHE "$ENV{LOCALAPPDATA}/typezero/deps" CACHE PATH
    "Where third-party libraries built for tz are kept between build trees")
set(TZ_OPENSSL_DIR ${CMAKE_BINARY_DIR}/openssl)

set(recipe ${CMAKE_CURRENT_LIST_DIR}/build.ps1 ${CMAKE_CURRENT_LIST_DIR}/tz.conf
           ${CMAKE_CURRENT_LIST_DIR}/direct.txt)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${recipe})
find_program(TZ_POWERSHELL powershell REQUIRED)
file(TO_NATIVE_PATH "${TZ_OPENSSL_DIR}" out)
file(TO_NATIVE_PATH "${LLVM_TOOLS_BINARY_DIR}" llvm_bin)
file(TO_NATIVE_PATH "${TZ_DEPS_CACHE}" cache)
execute_process(
  COMMAND ${TZ_POWERSHELL} -NoProfile -ExecutionPolicy Bypass
          -File ${CMAKE_CURRENT_LIST_DIR}/build.ps1 -Out ${out} -LlvmBin ${llvm_bin} -Cache ${cache}
  RESULT_VARIABLE result)
if(result)
  message(FATAL_ERROR "OpenSSL could not be built; see the output of build.ps1 above")
endif()

file(MAKE_DIRECTORY ${TZ_OUTPUT_DIR}/licenses)
configure_file(${TZ_OPENSSL_DIR}/lib/libcrypto.lib ${TZ_OUTPUT_DIR}/libcrypto.lib COPYONLY)
configure_file(${TZ_OPENSSL_DIR}/lib/libcrypto_direct.lib ${TZ_OUTPUT_DIR}/libcrypto_direct.lib
               COPYONLY)
configure_file(${TZ_OPENSSL_DIR}/lib/libssl.lib ${TZ_OUTPUT_DIR}/libssl.lib COPYONLY)
configure_file(${TZ_OPENSSL_DIR}/LICENSE.txt ${TZ_OUTPUT_DIR}/licenses/OpenSSL.txt COPYONLY)
