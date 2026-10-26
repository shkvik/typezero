import { X509Certificate } from "node:crypto";
import { rootCertificates } from "node:tls";
import { deflateRawSync } from "node:zlib";

const certificates = rootCertificates.map((pem) => new X509Certificate(pem));
const der = Buffer.concat(certificates.map((c) => c.raw));
const packed = deflateRawSync(der, { level: 9 });

const lines = [
  `#define TLS_ROOTS_COUNT ${certificates.length}`,
  `#define TLS_ROOTS_SIZE ${der.length}`,
  "",
  `static const unsigned char tls_roots[${packed.length}] = {`,
];
for (let i = 0; i < packed.length; i += 16) {
  const row = [...packed.subarray(i, i + 16)].map((b) => `0x${b.toString(16).padStart(2, "0")},`);
  lines.push(`    ${row.join(" ")}`);
}
lines.push("};", "");
process.stdout.write(lines.join("\n"));
