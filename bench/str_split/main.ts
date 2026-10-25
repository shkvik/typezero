// split(): a 21 MB log of 256 thousand lines, cut into lines, into words and
// into the fields between " | ", five times each.
const block = [
  "2026-10-24 09:15:02.114 | INFO  | api      | GET /api/users/1842 200 in 12 ms",
  "2026-10-24 09:15:02.131 | DEBUG | db       | select id, name, email from users where id = 1842",
  "2026-10-24 09:15:02.250 | WARN  | cache    | miss for key user:1842:profile, loading from the database",
  "2026-10-24 09:15:03.007 | INFO  | api      | POST /api/orders 201 in 48 ms",
  "2026-10-24 09:15:03.019 | ERROR | payments | card declined for order 99172: insufficient funds",
  "2026-10-24 09:15:03.544 | INFO  | worker   | processed 128 jobs, 3 failed, 0 retried",
  "2026-10-24 09:15:04.001 | DEBUG | http     | keep-alive connection 7 reused",
  "2026-10-24 09:15:04.290 | INFO  | api      | GET /api/products?page=2&size=50 200 in 7 ms",
  "2026-10-24 09:15:05.712 | WARN  | api      | slow request GET /api/reports/annual took 1840 ms",
  "2026-10-24 09:15:05.713 | INFO  | auth     | user 1842 signed in from 10.0.4.17",
  "2026-10-24 09:15:06.020 | ERROR | db       | deadlock detected, transaction 5521 rolled back",
  "2026-10-24 09:15:06.101 | INFO  | worker   | job 77 done in 230 ms",
  "2026-10-24 09:15:07.333 | DEBUG | cache    | evicted 12 keys, 4096 left",
  "2026-10-24 09:15:08.004 | INFO  | api      | DELETE /api/sessions/af31 204 in 3 ms",
  "2026-10-24 09:15:08.950 | WARN  | disk     | volume /data is 91% full",
  "2026-10-24 09:15:09.415 | INFO  | api      | GET /health 200 in 1 ms",
].join("\n") + "\n"
const text = block.repeat(16000)

let lines = 0
let words = 0
let fields = 0
for (let r = 0; r < 5; r++) {
  lines += text.split("\n").length
  words += text.split(" ").length
  fields += text.split(" | ").length
}
console.log(lines, words, fields)
