#include "InvidiousInstanceManager.hpp"
#include "src/applicationui.hpp"

#include <bb/data/JsonDataAccess>

#include <QtNetwork/QNetworkRequest>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <QTime>
#include <QDebug>
#include <QDateTime>
#include <QtNetwork/QSslCertificate>
#include <QTimer>

// Hardcoded fallback, used only until the first successful
// refreshInstanceList() -- e.g. very first app launch before any network
// call has completed, or launched with no connectivity. This is a
// snapshot of notPipe.json's "invidious" list as of when this was
// written (see http://144.31.189.129/notPipe.json) -- these are
// independently-run instances, NOT guaranteed to stay up forever, which
// is exactly why the live list (auto-updated by notPipe's maintainers)
// is preferred whenever available.
static const char *FALLBACK_INSTANCES[] = {
    "http://76.82.152.76:3000",
    "http://tube.kronickrusaders.ca:6666",
    "http://115.73.217.239:3000",
    "http://51.91.122.148:3000",
    "http://79.50.199.122:3000",
    "http://118.71.244.135:1224",
    "http://yt.nealfcc.top:3000",
    "http://92.217.193.252",
};
static const int FALLBACK_INSTANCES_COUNT = 8;

InvidiousInstanceManager::InvidiousInstanceManager(QObject *parent) :
        QObject(parent), refreshInFlight(false), pendingReply(0)
{
    // Seed a random source once per app run. qsrand() is process-global in
    // Qt4, so this is intentionally only done here, not per-call.
    qsrand(static_cast<uint>(QTime::currentTime().msec()));
}

void InvidiousInstanceManager::refreshInstanceList()
{
    if (refreshInFlight) {
        return;
    }
    refreshInFlight = true;

    QNetworkRequest request(QString("http://144.31.189.129/notPipe.json"));
    // Identify plainly rather than spoofing a browser -- this is a
    // metadata/discovery request, not a YouTube-facing one, so there's no
    // anti-bot concern here.
    request.setRawHeader("User-Agent", "bbtube (BlackBerry 10)");

    QNetworkReply *reply = ApplicationUI::networkManager->get(request);
    pendingReply = reply;
    QObject::connect(reply, SIGNAL(finished()), this, SLOT(onInstancesJsonFinished()));
    QObject::connect(reply, SIGNAL(sslErrors(QList<QSslError>)), this,
            SLOT(onSslErrors(QList<QSslError>)));

    // Qt4's QNetworkAccessManager on this BB10 build has no built-in
    // per-request timeout -- a stalled connection (e.g. a slow/broken
    // TLS handshake) would otherwise leave finished() never firing,
    // hanging refreshInstanceList() (and by extension, anything waiting
    // on it) indefinitely. 10s is generous for a small JSON fetch.
    QTimer::singleShot(10000, this, SLOT(onFetchTimeout()));
}

void InvidiousInstanceManager::onFetchTimeout()
{
    if (pendingReply && !pendingReply->isFinished()) {
        qDebug() << "[bbtube][invidious] notPipe.json fetch timed out after 10s, aborting";
        pendingReply->abort(); // triggers finished() -> onInstancesJsonFinished() with an error set
    }
}

void InvidiousInstanceManager::onSslErrors(const QList<QSslError> &errors)
{
    // QNetworkReply::errorString() alone just says "SSL handshake
    // failed" with no detail on WHY. This logs the actual QSslError
    // reason(s) -- e.g. CertificateUntrusted, HostNameMismatch,
    // CertificateExpired, SelfSignedCertificate -- so a fetch failure
    // can actually be diagnosed instead of guessed at.
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

void InvidiousInstanceManager::onInstancesJsonFinished()
{
    refreshInFlight = false;

    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply) {
        return;
    }
    if (reply == pendingReply) {
        pendingReply = 0;
    }
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        QVariant httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
        qDebug() << "[bbtube][invidious] notPipe.json fetch failed. url:" << reply->url().toString()
                 << ", error code:" << reply->error()
                 << ", error string:" << reply->errorString()
                 << ", http status:" << httpStatus;
        emit instanceListRefreshed(false);
        return;
    }

    QString json = QString::fromUtf8(reply->readAll());
    QStringList parsed = parseInstancesJson(json);

    if (parsed.isEmpty()) {
        qDebug() << "[bbtube][invidious] notPipe.json parsed to zero usable 'invidious' instances."
                     " First 200 chars of response:" << json.left(200);
        emit instanceListRefreshed(false);
        return;
    }

    liveInstances = parsed;
    qDebug() << "[bbtube][invidious] refreshed instance list, count =" << liveInstances.count();
    emit instanceListRefreshed(true);
}

QStringList InvidiousInstanceManager::parseInstancesJson(const QString &json)
{
    QStringList result;

    bb::data::JsonDataAccess jda;
    QVariant parsed = jda.loadFromBuffer(json);
    QVariantMap root = parsed.toMap();

    // notPipe.json's structure: a plain JSON object keyed by backend
    // type, e.g. {"invidious": ["http://host:port", ...], "yt2009": [...],
    // "piped": [...], "ytapilegacy": [...]}. We only understand/use the
    // "invidious" entries -- see the class comment in the header for why
    // the other three are skipped.
    QVariantList invidiousEntries = root["invidious"].toList();

    for (int i = 0; i < invidiousEntries.count(); i++) {
        QString uri = invidiousEntries[i].toString();
        if (uri.isEmpty()) {
            continue;
        }
        // Normalize: no trailing slash, so callers can always do
        // instanceBaseUrl + "/api/v1/...".
        while (uri.endsWith("/")) {
            uri.chop(1);
        }
        result.append(uri);
    }

    return result;
}

void InvidiousInstanceManager::markBad(const QString &instanceBaseUrl, int minutes)
{
    if (instanceBaseUrl.isEmpty()) return;
    QString key = instanceBaseUrl;
    while (key.endsWith('/')) key.chop(1);
    badUntilMs[key] = QDateTime::currentMSecsSinceEpoch() + qint64(minutes) * 60 * 1000;
    qDebug() << "[bbtube][invidious] marking instance as bad for" << minutes << "min:" << key;
}

QString InvidiousInstanceManager::pickRandomInstance() const
{
    QStringList all;
    if (!liveInstances.isEmpty()) {
        all = liveInstances;
    } else {
        for (int i = 0; i < FALLBACK_INSTANCES_COUNT; i++) {
            all << QString(FALLBACK_INSTANCES[i]);
        }
    }
    if (all.isEmpty()) {
        return "";
    }

    qint64 now = QDateTime::currentMSecsSinceEpoch();
    QStringList good;
    for (int i = 0; i < all.size(); i++) {
        QString key = all[i];
        while (key.endsWith('/')) key.chop(1);
        QMap<QString, qint64>::iterator it = badUntilMs.find(key);
        if (it != badUntilMs.end() && it.value() > now) continue;
        good << all[i];
    }
    if (good.isEmpty()) {
        good = all; // everything is marked bad: better a bad instance than none
    }
    return good[qrand() % good.count()];
}
