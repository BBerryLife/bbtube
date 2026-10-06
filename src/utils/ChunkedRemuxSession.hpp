#ifndef CHUNKEDREMUXSESSION_HPP_
#define CHUNKEDREMUXSESSION_HPP_

#include "src/utils/mp4_stream_remux.hpp"

#include <QObject>
#include <QString>
#include <QByteArray>
#include <QList>
#include <QMap>
#include <QFile>
#include <QTime>
#include <QNetworkAccessManager>
#include <QNetworkReply>

#include <vector>

// Downloads a video-only + audio-only DASH (fragmented mp4) pair in CHUNKS
// of about `chunkSeconds` each (default 5s -- YouTube video fragments are
// ~5.12s GOPs, so normally one chunk == one video fragment), stitches the
// chunks together, and hands the player a *merged, complete, valid* MP4
// that covers everything downloaded so far.
//
// Why merged files instead of one pre-allocated file (the old
// StreamingRemuxSession approach): BB10's mmrenderer will only open a local
// file that is complete, and the old approach had to know the sample table
// of the WHOLE video (hundreds of moof round-trips) before it could write
// the first byte -- ~30s on a slow relay -- and then the file was only
// playable once the whole download finished. Here only the moof headers of
// the chunks actually needed are fetched, the first merged file exists as
// soon as the first chunk has arrived, and later merged files simply cover
// more of the video.
//
// Usage (see PlayerPage):
//   session = new ChunkedRemuxSession(nm, videoUrl, audioUrl, cacheDir, base, this);
//   connect(progress -> decide when to merge), connect(mergedReady -> play/swap)
//   session->start();
//   session->requestMerge();   // whenever a newer merged file is wanted
//
// mergedReady() always reports a file that is already closed and complete.
// Only one merge runs at a time; requestMerge() during a merge is remembered
// and serviced right after. A merge copies the staged sample payloads in
// 1MB slices from the event loop, so the UI never blocks on it.
//
// C++03/GNU++98 only (QNX gcc 4.6.3, Qt 4.8).
class ChunkedRemuxSession: public QObject
{
Q_OBJECT
public:
    ChunkedRemuxSession(QNetworkAccessManager *networkManager, const QString &videoUrl,
            const QString &audioUrl, const QString &cacheDir, const QString &baseName,
            QObject *parent = 0, double chunkSeconds = 5.0);
    virtual ~ChunkedRemuxSession();

    void start();
    void cancel(); // synchronous: after it returns no slot of this object runs again

    // Ask for a merged file covering everything downloaded so far.
    void requestMerge();

    // Seconds of video+audio downloaded contiguously from t=0.
    double downloadedSeconds() const { return m_coveredSeconds; }
    double totalSeconds() const { return m_totalSeconds; }
    bool isDownloadComplete() const { return m_allAppended; }
    bool isMerging() const { return m_mergeActive; }
    bool hasFailed() const { return m_failed; }

    // Where a player that must continue playback in a (longer) merged file
    // should seek so it lands exactly on a keyframe.
    //
    // Chunk boundaries are keyframes, and the only positions a seek can hit
    // exactly: a seek into the middle of a GOP resumes at the previous
    // keyframe, a rewind of up to a GOP (6-8s on YouTube). Returns the
    // position, in seconds, of the chunk boundary closest to nearSeconds
    // (among the chunks downloaded so far), already nudged past that
    // keyframe's composition-time offset (its PTS is DTS + a few frame
    // durations whenever the stream has B-frames) so a PTS-based seek cannot
    // fall short of it.
    double keyframeResumeSeconds(double nearSeconds) const;

signals:
    // Fires every time another chunk has been appended to the staging files.
    void progress(double downloadedSeconds, double totalSeconds);
    // A merged file covering [0, coveredSeconds) is complete on disk.
    // isFinal: it covers the whole video (and is the cached copy).
    void mergedReady(QString path, double coveredSeconds, bool isFinal);
    void failed(QString message);

private slots:
    void onHeadFinished();
    void onJobFinished();
    void onRetryTimer();
    void onMergeStep();
    void onCacheHitTimer();

private:
    enum JobType { JobMoofVideo, JobMoofAudio, JobBodyVideo, JobBodyAudio };

    struct Job
    {
        int type;
        int chunk; // chunk that needed it -- dispatch priority (lower first)
        size_t frag; // moof jobs: fragment index
        qint64 start; // source byte range [start, start+len)
        qint64 len;
        qint64 bufOffset; // body jobs: where the bytes go in the chunk buffer
        int retries;
        QNetworkReply *reply;
        Job() : type(0), chunk(0), frag(0), start(0), len(0), bufOffset(0), retries(0), reply(0) {}
    };

    struct Chunk
    {
        size_t fragStart, fragEnd; // video fragments
        size_t aFragEnd; // audio fragments [0, aFragEnd) are needed
        double startSec, endSec;
        bool discoveryQueued, assembled, ready, appended;
        size_t vi0, vi1, ai0, ai1; // sample ranges in m_vAll / m_aAll
        QByteArray vBuf, aBuf;
        int vPending, aPending;
        int verifyRetries;
        // Hash of the last buffer that failed the structure check. Relay
        // garbage differs from attempt to attempt; identical bytes twice in
        // a row are the real content and the check is what is wrong.
        uint lastBadVideoHash, lastBadAudioHash;
        bool hasBadVideo, hasBadAudio;
        Chunk() : fragStart(0), fragEnd(0), aFragEnd(0), startSec(0), endSec(0),
                discoveryQueued(false), assembled(false), ready(false), appended(false),
                vi0(0), vi1(0), ai0(0), ai1(0), vPending(0), aPending(0), verifyRetries(0),
                lastBadVideoHash(0), lastBadAudioHash(0), hasBadVideo(false), hasBadAudio(false) {}
    };

    void requestHead(bool isVideo);
    void onHeadsReady();
    void scheduleDiscovery(size_t k);
    bool discoveryDone(size_t k) const;
    void tryAssemble();
    void assemble(size_t k);
    void queueBodyJobs(size_t k);
    void queueVideoBodyJobs(size_t k);
    void queueAudioBodyJobs(size_t k);
    void queueJob(Job *job);
    void pump();
    void dispatch(Job *job);
    void retryJob(Job *job, const QString &why);
    void handleMoof(Job *job, const QByteArray &data);
    void handleBody(Job *job, const QByteArray &data);
    void onChunkBuffersComplete(size_t k);
    void appendReadyChunks();
    void appendAudioFragsInOrder();
    void failWith(const QString &message);
    void teardown(bool removeStageFiles);
    void finishMerge();
    QString mergedPathFor(int seq, bool isFinal) const;
    QString finalPath() const;

    QNetworkAccessManager *m_nm;
    QString m_videoUrl, m_audioUrl, m_cacheDir, m_baseName;
    double m_chunkSeconds;
    QTime m_clock;

    bool m_started, m_cancelled, m_failed, m_headsDone, m_allAppended;

    // --- heads ---
    TrackHead m_videoHead, m_audioHead; // fragSamples == samples already APPENDED (accepted)
    bool m_videoHeadOk, m_audioHeadOk;
    int m_videoHeadSize, m_audioHeadSize;
    int m_videoHeadRetries, m_audioHeadRetries;
    QNetworkReply *m_videoHeadReply, *m_audioHeadReply;

    // --- plan ---
    std::vector<Chunk> m_chunks;
    std::vector<double> m_aFragStart; // audio sidx cumulative starts (+ total)
    std::vector<uint64_t> m_vFragOffset, m_aFragOffset; // absolute moof offsets
    std::vector<int> m_vFragState, m_aFragState; // 0 none, 1 in flight/queued, 2 done
    std::vector<std::vector<FragSample> > m_vFragSamples, m_aFragSamples;
    std::vector<FragSample> m_vAll, m_aAll; // samples in assigned order
    size_t m_aFragAppended; // audio fragments already flattened into m_aAll
    size_t m_aNextIdx; // next audio sample to hand to a chunk
    uint64_t m_aNextStartTicks;
    size_t m_nextToAssemble, m_nextToAppend;

    // --- network queue ---
    QList<Job *> m_queue;
    QMap<QNetworkReply *, Job *> m_inflight;
    QList<Job *> m_retryQueue;

    // --- staging (append-only raw sample payloads) ---
    QFile *m_vStage, *m_aStage;
    qint64 m_vStageBytes, m_aStageBytes;
    uint64_t m_vTicks, m_aTicks;
    // One entry per downloaded chunk: its first keyframe's start time and
    // the position to seek to in order to land on it.
    std::vector<double> m_boundaryStart, m_boundaryResume;
    QTime m_mergeClock;
    double m_coveredSeconds, m_totalSeconds;

    // --- merge ---
    bool m_mergeActive, m_mergeAgain;
    int m_mergeSeq;
    size_t m_mergeChunks, m_mergedChunksDone;
    bool m_mergeFinal;
    double m_mergeCovered;
    QString m_mergePath, m_mergePrevPath, m_mergePrevPrevPath;
    RemuxPlan m_mergePlan;
    QFile *m_mergeOut, *m_mergeAin, *m_mergeVin;
    qint64 m_mergeARemain, m_mergeVRemain;
};

#endif /* CHUNKEDREMUXSESSION_HPP_ */
