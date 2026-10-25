// ECDSA P-256 signatures with SHA-256, 50000 times;
// they are random, so the last is verified.
package main

import (
	"crypto/ecdsa"
	"crypto/rand"
	"crypto/sha256"
	"crypto/x509"
	"encoding/pem"
	"fmt"
)

const key = `-----BEGIN PRIVATE KEY-----
MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQgGw0mYuW3oxSvt0EY
nJvKXzdThD45siaRIzWojBykQX+hRANCAARnoIUMMjzuG6gDJ4WTCCf2YVcEmfeY
Ksmom19AoDLv4h4DMQ7+K0TawHM1PV+PDPnLHU+LD5M3fXSFdGZMKru3
-----END PRIVATE KEY-----
`

func main() {
	block, _ := pem.Decode([]byte(key))
	parsed, err := x509.ParsePKCS8PrivateKey(block.Bytes)
	if err != nil {
		panic(err)
	}
	priv := parsed.(*ecdsa.PrivateKey)
	data := []byte("a message of some length to sign, as a token or a handshake would")
	var signature []byte
	for i := 0; i < 50000; i++ {
		digest := sha256.Sum256(data)
		signature, _ = ecdsa.SignASN1(rand.Reader, priv, digest[:])
	}
	digest := sha256.Sum256(data)
	fmt.Println(ecdsa.VerifyASN1(&priv.PublicKey, digest[:], signature))
}
