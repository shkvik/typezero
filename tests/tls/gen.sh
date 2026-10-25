#!/bin/sh
# Makes the certificates of the TLS tests (tests/run/tls_*.ts), which are checked in next to this
# script: run it here, `sh gen.sh`, with the openssl command line of OpenSSL 3 (the one of Git for
# Windows will do). Every key is P-256, so that each certificate is about 600 bytes; the dates are
# fixed, so the certificates differ from run to run only in their keys and signatures.
#
#   ca.pem         the CA the tests trust, through NODE_EXTRA_CA_CERTS
#   key.pem        the key of every server certificate below
#   localhost.pem  localhost, 127.0.0.1 and ::1, by ca.pem, valid until 2126
#   expired.pem    the same names, by ca.pem, valid in 2000 only
#   future.pem     the same names, by ca.pem, valid from 2100
#   wrongname.pem  example.org only, by ca.pem
#   self.pem       localhost, signed by its own key, as `openssl req -x509` makes one
#   other.pem      localhost, by a CA the tests do not trust (whose certificate is not kept)
#   chain.pem      localhost, by an intermediate CA of that CA, then the intermediate
#   rooted.pem     other.pem, then the CA that signed it
set -e
cd "$(dirname "$0")"
# The shell of Git for Windows would take "/CN=..." for a path and rewrite it.
export MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*'
# Relative paths, which need no rewriting either.
work=.work
rm -rf "$work"
mkdir "$work"
trap 'rm -rf "$work"' EXIT

key() { openssl genpkey -algorithm EC -pkeyopt ec_paramgen_curve:P-256 -out "$1" 2>/dev/null; }

cat > "$work/ca.cnf" <<EOF
[ca]
default_ca = test
[test]
database = $work/index.txt
new_certs_dir = $work
serial = $work/serial
default_md = sha256
policy = any
unique_subject = no
[any]
commonName = supplied
[root]
basicConstraints = critical, CA:TRUE
keyUsage = critical, keyCertSign, cRLSign
subjectKeyIdentifier = hash
[intermediate]
basicConstraints = critical, CA:TRUE, pathlen:0
keyUsage = critical, keyCertSign, cRLSign
subjectKeyIdentifier = hash
authorityKeyIdentifier = keyid
[localhost]
basicConstraints = CA:FALSE
keyUsage = critical, digitalSignature
extendedKeyUsage = serverAuth
subjectAltName = DNS:localhost, IP:127.0.0.1, IP:::1
authorityKeyIdentifier = keyid
[wrongname]
basicConstraints = CA:FALSE
keyUsage = critical, digitalSignature
extendedKeyUsage = serverAuth
subjectAltName = DNS:example.org
authorityKeyIdentifier = keyid
EOF

# sign NAME CN ISSUER-CERT ISSUER-KEY EXTENSIONS FROM TO: NAME.pem for the key NAME.key.
sign() {
  : > "$work/index.txt"
  echo 1000 > "$work/serial"
  openssl req -new -key "$work/$1.key" -subj "/CN=$2" -out "$work/$1.csr"
  if [ "$3" = self ]; then
    openssl ca -batch -notext -selfsign -config "$work/ca.cnf" -keyfile "$work/$1.key" \
      -in "$work/$1.csr" -out "$work/$1.pem" -extensions "$5" -startdate "$6" -enddate "$7" \
      2>/dev/null
  else
    openssl ca -batch -notext -config "$work/ca.cnf" -cert "$3" -keyfile "$4" \
      -in "$work/$1.csr" -out "$work/$1.pem" -extensions "$5" -startdate "$6" -enddate "$7" \
      2>/dev/null
  fi
}

key "$work/ca.key"
key "$work/other_ca.key"
key "$work/intermediate.key"
key key.pem
for leaf in localhost expired future wrongname other chain; do cp key.pem "$work/$leaf.key"; done

sign ca "tz test CA" self - root 20260101000000Z 21260101000000Z
sign other_ca "untrusted CA" self - root 20260101000000Z 21260101000000Z
sign intermediate "untrusted intermediate CA" "$work/other_ca.pem" "$work/other_ca.key" \
  intermediate 20260101000000Z 21260101000000Z
sign localhost localhost "$work/ca.pem" "$work/ca.key" localhost 20260101000000Z 21260101000000Z
sign expired localhost "$work/ca.pem" "$work/ca.key" localhost 20000101000000Z 20010101000000Z
sign future localhost "$work/ca.pem" "$work/ca.key" localhost 21000101000000Z 22000101000000Z
sign wrongname example.org "$work/ca.pem" "$work/ca.key" wrongname 20260101000000Z \
  21260101000000Z
sign other localhost "$work/other_ca.pem" "$work/other_ca.key" localhost 20260101000000Z \
  21260101000000Z
sign chain localhost "$work/intermediate.pem" "$work/intermediate.key" localhost \
  20260101000000Z 21260101000000Z

openssl req -x509 -key key.pem -subj "/CN=localhost" -days 36500 \
  -addext "subjectAltName = DNS:localhost, IP:127.0.0.1" -out self.pem
for name in ca localhost expired future wrongname other; do cp "$work/$name.pem" "$name.pem"; done
cat "$work/chain.pem" "$work/intermediate.pem" > chain.pem
cat "$work/other.pem" "$work/other_ca.pem" > rooted.pem
