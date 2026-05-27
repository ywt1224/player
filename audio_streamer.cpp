#include "audio_streamer.h"

#include <iostream>
#include <cstring>

extern "C" {
#include <libavutil/opt.h>
#include <libavutil/mem.h>
#include <libavutil/mathematics.h>
}

AudioStreamer::AudioStreamer(QObject* parent)
    : QObject(parent)
{
    connect(&ws_socket_, &QWebSocket::connected,
            this, &AudioStreamer::onConnected);
    connect(&ws_socket_, &QWebSocket::disconnected,
            this, &AudioStreamer::onDisconnected);
    connect(&ws_socket_, &QWebSocket::textMessageReceived,
            this, &AudioStreamer::onTextMessage);
}

AudioStreamer::~AudioStreamer() {
    Shutdown();
}

bool AudioStreamer::Initialize(int src_rate, int src_channels,
                               AVSampleFormat src_fmt) {
    src_rate_     = src_rate;
    src_channels_ = src_channels;
    src_fmt_      = src_fmt;

    swr_ctx_ = swr_alloc_set_opts(nullptr,
        av_get_default_channel_layout(kDstChannels), AV_SAMPLE_FMT_S16, kDstRate,
        av_get_default_channel_layout(src_channels_), src_fmt_, src_rate_,
        0, nullptr);
    if (!swr_ctx_ || swr_init(swr_ctx_) < 0) {
        std::cerr << "[AudioStreamer] swr_init failed\n";
        return false;
    }

    if (WebRtcVad_Create(&vad_inst_) != 0 || WebRtcVad_Init(vad_inst_) != 0) {
        std::cerr << "[AudioStreamer] VAD init failed\n";
        return false;
    }
    WebRtcVad_set_mode(vad_inst_, 2);

    int opus_err = 0;
    opus_enc_ = opus_encoder_create(kDstRate, kDstChannels,
                                    OPUS_APPLICATION_AUDIO, &opus_err);
    if (opus_err != OPUS_OK || !opus_enc_) {
        std::cerr << "[AudioStreamer] opus_encoder_create failed\n";
        return false;
    }
    opus_encoder_ctl(opus_enc_, OPUS_SET_BITRATE(24000));
    opus_encoder_ctl(opus_enc_, OPUS_SET_COMPLEXITY(5));
    opus_encoder_ctl(opus_enc_, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));

    if (!ws_url_.empty()) {
        ws_socket_.open(QString::fromStdString(ws_url_));
    }

    running_ = true;
    process_thread_ = std::thread(&AudioStreamer::ProcessThreadFunc, this);

    return true;
}

void AudioStreamer::FeedAudioFrame(const uint8_t* const* data, int /*linesize*/,
                                    int nb_samples, int64_t pts_ms,
                                    AVSampleFormat fmt) {
    if (!running_) return;

    int bytes_per_sample = av_get_bytes_per_sample(fmt);
    int planes = av_sample_fmt_is_planar(fmt) ? src_channels_ : 1;
    int plane_size = nb_samples * bytes_per_sample;

    RawFrame frame;
    frame.nb_samples = nb_samples;
    frame.pts_ms     = pts_ms;
    frame.fmt        = fmt;
    frame.buffer.reserve(plane_size * planes);

    for (int p = 0; p < planes; ++p) {
        const uint8_t* plane = data[p];
        if (plane) {
            frame.buffer.insert(frame.buffer.end(), plane, plane + plane_size);
        }
    }

    {
        std::lock_guard<std::mutex> lock(raw_mtx_);
        raw_queue_.push(std::move(frame));
    }
    raw_cv_.notify_one();
}

void AudioStreamer::Shutdown() {
    running_ = false;
    raw_cv_.notify_all();

    if (process_thread_.joinable()) process_thread_.join();

    ws_socket_.close();

    if (opus_enc_) { opus_encoder_destroy(opus_enc_); opus_enc_ = nullptr; }
    if (vad_inst_) { WebRtcVad_Free(vad_inst_);       vad_inst_  = nullptr; }
    if (swr_ctx_)  { swr_free(&swr_ctx_); }
}

// ===== Processing Thread =====

void AudioStreamer::ProcessThreadFunc() {
    std::vector<int16_t> pcm_16k;

    while (running_) {
        RawFrame frame;
        {
            std::unique_lock<std::mutex> lock(raw_mtx_);
            raw_cv_.wait(lock, [this] { return !raw_queue_.empty() || !running_; });
            if (!running_) break;
            frame = std::move(raw_queue_.front());
            raw_queue_.pop();
        }

        if (!ResampleFrame(reinterpret_cast<const uint8_t* const*>(
                               frame.buffer.data()),
                           frame.nb_samples, frame.fmt, pcm_16k)) {
            continue;
        }

        RunVadAndEncode(pcm_16k, frame.pts_ms);
    }
}

bool AudioStreamer::ResampleFrame(const uint8_t* const* src, int src_samples,
                                  AVSampleFormat src_fmt,
                                  std::vector<int16_t>& dst_pcm) {
    int64_t delay = swr_get_delay(swr_ctx_, src_rate_);
    int dst_samples = av_rescale_rnd(delay + src_samples,
                                     kDstRate, src_rate_, AV_ROUND_UP);
    dst_pcm.resize(dst_samples * kDstChannels);

    const uint8_t* src_ptrs[8] = {};
    int bytes_per_sample = av_get_bytes_per_sample(src_fmt);
    int planes = av_sample_fmt_is_planar(src_fmt) ? src_channels_ : 1;
    for (int p = 0; p < planes; ++p) {
        src_ptrs[p] = src[p];
    }

    uint8_t* dst_ptrs[1] = { reinterpret_cast<uint8_t*>(dst_pcm.data()) };
    int converted = swr_convert(swr_ctx_, dst_ptrs, dst_samples,
                                src_ptrs, src_samples);
    if (converted < 0) return false;

    dst_pcm.resize(converted * kDstChannels);
    return true;
}

void AudioStreamer::RunVadAndEncode(const std::vector<int16_t>& pcm,
                                     int64_t base_pts) {
    size_t total_samples = pcm.size();
    size_t offset = 0;

    while (offset + kFrameSamples <= total_samples) {
        const int16_t* frame = pcm.data() + offset;
        int64_t frame_pts = base_pts + static_cast<int64_t>(offset * 1000LL / kDstRate);

        int vad_result = WebRtcVad_Process(vad_inst_, kDstRate, frame, kFrameSamples);
        bool is_speech = (vad_result == 1);

        bool send_vad_end = false;
        if (!is_speech) {
            silence_ms_ += kFrameMs;
            if (silence_ms_ >= kMaxSilenceMs) {
                send_vad_end = true;
                silence_ms_ = 0;
            }
        } else {
            silence_ms_ = 0;
        }

        std::vector<uint8_t> opus_out(kOpusMaxPayload);
        int opus_len = opus_encode(opus_enc_, frame, kFrameSamples,
                                   opus_out.data(),
                                   static_cast<opus_int32>(opus_out.size()));
        if (opus_len > 0) {
            opus_out.resize(opus_len);
            SendPacket(opus_out, frame_pts, is_speech, send_vad_end);
        }

        offset += kFrameSamples;
    }
}

// sendBinaryMessage() is thread-safe in Qt, so this can be called directly
// from the processing thread.
void AudioStreamer::SendPacket(const std::vector<uint8_t>& opus_data,
                                uint64_t pts, bool is_speech, bool vad_end) {
    if (ws_socket_.state() != QAbstractSocket::ConnectedState)
        return;

    size_t total_len = sizeof(NetAudioPacket) + opus_data.size();
    std::vector<uint8_t> buf(total_len);

    auto* hdr = reinterpret_cast<NetAudioPacket*>(buf.data());
    hdr->magic    = kMagic;
    hdr->version  = 1;
    hdr->seq      = seq_counter_++;
    hdr->pts      = pts;
    hdr->opus_len = static_cast<uint16_t>(opus_data.size());
    hdr->flags    = (is_speech ? 1 : 0) | (vad_end ? 2 : 0);
    std::memset(hdr->reserved, 0, sizeof(hdr->reserved));

    std::memcpy(buf.data() + sizeof(NetAudioPacket),
                opus_data.data(), opus_data.size());

    QByteArray payload(reinterpret_cast<const char*>(buf.data()),
                       static_cast<int>(buf.size()));
    ws_socket_.sendBinaryMessage(payload);
}

// ===== WebSocket Callbacks (run on main thread via Qt event loop) =====

void AudioStreamer::onConnected() {
    std::cout << "[AudioStreamer] WebSocket connected\n";
    ws_connected_.store(true);
    emit sigConnected();
}

void AudioStreamer::onDisconnected() {
    std::cout << "[AudioStreamer] WebSocket disconnected\n";
    ws_connected_.store(false);
    emit sigDisconnected();
}

void AudioStreamer::onTextMessage(const QString& message) {
    std::string msg = message.toStdString();
    std::cout << "[AudioStreamer] Recv: " << msg << "\n";
    if (subtitle_cb_) {
        subtitle_cb_(msg);
    }
}
