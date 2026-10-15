#ifndef INVIDIOUSCLIENT_HPP_
#define INVIDIOUSCLIENT_HPP_

#include "src/invidious/InvidiousInstanceManager.hpp"
#include "src/parser/models/VideoMetadata.hpp"
#include "src/parser/models/StorageData.hpp"

#include <QObject>
#include <QString>
#include <QUrl>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QSslError>

// Alternative to YoutubeClient::parse() for fetching a single video's
// metadata + playable stream URLs, routed through a randomly-selected
// Invidious instance instead of calling YouTube's InnerTube API directly
// from the device.
//
// WHY THIS EXISTS: see the comment above INNERTUBE_PLAYER_CLIENTS in
// YoutubeClient.cpp, and InvidiousInstanceManager.hpp -- in short, YouTube's
// PO-Token/SABR enforcement can leave BB10's direct InnerTube requests with
// no usable adaptive (720p+) stream URL. Invidious instances run their own
// server-side attestation (some via "invidious-companion", a headless
// BotGuard solver) and often have better luck. This is NOT guaranteed --
// individual instances can be just as blocked as we are, which is why this
// class retries across MAX_INSTANCE_ATTEMPTS different random instances
// before giving up.
//
// INTERFACE PARITY WITH YoutubeClient: emits the same
// metadataReceived(VideoMetadata, StorageData) / error(QString) signals as
// YoutubeClient, so callers (BasePage) can swap between the two without
// changing anything downstream (PlayerPage, StorageParser's consumers,
// remux, quality picker, etc. are all untouched).
//
// C++03/GNU++98 only (matches bbtube's QNX/gcc 4.6.3 toolchain).
class InvidiousClient: public QObject
{
Q_OBJECT
public:
    explicit InvidiousClient(InvidiousInstanceManager *instanceManager, QObject *parent = 0);
    virtual ~InvidiousClient()
    {
    }

    // videoId: an 11-character YouTube video ID (use
    // YoutubeClient::getVideoId() to extract one from a URL/search text
    // before calling this -- this class does not do URL parsing itself).
    // exclude: instances not to ask (the retries on failure honour it too).
    void fetchVideo(const QString &videoId, const QStringList &exclude = QStringList());

signals:
    void error(QString message);
    void metadataReceived(VideoMetadata videoMetadata, StorageData storageData);

private slots:
    void onVideoRequestFinished();
    void onSslErrors(const QList<QSslError> &errors);
    void onFetchTimeout();

private:
    // Bumped from 3 -> 5: with YouTube's SABR rollout increasingly leaving
    // the direct-InnerTube fallback (YoutubeClient) with only a 360p
    // progressive stream (no adaptive URLs at all, not even cipher-
    // encoded ones -- nothing to decrypt), a live Invidious instance is
    // often the only way to get 480p/720p/1080p at all. The instance list
    // is refreshed with several candidates (commonly 5-10+), so trying
    // more of them before giving up meaningfully improves the odds of
    // hitting one that isn't down/overloaded, at the cost of a few more
    // seconds of retry time in the worst case (all instances actually
    // are down).
    static const int MAX_INSTANCE_ATTEMPTS = 5;
    QStringList excludedInstances;

    void requestFromInstance(const QString &videoId, const QString &instanceBaseUrl,
            int attemptNumber);

    // Returns true if the parsed response has at least one usable
    // (non-empty url) entry in either formatStreams or adaptiveFormats --
    // i.e. this instance actually gave us something playable, as opposed
    // to a player response with metadata but SABR-blocked/empty streams.
    static bool hasUsableStreams(const QVariantMap &videoMap);

    static void mapToVideoMetadata(const QVariantMap &videoMap, VideoMetadata *outMetadata);
    // instanceUrl: the URL the /api/v1/videos/ request was actually sent
    // to (i.e. reply->url()) -- used to patch up any stream URL in the
    // response that's missing its scheme+host. See the large comment
    // above the definition for why this is needed (an Invidious server
    // bug, not something on our end).
    static void mapToStorageData(const QVariantMap &videoMap, const QUrl &instanceUrl,
            StorageData *outStorageData);
    // Returns `url` unchanged if it already has a scheme (e.g.
    // "http://host/videoplayback?..."); otherwise returns it prefixed
    // with instanceUrl's scheme://host[:port], treating `url` as a
    // path-only URL relative to the instance that returned it (e.g.
    // "/videoplayback?..." -> "http://82.40.56.182:14120/videoplayback?...").
    static QString resolveStreamUrl(const QString &url, const QUrl &instanceUrl);

    InvidiousInstanceManager *instanceManager; // not owned
    // Tracks the in-flight per-instance video request so onFetchTimeout()
    // can abort() it directly, rather than connecting a QTimer straight
    // to the reply's abort() slot -- see the identical comment in
    // InvidiousInstanceManager.hpp for why that approach silently fails
    // on this BB10/Qt4 build ("No such slot QNetworkReplyImpl::abort()").
    QNetworkReply *pendingReply;
};

#endif /* INVIDIOUSCLIENT_HPP_ */
