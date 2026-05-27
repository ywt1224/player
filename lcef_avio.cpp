#include "lcef_avio.h"
#include "lcef_decryptor.h"

extern "C" {
#include <libavformat/avio.h>
}

struct LcefAvioUserData {
    LcefDecryptor  decryptor;
    int64_t        position = 0;   // current read cursor in decrypted space
    uint8_t*       buffer = nullptr;
    int            bufferSize = 0;
};

static int lcef_read(void* opaque, uint8_t* buf, int bufSize) {
    auto* ud = static_cast<LcefAvioUserData*>(opaque);

    int64_t remaining = ud->decryptor.size() - ud->position;
    if (remaining <= 0) return AVERROR_EOF;

    int toRead = static_cast<int>(std::min(static_cast<int64_t>(bufSize), remaining));
    int n = ud->decryptor.read(buf, toRead, ud->position);
    if (n <= 0) return AVERROR(EIO);

    ud->position += n;
    return n;
}

static int64_t lcef_seek(void* opaque, int64_t offset, int whence) {
    auto* ud = static_cast<LcefAvioUserData*>(opaque);
    int64_t size = ud->decryptor.size();

    switch (whence & ~AVSEEK_FORCE) {
    case SEEK_SET: ud->position = offset; break;
    case SEEK_CUR: ud->position += offset; break;
    case SEEK_END: ud->position = size + offset; break;
    case AVSEEK_SIZE: return size;
    default: return AVERROR(EINVAL);
    }

    if (ud->position < 0) ud->position = 0;
    if (ud->position > size) ud->position = size;
    return ud->position;
}

AVIOContext* lcef_avio_open(const char* filePath, const char* password,
                            int bufferSize) {
    if (!filePath || !password || bufferSize <= 0) return nullptr;

    auto* ud = new LcefAvioUserData();
    ud->bufferSize = bufferSize;
    ud->buffer = static_cast<uint8_t*>(av_malloc(bufferSize));
    if (!ud->buffer) { delete ud; return nullptr; }

    if (!ud->decryptor.open(filePath, password)) {
        av_free(ud->buffer);
        delete ud;
        return nullptr;
    }

    AVIOContext* avio = avio_alloc_context(
        ud->buffer, bufferSize,
        0,          // write_flag = 0 (read-only)
        ud,
        lcef_read,
        nullptr,    // write (unused)
        lcef_seek);

    if (!avio) {
        av_free(ud->buffer);
        delete ud;
        return nullptr;
    }

    return avio;
}

void lcef_avio_close(AVIOContext* avio) {
    if (!avio) return;

    auto* ud = static_cast<LcefAvioUserData*>(avio->opaque);

    av_freep(&avio->buffer);
    avio_context_free(&avio);

    if (ud) {
        ud->decryptor.close();
        delete ud;
    }
}
