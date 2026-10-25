// 50 000 X25519 key exchanges between the same keys; the secret is printed.
package main

import (
	"crypto/ecdh"
	"crypto/x509"
	"encoding/hex"
	"encoding/pem"
	"fmt"
)

const privatePem = `-----BEGIN PRIVATE KEY-----
MC4CAQAwBQYDK2VuBCIEILDx6HXrOrgm2PqDbEj9yCzSL0TRCBWhBzmCkJXoAcFU
-----END PRIVATE KEY-----
`
const publicPem = `-----BEGIN PUBLIC KEY-----
MCowBQYDK2VuAyEAUK8OHWoiL3Lxgz3pczoxppdpSzndnKQX85k+QdmW2hk=
-----END PUBLIC KEY-----
`

func main() {
	block, _ := pem.Decode([]byte(privatePem))
	parsed, err := x509.ParsePKCS8PrivateKey(block.Bytes)
	if err != nil {
		panic(err)
	}
	priv := parsed.(*ecdh.PrivateKey)
	block, _ = pem.Decode([]byte(publicPem))
	parsedPublic, err := x509.ParsePKIXPublicKey(block.Bytes)
	if err != nil {
		panic(err)
	}
	pub := parsedPublic.(*ecdh.PublicKey)
	var secret []byte
	for i := 0; i < 50000; i++ {
		secret, _ = priv.ECDH(pub)
	}
	fmt.Println(hex.EncodeToString(secret))
}
