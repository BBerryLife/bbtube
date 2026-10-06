#include "StorageParser.hpp"
#include "src/parser/models/StorageData.hpp"
#include "src/utils/AudioTrackPicker.hpp"

#include <bb/data/JsonDataAccess>
#include <QUrl>

// StorageParser: turns a player-response JSON payload (from either the
// legacy ytplayer.config blob or the modern InnerTube /player endpoint)
// into a StorageData object with playable URLs.
//
// KEY CHANGES vs. the original (2021) implementation:
//  - "playabilityStatus" is now checked first: if status != "OK" we bail out
//    early (age-gated / unavailable / login-required videos used to silently
//    return zero formats, causing a confusing "no instances" error downstream).
//  - YouTube removed the combined progressive "formats" array's signed URLs
//    for many videos; almost everything now arrives via "adaptiveFormats"
//    with either a direct "url" or a "signatureCipher"/"cipher" field.
//  - Live streams now expose "dashManifestUrl" in addition to
//    "hlsManifestUrl"; we prefer HLS for player compatibility.
//  - Caption track keys changed slightly; "vssId" can be absent so we now
//    fall back to "languageCode".

void StorageParser::parseFromHtml(StorageData *storageData, QString *json)
{
    bb::data::JsonDataAccess jda;
    QVariant parsed = jda.loadFromBuffer(*json);
    QVariantMap map = parsed.toMap();
    QString playerResponse = map["args"].toMap()["player_response"].toString();
    QVariantMap playerResponseMap = jda.loadFromBuffer(playerResponse).toMap();

    parseFromJsonInternal(storageData, &playerResponseMap);
}

void StorageParser::parseFromJson(StorageData *storageData, QString *json)
{
    bb::data::JsonDataAccess jda;
    QVariant parsed = jda.loadFromBuffer(*json);
    QVariantMap playerResponseMap = parsed.toMap();

    parseFromJsonInternal(storageData, &playerResponseMap);
}

void StorageParser::parseFromJsonInternal(StorageData *storageData,
        const QVariantMap *playerResponseMap)
{
    // ── Check playability status first ──────────────────────────────────────
    QVariantMap playabilityStatus = (*playerResponseMap)["playabilityStatus"].toMap();
    QString status = playabilityStatus["status"].toString();

    if (!status.isEmpty() && status != "OK" && status != "LIVE_STREAM") {
        // LOGIN_REQUIRED, ERROR, UNPLAYABLE, AGE_CHECK_REQUIRED, etc.
        // Leave storageData empty; caller (YoutubeClient) surfaces this as
        // "Source unavailable" via the existing error path.
        return;
    }

    QVariantMap streamingData = (*playerResponseMap)["streamingData"].toMap();

    // ── Live stream handling ────────────────────────────────────────────────
    if (streamingData.contains("hlsManifestUrl")) {
        SingleVideoStorageData video;
        video.url = QUrl::fromPercentEncoding(streamingData["hlsManifestUrl"].toByteArray());
        video.duration = 0;
        video.quality = "live";
        storageData->instances.append(video);
        return;
    }
    if (streamingData.contains("dashManifestUrl")) {
        SingleVideoStorageData video;
        video.url = QUrl::fromPercentEncoding(streamingData["dashManifestUrl"].toByteArray());
        video.duration = 0;
        video.quality = "live";
        storageData->instances.append(video);
        return;
    }

    // ── Progressive (video+audio combined) formats ─────────────────────────
    QVariantList formats = streamingData["formats"].toList();

    for (int i = 0; i < formats.count(); i++) {
        QVariantMap format = formats[i].toMap();
        SingleVideoStorageData video;

        video.quality = format["qualityLabel"].toString();
        video.duration = format["approxDurationMs"].toInt();
        video.contentLength = format["contentLength"].toString().toULongLong();
        video.hasEmbeddedAudio = true; // progressive == muxed video+audio

        if (format.contains("url")) {
            video.url = QUrl::fromPercentEncoding(format["url"].toByteArray());
        } else if (format.contains("signatureCipher")) {
            video.cipher = format["signatureCipher"].toString();
        } else if (format.contains("cipher")) {
            video.cipher = format["cipher"].toString();
        }

        if (video.url.isEmpty() && video.cipher.isEmpty()) {
            continue; // unusable entry, skip
        }

        storageData->instances.append(video);
    }

    // ── Adaptive formats: video-only AND audio-only streams ────────────────
    // Modern YouTube puts almost everything here. Progressive "formats" is
    // often empty/limited (sometimes capped at 720p or missing entirely for
    // certain client contexts), so adaptiveFormats is now the primary source
    // for both audio and (if needed) video-only fallback.
    QVariantList adaptiveFormats = streamingData["adaptiveFormats"].toList();

    bool haveAudio = false;
    bool bestAudioIsMp4 = false;
    int bestAudioScore = -1; // audioOriginalScore(): 2 original, 1 unmarked, 0 dub
    unsigned long long bestAudioBitrate = 0;

    for (int i = 0; i < adaptiveFormats.count(); i++) {
        QVariantMap format = adaptiveFormats[i].toMap();
        QString mimeType = format["mimeType"].toString();

        bool isAudio = mimeType.startsWith("audio/");
        bool isVideo = mimeType.startsWith("video/");

        if (isAudio) {
            // Prefer mp4/m4a audio (AAC) for widest device compatibility;
            // fall back to whatever is available if no mp4 audio exists.
            bool isMp4Audio = mimeType.contains("audio/mp4");

            // Ranking: mp4 over anything else (the remuxer needs ISOBMFF),
            // then the ORIGINAL language over auto-dubs (see
            // AudioTrackPicker.hpp), then bitrate. The old code kept
            // whichever mp4 track came last, i.e. an arbitrary language.
            // signatureCipher/cipher carry the url (and its xtags) inside.
            QString haystack = format["url"].toString() + "&" + format["signatureCipher"].toString()
                    + "&" + format["cipher"].toString();
            int score = audioOriginalScore(haystack, format);
            unsigned long long bitrate = format["bitrate"].toString().toULongLong();

            bool shouldReplace;
            if (!haveAudio) {
                shouldReplace = true;
            } else if (isMp4Audio != bestAudioIsMp4) {
                shouldReplace = isMp4Audio;
            } else if (score != bestAudioScore) {
                shouldReplace = score > bestAudioScore;
            } else {
                shouldReplace = bitrate >= bestAudioBitrate;
            }
            if (!shouldReplace) {
                continue;
            }

            AudioStorageData audio;

            if (format.contains("url")) {
                audio.url = QUrl::fromPercentEncoding(format["url"].toByteArray());
            } else if (format.contains("signatureCipher")) {
                audio.cipher = format["signatureCipher"].toString();
            } else if (format.contains("cipher")) {
                audio.cipher = format["cipher"].toString();
            }

            audio.contentLength = format["contentLength"].toString().toULongLong();

            if (!audio.url.isEmpty() || !audio.cipher.isEmpty()) {
                storageData->audio = audio;
                haveAudio = true;
                bestAudioIsMp4 = isMp4Audio;
                bestAudioScore = score;
                bestAudioBitrate = bitrate;
            }
        } else if (isVideo) {
            // NOTE: this used to be gated behind "storageData->instances.isEmpty()",
            // intended only as a fallback when the progressive "formats" array
            // gave us nothing. In practice, "formats" almost always contains
            // exactly one entry (itag 18, 360p) even when it's non-empty, so
            // that guard silently discarded every 480p/720p/1080p video-only
            // adaptive stream and left users stuck at 360p with the quality
            // picker disabled (it only enables when there's more than one
            // instance, or a usable audio track). We now always collect
            // video-only mp4 (H.264) adaptive formats alongside progressive
            // ones; PlayerPage is responsible for muxing video-only + audio
            // together (or falling back to silent/audio-only playback) since
            // BB10's mmrenderer cannot play two separate streams at once.
            if (!mimeType.contains("video/mp4")) {
                continue; // skip webm/vp9 and av1 — not decodable on BB10 hardware
            }

            // Cap at 720p: 1080p60 (itag 299) has proven unreliable on-device
            // -- the remux plus mmrenderer playback of a video that large
            // regularly triggers a low-memory kill (see the
            // "received low memory signal" crashes in device logs), and the
            // long download window at that bitrate/duration also gives
            // flaky Invidious proxies more opportunity to drop a request
            // mid-stream (e.g. an unexpected HTTP 302 partway through
            // fragment discovery). 720p60 (itag 298) is comfortably smaller
            // and has been stable, so until 1080p is worth the risk again,
            // don't even offer it in the quality picker. Checking the
            // numeric "height" field rather than string-matching
            // qualityLabel (e.g. "1080p60") is deliberate: YouTube's label
            // text/formatting isn't guaranteed stable across responses, but
            // height is a plain integer.
            int height = format["height"].toInt();
            if (height > 720) {
                continue;
            }

            SingleVideoStorageData video;
            video.quality = format["qualityLabel"].toString();
            video.duration = format["approxDurationMs"].toInt();
            video.contentLength = format["contentLength"].toString().toULongLong();
            // Adaptive video-only stream -- no audio track in this file.
            // PlayerPage must pair it with storageData->audio and remux
            // (StreamingRemuxSession) before it's playable with sound.
            video.hasEmbeddedAudio = false;

            if (format.contains("url")) {
                video.url = QUrl::fromPercentEncoding(format["url"].toByteArray());
            } else if (format.contains("signatureCipher")) {
                video.cipher = format["signatureCipher"].toString();
            } else if (format.contains("cipher")) {
                video.cipher = format["cipher"].toString();
            }

            if (!video.url.isEmpty() || !video.cipher.isEmpty()) {
                storageData->instances.append(video);
            }
        }
    }

    // ── Closed captions ──────────────────────────────────────────────────
    QVariantList captionList = (*playerResponseMap)["captions"].toMap()
            ["playerCaptionsTracklistRenderer"].toMap()["captionTracks"].toList();

    for (int i = 0; i < captionList.count(); i++) {
        QVariantMap captionMap = captionList[i].toMap();
        ClosedCaptionData cc;

        cc.isLoaded = false;

        QString vssId = captionMap["vssId"].toString();
        if (!vssId.isEmpty()) {
            cc.languageCode = vssId.replace(".", "");
        } else {
            cc.languageCode = captionMap["languageCode"].toString();
        }

        // "name" may be {"simpleText": "..."} or {"runs": [{"text": "..."}]}
        QVariantMap nameMap = captionMap["name"].toMap();
        if (nameMap.contains("simpleText")) {
            cc.languageName = nameMap["simpleText"].toString();
        } else if (nameMap.contains("runs")) {
            QVariantList runs = nameMap["runs"].toList();
            QString text;
            for (int r = 0; r < runs.count(); r++) {
                text += runs[r].toMap()["text"].toString();
            }
            cc.languageName = text;
        }

        cc.url = captionMap["baseUrl"].toString() + "&fmt=ttml";

        storageData->captions.append(cc);
    }
}
