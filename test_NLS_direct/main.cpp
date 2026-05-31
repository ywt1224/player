// test_NLS_direct — 独立测试 NLS 直连的简化程序
// 流程: 解复用 → 解码音频 → 重采样16kHz → WSS发PCM → 接收并打印结果
//
// 用法: test_NLS_direct <媒体文件> <NLS_Token> <NLS_AppKey>
// 示例: test_NLS_direct test.mp4 xxxxxxxxxxxxxxxxxxxxxxxx w8bHbUQmJg2Ei94F

#include <iostream>
#include <cstring>
#include <chrono>
#include <thread>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

#include <QCoreApplication>
#include <QWebSocket>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>
#include <QUrlQuery>
#include <QSslError>
#include <QTimer>

static const int kDstRate    = 16000;
static const int kDstChannels = 1;
static const char* kRegion   = "cn-shanghai";

// ===== 全局变量 =====
QWebSocket g_ws;
bool       g_sessionStarted = false;
bool       g_done           = false;
QString    g_token;
QString    g_appKey;

// ===== NLS WebSocket 回调 =====

static void onConnected() {
    std::cout << "[TEST] NLS WSS connected\n";

    QJsonObject header;
    header["name"]      = QStringLiteral("StartTranscription");
    header["namespace"] = QStringLiteral("SpeechRecognizer");
    header["task_id"]   = QStringLiteral("test_001");

    QJsonObject payload;
    payload["format"]                     = QStringLiteral("pcm");
    payload["sample_rate"]                = kDstRate;
    payload["enable_intermediate_result"] = true;
    payload["enable_punctuation_prediction"] = true;

    QJsonObject cmd;
    cmd["header"]  = header;
    cmd["payload"] = payload;

    QJsonDocument doc(cmd);
    g_ws.sendTextMessage(doc.toJson(QJsonDocument::Compact));
    std::cout << "[TEST] StartTranscription sent\n";
}

static void onDisconnected() {
    std::cout << "[TEST] NLS WSS disconnected\n";
    g_sessionStarted = false;
    QCoreApplication::quit();
}

static void onTextMessage(const QString& msg) {
    std::string text = msg.toStdString();
    std::cout << "[TEST] NLS recv: " << text << "\n";

    QJsonDocument doc = QJsonDocument::fromJson(msg.toUtf8());
    QJsonObject root = doc.object();
    QString name = root["header"].toObject()["name"].toString();

    if (name == "TranscriptionStarted") {
        std::cout << "[TEST] *** 识别已开始 ***\n";
        g_sessionStarted = true;

    } else if (name == "TranscriptionResultChanged") {
        QString result = root["payload"].toObject()["result"].toString();
        std::cout << "[TEST] [中间结果] " << result.toStdString() << "\n";

    } else if (name == "SentenceEnd") {
        QJsonObject payload = root["payload"].toObject();
        QString result = payload["result"].toString();
        int bt = payload["begin_time"].toInt();
        int et = payload["end_time"].toInt();
        std::cout << "[TEST] *** [最终] [" << bt << "-" << et << "ms] "
                  << result.toStdString() << " ***\n";

    } else if (name == "TranscriptionCompleted") {
        std::cout << "[TEST] 识别完成\n";
        g_sessionStarted = false;

    } else if (name == "TaskFailed") {
        std::cerr << "[TEST] 识别失败: "
                  << root["payload"].toObject()["error_message"].toString().toStdString()
                  << "\n";
        QCoreApplication::quit();
    }
}

static void onSslErrors(const QList<QSslError>& errors) {
    for (const auto& e : errors)
        std::cerr << "[TEST] SSL error: " << e.errorString().toStdString() << "\n";
    g_ws.ignoreSslErrors();
}

// ===== 连接 NLS =====

static void connectToNls() {
    QString host = QString("nls-gateway-%1.aliyuncs.com").arg(kRegion);
    QUrl url;
    url.setScheme("wss");
    url.setHost(host);
    url.setPath("/stream/v1/asr");
    QUrlQuery query;
    query.addQueryItem("token", g_token);
    url.setQuery(query);

    std::cout << "[TEST] Connecting to " << host.toStdString() << " ...\n";

    QObject::connect(&g_ws, &QWebSocket::connected,           onConnected);
    QObject::connect(&g_ws, &QWebSocket::disconnected,        onDisconnected);
    QObject::connect(&g_ws, &QWebSocket::textMessageReceived, onTextMessage);
    QObject::connect(&g_ws,
        QOverload<const QList<QSslError>&>::of(&QWebSocket::sslErrors), onSslErrors);
    QObject::connect(&g_ws,
        QOverload<QAbstractSocket::SocketError>::of(&QWebSocket::error),
        [](QAbstractSocket::SocketError) {
            std::cerr << "[TEST] Socket error: "
                      << g_ws.errorString().toStdString() << "\n";
        });

    g_ws.open(url);
}

// ===== 初始化解码器 =====

static bool openAudio(const char* filename,
                      AVFormatContext*& fmtCtx, AVCodecContext*& decCtx,
                      SwrContext*& swrCtx, int audioStreamIdx) {
    // 打开文件
    int ret = avformat_open_input(&fmtCtx, filename, nullptr, nullptr);
    if (ret < 0) { std::cerr << "avformat_open_input failed\n"; return false; }
    ret = avformat_find_stream_info(fmtCtx, nullptr);
    if (ret < 0) { std::cerr << "avformat_find_stream_info failed\n"; return false; }

    // 找音频流
    audioStreamIdx = av_find_best_stream(fmtCtx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (audioStreamIdx < 0) { std::cerr << "No audio stream found\n"; return false; }

    AVStream* st = fmtCtx->streams[audioStreamIdx];
    std::cout << "[TEST] Audio: " << st->codecpar->sample_rate << "Hz, "
              << st->codecpar->channels << "ch\n";

    // 打开解码器
    const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec) { std::cerr << "Codec not found\n"; return false; }
    decCtx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(decCtx, st->codecpar);
    ret = avcodec_open2(decCtx, codec, nullptr);
    if (ret < 0) { std::cerr << "avcodec_open2 failed\n"; return false; }

    // 重采样器: 原始 → 16kHz mono S16
    #if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(60, 0, 0)
        // FFmpeg 5.x+ 新 API
        AVChannelLayout out_ch_layout = AV_CHANNEL_LAYOUT_MONO;
        AVChannelLayout in_ch_layout  = AV_CHANNEL_LAYOUT_STEREO;
        av_channel_layout_default(&in_ch_layout, decCtx->ch_layout.nb_channels);
        swrCtx = nullptr;
        swr_alloc_set_opts2(&swrCtx,
            &out_ch_layout, AV_SAMPLE_FMT_S16, kDstRate,
            &in_ch_layout, decCtx->sample_fmt, decCtx->sample_rate,
            0, nullptr);
    #else
        // FFmpeg 4.x 旧 API
        swrCtx = swr_alloc_set_opts(nullptr,
            av_get_default_channel_layout(kDstChannels),
            AV_SAMPLE_FMT_S16, kDstRate,
            av_get_default_channel_layout(decCtx->channels),
            decCtx->sample_fmt, decCtx->sample_rate, 0, nullptr);
    #endif
    swr_init(swrCtx);

    return true;
}

// ===== 发送 PCM 帧 =====

static bool sendPcmFrame(SwrContext* swrCtx, AVFrame* frame) {
    if (!g_sessionStarted) return true;

    // 计算重采样后的样本数
    int64_t delay = swr_get_delay(swrCtx, frame->sample_rate);
    int dstSamples = av_rescale_rnd(delay + frame->nb_samples,
                                    kDstRate, frame->sample_rate, AV_ROUND_UP);

    std::vector<int16_t> pcm(dstSamples * kDstChannels);
    uint8_t* dstPtr = reinterpret_cast<uint8_t*>(pcm.data());
    int converted = swr_convert(swrCtx, &dstPtr, dstSamples,
                                (const uint8_t**)frame->data, frame->nb_samples);
    if (converted < 0) return false;

    int bytes = converted * kDstChannels * sizeof(int16_t);
    QByteArray audio(reinterpret_cast<const char*>(pcm.data()), bytes);
    g_ws.sendBinaryMessage(audio);

    // 计算发送后应休眠的时间 (模拟实时发送速率)
    int sleepMs = (converted * 1000) / kDstRate;
    if (sleepMs > 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));

    return true;
}

// ===== 主函数 =====

int main(int argc, char* argv[]) {
    if (argc < 4) {
        std::cerr << "用法: " << argv[0]
                  << " <媒体文件> <NLS_Token> <NLS_AppKey>\n\n"
                  << "示例: " << argv[0]
                  << " test.mp4 xxxxxxxxxxxxxxxxxxxxxxxx w8bHbUQmJg2Ei94F\n";
        return 1;
    }

    const char* file   = argv[1];
    g_token  = QString(argv[2]);
    g_appKey = QString(argv[3]);

    QCoreApplication app(argc, argv);

    // --- 初始化 FFmpeg ---
    av_register_all();  // FFmpeg 4.2 仍建议调用

    AVFormatContext* fmtCtx = nullptr;
    AVCodecContext*  decCtx = nullptr;
    SwrContext*      swrCtx = nullptr;
    int audioIdx = -1;

    if (!openAudio(file, fmtCtx, decCtx, swrCtx, audioIdx)) {
        std::cerr << "[TEST] 音频初始化失败\n";
        return 1;
    }

    // --- 连接 NLS ---
    connectToNls();

    // --- 主循环: 读包 → 解码 → 重采样 → 发送 ---
    AVPacket pkt;
    AVFrame* frame = av_frame_alloc();

    // 用 QTimer 在主循环中驱动 (避免阻塞事件循环)
    QTimer timer;
    QObject::connect(&timer, &QTimer::timeout, [&]() {
        if (g_done) { timer.stop(); QCoreApplication::quit(); return; }

        // 尝试读取 packet
        int ret = av_read_frame(fmtCtx, &pkt);
        if (ret < 0) {
            if (ret == AVERROR_EOF) {
                std::cout << "[TEST] 文件读取完毕\n";
                g_ws.close();
                g_done = true;
            }
            return;
        }

        if (pkt.stream_index != audioIdx) {
            av_packet_unref(&pkt);
            return;
        }

        // 发送到解码器
        ret = avcodec_send_packet(decCtx, &pkt);
        av_packet_unref(&pkt);
        if (ret < 0) return;

        // 接收解码帧
        while (avcodec_receive_frame(decCtx, frame) >= 0) {
            sendPcmFrame(swrCtx, frame);
            av_frame_unref(frame);
        }
    });

    timer.start(5);  // 每 5ms 驱动一次

    int result = app.exec();

    // --- 清理 ---
    av_frame_free(&frame);
    avcodec_free_context(&decCtx);
    avformat_close_input(&fmtCtx);
    swr_free(&swrCtx);

    return result;
}
