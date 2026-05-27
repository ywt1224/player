#include "subtitle_manager.h"

#include <QFileInfo>
#include <nlohmann/json.hpp>
using json = nlohmann::json;

SubtitleManager::SubtitleManager() = default;

SubtitleManager::~SubtitleManager() {
    Shutdown();
}

void SubtitleManager::Init(const SubtitleRenderConfig& cfg) {
    cfg_ = cfg;

    QString fontPath = AutoDetectFont();
    if (fontPath.isEmpty()) {
        std::cerr << "[SubtitleManager] ERROR: No CJK font found\n";
    }

    int idMain    = QFontDatabase::addApplicationFont(fontPath);
    int idTrans   = idMain;
    int idInterim = idMain;

    if (idMain != -1) {
        QString family = QFontDatabase::applicationFontFamilies(idMain).value(0);
        fontMain_    = QFont(family, cfg_.fontSizeMain);
        fontTrans_   = QFont(family, cfg_.fontSizeTrans);
        fontInterim_ = QFont(family, cfg_.fontSizeInterim);
    } else {
        fontMain_    = QFont("sans-serif", cfg_.fontSizeMain);
        fontTrans_   = QFont("sans-serif", cfg_.fontSizeTrans);
        fontInterim_ = QFont("sans-serif", cfg_.fontSizeInterim);
    }

    fontMain_.setBold(true);

    initialized_ = true;
    std::cout << "[SubtitleManager] Initialized with font: "
              << fontPath.toStdString() << "\n";
}

void SubtitleManager::Shutdown() {
    std::lock_guard<std::mutex> lock(linesMutex_);
    finalLines_.clear();
    interimText_.clear();
    interimVisible_ = false;
    initialized_ = false;
}

// ===== JSON message dispatch =====

void SubtitleManager::OnMessage(const std::string& jsonMsg) {
    try {
        json j = json::parse(jsonMsg);
        std::string type = j.value("type", "");

        if (type == "subtitle_interim") {
            HandleInterim(j.value("text", ""));
        } else if (type == "subtitle_final") {
            HandleFinal(j.value("text", ""),
                        j.value("begin", 0.0),
                        j.value("end", 0.0),
                        j.value("sentenceId", 0));
        } else if (type == "translation") {
            int sid = j.value("sentenceId", -1);
            if (sid >= 0) {
                HandleTranslation(sid, j.value("translated", ""));
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "[SubtitleManager] JSON parse error: " << e.what() << "\n";
    }
}

void SubtitleManager::HandleInterim(const std::string& text) {
    interimText_      = text;
    interimVisible_   = !text.empty();
    interimLastUpdate_ = std::chrono::steady_clock::now();
}

void SubtitleManager::HandleFinal(const std::string& text, double begin,
                                   double end, int sid) {
    std::lock_guard<std::mutex> lock(linesMutex_);

    auto it = std::find_if(finalLines_.begin(), finalLines_.end(),
        [sid](const SubtitleLine& line) { return line.sentenceId == sid; });
    if (it != finalLines_.end()) {
        it->text    = text;
        it->beginMs = begin;
        it->endMs   = end;
        return;
    }

    SubtitleLine line;
    line.sentenceId = sid;
    line.text       = text;
    line.beginMs    = begin;
    line.endMs      = end;
    line.state      = SubtitleState::HIDDEN;
    line.opacity    = 0.0;
    line.currentY   = static_cast<double>(cfg_.screenH);

    auto pos = std::lower_bound(finalLines_.begin(), finalLines_.end(), line,
        [](const SubtitleLine& a, const SubtitleLine& b) {
            return a.beginMs < b.beginMs;
        });
    finalLines_.insert(pos, std::move(line));
}

void SubtitleManager::HandleTranslation(int sid, const std::string& text) {
    std::lock_guard<std::mutex> lock(linesMutex_);
    for (auto& line : finalLines_) {
        if (line.sentenceId == sid) {
            line.translation = text;
            return;
        }
    }
}

void SubtitleManager::Clear() {
    std::lock_guard<std::mutex> lock(linesMutex_);
    finalLines_.clear();
    interimText_.clear();
    interimVisible_ = false;
}

// ===== Render =====

void SubtitleManager::Render(QPainter* painter, double videoTimeMs,
                              int screenW, int screenH) {
    if (!initialized_ || !painter) return;

    cfg_.screenW = screenW;
    cfg_.screenH = screenH;

    UpdateFinalLines(videoTimeMs);
    UpdateInterim();
    CalculateStackLayout();

    {
        std::lock_guard<std::mutex> lock(linesMutex_);
        for (const auto& line : finalLines_) {
            if (line.opacity <= 0.01) continue;

            float x = cfg_.screenW / 2.0f;
            float y = static_cast<float>(line.currentY);

            DrawTextWithOutline(painter, line.text, x, y,
                                fontMain_, cfg_.colorMain,
                                cfg_.colorOutline,
                                static_cast<float>(line.opacity));

            if (!line.translation.empty()) {
                DrawTextWithOutline(painter, line.translation,
                                    x, y + cfg_.fontSizeMain * 0.9f,
                                    fontTrans_, cfg_.colorTrans,
                                    cfg_.colorOutline,
                                    static_cast<float>(line.opacity * 0.9));
            }
        }
    }

    if (interimVisible_ && !interimText_.empty()) {
        float interimY = cfg_.screenH - cfg_.bottomMargin
                         - cfg_.lineSpacing * 3;
        DrawTextWithOutline(painter, interimText_,
                            cfg_.screenW / 2.0f, interimY,
                            fontInterim_, cfg_.colorInterim,
                            cfg_.colorOutline, 0.75f);
    }
}

// ===== State update =====

void SubtitleManager::UpdateFinalLines(double videoTimeMs) {
    std::lock_guard<std::mutex> lock(linesMutex_);

    for (auto& line : finalLines_) {
        if (videoTimeMs < line.beginMs) {
            line.state   = SubtitleState::HIDDEN;
            line.opacity = 0.0;
        } else if (videoTimeMs < line.beginMs + cfg_.fadeInMs) {
            line.state = SubtitleState::FADE_IN;
            double t = (videoTimeMs - line.beginMs) / cfg_.fadeInMs;
            line.opacity = EaseInCubic(std::clamp(t, 0.0, 1.0));
        } else if (videoTimeMs < line.endMs) {
            line.state   = SubtitleState::VISIBLE;
            line.opacity = 1.0;
        } else if (videoTimeMs < line.endMs + cfg_.fadeOutMs) {
            line.state = SubtitleState::FADE_OUT;
            double t = (videoTimeMs - line.endMs) / cfg_.fadeOutMs;
            line.opacity = 1.0 - EaseOutCubic(std::clamp(t, 0.0, 1.0));
        } else {
            line.state   = SubtitleState::HIDDEN;
            line.opacity = 0.0;
        }
    }

    finalLines_.erase(
        std::remove_if(finalLines_.begin(), finalLines_.end(),
            [videoTimeMs](const SubtitleLine& line) {
                return videoTimeMs > line.endMs + 2000.0;
            }),
        finalLines_.end());
}

void SubtitleManager::UpdateInterim() {
    if (!interimVisible_) return;
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - interimLastUpdate_).count();
    if (elapsed > INTERIM_TIMEOUT_MS) {
        interimVisible_ = false;
    }
}

void SubtitleManager::CalculateStackLayout() {
    std::lock_guard<std::mutex> lock(linesMutex_);
    float currentY = cfg_.screenH - cfg_.bottomMargin;

    for (int i = static_cast<int>(finalLines_.size()) - 1; i >= 0; --i) {
        auto& line = finalLines_[i];
        if (line.opacity < 0.01) {
            line.targetY = cfg_.screenH;
            continue;
        }

        line.targetY = static_cast<double>(currentY);
        currentY -= cfg_.lineSpacing;
        if (!line.translation.empty()) {
            currentY -= cfg_.fontSizeTrans * 0.8f;
        }

        double lerpFactor = 0.25;
        line.currentY += (line.targetY - line.currentY) * lerpFactor;
    }
}

// ===== Text drawing =====

void SubtitleManager::DrawTextWithOutline(QPainter* painter,
                                           const std::string& text,
                                           float x, float y,
                                           const QFont& font,
                                           const QColor& fg,
                                           const QColor& outline,
                                           float opacity) {
    if (text.empty() || !painter) return;

    painter->save();

    // outline: draw offset in 8 directions
    QColor ol = outline;
    ol.setAlpha(static_cast<int>(outline.alpha() * opacity * 0.8f));
    painter->setFont(font);
    painter->setPen(ol);

    QString qtext = QString::fromStdString(text);
    int offset = 2;
    for (int dx = -offset; dx <= offset; ++dx) {
        for (int dy = -offset; dy <= offset; ++dy) {
            if (dx == 0 && dy == 0) continue;
            painter->drawText(static_cast<int>(x + dx),
                              static_cast<int>(y + dy), qtext);
        }
    }

    // fill text
    QColor fgColor = fg;
    fgColor.setAlpha(static_cast<int>(fg.alpha() * opacity));
    painter->setPen(fgColor);
    painter->drawText(static_cast<int>(x), static_cast<int>(y), qtext);

    painter->restore();
}

// ===== Font detection =====

QString SubtitleManager::AutoDetectFont() {
    const char* candidates[] = {
        "C:/Windows/Fonts/simhei.ttf",
        "C:/Windows/Fonts/msyh.ttc",
        "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc",
        "/System/Library/Fonts/PingFang.ttc",
        "/System/Library/Fonts/STHeiti Light.ttc"
    };
    for (const char* path : candidates) {
        QFileInfo fi(path);
        if (fi.exists()) return QString::fromLatin1(path);
    }
    return QString();
}
