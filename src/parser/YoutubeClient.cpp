#include "YoutubeClient.hpp"
#include "src/parser/models/VideoMetadata.hpp"
#include "src/parser/models/StorageData.hpp"
#include "src/parser/models/ChannelData.hpp"
#include "src/parser/models/RecommendedData.hpp"
#include "src/parser/models/TrendingData.hpp"

#include "src/applicationui.hpp"
#include "src/auth/GoogleAuthManager.hpp"
#include "src/parser/script/ScriptData.hpp"
#include "src/parser/script/ScriptParser.hpp"
#include "src/parser/cipher/DecryptHelper.hpp"
#include "src/parser/storage/StorageParser.hpp"
#include "src/parser/models/SearchData.hpp"
#include "src/parser/search/ItemRendererParser.hpp"
#include "src/parser/search/SuggestionsParser.hpp"
#include "src/parser/channel/ChannelPageParser.hpp"
#include "src/parser/recommended/RecommendedPageParser.hpp"
#include "src/parser/trending/TrendingPageParser.hpp"
#include "src/settings/TrendingRegions.hpp"

#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkRequest>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkCookie>
#include <QtNetwork/QNetworkCookieJar>
#include <QtCore/QDate>
#include <QtCore/QDateTime>
#include <QDebug>
#include <bb/data/JsonDataAccess>

#include <openssl/sha.h>

QMap<QString, ScriptData> YoutubeClient::cachedScripts;

// Current InnerTube client context (WEB client, up-to-date version)
// UPDATE THIS VERSION periodically - check https://www.youtube.com and search for "INNERTUBE_CLIENT_VERSION"
static const QString INNERTUBE_CLIENT_VERSION = "2.20260708.00.00"; // matches yt-dlp master "web" client, checked 2026-10-15
static const QString INNERTUBE_CLIENT_NAME    = "WEB";
static const QString INNERTUBE_API_URL_BASE   = "https://www.youtube.com/youtubei/v1/";

// Multi-client fallback list for the /player endpoint, tried in order until
// one gives us usable adaptiveFormats (i.e. isn't SABR-blocked). Sourced
// from yt-dlp's youtube/_base.py INNERTUBE_CLIENTS on 2026-08-09.
//
// WHY THIS ORDER: as of 2026, YouTube enforces two independent gates on the
// /player response:
//   1) SABR-only streaming experiments, which strip "url"/"cipher" out of
//      adaptiveFormats entirely for some clients/sessions (see
//      https://github.com/yt-dlp/yt-dlp/issues/12482).
//   2) A "GVS PO Token" requirement, which — separately — can make an
//      included URL 403 without a valid token.
// Per yt-dlp's current client table, "tv" (TVHTML5) and "android_vr" are the
// only two clients with NO explicit GVS_PO_TOKEN_POLICY (i.e. not required
// by default), so they're tried first. "android" is kept last as the
// known-working-for-itag18 fallback this app already used.
//
// IMPORTANT for android_vr: yt-dlp pins clientVersion to exactly "1.65.10"
// with an explicit upstream comment "Using a clientVersion>1.65 may return
// SABR streams only" — do not bump this without re-checking that comment.
//
// None of this is a permanent fix. YouTube's SABR rollout is an active,
// evolving anti-abuse system (yt-dlp itself is still finishing a proper SABR
// downloader as of mid-2026); any of these clients can start returning
// SABR-only responses too with no warning. Re-check yt-dlp's
// yt_dlp/extractor/youtube/_base.py INNERTUBE_CLIENTS table periodically.
struct InnertubeClientConfig {
    const char *label;              // for debug logs only
    const char *clientName;         // InnerTube "clientName"
    const char *clientVersion;
    const char *clientNameId;       // X-YouTube-Client-Name header / INNERTUBE_CONTEXT_CLIENT_NAME
    const char *userAgent;
    const char *extraContextFields; // raw JSON fragment(s), each ending in a comma, injected into the "client" object
    const char *apiKey;
};

static const InnertubeClientConfig INNERTUBE_PLAYER_CLIENTS[] = {
    {
        "tv",
        "TVHTML5",
        "7.20260707.07.00",
        "7",
        "Mozilla/5.0 (ChromiumStylePlatform) Cobalt/25.lts.30.1034943-gold (unlike Gecko), Unknown_TV_Unknown_0/Unknown (Unknown, Unknown)",
        "",
        "AIzaSyDCU8hByM-4DrUqRUYnGn-3llEO78bcxq8" // yt-dlp's shared default InnerTube key
    },
    {
        "android_vr",
        "ANDROID_VR",
        "1.65.10",
        "28",
        "com.google.android.apps.youtube.vr.oculus/1.65.10 (Linux; U; Android 12L; eureka-user Build/SQ3A.220605.009.A1) gzip",
        "\"deviceMake\":\"Oculus\",\"deviceModel\":\"Quest 3\",\"androidSdkVersion\":32,\"osName\":\"Android\",\"osVersion\":\"12L\",",
        "AIzaSyDCU8hByM-4DrUqRUYnGn-3llEO78bcxq8"
    },
    {
        "android",
        "ANDROID",
        "21.26.364",
        "3",
        "com.google.android.youtube/21.26.364 (Linux; U; Android 11) gzip",
        "\"androidSdkVersion\":30,\"osName\":\"Android\",\"osVersion\":\"11\",",
        "AIzaSyA8eiZmM1FaDVjRy-df2KTyQ_vz_yYM39w"
    }
};
static const int INNERTUBE_PLAYER_CLIENTS_COUNT = 3;

// REQUIRED for the WEB-client endpoints below (search/browse/etc): every
// InnerTube request must include a ?key= query parameter matching the
// calling client, or the server replies "400 Bad Request" before even
// looking at the JSON body. This is a long-standing public client key
// embedded in YouTube's own web client (not a secret).
static const QString INNERTUBE_API_KEY_WEB = "AIzaSyAO_FJ2SlqU8Q4STEHLGCilw_Y9_11qcW8";

// YouTube Charts (charts.youtube.com) — replaces the classic Trending page
// (browseId FEtrending), which YouTube shut down in July 2025. Charts runs
// on a completely separate subdomain/InnerTube client from the rest of the
// app, confirmed from that page's actual browser network request (DevTools,
// 2026-08-05): no ?key= query param is used here (unlike the www.youtube.com
// endpoints above); auth instead relies on the request being same-origin to
// charts.youtube.com.
static const QString CHARTS_API_URL_BASE    = "https://charts.youtube.com/youtubei/v1/";
static const QString CHARTS_CLIENT_NAME     = "WEB_MUSIC_ANALYTICS";
static const QString CHARTS_CLIENT_VERSION  = "2.0";
static const QString CHARTS_CLIENT_ID       = "31"; // X-Youtube-Client-Name value

// NOTE: request bodies are built inline via QString::arg() in each method
// below (parse/search/channel/recommended/trending) rather than through a
// shared helper, since each endpoint needs a slightly different context
// shape (e.g. the player endpoint uses the ANDROID client, others use WEB).

QString YoutubeClient::getVideoId(QString text)
{
    QRegExp videoIdRegExp(".*(?:youtu.be\\/|v\\/|u\\/\\w\\/|embed\\/|watch\\?v=|shorts\\/)([^#\\&\\?]*).*");
    videoIdRegExp.indexIn(text);

    if (videoIdRegExp.cap(1) != "") {
        return videoIdRegExp.cap(1);
    }

    QUrl url(text);
    if (url.isValid()) {
        QString videoId = url.queryItemValue("v");
        if (videoId != "" && url.host().contains("youtube.com")) {
            return videoId;
        }
    }

    // Bare video ID (no URL at all) -- this is the form passed in by
    // every call site that plays a video the app already knows about
    // (list item taps in RecommendedPage/TrendingPage/ChannelPage/etc,
    // PlayerPage's next/prev/up-next navigation, playlist playback --
    // all of these pass item->id / videoId directly through
    // BasePage::playVideoByIdOrUrl(), not a full URL). Without this
    // check, none of those matched either pattern above and got
    // silently misrouted to search() instead of actually playing.
    // YouTube video IDs are always exactly 11 characters from
    // [A-Za-z0-9_-] -- this is deliberately strict (exact length, exact
    // character set, matches the WHOLE string) so that ordinary search
    // text doesn't get misinterpreted as a video ID.
    QRegExp bareIdRegExp("^[A-Za-z0-9_-]{11}$");
    if (bareIdRegExp.exactMatch(text)) {
        return text;
    }

    return "";
}

void YoutubeClient::process(QString text)
{
    if (text == "") {
        return;
    }

    QString videoId = getVideoId(text);

    if (videoId != "") {
        parse(videoId);
        return;
    }

    search(text);
}

void YoutubeClient::parse(QString videoId)
{
    // Use InnerTube /player endpoint (POST) instead of fetching the watch page
    // This is far more reliable than scraping HTML and parsing ytInitialData.
    // Starts with the first client in INNERTUBE_PLAYER_CLIENTS; onPlayerApiFinished()
    // automatically retries with the next client if this one looks SABR-blocked
    // or fails outright.
    requestPlayerData(videoId, 0);

    // Also fetch the watch page to get ytInitialData for metadata & related videos
    QNetworkRequest watchRequest = prepareRequest("https://www.youtube.com/watch?v=" + videoId + "&hl=en");
    QNetworkReply *watchReply = ApplicationUI::networkManager->get(watchRequest);
    watchReply->setProperty("videoId", videoId);
    QObject::connect(watchReply, SIGNAL(finished()), this, SLOT(onGetHtmlFinished()));
}

// Computes the "Authorization: SAPISIDHASH ..." value Google's endpoints
// accept as proof of a signed-in session, given the raw SAPISID (or
// __Secure-3PAPISID) cookie value and the Origin the request claims to
// come from. Algorithm (unchanged since Google introduced it, used
// identically by yt-dlp's cookie-auth path and by youtube.com's own web
// client): SHA1("<unix-timestamp> <sapisid> <origin>"), hex-encoded, sent
// as "<unix-timestamp>_<hex-digest>".
//
// NOTE ON SCOPE: SAPISIDHASH is fundamentally a proof that the request
// carries a valid *browser* session (it's derived from a cookie only a
// signed-in browser has) -- it's not tied to any particular declared
// InnerTube "clientName" in the JSON body. Attaching it (+ the Cookie
// header) to the tv/android_vr/android /player calls below is
// low-risk -- if YouTube's server doesn't honor cookie auth for a given
// declared client, it simply ignores the header and behaves exactly as
// it did before this change (anonymous). It is NOT the same as full
// WEB-client OAuth/session emulation, and does not touch signature
// cipher solving (still unimplemented for the current WEB client's
// obfuscated player.js, which is a much larger undertaking involving a
// JS engine -- see yt-dlp's own struggles with this as of 2026).
static QString computeSapisidHashAt(qint64 timestamp, const QString &sapisid, const QString &origin);

static QString computeSapisidHash(const QString &sapisid, const QString &origin)
{
    return computeSapisidHashAt(QDateTime::currentDateTime().toTime_t(), sapisid, origin);
}

static QString computeSapisidHashAt(qint64 timestamp, const QString &sapisid, const QString &origin)
{
    QString toHash = QString("%1 %2 %3").arg(timestamp).arg(sapisid, origin);
    QByteArray toHashUtf8 = toHash.toUtf8();

    unsigned char digest[SHA_DIGEST_LENGTH]; // 20 bytes
    SHA1(reinterpret_cast<const unsigned char*>(toHashUtf8.constData()), toHashUtf8.size(), digest);

    QString hex;
    hex.reserve(SHA_DIGEST_LENGTH * 2);
    static const char hexChars[] = "0123456789abcdef";
    for (int i = 0; i < SHA_DIGEST_LENGTH; i++) {
        hex.append(hexChars[(digest[i] >> 4) & 0xF]);
        hex.append(hexChars[digest[i] & 0xF]);
    }

    return QString("%1_%2").arg(timestamp).arg(hex);
}

// Origin used both in the SAPISIDHASH computation above and in the
// "Origin"/"X-Origin" headers below -- these three must all agree for
// Google's server-side check to accept the hash.
static const QString INNERTUBE_AUTH_ORIGIN = "https://www.youtube.com";

// Issues the /player POST for a given client (see INNERTUBE_PLAYER_CLIENTS).
// The reply is tagged with "videoId" and "clientIndex" so onPlayerApiFinished()
// knows what it's looking at and which client to try next on failure.
QNetworkReply* YoutubeClient::requestPlayerData(const QString &videoId, int clientIndex)
{
    const InnertubeClientConfig &cfg = INNERTUBE_PLAYER_CLIENTS[clientIndex];

    QNetworkRequest request(INNERTUBE_API_URL_BASE + "player?key=" + QString(cfg.apiKey) + "&prettyPrint=false");
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    // NOTE: do NOT call applyInnerTubeHeaders() here — that sets WEB client
    // headers (X-YouTube-Client-Name: 1), which conflicts with the client
    // body below and causes a 400. Headers below must match cfg exactly.
    request.setRawHeader("User-Agent", QByteArray(cfg.userAgent));
    request.setRawHeader("X-YouTube-Client-Name", QByteArray(cfg.clientNameId));
    request.setRawHeader("X-YouTube-Client-Version", QByteArray(cfg.clientVersion));

    // If the user is signed in (Settings -> Google account), attach that
    // session so this /player call is authenticated instead of anonymous.
    // See computeSapisidHash()'s comment above for why this is attempted
    // on every client in the fallback list rather than requiring a
    // dedicated WEB client entry. Both cookieHeader() and sapisid() return
    // "" if not logged in or if decrypting the stored session failed, in
    // which case this whole block is a no-op and the request goes out
    // exactly as it did before (anonymous).
    QString cookieHeader = ApplicationUI::googleAuthManager->cookieHeader();
    QString sapisid = ApplicationUI::googleAuthManager->sapisid();
    if (!cookieHeader.isEmpty() && !sapisid.isEmpty()) {
        request.setRawHeader("Cookie", cookieHeader.toUtf8());
        request.setRawHeader("Authorization",
                ("SAPISIDHASH " + computeSapisidHash(sapisid, INNERTUBE_AUTH_ORIGIN)).toUtf8());
        request.setRawHeader("Origin", INNERTUBE_AUTH_ORIGIN.toUtf8());
        request.setRawHeader("X-Origin", INNERTUBE_AUTH_ORIGIN.toUtf8());
    }

    // "params":"8AEB" (a fixed, unexplained magic value from the original
    // implementation) is intentionally NOT sent — cross-checked against
    // yt-dlp's extractor, none of these clients send a fixed PLAYER_PARAMS.
    QString body = QString(
        "{"
        "\"context\":{"
            "\"client\":{"
                "\"clientName\":\"%1\","
                "\"clientVersion\":\"%2\","
                "%3"
                "\"hl\":\"en\","
                "\"gl\":\"US\""
            "}"
        "},"
        "\"videoId\":\"%4\","
        "\"contentCheckOk\":true,"
        "\"racyCheckOk\":true,"
        "\"playbackContext\":{"
            "\"contentPlaybackContext\":{"
                "\"html5Preference\":\"HTML5_PREF_WANTS\""
            "}"
        "}"
        "}"
    ).arg(cfg.clientName, cfg.clientVersion, cfg.extraContextFields, videoId);

    QNetworkReply *playerReply = ApplicationUI::networkManager->post(request, body.toUtf8());
    playerReply->setProperty("videoId", videoId);
    playerReply->setProperty("clientIndex", clientIndex);
    QObject::connect(playerReply, SIGNAL(finished()), this, SLOT(onPlayerApiFinished()));
    return playerReply;
}

void YoutubeClient::search(QString text)
{
    search(text, "");
}

void YoutubeClient::search(QString text, QString searchParams)
{
    // Use InnerTube /search endpoint (POST) — more stable than scraping HTML results
    QNetworkRequest request(INNERTUBE_API_URL_BASE + "search?key=" + INNERTUBE_API_KEY_WEB + "&prettyPrint=false");
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    applyInnerTubeHeaders(request);

    QString body;
    if (searchParams.isEmpty()) {
        body = QString(
            "{"
            "\"context\":{"
                "\"client\":{"
                    "\"clientName\":\"%1\","
                    "\"clientVersion\":\"%2\","
                    "\"hl\":\"en\",\"gl\":\"US\""
                "}"
            "},"
            "\"query\":\"%3\""
            "}"
        ).arg(INNERTUBE_CLIENT_NAME, INNERTUBE_CLIENT_VERSION, text);
    } else {
        body = QString(
            "{"
            "\"context\":{"
                "\"client\":{"
                    "\"clientName\":\"%1\","
                    "\"clientVersion\":\"%2\","
                    "\"hl\":\"en\",\"gl\":\"US\""
                "}"
            "},"
            "\"query\":\"%3\","
            "\"params\":\"%4\""
            "}"
        ).arg(INNERTUBE_CLIENT_NAME, INNERTUBE_CLIENT_VERSION, text, searchParams);
    }

    QNetworkReply *reply = ApplicationUI::networkManager->post(request, body.toUtf8());
    QObject::connect(reply, SIGNAL(finished()), this, SLOT(onSearchFinished()));
}

void YoutubeClient::suggestions(QString text)
{
    if (text == "") {
        return;
    }

    // Suggestion endpoint unchanged
    QNetworkRequest request = prepareRequest(
        "https://suggestqueries-clients6.youtube.com/complete/search?client=youtube&ds=yt&q="
        + QUrl::toPercentEncoding(text));
    QNetworkReply *reply = ApplicationUI::networkManager->get(request);
    QObject::connect(reply, SIGNAL(finished()), this, SLOT(onSuggestionsFinished()));
}

void YoutubeClient::channel(QString channelId, QString originalChannelId)
{
    // Use InnerTube /browse endpoint (POST) for channel pages
    QNetworkRequest request(INNERTUBE_API_URL_BASE + "browse?key=" + INNERTUBE_API_KEY_WEB + "&prettyPrint=false");
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    applyInnerTubeHeaders(request);

    QString body = QString(
        "{"
        "\"context\":{"
            "\"client\":{"
                "\"clientName\":\"%1\","
                "\"clientVersion\":\"%2\","
                "\"hl\":\"en\",\"gl\":\"US\""
            "}"
        "},"
        "\"browseId\":\"%3\","
        "\"params\":\"EgZ2aWRlb3PyBgQKAjoA\""
        "}"
    ).arg(INNERTUBE_CLIENT_NAME, INNERTUBE_CLIENT_VERSION, channelId);

    QNetworkReply *reply = ApplicationUI::networkManager->post(request, body.toUtf8());
    reply->setProperty("channelId", channelId);
    reply->setProperty("originalChannelId", originalChannelId);
    QObject::connect(reply, SIGNAL(finished()), this, SLOT(onChannelFinished()));
}

void YoutubeClient::channelVideosNextBatch(ChannelPageData *channelData)
{
    QNetworkRequest request(INNERTUBE_API_URL_BASE + "browse?key=" + INNERTUBE_API_KEY_WEB + "&prettyPrint=false");
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    applyInnerTubeHeaders(request);
    request.setRawHeader("X-YouTube-Client-Name", "1");
    request.setRawHeader("X-YouTube-Client-Version", channelData->clientVersion.toUtf8());

    QByteArray data = QString(
        "{\"context\":{\"client\":{\"clientName\":\"%1\",\"clientVersion\":\"%2\"}},"
        "\"continuation\":\"%3\"}"
    ).arg(INNERTUBE_CLIENT_NAME, channelData->clientVersion, channelData->ctoken).toUtf8();

    QNetworkReply *reply = ApplicationUI::networkManager->post(request, data);
    QObject::connect(reply, SIGNAL(finished()), this, SLOT(onChannelVideosNextBatchFinished()));
}

void YoutubeClient::recommended()
{
    qDebug() << "[bbtube][recommended] recommended() called";

    // Use InnerTube /browse?browseId=FEwhat_to_watch for the homepage/recommended feed
    QNetworkRequest request(INNERTUBE_API_URL_BASE + "browse?key=" + INNERTUBE_API_KEY_WEB + "&prettyPrint=false");
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    applyInnerTubeHeaders(request);

    // The home feed is personalized: without the signed-in session YouTube
    // only returns a "feed nudge" (no videos). See applyAuthHeaders().
    bool signedIn = applyAuthHeaders(request);

    QString body = QString(
        "{"
        "\"context\":{"
            "\"client\":{"
                "\"clientName\":\"%1\","
                "\"clientVersion\":\"%2\","
                "\"hl\":\"en\",\"gl\":\"US\""
            "},"
            "\"user\":{\"lockedSafetyMode\":false}"
        "},"
        "\"browseId\":\"FEwhat_to_watch\""
        "}"
    ).arg(INNERTUBE_CLIENT_NAME, INNERTUBE_CLIENT_VERSION);

    QNetworkReply *reply = ApplicationUI::networkManager->post(request, body.toUtf8());
    qDebug() << "[bbtube][recommended] request posted to" << request.url().toString()
             << ", signedIn =" << signedIn;
    QObject::connect(reply, SIGNAL(finished()), this, SLOT(onRecommendedFinished()));
}

void YoutubeClient::recommendedNextBatch(RecommendedData *recommendedData)
{
    QNetworkRequest request(INNERTUBE_API_URL_BASE + "browse?key=" + INNERTUBE_API_KEY_WEB + "&prettyPrint=false");
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    applyInnerTubeHeaders(request);
    applyAuthHeaders(request);
    request.setRawHeader("X-YouTube-Client-Name", "1");
    request.setRawHeader("X-YouTube-Client-Version", recommendedData->clientVersion.toUtf8());

    QByteArray data = QString(
        "{\"context\":{\"client\":{\"clientName\":\"%1\",\"clientVersion\":\"%2\"}},"
        "\"continuation\":\"%3\"}"
    ).arg(INNERTUBE_CLIENT_NAME, recommendedData->clientVersion, recommendedData->ctoken).toUtf8();

    QNetworkReply *reply = ApplicationUI::networkManager->post(request, data);
    QObject::connect(reply, SIGNAL(finished()), this, SLOT(onRecommendedNextBatchFinished()));
}

void YoutubeClient::trending(QString categoryKey)
{
    // As of July 2025, YouTube removed the classic Trending page/browseId
    // (FEtrending) entirely, replacing it with category-specific "Charts"
    // served from a *separate* domain (charts.youtube.com) using a
    // dedicated InnerTube client ("WEB_MUSIC_ANALYTICS", client id 31) --
    // captured directly from that page's real network request. categoryKey
    // is now the chart_params_chart_type value (e.g. "TRENDING_VIDEOS",
    // "PODCAST_SHOWS", "MOVIE_TRAILERS"), defaulting to TRENDING_VIDEOS.
    //
    // The chart is per-country: the region comes from Settings ->
    // Trending Region (see AppSettings::trendingRegion()).
    QString chartType = categoryKey.isEmpty() ? "TRENDING_VIDEOS" : categoryKey;
    QString region = ApplicationUI::appSettings->trendingRegion();

    requestTrending(chartType, region, false);
}

// isFallback marks the one automatic retry against the US chart that
// onTrendingFinished() makes if a region's request is rejected or comes back
// empty, so a bad region can't loop.
void YoutubeClient::requestTrending(const QString &chartType, const QString &region, bool isFallback)
{
    qDebug() << "[bbtube][trending] requestTrending() chartType =" << chartType
             << ", region =" << region << ", isFallback =" << isFallback;

    QNetworkRequest request(CHARTS_API_URL_BASE + "browse?alt=json");
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    applyChartsHeaders(request, region);

    QString body = QString(
        "{"
        "\"context\":{"
            "\"capabilities\":{},"
            "\"client\":{"
                "\"clientName\":\"%1\","
                "\"clientVersion\":\"%2\","
                "\"hl\":\"en\",\"gl\":\"%4\","
                "\"experimentIds\":[],\"experimentsToken\":\"\","
                "\"theme\":\"MUSIC\""
            "},"
            "\"request\":{\"internalExperimentFlags\":[]}"
        "},"
        "\"browseId\":\"FEmusic_analytics_charts_home\","
        "\"query\":\"flags=MusicCharts__enable_apac_and_shorts_charts_expansion"
            "&perspective=CHART_DETAILS"
            "&chart_params_country_code=%5"
            "&chart_params_chart_type=%3\""
        "}"
    ).arg(CHARTS_CLIENT_NAME, CHARTS_CLIENT_VERSION, chartType, region.toUpper(), region);

    qDebug() << "[bbtube][trending] request body =" << body;

    QNetworkReply *reply = ApplicationUI::networkManager->post(request, body.toUtf8());
    reply->setProperty("chartType", chartType);
    reply->setProperty("region", region);
    reply->setProperty("isFallback", isFallback);
    QObject::connect(reply, SIGNAL(finished()), this, SLOT(onTrendingFinished()));
}

// ─── Slot: handle /player API response (stream URLs) ─────────────────────────
void YoutubeClient::onPlayerApiFinished()
{
    QNetworkReply *reply = static_cast<QNetworkReply*>(QObject::sender());
    QString videoId = reply->property("videoId").toString();
    int clientIndex = reply->property("clientIndex").toInt();
    QString httpErrorMessage;

    if (reply->error() || hasHttpError(reply, &httpErrorMessage)) {
        // This client failed outright (network error, or e.g. a 400 from a
        // stale clientVersion). Try the next client before giving up.
        if (clientIndex + 1 < INNERTUBE_PLAYER_CLIENTS_COUNT) {
#ifdef QT_DEBUG
            qDebug() << "[bbtube][player] client" << INNERTUBE_PLAYER_CLIENTS[clientIndex].label
                     << "request failed (" << (reply->error() ? reply->errorString() : httpErrorMessage)
                     << ") - trying" << INNERTUBE_PLAYER_CLIENTS[clientIndex + 1].label;
#endif
            requestPlayerData(videoId, clientIndex + 1);
            reply->deleteLater();
            return;
        }
        emit error(reply->error() ? reply->errorString() : httpErrorMessage);
        // Discard any metadata that arrived for this videoId — it would
        // otherwise sit in pendingVideoMetadata forever since
        // tryEmitMetadata() requires BOTH halves to be present.
        pendingVideoMetadata.remove(videoId);
        reply->deleteLater();
        return;
    }

    QString response = QString(reply->readAll());
    StorageData storageData;
    StorageParser::parseFromJson(&storageData, &response);

#ifdef QT_DEBUG
    // TEMP DIAGNOSTIC: dump the parts of the raw /player response we
    // actually need, instead of the first N characters (which is all
    // "responseContext"/visitorData base64 noise and never reaches
    // playabilityStatus or streamingData).
    {
        int psIdx = response.indexOf("\"playabilityStatus\"");
        int sdIdx = response.indexOf("\"streamingData\"");
        qDebug() << "=== /player diagnostic for videoId" << videoId
                  << "client" << INNERTUBE_PLAYER_CLIENTS[clientIndex].label << "===";
        qDebug() << "playabilityStatus snippet:"
                  << (psIdx >= 0 ? response.mid(psIdx, 400) : "NOT FOUND");
        qDebug() << "streamingData snippet:"
                  << (sdIdx >= 0 ? response.mid(sdIdx, 600) : "NOT FOUND");
        qDebug() << "=== instances:" << storageData.instances.count()
                  << "audio empty:" << storageData.audio.url.isEmpty()
                  << storageData.audio.cipher.isEmpty() << "===";

        // List every itag/qualityLabel/mimeType available in this response
        // (both progressive "formats" and "adaptiveFormats"), so we can see
        // exactly which resolutions YouTube is offering for this specific
        // video without re-parsing the whole JSON by hand.
        bb::data::JsonDataAccess diagJda;
        QVariantMap diagMap = diagJda.loadFromBuffer(response).toMap();
        QVariantMap diagStreamingData = diagMap["streamingData"].toMap();

        QVariantList diagFormats = diagStreamingData["formats"].toList();
        qDebug() << "[bbtube][player] formats (progressive) count =" << diagFormats.count();
        for (int i = 0; i < diagFormats.count(); i++) {
            QVariantMap f = diagFormats[i].toMap();
            qDebug() << "[bbtube][player]  formats[" << i << "] itag =" << f["itag"].toInt()
                     << ", qualityLabel =" << f["qualityLabel"].toString()
                     << ", mimeType =" << f["mimeType"].toString();
        }

        QVariantList diagAdaptive = diagStreamingData["adaptiveFormats"].toList();
        qDebug() << "[bbtube][player] adaptiveFormats count =" << diagAdaptive.count();
        for (int i = 0; i < diagAdaptive.count(); i++) {
            QVariantMap f = diagAdaptive[i].toMap();
            qDebug() << "[bbtube][player]  adaptiveFormats[" << i << "] itag =" << f["itag"].toInt()
                     << ", qualityLabel =" << f["qualityLabel"].toString()
                     << ", mimeType =" << f["mimeType"].toString();
        }

        // SABR check: dump every key present on the first AUDIO entry. If
        // "url" / "signatureCipher" / "cipher" are all absent here, YouTube
        // is not returning direct playback URLs at all for this client
        // (forced SABR streaming) -- no amount of JSON parsing fixes that;
        // it needs either a different client context or a PO Token/SABR
        // downloader implementation. See:
        // https://github.com/yt-dlp/yt-dlp/issues/12482
        for (int i = 0; i < diagAdaptive.count(); i++) {
            QVariantMap f = diagAdaptive[i].toMap();
            if (f["mimeType"].toString().startsWith("audio/")) {
                qDebug() << "[bbtube][player][SABR-check] keys on first audio format:"
                         << f.keys();
                qDebug() << "[bbtube][player][SABR-check] has url =" << f.contains("url")
                         << ", has signatureCipher =" << f.contains("signatureCipher")
                         << ", has cipher =" << f.contains("cipher");
                break;
            }
        }
    }
#endif

    // Does this client's response give us anything beyond the single
    // progressive itag18 stream (i.e. any usable adaptive video and/or
    // audio URL/cipher)? If not, and there's another client left in
    // INNERTUBE_PLAYER_CLIENTS, retry with it instead of settling for
    // 360p-only — see the big comment above INNERTUBE_PLAYER_CLIENTS.
    bool audioUsable = !storageData.audio.url.isEmpty() || !storageData.audio.cipher.isEmpty();
    bool looksSabrBlocked = storageData.instances.count() <= 1 && !audioUsable;

    if (looksSabrBlocked && clientIndex + 1 < INNERTUBE_PLAYER_CLIENTS_COUNT) {
#ifdef QT_DEBUG
        qDebug() << "[bbtube][player] client" << INNERTUBE_PLAYER_CLIENTS[clientIndex].label
                 << "looks SABR-blocked (no usable adaptive URL/cipher) - trying"
                 << INNERTUBE_PLAYER_CLIENTS[clientIndex + 1].label;
#endif
        requestPlayerData(videoId, clientIndex + 1);
        reply->deleteLater();
        return;
    }

    // Decrypt any cipher-protected URLs.
    // NOTE: this check is intentionally NOT nested inside an
    // "instances.count() > 0" guard — a video can have zero progressive/
    // video-only instances but still have an audio-only cipher that needs
    // decrypting (e.g. some audio-only or restricted-format responses).
    bool needsJs = false;
    for (int i = 0; i < storageData.instances.count(); i++) {
        if (!storageData.instances[i].cipher.isEmpty()) {
            needsJs = true;
            break;
        }
    }
    if (!needsJs && !storageData.audio.cipher.isEmpty()) {
        needsJs = true;
    }

    if (needsJs) {
        // Fetch base.js to get cipher operations.
        // We fall back to fetching the watch page to get the script URL.
        QEventLoop loop;
        QNetworkRequest watchReq = prepareRequest("https://www.youtube.com/watch?v=" + videoId + "&hl=en");
        QNetworkReply *watchReply = ApplicationUI::networkManager->get(watchReq);
        QObject::connect(watchReply, SIGNAL(finished()), &loop, SLOT(quit()));
        loop.exec();

        QString watchHtml = QString(watchReply->readAll());
        watchReply->deleteLater();

        QString baseJsUrl = extractBaseJsUrl(watchHtml);
        if (!baseJsUrl.isEmpty() && !cachedScripts.contains(baseJsUrl)) {
            QNetworkReply *jsReply = ApplicationUI::networkManager->get(QNetworkRequest(baseJsUrl));
            QObject::connect(jsReply, SIGNAL(finished()), &loop, SLOT(quit()));
            loop.exec();
            onGetBaseJsFinished(jsReply);
        }

        if (!baseJsUrl.isEmpty() && cachedScripts.contains(baseJsUrl)) {
            ScriptData scriptData = cachedScripts[baseJsUrl];
            for (int i = 0; i < storageData.instances.count(); i++) {
                if (!storageData.instances[i].cipher.isEmpty()) {
                    storageData.instances[i].url = DecryptHelper::decryptUrl(
                        storageData.instances[i].cipher, &scriptData);
                }
            }
            if (!storageData.audio.cipher.isEmpty()) {
                storageData.audio.url = DecryptHelper::decryptUrl(
                    storageData.audio.cipher, &scriptData);
            }
        }
    }

    // Cache the storageData — the watch page handler will merge metadata + storageData
    pendingStorageData[videoId] = storageData;
    tryEmitMetadata(videoId);

    reply->deleteLater();
}

// ─── Slot: handle watch page HTML response (metadata & related videos) ────────
void YoutubeClient::onGetHtmlFinished()
{
    QNetworkReply *reply = static_cast<QNetworkReply*>(QObject::sender());
    QString requestedVideoId = reply->property("videoId").toString();

    QString httpErrorMessage;
    if (reply->error() || hasHttpError(reply, &httpErrorMessage)) {
        // The watch-page GET itself failed at the transport/HTTP level
        // (e.g. YouTube's consent/bot-check wall responding with a non-2xx
        // status). This only affects display metadata (title, related
        // videos) — it says nothing about whether the InnerTube /player
        // call succeeded. Don't discard a stream URL we may already have;
        // fall back to minimal metadata instead, same as the empty-parse
        // case below, so playback isn't blocked by a metadata-only failure.
        VideoMetadata fallbackMetadata;
        fallbackMetadata.video.videoId = requestedVideoId;
        pendingVideoMetadata[requestedVideoId] = fallbackMetadata;
        tryEmitMetadata(requestedVideoId);
        reply->deleteLater();
        return;
    }

    QString response = QString(reply->readAll());
    QString json = getJson(response);

    VideoMetadata videoMetadata;
    ItemRendererParser::populateVideoMetadata(&videoMetadata, &json);

    if (videoMetadata.video.videoId.isEmpty()) {
        // The watch-page HTML scrape failed to find ytInitialData (YouTube
        // served a stripped/consent/bot-check page instead of the full
        // watch page — this is independent of whether the InnerTube
        // /player call succeeded). Previously this dropped the whole
        // video, including a perfectly good stream URL already sitting in
        // pendingStorageData, and reported "Source unavailable" even
        // though playback would have worked fine.
        //
        // Instead: fall back to a minimal metadata object using the
        // videoId we requested with, so playback can still proceed. The
        // user just loses the nicer title/related-videos data for this
        // load; the video itself still plays.
        videoMetadata.video.videoId = requestedVideoId;
    }

    pendingVideoMetadata[videoMetadata.video.videoId] = videoMetadata;
    tryEmitMetadata(videoMetadata.video.videoId);

    reply->deleteLater();
}

// Emit metadataReceived only when BOTH player API response and watch page are ready
void YoutubeClient::tryEmitMetadata(const QString &videoId)
{
    if (pendingVideoMetadata.contains(videoId) && pendingStorageData.contains(videoId)) {
        VideoMetadata vm = pendingVideoMetadata.take(videoId);
        StorageData sd   = pendingStorageData.take(videoId);
        emit metadataReceived(vm, sd);
    }
}

// ─── Slot: handle InnerTube /search response ─────────────────────────────────
void YoutubeClient::onSearchFinished()
{
    QNetworkReply *reply = static_cast<QNetworkReply*>(QObject::sender());

    QString httpErrorMessage;
    if (reply->error() || hasHttpError(reply, &httpErrorMessage)) {
        emit error(reply->error() ? reply->errorString() : httpErrorMessage);
        reply->deleteLater();
        return;
    }

    QString response = QString(reply->readAll());
    SearchData searchData;
    ItemRendererParser::populateSearchData(&searchData, &response);
    emit searchDataReceived(searchData);

    reply->deleteLater();
}

void YoutubeClient::onSuggestionsFinished()
{
    QNetworkReply *reply = static_cast<QNetworkReply*>(QObject::sender());

    if (reply->error() || hasHttpError(reply, NULL)) {
        reply->deleteLater();
        return;
    }

    QString response = QString(reply->readAll());
    // YouTube suggestions endpoint returns: window.google.ac.h([...])
    // or just a raw JSON array depending on the client param
    QStringList list;
    QString configKey = "window.google.ac.h(";
    int startOfConfig = response.indexOf(configKey);
    if (startOfConfig >= 0) {
        QString json = response.mid(startOfConfig + configKey.length(),
            response.length() - startOfConfig - configKey.length() - 1).trimmed();
        list = SuggestionsParser::parseSuggestions(&json);
    } else {
        // Try parsing as raw JSON array
        list = SuggestionsParser::parseSuggestions(&response);
    }

    emit suggestionsReceived(list);
    reply->deleteLater();
}

void YoutubeClient::onGetBaseJsFinished(QNetworkReply *reply)
{
    QString httpErrorMessage;
    if (reply->error() || hasHttpError(reply, &httpErrorMessage)) {
        emit error(reply->error() ? reply->errorString() : httpErrorMessage);
        reply->deleteLater();
        return;
    }

    QString response = QString(reply->readAll());
    ScriptData scriptData = ScriptParser::parse(response);
    cachedScripts.insert(reply->request().url().toString(), scriptData);
    reply->deleteLater();
}

void YoutubeClient::onChannelFinished()
{
    QNetworkReply *reply = static_cast<QNetworkReply*>(QObject::sender());

    qDebug() << "[bbtube][channel] onChannelFinished, HTTP status ="
             << reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

    QString httpErrorMessage;
    if (reply->error() || hasHttpError(reply, &httpErrorMessage)) {
        qDebug() << "[bbtube][channel] request-level error:"
                  << (reply->error() ? reply->errorString() : httpErrorMessage);
        emit error(reply->error() ? reply->errorString() : httpErrorMessage);
        reply->deleteLater();
        return;
    }

    QString response = QString(reply->readAll());

    qDebug() << "[bbtube][channel] response length =" << response.length();
    qDebug() << "[bbtube][channel] response head =" << response.left(500);

    ChannelPageData channelData;
    // clientVersion now extracted from the InnerTube response itself
    channelData.clientVersion = INNERTUBE_CLIENT_VERSION;

    if (reply->property("originalChannelId").toString() != "") {
        channelData.channelId = reply->property("originalChannelId").toString();
    } else {
        channelData.channelId = reply->property("channelId").toString();
    }

    ChannelPageParser::parse(&channelData, &response);

    qDebug() << "[bbtube][channel] parsed title =" << channelData.title
             << ", redirectChannelId =" << channelData.redirectChannelId
             << ", videos.count() =" << channelData.videos.count()
             << ", ctoken empty =" << channelData.ctoken.isEmpty();

    if (!channelData.redirectChannelId.isEmpty()) {
        qDebug() << "[bbtube][channel] following redirect to" << channelData.redirectChannelId;
        channel(channelData.redirectChannelId, channelData.channelId);
        reply->deleteLater();
        return;
    }

    if (!channelData.title.isEmpty()) {
        qDebug() << "[bbtube][channel] EMITTING channelDataReceived, videos.count() ="
                 << channelData.videos.count() << " (BUILD MARKER v2)";
        emit channelDataReceived(channelData);
    } else {
        qDebug() << "[bbtube][channel] title empty -> emitting 'Channel not found'";
        emit error("Channel not found");
    }

    reply->deleteLater();
}

void YoutubeClient::onChannelVideosNextBatchFinished()
{
    QNetworkReply *reply = static_cast<QNetworkReply*>(QObject::sender());
    QString httpErrorMessage;
    if (reply->error() || hasHttpError(reply, &httpErrorMessage)) {
        emit error(reply->error() ? reply->errorString() : httpErrorMessage);
        reply->deleteLater();
        return;
    }

    QString response = QString(reply->readAll());
    ChannelPageData channelData;
    ChannelPageParser::parseNextBatch(&channelData, &response);
    emit channelVideosNextBatchReceived(channelData);
    reply->deleteLater();
}

void YoutubeClient::onRecommendedFinished()
{
    QNetworkReply *reply = static_cast<QNetworkReply*>(QObject::sender());

    qDebug() << "[bbtube][recommended] onRecommendedFinished, HTTP status ="
             << reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

    QString httpErrorMessage;
    if (reply->error() || hasHttpError(reply, &httpErrorMessage)) {
        qDebug() << "[bbtube][recommended] error:"
                 << (reply->error() ? reply->errorString() : httpErrorMessage);
        emit error(reply->error() ? reply->errorString() : httpErrorMessage);
        reply->deleteLater();
        return;
    }

    QString response = QString(reply->readAll());
    qDebug() << "[bbtube][recommended] response length =" << response.length();

    RecommendedData recommendedData;
    recommendedData.clientVersion = INNERTUBE_CLIENT_VERSION;
    RecommendedPageParser::parse(&recommendedData, &response);

    qDebug() << "[bbtube][recommended] parsed videos.count() =" << recommendedData.videos.count()
             << ", ctoken empty =" << recommendedData.ctoken.isEmpty();

    emit recommendedDataReceived(recommendedData);
    reply->deleteLater();
}

void YoutubeClient::onRecommendedNextBatchFinished()
{
    QNetworkReply *reply = static_cast<QNetworkReply*>(QObject::sender());
    QString httpErrorMessage;
    if (reply->error() || hasHttpError(reply, &httpErrorMessage)) {
        emit error(reply->error() ? reply->errorString() : httpErrorMessage);
        reply->deleteLater();
        return;
    }

    QString response = QString(reply->readAll());
    RecommendedData recommendedData;
    RecommendedPageParser::parseNextBatch(&recommendedData, &response);
    emit recommendedNextBatchReceived(recommendedData);
    reply->deleteLater();
}

void YoutubeClient::onTrendingFinished()
{
    QNetworkReply *reply = static_cast<QNetworkReply*>(QObject::sender());

    QString chartType = reply->property("chartType").toString();
    QString region = reply->property("region").toString();
    bool isFallback = reply->property("isFallback").toBool();
    bool canFallBack = !isFallback && region != TrendingRegions::fallbackCode();

    qDebug() << "[bbtube][trending] onTrendingFinished, region =" << region
             << ", HTTP status ="
             << reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

    QString httpErrorMessage;
    if (reply->error() || hasHttpError(reply, &httpErrorMessage)) {
        // Read the body even on error -- YouTube usually returns a JSON
        // error payload (error.code / error.message / error.status)
        // explaining *why* the request was rejected, which is far more
        // useful than the generic "Bad Request" Qt/HTTP reason phrase.
        QString errorBody = QString(reply->readAll());
        qDebug() << "[bbtube][trending] error:"
                 << (reply->error() ? reply->errorString() : httpErrorMessage);
        qDebug() << "[bbtube][trending] error response body =" << errorBody.left(2000);

        if (canFallBack) {
            reply->deleteLater();
            requestTrending(chartType, TrendingRegions::fallbackCode(), true);
            return;
        }

        emit error(reply->error() ? reply->errorString() : httpErrorMessage);
        reply->deleteLater();
        return;
    }

    QString response = QString(reply->readAll());
    qDebug() << "[bbtube][trending] response length =" << response.length();

    TrendingData trendingData;
    trendingData.region = region;
    TrendingPageParser::parse(&trendingData, &response);

    qDebug() << "[bbtube][trending] parsed videos.count() =" << trendingData.videos.count();

    if (trendingData.videos.isEmpty() && canFallBack) {
        // Region accepted but nothing to show (e.g. no Trending chart there).
        reply->deleteLater();
        requestTrending(chartType, TrendingRegions::fallbackCode(), true);
        return;
    }

    emit trendingDataReceived(trendingData);
    reply->deleteLater();
}

// ─── Helpers ──────────────────────────────────────────────────────────────────

QNetworkRequest YoutubeClient::prepareRequest(QString url)
{
    QNetworkRequest request(url);
    // Updated User-Agent — old Firefox 74 UA was getting blocked
    request.setRawHeader("User-Agent",
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:120.0) Gecko/20100101 Firefox/120.0");
    request.setRawHeader("Accept-Language", "en-US,en;q=0.9");
    request.setRawHeader("Accept", "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8");

    // Handle CONSENT cookie (YouTube GDPR consent) — updated cookie format
    QNetworkCookieJar *cookieJar = ApplicationUI::networkManager->cookieJar();
    QList<QNetworkCookie> cookies = cookieJar->cookiesForUrl(url);
    bool consentExists = false;

    for (int i = 0; i < cookies.count(); i++) {
        if (QString::fromUtf8(cookies[i].name()) == "CONSENT" ||
            QString::fromUtf8(cookies[i].name()) == "SOCS") {
            consentExists = true;
        }
    }

    if (!consentExists) {
        QList<QNetworkCookie> newCookies;
        // New SOCS cookie replaces old CONSENT cookie since ~2023
        newCookies.append(QNetworkCookie("SOCS", "CAESEwgDEgk0NTk4MjI4NTIaAmVuIAEaBgiA_LysBg"));
        newCookies.append(QNetworkCookie("CONSENT",
            QString("YES+cb.%1-17-p0.en+FX+%2")
            .arg(QDate::currentDate().toString("yyyyMMdd"), "667").toUtf8()));
        cookieJar->setCookiesFromUrl(newCookies, url);
    }

    return request;
}

void YoutubeClient::applyInnerTubeHeaders(QNetworkRequest &request)
{
    request.setRawHeader("User-Agent",
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:120.0) Gecko/20100101 Firefox/120.0");
    request.setRawHeader("X-YouTube-Client-Name", "1");
    request.setRawHeader("X-YouTube-Client-Version", INNERTUBE_CLIENT_VERSION.toUtf8());
    request.setRawHeader("Origin", "https://www.youtube.com");
    request.setRawHeader("Referer", "https://www.youtube.com/");
}

// Attaches the signed-in Google/YouTube session to a WEB-client InnerTube
// request (browse, etc). Returns false -- leaving the request untouched --
// if the user isn't logged in, so callers fall back to anonymous.
//
// Mirrors what yt-dlp does for cookie auth (extractor/youtube/_base.py):
//  - Cookie header with the stored session cookies
//  - Authorization: SAPISIDHASH + SAPISID1PHASH + SAPISID3PHASH, each
//    SHA1("<ts> <cookie> <origin>"), sharing one timestamp
//  - Origin / X-Origin / X-Goog-AuthUser (first signed-in account)
//
// Qt detail: with the default CookieLoadControl (Automatic), QNetworkAccessManager
// replaces any Cookie header we set with whatever is in its own cookie jar
// for the URL -- and prepareRequest() puts SOCS/CONSENT consent cookies in
// that jar for youtube.com, so the jar is never empty. Without switching to
// Manual the session cookies below would silently never be sent.
bool YoutubeClient::applyAuthHeaders(QNetworkRequest &request)
{
    GoogleAuthManager *auth = ApplicationUI::googleAuthManager;

    QString cookieHeader = auth->cookieHeader();
    if (cookieHeader.isEmpty()) {
        return false;
    }

    QString sapisid = auth->cookieValue("SAPISID");
    QString sapisid1p = auth->cookieValue("__Secure-1PAPISID");
    QString sapisid3p = auth->cookieValue("__Secure-3PAPISID");
    if (sapisid.isEmpty()) {
        sapisid = sapisid3p; // YouTube itself falls back to 3PAPISID
    }
    if (sapisid.isEmpty() && sapisid1p.isEmpty() && sapisid3p.isEmpty()) {
        return false;
    }

    // Keep the consent cookie alongside the session so a signed-in request
    // doesn't trigger the GDPR consent interstitial.
    if (!cookieHeader.contains("SOCS=")) {
        cookieHeader += "; SOCS=CAESEwgDEgk0NTk4MjI4NTIaAmVuIAEaBgiA_LysBg";
    }

    qint64 timestamp = QDateTime::currentDateTime().toTime_t();
    QStringList authorizations;
    if (!sapisid.isEmpty()) {
        authorizations << "SAPISIDHASH " + computeSapisidHashAt(timestamp, sapisid, INNERTUBE_AUTH_ORIGIN);
    }
    if (!sapisid1p.isEmpty()) {
        authorizations << "SAPISID1PHASH " + computeSapisidHashAt(timestamp, sapisid1p, INNERTUBE_AUTH_ORIGIN);
    }
    if (!sapisid3p.isEmpty()) {
        authorizations << "SAPISID3PHASH " + computeSapisidHashAt(timestamp, sapisid3p, INNERTUBE_AUTH_ORIGIN);
    }

    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    request.setRawHeader("Cookie", cookieHeader.toUtf8());
    request.setRawHeader("Authorization", authorizations.join(" ").toUtf8());
    request.setRawHeader("Origin", INNERTUBE_AUTH_ORIGIN.toUtf8());
    request.setRawHeader("X-Origin", INNERTUBE_AUTH_ORIGIN.toUtf8());
    request.setRawHeader("X-Goog-AuthUser", "0");

    return true;
}

// Headers for charts.youtube.com requests — this is a different origin
// from www.youtube.com, with its own client id/name, so it cannot reuse
// applyInnerTubeHeaders() (which hardcodes the www.youtube.com origin and
// the WEB client id "1"). Captured from the real browser request.
void YoutubeClient::applyChartsHeaders(QNetworkRequest &request, const QString &region)
{
    request.setRawHeader("User-Agent",
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:120.0) Gecko/20100101 Firefox/120.0");
    request.setRawHeader("X-YouTube-Client-Name", CHARTS_CLIENT_ID.toUtf8());
    request.setRawHeader("X-YouTube-Client-Version", CHARTS_CLIENT_VERSION.toUtf8());
    request.setRawHeader("Origin", "https://charts.youtube.com");
    request.setRawHeader("Referer",
            ("https://charts.youtube.com/charts/TrendingVideos/" + region).toUtf8());
}

// QNetworkReply::error() only flags network-level failures (DNS, timeout,
// connection refused, etc). An HTTP 400/403/404/500 response is still a
// "successfully completed" transfer as far as Qt is concerned — the
// error lives in the HTTP status code, which we have to check separately.
// Without this, a request rejected by the server (e.g. missing/invalid API
// key) would silently fall through to JSON parsing of an error payload,
// producing an empty result with no visible error (exactly what happened
// with the blank "Recommended" tab).
bool YoutubeClient::hasHttpError(QNetworkReply *reply, QString *errorMessage)
{
    QVariant statusCodeVariant = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
    if (!statusCodeVariant.isValid()) {
        return false; // no HTTP status available (e.g. pure network failure, handled separately)
    }

    int statusCode = statusCodeVariant.toInt();
    if (statusCode >= 200 && statusCode < 300) {
        return false;
    }

    if (errorMessage) {
        QString reasonPhrase =
            reply->attribute(QNetworkRequest::HttpReasonPhraseAttribute).toString();
        *errorMessage = QString("Server error %1 %2").arg(QString::number(statusCode), reasonPhrase);
    }
    return true;
}

// Extract the base.js URL from a watch page HTML response
QString YoutubeClient::extractBaseJsUrl(const QString &html)
{
    // Modern YouTube embeds the player script URL like:
    // "jsUrl":"/s/player/HASH/player_ias.vflset/en_US/base.js"
    QString key = "\"jsUrl\":\"";
    int idx = html.indexOf(key);
    if (idx >= 0) {
        int start = idx + key.length();
        int end   = html.indexOf('"', start);
        QString path = html.mid(start, end - start);
        if (path.startsWith("/")) {
            return "https://www.youtube.com" + path;
        }
        return path;
    }

    // Fallback: old format src="/s/player/..."
    int scriptIndex = html.indexOf("src=\"/s/player/");
    if (scriptIndex >= 0) {
        int jsUrlStart = scriptIndex + 5;
        int jsUrlEnd   = html.indexOf('"', jsUrlStart);
        return "https://www.youtube.com" + html.mid(jsUrlStart, jsUrlEnd - jsUrlStart);
    }

    return "";
}

// Extract ytInitialData JSON from HTML page (used for metadata/related videos)
QString YoutubeClient::getJson(QString response)
{
    QString json;

    // Modern YouTube uses:  var ytInitialData = {...};
    // The JSON is NOT terminated by "};" reliably (deeply nested). We do a balanced-brace scan.
    // NOTE: brace-init-list ({"a", "b"}) is C++11 syntax and is NOT supported by
    // the BB10 NDK toolchain (GCC 4.6.3 / C++98) — use append() instead.
    QStringList keys;
    keys.append("var ytInitialData = ");
    keys.append("window[\"ytInitialData\"] = ");

    for (int k = 0; k < keys.size(); k++) {
        int startIdx = response.indexOf(keys[k]);
        if (startIdx < 0) continue;

        int jsonStart = startIdx + keys[k].length();
        if (jsonStart >= response.length() || response[jsonStart] != '{') continue;

        // Balanced brace scan
        int depth = 0;
        bool inStr = false;
        bool escape = false;
        int jsonEnd = jsonStart;

        for (int i = jsonStart; i < response.length(); i++) {
            QChar c = response[i];
            if (escape) { escape = false; continue; }
            if (c == '\\' && inStr) { escape = true; continue; }
            if (c == '"') { inStr = !inStr; continue; }
            if (!inStr) {
                if (c == '{') depth++;
                else if (c == '}') {
                    depth--;
                    if (depth == 0) { jsonEnd = i; break; }
                }
            }
        }

        if (jsonEnd > jsonStart) {
            json = response.mid(jsonStart, jsonEnd - jsonStart + 1);
            return json;
        }
    }

    return json;
}

QString YoutubeClient::getApiKey(QString response)
{
    // Extract INNERTUBE_API_KEY from HTML (still present in watch pages)
    QString innertubeKey = "\"INNERTUBE_API_KEY\":\"";
    int i = response.indexOf(innertubeKey);
    if (i < 0) {
        // Fallback key name used in some responses
        innertubeKey = "\"innertubeApiKey\":\"";
        i = response.indexOf(innertubeKey);
    }
    if (i < 0) return "";
    int j = response.indexOf("\"", i + innertubeKey.length());
    return response.mid(i + innertubeKey.length(), j - i - innertubeKey.length());
}
