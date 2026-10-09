#ifndef STORAGEDATA_HPP_
#define STORAGEDATA_HPP_

class SingleVideoStorageData
{
public:
    QString url;
    QString cipher;
    QString quality;
    int duration;
    unsigned long long contentLength;
    // true for progressive formats (audio baked into the same file, e.g.
    // itag 18) -- can be played directly. false for adaptive video-only
    // formats (e.g. itag 136/137/298/299) -- must be paired with
    // StorageData::audio and remuxed (see StreamingRemuxSession) before
    // playback, since BB10's mmrenderer can't play two streams at once.
    bool hasEmbeddedAudio;

    SingleVideoStorageData() : duration(0), contentLength(0), hasEmbeddedAudio(true)
    {
    }
};

class AudioStorageData
{
public:
    QString url;
    QString cipher;
    unsigned long long contentLength;
};

class ClosedCaptionData
{
public:
    QString url;
    QString languageCode;
    QString languageName;
    bool isLoaded;
};

class StorageData
{
public:
    QList<SingleVideoStorageData> instances;
    AudioStorageData audio;
    QList<ClosedCaptionData> captions;
    // Invidious instance (scheme://host[:port], no trailing slash) that these
    // stream urls were obtained from; empty for the direct YouTube path. Its
    // /videoplayback proxy can fail even when its metadata api answered fine
    // (seen in the field: HTTP 500 / 502 on every media request), in which
    // case the player asks another instance for the same video.
    QString sourceInstance;
};

#endif /* STORAGEDATA_HPP_ */
