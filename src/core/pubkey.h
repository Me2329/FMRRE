#pragma once
// Signature checks with the two kinds of keys Android signs OTAs with: RSA (PKCS#1 v1.5) and ECDSA on NIST P-256.
// Only verification - public data in, yes / no out - so nothing here needs to be constant-time.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace tw::pk {

enum class Hash { Sha1, Sha256 };

// Big-endian magnitudes, as they are stored in certificates.
struct RsaKey {
    std::vector<uint8_t> n, e;
    int bits() const;
};
struct EcKey {  // a point on P-256
    std::vector<uint8_t> x, y;  // 32 bytes each
};

// RSASSA-PKCS1-v1_5, like Android's recovery (BoringSSL RSA_verify): sig^e mod n must be exactly
// 00 01 FF..FF 00 DigestInfo(hash, digest). The signature has the modulus' length.
bool rsaVerify(const RsaKey& k, Hash h, const std::string& digest, const uint8_t* sig, size_t sigLen);

// ECDSA on P-256. sigDer: SEQUENCE { INTEGER r, INTEGER s }. digest: 32 bytes (SHA-256).
bool ecdsaVerify(const EcKey& k, const std::string& digest, const uint8_t* sigDer, size_t sigLen);
bool ecOnCurve(const EcKey& k);

// Self-test of the curve arithmetic: the generator is on the curve and has the published order.
bool p256SelfTest();

}  // namespace tw::pk
