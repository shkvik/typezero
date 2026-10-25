# What a program of node:crypto links (docs/crypto.md, sections 3 and 6): a hash named in the code
# brings the direct layer of OpenSSL, tens of KB of it, and a name known only when the program runs
# all of it; `tz licenses` names OpenSSL only in the exe that has it.
#
#   cmake -DTZ=<tz.exe> -DDIR=<scratch directory> -P crypto_link.cmake

file(MAKE_DIRECTORY ${DIR})
file(WRITE ${DIR}/plain.ts "console.log(\"hi\")\n")
file(WRITE ${DIR}/direct.ts
     "import { createHash } from \"node:crypto\"\n"
     "console.log(createHash(\"sha256\").update(\"abc\").digest(\"hex\"))\n")
file(WRITE ${DIR}/full.ts
     "import { createHash } from \"node:crypto\"\n"
     "const name: string = process.argv[2] ?? \"sha256\"\n"
     "console.log(createHash(name).update(\"abc\").digest(\"hex\"))\n")
# Web Crypto (src/lib/webcrypto.ts): HMAC with its algorithm in a constant, as hono signs cookies,
# needs only the direct layer; an ECDSA key, all of OpenSSL.
file(WRITE ${DIR}/subtle_direct.ts
     "const algorithm = { name: \"HMAC\", hash: \"SHA-256\" }\n"
     "const key = await crypto.subtle.importKey(\"raw\", new Uint8Array(8), algorithm, false, [\"sign\"])\n"
     "console.log((await crypto.subtle.sign(algorithm.name, key, new Uint8Array(1))).byteLength)\n")
file(WRITE ${DIR}/subtle_full.ts
     "const pair = await crypto.subtle.generateKey({ name: \"ECDSA\", namedCurve: \"P-256\" }, false, [\"sign\"])\n"
     "console.log(pair.privateKey.type)\n")

function(fail message)
  message(FATAL_ERROR "${message}")
endfunction()

foreach(name plain direct full subtle_direct subtle_full)
  execute_process(COMMAND ${TZ} build -o ${DIR}/${name}.exe ${DIR}/${name}.ts
                  RESULT_VARIABLE result ERROR_VARIABLE errors)
  if(result)
    fail("tz build ${name}.ts failed: ${errors}")
  endif()
  file(SIZE ${DIR}/${name}.exe ${name}_size)
  execute_process(COMMAND ${TZ} licenses ${DIR}/${name}.exe RESULT_VARIABLE result
                  OUTPUT_VARIABLE ${name}_licenses ERROR_VARIABLE errors)
  if(result)
    fail("tz licenses ${name}.exe failed: ${errors}")
  endif()
endforeach()

execute_process(COMMAND ${DIR}/direct.exe OUTPUT_VARIABLE out)
if(NOT out STREQUAL "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\n")
  fail("direct.exe printed '${out}'")
endif()
execute_process(COMMAND ${DIR}/full.exe sha3-256 OUTPUT_VARIABLE out)
if(NOT out STREQUAL "3a985da74fe225b2045c172d6bd390bd855f086e3e9d525b46bfe24511431532\n")
  fail("full.exe printed '${out}'")
endif()

# SHA-256 alone: its assembly and the glue (about 30 KB), and the encodings and errors of the
# runtime the program now uses, under 200 KB more than a program without it.
math(EXPR direct_more "${direct_size} - ${plain_size}")
if(direct_more GREATER 200000)
  fail("SHA-256 named in the code adds ${direct_more} bytes; the direct layer should add tens of KB")
endif()
if(full_size LESS 2000000)
  fail("a hash named by a value is ${full_size} bytes: it should link all of OpenSSL")
endif()

execute_process(COMMAND ${DIR}/subtle_direct.exe OUTPUT_VARIABLE out)
if(NOT out STREQUAL "32\n")
  fail("subtle_direct.exe printed '${out}'")
endif()
if(subtle_direct_size GREATER 1000000)
  fail("HMAC of crypto.subtle is ${subtle_direct_size} bytes: it should take the direct layer")
endif()
if(subtle_full_size LESS 2000000)
  fail("ECDSA of crypto.subtle is ${subtle_full_size} bytes: it should link all of OpenSSL")
endif()

if(plain_licenses MATCHES "OpenSSL")
  fail("tz licenses names OpenSSL in a program without it:\n${plain_licenses}")
endif()
foreach(name direct full subtle_direct subtle_full)
  if(NOT ${name}_licenses MATCHES "OpenSSL [0-9.]+: the cryptography of node:crypto \\(Apache License 2.0\\)")
    fail("tz licenses does not name OpenSSL in ${name}.exe:\n${${name}_licenses}")
  endif()
  if(NOT ${name}_licenses MATCHES "Apache License\n *Version 2.0, January 2004")
    fail("tz licenses does not print the license of OpenSSL for ${name}.exe")
  endif()
endforeach()
if(NOT plain_licenses MATCHES "Node.js: .*\\(MIT\\).*V8: .*\\(BSD-3-Clause\\)")
  fail("tz licenses does not name the parts of the runtime:\n${plain_licenses}")
endif()
message(STATUS "plain ${plain_size}, direct ${direct_size}, full ${full_size}, subtle_direct "
               "${subtle_direct_size}, subtle_full ${subtle_full_size} bytes")
