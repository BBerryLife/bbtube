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
        std::vector<RunPart> *out, int maxParts)
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
            if (parts > maxParts) parts = maxParts;
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

static const int STALL_MS = 6000; // a request that has received nothing for this long is aborted and retried
static const int HEDGE_MS = 4000; // the chunk the player needs next: duplicate requests older than this
static const int WATCHDOG_MS = 1000;
static const int HEAD_TIMEOUT_MS = 8000;
static const int MAX_HEDGE_EXTRA = 3; // hedged duplicates may exceed MAX_INFLIGHT by this many

ChunkedRemuxSession::ChunkedRemuxSession(QNetworkAccessManager *networkManager,
        const QString &videoUrl, const QString &audioUrl, const QString &cacheDir,
        const QString &baseName, QObject *parent, double chunkSeconds, double startAtSeconds) :
        QObject(parent), m_nm(networkManager), m_videoUrl(videoUrl), m_audioUrl(audioUrl), m_cacheDir(
                cacheDir), m_baseName(baseName), m_chunkSeconds(chunkSeconds), m_startAtSeconds(
                startAtSeconds), m_started(false), m_cancelled(false), m_failed(false), m_headsDone(
                false), m_videoHeadOk(false), m_audioHeadOk(false), m_videoHeadSize(
                INITIAL_HEAD_FETCH_BYTES), m_audioHeadSize(INITIAL_HEAD_FETCH_BYTES), m_videoHeadRetries(
                0), m_audioHeadRetries(0), m_videoHeadReply(0), m_audioHeadReply(0), m_head(0), m_pendingSeekSec(
                -1.0), m_storedCount(0), m_totalSeconds(0), m_watchdog(0), m_vStage(0), m_aStage(0), m_vStageBytes(0), m_aStageBytes(
                0), m_mergeActive(false), m_mergeNextStart(-1), m_mergeSeq(0), m_mergeStart(0), m_mergeCount(
                0), m_mergeFinal(false), m_mergeOut(0), m_mergeAin(0), m_mergeVin(0), m_mergeASegIdx(
                0), m_mergeVSegIdx(0), m_mergeSegLeft(0), m_mergeSegOpen(false)
{
    m_watchdog = new QTimer(this);
    QObject::connect(m_watchdog, SIGNAL(timeout()), this, SLOT(onWatchdog()));
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
    emit mergedReady(finalPath(), 0, -1, true);
}

void ChunkedRemuxSession::requestHead(bool isVideo)
{
    int size = isVideo ? m_videoHeadSize : m_audioHeadSize;
    QNetworkRequest req(QUrl::fromEncoded((isVideo ? m_videoUrl : m_audioUrl).toUtf8()));
    req.setRawHeader("Range", "bytes=0-" + QByteArray::number(size - 1));
    QNetworkReply *reply = m_nm->get(req);
    if (isVideo) {
        m_videoHeadReply = reply;
        m_videoHeadClock.start();
    } else {
        m_audioHeadReply = reply;
        m_audioHeadClock.start();
    }
    if (!m_watchdog->isActive()) m_watchdog->start(WATCHDOG_MS);
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
    std::vector<ChunkPlanEntry> plan = planChunksFromSidx(m_videoHead.sidx, m_chunkSeconds);
    m_aFragStartSec = sidxFragmentStartSeconds(m_audioHead.sidx);
    if (plan.empty() || m_aFragStartSec.size() < 2) {
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
    // Exact start tick (in the audio track's timescale) of every audio
    // fragment. Audio samples are assigned to chunks by comparing their start
    // tick with integer chunk boundaries, so every sample lands in exactly
    // one chunk and chunks can be downloaded in any order.
    {
        uint64_t sidxTs = m_audioHead.sidx.timescale ? m_audioHead.sidx.timescale : m_audioHead.timescale;
        uint64_t t = 0;
        for (size_t j = 0; j < nA; j++) {
            m_aFragStartTicks.push_back(t * uint64_t(m_audioHead.timescale) / sidxTs);
            t += m_audioHead.sidx.entries[j].subsegmentDuration;
        }
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
        size_t j = 0;
        while (j + 1 < nA && m_aFragStartSec[j + 1] <= c.startSec + 1e-6) j++;
        c.aFragStart = j;
        size_t e = j + 1;
        if (k + 1 == plan.size()) {
            e = nA;
        } else {
            while (e < nA && m_aFragStartSec[e] < c.endSec - 1e-6) e++;
        }
        c.aFragEnd = e;
        m_chunks.push_back(c);
    }

    m_vStage = new QFile(QDir(m_cacheDir).absoluteFilePath(m_baseName + ".v.stage"));
    m_aStage = new QFile(QDir(m_cacheDir).absoluteFilePath(m_baseName + ".a.stage"));
    if (!m_vStage->open(QIODevice::WriteOnly | QIODevice::Truncate)
            || !m_aStage->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        failWith("cannot create staging files in the cache directory");
        return;
    }

    m_headsDone = true;
    double startAt = m_pendingSeekSec >= 0 ? m_pendingSeekSec : m_startAtSeconds;
    m_pendingSeekSec = -1;
    m_head = chunkIndexAt(startAt);
    qDebug() << "[bbtube][chunk] heads ready after" << m_clock.elapsed() << "ms:" << qint64(m_chunks.size())
             << "chunks of ~" << m_chunkSeconds << "s, total" << m_totalSeconds << "s, starting at chunk"
             << m_head << "(" << startAt << "s)";
    pump();
}

// ---------------------------------------------------------------------------
// Chunk queries
// ---------------------------------------------------------------------------
int ChunkedRemuxSession::chunkIndexAt(double absSeconds) const
{
    if (m_chunks.empty()) return -1;
    int best = 0;
    for (size_t i = 0; i < m_chunks.size(); i++) {
        if (m_chunks[i].startSec <= absSeconds + 1e-6) best = int(i); else break;
    }
    return best;
}

double ChunkedRemuxSession::chunkStartSeconds(int k) const
{
    if (k < 0 || k >= int(m_chunks.size())) return 0.0;
    return m_chunks[k].startSec;
}

double ChunkedRemuxSession::chunkEndSeconds(int k) const
{
    if (k < 0 || k >= int(m_chunks.size())) return 0.0;
    return m_chunks[k].endSec;
}

bool ChunkedRemuxSession::isChunkStored(int k) const
{
    return k >= 0 && k < int(m_chunks.size()) && m_chunks[k].state == 2;
}

int ChunkedRemuxSession::runStart(int k) const
{
    if (!isChunkStored(k)) return k;
    while (k > 0 && isChunkStored(k - 1)) k--;
    return k;
}

int ChunkedRemuxSession::runEnd(int k) const
{
    if (!isChunkStored(k)) return k;
    while (k < int(m_chunks.size()) && isChunkStored(k)) k++;
    return k;
}

int ChunkedRemuxSession::fileStartFor(int k, double maxBackSeconds) const
{
    int a = runStart(k);
    int lo = chunkIndexAt(chunkStartSeconds(k) - maxBackSeconds);
    if (lo > a) a = lo;
    return a;
}

// A seek can only be exact at a chunk boundary: each chunk begins with a
// keyframe, and a seek into the middle of a GOP resumes at the previous
// keyframe -- a rewind of up to a GOP (6-10s on YouTube). So when playback has
// to continue in a longer file (the old one ran out), the new file is entered
// exactly at the keyframe that starts the first chunk the old file did not
// have. That keyframe's presentation time is its decode time plus a few frame
// durations whenever the stream has B-frames (composition offset), so the
// returned position is nudged past it: a seek can then never fall short of
// that keyframe and land on the previous one.
double ChunkedRemuxSession::fileResumeSeconds(int fileStartChunk, int boundaryChunk) const
{
    if (boundaryChunk <= fileStartChunk || boundaryChunk >= int(m_chunks.size())) return 0.0;
    uint64_t ticks = 0;
    for (int k = fileStartChunk; k < boundaryChunk; k++) ticks += m_chunks[k].vTicks;
    double ts = double(m_videoHead.timescale);
    int32_t cto = m_chunks[boundaryChunk].firstCto;
    if (cto < 0) cto = 0;
    return double(ticks) / ts + double(cto) / ts + 0.04;
}

int ChunkedRemuxSession::dist(int k) const
{
    int n = int(m_chunks.size());
    if (n == 0) return 0;
    return ((k - m_head) % n + n) % n;
}

int ChunkedRemuxSession::firstMissingFrom(int k) const
{
    int n = int(m_chunks.size());
    for (int i = 0; i < n; i++) {
        int kk = (k + i) % n;
        if (m_chunks[kk].state != 2) return kk;
    }
    return -1;
}

int ChunkedRemuxSession::seekTo(double absSeconds)
{
    if (!m_headsDone) {
        m_pendingSeekSec = absSeconds;
        return -1;
    }
    int k = chunkIndexAt(absSeconds);
    int h = firstMissingFrom(k);
    if (h >= 0 && h != m_head) {
        bool alreadyWorking = (m_chunks[h].state == 1);
        qDebug() << "[bbtube][chunk] seek to" << absSeconds << "s -> chunk" << k << ", download head"
                 << m_head << "->" << h << (alreadyWorking ? "(already in progress)" : "(restarting there)");
        m_head = h;
        if (!alreadyWorking) {
            abortUnstoredWork();
        }
        pump();
    }
    return k;
}

// Drops everything that is queued or in flight (never stored chunks).
void ChunkedRemuxSession::abortUnstoredWork()
{
    QList<QNetworkReply *> replies = m_inflight.keys();
    for (int i = 0; i < replies.size(); i++) {
        QNetworkReply *r = replies[i];
        Job *j = m_inflight.take(r);
        QObject::disconnect(r, 0, this, 0);
        r->abort();
        r->deleteLater();
        if (j) {
            j->twin = 0;
            delete j;
        }
    }
    qDeleteAll(m_queue);
    m_queue.clear();
    qDeleteAll(m_retryQueue);
    m_retryQueue.clear();

    for (size_t f = 0; f < m_vFragState.size(); f++) if (m_vFragState[f] == 1) m_vFragState[f] = 0;
    for (size_t f = 0; f < m_aFragState.size(); f++) if (m_aFragState[f] == 1) m_aFragState[f] = 0;
    for (size_t k = 0; k < m_chunks.size(); k++) {
        Chunk &c = m_chunks[k];
        if (c.state != 1) continue;
        c.state = 0;
        c.assembled = false;
        c.ready = false;
        c.vBuf = QByteArray();
        c.aBuf = QByteArray();
        c.vPending = c.aPending = 0;
        c.verifyRetries = 0;
        c.hasBadVideo = c.hasBadAudio = false;
        std::vector<FragSample>().swap(c.vSamples);
        std::vector<FragSample>().swap(c.aSamples);
    }
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
    c.state = 1;

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
    for (size_t f = c.aFragStart; f < c.aFragEnd; f++) {
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
    for (size_t f = c.aFragStart; f < c.aFragEnd; f++)
        if (m_aFragState[f] != 2) return false;
    return true;
}

void ChunkedRemuxSession::tryAssemble()
{
    for (size_t k = 0; k < m_chunks.size(); k++) {
        Chunk &c = m_chunks[k];
        if (c.state == 1 && !c.assembled && discoveryDone(k)) {
            assemble(k);
        }
    }
}

void ChunkedRemuxSession::assemble(size_t k)
{
    Chunk &c = m_chunks[k];

    for (size_t f = c.fragStart; f < c.fragEnd; f++) {
        const std::vector<FragSample> &fs = m_vFragSamples[f];
        c.vSamples.insert(c.vSamples.end(), fs.begin(), fs.end());
    }
    c.vTicks = 0;
    for (size_t i = 0; i < c.vSamples.size(); i++) c.vTicks += c.vSamples[i].duration;
    c.firstCto = c.vSamples.empty() ? 0 : c.vSamples[0].compositionOffset;

    // Audio: every sample whose START tick lies in [S, E). S and E are the
    // same integers for adjacent chunks (E of chunk k == S of chunk k+1), so
    // each sample belongs to exactly one chunk no matter the download order.
    double ats = double(m_audioHead.timescale);
    uint64_t S = uint64_t(c.startSec * ats + 0.5);
    uint64_t E = (k + 1 == m_chunks.size()) ? ~uint64_t(0) : uint64_t(c.endSec * ats + 0.5);
    for (size_t j = c.aFragStart; j < c.aFragEnd; j++) {
        uint64_t t = m_aFragStartTicks[j];
        const std::vector<FragSample> &fs = m_aFragSamples[j];
        for (size_t i = 0; i < fs.size(); i++) {
            if (t >= S && t < E) c.aSamples.push_back(fs[i]);
            t += fs[i].duration;
        }
    }
    c.assembled = true;
    queueBodyJobs(k);
}

void ChunkedRemuxSession::queueVideoBodyJobs(size_t k)
{
    Chunk &c = m_chunks[k];
    std::vector<RunPart> parts;
    // After a failed structure check, refetch as ONE request: every extra
    // parallel request is another chance for the relay to answer with wrong
    // bytes, so the speed-up that parallelism gives is not worth it here.
    qint64 bytes = buildRunParts(c.vSamples, 0, c.vSamples.size(), &parts,
            c.verifyRetries > 0 ? 1 : MAX_PARTS);
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
    qint64 bytes = buildRunParts(c.aSamples, 0, c.aSamples.size(), &parts, MAX_PARTS);
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

    int n = int(m_chunks.size());
    int working = 0;
    for (int k = 0; k < n; k++) if (m_chunks[k].state == 1) working++;
    // Start new chunks from the download head onward (wrapping round to fill
    // earlier gaps once everything after the head is stored).
    for (int i = 0; i < n && working < int(CHUNK_WINDOW); i++) {
        int kk = (m_head + i) % n;
        if (m_chunks[kk].state == 0) {
            scheduleDiscovery(size_t(kk));
            working++;
        }
    }
    tryAssemble();

    while (m_inflight.size() < MAX_INFLIGHT && !m_queue.isEmpty()) {
        // Nearest the head first, so the chunk the player needs next always
        // wins bandwidth over prefetching later ones.
        int best = 0;
        int bestDist = dist(m_queue[0]->chunk);
        for (int i = 1; i < m_queue.size(); i++) {
            int d = dist(m_queue[i]->chunk);
            if (d < bestDist) {
                bestDist = d;
                best = i;
            }
        }
        Job *job = m_queue.takeAt(best);
        dispatch(job);
    }
    if (!m_watchdog->isActive() && !isDownloadComplete()) m_watchdog->start(WATCHDOG_MS);
}

void ChunkedRemuxSession::dispatch(Job *job)
{
    bool isVideo = (job->type == JobMoofVideo || job->type == JobBodyVideo);
    QNetworkRequest req(QUrl::fromEncoded((isVideo ? m_videoUrl : m_audioUrl).toUtf8()));
    req.setRawHeader("Range",
            "bytes=" + QByteArray::number(job->start) + "-"
                    + QByteArray::number(job->start + job->len - 1));
    if (job->retries > 0 || job->twin) {
        req.setRawHeader("Connection", "close");
    }
    QNetworkReply *reply = m_nm->get(req);
    job->reply = reply;
    job->startedAt.start();
    job->lastActivity.start();
    m_inflight.insert(reply, job);
    QObject::connect(reply, SIGNAL(finished()), this, SLOT(onJobFinished()));
    QObject::connect(reply, SIGNAL(downloadProgress(qint64, qint64)), this,
            SLOT(onJobProgress(qint64, qint64)));
}

void ChunkedRemuxSession::onJobProgress(qint64 received, qint64 total)
{
    Q_UNUSED(received);
    Q_UNUSED(total);
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(QObject::sender());
    Job *job = m_inflight.value(reply, 0);
    if (job) job->lastActivity.start();
}

// Removes a job that is either in flight or still queued, and frees it.
void ChunkedRemuxSession::cancelJob(Job *job)
{
    if (job->reply) {
        m_inflight.remove(job->reply);
        QObject::disconnect(job->reply, 0, this, 0);
        job->reply->abort();
        job->reply->deleteLater();
        job->reply = 0;
    } else {
        m_queue.removeAll(job);
    }
    delete job;
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
        if (job->twin) job->twin->twin = 0;
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

    // This request won the race against its hedged twin: the twin is dropped.
    if (job->twin) {
        Job *t = job->twin;
        job->twin = 0;
        t->twin = 0;
        cancelJob(t);
    }

    if (job->type == JobMoofVideo || job->type == JobMoofAudio) {
        handleMoof(job, data);
    } else {
        handleBody(job, data);
    }
    pump();
}

void ChunkedRemuxSession::retryJob(Job *job, const QString &why)
{
    if (job->twin) {
        // The duplicate request is still running; let it carry on alone.
        job->twin->twin = 0;
        job->twin = 0;
        delete job;
        return;
    }
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

// Once a second: aborts stalled requests (they are then retried) and, for the
// chunk the player will need next, races a duplicate request against any
// request that is merely slow. One hung connection used to hold the very first
// chunk back for ~20s while chunks behind it had long arrived.
void ChunkedRemuxSession::onWatchdog()
{
    if (m_cancelled || m_failed) return;

    if (m_videoHeadReply && m_videoHeadClock.elapsed() > HEAD_TIMEOUT_MS) m_videoHeadReply->abort();
    if (m_audioHeadReply && m_audioHeadClock.elapsed() > HEAD_TIMEOUT_MS) m_audioHeadReply->abort();
    if (m_cancelled || m_failed) return;

    QList<QNetworkReply *> replies = m_inflight.keys();
    for (int i = 0; i < replies.size(); i++) {
        if (m_cancelled || m_failed) return;
        Job *j = m_inflight.value(replies[i], 0);
        if (j && j->lastActivity.elapsed() > STALL_MS) {
            qDebug() << "[bbtube][chunk] request type" << j->type << "chunk" << j->chunk << "stalled for"
                     << j->lastActivity.elapsed() << "ms - aborting";
            replies[i]->abort(); // -> onJobFinished with an error -> retry
        }
    }
    if (m_cancelled || m_failed || !m_headsDone) return;

    // Hedge the head-of-line chunk.
    int headChunk = -1, headDist = 0;
    for (size_t k = 0; k < m_chunks.size(); k++) {
        if (m_chunks[k].state != 1) continue;
        int d = dist(int(k));
        if (headChunk < 0 || d < headDist) {
            headChunk = int(k);
            headDist = d;
        }
    }
    if (headChunk < 0) return;
    replies = m_inflight.keys();
    for (int i = 0; i < replies.size(); i++) {
        if (m_inflight.size() >= MAX_INFLIGHT + MAX_HEDGE_EXTRA) break;
        Job *j = m_inflight.value(replies[i], 0);
        if (!j || j->chunk != headChunk || j->twin) continue;
        if (j->startedAt.elapsed() < HEDGE_MS) continue;
        Job *t = new Job();
        t->type = j->type;
        t->chunk = j->chunk;
        t->frag = j->frag;
        t->start = j->start;
        t->len = j->len;
        t->bufOffset = j->bufOffset;
        t->retries = 0;
        t->twin = j;
        j->twin = t;
        qDebug() << "[bbtube][chunk] chunk" << headChunk << "request type" << j->type << "is slow ("
                 << j->startedAt.elapsed() << "ms) - racing a duplicate";
        dispatch(t);
    }
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
    bool videoOk = verifyAvcSamples(m_videoHead, c.vSamples, 0, c.vSamples.size(),
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
    bool audioOk = verifyAacSamples(m_audioHead, c.aSamples, 0, c.aSamples.size(),
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
    storeChunk(k);
}

void ChunkedRemuxSession::storeChunk(size_t k)
{
    Chunk &c = m_chunks[k];
    c.vOff = m_vStageBytes;
    c.vLen = c.vBuf.size();
    c.aOff = m_aStageBytes;
    c.aLen = c.aBuf.size();
    if (m_vStage->write(c.vBuf) != c.vLen || m_aStage->write(c.aBuf) != c.aLen) {
        failWith("failed writing to the staging files (disk full?)");
        return;
    }
    m_vStage->flush();
    m_aStage->flush();
    m_vStageBytes += c.vLen;
    m_aStageBytes += c.aLen;
    c.vBuf = QByteArray();
    c.aBuf = QByteArray();
    c.state = 2;
    m_storedCount++;
    if (int(k) == m_head) {
        int next = firstMissingFrom(int(k) + 1 < int(m_chunks.size()) ? int(k) + 1 : 0);
        if (next >= 0) m_head = next;
    }
    qDebug() << "[bbtube][chunk] chunk" << qint64(k) << "stored at" << m_clock.elapsed() << "ms ("
             << m_storedCount << "of" << qint64(m_chunks.size()) << "), head now" << m_head;
    if (isDownloadComplete()) {
        m_watchdog->stop();
    }
    emit progress(int(k));
}

// ---------------------------------------------------------------------------
// Merging
// ---------------------------------------------------------------------------
void ChunkedRemuxSession::requestMerge(int startChunk)
{
    if (m_cancelled || !m_headsDone || !m_vStage || !m_aStage) return;
    if (!isChunkStored(startChunk)) return;
    m_mergeNextStart = startChunk;
    if (!m_mergeActive) {
        startNextMerge();
    }
}

void ChunkedRemuxSession::closeMergeFiles(bool removePartial)
{
    if (m_mergeOut) {
        QString name = m_mergeOut->fileName();
        m_mergeOut->close();
        if (removePartial) QFile::remove(name);
    }
    delete m_mergeOut; m_mergeOut = 0;
    delete m_mergeAin; m_mergeAin = 0;
    delete m_mergeVin; m_mergeVin = 0;
}

void ChunkedRemuxSession::startNextMerge()
{
    int a = m_mergeNextStart;
    m_mergeNextStart = -1;
    if (!isChunkStored(a)) return;
    int b = runEnd(a);

    TrackHead v = m_videoHead;
    TrackHead au = m_audioHead;
    v.fragSamples.clear();
    au.fragSamples.clear();
    m_mergeASegs.clear();
    m_mergeVSegs.clear();
    qint64 vBytes = 0, aBytes = 0;
    for (int k = a; k < b; k++) {
        const Chunk &c = m_chunks[k];
        v.fragSamples.insert(v.fragSamples.end(), c.vSamples.begin(), c.vSamples.end());
        au.fragSamples.insert(au.fragSamples.end(), c.aSamples.begin(), c.aSamples.end());
        if (c.aLen > 0) m_mergeASegs.push_back(std::make_pair(c.aOff, c.aLen));
        if (c.vLen > 0) m_mergeVSegs.push_back(std::make_pair(c.vOff, c.vLen));
        aBytes += c.aLen;
        vBytes += c.vLen;
    }
    std::string err;
    if (v.fragSamples.empty() || au.fragSamples.empty()
            || !planPartialRemux(v, au, v.fragSamples.size(), au.fragSamples.size(), &m_mergePlan, &err)) {
        failWith(QString("cannot plan merged file: %1").arg(QString(err.c_str())));
        return;
    }
    if (qint64(m_mergePlan.videoBodySize) != vBytes || qint64(m_mergePlan.audioBodySize) != aBytes) {
        failWith("internal error: staged byte counts do not match the sample tables");
        return;
    }

    m_mergeStart = a;
    m_mergeCount = b - a;
    m_mergeFinal = (a == 0 && b == int(m_chunks.size()));
    m_mergeSeq++;
    m_mergePath = mergedPathFor(m_mergeSeq, m_mergeFinal);

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
    m_mergeASegIdx = 0;
    m_mergeVSegIdx = 0;
    m_mergeSegLeft = 0;
    m_mergeSegOpen = false;
    m_mergeActive = true;
    m_mergeClock.start();
    QTimer::singleShot(0, this, SLOT(onMergeStep()));
}

void ChunkedRemuxSession::onMergeStep()
{
    if (!m_mergeActive || m_cancelled) return;

    bool audioPhase = m_mergeASegIdx < m_mergeASegs.size();
    QFile *in = audioPhase ? m_mergeAin : m_mergeVin;
    if (!m_mergeSegOpen) {
        const std::vector<std::pair<qint64, qint64> > &segs = audioPhase ? m_mergeASegs : m_mergeVSegs;
        size_t idx = audioPhase ? m_mergeASegIdx : m_mergeVSegIdx;
        if (idx >= segs.size()) {
            finishMerge();
            return;
        }
        if (!in->seek(segs[idx].first)) {
            failWith("merge: cannot seek in staging file");
            return;
        }
        m_mergeSegLeft = segs[idx].second;
        m_mergeSegOpen = true;
    }
    qint64 n = m_mergeSegLeft < MERGE_SLICE_BYTES ? m_mergeSegLeft : MERGE_SLICE_BYTES;
    QByteArray b = in->read(n);
    if (qint64(b.size()) != n || m_mergeOut->write(b) != n) {
        failWith("merge copy failed");
        return;
    }
    m_mergeSegLeft -= n;
    if (m_mergeSegLeft == 0) {
        m_mergeSegOpen = false;
        if (audioPhase) m_mergeASegIdx++; else m_mergeVSegIdx++;
    }
    QTimer::singleShot(0, this, SLOT(onMergeStep()));
}

void ChunkedRemuxSession::finishMerge()
{
    closeMergeFiles(false);

    QString part = m_mergePath + ".part";
    QFile::remove(m_mergePath);
    if (!QFile::rename(part, m_mergePath)) {
        m_mergeActive = false;
        failWith("cannot move merged file into place");
        return;
    }
    m_mergeActive = false;
    qDebug() << "[bbtube][chunk] merged file" << m_mergePath << "ready at" << m_clock.elapsed()
             << "ms, chunks" << m_mergeStart << "to" << (m_mergeStart + m_mergeCount - 1)
             << (m_mergeFinal ? "(FINAL)" : "") << "- copy took" << m_mergeClock.elapsed() << "ms for"
             << qint64(m_mergePlan.totalOutputSize / 1024) << "KB";

    // Merged files from two merges ago are no longer needed: the player has
    // long since swapped away from them. The final copy is the cache entry.
    if (!m_mergeFinal) {
        m_mergedFiles.append(m_mergePath);
        while (m_mergedFiles.size() > 3) {
            QFile::remove(m_mergedFiles.takeFirst());
        }
    }
    QString path = m_mergePath;
    int a = m_mergeStart, count = m_mergeCount;
    bool isFinal = m_mergeFinal;
    emit mergedReady(path, a, count, isFinal);

    if (m_mergeNextStart >= 0 && !m_cancelled) {
        startNextMerge();
    }
}

// ---------------------------------------------------------------------------
// Teardown / failure
// ---------------------------------------------------------------------------
void ChunkedRemuxSession::teardown(bool removeStageFiles)
{
    m_watchdog->stop();
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
        if (j) {
            j->twin = 0;
            delete j;
        }
    }
    qDeleteAll(m_queue);
    m_queue.clear();
    qDeleteAll(m_retryQueue);
    m_retryQueue.clear();

    if (m_mergeActive || m_mergeOut) {
        m_mergeActive = false;
        closeMergeFiles(true);
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
