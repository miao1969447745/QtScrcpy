/*
 * Added by the ScrcpyGUI derivative project in 2026.
 * Licensed under the Apache License, Version 2.0.
 *
 * The overlap matcher is an independent Qt implementation informed by the
 * public design ideas in the MIT-licensed scrollshot and screenStitch projects.
 */
#include "longscreenshot.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include <QApplication>
#include <QClipboard>
#include <QInputDialog>
#include <QMessageBox>
#include <QPainter>
#include <QProgressDialog>
#include <QTimer>

#include "videoform.h"

namespace {
constexpr int kSwipeSteps = 12;
constexpr int kSwipeStepDelayMs = 35;
constexpr int kSettlePollDelayMs = 80;
constexpr int kMaxSettlePolls = 12;
constexpr int kNoChangePolls = 6;
constexpr int kMaximumOutputHeight = 60000;

inline double gray(QRgb pixel)
{
    return qRed(pixel) * 0.299 + qGreen(pixel) * 0.587 + qBlue(pixel) * 0.114;
}
}

LongScreenshotController::LongScreenshotController(VideoForm *videoForm)
    : QObject(videoForm)
    , m_videoForm(videoForm)
{
}

void LongScreenshotController::start()
{
    if (m_active || !m_videoForm) {
        return;
    }
    if (!m_videoForm->isLongScreenshotReady()) {
        QMessageBox::warning(m_videoForm, tr("长截图"),
                             tr("当前视频画面尚未就绪，或正在使用不支持抓帧的渲染模式。"));
        return;
    }

    bool accepted = false;
    const int maxFrames = QInputDialog::getInt(
        m_videoForm, tr("电脑兼容拼接长截图"),
        tr("最大拼接屏数（请先把页面滚动到顶部）："), 99, 2, 99, 1, &accepted);
    if (!accepted) {
        return;
    }

    begin(maxFrames, false);
}

bool LongScreenshotController::startRemote(int maxFrames)
{
    if (m_active || !m_videoForm || !m_videoForm->isLongScreenshotReady()
            || m_videoForm->pluginFrame().isNull() || maxFrames < 2 || maxFrames > 99) return false;
    begin(maxFrames, true);
    return true;
}

void LongScreenshotController::schedule(int delay, std::function<void()> callback)
{
    const quint64 generation = m_runGeneration;
    QTimer::singleShot(delay, this, [this, generation, callback]() {
        if (m_active && generation == m_runGeneration) callback();
    });
}

void LongScreenshotController::begin(int maxFrames, bool remote)
{
    m_remote = remote;
    ++m_runGeneration;
    m_epoch = m_videoForm->pluginCaptureEpoch();
    m_fingerDown = false;
    m_active = true;
    m_cancelled = false;
    m_maxFrames = maxFrames;
    m_swipeStep = 0;
    m_settlePolls = 0;
    m_stagnantCaptures = 0;
    m_changedAfterSwipe = false;
    m_frames.clear();
    m_beforeSwipe = QImage();
    m_lastProbe = QImage();

    if (!remote) {
    m_progress = new QProgressDialog(tr("正在读取第一屏…"), tr("取消"), 0, m_maxFrames,
                                     m_videoForm);
    m_progress->setWindowTitle(tr("电脑兼容拼接长截图"));
    m_progress->setWindowModality(Qt::WindowModal);
    m_progress->setAutoClose(false);
    m_progress->setAutoReset(false);
    m_progress->setMinimumDuration(0);
    connect(m_progress, &QProgressDialog::canceled, this, [this]() { m_cancelled = true; });
    m_progress->show();
    }

    schedule(remote ? 0 : 120, [this]() { captureInitialFrame(); });
}

void LongScreenshotController::releaseFinger()
{
    if (!m_fingerDown || !m_videoForm) return;
    m_fingerDown = false;
    if (m_remote) {
        const QSize size = m_videoForm->frameSize();
        m_videoForm->injectPluginTouch("up", size.width() / 2, qMax(0, size.height() / 2));
    } else {
        m_videoForm->injectLongScreenshotTouch(QEvent::MouseButtonRelease, 0.5, 0.5, Qt::LeftButton, Qt::NoButton);
    }
}

void LongScreenshotController::cancel()
{
    if (!m_active) return;
    m_cancelled = true;
    abort(QString(), false);
}

void LongScreenshotController::captureInitialFrame()
{
    if (isCancelled()) {
        abort(QString(), false);
        return;
    }

    QImage frame = captureFrame();
    if (frame.isNull()) {
        abort(tr("无法从当前视频窗口读取画面。"));
        return;
    }
    m_frames.append(frame);
    if (m_progress) {
        m_progress->setValue(1);
    }
    beginSwipe();
}

void LongScreenshotController::beginSwipe()
{
    if (isCancelled()) {
        abort(QString(), false);
        return;
    }
    if (!m_videoForm || m_frames.isEmpty()) {
        abort(tr("设备连接已经中断。"));
        return;
    }

    m_beforeSwipe = m_frames.constLast();
    m_lastProbe = QImage();
    m_swipeStep = 0;
    m_settlePolls = 0;
    m_changedAfterSwipe = false;
    updateProgress(tr("正在滑动并等待画面稳定（第 %1 屏）…").arg(m_frames.size() + 1));
    sendSwipeStep();
}

void LongScreenshotController::sendSwipeStep()
{
    if (isCancelled()) {
        abort(QString(), false);
        return;
    }
    if (!m_videoForm) {
        abort(tr("设备窗口已经关闭。"));
        return;
    }

    const qreal startY = 0.85;
    const qreal endY = startY - m_swipePercent / 100.0;
    bool sent = false;
    if (m_remote) {
        if (m_videoForm->pluginCaptureEpoch() != m_epoch) { abort(tr("截图期间采集会话发生变化。")); return; }
        const QSize size = m_videoForm->frameSize();
        const qreal progress = qMin(m_swipeStep, kSwipeSteps) / static_cast<qreal>(kSwipeSteps);
        const QString action = m_swipeStep == 0 ? "down" : (m_swipeStep <= kSwipeSteps ? "move" : "up");
        sent = m_videoForm->injectPluginTouch(action, size.width() / 2,
                   qBound(0, qRound((startY + (endY - startY) * progress) * size.height()), size.height() - 1));
        if (sent && action == "down") m_fingerDown = true;
        if (action == "up") m_fingerDown = false;
        if (!sent) { abort(tr("无法向设备发送滑动操作。")); return; }
        if (m_swipeStep > kSwipeSteps) {
            schedule(kSettlePollDelayMs, [this]() { pollForSettledFrame(); });
            return;
        }
        ++m_swipeStep;
        schedule(kSwipeStepDelayMs, [this]() { sendSwipeStep(); });
        return;
    }
    if (m_swipeStep == 0) {
        sent = m_videoForm->injectLongScreenshotTouch(
            QEvent::MouseButtonPress, 0.5, startY, Qt::LeftButton, Qt::LeftButton);
        m_fingerDown = sent;
    } else if (m_swipeStep <= kSwipeSteps) {
        const qreal progress = m_swipeStep / static_cast<qreal>(kSwipeSteps);
        const qreal y = startY + (endY - startY) * progress;
        sent = m_videoForm->injectLongScreenshotTouch(
            QEvent::MouseMove, 0.5, y, Qt::NoButton, Qt::LeftButton);
    } else {
        sent = m_videoForm->injectLongScreenshotTouch(
            QEvent::MouseButtonRelease, 0.5, endY, Qt::LeftButton, Qt::NoButton);
        m_fingerDown = false;
        if (!sent) {
            abort(tr("无法向设备发送滑动操作。"));
            return;
        }
        schedule(kSettlePollDelayMs, [this]() { pollForSettledFrame(); });
        return;
    }

    if (!sent) {
        abort(tr("无法向设备发送滑动操作。"));
        return;
    }
    ++m_swipeStep;
    schedule(kSwipeStepDelayMs, [this]() { sendSwipeStep(); });
}

void LongScreenshotController::pollForSettledFrame()
{
    if (isCancelled()) {
        abort(QString(), false);
        return;
    }

    QImage current = captureFrame();
    if (current.isNull()) {
        abort(tr("等待滚动停止时无法读取视频画面。"));
        return;
    }
    if (current.size() != m_beforeSwipe.size()) {
        abort(tr("截图过程中设备方向或画面尺寸发生了变化，请保持方向不变后重试。"));
        return;
    }

    ++m_settlePolls;
    if (sampledImageDifference(m_beforeSwipe, current) > 1.5) {
        m_changedAfterSwipe = true;
    }

    const bool visuallySettled = m_changedAfterSwipe && !m_lastProbe.isNull()
        && sampledImageDifference(m_lastProbe, current) <= 0.9;
    const bool reachedGuard = m_settlePolls >= kMaxSettlePolls;
    const bool noMovementGuard = !m_changedAfterSwipe && m_settlePolls >= kNoChangePolls;
    if (visuallySettled || reachedGuard || noMovementGuard) {
        acceptPostSwipeFrame(current);
        return;
    }

    m_lastProbe = current;
    schedule(kSettlePollDelayMs, [this]() { pollForSettledFrame(); });
}

void LongScreenshotController::acceptPostSwipeFrame(const QImage &frame)
{
    if (framesNearStagnant(m_frames.constLast(), frame)) {
        ++m_stagnantCaptures;
        if (m_stagnantCaptures >= 2) {
            finish(true);
            return;
        }
        updateProgress(tr("画面没有继续移动，正在确认是否已到达底部…"));
        schedule(80, [this]() { beginSwipe(); });
        return;
    }

    m_stagnantCaptures = 0;
    m_frames.append(frame);
    if (m_progress) {
        m_progress->setValue(m_frames.size());
    }
    if (m_frames.size() >= m_maxFrames) {
        finish(false);
        return;
    }
    schedule(80, [this]() { beginSwipe(); });
}

void LongScreenshotController::finish(bool reachedBottom)
{
    if (isCancelled()) {
        abort(QString(), false);
        return;
    }
    updateProgress(tr("正在识别固定顶部、固定底部和重叠区域…"));
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const StitchResult result = stitchFrames(m_frames, m_swipePercent, reachedBottom);
    QApplication::restoreOverrideCursor();

    if (result.image.isNull()) {
        abort(tr("长图生成失败，可能是页面过长或可用内存不足。"));
        return;
    }

    QApplication::clipboard()->setImage(result.image);
    const int frameCount = m_frames.size();
    const QSize resultSize = result.image.size();
    QString message = tr("长截图已复制到系统剪贴板。\n\n"
                         "拼接屏数：%1\n图片尺寸：%2 × %3\n"
                         "固定顶部：%4 px\n固定底部：%5 px")
                          .arg(frameCount)
                          .arg(resultSize.width())
                          .arg(resultSize.height())
                          .arg(result.fixedTop)
                          .arg(result.fixedBottom);
    if (!reachedBottom) {
        message += tr("\n\n已达到最大拼接屏数，页面可能仍有更多内容。");
    }
    if (!result.warnings.isEmpty()) {
        message += tr("\n\n提示：") + result.warnings.join(QLatin1Char(' '));
    }

    if (m_progress) {
        m_progress->close();
        m_progress->deleteLater();
    }
    m_progress = nullptr;
    m_frames.clear();
    m_active = false;
    ++m_runGeneration;
    emit finished({{"state", "completed"}, {"clipboard_written", true}, {"saved_file", false},
                   {"frames", frameCount}, {"width", resultSize.width()}, {"height", resultSize.height()},
                   {"fixed_top", result.fixedTop}, {"fixed_bottom", result.fixedBottom},
                   {"reached_bottom", reachedBottom}, {"message", message}});
    if (!m_remote) QMessageBox::information(m_videoForm, tr("长截图完成"), message);
}

void LongScreenshotController::abort(const QString &message, bool showMessage)
{
    releaseFinger();
    if (m_progress) {
        m_progress->close();
        m_progress->deleteLater();
    }
    m_progress = nullptr;
    m_frames.clear();
    m_beforeSwipe = QImage();
    m_lastProbe = QImage();
    m_active = false;
    ++m_runGeneration;
    emit finished({{"state", message.isEmpty() ? "cancelled" : "failed"}, {"error", message}, {"clipboard_written", false}});
    if (!m_remote && showMessage && !message.isEmpty() && m_videoForm) {
        QMessageBox::warning(m_videoForm, tr("长截图"), message);
    }
}

bool LongScreenshotController::isCancelled() const
{
    return m_cancelled || !m_videoForm || (m_remote && m_videoForm->pluginCaptureEpoch() != m_epoch);
}

QImage LongScreenshotController::captureFrame() const
{
    if (!m_videoForm) {
        return QImage();
    }
    return (m_remote ? m_videoForm->pluginFrame() : m_videoForm->grabVideoFrame()).convertToFormat(QImage::Format_RGB32);
}

void LongScreenshotController::updateProgress(const QString &message)
{
    if (m_progress) {
        m_progress->setLabelText(message);
    }
}

QImage LongScreenshotController::scaleForCompare(const QImage &source, int width, int height)
{
    return source.scaled(width, height, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
        .convertToFormat(QImage::Format_RGB32);
}

double LongScreenshotController::sampledRowDifference(const QImage &previous, const QImage &next,
                                                       int previousY, int nextY)
{
    if (previous.isNull() || next.isNull() || previous.width() != next.width()) {
        return std::numeric_limits<double>::max();
    }
    const QRgb *previousLine = reinterpret_cast<const QRgb *>(previous.constScanLine(previousY));
    const QRgb *nextLine = reinterpret_cast<const QRgb *>(next.constScanLine(nextY));
    const int startX = previous.width() * 5 / 100;
    const int endX = previous.width() * 94 / 100;
    const int stepX = qMax(2, previous.width() / 48);
    qint64 difference = 0;
    qint64 samples = 0;
    for (int x = startX; x < endX; x += stepX) {
        const QRgb a = previousLine[x];
        const QRgb b = nextLine[x];
        difference += qAbs(qRed(a) - qRed(b));
        difference += qAbs(qGreen(a) - qGreen(b));
        difference += qAbs(qBlue(a) - qBlue(b));
        samples += 3;
    }
    return samples == 0 ? std::numeric_limits<double>::max()
                        : difference / static_cast<double>(samples);
}

double LongScreenshotController::sampledImageDifference(const QImage &previous, const QImage &next)
{
    if (previous.isNull() || next.isNull() || previous.size() != next.size()) {
        return std::numeric_limits<double>::max();
    }
    const int sampleWidth = 96;
    const int sampleHeight = qMax(80, previous.height() * sampleWidth / previous.width());
    const QImage a = scaleForCompare(previous, sampleWidth, sampleHeight);
    const QImage b = scaleForCompare(next, sampleWidth, sampleHeight);
    double total = 0.0;
    int rows = 0;
    for (int y = sampleHeight * 3 / 100; y < sampleHeight * 97 / 100; y += 2) {
        total += sampledRowDifference(a, b, y, y);
        ++rows;
    }
    return rows == 0 ? std::numeric_limits<double>::max() : total / rows;
}

bool LongScreenshotController::framesNearStagnant(const QImage &previous, const QImage &next)
{
    if (previous.isNull() || next.isNull() || previous.size() != next.size()) {
        return false;
    }
    const int sampleWidth = 160;
    const int sampleHeight = qMax(100, previous.height() * sampleWidth / previous.width());
    const QImage a = scaleForCompare(previous, sampleWidth, sampleHeight);
    const QImage b = scaleForCompare(next, sampleWidth, sampleHeight);
    int stableRows = 0;
    int rowCount = 0;
    double total = 0.0;
    for (int y = sampleHeight * 3 / 100; y < sampleHeight * 97 / 100; y += 2) {
        const double difference = sampledRowDifference(a, b, y, y);
        if (difference <= 2.0) {
            ++stableRows;
        }
        total += difference;
        ++rowCount;
    }
    if (rowCount == 0) {
        return false;
    }
    const double stableRatio = stableRows / static_cast<double>(rowCount);
    const double meanDifference = total / rowCount;
    return stableRatio >= 0.90 || meanDifference <= 1.8;
}

void LongScreenshotController::detectFixedRegions(const QVector<QImage> &frames,
                                                  int &fixedTop, int &fixedBottom)
{
    fixedTop = 0;
    fixedBottom = 0;
    if (frames.size() < 2) {
        return;
    }

    const int sampleWidth = 160;
    const int sampleHeight = qMax(120, frames.constFirst().height() * sampleWidth
                                           / frames.constFirst().width());
    QVector<QImage> samples;
    samples.reserve(frames.size());
    for (const QImage &frame : frames) {
        samples.append(scaleForCompare(frame, sampleWidth, sampleHeight));
    }

    QVector<double> rowScores(sampleHeight, 0.0);
    for (int pair = 1; pair < samples.size(); ++pair) {
        const QImage &previous = samples[pair - 1];
        const QImage &next = samples[pair];
        for (int y = 0; y < sampleHeight; ++y) {
            const QRgb *a = reinterpret_cast<const QRgb *>(previous.constScanLine(y));
            const QRgb *b = reinterpret_cast<const QRgb *>(next.constScanLine(y));
            qint64 difference = 0;
            int count = 0;
            for (int x = sampleWidth * 5 / 100; x < sampleWidth * 95 / 100; x += 4) {
                difference += qAbs(qRed(a[x]) - qRed(b[x]));
                difference += qAbs(qGreen(a[x]) - qGreen(b[x]));
                difference += qAbs(qBlue(a[x]) - qBlue(b[x]));
                count += 3;
            }
            const double score = difference / static_cast<double>(qMax(1, count));
            rowScores[y] = qMax(rowScores[y], score);
        }
    }

    QVector<double> smooth(sampleHeight, 0.0);
    for (int y = 0; y < sampleHeight; ++y) {
        QVector<double> window;
        for (int offset = -2; offset <= 2; ++offset) {
            const int row = y + offset;
            if (row >= 0 && row < sampleHeight) {
                window.append(rowScores[row]);
            }
        }
        std::sort(window.begin(), window.end());
        smooth[y] = window[window.size() / 2];
    }

    QVector<double> sorted = smooth;
    std::sort(sorted.begin(), sorted.end());
    const double movingLevel = sorted[sorted.size() * 3 / 4];
    const double threshold = qMax(2.2, qMin(10.0, movingLevel * 0.24));
    int topRows = findFixedTopBoundary(smooth, threshold, sampleHeight * 40 / 100);
    int bottomRows = findFixedBottomBoundary(smooth, threshold, sampleHeight * 30 / 100);
    const int minimum = qMax(4, sampleHeight * 2 / 100);
    if (topRows < minimum) {
        topRows = 0;
    }
    if (bottomRows < minimum) {
        bottomRows = 0;
    }

    fixedTop = qRound(topRows * frames.constFirst().height() / static_cast<double>(sampleHeight));
    fixedBottom = qRound(bottomRows * frames.constFirst().height()
                         / static_cast<double>(sampleHeight));
}

int LongScreenshotController::findFixedTopBoundary(const QVector<double> &scores,
                                                   double threshold, int limit)
{
    constexpr int requiredRun = 5;
    int highRun = 0;
    for (int y = 0; y < qMin(limit, scores.size()); ++y) {
        highRun = scores[y] > threshold ? highRun + 1 : 0;
        if (highRun >= requiredRun) {
            return qMax(0, y - requiredRun + 1);
        }
    }
    return 0;
}

int LongScreenshotController::findFixedBottomBoundary(const QVector<double> &scores,
                                                      double threshold, int limit)
{
    constexpr int requiredRun = 5;
    int highRun = 0;
    const int minY = qMax(0, scores.size() - limit);
    for (int y = scores.size() - 1; y >= minY; --y) {
        highRun = scores[y] > threshold ? highRun + 1 : 0;
        if (highRun >= requiredRun) {
            const int firstMovingRowFromBottom = y + requiredRun - 1;
            return qMax(0, scores.size() - firstMovingRowFromBottom - 1);
        }
    }
    return 0;
}

QVector<double> LongScreenshotController::buildEdgeMap(const QImage &image)
{
    const int width = image.width();
    const int height = image.height();
    QVector<double> grayscale(width * height, 0.0);
    QVector<double> edge(width * height, 0.0);
    for (int y = 0; y < height; ++y) {
        const QRgb *line = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = 0; x < width; ++x) {
            grayscale[y * width + x] = gray(line[x]);
        }
    }
    for (int y = 1; y < height - 1; ++y) {
        for (int x = 1; x < width - 1; ++x) {
            const double dx = grayscale[y * width + x + 1] - grayscale[y * width + x - 1];
            const double dy = grayscale[(y + 1) * width + x]
                - grayscale[(y - 1) * width + x];
            edge[y * width + x] = qMin(255.0, qAbs(dx) + qAbs(dy));
        }
    }
    return edge;
}

double LongScreenshotController::edgeCorrelation(const QVector<double> &previous,
                                                  const QVector<double> &next,
                                                  int width, int height, int shift)
{
    const int overlap = height - shift;
    if (overlap < 12) {
        return -1.0;
    }
    const int startY = qMax(1, overlap * 3 / 100);
    const int endY = qMin(overlap - 1, overlap * 97 / 100);
    const int startX = qMax(1, width * 5 / 100);
    const int endX = qMin(width - 1, width * 94 / 100);
    double sumAB = 0.0;
    double sumAA = 0.0;
    double sumBB = 0.0;
    for (int y = startY; y < endY; y += 2) {
        const int previousY = y + shift;
        for (int x = startX; x < endX; x += 2) {
            const double a = previous[previousY * width + x];
            const double b = next[y * width + x];
            sumAB += a * b;
            sumAA += a * a;
            sumBB += b * b;
        }
    }
    const double denominator = std::sqrt(sumAA * sumBB);
    return denominator <= 0.0001 ? -1.0 : sumAB / denominator;
}

double LongScreenshotController::differenceScore(const QImage &previous,
                                                 const QImage &next, int shift)
{
    const int overlap = previous.height() - shift;
    if (overlap < previous.height() / 8) {
        return std::numeric_limits<double>::max();
    }
    const int startY = qMax(2, previous.height() * 12 / 100);
    const int endY = overlap - qMax(2, previous.height() * 5 / 100);
    if (endY <= startY) {
        return std::numeric_limits<double>::max();
    }
    const int startX = previous.width() * 7 / 100;
    const int endX = previous.width() * 93 / 100;
    const int stepX = qMax(2, previous.width() / 32);
    const int stepY = qMax(2, previous.height() / 120);
    qint64 difference = 0;
    qint64 samples = 0;
    for (int y = startY; y < endY; y += stepY) {
        const QRgb *a = reinterpret_cast<const QRgb *>(previous.constScanLine(y + shift));
        const QRgb *b = reinterpret_cast<const QRgb *>(next.constScanLine(y));
        for (int x = startX; x < endX; x += stepX) {
            difference += qAbs(qRed(a[x]) - qRed(b[x]));
            difference += qAbs(qGreen(a[x]) - qGreen(b[x]));
            difference += qAbs(qBlue(a[x]) - qBlue(b[x]));
            samples += 3;
        }
    }
    return samples == 0 ? std::numeric_limits<double>::max()
                        : difference / static_cast<double>(samples);
}

int LongScreenshotController::findVerticalShiftRobust(const QImage &previous, const QImage &next,
                                                       int expectedShift, int historyShift,
                                                       bool allowShortShift, double &bestScore,
                                                       double &confidence)
{
    const int sampleWidth = 180;
    const int sampleHeight = qMax(120, previous.height() * sampleWidth / previous.width());
    const QImage a = scaleForCompare(previous, sampleWidth, sampleHeight);
    const QImage b = scaleForCompare(next, sampleWidth, sampleHeight);
    const QVector<double> edgeA = buildEdgeMap(a);
    const QVector<double> edgeB = buildEdgeMap(b);

    const int predictedPixels = historyShift > 0 ? historyShift : expectedShift;
    const int absoluteMin = qMax(2, sampleHeight * 2 / 100);
    const int absoluteMax = qMin(sampleHeight - 12, sampleHeight * 82 / 100);
    const int predicted = qBound(absoluteMin,
        qRound(predictedPixels * sampleHeight / static_cast<double>(previous.height())),
        absoluteMax);
    const int minShift = allowShortShift ? absoluteMin
                                         : qMax(absoluteMin, qRound(predicted * 0.55));
    const int maxShift = qMin(absoluteMax, qRound(predicted * 1.45));

    int bestShift = minShift;
    double bestRank = -std::numeric_limits<double>::max();
    double bestCorrelation = -std::numeric_limits<double>::max();
    double secondRank = -std::numeric_limits<double>::max();
    for (int shift = minShift; shift <= maxShift; shift += 2) {
        const double correlation = edgeCorrelation(edgeA, edgeB, sampleWidth, sampleHeight, shift);
        const double rank = correlation - 0.080 * qAbs(shift - predicted) / sampleHeight;
        if (rank > bestRank) {
            if (qAbs(shift - bestShift) >= 3) {
                secondRank = bestRank;
            }
            bestRank = rank;
            bestCorrelation = correlation;
            bestShift = shift;
        } else if (qAbs(shift - bestShift) >= 3 && rank > secondRank) {
            secondRank = rank;
        }
    }

    const int refineStart = qMax(minShift, bestShift - 3);
    const int refineEnd = qMin(maxShift, bestShift + 3);
    for (int shift = refineStart; shift <= refineEnd; ++shift) {
        const double correlation = edgeCorrelation(edgeA, edgeB, sampleWidth, sampleHeight, shift);
        const double rank = correlation - 0.080 * qAbs(shift - predicted) / sampleHeight;
        if (rank > bestRank) {
            bestRank = rank;
            bestCorrelation = correlation;
            bestShift = shift;
        }
    }

    bestScore = differenceScore(a, b, bestShift);
    confidence = secondRank == -std::numeric_limits<double>::max()
        ? 1.0 : bestRank - secondRank;
    int result = qRound(bestShift * previous.height() / static_cast<double>(sampleHeight));
    if (bestCorrelation < 0.42 || bestScore > 36.0) {
        result = historyShift > 0 ? historyShift : expectedShift;
    }
    return qBound(1, result, previous.height() - 1);
}

int LongScreenshotController::medianOfRecent(const QVector<int> &values, int count)
{
    if (values.isEmpty()) {
        return 0;
    }
    const int start = qMax(0, values.size() - count);
    QVector<int> recent;
    for (int i = start; i < values.size(); ++i) {
        recent.append(values[i]);
    }
    std::sort(recent.begin(), recent.end());
    return recent[recent.size() / 2];
}

LongScreenshotController::StitchResult LongScreenshotController::stitchFrames(
    const QVector<QImage> &frames, int swipePercent, bool reachedBottom)
{
    StitchResult result;
    if (frames.isEmpty()) {
        return result;
    }
    if (frames.size() == 1) {
        result.image = frames.constFirst();
        return result;
    }

    detectFixedRegions(frames, result.fixedTop, result.fixedBottom);
    int viewportHeight = frames.constFirst().height() - result.fixedTop - result.fixedBottom;
    if (viewportHeight < frames.constFirst().height() * 40 / 100) {
        result.warnings.append(QObject::tr("固定区域识别结果异常，已改为整屏拼接。"));
        result.fixedTop = 0;
        result.fixedBottom = 0;
        viewportHeight = frames.constFirst().height();
    }

    QVector<int> shifts;
    QVector<int> shiftHistory;
    const int expectedShift = qMax(1, viewportHeight * swipePercent / 100);
    for (int i = 1; i < frames.size(); ++i) {
        const QImage previousViewport = frames[i - 1].copy(0, result.fixedTop,
                                                           frames[i - 1].width(), viewportHeight);
        const QImage nextViewport = frames[i].copy(0, result.fixedTop,
                                                   frames[i].width(), viewportHeight);
        double score = 0.0;
        double confidence = 0.0;
        const int historyShift = medianOfRecent(shiftHistory, 3);
        int shift = findVerticalShiftRobust(previousViewport, nextViewport, expectedShift,
                                            historyShift,
                                            reachedBottom && i == frames.size() - 1,
                                            score, confidence);
        if (historyShift > 0 && qAbs(shift - historyShift) > viewportHeight * 18 / 100
            && score > 5.0) {
            result.warnings.append(QObject::tr("第 %1 屏匹配不唯一，已使用近期滚动距离校正。")
                                       .arg(i + 1));
            shift = historyShift;
        }
        shifts.append(shift);
        shiftHistory.append(shift);
        if (score > 22.0 || confidence < 0.004) {
            result.warnings.append(QObject::tr("第 %1 屏的重叠相似度较低，建议检查拼接处。")
                                       .arg(i + 1));
        }
    }

    qint64 totalHeight = frames.constFirst().height();
    for (int shift : shifts) {
        totalHeight += shift;
    }
    if (totalHeight > kMaximumOutputHeight) {
        result.warnings.append(QObject::tr("生成图片超过 60000 像素，请减少最大拼接屏数。"));
        return result;
    }

    QImage output(frames.constFirst().width(), static_cast<int>(totalHeight), QImage::Format_RGB32);
    if (output.isNull()) {
        return result;
    }
    output.fill(Qt::white);
    QPainter painter(&output);
    int targetY = 0;
    const int width = output.width();
    if (result.fixedTop > 0) {
        painter.drawImage(QRect(0, targetY, width, result.fixedTop), frames.constFirst(),
                          QRect(0, 0, width, result.fixedTop));
        targetY += result.fixedTop;
    }
    painter.drawImage(QRect(0, targetY, width, viewportHeight), frames.constFirst(),
                      QRect(0, result.fixedTop, width, viewportHeight));
    targetY += viewportHeight;
    for (int i = 1; i < frames.size(); ++i) {
        const int shift = shifts[i - 1];
        const int sourceY = result.fixedTop + viewportHeight - shift;
        painter.drawImage(QRect(0, targetY, width, shift), frames[i],
                          QRect(0, sourceY, width, shift));
        targetY += shift;
    }
    if (result.fixedBottom > 0) {
        painter.drawImage(QRect(0, targetY, width, result.fixedBottom), frames.constLast(),
                          QRect(0, result.fixedTop + viewportHeight, width, result.fixedBottom));
    }
    painter.end();
    result.image = output;
    return result;
}
