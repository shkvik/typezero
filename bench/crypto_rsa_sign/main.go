// RSA-2048 signatures with SHA-256 and PKCS#1 v1.5, 3000 times;
// the SHA-256 of the last signature is printed.
package main

import (
	"crypto"
	"crypto/rand"
	"crypto/rsa"
	"crypto/sha256"
	"crypto/x509"
	"encoding/hex"
	"encoding/pem"
	"fmt"
)

const key = `-----BEGIN PRIVATE KEY-----
MIIEvgIBADANBgkqhkiG9w0BAQEFAASCBKgwggSkAgEAAoIBAQCm8Utj1f3Q7yFM
hH8Ui4jvCwxeRlVBnPOHMZsyji70Oq7TtBfLh1l+1hq4HfUCd4sSGmFYvn9np7zr
G0aFheNZeXcporzUtICnWEYeNhumaelBSgomxMW0+OOXzD426gPleNR2fs5uETmg
ZwcM2300C9SSh52G2FmgeKP/pr+2NKDxYaOPCkfsyCJxVIu/yXZWVZWGnX5tekRA
eYDZuTrQX0yixgBRx2zbLAIBtjUyFEGiBjAFAblwZASSEYXgw4g7SOnlO31iJ48g
xLbbcH0WHQXQbXbODA37ZMqCQxeq6ZQDt/Pfk/AYKu+3m2iBIuNDQstc9AB8HPxO
Q3jg07P7AgMBAAECggEAOgVsD99Mb2TlHFyCdGgIqBgdbNiiAgNkKm0lxPzgudZ5
c6/Ig17fznLoZ2xV/s0Ag0Ky6a6Pxj1orgfVS0vc1/ic31YlrgGsIz1NZWul0YFG
5f+vJolGM587LsN4LgrIXuVPZn2tDlkjP6n/K8tPKRshfunuaHLiN6EmUpCoWdFB
0mm/Me4kJDnLCNh2LcZExsEyyuegwRKiQkqpqrJYdXJAsTvJZdGyh7Gg41RTNtBk
zkpRwhkbmH1aiKg3k23JP7rpodvkvWxP/I9gYa/+aaBMHUXP46TEB+/JCAVJ2wSd
IiqnlF0dNgwT4iYGvOegA+o+sQLSS6qxAwzYwqc8+QKBgQDb87bBq8ruDVSLUq/3
ZQ9wuVcVp+gTKeaIwDHnsdXpPqROB9wR4gZQkmLSehNNdHqYhCIVTagDv3fqxl/C
Zfi+qRriEkZXb9PmXOVhQ1V2iPfbOGhb5MNmiCNt5g5CLFYDOGyApcQRGRYSnIi7
HJQYMkBeXMDTUfZjVCCRiPj2gwKBgQDCTYVe1kphqcBlVCK+k8hoULhIRMcrYkoS
pn/UCKMoezGMniJRGSwhqGmZ7nMmSPWBdRJhjUz06+ldH66L7D9V4tpKA5hERZu/
jF/9XEOZC/5duFEGGjqGRorZcKr7PktdEA7sqL2HJjlomor8+mN6eE+xnlzkiv4/
OVuz8k2TKQKBgCDrpFHudYwVwK6trJq5rIrxoqAiJnEf11pmcdG840hq0MrBEGY4
zzsl/daE4Pi501Oai0NnARu2f6tLD7AX9RgMVXK6y35wxWV4R/i7zEHWlEpicfQ3
FCv3el4LjdzHqAQJYOwMY4EvoQceGLTwvRW2B75X4aB/ZwJM6CcAOksjAoGBAI9W
luwmk8rGaruVvOfwp6hk2/3cVz3nDZQCpkxqbsrfTudYhSJkuoy96+F16ha1Pf/Z
uMyka8BHBOoBBEgO6Y5XgweU7VB4NXSyfuGuneOa2HSVE5i34rLJ+YZ9OnWMOBaY
URo/RUodZb6IO75yjM59UPVMSWUEMJm9m6v9NUM5AoGBAMiMxyOYtRvdUyeGw4Nx
v8o8jn7nCjqyDthFaWnhpMGNon7YBZtscLCXQO3aczg54JzMDv8n2fjcuPNotLEF
I24eIA6C5ZaDK/cVaSxJA1RqEYObQLsYEVPFWJ1mUvFblghrDCYlCOSTyb4mgEne
QaNhx+7+7bVHEa8EwZrZIkqU
-----END PRIVATE KEY-----
`

func main() {
	block, _ := pem.Decode([]byte(key))
	parsed, err := x509.ParsePKCS8PrivateKey(block.Bytes)
	if err != nil {
		panic(err)
	}
	priv := parsed.(*rsa.PrivateKey)
	data := []byte("a message of some length to sign, as a token or a handshake would")
	var signature []byte
	for i := 0; i < 3000; i++ {
		digest := sha256.Sum256(data)
		signature, _ = rsa.SignPKCS1v15(rand.Reader, priv, crypto.SHA256, digest[:])
	}
	sum := sha256.Sum256(signature)
	fmt.Println(hex.EncodeToString(sum[:]))
}
