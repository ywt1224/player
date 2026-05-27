#pragma once

#include <string>
#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <cstdint>
#include <functional>

#include <QObject>
#include <QWebSocket>

extern "C" {
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
#include <opus/opus.h>
}

#include "vad_detector.h"

#pragma pack(push, 1)
struct NetAudioPacket {
    uint32_t magic = 0x41555241;
    uint16_t version = 1;
    uint32_t seq = 0;
    uint64_t pts = 0;
    uint16_t opus_len = 0;
    uint8_t  flags = 0;
    uint8_t  reserved[3] = {0};
};
#pragma pack(pop)

class AudioStreamer : public QObject {
    Q_OBJECT
public:
    explicit AudioStreamer(QObject* parent = nullptr);
    ~AudioStreamer();

    void SetWsUrl(const std::string& ws_url) { ws_url_ = ws_url; }
    bool Initialize(int src_rate, int src_channels, AVSampleFormat src_fmt);
    void FeedAudioFrame(const uint8_t* const* data, int linesize,
                        int nb_samples, int64_t pts_ms, AVSampleFormat fmt);
    void Shutdown();
    bool IsConnected() const { return ws_connected_.load(); }

    using SubtitleCallback = std::function<void(const std::string& json)>;
    void SetSubtitleCallback(SubtitleCallback cb) { subtitle_cb_ = std::move(cb); }

signals:
    void sigConnected();
    void sigDisconnected();

private slots:
    void onConnected();
    void onDisconnected();
    void onTextMessage(const QString& message);

private:
    void ProcessThreadFunc();
    bool ResampleFrame(const uint8_t* const* src, int src_samples,
                       AVSampleFormat src_fmt, std::vector<int16_t>& dst_pcm);
    void RunVadAndEncode(const std::vector<int16_t>& pcm, int64_t base_pts);
    void SendPacket(const std::vector<uint8_t>& opus_data,
                    uint64_t pts, bool is_speech, bool vad_end);

    static constexpr int kDstRate       = 16000;
    static constexpr int kDstChannels   = 1;
    static constexpr int kFrameMs       = 20;
    static constexpr int kFrameSamples  = 320;
    static constexpr int kMaxSilenceMs  = 800;
    static constexpr int kOpusMaxPayload = 1275;
    static constexpr uint32_t kMagic    = 0x41555241;

    int src_rate_     = 48000;
    int src_channels_ = 2;
    AVSampleFormat src_fmt_ = AV_SAMPLE_FMT_FLTP;

    SwrContext*   swr_ctx_   = nullptr;
    VadDetector   vad_;
    OpusEncoder*  opus_enc_  = nullptr;

    struct RawFrame {
        std::vector<uint8_t> buffer;
        int nb_samples = 0;
        int64_t pts_ms = 0;
        AVSampleFormat fmt = AV_SAMPLE_FMT_NONE;
    };
    std::queue<RawFrame> raw_queue_;
    std::mutex raw_mtx_;
    std::condition_variable raw_cv_;
    std::atomic<bool> running_{false};

    std::thread process_thread_;

    QWebSocket ws_socket_;
    std::string ws_url_;
    std::atomic<bool> ws_connected_{false};

    uint32_t seq_counter_ = 0;

    SubtitleCallback subtitle_cb_;
};
