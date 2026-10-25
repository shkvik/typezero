// What tz:http, fetch and URL check at compile time: the types of the
// options and of the handler, the properties of URL that cannot be
// assigned (or only with '='), and the intrinsics that only tz's own
// library has.
import { serve } from "tz:http"
// expect-error: type 'string' is not assignable to type 'int | undefined' of field 'port'
serve({ port: "80" }, req => new Response("x"))
// expect-error: parameter 'req' is declared as 'string', but the function gets 'Request' here
serve({ port: 80 }, (req: string) => new Response("x"))
// expect-error: type 'string' is not assignable to type 'Response | Promise<Response>'
serve({ port: 80 }, req => "text")
// expect-error: cannot find name '__tz_net_close'
const close = __tz_net_close
const u = new URL("http://x")
// expect-error: cannot assign to 'origin' because it is a read-only property of 'URL'
u.origin = "y"
// expect-error: 'href' is a property, not a method: read it without '()'
u.href()
// expect-error: 'port' of 'URL' is assigned only with '=': write 'o.port = o.port + value'
u.port += "1"
