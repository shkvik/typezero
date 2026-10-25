# Downloads over HTTP (WinHTTP on Windows): the mirror of jsr.cmake, served by
# a server on 127.0.0.1 that serve.mjs runs under Node.js.
include(${SOURCE}/../cli.cmake)
if(NOT NODE)
  skip("Node.js is not installed")
endif()

jsr_publish(@std/fmt 1.2.0 jsr/fmt-1.2.0)
jsr_publish(@tz/greet 2.0.0 jsr/greet-2.0.0)

tz(init app)
write(app/main.tz [[
import { greet } from "greet"
console.log(greet("http"))
]])
tz(add greet IN app SERVE jsr STDOUT "added greet: jsr:@tz/greet@^2.0.0 (2.0.0)\n")
tz(run IN app STDOUT "<red>hello, http</red>\n")
# 404: the registry has no such package.
tz(add nosuch IN app SERVE jsr EXIT 1 STDERR
   "tz: 'nosuch' has not been adapted for tz yet: there is no jsr:@tz/nosuch in the registry")

# No server at all.
set(ENV{TZ_JSR_URL} "http://127.0.0.1:9/")
tz(add other IN app EXIT 1 STDERR "tz: cannot download http://127.0.0.1:9/@tz/other/meta.json: ")
