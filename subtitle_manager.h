#pragma once

#include <string>
#include <vector>
#include <deque>
#include <mutex>
#include <chrono>
#include <cmath>
#include <algorithm>
#include <iostream>

#include <QPainter>
#include <QFont>
#include <QColor>
#include <QFontDatabase>

// Mirror the whisper protocol: subtitle state machine
enum class SubtitleState {
    HIDDEN,
    FADE_IN,
    VISIBLE,
    FADE_OUT
};

struct SubtitleLine {
    int sentenceId = 0;
    std::string text;
    std::string translation;
    double beginMs = 0;
    double endMs   = 0;
    SubtitleState state = SubtitleState::HIDDEN;
    double opacity  = 0.0;
    double targetY  = 0.0;
    double currentY = 0.0;
};

struct SubtitleRenderConfig {
    int screenW = 1920;
    int screenH = 1080;
    int fontSizeMain    = 28;
    int fontSizeTrans   = 20;
    int fontSizeInterim = 24;
    QColor colorMain    {255, 255, 255, 255};
    QColor colorInterim {200, 200, 200, 180};
    QColor colorTrans   {255, 220, 100, 220};
    QColor colorOutline {0, 0, 0, 220};
    float fadeInMs   = 200.0f;
    float fadeOutMs  = 500.0f;
    float lineSpacing   = 36.0f;
    float bottomMargin  = 60.0f;
};

class SubtitleManager {
public:
    SubtitleManager();
    ~SubtitleManager();

    void Init(const SubtitleRenderConfig& cfg);
    void Shutdown();
    void OnMessage(const std::string& jsonMsg);
    void Render(QPainter* painter, double videoTimeMs, int screenW, int screenH);
    void Clear();

private:
    void HandleInterim(const std::string& text);
    void HandleFinal(const std::string& text, double begin, double end, int sid);
    void HandleTranslation(int sid, const std::string& text);
    void UpdateFinalLines(double videoTimeMs);
    void UpdateInterim();
    void CalculateStackLayout();
    void DrawTextWithOutline(QPainter* painter, const std::string& text,
                             float x, float y, const QFont& font,
                             const QColor& fg, const QColor& outline,
                             float opacity);

    static double EaseInCubic(double t)  { return t * t * t; }
    static double EaseOutCubic(double t) { double inv = 1.0 - t; return 1.0 - inv * inv * inv; }
    static QString AutoDetectFont();

    SubtitleRenderConfig cfg_;
    QFont fontMain_;
    QFont fontTrans_;
    QFont fontInterim_;

    std::mutex linesMutex_;
    std::deque<SubtitleLine> finalLines_;
    int nextId_ = 1;

    std::string interimText_;
    bool interimVisible_ = false;
    std::chrono::steady_clock::time_point interimLastUpdate_;
    static constexpr int INTERIM_TIMEOUT_MS = 600;

    bool initialized_ = false;
};
