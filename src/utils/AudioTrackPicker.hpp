#ifndef AUDIOTRACKPICKER_HPP_
#define AUDIOTRACKPICKER_HPP_

#include <QString>
#include <QUrl>
#include <QVariantMap>

// YouTube serves many auto-dubbed audio tracks next to the real one for
// popular videos (the log that prompted this had 68 audio formats for one
// video). Every audio format of such a video carries a content-type tag in
// its url, as the "xtags" query parameter:
//
//     xtags=acont=original:lang=en-US     the track the creator uploaded
//     xtags=acont=dubbed:lang=pt          human dub
//     xtags=acont=dubbed-auto:lang=de     machine dub
//     xtags=acont=descriptive             audio description
//
// (percent-encoded or not, depending on who produced the url). Instances /
// clients may also expose an "audioTrack" object whose displayName ends in
// "original" for the real track. Picking only by bitrate -- what the code
// did before -- lands on a random language.
//
// Returns:
//   2  explicitly the original track
//   1  not marked at all (single-language video: nothing to choose between)
//   0  explicitly a dub / description / secondary track
//
// Headers-only so both the Invidious and the Innertube paths can use it.
static inline int audioOriginalScore(const QString &url, const QVariantMap &format)
{
    // The url may be percent-encoded once or twice; decoding more than once
    // is harmless for the substring checks below.
    QString u = QUrl::fromPercentEncoding(url.toUtf8());
    u = QUrl::fromPercentEncoding(u.toUtf8()).toLower();

    int at = u.indexOf("acont=");
    if (at >= 0) {
        QString v = u.mid(at + 6);
        if (v.startsWith("original")) {
            return 2;
        }
        return 0; // dubbed, dubbed-auto, descriptive, secondary, ...
    }

    QVariantMap track = format["audioTrack"].toMap();
    if (!track.isEmpty()) {
        QString name = track["displayName"].toString().toLower();
        if (name.contains("original")) {
            return 2;
        }
        if (name.contains("dub") || name.contains("descriptive")) {
            return 0;
        }
    }
    return 1;
}

// Short human-readable description of the audio track, for the debug log.
static inline QString audioTrackDebugLabel(const QString &url, const QVariantMap &format)
{
    QString u = QUrl::fromPercentEncoding(url.toUtf8());
    u = QUrl::fromPercentEncoding(u.toUtf8());
    QString xtags;
    int at = u.indexOf("xtags=");
    if (at >= 0) {
        int end = u.indexOf('&', at);
        xtags = u.mid(at + 6, end < 0 ? -1 : end - at - 6);
    }
    QString name = format["audioTrack"].toMap()["displayName"].toString();
    return QString("xtags=[%1] audioTrack=[%2]").arg(xtags).arg(name);
}

#endif /* AUDIOTRACKPICKER_HPP_ */
