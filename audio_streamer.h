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
#include <chrono>

#include <QObject>
#include <QWebSocket>
#include <QNetworkAccessManager>

extern "C" {
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

#include "vad_detector.h"

class AudioStreamer : public QObject {
    Q_OBJECT
public:
    explicit AudioStreamer(QObject* parent = nullptr);
    ~AudioStreamer();

    void SetNlsConfig(const std::string& ak_id,
                      const std::string& ak_secret,
                      const std::string& app_key,
                      const std::string& region = "cn-shanghai");
    bool Initialize(int src_rate, int src_channels, AVSampleFormat src_fmt);
    void FeedAudioFrame(const uint8_t* const* data, int linesize,
                        int nb_samples, int64_t pts_ms, AVSampleFormat fmt);
    void Shutdown();
    bool IsConnected() const { return session_started_.load(); }

    using SubtitleCallback = std::function<void(const std::string& json)>;
    void SetSubtitleCallback(SubtitleCallback cb) { subtitle_cb_ = std::move(cb); }

signals:
    void sigConnected();
    void sigDisconnected();

private slots:
    void onConnected();
    void onDisconnected();
    void onTextMessage(const QString& message);
    void onSslErrors(const QList<QSslError>& errors);

private:
    // Processing thread
    void ProcessThreadFunc();
    bool ResampleFrame(const uint8_t* const* src, int src_samples,
                       AVSampleFormat src_fmt, std::vector<int16_t>& dst_pcm);

    // NLS protocol
    bool GenerateToken();
    void ConnectToNls();
    void SendStartCommand();
    void SendStopCommand();
    void ParseNlsResult(const std::string& json);

    static constexpr int kDstRate      = 16000;
    static constexpr int kDstChannels  = 1;
    static constexpr int kFrameMs      = 20;
    static constexpr int kFrameSamples = 320;
    static constexpr int kMaxSilenceMs = 800;

    int src_rate_     = 48000;
    int src_channels_ = 2;
    AVSampleFormat src_fmt_ = AV_SAMPLE_FMT_FLTP;

    SwrContext* swr_ctx_ = nullptr;
    VadDetector vad_;

    // NLS auth
    std::string ak_id_;
    std::string ak_secret_;
    std::string app_key_;
    std::string region_     = "cn-shanghai";
    std::string token_;
    long token_expire_time_ = -1;

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
    QNetworkAccessManager net_mgr_;
    std::atomic<bool> session_started_{false};
    std::atomic<bool> task_failed_{false};

    std::string task_id_;

    SubtitleCallback subtitle_cb_;

    int sentence_index_ = 0;
    double sentence_begin_ms_ = 0.0;
    std::chrono::steady_clock::time_point session_start_time_;
};
