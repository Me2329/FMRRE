#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

#include "core/sha256.h"

namespace tw::crypt {

// AES-256, encryption direction only (all that CTR mode needs). Table based, constant tables.
class Aes256 {
public:
    explicit Aes256(const uint8_t key[32]);
    void encryptBlock(const uint8_t in[16], uint8_t out[16]) const;

private:
    uint32_t rk_[60];
};

class HmacSha256 {
public:
    HmacSha256(const uint8_t* key, size_t len);
    void update(const void* d, size_t n) { inner_.update(d, n); }
    std::string final();  // raw 32 bytes; the object can't be reused afterwards
    // Fresh copy with the same key (cheap: the padded key blocks are already hashed).
    HmacSha256 clone() const { return *this; }

private:
    Sha256 inner_, outer_;
};

void pbkdf2Sha256(const std::string& password, const uint8_t* salt, size_t saltLen, uint32_t iterations, uint8_t* out, size_t outLen);
bool randomBytes(uint8_t* out, size_t n);

// Encrypted backup files:
//   "TWV2ENC1" | salt[16] | iterations (u32 LE) | check[16] | nonce[16] | AES-256-CTR data | HMAC-SHA256[32]
// Keys: PBKDF2-HMAC-SHA256(password, salt) -> 32 bytes AES key + 32 bytes MAC key; `check` tells a
// wrong password apart before anything is decrypted; the HMAC covers everything before it.
constexpr size_t kHeaderSize = 8 + 16 + 4 + 16 + 16, kTagSize = 32;
constexpr uint32_t kIterations = 200000;
bool isEncrypted(const std::string& path);

class Stream {
public:
    // Encrypt: makes a fresh salt + nonce; `header` gets the bytes to write first.
    bool beginEncrypt(const std::string& password, std::string& header, uint32_t iterations = kIterations);
    // Decrypt: `header` = the first kHeaderSize bytes of the file. false + err for a wrong password.
    bool beginDecrypt(const std::string& password, const uint8_t* header, std::string& err);
    // In place. Encrypting: plaintext -> ciphertext (MAC over the ciphertext); decrypting: the reverse.
    void process(uint8_t* data, size_t n);
    std::string tag();  // after the last process()

private:
    void deriveKeys(const std::string& password, const uint8_t* salt, uint32_t iterations, uint8_t* check);
    bool encrypt_ = true;
    uint8_t aesKey_[32]{}, macKey_[32]{}, counter_[16]{}, ks_[16]{};
    size_t ksUsed_ = 16;
    Aes256* aes_ = nullptr;
    HmacSha256* mac_ = nullptr;

public:
    Stream() = default;
    Stream(const Stream&) = delete;
    Stream& operator=(const Stream&) = delete;
    ~Stream();
};

}  // namespace tw::crypt
