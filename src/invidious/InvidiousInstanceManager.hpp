#ifndef INVIDIOUSINSTANCEMANAGER_HPP_
#define INVIDIOUSINSTANCEMANAGER_HPP_

#include <QObject>
#include <QString>
#include <QStringList>
#include <QMap>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QSslError>

// Maintains bbtube's pool of Invidious instance base URLs (e.g.
// "http://76.82.152.76:3000"), modeled on notPipe's (gohoski/notPipe)
// multi-instance strategy: rather than hardcoding one instance -- which
// dies the moment it gets enough traffic for YouTube to notice and block
// it -- we fetch a community-maintained list and pick a new random
// instance for each request. This spreads load and avoids a single point
// of failure.
//
// SOURCE: notPipe's own auto-updating instance list at
// http://144.31.189.129/notPipe.json (see notPipe's README, "Automatic
// updates of the instances"), specifically its "invidious" key -- a
// list of independently-run Invidious instances. This was switched to
// from api.invidious.io (the "official" Invidious instance directory)
// because that domain requires HTTPS and consistently fails SSL
// handshake on this device (QNetworkReply::SslHandshakeFailedError,
// even with the correct CA already present in BB10's trust store -- the
// underlying cause was never fully pinned down, but is most likely an
// SNI or cipher-suite incompatibility between this old OpenSSL build and
// that server's TLS config). notPipe.json's "invidious" entries are
// plain HTTP, which sidesteps the whole problem, at the cost of the
// instance list traffic itself being unencrypted (the video
// URLs/metadata fetched FROM a chosen instance are a separate, later
// concern -- see InvidiousClient).
//
// notPipe.json's other keys ("yt2009", "piped", "ytapilegacy") are
// deliberately NOT used here: yt2009 is an HTML/Flash frontend, not a
// JSON API, and would need an entirely different (HTML-scraping)
// client; piped and ytapilegacy have their own, different JSON schemas
// that InvidiousClient's parser (written for Invidious's
// /api/v1/videos/ format) does not understand.
//
// IMPORTANT CAVEAT (do not remove this comment without re-reading it):
// Invidious instances are not immune to the same PO-Token/SABR problems
// bbtube's direct InnerTube client hits -- see the large comment above
// INNERTUBE_PLAYER_CLIENTS in YoutubeClient.cpp for the full background.
// Some instances run "invidious-companion" (a server-side BotGuard
// solver) and have a much better success rate than bbtube calling
// YouTube directly, but there is no guarantee any given instance, at any
// given moment, can actually produce a playable URL for a given video.
// InvidiousClient (separate file) is expected to try more than one
// instance per video if the first one comes back empty.
//
// C++03/GNU++98 only (matches bbtube's QNX/gcc 4.6.3 toolchain).
class InvidiousInstanceManager: public QObject
{
Q_OBJECT
public:
    explicit InvidiousInstanceManager(QObject *parent = 0);
    virtual ~InvidiousInstanceManager()
    {
    }

    // Kicks off a background refresh of the instance list from
    // notPipe.json. Safe to call multiple times; a refresh already in
    // flight is not duplicated. Not required before calling
    // pickRandomInstance() -- see below.
    void refreshInstanceList();

    // Returns a randomly chosen instance base URL with no trailing slash
    // (e.g. "http://76.82.152.76:3000"), or "" if no instance list is
    // available yet (refreshInstanceList() hasn't completed even once,
    // and the hardcoded fallback below is somehow also empty -- should
    // not happen in practice).
    //
    // Before the first successful refreshInstanceList() completes (e.g.
    // app's first-ever launch, or launched offline), this picks from a
    // small hardcoded fallback list -- a snapshot of notPipe.json's
    // "invidious" entries at the time this was written (see
    // FALLBACK_INSTANCES in the .cpp) -- rather than returning "" -- so
    // video playback isn't blocked on this being the very first network
    // call the app happens to make.
    QString pickRandomInstance() const;

    // An instance whose media proxy failed is skipped by pickRandomInstance()
    // for a while (it is still used if every known instance is marked bad).
    void markBad(const QString &instanceBaseUrl, int minutes = 10);

    // Returns true once at least one successful refresh has completed
    // (i.e. pickRandomInstance() is drawing from the live notPipe.json
    // list rather than the hardcoded fallback).
    bool hasLiveList() const
    {
        return !liveInstances.isEmpty();
    }

signals:
    // Emitted after a refresh attempt completes, success or failure --
    // mainly useful for logging/diagnostics; callers don't need to wait
    // for this before calling pickRandomInstance().
    void instanceListRefreshed(bool success);

private slots:
    void onInstancesJsonFinished();
    void onSslErrors(const QList<QSslError> &errors);
    void onFetchTimeout();

private:
    mutable QMap<QString, qint64> badUntilMs; // instance base url -> epoch ms
    QStringList liveInstances;  // populated from notPipe.json's "invidious" list; empty until first successful refresh
    bool refreshInFlight;
    // Tracks the in-flight instances.json request so onFetchTimeout() can
    // abort() it directly. NOT done via QTimer::singleShot(ms, reply,
    // SLOT(abort())) -- on this BB10/Qt4 build that logs
    // "Object::connect: No such slot QNetworkReplyImpl::abort()" and
    // silently never fires, because QNetworkReply::abort() is a pure
    // virtual and the concrete QNetworkReplyImpl doesn't re-expose it to
    // the moc/signal-slot system the way a normal Q_SLOT would need.
    // Routing through our own slot and calling reply->abort() as a plain
    // virtual function call sidesteps that entirely.
    QNetworkReply *pendingReply;

    // Parses notPipe.json and returns the "invidious" array as a list of
    // base URLs (e.g. "http://76.82.152.76:3000"). Other keys
    // ("yt2009", "piped", "ytapilegacy") are ignored -- see class
    // comment for why.
    static QStringList parseInstancesJson(const QString &json);
};

#endif /* INVIDIOUSINSTANCEMANAGER_HPP_ */
