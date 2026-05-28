#include "audio_streamer.h"

#include <iostream>
#include <cstring>

extern "C" {
#include <libavutil/opt.h>
#include <libavutil/mem.h>
#include <libavutil/mathematics.h>
}

#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>
#include <QUrlQuery>
// [暂不启用] 动态Token生成依赖，临时Token模式下不需要
// #include <QNetworkRequest>
// #include <QNetworkReply>
// #include <QEventLoop>
// #include <QDateTime>
// #include <QMessageAuthenticationCode>
// #include <QRandomGenerator>
#include <QSslError>

// ===== Construction / Destruction =====

AudioStreamer::AudioStreamer(QObject* parent)
    : QObject(parent)
{
    connect(&ws_socket_, &QWebSocket::connected,
            this, &AudioStreamer::onConnected);
    connect(&ws_socket_, &QWebSocket::disconnected,
            this, &AudioStreamer::onDisconnected);
    connect(&ws_socket_, &QWebSocket::textMessageReceived,
            this, &AudioStreamer::onTextMessage);
    connect(&ws_socket_, QOverload<const QList<QSslError>&>::of(&QWebSocket::sslErrors),
            this, &AudioStreamer::onSslErrors);
}

AudioStreamer::~AudioStreamer() {
    Shutdown();
}

// ===== NLS Config =====
// [暂不启用] 动态Token模式入口，临时Token模式下用SetToken替代
/*
void AudioStreamer::SetNlsConfig(const std::string& ak_id,
                                 const std::string& ak_secret,
                                 const std::string& app_key,
                                 const std::string& region) {
    ak_id_     = ak_id;
    ak_secret_ = ak_secret;
    app_key_   = app_key;
    region_    = region;
}
*/

void AudioStreamer::SetToken(const std::string& token,
                             const std::string& app_key,
                             const std::string& region) {
    token_   = token;
    app_key_ = app_key;
    region_  = region;
}

// ===== Token =====
// [暂不启用] 用AK签名从阿里云HTTP接口获取临时Token
// 需要 QNetworkAccessManager + HMAC-SHA1签名，当前使用硬编码Token，跳过此步骤
/*
bool AudioStreamer::GenerateToken() {
    QString token_url = QString("http://nls-meta.%1.aliyuncs.com/pop/2018-05-18/tokens")
                        .arg(QString::fromStdString(region_));

    QString ak = QString::fromStdString(ak_id_);
    QString sk = QString::fromStdString(ak_secret_);

    QString timestamp = QDateTime::currentDateTimeUtc()
                        .toString("yyyy-MM-ddTHH:mm:ssZ");
    QString nonce = QString::number(QRandomGenerator::global()->generate64());

    QUrlQuery params;
    params.addQueryItem("AccessKeyId", ak);
    params.addQueryItem("Action", "CreateToken");
    params.addQueryItem("Format", "JSON");
    params.addQueryItem("RegionId", QString::fromStdString(region_));
    params.addQueryItem("SignatureMethod", "HMAC-SHA1");
    params.addQueryItem("SignatureNonce", nonce);
    params.addQueryItem("SignatureVersion", "1.0");
    params.addQueryItem("Timestamp", timestamp);
    params.addQueryItem("Version", "2018-05-18");

    QString canonical = params.toString(QUrl::FullyDecoded);
    QString string_to_sign = "GET&%2F&"
                           + QString(QUrl::toPercentEncoding(canonical));

    QByteArray signature = QMessageAuthenticationCode::hash(
        string_to_sign.toUtf8(), (sk + "&").toUtf8(),
        QCryptographicHash::Sha1);

    QString sig_encoded = QString(QUrl::toPercentEncoding(
        signature.toBase64()));

    QUrl url(token_url);
    QUrlQuery final_q(canonical);
    final_q.addQueryItem("Signature", sig_encoded);
    url.setQuery(final_q);

    QNetworkRequest request(url);
    QNetworkReply* reply = net_mgr_.get(request);

    QEventLoop loop;
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    if (reply->error() != QNetworkReply::NoError) {
        std::cerr << "[AudioStreamer] Token request failed: "
                  << reply->errorString().toStdString() << "\n";
        reply->deleteLater();
        return false;
    }

    QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
    reply->deleteLater();

    QJsonObject obj = doc.object();
    if (obj.contains("Token")) {
        QJsonObject t = obj["Token"].toObject();
        token_ = t["Id"].toString().toStdString();
        token_expire_time_ = static_cast<long>(t["ExpireTime"].toDouble());
        std::cout << "[AudioStreamer] Token obtained, expires in "
                  << (token_expire_time_ - std::time(0)) << "s\n";
        return true;
    }

    std::cerr << "[AudioStreamer] Unexpected token response\n";
    return false;
}
*/

// ===== NLS Connection =====

void AudioStreamer::ConnectToNls() {
    std::string host = "nls-gateway-" + region_ + ".aliyuncs.com";

    QUrl url;
    url.setScheme("wss");
    url.setHost(QString::fromStdString(host));
    url.setPath("/stream/v1/asr");

    QUrlQuery query;
    query.addQueryItem("token", QString::fromStdString(token_));
    url.setQuery(query);

    std::cout << "[AudioStreamer] Connecting to NLS: " << host << "\n";
    ws_socket_.open(url);
}

void AudioStreamer::SendStartCommand() {
    task_id_ = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());

    QJsonObject header;
    header["name"]      = QStringLiteral("StartTranscription");
    header["namespace"] = QStringLiteral("SpeechRecognizer");
    header["task_id"]   = QString::fromStdString(task_id_);

    QJsonObject payload;
    payload["format"]                        = QStringLiteral("pcm");
    payload["sample_rate"]                   = kDstRate;
    payload["enable_intermediate_result"]    = true;
    payload["enable_punctuation_prediction"] = true;
    payload["enable_inverse_text_normalization"] = true;
    // payload["max_sentence_silence"] = kMaxSilenceMs;  // [暂不启用] 需要kMaxSilenceMs常量

    QJsonObject cmd;
    cmd["header"]  = header;
    cmd["payload"] = payload;

    QJsonDocument doc(cmd);
    ws_socket_.sendTextMessage(doc.toJson(QJsonDocument::Compact));
    std::cout << "[AudioStreamer] StartTranscription sent\n";
}

void AudioStreamer::SendStopCommand() {
    if (!session_started_) return;

    QJsonObject header;
    header["name"]      = QStringLiteral("StopTranscription");
    header["namespace"] = QStringLiteral("SpeechRecognizer");
    header["task_id"]   = QString::fromStdString(task_id_);

    QJsonObject cmd;
    cmd["header"]  = header;
    cmd["payload"] = QJsonObject();

    QJsonDocument doc(cmd);
    ws_socket_.sendTextMessage(doc.toJson(QJsonDocument::Compact));
    std::cout << "[AudioStreamer] StopTranscription sent\n";
}

// ===== Initialize / Shutdown =====

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

    if (!vad_.Init(kDstRate, kFrameSamples)) {
        std::cerr << "[AudioStreamer] VAD init failed\n";
        return false;
    }

    // [暂不启用] 动态Token获取，使用SetToken预先设置的硬编码Token
    // if (token_.empty()) {
    //     if (!GenerateToken()) return false;
    // }

    running_ = true;
    process_thread_ = std::thread(&AudioStreamer::ProcessThreadFunc, this);

    ConnectToNls();

    return true;
}

void AudioStreamer::Shutdown() {
    SendStopCommand();

    running_ = false;
    raw_cv_.notify_all();

    if (process_thread_.joinable()) process_thread_.join();

    ws_socket_.close();
    if (swr_ctx_) { swr_free(&swr_ctx_); }
}

// ===== Feed Audio (called from decoder thread) =====

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

        // Send PCM directly to NLS (no Opus encoding)
        if (session_started_ && !task_failed_) {
            int bytes = static_cast<int>(pcm_16k.size()) * sizeof(int16_t);
            QByteArray audio(reinterpret_cast<const char*>(pcm_16k.data()), bytes);
            ws_socket_.sendBinaryMessage(audio);
        }
    }
}

// ===== Resample =====

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

// ===== WebSocket Callbacks =====

void AudioStreamer::onConnected() {
    std::cout << "[AudioStreamer] NLS WSS connected\n";
    SendStartCommand();
}

void AudioStreamer::onDisconnected() {
    std::cout << "[AudioStreamer] NLS WSS disconnected\n";
    session_started_ = false;
    emit sigDisconnected();
}

void AudioStreamer::onSslErrors(const QList<QSslError>& errors) {
    for (const auto& e : errors) {
        std::cerr << "[AudioStreamer] SSL error: "
                  << e.errorString().toStdString() << "\n";
    }
    ws_socket_.ignoreSslErrors();
}

void AudioStreamer::onTextMessage(const QString& message) {
    std::string msg = message.toStdString();
    std::cout << "[AudioStreamer] NLS recv: " << msg.substr(0, 200) << "\n";
    ParseNlsResult(msg);
}

// ===== Parse NLS Result → Subtitle JSON =====

void AudioStreamer::ParseNlsResult(const std::string& nls_json) {
    QJsonDocument doc = QJsonDocument::fromJson(
        QByteArray::fromStdString(nls_json));
    QJsonObject root = doc.object();
    QJsonObject header = root["header"].toObject();
    QString name = header["name"].toString();

    if (name == "TranscriptionStarted") {
        std::cout << "[AudioStreamer] Transcription started\n";
        session_started_ = true;
        // sentence_begin_ms_ = 0.0;  // [暂不启用]
        session_start_time_ = std::chrono::steady_clock::now();
        emit sigConnected();

    } else if (name == "TranscriptionResultChanged") {
        QJsonObject payload = root["payload"].toObject();
        QString text = payload["result"].toString();
        if (text.isEmpty()) return;

        QJsonObject sub;
        sub["type"] = QStringLiteral("subtitle_interim");
        sub["text"] = text;

        QJsonDocument out(sub);
        std::string json = out.toJson(QJsonDocument::Compact).toStdString();
        if (subtitle_cb_) subtitle_cb_(json);

    } else if (name == "SentenceEnd") {
        QJsonObject payload = root["payload"].toObject();
        QString text = payload["result"].toString();
        if (text.isEmpty()) return;

        int begin_time = payload["begin_time"].toInt();
        int end_time   = payload["end_time"].toInt();
        sentence_index_++;

        QJsonObject sub;
        sub["type"]       = QStringLiteral("subtitle_final");
        sub["text"]       = text;
        sub["begin"]      = static_cast<double>(begin_time);
        sub["end"]        = static_cast<double>(end_time);
        sub["sentenceId"] = sentence_index_;

        QJsonDocument out(sub);
        std::string json = out.toJson(QJsonDocument::Compact).toStdString();
        if (subtitle_cb_) subtitle_cb_(json);

        std::cout << "[AudioStreamer] SentenceEnd: " << text.toStdString()
                  << " [" << begin_time << "-" << end_time << "ms]\n";

    } else if (name == "TranscriptionCompleted") {
        std::cout << "[AudioStreamer] Transcription completed\n";
        session_started_ = false;

    } else if (name == "TaskFailed") {
        std::cerr << "[AudioStreamer] TaskFailed: "
                  << root["payload"].toObject()["error_message"].toString().toStdString()
                  << "\n";
        task_failed_ = true;
        session_started_ = false;
        emit sigDisconnected();
    }
}
