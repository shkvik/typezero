// same-as-node
// env: OPENSSL_ia32cap=~0x200000200000000:~0xffffffffffffffff
// The ciphers of crypto_cipher.ts with neither AES-NI nor AVX-512 (the constant-time code of OpenSSL), which OPENSSL_ia32cap turns off.
import { ciphered } from "../crypto_cipher.ts"

if (!ciphered)
  console.log("not ciphered")
