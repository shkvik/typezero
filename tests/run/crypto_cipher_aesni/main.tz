// same-as-node
// env: OPENSSL_ia32cap=~0x0:~0xc0030000
// The ciphers of crypto_cipher.ts with AES-NI without AVX-512, which OPENSSL_ia32cap turns off.
import { ciphered } from "../crypto_cipher.ts"

if (!ciphered)
  console.log("not ciphered")
