#ifndef LCEF_AVIO_H
#define LCEF_AVIO_H

#ifdef __cplusplus
extern "C" {
#endif

struct AVIOContext;

#ifdef __cplusplus
}
#endif

struct LcefAvioUserData;

// Allocate a custom AVIO context backed by an LCEF-encrypted file.
// Returns nullptr on failure.
AVIOContext* lcef_avio_open(const char* filePath, const char* password,
                            int bufferSize);

// Close and free. Safe to call with nullptr.
void lcef_avio_close(AVIOContext* avio);

#endif // LCEF_AVIO_H
