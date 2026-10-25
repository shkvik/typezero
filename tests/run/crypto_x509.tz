// same-as-node
// X509Certificate of a certificate and of the CA that issued it.
import {
  createPrivateKey, createPublicKey, X509Certificate,
} from "node:crypto"

const caPem = "-----BEGIN CERTIFICATE-----\nMIIB5TCCAYugAwIBAgIUb8jpouuTtg9Bnawipnl9mK72e+kwCgYIKoZIzj0EAwIw\nQDELMAkGA1UEBhMCUlUxFjAUBgNVBAoMDVR5cGV6ZXJvIFRlc3QxGTAXBgNVBAMM\nEFR5cGV6ZXJvIFRlc3QgQ0EwHhcNMjYxMDI0MDc0NTM3WhcNMzYxMDIxMDc0NTM3\nWjBAMQswCQYDVQQGEwJSVTEWMBQGA1UECgwNVHlwZXplcm8gVGVzdDEZMBcGA1UE\nAwwQVHlwZXplcm8gVGVzdCBDQTBZMBMGByqGSM49AgEGCCqGSM49AwEHA0IABET3\n2ICDTPzfQzSKD7ML+ugQyrwkGW4Z8xMxTotstwg9PzvDBCLUr+P8zTtCQILv9+gO\nLeqJXbsD6bwjDYEiAzujYzBhMB0GA1UdDgQWBBQ9lUhAiMdcAzKRtlfEvCkokpJf\nkzAfBgNVHSMEGDAWgBQ9lUhAiMdcAzKRtlfEvCkokpJfkzAPBgNVHRMBAf8EBTAD\nAQH/MA4GA1UdDwEB/wQEAwICBDAKBggqhkjOPQQDAgNIADBFAiEA+ldZMLL/YOn/\nEK8CdwNVgi8HBVlDLAkoJfLAnRNMWsACIF2sxXQWjugT4FSlidPj2QUizW3Yo35a\nQCbA7dOyUJ2H\n-----END CERTIFICATE-----\n"
const leafPem = "-----BEGIN CERTIFICATE-----\nMIICfjCCAiOgAwIBAgIFGis8TV4wCgYIKoZIzj0EAwIwQDELMAkGA1UEBhMCUlUx\nFjAUBgNVBAoMDVR5cGV6ZXJvIFRlc3QxGTAXBgNVBAMMEFR5cGV6ZXJvIFRlc3Qg\nQ0EwHhcNMjYxMDI0MDc0NTM3WhcNMjkwMTI2MDc0NTM3WjBHMQswCQYDVQQGEwJS\nVTEPMA0GA1UECAwGTW9zY293MREwDwYDVQQKDAhUeXBlemVybzEUMBIGA1UEAwwL\nZXhhbXBsZS5jb20wWTATBgcqhkjOPQIBBggqhkjOPQMBBwNCAAT8iAzehYUx8U9p\n+A3Mx8g+O1M5QgIoDZjO7P/5pV8LT5R6r6TMDY7WuPyuS0yYAdzGG5YfSb5mBmSu\nIa/MaysUo4IBATCB/jA+BgNVHREENzA1ggtleGFtcGxlLmNvbYINKi5leGFtcGxl\nLm9yZ4cEfwAAAYERYWRtaW5AZXhhbXBsZS5jb20wHQYDVR0lBBYwFAYIKwYBBQUH\nAwEGCCsGAQUFBwMCMF0GCCsGAQUFBwEBBFEwTzAjBggrBgEFBQcwAYYXaHR0cDov\nL29jc3AuZXhhbXBsZS5jb20wKAYIKwYBBQUHMAKGHGh0dHA6Ly9jYS5leGFtcGxl\nLmNvbS9jYS5wZW0wHQYDVR0OBBYEFD1DakJ085flVmeGTqbeWZ3rk6bFMB8GA1Ud\nIwQYMBaAFD2VSECIx1wDMpG2V8S8KSiSkl+TMAoGCCqGSM49BAMCA0kAMEYCIQCT\ni6lg0duxs1mMVFkDI08ro5FhLSrzeMA098ejcGHrYQIhALpr2lHCG6CJcbYjGKOO\njkDs5k0MQsW4k2HO2F0ERIng\n-----END CERTIFICATE-----\n"
const leafKeyPem = "-----BEGIN EC PRIVATE KEY-----\nMHcCAQEEIEpIeyUBvrdKL/EqIOR1ns5z5T3Du94rfZEhMBFoLcuvoAoGCCqGSM49\nAwEHoUQDQgAE/IgM3oWFMfFPafgNzMfIPjtTOUICKA2Yzuz/+aVfC0+Ueq+kzA2O\n1rj8rktMmAHcxhuWH0m+ZgZkriGvzGsrFA==\n-----END EC PRIVATE KEY-----\n"
const cert = new X509Certificate(leafPem)
const ca = new X509Certificate(caPem)
console.log(cert.subject)
console.log(cert.issuer)
console.log(cert.subjectAltName)
console.log(cert.infoAccess)
console.log(cert.validFrom, cert.validTo, cert.validToDate.getUTCFullYear())
console.log(cert.serialNumber, cert.ca, ca.ca)
console.log(cert.fingerprint)
console.log(cert.fingerprint256)
console.log(cert.fingerprint512.length, cert.keyUsage, ca.keyUsage)
console.log(cert.raw.length, cert.toString() == leafPem, cert.toJSON() == leafPem)
console.log(cert.publicKey.asymmetricKeyType, cert.publicKey.equals(createPublicKey(createPrivateKey(leafKeyPem))))
console.log(cert.checkHost("example.com"), cert.checkHost("a.example.org"), cert.checkHost("example.net"))
console.log(cert.checkHost("a.b.example.org"), cert.checkEmail("admin@example.com"), cert.checkIP("127.0.0.1"))
console.log(cert.checkIP("10.0.0.1"), cert.checkIssued(ca), ca.checkIssued(cert))
console.log(cert.verify(ca.publicKey), cert.verify(cert.publicKey), cert.checkPrivateKey(createPrivateKey(leafKeyPem)))
console.log(new X509Certificate(cert.raw).serialNumber)

