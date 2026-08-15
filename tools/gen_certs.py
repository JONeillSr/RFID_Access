"""Generate lib/CloudSync/RootCerts.h from verified PEM files.

Written as a file rather than a shell heredoc because the escaping matters:
each PEM line has to become a C string literal ending in a backslash-n escape,
and heredocs in this environment eat backslashes.
"""
import os

BS_N = chr(92) + "n"          # the two characters: backslash, n

SCRATCH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "lib", "CloudSync", "certs")
OUT = r"c:\PlatformIO\Projects\RFID_Access\lib\CloudSync\RootCerts.h"


def pem_to_c(path, name):
    lines = [l.strip() for l in open(os.path.join(SCRATCH, path)) if l.strip()]
    body = "\n".join('    "%s%s"' % (l, BS_N) for l in lines)
    return "static const char %s[] PROGMEM =\n%s;\n" % (name, body)


HEADER = '''/**
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
 *   openssl s_client -connect <host>:443 -servername <host> \\
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

'''

FOOTER = '''
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
 *   openssl s_client -connect $host:443 -servername $host -showcerts \\
 *     </dev/null 2>/dev/null > chain.txt
 *   # pull the anchor you want out of chain.txt into anchor.pem, then:
 *   openssl x509 -in anchor.pem -noout -subject -dates -fingerprint -sha256
 *   openssl s_client -connect $host:443 -servername $host \\
 *     -CAfile anchor.pem -verify_return_error </dev/null | grep Verify
 *
 * Only a "Verify return code: 0 (ok)" with the system store excluded proves the
 * anchor actually works. Then regenerate this file with tools/gen_certs.py.
 */
'''

out = HEADER
out += "// DigiCert Global Root G2 -- expires 2038-01-15. The long-lived anchor.\n"
out += ("// SHA-256 CB:3C:CB:B7:60:31:E5:E0:13:8F:8D:D3:9A:23:F9:DE:47:FF:C3:5E:"
        "43:C1:14:4C:EA:27:D4:6A:5A:B1:CB:5F\n")
out += pem_to_c("digicert_g2.pem", "CERT_DIGICERT_GLOBAL_ROOT_G2")
out += "\n// Microsoft TLS RSA Root G2 -- expires 2029-06-19. Present in the chain\n"
out += "// served today; kept as a second anchor in case Azure stops chaining to\n"
out += "// DigiCert. Shorter life, so DigiCert G2 above is the primary.\n"
out += ("// SHA-256 DD:CD:1E:8A:20:63:8D:4A:AF:F7:20:1B:B1:D5:64:52:AC:D2:C7:59:"
        "F1:68:6B:DC:38:F7:3D:D1:57:32:BD:C2\n")
out += pem_to_c("msroot.pem", "CERT_MICROSOFT_TLS_RSA_ROOT_G2")
out += FOOTER

os.makedirs(os.path.dirname(OUT), exist_ok=True)
open(OUT, "w", encoding="utf-8").write(out)
print("wrote %s (%d bytes)" % (OUT, len(out)))
