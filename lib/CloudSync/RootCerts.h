/**
 * @file    RootCerts.h
 * @brief   TLS trust anchors for the Azure backend.
 *
 * GENERATED -- do not hand-edit. See the refresh procedure at the bottom.
 *
 * THE MAINTENANCE TRAP IN THIS WHOLE DESIGN.
 *
 * A device validates the backend against these and nothing else; an ESP32 has
 * no system trust store. If every anchor here expires or is rotated out, EVERY
 * DOOR STOPS SYNCING AT ONCE. Doors keep granting access from their cached
 * rosters -- that is exactly what the local-first design is for -- but the
 * fleet goes silently blind until someone reflashes it.
 *
 * Three mitigations, all deliberate:
 *   1. Two anchors are embedded, not one.
 *   2. A sync failure is surfaced loudly on /status rather than logged quietly,
 *      because a fleet that stopped reporting looks identical to a quiet one.
 *   3. OTA never depends on sync succeeding, so a certificate failure stays
 *      recoverable over the network instead of requiring a ladder.
 *
 * Verified 2026-08-15 against the live endpoint with the system trust store
 * EXCLUDED:
 *
 *   openssl s_client -connect <host>:443 -servername <host> \
 *           -CAfile bundle.pem -verify_return_error
 *   -> Verification: OK
 *
 * Both anchors validate the chain; DigiCert Global Root G2 alone is sufficient.
 *
 * Chain as served by *.azurewebsites.net:
 *   leaf  *.azurewebsites.net
 *     <- Microsoft TLS G2 RSA CA OCSP 16   (intermediate)
 *     <- Microsoft TLS RSA Root G2         (cross-signed, expires 2029-06-19)
 *     <- DigiCert Global Root G2           (root,         expires 2038-01-15)
 */

#pragma once
#include <Arduino.h>

// DigiCert Global Root G2 -- expires 2038-01-15. The long-lived anchor.
// SHA-256 CB:3C:CB:B7:60:31:E5:E0:13:8F:8D:D3:9A:23:F9:DE:47:FF:C3:5E:43:C1:14:4C:EA:27:D4:6A:5A:B1:CB:5F
static const char CERT_DIGICERT_GLOBAL_ROOT_G2[] PROGMEM =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIDjjCCAnagAwIBAgIQAzrx5qcRqaC7KGSxHQn65TANBgkqhkiG9w0BAQsFADBh\n"
    "MQswCQYDVQQGEwJVUzEVMBMGA1UEChMMRGlnaUNlcnQgSW5jMRkwFwYDVQQLExB3\n"
    "d3cuZGlnaWNlcnQuY29tMSAwHgYDVQQDExdEaWdpQ2VydCBHbG9iYWwgUm9vdCBH\n"
    "MjAeFw0xMzA4MDExMjAwMDBaFw0zODAxMTUxMjAwMDBaMGExCzAJBgNVBAYTAlVT\n"
    "MRUwEwYDVQQKEwxEaWdpQ2VydCBJbmMxGTAXBgNVBAsTEHd3dy5kaWdpY2VydC5j\n"
    "b20xIDAeBgNVBAMTF0RpZ2lDZXJ0IEdsb2JhbCBSb290IEcyMIIBIjANBgkqhkiG\n"
    "9w0BAQEFAAOCAQ8AMIIBCgKCAQEAuzfNNNx7a8myaJCtSnX/RrohCgiN9RlUyfuI\n"
    "2/Ou8jqJkTx65qsGGmvPrC3oXgkkRLpimn7Wo6h+4FR1IAWsULecYxpsMNzaHxmx\n"
    "1x7e/dfgy5SDN67sH0NO3Xss0r0upS/kqbitOtSZpLYl6ZtrAGCSYP9PIUkY92eQ\n"
    "q2EGnI/yuum06ZIya7XzV+hdG82MHauVBJVJ8zUtluNJbd134/tJS7SsVQepj5Wz\n"
    "tCO7TG1F8PapspUwtP1MVYwnSlcUfIKdzXOS0xZKBgyMUNGPHgm+F6HmIcr9g+UQ\n"
    "vIOlCsRnKPZzFBQ9RnbDhxSJITRNrw9FDKZJobq7nMWxM4MphQIDAQABo0IwQDAP\n"
    "BgNVHRMBAf8EBTADAQH/MA4GA1UdDwEB/wQEAwIBhjAdBgNVHQ4EFgQUTiJUIBiV\n"
    "5uNu5g/6+rkS7QYXjzkwDQYJKoZIhvcNAQELBQADggEBAGBnKJRvDkhj6zHd6mcY\n"
    "1Yl9PMWLSn/pvtsrF9+wX3N3KjITOYFnQoQj8kVnNeyIv/iPsGEMNKSuIEyExtv4\n"
    "NeF22d+mQrvHRAiGfzZ0JFrabA0UWTW98kndth/Jsw1HKj2ZL7tcu7XUIOGZX1NG\n"
    "Fdtom/DzMNU+MeKNhJ7jitralj41E6Vf8PlwUHBHQRFXGU7Aj64GxJUTFy8bJZ91\n"
    "8rGOmaFvE7FBcf6IKshPECBV1/MUReXgRPTqh5Uykw7+U0b6LJ3/iyK5S9kJRaTe\n"
    "pLiaWN0bfVKfjllDiIGknibVb63dDcY3fe0Dkhvld1927jyNxF1WW6LZZm6zNTfl\n"
    "MrY=\n"
    "-----END CERTIFICATE-----\n";

// Microsoft TLS RSA Root G2 -- expires 2029-06-19. Present in the chain
// served today; kept as a second anchor in case Azure stops chaining to
// DigiCert. Shorter life, so DigiCert G2 above is the primary.
// SHA-256 DD:CD:1E:8A:20:63:8D:4A:AF:F7:20:1B:B1:D5:64:52:AC:D2:C7:59:F1:68:6B:DC:38:F7:3D:D1:57:32:BD:C2
static const char CERT_MICROSOFT_TLS_RSA_ROOT_G2[] PROGMEM =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIFiTCCBHGgAwIBAgIQCwxrLEZpF7BHc8ZH1K/AyDANBgkqhkiG9w0BAQwFADBh\n"
    "MQswCQYDVQQGEwJVUzEVMBMGA1UEChMMRGlnaUNlcnQgSW5jMRkwFwYDVQQLExB3\n"
    "d3cuZGlnaWNlcnQuY29tMSAwHgYDVQQDExdEaWdpQ2VydCBHbG9iYWwgUm9vdCBH\n"
    "MjAeFw0yNTA1MjEwMDAwMDBaFw0yOTA2MTkyMzU5NTlaMFExCzAJBgNVBAYTAlVT\n"
    "MR4wHAYDVQQKExVNaWNyb3NvZnQgQ29ycG9yYXRpb24xIjAgBgNVBAMTGU1pY3Jv\n"
    "c29mdCBUTFMgUlNBIFJvb3QgRzIwggIiMA0GCSqGSIb3DQEBAQUAA4ICDwAwggIK\n"
    "AoICAQDf6oufR+EoEHGvQdYZ25JX3mur5i7erTpgg7cTmKxbuTILe+ufcidrXUCr\n"
    "vhgGk7IN0hLtuHT1fy/qqBeU9jMWV4reIHwh3bfarN5OZLBazUt18+8CZE3tUtqj\n"
    "jwTokfjX+z8Z/U5FOV7oKcPW8mevswCUwY3h8EoYmDn6wAmEM0EFAwWr9HXhU6Uh\n"
    "klxETOZgV6SQApfH1diTBDJK7YVR7dbFuqA/Noovb0w5qARpIoQ7dRT32T60qdAH\n"
    "QTiBfkZIHegZ5nC4oKoY3XK/fn21bE4ZcBGEBBOB1GL9nGvxHN3/7Kfg5seNMUu/\n"
    "8mszzNGMtv6xG6NKqF8OfzF2OD8HR2wBqKylFNqCsF8fbLyJGsASKst7lx8oLjEW\n"
    "ilNMdWb5fQHWwmCqZY8xnnLLzJst5UQZk1erbo7C2S5lsHIt56HDoX5JHVln1gnU\n"
    "GBJtwJVFeMnxYGrk9u4GJDtzSloRwj6XYcB47u8TpzDiSjgt7lgXEyC3NirfCzK0\n"
    "wjixkd0SsEW2fMCxHWKhnd1xEhWWAZ0KCfWx3bPZ4DhCNPZptsOvFnP+1EP4Q+RY\n"
    "+U+z8+zWPZQ6QDgVqwyG0GTOGmPohJRVCVq2BLbRPpoVx2QRgNAbgg5N/0WesmUH\n"
    "JR/bmsjG7NZbhVAEnxzLXSCCZ5554t/o8uhvxCByMIblnXUnNQIDAQABo4IBSzCC\n"
    "AUcwDwYDVR0TAQH/BAUwAwEB/zAdBgNVHQ4EFgQU3pGGSLehMVkx8UtfB6nciHna\n"
    "qHYwHwYDVR0jBBgwFoAUTiJUIBiV5uNu5g/6+rkS7QYXjzkwDgYDVR0PAQH/BAQD\n"
    "AgGGMBMGA1UdJQQMMAoGCCsGAQUFBwMBMHYGCCsGAQUFBwEBBGowaDAkBggrBgEF\n"
    "BQcwAYYYaHR0cDovL29jc3AuZGlnaWNlcnQuY29tMEAGCCsGAQUFBzAChjRodHRw\n"
    "Oi8vY2FjZXJ0cy5kaWdpY2VydC5jb20vRGlnaUNlcnRHbG9iYWxSb290RzIuY3J0\n"
    "MEIGA1UdHwQ7MDkwN6A1oDOGMWh0dHA6Ly9jcmwzLmRpZ2ljZXJ0LmNvbS9EaWdp\n"
    "Q2VydEdsb2JhbFJvb3RHMi5jcmwwEwYDVR0gBAwwCjAIBgZngQwBAgIwDQYJKoZI\n"
    "hvcNAQEMBQADggEBAAu8tCs3dMVLpzYCNsav4RPMipqXG/zjRIzuVADl5EEaRvAL\n"
    "djT/mVViNaqtipwMWmLMQ8DL6kodvWsdr7EZJWac93luWyWAJIGFx3ktNV9CCXjt\n"
    "n+Jl1cQgUIIQj2o67RiOSImrpgn44YD8BnUWJyVaj7g6cGwYR/Bj9FMO2RU1IPOR\n"
    "PRMBoOL6JAhFVnfRZ6kxQtBX/xomvsVD2FepY/+v8zrY9ntLEKKXoc9mvmdnCfm1\n"
    "TOerGSu/Ij193sb372M4LN1WxPkJUtrf44hv1W1r9whBL44+hjGf8XxK9dZhpEZG\n"
    "KO9XurBvktjSdyXte6YpzjtyeRHU4KdUbTUrpHo=\n"
    "-----END CERTIFICATE-----\n";

// mbedTLS accepts concatenated PEMs as a single trust store, so both anchors
// are offered and either may validate the chain.
static const char* const CLOUD_ROOT_CERTS[] = {
    CERT_DIGICERT_GLOBAL_ROOT_G2,
    CERT_MICROSOFT_TLS_RSA_ROOT_G2,
};
static const size_t CLOUD_ROOT_CERT_COUNT =
    sizeof(CLOUD_ROOT_CERTS) / sizeof(CLOUD_ROOT_CERTS[0]);

/*
 * TO REFRESH -- never hand-copy a certificate from a web page or from memory.
 * Re-extract it from the live endpoint and prove it validates before trusting:
 *
 *   host=jtc-prod-rfidaccess-eastus2-func.azurewebsites.net
 *   openssl s_client -connect $host:443 -servername $host -showcerts \
 *     </dev/null 2>/dev/null > chain.txt
 *   # pull the anchor you want out of chain.txt into anchor.pem, then:
 *   openssl x509 -in anchor.pem -noout -subject -dates -fingerprint -sha256
 *   openssl s_client -connect $host:443 -servername $host \
 *     -CAfile anchor.pem -verify_return_error </dev/null | grep Verify
 *
 * Only a "Verify return code: 0 (ok)" with the system store excluded proves the
 * anchor actually works. Then regenerate this file with tools/gen_certs.py.
 */
