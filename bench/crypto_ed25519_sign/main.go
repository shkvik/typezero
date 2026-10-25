// Ed25519 signatures, 50000 times;
// the SHA-256 of the last signature is printed.
package main

import (
	"crypto/ed25519"
	"crypto/sha256"
	"crypto/x509"
	"encoding/hex"
	"encoding/pem"
	"fmt"
)

const key = `-----BEGIN PRIVATE KEY-----
MC4CAQAwBQYDK2VwBCIEILv4fsSzEiPk7kwDzHM70NuGeJRFZkyyEOjHEB0ttb//
-----END PRIVATE KEY-----
`

func main() {
	block, _ := pem.Decode([]byte(key))
	parsed, err := x509.ParsePKCS8PrivateKey(block.Bytes)
	if err != nil {
		panic(err)
	}
	priv := parsed.(ed25519.PrivateKey)
	data := []byte("a message of some length to sign, as a token or a handshake would")
	var signature []byte
	for i := 0; i < 50000; i++ {
		signature = ed25519.Sign(priv, data)
	}
	sum := sha256.Sum256(signature)
	fmt.Println(hex.EncodeToString(sum[:]))
}
