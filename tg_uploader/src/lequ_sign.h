#pragma once
//
// LeQu (乐趣Live / com.tencent.coreframe) — Zeus request signing.
//
// Faithful C++ port of LeQvLive/lequ_sign.py (reverse-engineered from
// libgojni.so, verified against real Frida captures 2026-06-06). Produces the
// EL-* headers + (optionally AES-128-CBC encrypted) body the gateway expects.
//
//   EL-AUTH  = auth (passed through unchanged; omitted when empty)
//   EL-NS    = base64(16-byte nonce: 12 random || 4 time-derived)
//   EL-SIGN  = base64(SHA512(SFib + auth + ect + EL-NS + EL-VER + path + body_sign)[:24])
//   EL-VER   = "E1.0" when (encrypt && body non-empty) else "M1.0"
//   EL-ECT   = decimal(ect)
//   body     = base64(AES-128-CBC(body)) when encrypted, else raw body
//
#include <string>

struct LequSigned {
    std::string el_sign;   // EL-SIGN header
    std::string el_ns;     // EL-NS header (base64 nonce)
    std::string el_ver;    // EL-VER header ("E1.0" | "M1.0")
    std::string el_ect;    // EL-ECT header (decimal ect)
    std::string body;      // request body to send (encrypted+base64, or raw)
};

// Sign a request. Generates a fresh nonce each call (one per request — the
// gateway rejects reused nonces and stale timestamps).
LequSigned lequ_sign(const std::string& path, const std::string& auth,
                     const std::string& body, int ect, bool encrypt);

// Deterministic variant used by the self-test: caller supplies the exact
// 16-byte nonce so output can be compared to captured ground-truth values.
LequSigned lequ_sign_with_nonce(const std::string& path, const std::string& auth,
                                const std::string& body, int ect, bool encrypt,
                                const unsigned char nonce[16]);

// Verify the port against the captured Frida vectors (SHA512 + AES). Prints
// PASS/FAIL per case to stdout. Returns true iff every vector matches.
bool lequ_sign_selftest();

// Standard base64 (with padding, '+' '/') helpers — exposed for reuse by the
// client/decrypt paths and tests.
std::string lequ_b64_encode(const unsigned char* data, size_t len);
std::string lequ_b64_decode(const std::string& in);   // returns raw bytes; "" on bad input
