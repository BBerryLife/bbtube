#include "src/utils/StreamingRemuxSession.hpp"

#include <QNetworkRequest>
#include <QUrl>
#include <QVariant>
#include <QTimer>

#ifdef QT_DEBUG
#include <QDebug>
#endif

// Delay, in ms, before re-issuing a request that just failed with a
// transient network error (dropped/refused connection, timeout). Retrying
// instantly (the original behaviour) works fine for a genuine one-off
// packet-loss blip, but does nothing to help the specific failure mode
// seen in the field: a request reusing a POOLED connection from
// ApplicationUI::networkManager's shared per-host keep-alive pool that
// the remote server (or a NAT/proxy in between) silently closed while
// idle between videos. An instant retry against the same pool just grabs
// another stale socket (or the same one) and fails again immediately --
// this is exactly the "Connection closed" -> "Connection refused" ->
// "Connection refused" -> FAILED sequence seen when switching from one
// video straight into another. A short delay gives Qt's connection cache
// a chance to notice the socket is dead and evict it before the retry,
// and combined with the "Connection: close" header set below (which
// forces that specific retry off the pool entirely), gives the retry a
// real chance of succeeding instead of just repeating the same failure.
static const int RETRY_DELAY_MS = 400;

// How much of each source to fetch first, trying to cover ftyp+moov+sidx in
// one request. Real YouTube adaptiveFormats moov boxes are typically a few
// KB to a few dozen KB, and sidx (when present, for fragmented sources)
// immediately follows moov and is usually small too (a dozen bytes per
// fragment entry) -- 128KB covers this comfortably for the vast majority
// of videos. If parseHead() still can't find a complete moov+mdat-header
// (or, for fragmented sources, moov+sidx) in this many bytes, we double
// the fetch size and retry, up to MAX_HEAD_FETCH_BYTES.
static const int INITIAL_HEAD_FETCH_BYTES = 128 * 1024;
static const int MAX_HEAD_FETCH_BYTES = 4 * 1024 * 1024;

// How many bytes to fetch per fragment when discovering a fragmented
// source's real sample table (moof headers only, not the mdat payload
// that follows each one). moof boxes for adaptiveFormats are typically a
// few hundred bytes to a couple KB even at high sample counts; 8KB gives
// generous headroom without pulling in meaningful mdat payload.
static const int MOOF_DISCOVERY_FETCH_BYTES = 8 * 1024;

// How many fragment moof headers to request concurrently during
// discovery, per track. Sequentially requesting them one at a time (the
// original implementation) meant total discovery latency scaled with
// fragment count * per-request round-trip time -- for a long video with
// hundreds of fragments against a slow/distant Invidious instance this
// could take long enough that the person would give up and switch quality
// mid-discovery, aborting the in-flight session. Running several in
// parallel divides that latency by roughly this factor. Kept modest (not
// dozens) to avoid overwhelming the Invidious instance or BB10's own
// concurrent-connection handling.
static const int MOOF_DISCOVERY_CONCURRENCY = 5;
// Transient network errors on a moof header fetch (dropped connection,
// timeout, occasional edge-server reset when several Range requests hit
// the same videoplayback URL concurrently) are retried this many times
// before giving up and failing the whole remux session.
static const int MOOF_MAX_RETRIES = 3;
// Same idea as MOOF_MAX_RETRIES, but for the actual body (mdat) fetches
// rather than moof header discovery. Seen in the field on a slow/flaky
// relay: an audio body batch's Range request came back HTTP 200 with an
// empty body instead of 206 with the requested bytes ("server may not
// have honored the Range request"), which previously failed the whole
// remux session on the very first such hiccup even though every other
// batch in the same session succeeded. Retry a batch this many times
// before giving up.
static const int BODY_MAX_RETRIES = 3;
// Upper bound, in bytes, for how much a single grouped fragment-body
// Range request is allowed to cover -- see the comment on
// m_video/audioFragBodyBatchCount in the header for why samples are
// batched at all. 4 MiB keeps individual requests/responses (and the
// in-memory QByteArray holding one) comfortably small for BB10's limited
// RAM while still cutting a 44k-sample video down to a few hundred
// requests instead of 44k.
static const qint64 FRAG_BODY_BATCH_BYTES = 4 * 1024 * 1024;
// How many video body batch requests run concurrently. Discovery already
// uses concurrency (MOOF_DISCOVERY_CONCURRENCY) since it's cheap
// (8KB/request); this is the same idea applied to the much larger actual
// sample data, since a single sequential connection's effective
// throughput over a slow relay can fall behind real-time playback speed
// on longer/higher-bitrate videos and stall it. Audio stays
// single-request -- it's small enough that concurrency there wouldn't
// meaningfully change total download time.
// Dialed back from 4 -> 2 after reports of playback failing a few
// seconds in on large videos even though every batch passed its
// byte-count check. Hypothesis: some community Invidious relays don't
// cleanly isolate multiple simultaneous Range requests from the same
// client/connection pool, so a batch can come back the right SIZE but
// with wrong/mixed CONTENT -- which this code has no way to detect (no
// checksum, just a length check). 2 concurrent requests still gives a
// real throughput win over the old fully-sequential approach while
// being less likely to trip that kind of relay-side bug than 4 was. If
// large videos still stall/fail partway through at 2, try 1 (fully
// serial, matching pre-concurrency behavior) to confirm whether this
// really is the cause before looking elsewhere.
static const int FRAG_BODY_CONCURRENCY = 2;

static Mp4RemuxBytes qByteArrayToBytes(const QByteArray &arr)
{
    const uint8_t *p = reinterpret_cast<const uint8_t *>(arr.constData());
    return Mp4RemuxBytes(p, p + arr.size());
}

StreamingRemuxSession::StreamingRemuxSession(QNetworkAccessManager *networkManager,
        const QString &videoUrl, const QString &audioUrl, const QString &outputPath,
        QObject *parent, double maxDurationSeconds) :
        QObject(parent), m_networkManager(networkManager), m_videoUrl(videoUrl), m_audioUrl(
                audioUrl), m_outputPath(outputPath), m_maxDurationSeconds(maxDurationSeconds), m_videoHeadFetchSize(
                INITIAL_HEAD_FETCH_BYTES), m_audioHeadFetchSize(INITIAL_HEAD_FETCH_BYTES), m_videoHeadParsed(
                false), m_audioHeadParsed(false), m_planStarted(false), m_failed(false), m_videoHeadReply(
                0), m_audioHeadReply(0), m_videoBodyReply(0), m_audioBodyReply(0), m_outputFile(0), m_videoBytesWritten(
                0), m_audioDone(false), m_videoDone(false), m_videoFragmentsReady(false), m_audioFragmentsReady(
                false), m_videoFragDiscoveryIndex(0), m_audioFragDiscoveryIndex(0), m_videoFragDiscoveryCompleted(
                0), m_audioFragDiscoveryCompleted(0), m_videoBodyDispatchIndex(
                0), m_videoBodyCompletedCount(0), m_audioFragBodyIndex(
                0), m_audioFragBodyBatchCount(0), m_audioFragBodyOffsetSoFar(0), m_audioFragBodyReply(
                0), m_audioFragBodyRetryCount(0), m_videoHeadRetryCount(0), m_audioHeadRetryCount(
                0), m_pendingMoofRetryIsVideo(false), m_pendingMoofRetryFragIndex(
                0), m_pendingVideoBodyRetryBatchIndex(0)
{
}

StreamingRemuxSession::~StreamingRemuxSession()
{
    if (m_videoHeadReply) { m_videoHeadReply->abort(); m_videoHeadReply->deleteLater(); }
    if (m_audioHeadReply) { m_audioHeadReply->abort(); m_audioHeadReply->deleteLater(); }
    if (m_videoBodyReply) { m_videoBodyReply->abort(); m_videoBodyReply->deleteLater(); }
    if (m_audioBodyReply) { m_audioBodyReply->abort(); m_audioBodyReply->deleteLater(); }
    for (int i = 0; i < m_videoMoofReplies.size(); i++) {
        m_videoMoofReplies[i]->abort();
        m_videoMoofReplies[i]->deleteLater();
    }
    for (int i = 0; i < m_audioMoofReplies.size(); i++) {
        m_audioMoofReplies[i]->abort();
        m_audioMoofReplies[i]->deleteLater();
    }
    for (int i = 0; i < m_videoBodyReplies.size(); i++) {
        m_videoBodyReplies[i]->abort();
        m_videoBodyReplies[i]->deleteLater();
    }
    if (m_audioFragBodyReply) { m_audioFragBodyReply->abort(); m_audioFragBodyReply->deleteLater(); }
    if (m_outputFile) {
        m_outputFile->close();
        delete m_outputFile;
    }
}

void StreamingRemuxSession::cancel()
{
    // Disconnect every signal/slot connection involving this object
    // (both directions: signals this object emits, and any reply/timer
    // signal currently wired to one of this object's slots) before
    // touching any network reply. deleteLater() alone only schedules
    // destruction for the next event loop iteration -- but
    // QNetworkReply::abort() can synchronously emit finished() on some
    // Qt/BB10 builds, and if that happens before this session's own
    // destructor runs, its finished-handler slots (onVideoMoofFinished()
    // etc.) would still fire and touch member state concurrently with,
    // or after, teardown has started: a use-after-free landmine.
    // Disconnecting first guarantees none of this session's own slots can
    // run again, regardless of whether abort() completes synchronously.
    QObject::disconnect(this, 0, 0, 0);

    m_failed = true; // belt-and-suspenders, in case anything still checks it

    if (m_videoHeadReply) { m_videoHeadReply->abort(); m_videoHeadReply->deleteLater(); m_videoHeadReply = 0; }
    if (m_audioHeadReply) { m_audioHeadReply->abort(); m_audioHeadReply->deleteLater(); m_audioHeadReply = 0; }
    if (m_videoBodyReply) { m_videoBodyReply->abort(); m_videoBodyReply->deleteLater(); m_videoBodyReply = 0; }
    if (m_audioBodyReply) { m_audioBodyReply->abort(); m_audioBodyReply->deleteLater(); m_audioBodyReply = 0; }
    for (int i = 0; i < m_videoMoofReplies.size(); i++) {
        m_videoMoofReplies[i]->abort();
        m_videoMoofReplies[i]->deleteLater();
    }
    m_videoMoofReplies.clear();
    for (int i = 0; i < m_audioMoofReplies.size(); i++) {
        m_audioMoofReplies[i]->abort();
        m_audioMoofReplies[i]->deleteLater();
    }
    m_audioMoofReplies.clear();
    for (int i = 0; i < m_videoBodyReplies.size(); i++) {
        m_videoBodyReplies[i]->abort();
        m_videoBodyReplies[i]->deleteLater();
    }
    m_videoBodyReplies.clear();
    if (m_audioFragBodyReply) { m_audioFragBodyReply->abort(); m_audioFragBodyReply->deleteLater(); m_audioFragBodyReply = 0; }
    if (m_outputFile) {
        m_outputFile->close();
    }
}

void StreamingRemuxSession::start()
{
    requestVideoHead();
    requestAudioHead();
}

void StreamingRemuxSession::requestVideoHead()
{
    // See the comment on beginBodyDownloads() below for why fromEncoded()
    // is used here instead of QNetworkRequest(m_videoUrl) directly.
    QNetworkRequest req(QUrl::fromEncoded(m_videoUrl.toUtf8()));
    req.setRawHeader("Range", "bytes=0-" + QByteArray::number(m_videoHeadFetchSize - 1));
    m_videoHeadReply = m_networkManager->get(req);
    QObject::connect(m_videoHeadReply, SIGNAL(finished()), this, SLOT(onVideoHeadFinished()));
}

void StreamingRemuxSession::requestAudioHead()
{
    QNetworkRequest req(QUrl::fromEncoded(m_audioUrl.toUtf8()));
    req.setRawHeader("Range", "bytes=0-" + QByteArray::number(m_audioHeadFetchSize - 1));
    m_audioHeadReply = m_networkManager->get(req);
    QObject::connect(m_audioHeadReply, SIGNAL(finished()), this, SLOT(onAudioHeadFinished()));
}

void StreamingRemuxSession::onVideoHeadFinished()
{
    if (m_failed) return;
    QNetworkReply *reply = m_videoHeadReply;
    m_videoHeadReply = 0;

    if (reply->error()) {
        QString msg = reply->errorString();
        reply->deleteLater();
        if (m_videoHeadRetryCount < BODY_MAX_RETRIES) {
            m_videoHeadRetryCount++;
            qDebug() << "[bbtube][remux] video head fetch failed (" << msg
                     << ") - retrying, attempt" << m_videoHeadRetryCount << "of" << BODY_MAX_RETRIES;
            requestVideoHead();
            return;
        }
        failWith(QString("video head fetch failed after %1 retries: %2").arg(BODY_MAX_RETRIES).arg(
                msg));
        return;
    }

    Mp4RemuxBytes buf = qByteArrayToBytes(reply->readAll());
    reply->deleteLater();

    try {
        m_videoHead = parseHead(buf, "video");
        // Fragmented sources need a sidx box to proceed; if the initial
        // head fetch didn't reach far enough to include one, treat that
        // the same as "not enough head yet" and retry with a bigger
        // fetch, same as the moov-too-small case below.
        if (m_videoHead.isFragmented && !m_videoHead.sidx.found) {
            if (m_videoHeadFetchSize >= MAX_HEAD_FETCH_BYTES) {
                failWith(QString("video head parse failed (giving up after %1 bytes): "
                        "fragmented source but no sidx box found").arg(m_videoHeadFetchSize));
                return;
            }
            m_videoHeadFetchSize *= 2;
            requestVideoHead();
            return;
        }
        // NOTE: m_videoHeadParsed is intentionally NOT set here for the
        // fragmented case -- tryBeginPlan() uses this flag as its sole
        // "video track is ready to plan" gate, and for fragmented
        // sources the track isn't actually ready until fragment
        // discovery has populated fragSamples/mdatBodyOffsetInSource via
        // onTrackFragmentsReady(). Setting it early here let audio
        // finish first and call tryBeginPlan() while m_videoHead was
        // still a default-constructed/partial head (mdatBodyOffsetInSource
        // == 0, fragSamples empty), which planStreamingRemux() would
        // then plan against -- corrupting the output offset table and
        // crashing later on an unrelated allocation.
        if (!m_videoHead.isFragmented) {
            m_videoHeadParsed = true;
        }
    } catch (const std::exception &e) {
        if (m_videoHeadFetchSize >= MAX_HEAD_FETCH_BYTES) {
            failWith(QString("video head parse failed (giving up after %1 bytes): %2")
                    .arg(m_videoHeadFetchSize).arg(e.what()));
            return;
        }
#ifdef QT_DEBUG
        qDebug() << "[bbtube][remux] video head" << m_videoHeadFetchSize
                 << "bytes not enough (" << e.what() << ") - doubling and retrying";
#endif
        m_videoHeadFetchSize *= 2;
        requestVideoHead();
        return;
    }

    if (m_videoHead.isFragmented) {
        beginFragmentDiscovery(m_videoHead, /*isVideoTrack=*/true);
    } else {
        tryBeginPlan();
    }
}

void StreamingRemuxSession::onAudioHeadFinished()
{
    if (m_failed) return;
    QNetworkReply *reply = m_audioHeadReply;
    m_audioHeadReply = 0;

    if (reply->error()) {
        QString msg = reply->errorString();
        reply->deleteLater();
        if (m_audioHeadRetryCount < BODY_MAX_RETRIES) {
            m_audioHeadRetryCount++;
            qDebug() << "[bbtube][remux] audio head fetch failed (" << msg
                     << ") - retrying, attempt" << m_audioHeadRetryCount << "of" << BODY_MAX_RETRIES;
            requestAudioHead();
            return;
        }
        failWith(QString("audio head fetch failed after %1 retries: %2").arg(BODY_MAX_RETRIES).arg(
                msg));
        return;
    }

    Mp4RemuxBytes buf = qByteArrayToBytes(reply->readAll());
    reply->deleteLater();

    try {
        m_audioHead = parseHead(buf, "audio");
        if (m_audioHead.isFragmented && !m_audioHead.sidx.found) {
            if (m_audioHeadFetchSize >= MAX_HEAD_FETCH_BYTES) {
                failWith(QString("audio head parse failed (giving up after %1 bytes): "
                        "fragmented source but no sidx box found").arg(m_audioHeadFetchSize));
                return;
            }
            m_audioHeadFetchSize *= 2;
            requestAudioHead();
            return;
        }
        // See matching comment in onVideoHeadFinished(): don't mark the
        // track "parsed"/ready-to-plan here for fragmented sources --
        // that only becomes true once fragment discovery finishes
        // (onTrackFragmentsReady), or tryBeginPlan() can run against a
        // still-empty m_audioHead if video's head happens to finish
        // first.
        if (!m_audioHead.isFragmented) {
            m_audioHeadParsed = true;
        }
    } catch (const std::exception &e) {
        if (m_audioHeadFetchSize >= MAX_HEAD_FETCH_BYTES) {
            failWith(QString("audio head parse failed (giving up after %1 bytes): %2")
                    .arg(m_audioHeadFetchSize).arg(e.what()));
            return;
        }
#ifdef QT_DEBUG
        qDebug() << "[bbtube][remux] audio head" << m_audioHeadFetchSize
                 << "bytes not enough (" << e.what() << ") - doubling and retrying";
#endif
        m_audioHeadFetchSize *= 2;
        requestAudioHead();
        return;
    }

    if (m_audioHead.isFragmented) {
        beginFragmentDiscovery(m_audioHead, /*isVideoTrack=*/false);
    } else {
        tryBeginPlan();
    }
}

// ---------------------------------------------------------------------------
// Fragment discovery (fragmented sources only): walk this track's sidx
// entries in order, fetching each fragment's small moof header (not its
// mdat payload) and accumulating decoded samples into track.fragSamples.
// Once every fragment has been visited, build the track's progressive
// sample tables from the accumulated samples and fall through to the
// normal tryBeginPlan()/beginBodyDownloads() flow, which from this point
// on treats the track exactly like a non-fragmented one EXCEPT that body
// streaming must go through the fragment-by-fragment path (see
// beginFragmentedBodyDownloads()) since the source's mdat data isn't one
// contiguous range.
// ---------------------------------------------------------------------------

void StreamingRemuxSession::beginFragmentDiscovery(TrackHead &track, bool isVideoTrack)
{
    // Precompute each fragment's absolute source offset up front from
    // sidx (a simple running sum) so requests can be dispatched
    // out-of-order without needing a single sequential cursor.
    std::vector<uint64_t> &offsets = isVideoTrack ? m_videoFragOffsets : m_audioFragOffsets;
    offsets.clear();
    offsets.reserve(track.sidx.entries.size());
    uint64_t running = uint64_t(track.sidx.firstFragmentOffset);
    for (size_t i = 0; i < track.sidx.entries.size(); i++) {
        offsets.push_back(running);
        running += track.sidx.entries[i].referencedSize;
    }

    if (isVideoTrack) {
        m_videoFragDiscoveryIndex = 0;
        m_videoFragDiscoveryCompleted = 0;
    } else {
        m_audioFragDiscoveryIndex = 0;
        m_audioFragDiscoveryCompleted = 0;
    }

    // Reserve space in fragSamples-per-fragment storage: we can't append
    // directly to track.fragSamples as requests complete since they don't
    // finish in order, so stash each fragment's samples in a slot indexed
    // by fragment number and concatenate once every fragment is in.
    track.fragSamples.clear(); // will be rebuilt from the per-fragment slots

    qDebug() << "[bbtube][remux][debug] beginFragmentDiscovery" << (isVideoTrack ? "video" : "audio")
             << "sidx.found=" << track.sidx.found
             << "sidx.firstFragmentOffset=" << qint64(track.sidx.firstFragmentOffset)
             << "sidx.entries.size()=" << qint64(track.sidx.entries.size());
    for (size_t i = 0; i < track.sidx.entries.size() && i < 5; i++) {
        qDebug() << "[bbtube][remux][debug]   sidx.entries[" << qint64(i) << "] referencedSize="
                 << qint64(track.sidx.entries[i].referencedSize) << "subsegmentDuration="
                 << qint64(track.sidx.entries[i].subsegmentDuration);
    }

    if (track.sidx.entries.empty()) {
        failWith(QString("%1: sidx box has no fragment entries")
                .arg(isVideoTrack ? "video" : "audio"));
        return;
    }

    if (isVideoTrack) {
        m_videoFragSlots.assign(track.sidx.entries.size(), std::vector<FragSample>());
        m_videoFragRetries.assign(track.sidx.entries.size(), 0);
    } else {
        m_audioFragSlots.assign(track.sidx.entries.size(), std::vector<FragSample>());
        m_audioFragRetries.assign(track.sidx.entries.size(), 0);
    }

    dispatchMoofRequests(isVideoTrack);
}

// Keeps up to MOOF_DISCOVERY_CONCURRENCY moof requests in flight for this
// track at once. Called both to kick off the initial batch and again
// after each completion to keep the window full until every fragment has
// been dispatched.
void StreamingRemuxSession::dispatchMoofRequests(bool isVideoTrack)
{
    TrackHead &track = isVideoTrack ? m_videoHead : m_audioHead;
    size_t &nextIdx = isVideoTrack ? m_videoFragDiscoveryIndex : m_audioFragDiscoveryIndex;
    QList<QNetworkReply *> &inFlight = isVideoTrack ? m_videoMoofReplies : m_audioMoofReplies;
    const std::vector<uint64_t> &offsets = isVideoTrack ? m_videoFragOffsets : m_audioFragOffsets;
    QString url = isVideoTrack ? m_videoUrl : m_audioUrl;

    while (inFlight.size() < MOOF_DISCOVERY_CONCURRENCY && nextIdx < track.sidx.entries.size()) {
        uint64_t offset = offsets[nextIdx];
        QNetworkRequest req(QUrl::fromEncoded(url.toUtf8()));
        qint64 rangeEnd = qint64(offset) + MOOF_DISCOVERY_FETCH_BYTES - 1;
        QByteArray rangeHeader = "bytes=" + QByteArray::number(qint64(offset)) + "-"
                + QByteArray::number(rangeEnd);
        req.setRawHeader("Range", rangeHeader);

        qDebug() << "[bbtube][remux][debug] dispatchMoofRequests" << (isVideoTrack ? "video" : "audio")
                 << "fragIndex=" << qint64(nextIdx) << "offset=" << qint64(offset)
                 << "range=" << rangeHeader << "inFlight=" << inFlight.size() + 1;

        QNetworkReply *reply = m_networkManager->get(req);
        // fragIndex travels with the reply itself (rather than being
        // inferred from completion order, which is no longer meaningful
        // once several requests are in flight at once).
        reply->setProperty("fragIndex", qint64(nextIdx));
        inFlight.append(reply);
        if (isVideoTrack) {
            QObject::connect(reply, SIGNAL(finished()), this, SLOT(onVideoMoofFinished()));
        } else {
            QObject::connect(reply, SIGNAL(finished()), this, SLOT(onAudioMoofFinished()));
        }
        nextIdx++;
    }
}

// Re-issues the moof request for a single fragment that just failed with
// a (presumed transient) network error, without touching the discovery
// cursor/window -- dispatchMoofRequests() only ever moves nextIdx
// forward, so a failed fragment's slot would otherwise never be
// requested again. The reply is wired to the same onVideo/AudioMoofFinished
// slot as a normal dispatch; that slot's existing retry-count check
// decides whether a further failure retries again or gives up.
void StreamingRemuxSession::retryMoofRequest(bool isVideoTrack, size_t fragIndex)
{
    // Delayed via a timer rather than dispatched immediately -- see
    // RETRY_DELAY_MS for why (stale pooled connection from
    // ApplicationUI::networkManager's shared per-host keep-alive pool).
    // onMoofRetryTimer() does the actual m_networkManager->get() once the
    // timer fires, reading the target back from these members.
    m_pendingMoofRetryIsVideo = isVideoTrack;
    m_pendingMoofRetryFragIndex = fragIndex;
    QTimer::singleShot(RETRY_DELAY_MS, this, SLOT(onMoofRetryTimer()));
}

void StreamingRemuxSession::onMoofRetryTimer()
{
    if (m_failed) return;

    bool isVideoTrack = m_pendingMoofRetryIsVideo;
    size_t fragIndex = m_pendingMoofRetryFragIndex;

    TrackHead &track = isVideoTrack ? m_videoHead : m_audioHead;
    const std::vector<uint64_t> &offsets = isVideoTrack ? m_videoFragOffsets : m_audioFragOffsets;
    QList<QNetworkReply *> &inFlight = isVideoTrack ? m_videoMoofReplies : m_audioMoofReplies;
    QString url = isVideoTrack ? m_videoUrl : m_audioUrl;
    int retryCount = isVideoTrack ? m_videoFragRetries[fragIndex] : m_audioFragRetries[fragIndex];

    uint64_t offset = offsets[fragIndex];
    QNetworkRequest req(QUrl::fromEncoded(url.toUtf8()));
    qint64 rangeEnd = qint64(offset) + MOOF_DISCOVERY_FETCH_BYTES - 1;
    QByteArray rangeHeader = "bytes=" + QByteArray::number(qint64(offset)) + "-"
            + QByteArray::number(rangeEnd);
    req.setRawHeader("Range", rangeHeader);
    // Force this retry off the shared connection pool -- if the original
    // failure was a stale/half-closed pooled socket, reusing the pool
    // again here just risks grabbing another dead one (or the same one).
    req.setRawHeader("Connection", "close");

    qDebug() << "[bbtube][remux][debug] retryMoofRequest" << (isVideoTrack ? "video" : "audio")
             << "fragIndex=" << qint64(fragIndex) << "attempt=" << (retryCount + 1)
             << "of" << MOOF_MAX_RETRIES << "offset=" << qint64(offset) << "range=" << rangeHeader;

    QNetworkReply *reply = m_networkManager->get(req);
    reply->setProperty("fragIndex", qint64(fragIndex));
    inFlight.append(reply);
    if (isVideoTrack) {
        QObject::connect(reply, SIGNAL(finished()), this, SLOT(onVideoMoofFinished()));
    } else {
        QObject::connect(reply, SIGNAL(finished()), this, SLOT(onAudioMoofFinished()));
    }
    (void)track; // kept for symmetry/future use, silences unused-variable warning on some toolchains

    // All fragments dispatched and none still in flight -- but only once
    // every fragment has actually COMPLETED (not just been dispatched) is
    // discovery really done; onVideoMoofFinished()/onAudioMoofFinished()
    // check that and call onTrackFragmentsReady() themselves once the
    // completed count catches up, so there's nothing further to do here
    // in that case.
}

void StreamingRemuxSession::onVideoMoofFinished()
{
    if (m_failed) return;
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(QObject::sender());
    m_videoMoofReplies.removeOne(reply);
    qint64 fragIndex = reply->property("fragIndex").toLongLong();

    qDebug() << "[bbtube][remux][debug] onVideoMoofFinished fragIndex=" << fragIndex
             << "error=" << int(reply->error()) << "errorString=" << reply->errorString()
             << "httpStatus=" << reply->attribute(QNetworkRequest::HttpStatusCodeAttribute)
             << "bytesAvailable=" << reply->bytesAvailable() << "url=" << reply->url().toString();

    if (reply->error()) {
        QString msg = reply->errorString();
        reply->deleteLater();
        // Transient network errors (dropped connection, timeout, an edge
        // server hiccup from MOOF_DISCOVERY_CONCURRENCY simultaneous
        // Range requests hitting the same videoplayback URL) are common
        // enough here that failing the whole remux session on the first
        // one is too aggressive -- retry this single fragment a few
        // times before giving up.
        int &retryCount = m_videoFragRetries[size_t(fragIndex)];
        if (retryCount < MOOF_MAX_RETRIES) {
            retryCount++;
            qDebug() << "[bbtube][remux] video fragment" << fragIndex
                     << "header fetch failed (" << msg << ") - retrying, attempt"
                     << retryCount << "of" << MOOF_MAX_RETRIES;
            retryMoofRequest(true, size_t(fragIndex));
            return;
        }
        failWith(QString("video fragment %1 header fetch failed after %2 retries: %3")
                .arg(fragIndex).arg(MOOF_MAX_RETRIES).arg(msg));
        return;
    }
    Mp4RemuxBytes buf = qByteArrayToBytes(reply->readAll());
    reply->deleteLater();

    uint64_t fragOffset = m_videoFragOffsets[size_t(fragIndex)];
    try {
        m_videoFragSlots[size_t(fragIndex)] = parseMoofSamples(buf, size_t(fragOffset), "video");
    } catch (const std::exception &e) {
        failWith(QString("video fragment %1 moof parse failed: %2").arg(fragIndex).arg(e.what()));
        return;
    }

    m_videoFragDiscoveryCompleted++;
    if (m_videoFragDiscoveryCompleted >= m_videoHead.sidx.entries.size()) {
        // Every fragment's samples are in (regardless of completion
        // order) -- concatenate the per-fragment slots back into
        // fragSamples in fragment order and move on.
        m_videoHead.fragSamples.clear();
        for (size_t i = 0; i < m_videoFragSlots.size(); i++) {
            m_videoHead.fragSamples.insert(m_videoHead.fragSamples.end(),
                    m_videoFragSlots[i].begin(), m_videoFragSlots[i].end());
        }
        onTrackFragmentsReady(true);
        return;
    }
    dispatchMoofRequests(true);
}

void StreamingRemuxSession::onAudioMoofFinished()
{
    if (m_failed) return;
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(QObject::sender());
    m_audioMoofReplies.removeOne(reply);
    qint64 fragIndex = reply->property("fragIndex").toLongLong();

    qDebug() << "[bbtube][remux][debug] onAudioMoofFinished fragIndex=" << fragIndex
             << "error=" << int(reply->error()) << "errorString=" << reply->errorString()
             << "httpStatus=" << reply->attribute(QNetworkRequest::HttpStatusCodeAttribute)
             << "bytesAvailable=" << reply->bytesAvailable() << "url=" << reply->url().toString();

    if (reply->error()) {
        QString msg = reply->errorString();
        reply->deleteLater();
        // See matching comment in onVideoMoofFinished(): retry a single
        // fragment a few times on transient network errors before
        // failing the whole session.
        int &retryCount = m_audioFragRetries[size_t(fragIndex)];
        if (retryCount < MOOF_MAX_RETRIES) {
            retryCount++;
            qDebug() << "[bbtube][remux] audio fragment" << fragIndex
                     << "header fetch failed (" << msg << ") - retrying, attempt"
                     << retryCount << "of" << MOOF_MAX_RETRIES;
            retryMoofRequest(false, size_t(fragIndex));
            return;
        }
        failWith(QString("audio fragment %1 header fetch failed after %2 retries: %3")
                .arg(fragIndex).arg(MOOF_MAX_RETRIES).arg(msg));
        return;
    }
    Mp4RemuxBytes buf = qByteArrayToBytes(reply->readAll());
    reply->deleteLater();

    uint64_t fragOffset = m_audioFragOffsets[size_t(fragIndex)];
    try {
        m_audioFragSlots[size_t(fragIndex)] = parseMoofSamples(buf, size_t(fragOffset), "audio");
    } catch (const std::exception &e) {
        failWith(QString("audio fragment %1 moof parse failed: %2").arg(fragIndex).arg(e.what()));
        return;
    }

    m_audioFragDiscoveryCompleted++;
    if (m_audioFragDiscoveryCompleted >= m_audioHead.sidx.entries.size()) {
        m_audioHead.fragSamples.clear();
        for (size_t i = 0; i < m_audioFragSlots.size(); i++) {
            m_audioHead.fragSamples.insert(m_audioHead.fragSamples.end(),
                    m_audioFragSlots[i].begin(), m_audioFragSlots[i].end());
        }
        onTrackFragmentsReady(false);
        return;
    }
    dispatchMoofRequests(false);
}

// Preview-mode support -- see the header comment on truncateTrackForPreview().
void StreamingRemuxSession::truncateTrackForPreview(TrackHead &track)
{
    if (m_maxDurationSeconds <= 0.0 || track.fragSamples.empty() || track.timescale == 0) {
        return;
    }

    uint64_t maxTicks = uint64_t(m_maxDurationSeconds * double(track.timescale));
    uint64_t cumulative = 0;
    size_t cutIdx = track.fragSamples.size();
    for (size_t i = 0; i < track.fragSamples.size(); i++) {
        cumulative += track.fragSamples[i].duration;
        if (cumulative >= maxTicks) {
            cutIdx = i + 1; // keep this sample -- it's the one that crosses the cutoff
            break;
        }
    }
    if (cutIdx >= track.fragSamples.size()) {
        return; // track is already shorter than the cap -- nothing to trim
    }

    track.fragSamples.resize(cutIdx);
    track.duration = cumulative;

#ifdef QT_DEBUG
    qDebug() << "[bbtube][remux] preview: truncated" << QString::fromStdString(track.label)
             << "to" << qint64(cutIdx) << "samples (" << (double(cumulative) / track.timescale)
             << "s)";
#endif
}

void StreamingRemuxSession::onTrackFragmentsReady(bool isVideoTrack)
{
    TrackHead &track = isVideoTrack ? m_videoHead : m_audioHead;
    truncateTrackForPreview(track);
    try {
        buildProgressiveTablesFromFragments(track);
    } catch (const std::exception &e) {
        failWith(QString("%1: failed building sample table from fragments: %2")
                .arg(isVideoTrack ? "video" : "audio").arg(e.what()));
        return;
    }

    if (isVideoTrack) {
        m_videoFragmentsReady = true;
        m_videoHeadParsed = true;
    } else {
        m_audioFragmentsReady = true;
        m_audioHeadParsed = true;
    }

#ifdef QT_DEBUG
    qDebug() << "[bbtube][remux]" << (isVideoTrack ? "video" : "audio")
             << "fragment discovery complete -" << qint64(track.fragSamples.size())
             << "samples across" << qint64(track.sidx.entries.size()) << "fragments";
#endif

    tryBeginPlan();
}

void StreamingRemuxSession::tryBeginPlan()
{
    if (m_planStarted || !m_videoHeadParsed || !m_audioHeadParsed) {
        return;
    }
    m_planStarted = true;

    std::string err;
    if (!planStreamingRemux(m_videoHead, m_audioHead, &m_plan, &err)) {
        failWith("remux planning failed: " + QString::fromStdString(err));
        return;
    }
    if (!preallocateAndWriteHead(m_outputPath.toStdString(), m_plan, &err)) {
        failWith("could not allocate output file: " + QString::fromStdString(err));
        return;
    }

#ifdef QT_DEBUG
    qDebug() << "[bbtube][remux] plan ready - total output"
             << qint64(m_plan.totalOutputSize) << "bytes, video body"
             << qint64(m_plan.videoBodySize) << "bytes, audio body"
             << qint64(m_plan.audioBodySize) << "bytes -" << m_outputPath;
#endif

    emit headReady();

    if (m_videoHead.isFragmented || m_audioHead.isFragmented) {
        beginFragmentedBodyDownloads();
    } else {
        beginBodyDownloads();
    }
}

void StreamingRemuxSession::beginBodyDownloads()
{
    // Keep one QFile open for the duration of the (potentially long,
    // chunk-by-chunk) video body stream, instead of reopening the file on
    // every readyRead() -- audio is written in one shot separately below
    // since it's small and typically arrives as 1-2 chunks anyway.
    m_outputFile = new QFile(m_outputPath);
    if (!m_outputFile->open(QIODevice::ReadWrite)) {
        failWith("could not reopen output file for streaming writes: " + m_outputPath);
        return;
    }

    // m_videoUrl/m_audioUrl come straight from YouTube's videoplayback
    // URLs (via Invidious or InnerTube directly) and are ALREADY
    // percent-encoded (e.g. "aitags=133%2C134..."). QNetworkRequest's
    // QString constructor implicitly converts through QUrl(QString),
    // which treats the input as a *human-readable* URL and re-encodes
    // it -- turning an existing "%2C" into "%252C" (the literal '%'
    // getting encoded a second time) and corrupting the query string.
    // QUrl::fromEncoded() instead takes the bytes as already-valid
    // percent-encoded form and leaves them alone. This bug was silent
    // until the Invidious remux path was in regular use, since
    // (informally observed) not every server/instance seemed to choke
    // on the double-encoded query the same way.
    QNetworkRequest audioReq(QUrl::fromEncoded(m_audioUrl.toUtf8()));
    audioReq.setRawHeader("Range",
            "bytes=" + QByteArray::number(qint64(m_audioHead.mdatBodyOffsetInSource)) + "-");
    m_audioBodyReply = m_networkManager->get(audioReq);
    QObject::connect(m_audioBodyReply, SIGNAL(finished()), this, SLOT(onAudioBodyFinished()));

    QNetworkRequest videoReq(QUrl::fromEncoded(m_videoUrl.toUtf8()));
    videoReq.setRawHeader("Range",
            "bytes=" + QByteArray::number(qint64(m_videoHead.mdatBodyOffsetInSource)) + "-");
    m_videoBodyReply = m_networkManager->get(videoReq);
    QObject::connect(m_videoBodyReply, SIGNAL(readyRead()), this, SLOT(onVideoBodyReadyRead()));
    QObject::connect(m_videoBodyReply, SIGNAL(finished()), this, SLOT(onVideoBodyFinished()));

#ifdef QT_DEBUG
    qDebug() << "[bbtube][remux][debug] beginBodyDownloads dispatched audio+video body requests";
#endif
}

void StreamingRemuxSession::onAudioBodyFinished()
{
    if (m_failed) return;
    QNetworkReply *reply = m_audioBodyReply;
    m_audioBodyReply = 0;

#ifdef QT_DEBUG
    qDebug() << "[bbtube][remux][debug] onAudioBodyFinished error=" << int(reply->error())
              << "httpStatus=" << reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
#endif

    if (reply->error()) {
        QString msg = reply->errorString();
        reply->deleteLater();
        failWith("audio body download failed: " + msg);
        return;
    }

    QByteArray body = reply->readAll();
    reply->deleteLater();

    if (quint64(body.size()) != m_plan.audioBodySize) {
        // Server may not have honored the Range request (some CDNs ignore
        // Range on certain error/redirect paths) -- this would silently
        // corrupt the remux, so fail loudly instead.
        failWith(QString("audio body size mismatch: expected %1, got %2 -- server may not "
                "have honored the Range request").arg(qint64(m_plan.audioBodySize)).arg(
                body.size()));
        return;
    }

    if (!m_outputFile->seek(qint64(m_plan.audioOutputOffset))
            || m_outputFile->write(body) != body.size()) {
        failWith("failed writing audio body to output file");
        return;
    }

    // Flush Qt's internal write buffer to the OS before telling anyone
    // audio is ready to play. mediaPlayer's playback happens in a
    // separate process (mm-renderer) that opens this same path
    // independently of our QFile handle -- without an explicit flush,
    // the just-written audio mdat bytes can still be sitting in QFile's
    // buffer (not yet handed to the kernel) when mm-renderer opens and
    // reads the file, which is exactly the kind of race that produces an
    // immediate attachInput failure or a truncated/garbled first few
    // seconds of audio.
    m_outputFile->flush();

    m_audioDone = true;
    emit audioComplete();
    checkAllDone();
}

void StreamingRemuxSession::onVideoBodyReadyRead()
{
    if (m_failed || !m_videoBodyReply) return;

    QByteArray chunk = m_videoBodyReply->readAll();
    if (chunk.isEmpty()) return;

    if (!m_outputFile->seek(qint64(m_plan.videoOutputOffset) + m_videoBytesWritten)
            || m_outputFile->write(chunk) != chunk.size()) {
        failWith("failed writing video chunk to output file");
        return;
    }

    m_videoBytesWritten += chunk.size();
    emit progress(m_videoBytesWritten, qint64(m_plan.videoBodySize));
}

void StreamingRemuxSession::onVideoBodyFinished()
{
    if (m_failed) return;
    QNetworkReply *reply = m_videoBodyReply;
    m_videoBodyReply = 0;

    if (reply->error()) {
        QString msg = reply->errorString();
        reply->deleteLater();
        failWith("video body download failed: " + msg);
        return;
    }
    // Drain anything left in the reply's buffer that readyRead() hadn't
    // been fired for yet before finished().
    QByteArray tail = reply->readAll();
    reply->deleteLater();
    if (!tail.isEmpty()) {
        if (!m_outputFile->seek(qint64(m_plan.videoOutputOffset) + m_videoBytesWritten)
                || m_outputFile->write(tail) != tail.size()) {
            failWith("failed writing final video chunk to output file");
            return;
        }
        m_videoBytesWritten += tail.size();
        emit progress(m_videoBytesWritten, qint64(m_plan.videoBodySize));
    }

    if (quint64(m_videoBytesWritten) != m_plan.videoBodySize) {
        failWith(QString("video body size mismatch: expected %1, wrote %2 -- server may not "
                "have honored the Range request").arg(qint64(m_plan.videoBodySize)).arg(
                m_videoBytesWritten));
        return;
    }

    m_videoDone = true;
    checkAllDone();
}

// ---------------------------------------------------------------------------
// Fragment-by-fragment body streaming (fragmented sources only). Unlike
// the contiguous-mdat path above (one long Range request per track), each
// sample of a fragmented track was recorded during discovery with its own
// absolute source offset (FragSample::offsetInSource) and its own output
// position was assigned by planStreamingRemux()'s stco patching -- so here
// we walk track.fragSamples in order, one small Range request per sample,
// writing each straight to its final output offset. This is chattier than
// the old one-request-per-track approach, but samples for adaptiveFormats
// video are typically dozens of KB each (audio samples smaller), so the
// request overhead stays proportionally small next to the payload.
// ---------------------------------------------------------------------------

void StreamingRemuxSession::beginFragmentedBodyDownloads()
{
    m_outputFile = new QFile(m_outputPath);
    if (!m_outputFile->open(QIODevice::ReadWrite)) {
        failWith("could not reopen output file for streaming writes: " + m_outputPath);
        return;
    }

#ifdef QT_DEBUG
    qDebug() << "[bbtube][remux][debug] beginFragmentedBodyDownloads output file reopened,"
             << "starting body downloads -- video samples ="
             << qint64(m_videoHead.fragSamples.size()) << "audio samples ="
             << qint64(m_audioHead.fragSamples.size());
#endif

    m_audioFragBodyIndex = 0;
    m_audioFragBodyBatchCount = 0;
    m_audioFragBodyOffsetSoFar = 0;
    buildVideoBodyBatches();
    if (m_videoBodyBatches.empty()) {
        // No video samples at all (shouldn't normally happen, but don't
        // hang waiting for a completion that can never come).
        m_videoDone = true;
        checkAllDone();
    } else {
        dispatchVideoBodyBatches();
    }
    requestNextFragBody(false);
}

// Audio only now (see class-level comment on onVideoBodyBatchFinished()
// for why video moved to a separate, concurrent path). Kept as a single
// sequential request-then-next-request cursor since audio is small
// enough that it isn't worth the extra batch-list plumbing.
void StreamingRemuxSession::requestNextFragBody(bool isVideoTrack)
{
    requestNextFragBodyImpl(isVideoTrack, false);
}

// isRetry forces the request off the shared connection pool (Connection:
// close) -- see the comment on RETRY_DELAY_MS for why a retry needs that.
// Split out from requestNextFragBody() so the normal advancing-forward
// call site doesn't pay that cost on every batch, only on an actual retry.
void StreamingRemuxSession::requestNextFragBodyImpl(bool isVideoTrack, bool isRetry)
{
    (void)isVideoTrack; // always false now; kept in the signature to match the .hpp/dispatchMoofRequests style

    if (m_audioFragBodyIndex >= m_audioHead.fragSamples.size()) {
        m_audioDone = true;
        emit audioComplete();
        checkAllDone();
        return;
    }

    // Grow the batch by adding consecutive samples as long as each next
    // sample's offsetInSource picks up exactly where the previous one's
    // data ends (i.e. there's no gap to skip over with a second Range
    // request) and the total stays under FRAG_BODY_BATCH_BYTES.
    const std::vector<FragSample> &samples = m_audioHead.fragSamples;
    size_t idx = m_audioFragBodyIndex;
    size_t endIdx = idx + 1;
    qint64 batchBytes = qint64(samples[idx].size);
    while (endIdx < samples.size()) {
        const FragSample &prev = samples[endIdx - 1];
        const FragSample &next = samples[endIdx];
        bool contiguous = (prev.offsetInSource + prev.size) == next.offsetInSource;
        if (!contiguous || batchBytes + qint64(next.size) > FRAG_BODY_BATCH_BYTES) {
            break;
        }
        batchBytes += qint64(next.size);
        endIdx++;
    }
    m_audioFragBodyBatchCount = endIdx - idx;

    QNetworkRequest req(QUrl::fromEncoded(m_audioUrl.toUtf8()));
    qint64 rangeStart = qint64(samples[idx].offsetInSource);
    qint64 rangeEnd = rangeStart + batchBytes - 1;
    req.setRawHeader("Range",
            "bytes=" + QByteArray::number(rangeStart) + "-" + QByteArray::number(rangeEnd));
    if (isRetry) {
        req.setRawHeader("Connection", "close");
    }

#ifdef QT_DEBUG
    qDebug() << "[bbtube][remux][debug] dispatchAudioBodyRequest fragIndex=" << qint64(idx)
              << "batchCount=" << qint64(m_audioFragBodyBatchCount) << "range=" << rangeStart
              << "-" << rangeEnd;
#endif

    QNetworkReply *reply = m_networkManager->get(req);
    m_audioFragBodyReply = reply;
    QObject::connect(reply, SIGNAL(finished()), this, SLOT(onAudioFragBodyFinished()));
}

void StreamingRemuxSession::onAudioFragBodyRetryTimer()
{
    if (m_failed) return;
    requestNextFragBodyImpl(false, true);
}

// Splits the video track's samples into a fixed list of contiguous-range
// batches, computed once up front so every batch's absolute output
// offset is known in advance -- see buildVideoBodyBatches()'s use of a
// running prefix-sum below. That's what makes it safe to have several
// batches' requests in flight (and completing) in any order, unlike the
// old single-cursor approach where the next request could only be formed
// once the previous one's byte count was known.
void StreamingRemuxSession::buildVideoBodyBatches()
{
    m_videoBodyBatches.clear();
    m_videoBodyDispatchIndex = 0;
    m_videoBodyCompletedCount = 0;

    const std::vector<FragSample> &samples = m_videoHead.fragSamples;
    qint64 outOffsetCursor = qint64(m_plan.videoOutputOffset);
    size_t idx = 0;
    while (idx < samples.size()) {
        // Grow the batch by adding consecutive samples as long as each
        // next sample's offsetInSource picks up exactly where the
        // previous one's data ends (i.e. there's no gap to skip over
        // with a second Range request) and the total stays under
        // FRAG_BODY_BATCH_BYTES. This is the normal case both within a
        // fragment and, in practice, across fragment boundaries too,
        // since fragments' mdat payloads are laid out back-to-back in
        // the source.
        size_t endIdx = idx + 1;
        qint64 batchBytes = qint64(samples[idx].size);
        while (endIdx < samples.size()) {
            const FragSample &prev = samples[endIdx - 1];
            const FragSample &next = samples[endIdx];
            bool contiguous = (prev.offsetInSource + prev.size) == next.offsetInSource;
            if (!contiguous || batchBytes + qint64(next.size) > FRAG_BODY_BATCH_BYTES) {
                break;
            }
            batchBytes += qint64(next.size);
            endIdx++;
        }

        FragBodyBatch batch;
        batch.startIdx = idx;
        batch.count = endIdx - idx;
        batch.rangeStart = qint64(samples[idx].offsetInSource);
        batch.batchBytes = batchBytes;
        batch.outOffset = outOffsetCursor;
        m_videoBodyBatches.push_back(batch);

        outOffsetCursor += batchBytes;
        idx = endIdx;
    }

    m_videoBodyRetries.assign(m_videoBodyBatches.size(), 0);
}

// Keeps up to FRAG_BODY_CONCURRENCY video body batch requests in flight
// at once. Called both to kick off the initial window and again after
// each completion to keep it full until every batch has been dispatched.
// Running several batches at once (instead of the previous strictly
// one-at-a-time approach) matters for longer/higher-bitrate videos: over
// a slow relay, a single sequential connection's effective throughput
// can fall behind real-time playback speed and stall it, even though the
// relay has spare capacity for more parallel connections.
void StreamingRemuxSession::dispatchVideoBodyBatches()
{
    while (m_videoBodyReplies.size() < FRAG_BODY_CONCURRENCY
            && m_videoBodyDispatchIndex < m_videoBodyBatches.size()) {
        const FragBodyBatch &batch = m_videoBodyBatches[m_videoBodyDispatchIndex];

        QNetworkRequest req(QUrl::fromEncoded(m_videoUrl.toUtf8()));
        qint64 rangeEnd = batch.rangeStart + batch.batchBytes - 1;
        req.setRawHeader("Range",
                "bytes=" + QByteArray::number(batch.rangeStart) + "-"
                        + QByteArray::number(rangeEnd));

#ifdef QT_DEBUG
        qDebug() << "[bbtube][remux][debug] dispatchVideoBodyRequest batchIndex="
                  << qint64(m_videoBodyDispatchIndex) << "range=" << batch.rangeStart << "-"
                  << rangeEnd << "inFlight=" << qint64(m_videoBodyReplies.size() + 1);
#endif

        QNetworkReply *reply = m_networkManager->get(req);
        // Batch index travels with the reply itself, since completions
        // can arrive out of dispatch order once several are in flight.
        reply->setProperty("batchIndex", qint64(m_videoBodyDispatchIndex));
        m_videoBodyReplies.append(reply);
        QObject::connect(reply, SIGNAL(finished()), this, SLOT(onVideoBodyBatchFinished()));

        m_videoBodyDispatchIndex++;
    }
}

// Re-issues the Range request for a single video body batch that just
// failed (network error or wrong-sized response), without touching
// m_videoBodyDispatchIndex -- dispatchVideoBodyBatches() only ever moves
// that cursor forward to reach undispatched batches, so a failed batch's
// slot would otherwise never be requested again. Wired to the same
// onVideoBodyBatchFinished() slot as a normal dispatch; that slot's
// existing retry-count check decides whether a further failure retries
// again or gives up.
void StreamingRemuxSession::retryVideoBodyBatch(size_t batchIndex)
{
    // Delayed via a timer -- see RETRY_DELAY_MS / onMoofRetryTimer() above
    // for why. onVideoBodyBatchRetryTimer() does the actual dispatch once
    // the timer fires.
    m_pendingVideoBodyRetryBatchIndex = batchIndex;
    QTimer::singleShot(RETRY_DELAY_MS, this, SLOT(onVideoBodyBatchRetryTimer()));
}

void StreamingRemuxSession::onVideoBodyBatchRetryTimer()
{
    if (m_failed) return;

    size_t batchIndex = m_pendingVideoBodyRetryBatchIndex;
    const FragBodyBatch &batch = m_videoBodyBatches[batchIndex];

    QNetworkRequest req(QUrl::fromEncoded(m_videoUrl.toUtf8()));
    qint64 rangeEnd = batch.rangeStart + batch.batchBytes - 1;
    req.setRawHeader("Range",
            "bytes=" + QByteArray::number(batch.rangeStart) + "-" + QByteArray::number(rangeEnd));
    // Force this retry off the shared connection pool -- see the same
    // header in onMoofRetryTimer() above for why.
    req.setRawHeader("Connection", "close");

    qDebug() << "[bbtube][remux][debug] retryVideoBodyBatch batchIndex=" << qint64(batchIndex)
             << "attempt=" << (m_videoBodyRetries[batchIndex] + 1) << "of" << BODY_MAX_RETRIES
             << "range=" << batch.rangeStart << "-" << rangeEnd;

    QNetworkReply *reply = m_networkManager->get(req);
    reply->setProperty("batchIndex", qint64(batchIndex));
    m_videoBodyReplies.append(reply);
    QObject::connect(reply, SIGNAL(finished()), this, SLOT(onVideoBodyBatchFinished()));
}

void StreamingRemuxSession::onVideoBodyBatchFinished()
{
    if (m_failed) return;
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(QObject::sender());
    m_videoBodyReplies.removeOne(reply);
    qint64 batchIndex = reply->property("batchIndex").toLongLong();
    const FragBodyBatch &batch = m_videoBodyBatches[size_t(batchIndex)];

#ifdef QT_DEBUG
    qDebug() << "[bbtube][remux][debug] onVideoBodyBatchFinished batchIndex=" << batchIndex
              << "error=" << int(reply->error()) << "httpStatus="
              << reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
#endif

    if (reply->error()) {
        QString msg = reply->errorString();
        reply->deleteLater();
        int &retryCount = m_videoBodyRetries[size_t(batchIndex)];
        if (retryCount < BODY_MAX_RETRIES) {
            retryCount++;
            qDebug() << "[bbtube][remux] video sample batch" << batchIndex
                     << "download failed (" << msg << ") - retrying, attempt" << retryCount
                     << "of" << BODY_MAX_RETRIES;
            retryVideoBodyBatch(size_t(batchIndex));
            return;
        }
        failWith(QString("video sample batch %1 download failed after %2 retries: %3").arg(
                batchIndex).arg(BODY_MAX_RETRIES).arg(msg));
        return;
    }
    QByteArray body = reply->readAll();
    reply->deleteLater();

    // Verified before any of it is written -- catches a server/proxy
    // returning a wrong-but-plausible body up front rather than after
    // partially writing.
    if (qint64(body.size()) != batch.batchBytes) {
        int &retryCount = m_videoBodyRetries[size_t(batchIndex)];
        if (retryCount < BODY_MAX_RETRIES) {
            retryCount++;
            qDebug() << "[bbtube][remux] video sample batch" << batchIndex
                     << "size mismatch (expected" << batch.batchBytes << "got" << body.size()
                     << ") - relay may not have honored the Range request, retrying, attempt"
                     << retryCount << "of" << BODY_MAX_RETRIES;
            retryVideoBodyBatch(size_t(batchIndex));
            return;
        }
        failWith(QString("video sample batch %1 (%2 samples) size mismatch: expected %3, "
                "got %4 after %5 retries -- server may not have honored the Range request").arg(
                batchIndex).arg(qint64(batch.count)).arg(batch.batchBytes).arg(body.size()).arg(
                BODY_MAX_RETRIES));
        return;
    }

    // batch.outOffset was computed up front in buildVideoBodyBatches(),
    // so this write is correct regardless of which order batches finish
    // in.
    if (!m_outputFile->seek(batch.outOffset) || m_outputFile->write(body) != body.size()) {
        failWith(QString("failed writing video sample batch %1 to output file").arg(batchIndex));
        return;
    }

    m_videoBytesWritten += body.size();
    emit progress(m_videoBytesWritten, qint64(m_plan.videoBodySize));

    m_videoBodyCompletedCount++;
    if (m_videoBodyCompletedCount >= m_videoBodyBatches.size()) {
        m_videoDone = true;
        checkAllDone();
        return;
    }
    dispatchVideoBodyBatches();
}

void StreamingRemuxSession::onAudioFragBodyFinished()
{
    if (m_failed) return;
    QNetworkReply *reply = m_audioFragBodyReply;
    m_audioFragBodyReply = 0;

#ifdef QT_DEBUG
    qDebug() << "[bbtube][remux][debug] onAudioFragBodyFinished fragIndex="
              << qint64(m_audioFragBodyIndex) << "error=" << int(reply->error())
              << "httpStatus=" << reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
#endif

    if (reply->error()) {
        QString msg = reply->errorString();
        reply->deleteLater();
        if (m_audioFragBodyRetryCount < BODY_MAX_RETRIES) {
            m_audioFragBodyRetryCount++;
            qDebug() << "[bbtube][remux] audio body batch at" << qint64(m_audioFragBodyIndex)
                     << "failed (" << msg << ") - retrying, attempt" << m_audioFragBodyRetryCount
                     << "of" << BODY_MAX_RETRIES;
            QTimer::singleShot(RETRY_DELAY_MS, this, SLOT(onAudioFragBodyRetryTimer()));
            return;
        }
        failWith(QString("audio fragment sample download failed after %1 retries: %2").arg(
                BODY_MAX_RETRIES).arg(msg));
        return;
    }
    QByteArray body = reply->readAll();
    reply->deleteLater();

    quint64 expectedBytes = 0;
    for (size_t i = 0; i < m_audioFragBodyBatchCount; i++) {
        expectedBytes += m_audioHead.fragSamples[m_audioFragBodyIndex + i].size;
    }
    if (quint64(body.size()) != expectedBytes) {
        if (m_audioFragBodyRetryCount < BODY_MAX_RETRIES) {
            m_audioFragBodyRetryCount++;
            qDebug() << "[bbtube][remux] audio sample batch at" << qint64(m_audioFragBodyIndex)
                     << "size mismatch (expected" << expectedBytes << "got" << body.size()
                     << ") - relay may not have honored the Range request, retrying, attempt"
                     << m_audioFragBodyRetryCount << "of" << BODY_MAX_RETRIES;
            QTimer::singleShot(RETRY_DELAY_MS, this, SLOT(onAudioFragBodyRetryTimer()));
            return;
        }
        failWith(QString("audio sample batch at %1 (%2 samples) size mismatch: expected %3, "
                "got %4 after %5 retries -- server may not have honored the Range request").arg(
                m_audioFragBodyIndex).arg(m_audioFragBodyBatchCount).arg(expectedBytes).arg(
                body.size()).arg(BODY_MAX_RETRIES));
        return;
    }

    // This batch succeeded -- reset the counter so a later, different
    // batch starts its own retry budget from zero rather than inheriting
    // however many retries this one happened to need.
    m_audioFragBodyRetryCount = 0;

    qint64 outOffset = qint64(m_plan.audioOutputOffset) + m_audioFragBodyOffsetSoFar;

    if (!m_outputFile->seek(outOffset) || m_outputFile->write(body) != body.size()) {
        failWith("failed writing audio sample batch to output file");
        return;
    }

    m_audioFragBodyOffsetSoFar += body.size();
    m_audioFragBodyIndex += m_audioFragBodyBatchCount;

    if (m_audioFragBodyIndex >= m_audioHead.fragSamples.size()) {
        // Same flush-before-signal requirement as the non-fragmented
        // audio path above (onAudioBodyFinished) -- see the comment
        // there. Do it here, right after the last audio batch lands on
        // disk, rather than relying on requestNextFragBody()'s "no more
        // samples" branch below to also remember it.
        m_outputFile->flush();
    }

    requestNextFragBody(false);
}

void StreamingRemuxSession::checkAllDone()
{
    if (m_audioDone && m_videoDone) {
        if (m_outputFile) {
            m_outputFile->close();
        }
        emit finished();
    }
}

void StreamingRemuxSession::failWith(const QString &message)
{
    if (m_failed) return; // only report the first failure
    m_failed = true;

#ifdef QT_DEBUG
    qDebug() << "[bbtube][remux] FAILED:" << message;
#endif

    if (m_videoHeadReply) { m_videoHeadReply->abort(); m_videoHeadReply->deleteLater(); m_videoHeadReply = 0; }
    if (m_audioHeadReply) { m_audioHeadReply->abort(); m_audioHeadReply->deleteLater(); m_audioHeadReply = 0; }
    if (m_videoBodyReply) { m_videoBodyReply->abort(); m_videoBodyReply->deleteLater(); m_videoBodyReply = 0; }
    if (m_audioBodyReply) { m_audioBodyReply->abort(); m_audioBodyReply->deleteLater(); m_audioBodyReply = 0; }
    for (int i = 0; i < m_videoMoofReplies.size(); i++) {
        m_videoMoofReplies[i]->abort();
        m_videoMoofReplies[i]->deleteLater();
    }
    m_videoMoofReplies.clear();
    for (int i = 0; i < m_audioMoofReplies.size(); i++) {
        m_audioMoofReplies[i]->abort();
        m_audioMoofReplies[i]->deleteLater();
    }
    m_audioMoofReplies.clear();
    for (int i = 0; i < m_videoBodyReplies.size(); i++) {
        m_videoBodyReplies[i]->abort();
        m_videoBodyReplies[i]->deleteLater();
    }
    m_videoBodyReplies.clear();
    if (m_audioFragBodyReply) { m_audioFragBodyReply->abort(); m_audioFragBodyReply->deleteLater(); m_audioFragBodyReply = 0; }
    if (m_outputFile) { m_outputFile->close(); }

    emit failed(message);
}
