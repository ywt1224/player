#ifndef LCEF_DECRYPTOR_H
#define LCEF_DECRYPTOR_H

#include "lcef_format.h"
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

class LcefDecryptor {
public:
    ~LcefDecryptor();

    bool open(const std::string& filePath, const std::string& password);
    void close();

    int  read(uint8_t* buf, int size, int64_t offset);
    int64_t size() const { return static_cast<int64_t>(originalSize_); }

private:
    bool parseHeader();
    bool deriveKey(const std::string& password);
    static void ctrAdvanceCounter(uint8_t* counter, uint64_t n);

    FILE*       fp_ = nullptr;
    LCEFHeader  header_;
    std::vector<uint8_t> key_;
    uint64_t    originalSize_ = 0;
};

#endif // LCEF_DECRYPTOR_H
