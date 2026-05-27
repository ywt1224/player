#include "lcef_decryptor.h"
#include <openssl/evp.h>
#include <algorithm>
#include <cstring>

LcefDecryptor::~LcefDecryptor() {
    close();
}

bool LcefDecryptor::open(const std::string& filePath, const std::string& password) {
    close();

#ifdef _MSC_VER
    fopen_s(&fp_, filePath.c_str(), "rb");
#else
    fp_ = fopen(filePath.c_str(), "rb");
#endif
    if (!fp_) return false;

    if (!parseHeader()) { close(); return false; }
    if (!deriveKey(password)) { close(); return false; }

    return true;
}

void LcefDecryptor::close() {
    if (fp_) {
        fclose(fp_);
        fp_ = nullptr;
    }
    if (!key_.empty()) {
        OPENSSL_cleanse(key_.data(), key_.size());
        key_.clear();
    }
    originalSize_ = 0;
}

bool LcefDecryptor::parseHeader() {
    if (fread(&header_, 1, sizeof(header_), fp_) != sizeof(header_))
        return false;
    //校验是否为加密文件
    if (memcmp(header_.magic, "LCEF", 4) != 0) return false;
    if (header_.version != 0x01) return false;
    if (header_.header_size != 256 || header_.data_offset != 256) return false;

    originalSize_ = header_.original_size;
    return true;
}

bool LcefDecryptor::deriveKey(const std::string& password) {
    if (header_.algo != ALGO_AES_128_CTR && header_.algo != ALGO_AES_256_CTR)
        return false;

    size_t keyLen = (header_.algo == ALGO_AES_128_CTR) ? 16 : 32;
    key_.resize(keyLen);

    if (PKCS5_PBKDF2_HMAC(password.c_str(), static_cast<int>(password.size()),
                          header_.salt, sizeof(header_.salt),
                          static_cast<int>(header_.pbkdf2_iters),
                          EVP_sha256(),
                          static_cast<int>(keyLen),
                          key_.data()) != 1) {
        key_.clear();
        return false;
    }
    return true;
}

int LcefDecryptor::read(uint8_t* buf, int size, int64_t offset) {
    if (!fp_ || size <= 0) return 0;

    // Clamp to file bounds
    int64_t end = std::min(offset + size, static_cast<int64_t>(originalSize_));
    if (offset >= static_cast<int64_t>(originalSize_)) return 0;
    int effectiveSize = static_cast<int>(end - offset);

    int  inBlockOff = offset % 16;
    int64_t blockAlignedOff = offset - inBlockOff;
    uint64_t blockNum = blockAlignedOff / 16;

    // Read from block-aligned file position (data_offset + blockAlignedOff)
    int64_t filePos = static_cast<int64_t>(header_.data_offset) + blockAlignedOff;
#ifdef _MSC_VER
    if (_fseeki64(fp_, filePos, SEEK_SET) != 0) return -1;
#else
    if (fseeko(fp_, filePos, SEEK_SET) != 0) return -1;
#endif

    int readSize = effectiveSize + inBlockOff;
    std::vector<uint8_t> ciphertext(readSize);
    size_t nRead = fread(ciphertext.data(), 1, readSize, fp_);
    if (nRead <= static_cast<size_t>(inBlockOff)) return 0;

    // Build counter at block-aligned position
    uint8_t counter[16];
    memcpy(counter, header_.iv, 16);
    ctrAdvanceCounter(counter, blockNum);

    // Decrypt
    const EVP_CIPHER* cipher = (key_.size() == 16)
        ? EVP_aes_128_ctr() : EVP_aes_256_ctr();
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    EVP_DecryptInit_ex(ctx, cipher, nullptr, key_.data(), counter);

    std::vector<uint8_t> plaintext(readSize);
    int outLen = 0;
    EVP_DecryptUpdate(ctx, plaintext.data(), &outLen,
                      ciphertext.data(), static_cast<int>(nRead));
    EVP_CIPHER_CTX_free(ctx);

    // Skip partial-block prefix
    int copyLen = std::min(static_cast<int>(nRead) - inBlockOff, effectiveSize);
    memcpy(buf, plaintext.data() + inBlockOff, copyLen);
    return copyLen;
}

void LcefDecryptor::ctrAdvanceCounter(uint8_t* counter, uint64_t n) {
    // 128-bit big-endian addition (add n to lower 64 bits, carry to upper)
    uint32_t carry = 0;
    for (int i = 15; i >= 8; i--) {
        uint32_t sum = counter[i] + static_cast<uint8_t>(n & 0xFF) + carry;
        counter[i] = static_cast<uint8_t>(sum & 0xFF);
        carry = (sum >> 8) & 0xFF;
        n >>= 8;
    }
    for (int i = 7; i >= 0 && (carry || n); i--) {
        uint32_t sum = counter[i] + static_cast<uint8_t>(n & 0xFF) + carry;
        counter[i] = static_cast<uint8_t>(sum & 0xFF);
        carry = (sum >> 8) & 0xFF;
        n >>= 8;
    }
}
