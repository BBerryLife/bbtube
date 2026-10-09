#include "InvidiousClient.hpp"
#include "src/applicationui.hpp"
#include "src/utils/AudioTrackPicker.hpp"

#include <bb/data/JsonDataAccess>

#include <QtNetwork/QNetworkRequest>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <QSet>
#include <QDebug>
#include <QtNetwork/QSslCertificate>
#include <QTimer>

InvidiousClient::InvidiousClient(InvidiousInstanceManager *instanceManager, QObject *parent) :
        QObject(parent), instanceManager(instanceManager), pendingReply(0)
{
}

void InvidiousClient::fetchVideo(const QString &videoId)
{
    QString instanceBaseUrl = instanceManager->pickRandomInstance();
    if (instanceBaseUrl.isEmpty()) {
        emit error("No Invidious instance available");
        return;
    }
    requestFromInstance(videoId, instanceBaseUrl, 1);
}

void InvidiousClient::requestFromInstance(const QString &videoId, const QString &instanceBaseUrl,
        int attemptNumber)
{
    qDebug() << "[bbtube][invidious] attempt" << attemptNumber << "requesting" << videoId
             << "from" << instanceBaseUrl;

    QNetworkRequest request(
            instanceBaseUrl + "/api/v1/videos/" + videoId + "?local=true");
    request.setRawHeader("User-Agent", "bbtube (BlackBerry 10)");

    QNetworkReply *reply = ApplicationUI::networkManager->get(request);
    pendingReply = reply;
    reply->setProperty("videoId", videoId);
    reply->setProperty("attemptNumber", attemptNumber);
    QObject::connect(reply, SIGNAL(finished()), this, SLOT(onVideoRequestFinished()));
    QObject::connect(reply, SIGNAL(sslErrors(QList<QSslError>)), this,
            SLOT(onSslErrors(QList<QSslError>)));

    // Same rationale as InvidiousInstanceManager::refreshInstanceList():
    // no built-in request timeout on this Qt4 build, so without this a
    // stalled handshake against one bad instance would hang the video
    // load indefinitely (the "spins forever, never gets to the player"
    // symptom) instead of failing fast and retrying the next instance.
    //
    // NOT connected via QTimer::singleShot(ms, reply, SLOT(abort())) --
    // that logs "Object::connect: No such slot QNetworkReplyImpl::abort()"
    // on this BB10/Qt4 build and never actually fires, because
    // QNetworkReply::abort() is pure virtual and the concrete
    // QNetworkReplyImpl doesn't re-expose it through moc the way a normal
    // Q_SLOT needs. Routing through our own onFetchTimeout() slot and
    // calling reply->abort() as a plain (non-signal-slot) virtual call
    // sidesteps that entirely. This was confirmed to be the actual cause
    // of requests hanging past their intended 10s timeout in practice.
    QTimer::singleShot(10000, this, SLOT(onFetchTimeout()));
}

void InvidiousClient::onFetchTimeout()
{
    if (pendingReply && !pendingReply->isFinished()) {
        qDebug() << "[bbtube][invidious] video request timed out after 10s, aborting";
        pendingReply->abort(); // triggers finished() -> onVideoRequestFinished() with an error set
    }
}

void InvidiousClient::onSslErrors(const QList<QSslError> &errors)
{
    for (int i = 0; i < errors.count(); i++) {
        qDebug() << "[bbtube][invidious][ssl] error" << i << ":" << errors[i].errorString()
                 << "(code" << errors[i].error() << ")";
        QSslCertificate cert = errors[i].certificate();
        if (!cert.isNull()) {
            qDebug() << "[bbtube][invidious][ssl]   certificate subject:"
                     << cert.subjectInfo(QSslCertificate::CommonName) << ", issuer:"
                     << cert.issuerInfo(QSslCertificate::CommonName) << ", expires:"
                     << cert.expiryDate().toString();
        }
    }
}

void InvidiousClient::onVideoRequestFinished()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply) {
        return;
    }
    if (reply == pendingReply) {
        pendingReply = 0;
    }
    reply->deleteLater();

    QString videoId = reply->property("videoId").toString();
    int attemptNumber = reply->property("attemptNumber").toInt();
    QVariant httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);

    bool networkOk = reply->error() == QNetworkReply::NoError;
    QString body = QString::fromUtf8(reply->readAll());

    qDebug() << "[bbtube][invidious] attempt" << attemptNumber << "response for" << videoId
             << "-- url:" << reply->url().toString()
             << ", network error code:" << reply->error()
             << ", network error string:" << reply->errorString()
             << ", http status:" << httpStatus
             << ", body length:" << body.length();

    QVariantMap videoMap;
    bool parsedOk = false;
    if (networkOk && !body.isEmpty()) {
        bb::data::JsonDataAccess jda;
        QVariant parsed = jda.loadFromBuffer(body);
        videoMap = parsed.toMap();
        // A successful Invidious video response always has a non-empty
        // "videoId" field; an error response instead has an "error"
        // string field and no videoId. Use that as our parse-succeeded
        // check rather than trusting the HTTP status code alone (some
        // instances return HTTP 200 with a JSON error body).
        parsedOk = !videoMap["videoId"].toString().isEmpty();

        if (!parsedOk) {
            // Either malformed JSON, or a well-formed Invidious error
            // response like {"error": "..."} -- log which, and the
            // instance's own error message if present, rather than just
            // "failed" with no explanation.
            QString instanceError = videoMap["error"].toString();
            if (!instanceError.isEmpty()) {
                qDebug() << "[bbtube][invidious] attempt" << attemptNumber
                         << "instance reported error:" << instanceError;
            } else {
                qDebug() << "[bbtube][invidious] attempt" << attemptNumber
                         << "response body did not parse as a valid video JSON. First 200 chars:"
                         << body.left(200);
            }
        }
    }

    bool usable = parsedOk && hasUsableStreams(videoMap);

    if (parsedOk && !usable) {
        qDebug() << "[bbtube][invidious] attempt" << attemptNumber << "for" << videoId
                 << "-- video JSON parsed OK but had no usable formatStreams/adaptiveFormats URLs"
                    " (likely SABR-blocked on this instance too)";
    }

    if (!usable) {
        qDebug() << "[bbtube][invidious] attempt" << attemptNumber << "for" << videoId
                  << "failed or had no usable streams"
                  << (networkOk ? "" : QString("(network error: %1)").arg(reply->errorString()));

        if (attemptNumber < MAX_INSTANCE_ATTEMPTS) {
            QString nextInstance = instanceManager->pickRandomInstance();
            if (!nextInstance.isEmpty()) {
                qDebug() << "[bbtube][invidious] retrying with instance:" << nextInstance;
                requestFromInstance(videoId, nextInstance, attemptNumber + 1);
                return;
            } else {
                qDebug() << "[bbtube][invidious] no next instance available to retry with";
            }
        } else {
            int maxAttempts = MAX_INSTANCE_ATTEMPTS;
            qDebug() << "[bbtube][invidious] reached MAX_INSTANCE_ATTEMPTS (" << maxAttempts
                     << "), giving up on Invidious for" << videoId;
        }

        emit error("Could not get a playable stream from any Invidious instance");
        return;
    }

    VideoMetadata metadata;
    mapToVideoMetadata(videoMap, &metadata);

    StorageData storageData;
    mapToStorageData(videoMap, reply->url(), &storageData);

    emit metadataReceived(metadata, storageData);
}

bool InvidiousClient::hasUsableStreams(const QVariantMap &videoMap)
{
    // Progressive streams are self-contained (audio+video already
    // combined) -- any non-empty URL here is directly playable.
    QVariantList formatStreams = videoMap["formatStreams"].toList();
    for (int i = 0; i < formatStreams.count(); i++) {
        if (!formatStreams[i].toMap()["url"].toString().isEmpty()) {
            return true;
        }
    }

    // Adaptive (video-only) streams need a paired audio track to be
    // playable at all (see mapToStorageData, which now only adds
    // video-only instances when a usable audio track is also present).
    // Checking for ANY non-empty adaptiveFormats URL here (as this used
    // to do) is not enough: an instance can return video-only entries
    // with real URLs but zero usable audio tracks, which
    // mapToStorageData correctly refuses to turn into a playable
    // instance -- but this function would have called that instance
    // "usable" anyway, so InvidiousClient never retried a different
    // instance and the video silently failed to play (or played
    // without sound / MediaError) instead.
    QVariantList adaptiveFormats = videoMap["adaptiveFormats"].toList();
    bool hasVideo = false;
    bool hasAudio = false;
    for (int i = 0; i < adaptiveFormats.count(); i++) {
        QVariantMap fmt = adaptiveFormats[i].toMap();
        if (fmt["url"].toString().isEmpty()) {
            continue;
        }
        QString type = fmt["type"].toString();
        if (type.startsWith("audio/")) {
            hasAudio = true;
        } else if (type.startsWith("video/")) {
            hasVideo = true;
        }
        if (hasVideo && hasAudio) {
            return true;
        }
    }

    return false;
}

void InvidiousClient::mapToVideoMetadata(const QVariantMap &videoMap, VideoMetadata *outMetadata)
{
    SingleVideoMetadata video;
    video.title = videoMap["title"].toString();
    video.videoId = videoMap["videoId"].toString();
    video.channelId = videoMap["authorId"].toString();
    video.channelTitle = videoMap["author"].toString();
    // Invidious gives lengthSeconds as an integer, but SingleVideoMetadata
    // (shared with the InnerTube path) stores a pre-formatted lengthText
    // and treats "" as "this is a live stream" (see
    // SingleVideoMetadata::isLiveStream()). We don't need a fully
    // formatted "H:MM:SS" here -- lengthText is only read by
    // isLiveStream() and a couple of list-item labels elsewhere -- so a
    // simple non-empty placeholder is enough to mark this as non-live;
    // liveNow (checked separately, see mapToStorageData) is what actually
    // drives live-stream handling in StorageData/PlayerPage.
    bool liveNow = videoMap["liveNow"].toBool();
    video.lengthText = liveNow ? "" : QString::number(videoMap["lengthSeconds"].toInt());
    video.viewsCount = QString::number(videoMap["viewCount"].toLongLong());
    video.shortViewsCount = video.viewsCount;
    video.dateUploadedAgo = videoMap["publishedText"].toString();

    QVariantList thumbnails = videoMap["videoThumbnails"].toList();
    if (!thumbnails.isEmpty()) {
        // Thumbnails are listed largest-first in Invidious's schema; take
        // a mid-sized one if available, else whatever's there.
        int idx = thumbnails.count() > 2 ? thumbnails.count() / 2 : 0;
        video.thumbnailUrl = thumbnails[idx].toMap()["url"].toString();
    }

    outMetadata->video = video;

    QVariantList recommended = videoMap["recommendedVideos"].toList();
    for (int i = 0; i < recommended.count(); i++) {
        QVariantMap rec = recommended[i].toMap();
        SingleVideoMetadata relatedVideo;
        relatedVideo.videoId = rec["videoId"].toString();
        relatedVideo.title = rec["title"].toString();
        relatedVideo.channelTitle = rec["author"].toString();
        relatedVideo.shortViewsCount = rec["viewCountText"].toString();
        relatedVideo.viewsCount = relatedVideo.shortViewsCount;
        relatedVideo.lengthText = QString::number(rec["lengthSeconds"].toInt());

        QVariantList recThumbnails = rec["videoThumbnails"].toList();
        if (!recThumbnails.isEmpty()) {
            int idx = recThumbnails.count() > 2 ? recThumbnails.count() / 2 : 0;
            relatedVideo.thumbnailUrl = recThumbnails[idx].toMap()["url"].toString();
        }

        outMetadata->relatedVideos.otherVideos.append(relatedVideo);
    }
    if (!outMetadata->relatedVideos.otherVideos.isEmpty()) {
        outMetadata->relatedVideos.nextVideo = outMetadata->relatedVideos.otherVideos[0];
    }
}

// Ascending-quality comparator for qSort, matching the order
// PlayerPage::getIndexOfDefaultQuality() expects (it does a plain
// QString::compare walk assuming instances[] is sorted low-to-high by
// quality label, e.g. "144p" < "240p" < ... < "720p60"). String comparison
// alone isn't numerically correct ("1080p" < "720p" alphabetically!), so
// we extract the leading number from each label to compare numerically,
// falling back to string comparison only if that fails.
static int qualityLabelToNumber(const QString &label)
{
    QString digits;
    for (int i = 0; i < label.length(); i++) {
        if (label[i].isDigit()) {
            digits += label[i];
        } else if (!digits.isEmpty()) {
            break; // stop at the first non-digit run after we've seen digits (handles "720p60" -> "720")
        }
    }
    bool ok = false;
    int value = digits.toInt(&ok);
    return ok ? value : 0;
}

static bool singleVideoStorageDataLessThan(const SingleVideoStorageData &a,
        const SingleVideoStorageData &b)
{
    return qualityLabelToNumber(a.quality) < qualityLabelToNumber(b.quality);
}

QString InvidiousClient::resolveStreamUrl(const QString &url, const QUrl &instanceUrl)
{
    QUrl parsed(url);
    if (!parsed.scheme().isEmpty()) {
        return url; // already a complete URL -- nothing to fix
    }

    // Known Invidious server-side bug (see iv-org/invidious PR #4992,
    // "Fix missing host parameter on playback URLs when local=true"):
    // some instances/versions return adaptiveFormats[].url as a
    // path-only string (e.g. "/videoplayback?expire=...") instead of a
    // full URL, when it should have been "proxified" to include the
    // instance's own scheme+host. Video head fetches then fail with
    // "Protocol \"\" is unknown" because there's no scheme at all.
    // Patch it up here using the scheme+host+port of the instance that
    // actually served this response (reply->url(), passed through as
    // instanceUrl) -- this is exactly what the instance itself should
    // have done server-side.
    QString base = instanceUrl.scheme() + "://" + instanceUrl.authority();
    if (url.startsWith("/")) {
        return base + url;
    }
    return base + "/" + url;
}

void InvidiousClient::mapToStorageData(const QVariantMap &videoMap, const QUrl &instanceUrl,
        StorageData *outStorageData)
{
    // Remember which instance these urls came from: its media proxy can fail
    // even though its metadata api answered (see PlayerPage::onSourceFailing).
    outStorageData->sourceInstance = instanceUrl.scheme() + "://" + instanceUrl.authority();
    bool liveNow = videoMap["liveNow"].toBool();

    if (liveNow) {
        QString hlsUrl = videoMap["hlsUrl"].toString();
        if (!hlsUrl.isEmpty()) {
            SingleVideoStorageData live;
            live.url = resolveStreamUrl(hlsUrl, instanceUrl);
            live.quality = "live";
            live.hasEmbeddedAudio = true;
            // duration intentionally left at its default (0) here --
            // PlayerPage::playVideo() treats duration == 0 as the signal
            // for "this is a livestream" (isLiveStream = duration == 0),
            // which is exactly correct in this liveNow branch.
            outStorageData->instances.append(live);
        }
        return;
    }

    // NOT a livestream past this point. PlayerPage::playVideo() computes
    // isLiveStream as (instances[0].duration == 0) -- so every instance
    // below MUST have a real, non-zero duration, or an ordinary video
    // gets misdetected as a livestream. That misdetection was the actual
    // root cause of adaptive (video-only) streams being played directly
    // instead of going through remux: PlayerPage's remux-vs-direct-play
    // check is `if (!isLiveStream && !data.hasEmbeddedAudio && ...)`, and
    // duration was never being set here at all (silently staying at the
    // SingleVideoStorageData default of 0), which made isLiveStream true
    // for every single video and short-circuited that check.
    //
    // lengthSeconds is a per-VIDEO field (not per-format), so this is
    // computed once here and reused for every instance -- more reliable
    // than adaptiveFormats[].approxDurationMs, which per Invidious's own
    // API docs isn't guaranteed present on every format entry.
    int durationMs = videoMap["lengthSeconds"].toInt() * 1000;

    // Progressive (audio+video combined) streams -- these can be played
    // directly, no remux needed. Matches StorageParser's handling of
    // InnerTube's "formats" array (see hasEmbeddedAudio comment in
    // StorageData.hpp).
    QVariantList formatStreams = videoMap["formatStreams"].toList();
    for (int i = 0; i < formatStreams.count(); i++) {
        QVariantMap fmt = formatStreams[i].toMap();
        QString url = fmt["url"].toString();
        if (url.isEmpty()) {
            continue;
        }
        SingleVideoStorageData instance;
        instance.url = resolveStreamUrl(url, instanceUrl);
        instance.quality = fmt["qualityLabel"].toString();
        instance.hasEmbeddedAudio = true;
        instance.duration = durationMs;
        outStorageData->instances.append(instance);
    }

    // Adaptive (video-only) streams -- highest-quality options, but need
    // pairing with a separate audio track and remuxing (see
    // StreamingRemuxSession) before BB10's mmrenderer can play them.
    QVariantList adaptiveFormats = videoMap["adaptiveFormats"].toList();

    // PASS 1: find the best available audio track first, scanning the
    // WHOLE adaptiveFormats list. This has to happen before deciding
    // which video-only entries to keep (pass 2) -- audio and video
    // entries can appear in any order in the array, so if we only
    // scanned once and appended video instances as we went, an audio
    // track appearing later in the list would be missed for instances
    // already added, leaving storageData.audio.url empty even though a
    // usable audio track existed. That was silently producing
    // video-only instances with hasEmbeddedAudio=false but no paired
    // audio -- which PlayerPage's remux-vs-direct-play check
    // (!hasEmbeddedAudio && audio.url != "") doesn't catch, so it fell
    // through to direct playback of a video-only (silent, and on some
    // devices outright unplayable) stream.
    QString bestAudioUrl;
    unsigned long long bestAudioBitrate = 0;
    bool bestAudioIsMp4 = false;
    int bestAudioScore = -1; // see audioOriginalScore(): 2 original, 1 unmarked, 0 dub
    QString bestAudioLabel;
    int audioCandidatesSeen = 0;
    int originalAudioSeen = 0;
    int dubbedAudioSeen = 0;

    for (int i = 0; i < adaptiveFormats.count(); i++) {
        QVariantMap fmt = adaptiveFormats[i].toMap();
        QString url = fmt["url"].toString();
        if (url.isEmpty()) {
            continue;
        }
        QString type = fmt["type"].toString(); // e.g. "audio/mp4; codecs=\"mp4a.40.2\"" or "audio/webm; codecs=\"opus\""
        if (!type.startsWith("audio/")) {
            continue;
        }
        audioCandidatesSeen++;

        int score = audioOriginalScore(url, fmt);
        if (score == 2) originalAudioSeen++;
        if (score == 0) dubbedAudioSeen++;

        // Ranking, strongest first:
        //  1. container: mp4_stream_remux only understands ISOBMFF/MP4 --
        //     an audio/webm (Opus) track is a different container (EBML)
        //     and will never contain an "ftyp" box, so any mp4 track beats
        //     any webm one (webm only if no mp4 audio exists at all,
        //     matching StorageParser's InnerTube path).
        //  2. language: the original track beats unmarked, which beats
        //     dubs (YouTube auto-dubs popular videos into dozens of
        //     languages; choosing by bitrate alone picked e.g. a Portuguese
        //     dub of an English video).
        //  3. bitrate.
        bool isMp4Audio = type.contains("audio/mp4");
        unsigned long long bitrate = fmt["bitrate"].toString().toULongLong();

        bool shouldReplace;
        if (bestAudioUrl.isEmpty()) {
            shouldReplace = true;
        } else if (isMp4Audio != bestAudioIsMp4) {
            shouldReplace = isMp4Audio;
        } else if (score != bestAudioScore) {
            shouldReplace = score > bestAudioScore;
        } else {
            shouldReplace = bitrate >= bestAudioBitrate;
        }
        if (shouldReplace) {
            bestAudioBitrate = bitrate;
            bestAudioUrl = url;
            bestAudioIsMp4 = isMp4Audio;
            bestAudioScore = score;
            bestAudioLabel = audioTrackDebugLabel(url, fmt);
        }
    }
    qDebug() << "[bbtube][invidious] audio pick:" << bestAudioLabel << ", score" << bestAudioScore
             << "(2=original 1=unmarked 0=dub) ; candidates:" << audioCandidatesSeen
             << ", marked original:" << originalAudioSeen << ", marked dub/other:" << dubbedAudioSeen;
    if (bestAudioScore == 0 && originalAudioSeen == 0) {
        qDebug() << "[bbtube][invidious] WARNING: every mp4 audio track is a dub; the original may only exist as webm";
    }

    if (!bestAudioUrl.isEmpty()) {
        outStorageData->audio.url = resolveStreamUrl(bestAudioUrl, instanceUrl);
        outStorageData->audio.contentLength = 0; // not surfaced separately by Invidious's schema per-track here
    }

    // PASS 2: add video-only instances, but ONLY if pass 1 actually
    // found a usable audio track to pair them with via remux. A
    // video-only instance with no audio anywhere in this response can't
    // be played correctly by this app (no remux is possible, and direct
    // playback would be silent/fail) -- skip adding it entirely rather
    // than let it become a selectable-but-broken quality option.
    // formatStreams (progressive, handled above) remain available
    // regardless, since those already include their own audio.
    int videoCandidatesAdded = 0;
    bool hasUsableAudio = !bestAudioUrl.isEmpty();

    // Quality labels already covered by a progressive (formatStreams)
    // instance added above -- YouTube commonly exposes the same
    // resolution (e.g. "360p") both as a progressive itag 18 stream AND
    // as a video-only adaptiveFormats entry (e.g. itag 134) meant to be
    // paired with separate audio. Without this check both get appended
    // to outStorageData->instances and the quality picker shows "360p"
    // twice -- functionally harmless (both play) but confusing, and
    // wasteful since the progressive stream is strictly simpler (no
    // remux needed). Skip the adaptive duplicate; the progressive
    // instance already covers that quality.
    QSet<QString> existingQualityLabels;
    for (int i = 0; i < outStorageData->instances.count(); i++) {
        existingQualityLabels.insert(outStorageData->instances[i].quality);
    }

    for (int i = 0; i < adaptiveFormats.count(); i++) {
        QVariantMap fmt = adaptiveFormats[i].toMap();
        QString url = fmt["url"].toString();
        if (url.isEmpty()) {
            continue;
        }
        QString type = fmt["type"].toString(); // e.g. "video/mp4; codecs=\"avc1.4d401f\""

        if (!type.startsWith("video/")) {
            continue; // audio entries already handled in pass 1
        }
        if (!hasUsableAudio) {
            continue; // no audio to pair this with -- see comment above
        }
        // Only H.264 (avc1) mp4 video is usable by BB10's mmrenderer /
        // this app's remuxer -- skip VP9/AV1/webm, matching
        // StorageParser's existing H.264-only filtering for the
        // InnerTube path.
        if (!type.contains("mp4") || !type.contains("avc1")) {
            continue;
        }

        QString qualityLabel = fmt["qualityLabel"].toString();
        if (qualityLabel.isEmpty()) {
            continue;
        }
        if (existingQualityLabels.contains(qualityLabel)) {
            continue; // already covered by a progressive instance -- see comment above
        }

        // Cap at 720p: 1080p (itag 137/299, 30fps or 60fps) has proven
        // unreliable on-device -- the remux plus mmrenderer playback of a
        // video that large regularly triggers a low-memory kill (see the
        // "received low memory signal" crashes in device logs), and the
        // long download window at that bitrate/duration also gives flaky
        // Invidious proxies more opportunity to drop a request mid-stream.
        // 720p (itag 136/298) is comfortably smaller and has been stable,
        // so don't even offer anything above it in the quality picker.
        // qualityLabelToNumber() (below) already exists in this file for
        // sorting instances by resolution, so reuse it here rather than
        // duplicating the "parse leading digits" logic.
        if (qualityLabelToNumber(qualityLabel) > 720) {
            continue;
        }

        SingleVideoStorageData instance;
        instance.url = resolveStreamUrl(url, instanceUrl);
        instance.quality = qualityLabel;
        instance.hasEmbeddedAudio = false;
        instance.contentLength = fmt["clen"].toString().toULongLong();
        instance.duration = durationMs;
        outStorageData->instances.append(instance);
        existingQualityLabels.insert(qualityLabel);
        videoCandidatesAdded++;
    }

    qDebug() << "[bbtube][invidious] mapToStorageData: adaptiveFormats total ="
             << adaptiveFormats.count() << ", audio candidates seen =" << audioCandidatesSeen
             << ", usable audio found?" << hasUsableAudio << ", chosen audio is mp4?" << bestAudioIsMp4
             << ", video (h264/mp4) instances added =" << videoCandidatesAdded
             << ", formatStreams (progressive) added =" << outStorageData->instances.count() - videoCandidatesAdded;

    qSort(outStorageData->instances.begin(), outStorageData->instances.end(),
            singleVideoStorageDataLessThan);
}
