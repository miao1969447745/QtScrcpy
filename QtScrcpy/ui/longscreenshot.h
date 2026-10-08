/*
 * Added by the ScrcpyGUI derivative project in 2026.
 * Licensed under the Apache License, Version 2.0.
 */
#ifndef LONGSCREENSHOT_H
#define LONGSCREENSHOT_H

#include <QImage>
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QVector>
#include <QJsonObject>
#include <functional>

class QProgressDialog;
class VideoForm;

class LongScreenshotController : public QObject
{
    Q_OBJECT

public:
    explicit LongScreenshotController(VideoForm *videoForm);
    void start();
    bool startRemote(int maxFrames = 99);
    void cancel();
    bool isActive() const { return m_active; }
signals:
    void finished(const QJsonObject &result);

private:
    friend class PhoneBridgeTests;
    void begin(int maxFrames, bool remote);
    void releaseFinger();
    void schedule(int delay, std::function<void()> callback);
    struct StitchResult {
        QImage image;
        int fixedTop = 0;
        int fixedBottom = 0;
        QStringList warnings;
    };

    void captureInitialFrame();
    void beginSwipe();
    void sendSwipeStep();
    void pollForSettledFrame();
    void acceptPostSwipeFrame(const QImage &frame);
    void finish(bool reachedBottom);
    void abort(const QString &message, bool showMessage = true);
    bool isCancelled() const;
    QImage captureFrame() const;
    void updateProgress(const QString &message);

    static QImage scaleForCompare(const QImage &source, int width, int height);
    static double sampledRowDifference(const QImage &previous, const QImage &next,
                                       int previousY, int nextY);
    static double sampledImageDifference(const QImage &previous, const QImage &next);
    static bool framesNearStagnant(const QImage &previous, const QImage &next);
    static void detectFixedRegions(const QVector<QImage> &frames, int &fixedTop, int &fixedBottom);
    static int findFixedTopBoundary(const QVector<double> &scores, double threshold, int limit);
    static int findFixedBottomBoundary(const QVector<double> &scores, double threshold, int limit);
    static QVector<double> buildEdgeMap(const QImage &image);
    static double edgeCorrelation(const QVector<double> &previous, const QVector<double> &next,
                                  int width, int height, int shift);
    static double differenceScore(const QImage &previous, const QImage &next, int shift);
    static int findVerticalShiftRobust(const QImage &previous, const QImage &next,
                                       int expectedShift, int historyShift, bool allowShortShift,
                                       double &bestScore, double &confidence);
    static int medianOfRecent(const QVector<int> &values, int count);
    static StitchResult stitchFrames(const QVector<QImage> &frames, int swipePercent,
                                     bool reachedBottom);

private:
    QPointer<VideoForm> m_videoForm;
    QPointer<QProgressDialog> m_progress;
    QVector<QImage> m_frames;
    QImage m_beforeSwipe;
    QImage m_lastProbe;
    int m_maxFrames = 99;
    int m_swipePercent = 35;
    int m_swipeStep = 0;
    int m_settlePolls = 0;
    int m_stagnantCaptures = 0;
    bool m_changedAfterSwipe = false;
    bool m_active = false;
    bool m_cancelled = false;
    bool m_remote = false;
    bool m_fingerDown = false;
    quint64 m_epoch = 0;
    quint64 m_runGeneration = 0;
};

#endif // LONGSCREENSHOT_H
