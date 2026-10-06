#include "GlobalPlayerContext.hpp"
#include "src/db/DbHelper.hpp"
#include "src/utils/PlaylistVideoProxy.hpp"
#include "src/models/PlaylistVideoModel.hpp"
#include "src/models/PlaylistListItemModel.hpp"
#include "src/utils/VideoViewedPercentProxy.hpp"

#include <bb/multimedia/MediaPlayer>
#include <bb/multimedia/MediaState>
#include <bb/multimedia/MediaError>
#include <bb/multimedia/EqualizerPreset>
#include <bb/multimedia/NowPlayingConnection>
#include <bb/cascades/ForeignWindowControl>
#include <bb/cascades/UIOrientation>
#include <bb/cascades/OrientationSupport>
#include <bb/cascades/ScalingMethod>
#include <bb/multimedia/RepeatMode>

GlobalPlayerContext::GlobalPlayerContext(QObject *parent) :
        QObject(parent), frameWidth(0), frameHeight(0), classic(false), audioOnly(false), screenWidth(
                0), screenHeight(0), windowId(""), windowGroup(""), windowHandle(0), repeatMode(
                bb::multimedia::RepeatMode::None), playlistId(0), updateViewedPercent(true), continuePlaying(
                false), partialSource(false), positionOffsetMs(0), fullDurationMs(0), seekIntercept(false)
{
    mediaPlayer = new bb::multimedia::MediaPlayer(this);
    mediaPlayer->setVideoOutput(bb::multimedia::VideoOutput::PrimaryDisplay);
    mediaPlayer->setEqualizerPreset(
            (bb::multimedia::EqualizerPreset::Type) DbHelper::getEqualizerPreset());
    mediaPlayer->setClosedCaptionEnabled(false);
    // load empty subtitles to "prepare" the media player
    mediaPlayer->setClosedCaptionUrl(1, QUrl("asset:///cc.xml"));

    npc = new bb::multimedia::NowPlayingConnection("myConnection", this);
    npc->setOverlayStyle(bb::multimedia::OverlayStyle::Fancy);
    npc->setPreviousEnabled(false);
    npc->setIconUrl(QUrl("file://" + QDir::currentPath() + "/app/native/icon.png"));

    QObject::connect(bb::cascades::OrientationSupport::instance(),
            SIGNAL(orientationAboutToChange(bb::cascades::UIOrientation::Type)), this,
            SLOT(onOrientationAboutToChange(bb::cascades::UIOrientation::Type)));
    QObject::connect(mediaPlayer, SIGNAL(videoDimensionsChanged(const QSize &)), this,
            SLOT(onVideoDimensionsChanged(const QSize &)));
    QObject::connect(mediaPlayer, SIGNAL(durationChanged(unsigned int)), this,
            SLOT(onPlayerDurationChanged(unsigned int)));
    QObject::connect(mediaPlayer, SIGNAL(mediaStateChanged(bb::multimedia::MediaState::Type)), this,
            SLOT(onMediaStateChanged(bb::multimedia::MediaState::Type)));
    QObject::connect(mediaPlayer, SIGNAL(positionChanged(unsigned int)), this,
            SLOT(onPlayerPositionChanged(unsigned int)));
    QObject::connect(npc, SIGNAL(acquired()), this, SLOT(onNpcAcquired()));
    QObject::connect(npc, SIGNAL(pause()), this, SLOT(onNpcPause()));
    QObject::connect(npc, SIGNAL(play()), this, SLOT(onNpcPlay()));
    QObject::connect(npc, SIGNAL(previous()), this, SLOT(onNpcPrev()));
    QObject::connect(npc, SIGNAL(next()), this, SLOT(onNpcNext()));
    QObject::connect(npc, SIGNAL(revoked()), this, SLOT(onNpcRevoked()));

    QObject::connect(PlaylistVideoProxy::getInstance(), SIGNAL(added(PlaylistVideoModel*)), this,
            SLOT(onPlaylistVideoAdded(PlaylistVideoModel*)));
    QObject::connect(PlaylistVideoProxy::getInstance(),
            SIGNAL(deleted(QString, PlaylistListItemModel::Type)), this,
            SLOT(onPlaylistVideoDeleted(QString, PlaylistListItemModel::Type)));
    QObject::connect(PlaylistVideoProxy::getInstance(),
            SIGNAL(deletedAll(PlaylistListItemModel::Type)), this,
            SLOT(onPlaylistVideoDeletedAll(PlaylistListItemModel::Type)));

    setScreenSize();
}

bb::multimedia::MediaPlayer *GlobalPlayerContext::getMediaPlayer()
{
    return mediaPlayer;
}

void GlobalPlayerContext::onNpcAcquired()
{
    mediaPlayer->play();

    npc->setMediaState(bb::multimedia::MediaState::Started);
}
void GlobalPlayerContext::onNpcPause()
{
    mediaPlayer->pause();
}
void GlobalPlayerContext::onNpcPlay()
{
    mediaPlayer->play();
}
void GlobalPlayerContext::onNpcPrev()
{
    emit npcPrev();
}
void GlobalPlayerContext::onNpcNext()
{
    emit npcNext();
}
void GlobalPlayerContext::onNpcRevoked()
{
    mediaPlayer->pause();
}
void GlobalPlayerContext::onPlayerDurationChanged(unsigned int duration)
{
    npc->setDuration(fullDurationMs > 0 ? (unsigned int) fullDurationMs : duration);
}
void GlobalPlayerContext::setPrevEnabled(bool enabled)
{
    npc->setPreviousEnabled(enabled);
}
void GlobalPlayerContext::setNextEnabled(bool enabled)
{
    npc->setNextEnabled(enabled);
}
void GlobalPlayerContext::setNpcMetadata(QVariantMap metadata)
{
    npc->setMetaData(metadata);
}
void GlobalPlayerContext::onPlayerPositionChanged(unsigned int position)
{
    position += positionOffsetMs; // file time -> video time

    npc->setPosition(position);

    emit positionChanged(position);
}
void GlobalPlayerContext::setForeignWindowControl(bb::cascades::ForeignWindowControl* fwc)
{
    this->fwc = fwc;
    mediaPlayer->setWindowId(fwc->windowId());
    mediaPlayer->setWindowGroupId(fwc->windowGroup());
}
void GlobalPlayerContext::setCcForeignWindowControl(bb::cascades::ForeignWindowControl* fwc)
{
    this->ccfwc = fwc;
    if (fwc) {
        mediaPlayer->setClosedCaptionWindowId(fwc->windowId());
    }
}
void GlobalPlayerContext::setWindowToForeignWindowControl(bb::cascades::ForeignWindowControl* fwc)
{
    this->fwc = fwc;
    fwc->bindToWindow(this->windowHandle, this->windowGroup, this->windowId);
}
void GlobalPlayerContext::setWindowToCcForeignWindowControl(bb::cascades::ForeignWindowControl* fwc)
{
    this->ccfwc = fwc;
    fwc->bindToWindow(this->ccWindowHandle, this->ccWindowGroup, this->ccWindowId);
}
void GlobalPlayerContext::onMediaStateChanged(bb::multimedia::MediaState::Type state)
{
    npc->setMediaState(state);

    if (state == bb::multimedia::MediaState::Stopped) {
        // "Stopped" is reported both when playback truly reaches the end
        // of the file and when it stops for any other reason (user
        // pause, a transient error, etc). Only mark the video as fully
        // watched / apply repeat-track behavior when we actually reached
        // the end, otherwise a mid-video pause or hiccup gets recorded as
        // "100% watched" and can trigger an unwanted repeat.
        unsigned int position = getPosition();
        unsigned int duration = getDuration();
        // A partial remux file also "ends" -- but that is only the end of
        // what has been downloaded so far. Treating it as the end of the
        // video would mark it 100% watched in the DB and, with
        // repeat-track on, restart the partial file from 0:00.
        bool reachedEnd = !partialSource && duration > 0 && position >= duration - 1500;

        if (reachedEnd) {
            if (updateViewedPercent) {
                DbHelper::setViewedPercent(this->videoMetadata.video.videoId, 10000);
                VideoViewedPercentProxy::getInstance()->setViewedPercent(
                        this->videoMetadata.video.videoId, 10000);
            }
            updateViewedPercent = true;

            if (repeatMode == bb::multimedia::RepeatMode::Track) {
                mediaPlayer->play();

                return;
            }
        }
    }

    emit mediaStateChanged(state);
}
bb::multimedia::MediaState::Type GlobalPlayerContext::getMediaState()
{
    return mediaPlayer->mediaState();
}
int GlobalPlayerContext::getPosition()
{
    return (int) mediaPlayer->position() + positionOffsetMs;
}
int GlobalPlayerContext::getDuration()
{
    return fullDurationMs > 0 ? fullDurationMs : (int) mediaPlayer->duration();
}
bb::multimedia::MediaError::Type GlobalPlayerContext::play()
{
    return mediaPlayer->play();
}
void GlobalPlayerContext::setPartialSource(bool partial)
{
    partialSource = partial;
}
bb::multimedia::MediaError::Type GlobalPlayerContext::play(QString url)
{
    // The caller describes a remuxed window of the video afterwards.
    partialSource = false;
    positionOffsetMs = 0;
    fullDurationMs = 0;
    seekIntercept = false;
    // Local filesystem paths (e.g. from the remux cache) have no scheme,
    // so QUrl(url) parses them with an empty scheme() -- mmrenderer then
    // can't identify the input type (it tries "playlist"/"autolist"
    // plugins first and fails with UnsupportedMediaType) even though the
    // file is a perfectly valid local mp4. Detect that case and build a
    // proper file:// URI instead. Remote http(s) urls already parse with
    // a scheme via QUrl(url), so they're left untouched.
    QUrl mediaUrl = QUrl(url);
    if (mediaUrl.scheme().isEmpty()) {
        mediaUrl = QUrl::fromLocalFile(url);
    }
    qDebug() << "[bbtube][player] play() input url =" << url
              << ", scheme =" << QUrl(url).scheme()
              << ", final mediaUrl =" << mediaUrl.toString()
              << ", isLocalFile =" << mediaUrl.isLocalFile();
    mediaPlayer->setSourceUrl(mediaUrl);
    mediaPlayer->prepare();
    bb::multimedia::MediaError::Type error = mediaPlayer->play();

    if (error == bb::multimedia::MediaError::None) {
        npc->acquire();
    }

    return error;
}
bb::multimedia::MediaError::Type GlobalPlayerContext::changeQuality(QString url, bool forcePlay,
        int seekMs)
{
    int currPosition = mediaPlayer->position();
    bb::multimedia::MediaState::Type currState = mediaPlayer->mediaState();
    // Same scheme-less-local-path issue as play() above -- see comment there.
    QUrl mediaUrl = QUrl(url);
    if (mediaUrl.scheme().isEmpty()) {
        mediaUrl = QUrl::fromLocalFile(url);
    }
    mediaPlayer->setSourceUrl(mediaUrl);
    mediaPlayer->prepare();

    // seekMs >= 0: the caller knows exactly where the new file must resume
    // (a keyframe boundary -- see ChunkedRemuxSession::keyframeResumeSeconds).
    // Seeking to an arbitrary position instead can make the player resume at
    // the previous keyframe, i.e. rewind by up to a whole GOP.
    // seekMs is VIDEO time; the new file starts positionOffsetMs into it
    // (setTimeline() has already been called for it).
    int target = seekMs >= 0 ? seekMs - positionOffsetMs : currPosition;
    if (target < 0) target = 0;
    if (!audioOnly) {
        mediaPlayer->seekTime(target);
    }
    // forcePlay: resume even though the old (partial) file had already run
    // out of data and the player sat in Stopped.
    if (currState == bb::multimedia::MediaState::Started || forcePlay) {
        bb::multimedia::MediaError::Type error = mediaPlayer->play();

        return error;
    }

    return bb::multimedia::MediaError::None;
}
QString GlobalPlayerContext::getSourceUrl()
{
    return mediaPlayer->sourceUrl().toString();
}
void GlobalPlayerContext::pause()
{
    mediaPlayer->pause();
}
void GlobalPlayerContext::acquire()
{
    npc->acquire();
}
void GlobalPlayerContext::seekTime(unsigned int position)
{
    if (seekIntercept) {
        emit seekRequested(position);
        return;
    }
    seekTimeDirect(position);
}
void GlobalPlayerContext::seekTimeDirect(unsigned int position)
{
    int rel = (int) position - positionOffsetMs;
    mediaPlayer->seekTime(rel > 0 ? (unsigned int) rel : 0);
}
void GlobalPlayerContext::setTimeline(int offsetMs, int fullDuration)
{
    positionOffsetMs = offsetMs;
    fullDurationMs = fullDuration;
    if (fullDurationMs > 0) {
        npc->setDuration((unsigned int) fullDurationMs);
    }
}
void GlobalPlayerContext::setSeekIntercept(bool intercept)
{
    seekIntercept = intercept;
}
void GlobalPlayerContext::skipXSecondsForward(unsigned int seconds)
{
    unsigned int videoDuration = getDuration();
    unsigned int videoPosition = getPosition();

    if (videoPosition + seconds * 1000 < videoDuration) {
        seekTime(videoPosition + seconds * 1000);
    } else {
        seekTime(videoDuration - 1);
    }
}
void GlobalPlayerContext::skipXSecondsBackward(unsigned int seconds)
{
    int videoPosition = getPosition();

    if (videoPosition - (int)seconds * 1000 > 0) {
        seekTime(videoPosition - (int)seconds * 1000);
    } else {
        seekTime(0);
    }
}

void GlobalPlayerContext::setMetadata(VideoMetadata videoMetadata, StorageData storageData,
        bool audioOnly)
{
    setViewedPercent();

    this->videoMetadata = videoMetadata;
    this->storageData = storageData;
    this->audioOnly = audioOnly;

    emit metadataChanged();
}
void GlobalPlayerContext::setAudioOnly(bool audioOnly)
{
    this->audioOnly = audioOnly;

    emit metadataChanged();
}
void GlobalPlayerContext::setSelectedStorageData(SingleVideoStorageData storageData)
{
    this->selectedStorageData = storageData;
}

void GlobalPlayerContext::onOrientationAboutToChange(bb::cascades::UIOrientation::Type type)
{
    int minValue = screenWidth <= screenHeight ? screenWidth : screenHeight;
    int maxValue = screenWidth > screenHeight ? screenWidth : screenHeight;

    if (type == bb::cascades::UIOrientation::Portrait) {
        screenWidth = minValue;
        screenHeight = maxValue;
    } else {
        screenWidth = maxValue;
        screenHeight = minValue;
    }

    emit orientationChanged();
}
void GlobalPlayerContext::onVideoDimensionsChanged(const QSize & size)
{
    if (size.width() != 0 && size.height() != 0) {
        resizeVideo();
    }
}

void GlobalPlayerContext::goFullscreen()
{
    setFrameSize(screenWidth, screenHeight);
}

void GlobalPlayerContext::resizeFullScreenVideo()
{
    goFullscreen();
    resizeVideo();
}

void GlobalPlayerContext::resizeVideo(int frameWidth, int frameHeight)
{
    setFrameSize(frameWidth, frameHeight);
    resizeVideo();
}

void GlobalPlayerContext::resizeVideo()
{
    if (ccWindowHandle && ccfwc) {
        if (!audioOnly) {
            bb::cascades::ScalingMethod::Type currScalingMethod = scalingMethod;
            scalingMethod = bb::cascades::ScalingMethod::AspectFit;
            resizeVideoLogic(ccfwc);
            scalingMethod = currScalingMethod;
        } else {
            ccfwc->setPreferredWidth(frameWidth);
            ccfwc->setPreferredHeight(frameHeight);
            ccfwc->setTranslationX(0);
            ccfwc->setTranslationY(0);
            ccfwc->setScale(1);
        }
    }
    if (!audioOnly) {
        resizeVideoLogic(fwc);
    }
}

void GlobalPlayerContext::resizeVideoLogic(bb::cascades::ForeignWindowControl *fwc)
{
    double ratio = (mediaPlayer->videoDimensions().width() + 0.0)
            / mediaPlayer->videoDimensions().height();
    double frameRatio = (frameWidth + 0.0) / frameHeight;
    double scale;

    int newWidth;
    int newHeight;

    switch (scalingMethod) {
        case bb::cascades::ScalingMethod::AspectFit:
        case bb::cascades::ScalingMethod::AspectFill:
        case bb::cascades::ScalingMethod::None:
            if (frameRatio <= ratio) {
                newWidth = frameWidth;
                newHeight = frameWidth / ratio;
                scale = (frameHeight + 0.0) / newHeight;
            } else {
                newWidth = frameHeight * ratio;
                newHeight = frameHeight;
                scale = (frameWidth + 0.0) / newWidth;
            }

            if (scalingMethod == bb::cascades::ScalingMethod::AspectFit) {
                scale = 1;
            } else if (scalingMethod == bb::cascades::ScalingMethod::None) {
                scale = (mediaPlayer->videoDimensions().width() + 0.0) / newWidth;
            }

            break;
        case bb::cascades::ScalingMethod::Fill:
            newWidth = frameWidth;
            newHeight = frameHeight;
            scale = 1;
            break;
    }

    fwc->setPreferredWidth(newWidth);
    fwc->setPreferredHeight(newHeight);
    fwc->setTranslationX((frameWidth - newWidth) / 2);
    fwc->setTranslationY((frameHeight - newHeight) / 2);
    fwc->setScale(scale);
}

void GlobalPlayerContext::setFrameSize(int frameWidth, int frameHeight)
{
    this->frameWidth = frameWidth;
    this->frameHeight = frameHeight;
}
void GlobalPlayerContext::setScreenSize()
{
    screen_context_t screen_context;
    screen_create_context(&screen_context, 0);

    int count = 0;
    screen_get_context_property_iv(screen_context, SCREEN_PROPERTY_DISPLAY_COUNT, &count);

    screen_display_t *screen_disps = (screen_display_t *) calloc(count, sizeof(screen_display_t));
    screen_get_context_property_pv(screen_context, SCREEN_PROPERTY_DISPLAYS,
            (void **) screen_disps);
    screen_display_t screen_disp = screen_disps[0];
    free(screen_disps);

    int sizes[2];
    screen_get_display_property_iv(screen_disp, SCREEN_PROPERTY_SIZE, sizes);

    int dpi[2];
    screen_get_display_property_iv(screen_disp, SCREEN_PROPERTY_DPI, dpi);

    screen_destroy_context(screen_context);

    this->screenWidth = sizes[0];
    this->screenHeight = sizes[1];
    this->classic = (sizes[0] == 720 && sizes[1] == 720 && dpi[0] == 294 && dpi[1] == 294);
}
bool GlobalPlayerContext::isWindowInitialized()
{
    return windowId != "";
}
bool GlobalPlayerContext::isClassic()
{
    return classic;
}
bool GlobalPlayerContext::hasKeyboard()
{
    return screenWidth == screenHeight;
}
void GlobalPlayerContext::setWindowData(screen_window_t handle, QString windowId,
        QString windowGroup)
{
    this->windowId = windowId;
    this->windowGroup = windowGroup;
    this->windowHandle = handle;
}
screen_window_t GlobalPlayerContext::getWindowHandle()
{
    return windowHandle;
}
void GlobalPlayerContext::resetWindowHandle()
{
    windowHandle = 0;
}
void GlobalPlayerContext::setCcWindowData(screen_window_t handle, QString windowId,
        QString windowGroup)
{
    this->ccWindowId = windowId;
    this->ccWindowGroup = windowGroup;
    this->ccWindowHandle = handle;
}
void GlobalPlayerContext::resetCcWindowHandle()
{
    ccWindowHandle = 0;
}
VideoMetadata GlobalPlayerContext::getVideoMetadata()
{
    return videoMetadata;
}
StorageData GlobalPlayerContext::getStorageData()
{
    return storageData;
}
SingleVideoStorageData GlobalPlayerContext::getSelectedStorageData()
{
    return selectedStorageData;
}
bool GlobalPlayerContext::isAudioOnly()
{
    return audioOnly;
}
bool GlobalPlayerContext::containsVideo()
{
    return videoMetadata.video.videoId != "";
}
void GlobalPlayerContext::stopMiniPlayer()
{
    setViewedPercent();
    updateViewedPercent = false;

    mediaPlayer->stop();
    npc->revoke();
    videoMetadata.video.videoId = "";
    resetWindowHandle();
    stopPlaylist();

    emit miniPlayerStopped();
}
bb::cascades::ScalingMethod::Type GlobalPlayerContext::getScalingMethod()
{
    return scalingMethod;
}
void GlobalPlayerContext::setScalingMethod(bb::cascades::ScalingMethod::Type method)
{
    scalingMethod = method;
}
void GlobalPlayerContext::resetScalingMethod()
{
    scalingMethod = bb::cascades::ScalingMethod::AspectFit;
}
bb::cascades::ScalingMethod::Type GlobalPlayerContext::getPlayerScalingMethod()
{
    return playerScalingMethod;
}
void GlobalPlayerContext::setPlayerScalingMethod(bb::cascades::ScalingMethod::Type method)
{
    playerScalingMethod = method;
}
void GlobalPlayerContext::resetPlayerScalingMethod()
{
    playerScalingMethod = bb::cascades::ScalingMethod::AspectFit;
}
int GlobalPlayerContext::getScreenWidth()
{
    return screenWidth;
}
int GlobalPlayerContext::getScreenHeight()
{
    return screenHeight;
}
bb::multimedia::MediaError::Type GlobalPlayerContext::setEqualizerPreset(
        bb::multimedia::EqualizerPreset::Type preset)
{
    return mediaPlayer->setEqualizerPreset(preset);
}
bb::multimedia::EqualizerPreset::Type GlobalPlayerContext::getEqualizerPreset()
{
    return mediaPlayer->equalizerPreset();
}
bb::multimedia::RepeatMode::Type GlobalPlayerContext::getRepeatMode()
{
    return repeatMode;
}
void GlobalPlayerContext::setRepeatMode(bb::multimedia::RepeatMode::Type type)
{
    repeatMode = type;
}
QString GlobalPlayerContext::getCcLanguage()
{
    return ccLanguage;
}
void GlobalPlayerContext::setCcLanguage(QString language)
{
    ccLanguage = language;
}
void GlobalPlayerContext::setClosedCaptionUrl(QUrl url)
{
    mediaPlayer->setClosedCaptionUrl(1, url);
    mediaPlayer->setClosedCaptionEnabled(true);
}
void GlobalPlayerContext::resetClosedCaptionUrl()
{
    mediaPlayer->setClosedCaptionUrl(1, QUrl("asset:///cc.xml"));
    mediaPlayer->setClosedCaptionEnabled(false);
}
void GlobalPlayerContext::setPlaylist(int playlistId, QList<QString> playlistSequence)
{
    if (this->playlistId != playlistId) {
        this->playlistId = playlistId;
        emit playlistChanged(playlistId);
    }

    this->playlistSequence = playlistSequence;
}
int GlobalPlayerContext::getPlaylistId()
{
    return playlistId;
}
void GlobalPlayerContext::onPlaylistVideoAdded(PlaylistVideoModel *video)
{
    if (video->playlistId == playlistId) {
        playlistSequence.append(video->videoId);
    }
}
void GlobalPlayerContext::onPlaylistVideoDeleted(QString videoId, PlaylistListItemModel::Type type)
{
    if ((int) type == playlistId) {
        playlistSequence.removeOne(videoId);

        if (playlistSequence.count() == 0) {
            stopPlaylist();
        }
    }
}
void GlobalPlayerContext::onPlaylistVideoDeletedAll(PlaylistListItemModel::Type type)
{
    if ((int) type == playlistId) {
        stopPlaylist();
    }
}
QString GlobalPlayerContext::getNextVideoFromPlaylist()
{
    for (int i = 0; i < playlistSequence.count(); i++) {
        if (playlistSequence[i] == videoMetadata.video.videoId) {
            if (i < playlistSequence.count() - 1) {
                return playlistSequence[i + 1];
            }
            if (repeatMode == bb::multimedia::RepeatMode::All) {
                return playlistSequence[0];
            }

            return "";
        }
    }

    if (playlistSequence.count() > 0) {
        return playlistSequence[0];
    }

    return "";
}

QString GlobalPlayerContext::getPrevVideoFromPlaylist()
{
    for (int i = 0; i < playlistSequence.count(); i++) {
        if (playlistSequence[i] == videoMetadata.video.videoId) {
            if (i > 0) {
                return playlistSequence[i - 1];
            }
            if (repeatMode == bb::multimedia::RepeatMode::All) {
                return playlistSequence[playlistSequence.count() - 1];
            }

            return "";
        }
    }

    if (playlistSequence.count() > 0) {
        return playlistSequence[0];
    }

    return "";
}

QString GlobalPlayerContext::getVideoFromPlaylistByIndex(int index)
{
    return playlistSequence[index];
}

void GlobalPlayerContext::stopPlaylist()
{
    if (this->playlistId != 0) {
        playlistId = 0;
        emit playlistChanged(0);
    }

    playlistSequence.clear();
}

void GlobalPlayerContext::shufflePlaylist()
{
    qsrand(QDateTime::currentDateTimeUtc().toTime_t());

    for (int i = playlistSequence.count(); i > 1; i--) {
        int j = qrand() % i;
        playlistSequence.swap(i - 1, j);
    }
}

QList<PlaylistVideoModel*> GlobalPlayerContext::getPlaylistTransposition(
        const QMap<QString, PlaylistVideoModel*> *map)
{
    QList<PlaylistVideoModel*> list;

    for (int i = 0; i < playlistSequence.count(); i++) {
        if (map->contains(playlistSequence[i])) {
            list.append((*map)[playlistSequence[i]]);
        }
    }

    return list;
}

void GlobalPlayerContext::setViewedPercent()
{
    if (this->videoMetadata.video.videoId == "") {
        return;
    }

    int viewedPercent =
            getDuration() > 0 ? 10000 * (long long) getPosition() / getDuration() : 0;
    DbHelper::setViewedPercent(this->videoMetadata.video.videoId, viewedPercent);
    VideoViewedPercentProxy::getInstance()->setViewedPercent(this->videoMetadata.video.videoId,
            viewedPercent);
}

void GlobalPlayerContext::setContinuePlaying(bool value)
{
    continuePlaying = value;
}

bool GlobalPlayerContext::isContinuePlaying()
{
    return continuePlaying;
}
