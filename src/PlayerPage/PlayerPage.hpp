#ifndef PLAYERPAGE_HPP_
#define PLAYERPAGE_HPP_

#include "src/parser/models/StorageData.hpp"
#include "src/parser/models/VideoMetadata.hpp"
#include "src/parser/models/ChannelData.hpp"
#include "src/utils/BasePage.hpp"
#include "src/VideoList/SearchListItemProvider.hpp"
#include "src/models/VideoListItemModel.hpp"
#include "src/settings/AppSettings.hpp"
#include "src/models/PlaylistVideoModel.hpp"
#include "src/models/PlaylistListItemModel.hpp"
#include "src/utils/CustomListView.hpp"
#include "src/utils/ChunkedRemuxSession.hpp"
#include <QPointer>
#include <QTimer>
#include <bb/cascades/ActivityIndicator>
#include <bb/cascades/Container>
#include <QTime>

#include <bb/cascades/Container>
#include <bb/cascades/Slider>
#include <bb/cascades/Label>
#include <bb/cascades/ListView>
#include <bb/cascades/ForeignWindowControl>
#include <bb/cascades/NavigationPane>
#include <bb/cascades/ActionItem>
#include <bb/cascades/InvokeActionItem>
#include <bb/cascades/ImageButton>

#include <bb/multimedia/MediaPlayer>
#include <bb/multimedia/MediaState>
#include <bb/multimedia/NowPlayingConnection>
#include <QStringList>
#include <bb/cascades/TouchEvent>
#include <bb/cascades/TrackpadEvent>
#include <bb/system/SystemUiResult>
#include <bb/cascades/DoubleTapEvent>

class PlayerPage: public BasePage
{
Q_OBJECT
private slots:
    void onForeignWindowBoundingChanged(bool);
    void onCcForeignWindowBoundingChanged(bool isBound);
    void onOrientationChanged();
    void onTouch(bb::cascades::TouchEvent *);
    void onMediaStateChanged(bb::multimedia::MediaState::Type);
    void onPlayActionItemClick();
    void onNextActionItemClick();
    void onPreviousActionItemClick();
    void onQualityActionItemClick();
    void onScalingActionItemClick();
    void onCcActionItemClick();
    void onTimecodeActionItemClick();
    void onDownloadActionItemClick();
    void onMetadataReceived(VideoMetadata, StorageData);
    void onPlayerPositionChanged(unsigned int);
    void onNpcPrev();
    void onNpcNext();
    void onProgressSliderValueChanged(float);
    void onProgressSliderImmediateValueChanged(float);
    void onProgressSliderTouch(bb::cascades::TouchEvent *);
    void onProgressSliderTrackpadEvent(bb::cascades::TrackpadEvent*);
    void onProgressSliderFocusedChanged(bool);
    void onCloseInfoContainerClick();
    void onBackward10ButtonClick();
    void onForward10ButtonClick();
    void onCloseButtonTrackpadEvent(bb::cascades::TrackpadEvent*);
    void onUpnextVideoListItemClick(QVariantList);
    void onQualityDialogFinished(bb::system::SystemUiResult::Type);
    void onScalingDialogFinished(bb::system::SystemUiResult::Type);
    void onCcDialogFinished(bb::system::SystemUiResult::Type);
    void onDownloadDialogFinished(bb::system::SystemUiResult::Type);
    void onChannelActionItemClick(QVariantList);
    void onChannelDataReceived(ChannelPageData);
    void onPlayAudioOnlyActionItemClick(QVariantList indexPath);
    void onVideoChannelActionItemClick();
    void onOpenVideoInBrowserActionItemClick();
    void onCopyVideoLinkActionItemClick();
    void onCopyStreamUrlActionItemClick();
    void onAddToFavoritesActionItemClick();
    void onAddToWatchLaterActionItemClick();
    void addToHistory();
    void onBrowserActionItemClick();
    void onDoubleTappedHandler(bb::cascades::DoubleTapEvent*);
    void onLoopScalingShortcut();
    void onAudioOnlyShortcut();
    void onVideoShortcut();
    void onRepeatActionItemClick();
    void onPlaylistActionItemClick();
    void onPlaylistChanged(int);
    void onPlaylistVideoAdded(PlaylistVideoModel* video);
    void onPlaylistVideoDeleted(QString videoId, PlaylistListItemModel::Type playlistType);
    void onPlaylistVideoDeletedAll(PlaylistListItemModel::Type playlistType);
    void onRemuxProgress(int chunkIndex);
    void onRemuxMerged(QString path, int startChunk, int chunkCount, bool isFinal);
    void onRemuxFailed(QString errorMessage);
    void onSeekRequested(unsigned int positionMs);
    void onRemuxWatchdog();
private:
    void init(VideoMetadata videoMetadata, StorageData storageData,
            bb::cascades::NavigationPane *navigationPane, bool audioOnly);
    VideoMetadata videoMetadata;
    StorageData storageData;
    AppSettings *appSettings;

    bb::cascades::ForeignWindowControl *foreignWindowControl;
    bb::cascades::ForeignWindowControl *ccForeignWindowControl;
    bb::cascades::Container *infoContainer;
    bb::cascades::Container *audioBackground;
    CustomListView *upNextListView;
    bb::cascades::Slider *progressSlider;
    bb::cascades::ImageButton *forward10Button;
    bb::cascades::ImageButton *backward10Button;
    bb::cascades::ActionItem *playlistActionItem;
    bb::cascades::ActionItem *qualityActionItem;
    bb::cascades::ActionItem *scalingActionItem;
    bb::cascades::ActionItem *nextActionItem;
    bb::cascades::ActionItem *previousActionItem;
    bb::cascades::ActionItem *downloadActionItem;
    bb::cascades::ActionItem *playActionItem;
    bb::cascades::ActionItem *repeatActionItem;
    bb::cascades::ActionItem *ccActionItem;
    bb::cascades::ActionItem *timecodeActionItem;
    bb::cascades::ActionItem *addToFavoritesActionItem;
    bb::cascades::ActionItem *addToWatchLaterActionItem;
    bb::cascades::InvokeActionItem *shareVideoActionItem;
    bb::cascades::Label *passedTime;
    bb::cascades::Label *remainingTime;
    bb::cascades::Label *title;
    bb::cascades::Label *subtitle;
    bb::cascades::ImageButton *closeButton;
    bb::cascades::Container *subtitleContainer;

    bool actionBarsVisible;
    bool movingInSlider;
    bool manualSeeking;
    bool trackpadFocusInSlider;
    bool sliderDoubleTap;
    bool isLiveStream;
    bool autoplay;
    bool alreadyPlaying;
    int duration;
    QString quality;
    QString nextVideoId;
    QString prevVideoId;
    // Downloads the adaptive video+audio pair in ~5s chunks, keeps them, and
    // produces complete merged MP4 files covering runs of consecutive chunks
    // (see ChunkedRemuxSession). 0 when the current video isn't remuxed.
    ChunkedRemuxSession *remuxSession;
    // Session of the quality we were playing before a mid-playback quality
    // change, kept running (and kept feeding the file the player is still
    // reading) until the new quality's first file takes over.
    ChunkedRemuxSession *retiringRemuxSession;
    // A merged file of the current remuxSession has been handed to the player.
    bool remuxPlaybackStarted;
    // The merged file the player has loaded is chunks
    // [playingStartChunk, playingStartChunk + playingChunkCount) of the
    // session that produced it. Its own 0:00 is playingStartSec into the
    // video; remuxPlayableSeconds is where its content ends (video time).
    int playingStartChunk;
    int playingChunkCount;
    double playingStartSec;
    double remuxPlayableSeconds;
    // The loaded file ends before the video does: the player will run out of
    // data at remuxPlayableSeconds unless it is swapped for a longer one.
    bool playingPartialRemux;
    bool remuxStallToastShown;
    // A longer merged file that is ready but not swapped in yet. Swapping
    // reloads the source, so it is done exactly when the player has played
    // the current file to its end: the new file then resumes on the keyframe
    // at that very boundary and nothing is repeated or skipped.
    QString pendingSwapPath;
    int pendingSwapStart;
    int pendingSwapCount;
    bool pendingSwapFinal;
    QPointer<ChunkedRemuxSession> pendingSwapSession;
    bool remuxRanDry; // player reached the end of the partial file
    bool remuxSwapping; // inside a source swap: ignore the state changes it causes
    unsigned int lastPositionMs; // last position tick (video time; the player's own value is unreliable at EOF)
    QTime lastPositionTickClock;
    double remuxLastMergeSeconds; // how long the last merge took, to start the next one early enough
    bool remuxMergeTimed;
    QTime remuxMergeClock;
    QTimer *remuxWatchdog;
    bool finalCopyRequested;
    // Where the session starts downloading (saved position / current position).
    double remuxStartAtSec;
    int initialStartChunk;
    // The person seeked to a place the loaded file does not contain: the
    // download head was moved there, a loading indicator is shown, and the
    // video resumes from seekTargetMs once a merged file covering it exists.
    bool seekPending;
    bool seekMergeRequested;
    unsigned int seekTargetMs;
    int seekChunk;
    bool seekResumePlay;
    bb::cascades::Container *seekLoadingOverlay;
    bb::cascades::ActivityIndicator *seekLoadingSpinner;
    QString pendingRemuxQualityLabel; // "" == initial playback, else = quality label pending a changeQuality() once the remux head is ready
    void resizeVideo();
    void playVideo();
    void setInfos();
    void toggleInfosVisibility();
    void hideInfos();
    void showInfos();
    void setAudioOnly(bool audioOnly);
    void changeQuality(QString newQuality, QString url, bool forcePlay = false, int seekMs = -1);
    void startPlaybackAt(QString url, int offsetMs = 0, bool partial = false);
    void playVideoWithRemux(SingleVideoStorageData videoData);
    void changeQualityWithRemux(QString newQuality, SingleVideoStorageData videoData);
    void startRemuxSession(SingleVideoStorageData videoData, double startAtSeconds);
    void clearRemuxSessions(); // cancels+deletes remuxSession and retiringRemuxSession, if present
    void maybeRequestRemuxMerge();
    void queuePendingSwap(ChunkedRemuxSession *session, QString path, int startChunk, int chunkCount, bool isFinal);
    void loadMergedFile(QString label, QString path, ChunkedRemuxSession *session, int startChunk, int chunkCount, bool isFinal, double resumeFileSeconds, double resumeAbsSeconds, bool forcePlay);
    ChunkedRemuxSession *playingSession();
    void beginRemuxSeek(unsigned int positionMs);
    void requestSeekMerge();
    void showSeekLoading(bool show);
    void maybeRequestFinalCopy();
    void swapToPendingFile();
    void markMergeRequested();
    void seekWithinPlayable(unsigned int positionMs);
    QString remuxCacheDir();
    QString remuxBaseNameFor(QString videoId, QString quality);
    int getIndexOfDefaultQuality();
    QString getScalingMethodString(bb::cascades::ScalingMethod::Type type);
    void adjustInfoScreen();
    void updateScaling(bb::cascades::ScalingMethod::Type newMethod);
    static QStringList watched;
    void updateRepeatActionItem();
    QString getCcPath(QString languageCode);
    void setPrevVideoId();
    void setNextVideoId();
    void setProgressSliderEnabled(bool value);
public:
    PlayerPage(VideoMetadata videoMetadata, StorageData storageData,
            bb::cascades::NavigationPane *navigationPane, bool audioOnly, bool isPlaylistPlaying =
                    false);
    PlayerPage(bb::cascades::NavigationPane *navigationPane);

    virtual ~PlayerPage()
    {
        playerContext->setCcForeignWindowControl(0);
    }
    virtual void playVideoFromOutside(QString url);
    virtual void playVideoFromPlaylist(QString url);
};

class PlayerPageSearchListItemActionSetBuilder: public SearchListItemActionSetBuilder
{
public:
    virtual ~PlayerPageSearchListItemActionSetBuilder()
    {
    }

    virtual void buildActionSet(const VideoListItemModel* item, SearchListItem *listItem)
    {
        listItem->addContinueActionItem();
        if (!item->isLiveStream()) {
            listItem->addPlayAudioOnlyActionItem();
        }
        listItem->addChannelActionItem();
        buildCommonVideoActionSet(item, listItem);
    }
};

#endif /* PLAYERPAGE_HPP_ */
