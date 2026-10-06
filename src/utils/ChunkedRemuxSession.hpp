#ifndef CHUNKEDREMUXSESSION_HPP_
#define CHUNKEDREMUXSESSION_HPP_

#include "src/utils/mp4_stream_remux.hpp"

#include <QObject>
#include <QString>
#include <QStringList>
#include <QByteArray>
#include <QList>
#include <QMap>
#include <QFile>
#include <QTime>
#include <QTimer>
#include <QNetworkAccessManager>
#include <QNetworkReply>

#include <vector>
#include <utility>

// Downloads a video-only + audio-only DASH (fragmented mp4) pair in CHUNKS of
// about `chunkSeconds` each (YouTube video fragments are keyframe-aligned
// GOPs of 5-10s, so a chunk is normally exactly one video fragment), keeps
// every downloaded chunk, and stitches runs of consecutive chunks into
// complete, valid MP4 files for the player.
//
// Random access: the "download head" can be moved at any time (seekTo()).
// Downloading then continues from that chunk onward; chunks already stored
// are never thrown away until the session is destroyed (= the video is
// closed). A merged file covers one *run* of consecutive stored chunks; its
// own timeline starts at 0, i.e. at chunkStartSeconds(startChunk) of the
// video -- the caller maps positions with that offset.
//
// Why merged files instead of one pre-allocated file: see the history of
// StreamingRemuxSession -- BB10's mmrenderer only opens complete local files.
//
// Threading: everything runs on the event loop of the owning thread. Merging
// copies staged payloads in 1MB slices from the event loop.
//
// C++03/GNU++98 only (QNX gcc 4.6.3, Qt 4.8).
class ChunkedRemuxSession: public QObject
{
Q_OBJECT
public:
    // startAtSeconds: where downloading begins (saved watch position, or the
    // current position when switching quality).
    ChunkedRemuxSession(QNetworkAccessManager *networkManager, const QString &videoUrl,
            const QString &audioUrl, const QString &cacheDir, const QString &baseName,
            QObject *parent = 0, double chunkSeconds = 5.0, double startAtSeconds = 0.0);
    virtual ~ChunkedRemuxSession();

    void start();
    void cancel(); // synchronous: after it returns no slot of this object runs again

    // Moves the download head to the chunk containing absSeconds and returns
    // that chunk's index (-1 if the chunk plan is not known yet; the request
    // is remembered). Downloading continues from there; if that chunk is
    // already stored the head moves on to the first missing chunk after it.
    int seekTo(double absSeconds);

    // Builds a merged file for the run of stored chunks that begins at
    // startChunk (which must be stored). Async; result via mergedReady().
    void requestMerge(int startChunk);

    // --- queries (valid once heads are ready; see headsReady()) ---
    bool headsReady() const { return m_headsDone; }
    int chunkCount() const { return int(m_chunks.size()); }
    int chunkIndexAt(double absSeconds) const;
    double chunkStartSeconds(int k) const;
    double chunkEndSeconds(int k) const;
    bool isChunkStored(int k) const;
    int runStart(int k) const; // first chunk of the stored run containing k (k if not stored)
    int runEnd(int k) const; // exclusive end of the stored run that starts at/contains k (k if not stored)
    // Start chunk for a file meant to be played from chunk k: the run start,
    // but no more than ~maxBackSeconds before k (keeps files small).
    int fileStartFor(int k, double maxBackSeconds) const;
    // Where a player that continues in a (longer) file that starts at
    // fileStartChunk must seek -- in that file's own timeline, in seconds --
    // to land exactly on the keyframe that begins chunk boundaryChunk. See
    // the long comment at the implementation for why this matters.
    double fileResumeSeconds(int fileStartChunk, int boundaryChunk) const;
    double totalSeconds() const { return m_totalSeconds; }
    bool isDownloadComplete() const { return m_storedCount == int(m_chunks.size()) && m_headsDone; }
    bool isMerging() const { return m_mergeActive; }
    bool hasFailed() const { return m_failed; }

signals:
    // Chunk k has just been stored.
    void progress(int chunkIndex);
    // A merged file is complete on disk. It contains chunks
    // [startChunk, startChunk + chunkCount) -- chunkCount is -1 for a cached
    // complete file (isFinal, chunk plan unknown). isFinal: it covers the
    // whole video from 0:00 (it is also the cached copy for next time).
    void mergedReady(QString path, int startChunk, int chunkCount, bool isFinal);
    void failed(QString message);

private slots:
    void onHeadFinished();
    void onJobFinished();
    void onJobProgress(qint64 received, qint64 total);
    void onRetryTimer();
    void onMergeStep();
    void onCacheHitTimer();
    void onWatchdog();

private:
    enum JobType { JobMoofVideo, JobMoofAudio, JobBodyVideo, JobBodyAudio };

    struct Job
    {
        int type;
        int chunk; // chunk that needed it
        size_t frag; // moof jobs: fragment index
        qint64 start; // source byte range [start, start+len)
        qint64 len;
        qint64 bufOffset; // body jobs: where the bytes go in the chunk buffer
        int retries;
        QNetworkReply *reply;
        QTime startedAt;
        QTime lastActivity;
        Job *twin; // hedged duplicate of the same request (first to finish wins)
        Job() : type(0), chunk(0), frag(0), start(0), len(0), bufOffset(0), retries(0), reply(0), twin(0) {}
    };

    struct Chunk
    {
        size_t fragStart, fragEnd; // video fragments [fragStart, fragEnd)
        size_t aFragStart, aFragEnd; // audio fragments overlapping the chunk
        double startSec, endSec;
        int state; // 0 idle, 1 working, 2 stored
        bool assembled, ready;
        std::vector<FragSample> vSamples, aSamples; // assigned in assemble(), kept after storing
        QByteArray vBuf, aBuf;
        int vPending, aPending;
        int verifyRetries;
        uint lastBadVideoHash, lastBadAudioHash;
        bool hasBadVideo, hasBadAudio;
        qint64 vOff, vLen, aOff, aLen; // location in the staging files once stored
        uint64_t vTicks; // sum of video sample durations
        int32_t firstCto; // composition offset of the first video sample
        Chunk() : fragStart(0), fragEnd(0), aFragStart(0), aFragEnd(0), startSec(0), endSec(0), state(0),
                assembled(false), ready(false), vPending(0), aPending(0), verifyRetries(0),
                lastBadVideoHash(0), lastBadAudioHash(0), hasBadVideo(false), hasBadAudio(false),
                vOff(0), vLen(0), aOff(0), aLen(0), vTicks(0), firstCto(0) {}
    };

    void requestHead(bool isVideo);
    void onHeadsReady();
    int dist(int k) const; // distance of chunk k ahead of the download head (wraps)
    int firstMissingFrom(int k) const;
    void abortUnstoredWork();
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
    void cancelJob(Job *job);
    void retryJob(Job *job, const QString &why);
    void handleMoof(Job *job, const QByteArray &data);
    void handleBody(Job *job, const QByteArray &data);
    void onChunkBuffersComplete(size_t k);
    void storeChunk(size_t k);
    void startNextMerge();
    void failWith(const QString &message);
    void teardown(bool removeStageFiles);
    void finishMerge();
    void closeMergeFiles(bool removePartial);
    QString mergedPathFor(int seq, bool isFinal) const;
    QString finalPath() const;

    QNetworkAccessManager *m_nm;
    QString m_videoUrl, m_audioUrl, m_cacheDir, m_baseName;
    double m_chunkSeconds;
    double m_startAtSeconds;
    QTime m_clock;

    bool m_started, m_cancelled, m_failed, m_headsDone;

    // --- heads ---
    TrackHead m_videoHead, m_audioHead; // fragSamples stays empty: samples live in the chunks
    bool m_videoHeadOk, m_audioHeadOk;
    int m_videoHeadSize, m_audioHeadSize;
    int m_videoHeadRetries, m_audioHeadRetries;
    QNetworkReply *m_videoHeadReply, *m_audioHeadReply;
    QTime m_videoHeadClock, m_audioHeadClock;

    // --- plan ---
    std::vector<Chunk> m_chunks;
    std::vector<double> m_aFragStartSec; // audio sidx cumulative starts (+ total)
    std::vector<uint64_t> m_aFragStartTicks; // same, in audio track ticks (exact)
    std::vector<uint64_t> m_vFragOffset, m_aFragOffset; // absolute moof offsets in the sources
    std::vector<int> m_vFragState, m_aFragState; // 0 none, 1 in flight/queued, 2 done
    std::vector<std::vector<FragSample> > m_vFragSamples, m_aFragSamples; // kept: shared between chunks
    int m_head; // chunk the download is currently working towards
    double m_pendingSeekSec; // seekTo() called before the plan existed (<0: none)
    int m_storedCount;
    double m_totalSeconds;

    // --- network queue ---
    QList<Job *> m_queue;
    QMap<QNetworkReply *, Job *> m_inflight;
    QList<Job *> m_retryQueue;
    QTimer *m_watchdog;

    // --- staging: payloads of stored chunks, appended in completion order ---
    QFile *m_vStage, *m_aStage;
    qint64 m_vStageBytes, m_aStageBytes;

    // --- merge ---
    bool m_mergeActive;
    int m_mergeNextStart; // a request that arrived during a merge (-1: none)
    int m_mergeSeq;
    int m_mergeStart, m_mergeCount;
    bool m_mergeFinal;
    QString m_mergePath;
    QStringList m_mergedFiles; // non-final merged files, oldest first
    RemuxPlan m_mergePlan;
    QFile *m_mergeOut, *m_mergeAin, *m_mergeVin;
    std::vector<std::pair<qint64, qint64> > m_mergeASegs, m_mergeVSegs; // (offset, length) in the staging files
    size_t m_mergeASegIdx, m_mergeVSegIdx;
    qint64 m_mergeSegLeft; // bytes left in the segment currently being copied
    bool m_mergeSegOpen;
    QTime m_mergeClock;
};

#endif /* CHUNKEDREMUXSESSION_HPP_ */
