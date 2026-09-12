#ifndef STREAMINGREMUXSESSION_HPP_
#define STREAMINGREMUXSESSION_HPP_

#include "src/utils/mp4_stream_remux.hpp"

#include <QObject>
#include <QString>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QFile>
#include <QList>

// Manages downloading a video-only + audio-only adaptive stream pair and
// remuxing them into a single local MP4 file WHILE they download, instead
// of waiting for both downloads to finish first. See mp4_stream_remux.hpp
// for the underlying box-parsing/layout logic.
//
// Two source shapes are supported, auto-detected per track from parseHead():
//   - Plain progressive mp4 (contiguous mdat, real stco/stsz in moov): the
//     original fast path -- head fetch, then one Range-streamed body fetch.
//   - DASH-fragmented mp4 (YouTube/Invidious adaptiveFormats: ftyp+moov+
//     sidx+[moof+mdat]xN, empty stco/stsz in moov): after the head fetch,
//     a short "fragment discovery" pass fetches just each fragment's small
//     moof header (via sidx-derived byte ranges) to build a real
//     progressive sample table, THEN body streaming proceeds fragment by
//     fragment, copying each fragment's real mdat bytes to their final
//     per-sample output positions.
//
// Typical usage (see PlayerPage.cpp):
//   StreamingRemuxSession *session = new StreamingRemuxSession(
//       networkManager, videoUrl, audioUrl, outputPath, this);
//   connect(session, SIGNAL(headReady()), this, SLOT(onRemuxHeadReady()));
//   connect(session, SIGNAL(failed(QString)), this, SLOT(onRemuxFailed(QString)));
//   session->start();
//   // once headReady() fires, session->outputPath() is a valid, playable
//   // (if still-growing) local MP4 file.
//
// Preview mode (maxDurationSeconds > 0, see constructor): truncates each
// track's fragSamples to just the samples covering the first N seconds
// (fragmented sources only -- see beginFragmentDiscovery's isFragmented
// check) before the sample table is built, producing a small, fully
// self-contained, independently playable MP4 covering only that much of
// the video. finished() still means "this file, such as it is, is
// completely written" -- it's just a much shorter file. Used by
// PlayerPage to get a ~15s preview onto disk (and into the player) fast
// while the real full-length remux runs alongside it; see
// PlayerPage::startPreviewThenFullRemux().
// One contiguous Range request's worth of video-body samples, computed
// once up front (in dispatch order) so each batch's absolute output
// offset is known regardless of which order its request actually
// completes in -- this is what lets several be downloaded at once
// instead of strictly one-at-a-time. See FRAG_BODY_CONCURRENCY in the
// .cpp.
struct FragBodyBatch
{
    size_t startIdx; // index into TrackHead::fragSamples of this batch's first sample
    size_t count; // number of samples covered
    qint64 rangeStart; // source byte offset (Range request start)
    qint64 batchBytes; // total bytes covered (Range request length)
    qint64 outOffset; // absolute offset in the output file to write this batch's bytes
};

// C++03/GNU++98 only (matches bbtube's QNX/gcc 4.6.3 toolchain).
class StreamingRemuxSession: public QObject
{
Q_OBJECT
public:
    StreamingRemuxSession(QNetworkAccessManager *networkManager, const QString &videoUrl,
            const QString &audioUrl, const QString &outputPath, QObject *parent = 0,
            double maxDurationSeconds = 0.0);
    virtual ~StreamingRemuxSession();

    void start();
    QString outputPath() const
    {
        return m_outputPath;
    }

    // Synchronously disconnects every signal/slot connection this session
    // owns (including its own internal reply->finished() connections) and
    // aborts all in-flight network replies, before returning. Call this
    // BEFORE deleteLater()-ing a session that's being replaced (e.g. the
    // person changed quality while discovery was still in progress) --
    // deleteLater() alone only schedules destruction for the next event
    // loop iteration, and QNetworkReply::abort() can synchronously emit
    // finished() on some Qt/BB10 builds; if that happens before this
    // session's own destructor runs, its finished-handler slots
    // (onVideoMoofFinished() etc.) would still fire and touch member state
    // concurrently with -- or after -- teardown has started, which is a
    // use-after-free landmine. Disconnecting internal signal/slot wiring
    // first guarantees none of that session's own slots can run again,
    // regardless of what abort() does synchronously vs. asynchronously.
    void cancel();

signals:
    // Output file has been pre-allocated to its final size with a valid
    // ftyp+moov+mdat header written -- safe to open/probe/start playing
    // from the beginning now (playback should stay behind
    // videoBytesWritten() though; see progress()).
    void headReady();

    // Fires as each video chunk is written to disk.
    void progress(qint64 videoBytesWritten, qint64 videoBytesTotal);

    void audioComplete();
    void finished(); // both tracks fully written -- output file is complete
    void failed(QString errorMessage);

private slots:
    void onVideoHeadFinished();
    void onAudioHeadFinished();
    void onAudioBodyFinished();
    void onVideoBodyReadyRead();
    void onVideoBodyFinished();

    // Fragment-discovery path (fragmented sources only).
    void onVideoMoofFinished();
    void onAudioMoofFinished();
    // Fragment-by-fragment body streaming path (fragmented sources only).
    // Video runs FRAG_BODY_CONCURRENCY batches at once (see
    // dispatchVideoBodyBatches()); audio stays single-request since it's
    // small and finishes quickly regardless.
    void onVideoBodyBatchFinished();
    void onAudioFragBodyFinished();

    // Delayed-retry trampolines: QTimer::singleShot needs a slot to land
    // in (no lambdas -- this is C++03/gcc 4.6.3), and each one carries
    // the retry's target via a plain member set right before arming the
    // timer. See RETRY_DELAY_MS in the .cpp for why retries are delayed
    // at all instead of firing instantly.
    void onMoofRetryTimer();
    void onVideoBodyBatchRetryTimer();
    void onAudioFragBodyRetryTimer();

private:
    void requestVideoHead();
    void requestAudioHead();
    void tryBeginPlan();
    void beginBodyDownloads();
    void failWith(const QString &message);
    void checkAllDone();

    // Fragment-discovery helpers (fragmented sources only).
    void beginFragmentDiscovery(TrackHead &track, bool isVideoTrack);
    void dispatchMoofRequests(bool isVideoTrack); // fills the concurrency window
    void retryMoofRequest(bool isVideoTrack, size_t fragIndex); // re-issues a single fragment after a transient failure
    void onTrackFragmentsReady(bool isVideoTrack);
    // Preview-mode support: drops every fragSamples entry at/after the
    // first one whose cumulative duration (in the track's own timescale)
    // reaches m_maxDurationSeconds, and updates track.duration to match
    // so the output moov reports the truncated length instead of the
    // original. No-op if m_maxDurationSeconds <= 0 or the track is
    // already shorter than that. Called from onTrackFragmentsReady(),
    // before buildProgressiveTablesFromFragments() -- see the header
    // comment on "Preview mode" above for why this has to happen before
    // that call rather than after.
    void truncateTrackForPreview(TrackHead &track);

    // Fragment-by-fragment body streaming helpers (fragmented sources only).
    void beginFragmentedBodyDownloads();
    void requestNextFragBody(bool isVideoTrack); // audio only now -- see class-level comment above onVideoBodyBatchFinished()
    void requestNextFragBodyImpl(bool isVideoTrack, bool isRetry); // shared by requestNextFragBody() and onAudioFragBodyRetryTimer()
    void buildVideoBodyBatches(); // computes m_videoBodyBatches once, up front
    void dispatchVideoBodyBatches(); // fills the FRAG_BODY_CONCURRENCY window
    void retryVideoBodyBatch(size_t batchIndex); // re-issues a single batch after a transient failure

    // Pending-retry state for the delayed-retry trampolines above --
    // written right before arming a QTimer::singleShot and read back
    // when it fires. Only one of each kind is ever pending at a time
    // per call site (the moof/body-batch code paths retry one
    // fragment/batch at a time), so a single pair of members per
    // trampoline is enough.
    bool m_pendingMoofRetryIsVideo;
    size_t m_pendingMoofRetryFragIndex;
    size_t m_pendingVideoBodyRetryBatchIndex;

    QNetworkAccessManager *m_networkManager;
    QString m_videoUrl;
    QString m_audioUrl;
    QString m_outputPath;
    // Preview-mode cap (0 = disabled, full remux as before). See the
    // "Preview mode" header comment above and truncateTrackForPreview().
    double m_maxDurationSeconds;

    int m_videoHeadFetchSize;
    int m_audioHeadFetchSize;
    // Retry counters for a transient network error (timeout, dropped
    // connection) on the initial head fetch itself -- distinct from
    // m_video/audioHeadFetchSize's "double and retry" loop above, which
    // only handles a head that came back OK but too small to contain a
    // full moov/sidx. A network error has nothing to do with fetch size,
    // so it's retried at the SAME size up to BODY_MAX_RETRIES times
    // before giving up.
    int m_videoHeadRetryCount;
    int m_audioHeadRetryCount;
    bool m_videoHeadParsed;
    bool m_audioHeadParsed;
    bool m_planStarted;
    bool m_failed;

    TrackHead m_videoHead;
    TrackHead m_audioHead;
    RemuxPlan m_plan;

    QNetworkReply *m_videoHeadReply;
    QNetworkReply *m_audioHeadReply;
    QNetworkReply *m_videoBodyReply;
    QNetworkReply *m_audioBodyReply;

    QFile *m_outputFile; // kept open for the duration of the video body stream
    qint64 m_videoBytesWritten;
    bool m_audioDone;
    bool m_videoDone;

    // --- fragmented-source state ---
    bool m_videoFragmentsReady;
    bool m_audioFragmentsReady;
    // Discovery cursor: m_video/audioFragDiscoveryIndex is the next fragment
    // index still needing a moof request DISPATCHED (may be dispatched
    // out of completion order since several run concurrently -- see
    // MOOF_DISCOVERY_CONCURRENCY); m_video/audioFragDiscoveryCompleted
    // counts how many have finished (regardless of dispatch order) so we
    // know when the whole track is done. m_video/audioFragDiscoveryOffset
    // is a per-fragment-index running offset table computed once up front
    // (from sidx) since offsets can no longer be derived by simply adding
    // the previous fragment's size to a single cursor once requests are
    // in flight out of order.
    size_t m_videoFragDiscoveryIndex;
    size_t m_audioFragDiscoveryIndex;
    size_t m_videoFragDiscoveryCompleted;
    size_t m_audioFragDiscoveryCompleted;
    std::vector<uint64_t> m_videoFragOffsets; // [i] = absolute source offset of fragment i's moof
    std::vector<uint64_t> m_audioFragOffsets;
    QList<QNetworkReply *> m_videoMoofReplies; // in-flight, order not significant
    QList<QNetworkReply *> m_audioMoofReplies;
    // Per-fragment retry counters for transient moof-fetch network errors
    // (e.g. a dropped connection on the videoplayback edge server when
    // several Range requests hit it concurrently). Indexed by fragment
    // number, sized/zeroed alongside m_video/audioFragSlots in
    // beginFragmentDiscovery(); a fragment that exhausts
    // MOOF_MAX_RETRIES still fails the session via failWith() as before.
    std::vector<int> m_videoFragRetries;
    std::vector<int> m_audioFragRetries;
    // Per-fragment sample storage, indexed by fragment number -- needed
    // because completions arrive out of dispatch order once several
    // requests run concurrently, so samples can't just be appended to
    // TrackHead::fragSamples as each reply finishes. Concatenated back
    // into fragSamples (in fragment order) once every fragment is in.
    std::vector<std::vector<FragSample> > m_videoFragSlots;
    std::vector<std::vector<FragSample> > m_audioFragSlots;

    // Body-streaming cursor for fragmented sources: which sample index
    // we're currently writing (drives both the per-fragment source Range
    // request and the per-sample output offset), plus a running total of
    // bytes already written for this track so each write's output offset
    // is O(1) to compute instead of re-summing every prior sample size.
    //
    // Samples are NOT fetched one at a time -- a long video can have tens
    // of thousands of samples, and one HTTP Range request per sample was
    // both extremely slow and, worse, occasionally returned a
    // wrong-but-right-sized body (matching the requested Content-Length
    // without actually being that byte range) with no error, silently
    // writing garbage into the output at exactly the point another
    // sample's data belonged -- the video-turns-green/audio-repeats-a-
    // syllable corruption this batching fixes. Instead, requestNextFragBody()
    // groups consecutive samples whose offsetInSource run contiguously
    // (which all samples within -- and typically across -- a fragment do)
    // into a single Range request up to FRAG_BODY_BATCH_BYTES, and the
    // Finished handler splits that one response back into per-sample
    // writes using m_video/audioFragBodyBatchCount.
    // Video: batched + concurrent (see FragBodyBatch above and
    // FRAG_BODY_CONCURRENCY in the .cpp).
    std::vector<FragBodyBatch> m_videoBodyBatches;
    size_t m_videoBodyDispatchIndex; // next batch index still needing a request DISPATCHED
    size_t m_videoBodyCompletedCount; // how many batches have finished (any order)
    QList<QNetworkReply *> m_videoBodyReplies; // in-flight, order not significant
    // Per-batch retry counters for transient body-fetch failures (network
    // error, or a Range request the relay answered with the wrong byte
    // count -- e.g. HTTP 200 with an empty body instead of 206 with the
    // requested range, seen in the field on a slow/flaky relay). Sized
    // alongside m_videoBodyBatches in buildVideoBodyBatches(); a batch
    // that exhausts BODY_MAX_RETRIES still fails the session via
    // failWith() as before. Unlike the moof-discovery retry counters
    // above, these are NOT shared with them -- a body batch and a moof
    // fragment are different requests against different byte ranges.
    std::vector<int> m_videoBodyRetries;

    // Audio: still single-request-at-a-time -- small enough that the
    // extra plumbing for concurrency isn't worth it.
    size_t m_audioFragBodyIndex;
    size_t m_audioFragBodyBatchCount;
    qint64 m_audioFragBodyOffsetSoFar;
    QNetworkReply *m_audioFragBodyReply;
    // Retry counter for the audio body batch currently in flight -- reset
    // to 0 each time m_audioFragBodyIndex advances to a new batch start,
    // since retries re-request the SAME batch (same fragIndex/batchCount)
    // rather than moving the cursor forward. See m_videoBodyRetries above
    // for why this exists.
    int m_audioFragBodyRetryCount;
};

#endif /* STREAMINGREMUXSESSION_HPP_ */
