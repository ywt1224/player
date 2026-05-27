#ifndef LCEF_FORMAT_HPP
#define LCEF_FORMAT_HPP
#include <cstdint>
#include <string>
#include <cstdio>
#include <cstring>

inline bool isLcefFile(const std::string& path) {
    FILE* fp = nullptr;
#ifdef _MSC_VER
    fopen_s(&fp, path.c_str(), "rb");
#else
    fp = fopen(path.c_str(), "rb");
#endif
    if (!fp) return false;

    char magic[4] = {};
    size_t n = fread(magic, 1, 4, fp);
    fclose(fp);

    return (n == 4 && memcmp(magic, "LCEF", 4) == 0);
}



// LCEF (Local Crypto Encrypted File) 格式定义
struct LCEFHeader {
    uint8_t  magic[4];           // "LCEF"
    uint8_t  version;            // 0x01
    uint8_t  algo;               // 0x01=AES-128-CTR, 0x02=AES-256-CTR
    uint8_t  key_derivation;     // 0x01=PBKDF2-HMAC-SHA256
    uint8_t  reserved1;

    uint32_t header_size;        // 256
    uint32_t data_offset;        // 256
    uint64_t original_size;      // 原始文件大小
    uint64_t encrypted_size;     // 加密数据区大小（与 original_size 相同）
    char     container[8];       // "mp4\0", "mkv\0", "flv\0"...

    uint8_t  iv[16];             // AES-CTR 初始向量
    uint8_t  salt[16];           // PBKDF2 盐值
    uint32_t pbkdf2_iters;       // 迭代次数

    uint8_t  reserved2[180];     // 扩展预留
};

static_assert(sizeof(LCEFHeader) == 256, "LCEFHeader must be 256 bytes");

// 算法常量
constexpr uint8_t ALGO_AES_128_CTR = 0x01;
constexpr uint8_t ALGO_AES_256_CTR = 0x02;
constexpr uint8_t KDF_PBKDF2_HMAC_SHA256 = 0x01;

#endif // LCEF_FORMAT_HPP
