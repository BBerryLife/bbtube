#include "src/utils/ChunkedRemuxSession.hpp"

#include <QNetworkRequest>
#include <QUrl>
#include <QVariant>
#include <QTimer>
#include <QDir>
#include <QFileInfo>
#include <QStringList>
#include <QDebug>

#include <cstring>
#include <stdexcept>

// Head fetch: ftyp+moov+sidx of each source -- a few KB for YouTube adaptive
// formats, so 32KB nearly always suffices (every KB costs ~12ms on a 80KB/s
// relay); doubled on demand up to 4MB.
static const int INITIAL_HEAD_FETCH_BYTES = 32 * 1024;
static const int MAX_HEAD_FETCH_BYTES = 4 * 1024 * 1024;
// A fragment's moof is a few hundred bytes to a few KB; 8KB is a safe first
// guess, grown x4 (not counted as a retry) if a moof turns out bigger.
static const qint64 MOOF_FETCH_BYTES = 8 * 1024;
static const qint64 MOOF_FETCH_MAX_BYTES = 128 * 1024;
// Total simultaneous HTTP requests (moof + body, both tracks). The relay
// seen in the field (community Invidious instance proxying googlevideo)
// delivers only ~50-90KB/s PER CONNECTION, so throughput scales with the
// number of parallel connections. The old session used 2; wrong-content
// bodies (right length, wrong bytes), the reason it stayed at 2, are now
// caught by verifyAvcSamples() and re-fetched.
static const int MAX_INFLIGHT = 6;
// One contiguous run of samples is split into at most this many parallel
// Range requests, each at least MIN_PART_BYTES long.
static const int MAX_PARTS = 4;
static const qint64 MIN_PART_BYTES = 96 * 1024;
// How many chunks past the last appended one may be in progress at once
// (their buffers live in RAM: ~1MB per 5s of 720p).
static const size_t CHUNK_WINDOW = 3;
static const int MAX_RETRIES = 5;
// Refetches of a whole chunk whose bytes failed the structure check. Each
// attempt re-downloads ~0.5MB, cheap next to losing the whole playback to
// an unlucky streak on a flaky relay.
static const int MAX_VERIFY_RETRIES = 6;
static const int RETRY_DELAY_MS = 400; // see StreamingRemuxSession for why delayed + Connection: close
static const qint64 MERGE_SLICE_BYTES = 1024 * 1024;

static Mp4RemuxBytes toBytes(const QByteArray &a)
{
    const uint8_t *p = reinterpret_cast<const uint8_t *>(a.constData());
    return Mp4RemuxBytes(p, p + a.size());
}

struct RunPart
{
    qint64 start;
    qint64 len;
    qint64 bufOffset;
};

// Groups samples [i0,i1) into contiguous source runs and splits big runs
// into parallel parts. Returns the total payload size.
static qint64 buildRunParts(const std::vector<FragSample> &s, size_t i0, size_t i1,
        std::vector<RunPart> *out)
{
    qint64 bufOff = 0;
    size_t i = i0;
    while (i < i1) {
        qint64 runStart = qint64(s[i].offsetInSource);
        qint64 runLen = 0;
        size_t j = i;
        while (j < i1 && qint64(s[j].offsetInSource) == runStart + runLen) {
            runLen += qint64(s[j].size);
            j++;
        }
        if (runLen > 0) {
            int parts = int(runLen / MIN_PART_BYTES);
            if (parts < 1) parts = 1;
            if (parts > MAX_PARTS) parts = MAX_PARTS;
            qint64 base = runLen / parts;
            qint64 off = 0;
            for (int p = 0; p < parts; p++) {
                RunPart rp;
                rp.start = runStart + off;
                rp.len = (p == parts - 1) ? (runLen - off) : base;
                rp.bufOffset = bufOff + off;
                out->push_back(rp);
                off += rp.len;
            }
            bufOff += runLen;
        }
        i = j;
    }
    return bufOff;
}

ChunkedRemuxSession::ChunkedRemuxSession(QNetworkAccessManager *networkManager,
        const QString &videoUrl, const QString &audioUrl, const QString &cacheDir,
        const QString &baseName, QObject *parent, double chunkSeconds) :
        QObject(parent), m_nm(networkManager), m_videoUrl(videoUrl), m_audioUrl(audioUrl), m_cacheDir(
                cacheDir), m_baseName(baseName), m_chunkSeconds(chunkSeconds), m_started(false), m_cancelled(
                false), m_failed(false), m_headsDone(false), m_allAppended(false), m_videoHeadOk(
                false), m_audioHeadOk(false), m_videoHeadSize(INITIAL_HEAD_FETCH_BYTES), m_audioHeadSize(
                INITIAL_HEAD_FETCH_BYTES), m_videoHeadRetries(0), m_audioHeadRetries(0), m_videoHeadReply(
                0), m_audioHeadReply(0), m_aFragAppended(0), m_aNextIdx(0), m_aNextStartTicks(0), m_nextToAssemble(
                0), m_nextToAppend(0), m_vStage(0), m_aStage(0), m_vStageBytes(0), m_aStageBytes(0), m_vTicks(
                0), m_aTicks(0), m_coveredSeconds(0), m_totalSeconds(0), m_mergeActive(false), m_mergeAgain(
                false), m_mergeSeq(0), m_mergeChunks(0), m_mergedChunksDone(0), m_mergeFinal(false), m_mergeCovered(
                0), m_mergeOut(0), m_mergeAin(0), m_mergeVin(0), m_mergeARemain(0), m_mergeVRemain(0)
{
}

ChunkedRemuxSession::~ChunkedRemuxSession()
{
    if (!m_cancelled) {
        cancel();
    }
}

QString ChunkedRemuxSession::finalPath() const
{
    return QDir(m_cacheDir).absoluteFilePath(m_baseName + ".mp4");
}

QString ChunkedRemuxSession::mergedPathFor(int seq, bool isFinal) const
{
    if (isFinal) return finalPath();
    return QDir(m_cacheDir).absoluteFilePath(m_baseName + "_m" + QString::number(seq) + ".mp4");
}

void ChunkedRemuxSession::start()
{
    if (m_started) return;
    m_started = true;
    m_clock.start();
    QDir().mkpath(m_cacheDir);

    // A complete merged copy from an earlier play of this same video/quality.
    // Only ever created by renaming a fully written file into place.
    QFileInfo cached(finalPath());
    if (cached.exists() && cached.size() > 0) {
        QTimer::singleShot(0, this, SLOT(onCacheHitTimer()));
        return;
    }

    // Leftovers of an earlier interrupted run for the same base name.
    QDir d(m_cacheDir);
    QStringList filters;
    filters << (m_baseName + "_m*") << (m_baseName + "*.stage") << (m_baseName + "*.part");
    QStringList stale = d.entryList(filters, QDir::Files);
    for (int i = 0; i < stale.size(); i++) {
        QFile::remove(d.absoluteFilePath(stale[i]));
    }

    requestHead(true);
    requestHead(false);
}

void ChunkedRemuxSession::onCacheHitTimer()
{
    if (m_cancelled) return;
    qDebug() << "[bbtube][chunk] cache hit, playing" << finalPath();
    m_allAppended = true;
    emit mergedReady(finalPath(), 0.0, true);
}

void ChunkedRemuxSession::requestHead(bool isVideo)
{
    int size = isVideo ? m_videoHeadSize : m_audioHeadSize;
    QNetworkRequest req(QUrl::fromEncoded((isVideo ? m_videoUrl : m_audioUrl).toUtf8()));
    req.setRawHeader("Range", "bytes=0-" + QByteArray::number(size - 1));
    QNetworkReply *reply = m_nm->get(req);
    if (isVideo) m_videoHeadReply = reply; else m_audioHeadReply = reply;
    QObject::connect(reply, SIGNAL(finished()), this, SLOT(onHeadFinished()));
}

void ChunkedRemuxSession::onHeadFinished()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(QObject::sender());
    bool isVideo = (reply == m_videoHeadReply);
    if (isVideo) m_videoHeadReply = 0; else m_audioHeadReply = 0;
    if (m_cancelled || m_failed) {
        reply->deleteLater();
        return;
    }

    int &retries = isVideo ? m_videoHeadRetries : m_audioHeadRetries;
    int &size = isVideo ? m_videoHeadSize : m_audioHeadSize;
    const char *label = isVideo ? "video" : "audio";

    if (reply->error()) {
        QString msg = reply->errorString();
        reply->deleteLater();
        if (retries < MAX_RETRIES) {
            retries++;
            qDebug() << "[bbtube][chunk]" << label << "head fetch failed (" << msg << ") - retry"
                     << retries;
            requestHead(isVideo);
            return;
        }
        failWith(QString("%1 head fetch failed: %2").arg(label).arg(msg));
        return;
    }

    Mp4RemuxBytes buf = toBytes(reply->readAll());
    reply->deleteLater();

    TrackHead head;
    bool needMore = false;
    try {
        head = parseHead(buf, label);
        if (!head.isFragmented) {
            failWith(QString("%1 source is not a fragmented (DASH) mp4").arg(label));
            return;
        }
        if (head.isVideo != isVideo) {
            failWith(QString("%1 url returned a %2 track").arg(label).arg(
                    head.isVideo ? "video" : "audio"));
            return;
        }
        if (!head.sidx.found) needMore = true;
    } catch (const std::exception &e) {
        needMore = true;
    }
    if (needMore) {
        if (size >= MAX_HEAD_FETCH_BYTES) {
            failWith(QString("%1 head parse failed (moov/sidx not found in %2 bytes)").arg(label).arg(
                    size));
            return;
        }
        size *= 2;
        requestHead(isVideo);
        return;
    }

    if (isVideo) {
        m_videoHead = head;
        m_videoHeadOk = true;
    } else {
        m_audioHead = head;
        m_audioHeadOk = true;
    }
    if (m_videoHeadOk && m_audioHeadOk) {
        onHeadsReady();
    }
}

void ChunkedRemuxSession::onHeadsReady()
{
    m_headsDone = true;

    std::vector<ChunkPlanEntry> plan = planChunksFromSidx(m_videoHead.sidx, m_chunkSeconds);
    m_aFragStart = sidxFragmentStartSeconds(m_audioHead.sidx);
    if (plan.empty() || m_aFragStart.size() < 2) {
        failWith("sidx has no usable fragments");
        return;
    }
    size_t nV = m_videoHead.sidx.entries.size();
    size_t nA = m_audioHead.sidx.entries.size();
    m_totalSeconds = plan.back().endSec;

    uint64_t cum = m_videoHead.sidx.firstFragmentOffset;
    for (size_t i = 0; i < nV; i++) {
        m_vFragOffset.push_back(cum);
        cum += m_videoHead.sidx.entries[i].referencedSize;
    }
    cum = m_audioHead.sidx.firstFragmentOffset;
    for (size_t i = 0; i < nA; i++) {
        m_aFragOffset.push_back(cum);
        cum += m_audioHead.sidx.entries[i].referencedSize;
    }
    m_vFragState.assign(nV, 0);
    m_aFragState.assign(nA, 0);
    m_vFragSamples.resize(nV);
    m_aFragSamples.resize(nA);

    for (size_t k = 0; k < plan.size(); k++) {
        Chunk c;
        c.fragStart = plan[k].fragStart;
        c.fragEnd = plan[k].fragEnd;
        c.startSec = plan[k].startSec;
        c.endSec = plan[k].endSec;
        if (k + 1 == plan.size()) {
            c.aFragEnd = nA;
        } else {
            size_t j = 1;
            while (j < nA && m_aFragStart[j] < c.endSec - 1e-6) j++;
            c.aFragEnd = j;
        }
        m_chunks.push_back(c);
    }

    m_vStage = new QFile(QDir(m_cacheDir).absoluteFilePath(m_baseName + ".v.stage"));
    m_aStage = new QFile(QDir(m_cacheDir).absoluteFilePath(m_baseName + ".a.stage"));
    if (!m_vStage->open(QIODevice::WriteOnly | QIODevice::Truncate)
            || !m_aStage->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        failWith("cannot create staging files in the cache directory");
        return;
    }

    qDebug() << "[bbtube][chunk] heads ready after" << m_clock.elapsed() << "ms:" << qint64(m_chunks.size())
             << "chunks of ~" << m_chunkSeconds << "s, total" << m_totalSeconds << "s";
    pump();
}

// ---------------------------------------------------------------------------
// Discovery + body scheduling
// ---------------------------------------------------------------------------
void ChunkedRemuxSession::queueJob(Job *job)
{
    m_queue.append(job);
}

void ChunkedRemuxSession::scheduleDiscovery(size_t k)
{
    Chunk &c = m_chunks[k];
    if (c.discoveryQueued) return;
    c.discoveryQueued = true;

    for (size_t f = c.fragStart; f < c.fragEnd; f++) {
        if (m_vFragState[f] != 0) continue;
        m_vFragState[f] = 1;
        Job *j = new Job();
        j->type = JobMoofVideo;
        j->chunk = int(k);
        j->frag = f;
        j->start = qint64(m_vFragOffset[f]);
        j->len = MOOF_FETCH_BYTES;
        queueJob(j);
    }
    for (size_t f = 0; f < c.aFragEnd; f++) {
        if (m_aFragState[f] != 0) continue;
        m_aFragState[f] = 1;
        Job *j = new Job();
        j->type = JobMoofAudio;
        j->chunk = int(k);
        j->frag = f;
        j->start = qint64(m_aFragOffset[f]);
        j->len = MOOF_FETCH_BYTES;
        queueJob(j);
    }
}

bool ChunkedRemuxSession::discoveryDone(size_t k) const
{
    const Chunk &c = m_chunks[k];
    for (size_t f = c.fragStart; f < c.fragEnd; f++)
        if (m_vFragState[f] != 2) return false;
    for (size_t f = 0; f < c.aFragEnd; f++)
        if (m_aFragState[f] != 2) return false;
    return true;
}

void ChunkedRemuxSession::tryAssemble()
{
    // Strictly in chunk order: sample indices and audio cut points depend on
    // everything before them.
    while (m_nextToAssemble < m_chunks.size() && m_chunks[m_nextToAssemble].discoveryQueued
            && discoveryDone(m_nextToAssemble)) {
        assemble(m_nextToAssemble);
        m_nextToAssemble++;
    }
}

void ChunkedRemuxSession::appendAudioFragsInOrder()
{
    // Flatten every audio fragment that is already known, in order.
    while (m_aFragAppended < m_aFragState.size() && m_aFragState[m_aFragAppended] == 2) {
        std::vector<FragSample> &fs = m_aFragSamples[m_aFragAppended];
        m_aAll.insert(m_aAll.end(), fs.begin(), fs.end());
        std::vector<FragSample>().swap(fs);
        m_aFragAppended++;
    }
}

void ChunkedRemuxSession::assemble(size_t k)
{
    Chunk &c = m_chunks[k];

    c.vi0 = m_vAll.size();
    for (size_t f = c.fragStart; f < c.fragEnd; f++) {
        std::vector<FragSample> &fs = m_vFragSamples[f];
        m_vAll.insert(m_vAll.end(), fs.begin(), fs.end());
        std::vector<FragSample>().swap(fs);
    }
    c.vi1 = m_vAll.size();

    appendAudioFragsInOrder();
    bool last = (k + 1 == m_chunks.size());
    uint64_t endTicks = uint64_t(c.endSec * double(m_audioHead.timescale) + 0.5);
    c.ai0 = m_aNextIdx;
    size_t i = c.ai0;
    while (i < m_aAll.size() && (last || m_aNextStartTicks < endTicks)) {
        m_aNextStartTicks += m_aAll[i].duration;
        i++;
    }
    c.ai1 = i;
    m_aNextIdx = i;
    c.assembled = true;

    queueBodyJobs(k);
}

void ChunkedRemuxSession::queueVideoBodyJobs(size_t k)
{
    Chunk &c = m_chunks[k];
    std::vector<RunPart> parts;
    qint64 bytes = buildRunParts(m_vAll, c.vi0, c.vi1, &parts);
    c.vBuf = QByteArray(int(bytes), 0);
    c.vPending = int(parts.size());
    for (size_t i = 0; i < parts.size(); i++) {
        Job *j = new Job();
        j->type = JobBodyVideo;
        j->chunk = int(k);
        j->start = parts[i].start;
        j->len = parts[i].len;
        j->bufOffset = parts[i].bufOffset;
        queueJob(j);
    }
}

void ChunkedRemuxSession::queueAudioBodyJobs(size_t k)
{
    Chunk &c = m_chunks[k];
    std::vector<RunPart> parts;
    qint64 bytes = buildRunParts(m_aAll, c.ai0, c.ai1, &parts);
    c.aBuf = QByteArray(int(bytes), 0);
    c.aPending = int(parts.size());
    for (size_t i = 0; i < parts.size(); i++) {
        Job *j = new Job();
        j->type = JobBodyAudio;
        j->chunk = int(k);
        j->start = parts[i].start;
        j->len = parts[i].len;
        j->bufOffset = parts[i].bufOffset;
        queueJob(j);
    }
}

void ChunkedRemuxSession::queueBodyJobs(size_t k)
{
    queueVideoBodyJobs(k);
    queueAudioBodyJobs(k);
    Chunk &c = m_chunks[k];
    if (c.vPending == 0 && c.aPending == 0) {
        onChunkBuffersComplete(k); // degenerate: nothing to download
    }
}

void ChunkedRemuxSession::pump()
{
    if (m_cancelled || m_failed || !m_headsDone) return;

    size_t end = m_nextToAppend + CHUNK_WINDOW;
    if (end > m_chunks.size()) end = m_chunks.size();
    for (size_t k = m_nextToAppend; k < end; k++) {
        scheduleDiscovery(k);
    }
    tryAssemble();

    while (m_inflight.size() < MAX_INFLIGHT && !m_queue.isEmpty()) {
        // Lowest chunk first, so the chunk the player needs next always wins
        // bandwidth over prefetching later ones.
        int best = 0;
        for (int i = 1; i < m_queue.size(); i++) {
            if (m_queue[i]->chunk < m_queue[best]->chunk) best = i;
        }
        Job *job = m_queue.takeAt(best);
        dispatch(job);
    }
}

void ChunkedRemuxSession::dispatch(Job *job)
{
    bool isVideo = (job->type == JobMoofVideo || job->type == JobBodyVideo);
    QNetworkRequest req(QUrl::fromEncoded((isVideo ? m_videoUrl : m_audioUrl).toUtf8()));
    req.setRawHeader("Range",
            "bytes=" + QByteArray::number(job->start) + "-"
                    + QByteArray::number(job->start + job->len - 1));
    if (job->retries > 0) {
        req.setRawHeader("Connection", "close");
    }
    QNetworkReply *reply = m_nm->get(req);
    job->reply = reply;
    m_inflight.insert(reply, job);
    QObject::connect(reply, SIGNAL(finished()), this, SLOT(onJobFinished()));
}

void ChunkedRemuxSession::onJobFinished()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(QObject::sender());
    Job *job = m_inflight.take(reply);
    if (!job) {
        reply->deleteLater();
        return;
    }
    job->reply = 0;
    if (m_cancelled || m_failed) {
        reply->deleteLater();
        delete job;
        return;
    }
    if (reply->error()) {
        QString msg = reply->errorString();
        reply->deleteLater();
        retryJob(job, msg);
        pump();
        return;
    }
    QByteArray data = reply->readAll();
    reply->deleteLater();

    if (job->type == JobMoofVideo || job->type == JobMoofAudio) {
        handleMoof(job, data);
    } else {
        handleBody(job, data);
    }
    pump();
}

void ChunkedRemuxSession::retryJob(Job *job, const QString &why)
{
    if (job->retries < MAX_RETRIES) {
        job->retries++;
        qDebug() << "[bbtube][chunk] request type" << job->type << "chunk" << job->chunk
                 << "failed (" << why << ") - retry" << job->retries << "of" << MAX_RETRIES;
        m_retryQueue.append(job);
        QTimer::singleShot(RETRY_DELAY_MS, this, SLOT(onRetryTimer()));
        return;
    }
    int type = job->type;
    int chunk = job->chunk;
    delete job;
    failWith(QString("request (type %1, chunk %2) failed after %3 retries: %4").arg(type).arg(
            chunk).arg(MAX_RETRIES).arg(why));
}

void ChunkedRemuxSession::onRetryTimer()
{
    if (m_cancelled || m_failed || m_retryQueue.isEmpty()) return;
    Job *j = m_retryQueue.takeFirst();
    m_queue.append(j);
    pump();
}

void ChunkedRemuxSession::handleMoof(Job *job, const QByteArray &data)
{
    bool isVideo = (job->type == JobMoofVideo);
    size_t f = job->frag;
    try {
        std::vector<FragSample> s = parseMoofSamples(toBytes(data), size_t(job->start),
                isVideo ? "video" : "audio");
        if (s.empty()) throw std::runtime_error("moof describes no samples");
        if (isVideo) {
            m_vFragSamples[f] = s;
            m_vFragState[f] = 2;
        } else {
            m_aFragSamples[f] = s;
            m_aFragState[f] = 2;
        }
        delete job;
    } catch (const std::exception &e) {
        if (job->len < MOOF_FETCH_MAX_BYTES) {
            // Most likely the moof is larger than what was fetched.
            job->len *= 4;
            queueJob(job);
        } else {
            retryJob(job, QString("moof parse failed: %1").arg(e.what()));
        }
    }
}

void ChunkedRemuxSession::handleBody(Job *job, const QByteArray &data)
{
    if (qint64(data.size()) != job->len) {
        retryJob(job, QString("size mismatch: expected %1 got %2").arg(job->len).arg(data.size()));
        return;
    }
    size_t k = size_t(job->chunk);
    Chunk &c = m_chunks[k];
    bool isVideo = (job->type == JobBodyVideo);
    QByteArray &buf = isVideo ? c.vBuf : c.aBuf;
    std::memcpy(buf.data() + job->bufOffset, data.constData(), size_t(data.size()));
    int &pending = isVideo ? c.vPending : c.aPending;
    pending--;
    delete job;
    if (c.vPending == 0 && c.aPending == 0) {
        onChunkBuffersComplete(k);
    }
}

void ChunkedRemuxSession::onChunkBuffersComplete(size_t k)
{
    Chunk &c = m_chunks[k];
    bool videoOk = verifyAvcSamples(m_videoHead, m_vAll, c.vi0, c.vi1 - c.vi0,
            reinterpret_cast<const uint8_t *>(c.vBuf.constData()), size_t(c.vBuf.size()));
    if (!videoOk) {
        uint h = qHash(c.vBuf);
        if (c.hasBadVideo && c.lastBadVideoHash == h) {
            qDebug() << "[bbtube][chunk] chunk" << qint64(k)
                     << "video failed the structure check twice with identical bytes - accepting as genuine";
            videoOk = true;
        }
        c.hasBadVideo = true;
        c.lastBadVideoHash = h;
    }
    if (!videoOk) {
        if (c.verifyRetries < MAX_VERIFY_RETRIES) {
            c.verifyRetries++;
            qDebug() << "[bbtube][chunk] chunk" << qint64(k)
                     << "video bytes failed the NAL-structure check (wrong data from relay?) - refetching,"
                     << "attempt" << c.verifyRetries;
            queueVideoBodyJobs(k);
            return;
        }
        failWith(QString("chunk %1: video data is corrupt after %2 refetches").arg(qint64(k)).arg(
                MAX_VERIFY_RETRIES));
        return;
    }
    bool audioOk = verifyAacSamples(m_audioHead, m_aAll, c.ai0, c.ai1 - c.ai0,
            reinterpret_cast<const uint8_t *>(c.aBuf.constData()), size_t(c.aBuf.size()));
    if (!audioOk) {
        uint h = qHash(c.aBuf);
        if (c.hasBadAudio && c.lastBadAudioHash == h) {
            qDebug() << "[bbtube][chunk] chunk" << qint64(k)
                     << "audio failed the structure check twice with identical bytes - accepting as genuine";
            audioOk = true;
        }
        c.hasBadAudio = true;
        c.lastBadAudioHash = h;
    }
    if (!audioOk) {
        if (c.verifyRetries < MAX_VERIFY_RETRIES) {
            c.verifyRetries++;
            qDebug() << "[bbtube][chunk] chunk" << qint64(k)
                     << "audio bytes failed the AAC structure check - refetching, attempt"
                     << c.verifyRetries;
            queueAudioBodyJobs(k);
            return;
        }
        failWith(QString("chunk %1: audio data is corrupt after %2 refetches").arg(qint64(k)).arg(
                MAX_VERIFY_RETRIES));
        return;
    }
    c.ready = true;
    appendReadyChunks();
}

void ChunkedRemuxSession::appendReadyChunks()
{
    bool any = false;
    while (m_nextToAppend < m_chunks.size() && m_chunks[m_nextToAppend].ready) {
        Chunk &c = m_chunks[m_nextToAppend];
        if (m_vStage->write(c.vBuf) != c.vBuf.size() || m_aStage->write(c.aBuf) != c.aBuf.size()) {
            failWith("failed writing to the staging files (disk full?)");
            return;
        }
        m_vStage->flush();
        m_aStage->flush();
        m_vStageBytes += c.vBuf.size();
        m_aStageBytes += c.aBuf.size();
        for (size_t i = c.vi0; i < c.vi1; i++) {
            m_videoHead.fragSamples.push_back(m_vAll[i]);
            m_vTicks += m_vAll[i].duration;
        }
        for (size_t i = c.ai0; i < c.ai1; i++) {
            m_audioHead.fragSamples.push_back(m_aAll[i]);
            m_aTicks += m_aAll[i].duration;
        }
        c.vBuf = QByteArray();
        c.aBuf = QByteArray();
        c.appended = true;
        m_nextToAppend++;
        any = true;

        double vs = double(m_vTicks) / double(m_videoHead.timescale);
        double as = double(m_aTicks) / double(m_audioHead.timescale);
        m_coveredSeconds = vs < as ? vs : as;
        qDebug() << "[bbtube][chunk] chunk" << qint64(m_nextToAppend - 1) << "appended at"
                 << m_clock.elapsed() << "ms, covered" << m_coveredSeconds << "s of" << m_totalSeconds;
    }
    if (m_nextToAppend == m_chunks.size()) {
        m_allAppended = true;
    }
    if (any) {
        emit progress(m_coveredSeconds, m_totalSeconds);
    }
}

// ---------------------------------------------------------------------------
// Merging
// ---------------------------------------------------------------------------
void ChunkedRemuxSession::requestMerge()
{
    if (m_cancelled || !m_vStage || !m_aStage) return;
    if (m_nextToAppend == 0) return;
    if (m_mergeActive) {
        m_mergeAgain = true;
        return;
    }
    if (m_mergedChunksDone >= m_nextToAppend) return; // nothing newer than the last merged file

    size_t vn = m_videoHead.fragSamples.size();
    size_t an = m_audioHead.fragSamples.size();
    std::string err;
    if (!planPartialRemux(m_videoHead, m_audioHead, vn, an, &m_mergePlan, &err)) {
        failWith(QString("cannot plan merged file: %1").arg(QString(err.c_str())));
        return;
    }
    if (qint64(m_mergePlan.videoBodySize) != m_vStageBytes
            || qint64(m_mergePlan.audioBodySize) != m_aStageBytes) {
        failWith("internal error: staged byte counts do not match the sample tables");
        return;
    }

    m_mergeChunks = m_nextToAppend;
    m_mergeFinal = (m_mergeChunks == m_chunks.size());
    m_mergeCovered = m_coveredSeconds;
    m_mergeSeq++;
    m_mergePath = mergedPathFor(m_mergeSeq, m_mergeFinal);
    m_mergeARemain = m_aStageBytes;
    m_mergeVRemain = m_vStageBytes;

    m_mergeOut = new QFile(m_mergePath + ".part");
    m_mergeAin = new QFile(m_aStage->fileName());
    m_mergeVin = new QFile(m_vStage->fileName());
    if (!m_mergeOut->open(QIODevice::WriteOnly | QIODevice::Truncate)
            || !m_mergeAin->open(QIODevice::ReadOnly) || !m_mergeVin->open(QIODevice::ReadOnly)) {
        failWith("cannot open files for merging");
        return;
    }
    QByteArray head(reinterpret_cast<const char *>(&m_mergePlan.headBytes[0]),
            int(m_mergePlan.headBytes.size()));
    if (m_mergeOut->write(head) != head.size()) {
        failWith("failed writing merged file header");
        return;
    }
    m_mergeActive = true;
    QTimer::singleShot(0, this, SLOT(onMergeStep()));
}

void ChunkedRemuxSession::onMergeStep()
{
    if (!m_mergeActive || m_cancelled) return;

    QFile *in = 0;
    qint64 *remain = 0;
    if (m_mergeARemain > 0) {
        in = m_mergeAin;
        remain = &m_mergeARemain;
    } else if (m_mergeVRemain > 0) {
        in = m_mergeVin;
        remain = &m_mergeVRemain;
    } else {
        finishMerge();
        return;
    }
    qint64 n = *remain < MERGE_SLICE_BYTES ? *remain : MERGE_SLICE_BYTES;
    QByteArray b = in->read(n);
    if (qint64(b.size()) != n || m_mergeOut->write(b) != n) {
        failWith("merge copy failed");
        return;
    }
    *remain -= n;
    QTimer::singleShot(0, this, SLOT(onMergeStep()));
}

void ChunkedRemuxSession::finishMerge()
{
    m_mergeOut->close();
    delete m_mergeOut; m_mergeOut = 0;
    delete m_mergeAin; m_mergeAin = 0;
    delete m_mergeVin; m_mergeVin = 0;

    QString part = m_mergePath + ".part";
    QFile::remove(m_mergePath);
    if (!QFile::rename(part, m_mergePath)) {
        m_mergeActive = false;
        failWith("cannot move merged file into place");
        return;
    }
    m_mergeActive = false;
    m_mergedChunksDone = m_mergeChunks;
    qDebug() << "[bbtube][chunk] merged file" << m_mergePath << "ready at" << m_clock.elapsed()
             << "ms, covers" << m_mergeCovered << "s" << (m_mergeFinal ? "(FINAL)" : "");

    // Merged files from two merges ago are no longer needed: the player has
    // long since swapped away from them. The final copy is the cache entry.
    if (!m_mergePrevPrevPath.isEmpty()) {
        QFile::remove(m_mergePrevPrevPath);
    }
    m_mergePrevPrevPath = m_mergePrevPath;
    m_mergePrevPath = m_mergeFinal ? QString() : m_mergePath;

    QString path = m_mergePath;
    double covered = m_mergeCovered;
    bool isFinal = m_mergeFinal;
    if (isFinal) {
        // Staging payloads are redundant now.
        if (m_vStage) { QString n = m_vStage->fileName(); m_vStage->close(); delete m_vStage; m_vStage = 0; QFile::remove(n); }
        if (m_aStage) { QString n = m_aStage->fileName(); m_aStage->close(); delete m_aStage; m_aStage = 0; QFile::remove(n); }
    }
    emit mergedReady(path, covered, isFinal);

    if (m_mergeAgain && !isFinal) {
        m_mergeAgain = false;
        requestMerge();
    }
}

// ---------------------------------------------------------------------------
// Teardown / failure
// ---------------------------------------------------------------------------
void ChunkedRemuxSession::teardown(bool removeStageFiles)
{
    if (m_videoHeadReply) {
        QObject::disconnect(m_videoHeadReply, 0, this, 0);
        m_videoHeadReply->abort();
        m_videoHeadReply->deleteLater();
        m_videoHeadReply = 0;
    }
    if (m_audioHeadReply) {
        QObject::disconnect(m_audioHeadReply, 0, this, 0);
        m_audioHeadReply->abort();
        m_audioHeadReply->deleteLater();
        m_audioHeadReply = 0;
    }
    QList<QNetworkReply *> replies = m_inflight.keys();
    for (int i = 0; i < replies.size(); i++) {
        QNetworkReply *r = replies[i];
        Job *j = m_inflight.take(r);
        QObject::disconnect(r, 0, this, 0);
        r->abort();
        r->deleteLater();
        delete j;
    }
    qDeleteAll(m_queue);
    m_queue.clear();
    qDeleteAll(m_retryQueue);
    m_retryQueue.clear();

    if (m_mergeActive || m_mergeOut) {
        m_mergeActive = false;
        if (m_mergeOut) {
            m_mergeOut->close();
            QFile::remove(m_mergeOut->fileName());
        }
        delete m_mergeOut; m_mergeOut = 0;
        delete m_mergeAin; m_mergeAin = 0;
        delete m_mergeVin; m_mergeVin = 0;
    }

    if (removeStageFiles) {
        if (m_vStage) { QString n = m_vStage->fileName(); m_vStage->close(); delete m_vStage; m_vStage = 0; QFile::remove(n); }
        if (m_aStage) { QString n = m_aStage->fileName(); m_aStage->close(); delete m_aStage; m_aStage = 0; QFile::remove(n); }
    }
}

void ChunkedRemuxSession::cancel()
{
    if (m_cancelled) return;
    m_cancelled = true;
    teardown(true);
}

void ChunkedRemuxSession::failWith(const QString &message)
{
    if (m_failed || m_cancelled) return;
    m_failed = true;
    qDebug() << "[bbtube][chunk] FAILED after" << m_clock.elapsed() << "ms:" << message;
    // Network work stops, but the staging files stay: whatever was already
    // downloaded can still be merged and played.
    teardown(false);
    emit failed(message);
}
