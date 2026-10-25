// same-as-node
// URL of the WHATWG URL Standard, a global as in node: parsing (special
// and other schemes, hosts as names, IPv4 in any of its forms and IPv6,
// paths with . and .., percent-encoding), relative URLs against a base,
// canParse and parse, the setters, and how a URL prints.
const inputs = [
  "https://user:pw@example.com:8080/a/b?x=1&y=2#frag",
  "http://EXAMPLE.com:80/./a/../b/%2e%2E/c",
  "http://[::1]:3000/x",
  "http://[2001:db8:0:0:1:0:0:1]/",
  "http://[::ffff:192.168.0.1]/",
  "http://0x7f.1/",
  "http://0300.0250.0.1/",
  "file:///C:/Users/x/../y.txt",
  "file://localhost/etc/hosts",
  "file:c:\a\b",
  "mailto:someone@example.com",
  "data:text/plain,hello world",
  "http://example.com/a b?c d#e f",
  "http://example.com/путь?запрос=значение#фрагмент",
  "http://пример.рф/",
  "http://ex%41mple.com/",
  "HTTP://example.COM",
  "http://a/b\c\d",
  "http://example.com:/x",
  "ws://h:80/",
  "wss://h:443/",
  "http://h/?a='b'&c=\"d\"",
  "non-spec://h/a b",
  "blob:https://a.com/uuid",
  "about:blank",
  "http://h/#a#b",
  "foo://",
]
for (const s of inputs) {
  const u = new URL(s)
  console.log(u.href, "|", u.origin, "|", u.protocol, u.username, u.password, u.host, u.hostname,
              u.port, u.pathname, u.search, u.hash)
}
for (const s of ["http://h:65536/", "http://[::1/", "http://a b/", "http:///", "https://h:x/",
                 "http://192.168.0.257/", "nope"])
  console.log(s, URL.canParse(s), URL.parse(s) == undefined)
const base = "http://base.com/dir/file?q#f"
for (const r of ["", "?x", "#y", "/abs", "../up", "//other.com/p", "./a/b", "http:foo"])
  console.log(JSON.stringify(r), new URL(r, base).href)
console.log(new URL("c", new URL(base)).href, URL.canParse("/x", "http://h"))
try {
  new URL("not a url")
} catch (e) {
  console.log(e.name, e.message)
}

// The setters.
const u = new URL("http://h/p?a=1#top")
u.pathname = "/new path"
u.hash = "end"
u.port = "8080"
u.hostname = "other.org"
u.username = "me"
u.password = "s3cret"
console.log(u.href)
u.protocol = "https"
u.port = "443"
console.log(u.href, u.port == "")
u.host = "example.com:9000"
u.search = "q=1 2"
console.log(u.href)
u.href = "http://reset.com/"
console.log(u.href, u.toString(), u.toJSON(), JSON.stringify({ u: u.toJSON() }))
console.log(new URL("https://user:pw@example.com:8080/a/b?x=1&y=2#frag"))
