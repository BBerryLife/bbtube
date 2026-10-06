#include "PlayerPage.hpp"

#include "src/parser/YoutubeClient.hpp"
#include "src/parser/models/StorageData.hpp"
#include "src/parser/models/VideoMetadata.hpp"
#include "src/parser/models/ChannelData.hpp"
#include "src/utils/UIUtils.hpp"
#include "src/models/VideoListItemModel.hpp"
#include "src/VideoList/SearchListItemProvider.hpp"
#include "src/ChannelPage/ChannelPage.hpp"
#include "src/db/DbHelper.hpp"
#include "src/utils/ActionItemService.hpp"
#include "src/applicationui.hpp"
#include "src/settings/AppSettings.hpp"
#include "src/utils/CCUtils.hpp"
#include "src/utils/PlaylistVideoProxy.hpp"
#include "src/models/PlaylistVideoModel.hpp"
#include "src/models/PlaylistListItemModel.hpp"
#include "src/utils/PlaylistSheet.hpp"
#include "src/utils/UpdatableDataModel.hpp"
#include "TimecodeSheet.hpp"
#include "src/utils/VideoViewedPercentProxy.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QDateTime>
#include <QRegExp>

#include <bb/cascades/Page>
#include <bb/cascades/Container>
#include <bb/cascades/ActivityIndicator>
#include <bb/cascades/DockLayout>
#include <bb/cascades/StackLayout>
#include <bb/cascades/StackLayoutProperties>
#include <bb/cascades/LayoutOrientation>
#include <bb/cascades/ForeignWindowControl>
#include <bb/cascades/TouchEvent>
#include <bb/cascades/Window>
#include <bb/cascades/Slider>
#include <bb/cascades/ImageButton>
#include <bb/cascades/ToggleButton>
#include <bb/cascades/Label>
#include <bb/cascades/LabelAutoSizeProperties>
#include <bb/cascades/SystemDefaults>
#include <bb/cascades/ScalingMethod>

#include <bb/multimedia/MediaError>
#include <bb/multimedia/MediaState>
#include <bb/multimedia/MetaData>
#include <bb/multimedia/RepeatMode>

#include <bb/cascades/TouchEvent>
#include <bb/cascades/TouchType>
#include <bb/cascades/PropagationPhase>
#include <bb/cascades/TextField>
#include <bb/cascades/HorizontalAlignment>
#include <bb/cascades/VerticalAlignment>
#include <bb/cascades/Color>
#include <bb/cascades/UIConfig>
#include <bb/system/InvokeManager>
#include <bb/system/InvokeRequest>
#include <bb/system/InvokeTargetReply>
#include <bb/system/SystemListDialog>
#include <bb/system/SystemUiResult>
#include <bb/system/Clipboard>
#include <bb/cascades/ActionItem>
#include <bb/cascades/InvokeActionItem>
#include <bb/cascades/TrackpadHandler>
#include <bb/cascades/TrackpadEvent>
#include <bb/cascades/TrackpadEventType>
#include <bb/cascades/DoubleTapHandler>
#include <bb/cascades/DoubleTapEvent>
#include <bb/cascades/Shortcut>

#include <bb/cascades/QListDataModel>
#include <QStringList>

QStringList PlayerPage::watched;

PlayerPage::PlayerPage(bb::cascades::NavigationPane *navigationPane) :
        BasePage(navigationPane, false)
{
    alreadyPlaying = true;
    playerContext->setScalingMethod(playerContext->getPlayerScalingMethod());
    init(playerContext->getVideoMetadata(), playerContext->getStorageData(), navigationPane,
            playerContext->isAudioOnly());
}

PlayerPage::PlayerPage(VideoMetadata videoMetadata, StorageData storageData,
        bb::cascades::NavigationPane *navigationPane, bool audioOnly, bool isPlaylistPlaying) :
        BasePage(navigationPane, false)
{
    alreadyPlaying = false;
    playerContext->resetScalingMethod();
    playerContext->resetPlayerScalingMethod();
    if (!isPlaylistPlaying) {
        playerContext->stopPlaylist();
    }
    init(videoMetadata, storageData, navigationPane, audioOnly);
}

void PlayerPage::init(VideoMetadata videoMetadata, StorageData storageData,
        bb::cascades::NavigationPane *navigationPane, bool audioOnly)
{
    this->videoMetadata = videoMetadata;
    this->storageData = storageData;
    this->audioOnly = audioOnly;
    this->manualSeeking = false;
    this->movingInSlider = false;
    this->sliderDoubleTap = false;
    this->trackpadFocusInSlider = false;
    appSettings = ApplicationUI::appSettings;
    this->remuxSession = 0;
    this->retiringRemuxSession = 0;
    this->remuxPlaybackStarted = false;
    this->playingStartChunk = 0;
    this->playingChunkCount = 0;
    this->playingStartSec = 0;
    this->remuxPlayableSeconds = 0;
    this->playingPartialRemux = false;
    this->remuxStallToastShown = false;
    this->pendingSwapStart = 0;
    this->pendingSwapCount = 0;
    this->pendingSwapFinal = false;
    this->remuxRanDry = false;
    this->remuxSwapping = false;
    this->lastPositionMs = 0;
    this->remuxLastMergeSeconds = 0;
    this->remuxMergeTimed = false;
    this->finalCopyRequested = false;
    this->remuxStartAtSec = 0;
    this->initialStartChunk = -1;
    this->seekPending = false;
    this->seekMergeRequested = false;
    this->seekTargetMs = 0;
    this->seekChunk = -1;
    this->seekResumePlay = false;
    this->seekLoadingOverlay = 0;
    this->seekLoadingSpinner = 0;
    this->remuxWatchdog = new QTimer(this);
    QObject::connect(remuxWatchdog, SIGNAL(timeout()), this, SLOT(onRemuxWatchdog()));
    this->pendingRemuxQualityLabel = "";

    root->setLayout(new bb::cascades::DockLayout());
    foreignWindowControl = bb::cascades::ForeignWindowControl::create().updatedProperties(
            bb::cascades::WindowProperty::Size | bb::cascades::WindowProperty::Position
                    | bb::cascades::WindowProperty::Visible);
    root->add(foreignWindowControl);
    ccForeignWindowControl = bb::cascades::ForeignWindowControl::create().updatedProperties(
            bb::cascades::WindowProperty::Size | bb::cascades::WindowProperty::Position
                    | bb::cascades::WindowProperty::Visible);
    root->add(ccForeignWindowControl);

    if (playerContext->isWindowInitialized()) {
        playerContext->setWindowToForeignWindowControl(foreignWindowControl);
        playerContext->setWindowToCcForeignWindowControl(ccForeignWindowControl);

        if (alreadyPlaying) {
            playerContext->resizeFullScreenVideo();
        } else {
            playerContext->goFullscreen();
        }
    } else {
        foreignWindowControl->setWindowId("PlayerWindow");
        ccForeignWindowControl->setWindowId("CCWindow");
        playerContext->setForeignWindowControl(foreignWindowControl);
        playerContext->setCcForeignWindowControl(ccForeignWindowControl);
        playerContext->goFullscreen();
    }

    if (!alreadyPlaying) {
        playerContext->setMetadata(videoMetadata, storageData, audioOnly);
    }

    audioBackground = new bb::cascades::Container();
    audioBackground->setVerticalAlignment(bb::cascades::VerticalAlignment::Fill);
    audioBackground->setHorizontalAlignment(bb::cascades::HorizontalAlignment::Fill);
    audioBackground->setVisible(audioOnly);
    audioBackground->setLayout(new bb::cascades::DockLayout());
    bb::cascades::ImageView *backgroundImage = bb::cascades::ImageView::create();
    backgroundImage->setVerticalAlignment(bb::cascades::VerticalAlignment::Center);
    backgroundImage->setHorizontalAlignment(bb::cascades::HorizontalAlignment::Center);
    backgroundImage->setImageSource(QString("asset:///images/player_background.png"));
    audioBackground->add(backgroundImage);
    root->add(audioBackground);

    // Shown while waiting for a part of the video that is not downloaded yet
    // (the person seeked there); the earlier, already downloaded parts stay.
    seekLoadingOverlay = new bb::cascades::Container();
    seekLoadingOverlay->setVerticalAlignment(bb::cascades::VerticalAlignment::Center);
    seekLoadingOverlay->setHorizontalAlignment(bb::cascades::HorizontalAlignment::Center);
    seekLoadingOverlay->setLayout(new bb::cascades::DockLayout());
    seekLoadingSpinner = bb::cascades::ActivityIndicator::create();
    seekLoadingSpinner->setVerticalAlignment(bb::cascades::VerticalAlignment::Center);
    seekLoadingSpinner->setHorizontalAlignment(bb::cascades::HorizontalAlignment::Center);
    seekLoadingSpinner->setPreferredWidth(ui->du(20));
    seekLoadingSpinner->setPreferredHeight(ui->du(20));
    seekLoadingOverlay->add(seekLoadingSpinner);
    seekLoadingOverlay->setVisible(false);
    root->add(seekLoadingOverlay);

    infoContainer = bb::cascades::Container::create();
    infoContainer->setLeftPadding(ui->du(1));
    infoContainer->setTopPadding(ui->du(1));
    infoContainer->setRightPadding(ui->du(1));
    infoContainer->setBottomPadding(ui->du(1));
    infoContainer->setVerticalAlignment(bb::cascades::VerticalAlignment::Top);
    infoContainer->setHorizontalAlignment(bb::cascades::HorizontalAlignment::Fill);
    infoContainer->setOpacity(0.5);
    infoContainer->setBackground(bb::cascades::Color::Black);

    bb::cascades::Container *headerContainer = bb::cascades::Container::create();
    bb::cascades::Container *titleContainer = bb::cascades::Container::create();
    headerContainer->setLayout(new bb::cascades::DockLayout());
    headerContainer->setHorizontalAlignment(bb::cascades::HorizontalAlignment::Fill);
    titleContainer->setHorizontalAlignment(bb::cascades::HorizontalAlignment::Fill);
    if (playerContext->isClassic()) {
        titleContainer->setRightPadding(40); // space for 'Close' icon, it's size is 32x32
    }
    title = bb::cascades::Label::create();
    title->textStyle()->setBase(bb::cascades::SystemDefaults::TextStyles::titleText());
    title->setMultiline(true);
    title->autoSize()->setMaxLineCount(2);
    title->setHorizontalAlignment(bb::cascades::HorizontalAlignment::Left);
    title->setVerticalAlignment(bb::cascades::VerticalAlignment::Top);
    titleContainer->add(title);
    headerContainer->add(titleContainer);

    closeButton = bb::cascades::ImageButton::create().defaultImage(
            QString("asset:///images/ic_close.png")).pressedImage(
            QString("asset:///images/ic_close.png"));
    closeButton->setHorizontalAlignment(bb::cascades::HorizontalAlignment::Right);
    closeButton->setVerticalAlignment(bb::cascades::VerticalAlignment::Top);
    if (playerContext->isClassic()) {
        headerContainer->add(closeButton);
    } else {
        closeButton->setParent(this);
    }

    subtitleContainer = bb::cascades::Container::create();
    subtitleContainer->setHorizontalAlignment(bb::cascades::HorizontalAlignment::Fill);
    subtitle = bb::cascades::Label::create();
    subtitle->textStyle()->setBase(bb::cascades::SystemDefaults::TextStyles::subtitleText());
    subtitle->setTopMargin(0);
    subtitleContainer->add(subtitle);

    upNextListView = new CustomListView();
    SearchFlatListItemProvider *upNextListViewProvider = new SearchFlatListItemProvider(
            upNextListView, new PlayerPageSearchListItemActionSetBuilder());
    upNextListView->setListItemProvider(upNextListViewProvider);

    forward10Button = bb::cascades::ImageButton::create().defaultImage(
            QString("asset:///images/ic_forward_10.png")).pressedImage(
            QString("asset:///images/ic_forward_10.png"));
    backward10Button = bb::cascades::ImageButton::create().defaultImage(
            QString("asset:///images/ic_backward_10.png")).pressedImage(
            QString("asset:///images/ic_backward_10.png"));
    bb::cascades::Container *progressContainer = bb::cascades::Container::create().layout(
            bb::cascades::StackLayout::create().orientation(
                    bb::cascades::LayoutOrientation::LeftToRight));
    progressContainer->setHorizontalAlignment(bb::cascades::HorizontalAlignment::Fill);
    progressContainer->setBottomPadding(ui->du(15));
    progressContainer->setTopPadding(ui->du(3));
    progressContainer->add(backward10Button);
    bb::cascades::Container *sliderArea = bb::cascades::Container::create();
    sliderArea->setLayoutProperties(StackLayoutProperties::create().spaceQuota(1));
    progressSlider = bb::cascades::Slider::create().from(0);
    progressSlider->setBottomMargin(-(ui->du(2)));
    sliderArea->add(progressSlider);
    bb::cascades::Container *timeLabelsContainer = bb::cascades::Container::create();
    timeLabelsContainer->setLayout(bb::cascades::DockLayout::create());
    timeLabelsContainer->setLeftPadding(ui->du(2));
    timeLabelsContainer->setRightPadding(ui->du(2));
    timeLabelsContainer->setHorizontalAlignment(bb::cascades::HorizontalAlignment::Fill);
    passedTime = bb::cascades::Label::create();
    remainingTime = bb::cascades::Label::create();
    remainingTime->setHorizontalAlignment(bb::cascades::HorizontalAlignment::Right);
    timeLabelsContainer->add(passedTime);
    timeLabelsContainer->add(remainingTime);
    sliderArea->add(timeLabelsContainer);
    progressContainer->add(sliderArea);
    progressContainer->add(forward10Button);

    infoContainer->add(headerContainer);
    infoContainer->add(subtitleContainer);
    infoContainer->add(upNextListView);
    infoContainer->add(progressContainer);
    root->add(infoContainer);
    this->actionBarsVisible = playerContext->getMediaState() == bb::multimedia::MediaState::Paused
            || playerContext->getMediaState() == bb::multimedia::MediaState::Stopped;

    if (this->actionBarsVisible) {
        showInfos();
    } else {
        hideInfos();
    }

    root->add(overlay);
    adjustInfoScreen();

    this->setContent(root);

    if (playerContext->getPlaylistId() > 0) {
        playlistActionItem = bb::cascades::ActionItem::create();
        playlistActionItem->setTitle("Playlist");
        playlistActionItem->setImageSource(QString("asset:///images/ic_view_list.png"));
        playlistActionItem->addShortcut(Shortcut::create().key("l"));
        this->addAction(playlistActionItem, bb::cascades::ActionBarPlacement::InOverflow);
        QObject::connect(playlistActionItem, SIGNAL(triggered()), this,
                SLOT(onPlaylistActionItemClick()));
    } else {
        playActionItem = 0;
    }
    qualityActionItem = bb::cascades::ActionItem::create();
    qualityActionItem->addShortcut(Shortcut::create().key("q"));
    this->addAction(qualityActionItem, bb::cascades::ActionBarPlacement::InOverflow);
    scalingActionItem = bb::cascades::ActionItem::create();
    scalingActionItem->setTitle(getScalingMethodString(playerContext->getScalingMethod()));
    scalingActionItem->setImageSource(QString("asset:///images/ic_scaling.png"));
    scalingActionItem->setObjectName("Scaling");
    scalingActionItem->addShortcut(Shortcut::create().key("s"));
    this->addAction(scalingActionItem, bb::cascades::ActionBarPlacement::InOverflow);
    ccActionItem = bb::cascades::ActionItem::create();
    ccActionItem->setTitle("Off");
    ccActionItem->setImageSource(QString("asset:///images/ic_cc.png"));
    ccActionItem->addShortcut(Shortcut::create().key("c"));
    this->addAction(ccActionItem, bb::cascades::ActionBarPlacement::InOverflow);
    timecodeActionItem = bb::cascades::ActionItem::create();
    timecodeActionItem->setTitle("Timestamps");
    timecodeActionItem->setImageSource(QString("asset:///images/ic_timecode.png"));
    timecodeActionItem->addShortcut(Shortcut::create().key("t"));
    this->addAction(timecodeActionItem, bb::cascades::ActionBarPlacement::InOverflow);
    repeatActionItem = bb::cascades::ActionItem::create();
    repeatActionItem->addShortcut(Shortcut::create().key("r"));
    this->addAction(repeatActionItem, bb::cascades::ActionBarPlacement::InOverflow);
    previousActionItem = bb::cascades::ActionItem::create();
    previousActionItem->setTitle("Previous");
    previousActionItem->setImageSource(QString("asset:///images/ic_previous.png"));
    previousActionItem->setObjectName("Previous");
    previousActionItem->addShortcut(Shortcut::create().key("p"));
    this->addAction(previousActionItem, bb::cascades::ActionBarPlacement::OnBar);
    playActionItem = bb::cascades::ActionItem::create();
    playActionItem->setTitle("Play");
    playActionItem->setImageSource(QString("asset:///images/ic_play.png"));
    playActionItem->setObjectName("Play");
    playActionItem->addShortcut(Shortcut::create().key("Space"));
    this->addAction(playActionItem, bb::cascades::ActionBarPlacement::Signature);
    nextActionItem = bb::cascades::ActionItem::create();
    nextActionItem->setTitle("Next");
    nextActionItem->setImageSource(QString("asset:///images/ic_next.png"));
    nextActionItem->setObjectName("Next");
    nextActionItem->addShortcut(Shortcut::create().key("n"));
    this->addAction(nextActionItem, bb::cascades::ActionBarPlacement::OnBar);
    downloadActionItem = bb::cascades::ActionItem::create();
    downloadActionItem->setTitle("Download");
    downloadActionItem->setImageSource(QString("asset:///images/ic_download.png"));
    downloadActionItem->setObjectName("Download");
    downloadActionItem->addShortcut(Shortcut::create().key("d"));
    this->addAction(downloadActionItem, bb::cascades::ActionBarPlacement::InOverflow);

    addToFavoritesActionItem = ActionItemService::addToFavoritesActionItem(this);
    addToWatchLaterActionItem = ActionItemService::addToWatchLaterActionItem(this);
    bb::cascades::ActionItem *channelActionItem = ActionItemService::channelActionItem(this);
    shareVideoActionItem = ActionItemService::shareVideoActionItem(this);
    bb::cascades::ActionItem *openVideoInBrowserActionItem =
            ActionItemService::openVideoInBrowserActionItem(this);
    bb::cascades::ActionItem *copyVideoLinkActionItem = ActionItemService::copyVideoLinkActionItem(
            this);

    this->addAction(addToFavoritesActionItem, bb::cascades::ActionBarPlacement::InOverflow);
    this->addAction(addToWatchLaterActionItem, bb::cascades::ActionBarPlacement::InOverflow);
    this->addAction(channelActionItem, bb::cascades::ActionBarPlacement::InOverflow);
    this->addAction(shareVideoActionItem, bb::cascades::ActionBarPlacement::InOverflow);
    this->addAction(openVideoInBrowserActionItem, bb::cascades::ActionBarPlacement::InOverflow);
    this->addAction(copyVideoLinkActionItem, bb::cascades::ActionBarPlacement::InOverflow);

    bb::cascades::ActionItem *copyStreamUrlActionItem = bb::cascades::ActionItem::create().parent(this);
    copyStreamUrlActionItem->setImageSource(QString("asset:///images/ic_copy_link.png"));
    copyStreamUrlActionItem->setTitle("Copy Stream URL");
    this->addAction(copyStreamUrlActionItem, bb::cascades::ActionBarPlacement::InOverflow);

    QObject::connect(youtubeClient, SIGNAL(channelDataReceived(ChannelPageData)), this,
            SLOT(onChannelDataReceived(ChannelPageData)));
    QObject::connect(foreignWindowControl, SIGNAL(boundToWindowChanged(bool)), this,
            SLOT(onForeignWindowBoundingChanged(bool)));
    QObject::connect(ccForeignWindowControl, SIGNAL(boundToWindowChanged(bool)), this,
            SLOT(onCcForeignWindowBoundingChanged(bool)));

    QObject::connect(root, SIGNAL(touch(bb::cascades::TouchEvent *)), this,
            SLOT(onTouch(bb::cascades::TouchEvent *)));
    QObject::connect(audioBackground, SIGNAL(touch(bb::cascades::TouchEvent *)), this,
            SLOT(onTouch(bb::cascades::TouchEvent *)));
    QObject::connect(headerContainer, SIGNAL(touch(bb::cascades::TouchEvent *)), this,
            SLOT(onTouch(bb::cascades::TouchEvent *)));
    QObject::connect(subtitleContainer, SIGNAL(touch(bb::cascades::TouchEvent *)), this,
            SLOT(onTouch(bb::cascades::TouchEvent *)));
    QObject::connect(foreignWindowControl, SIGNAL(touch(bb::cascades::TouchEvent *)), this,
            SLOT(onTouch(bb::cascades::TouchEvent *)));
    QObject::connect(ccForeignWindowControl, SIGNAL(touch(bb::cascades::TouchEvent *)), this,
            SLOT(onTouch(bb::cascades::TouchEvent *)));

    QObject::connect(playerContext, SIGNAL(mediaStateChanged(bb::multimedia::MediaState::Type)),
            this, SLOT(onMediaStateChanged(bb::multimedia::MediaState::Type)));
    QObject::connect(playerContext, SIGNAL(positionChanged(unsigned int)), this,
            SLOT(onPlayerPositionChanged(unsigned int)));
    QObject::connect(playerContext, SIGNAL(seekRequested(unsigned int)), this,
            SLOT(onSeekRequested(unsigned int)));
    QObject::connect(playerContext, SIGNAL(npcPrev()), this, SLOT(onNpcPrev()));
    QObject::connect(playerContext, SIGNAL(npcNext()), this, SLOT(onNpcNext()));
    QObject::connect(playerContext, SIGNAL(orientationChanged()), this,
            SLOT(onOrientationChanged()));
    QObject::connect(playerContext, SIGNAL(playlistChanged(int)), this,
            SLOT(onPlaylistChanged(int)));

    QObject::connect(progressSlider, SIGNAL(valueChanged(float)), this,
            SLOT(onProgressSliderValueChanged(float)));
    QObject::connect(progressSlider, SIGNAL(immediateValueChanged(float)), this,
            SLOT(onProgressSliderImmediateValueChanged(float)));
    QObject::connect(progressSlider, SIGNAL(touch(bb::cascades::TouchEvent *)), this,
            SLOT(onProgressSliderTouch(bb::cascades::TouchEvent *)));
    QObject::connect(progressSlider, SIGNAL(focusedChanged(bool)), this,
            SLOT(onProgressSliderFocusedChanged(bool)));
    QObject::connect(playActionItem, SIGNAL(triggered()), this, SLOT(onPlayActionItemClick()));
    QObject::connect(nextActionItem, SIGNAL(triggered()), this, SLOT(onNextActionItemClick()));
    QObject::connect(previousActionItem, SIGNAL(triggered()), this,
            SLOT(onPreviousActionItemClick()));
    QObject::connect(qualityActionItem, SIGNAL(triggered()), this,
            SLOT(onQualityActionItemClick()));
    QObject::connect(repeatActionItem, SIGNAL(triggered()), this, SLOT(onRepeatActionItemClick()));
    QObject::connect(scalingActionItem, SIGNAL(triggered()), this,
            SLOT(onScalingActionItemClick()));
    QObject::connect(ccActionItem, SIGNAL(triggered()), this, SLOT(onCcActionItemClick()));
    QObject::connect(timecodeActionItem, SIGNAL(triggered()), this,
            SLOT(onTimecodeActionItemClick()));
    QObject::connect(downloadActionItem, SIGNAL(triggered()), this,
            SLOT(onDownloadActionItemClick()));
    QObject::connect(channelActionItem, SIGNAL(triggered()), this,
            SLOT(onVideoChannelActionItemClick()));
    QObject::connect(openVideoInBrowserActionItem, SIGNAL(triggered()), this,
            SLOT(onOpenVideoInBrowserActionItemClick()));
    QObject::connect(copyVideoLinkActionItem, SIGNAL(triggered()), this,
            SLOT(onCopyVideoLinkActionItemClick()));
    QObject::connect(addToFavoritesActionItem, SIGNAL(triggered()), this,
            SLOT(onAddToFavoritesActionItemClick()));
    QObject::connect(addToWatchLaterActionItem, SIGNAL(triggered()), this,
            SLOT(onAddToWatchLaterActionItemClick()));
    QObject::connect(copyStreamUrlActionItem, SIGNAL(triggered()), this,
            SLOT(onCopyStreamUrlActionItemClick()));

    QObject::connect(closeButton, SIGNAL(clicked()), this, SLOT(onCloseInfoContainerClick()));
    QObject::connect(backward10Button, SIGNAL(clicked()), this, SLOT(onBackward10ButtonClick()));
    QObject::connect(forward10Button, SIGNAL(clicked()), this, SLOT(onForward10ButtonClick()));

    QObject::connect(upNextListView, SIGNAL(triggered(QVariantList)), this,
            SLOT(onUpnextVideoListItemClick(QVariantList)));
    QObject::connect(upNextListViewProvider, SIGNAL(channelActionItemClick(QVariantList)), this,
            SLOT(onChannelActionItemClick(QVariantList)));
    QObject::connect(upNextListViewProvider, SIGNAL(playAudioOnlyActionItemClick(QVariantList)),
            this, SLOT(onPlayAudioOnlyActionItemClick(QVariantList)));

    bb::cascades::TrackpadHandler *progressSliderTrackpadHandler =
            new bb::cascades::TrackpadHandler();
    QObject::connect(progressSliderTrackpadHandler, SIGNAL(trackpad(bb::cascades::TrackpadEvent*)),
            this, SLOT(onProgressSliderTrackpadEvent(bb::cascades::TrackpadEvent*)));
    bb::cascades::TrackpadHandler *closeButtonTrackpadHandler = new bb::cascades::TrackpadHandler();
    QObject::connect(closeButtonTrackpadHandler, SIGNAL(trackpad(bb::cascades::TrackpadEvent*)),
            this, SLOT(onCloseButtonTrackpadEvent(bb::cascades::TrackpadEvent*)));
    bb::cascades::DoubleTapHandler *doubleTapHandler =
            bb::cascades::DoubleTapHandler::create().onDoubleTapped(this,
                    SLOT(onDoubleTappedHandler(bb::cascades::DoubleTapEvent*)));

    progressSlider->addGestureHandler(doubleTapHandler);
    progressSlider->addEventHandler(progressSliderTrackpadHandler);
    closeButton->addEventHandler(closeButtonTrackpadHandler);

    bb::cascades::Shortcut *loopScalingShortcut = bb::cascades::Shortcut::create().key("f");
    bb::cascades::Shortcut *audioOnlyShortcut = bb::cascades::Shortcut::create().key("a");
    bb::cascades::Shortcut *videoShortcut = bb::cascades::Shortcut::create().key("v");
    bb::cascades::Shortcut *forward10Shortcut = bb::cascades::Shortcut::create().key("j");
    bb::cascades::Shortcut *backward10Shortcut = bb::cascades::Shortcut::create().key("g");
    this->addShortcut(loopScalingShortcut);
    this->addShortcut(audioOnlyShortcut);
    this->addShortcut(videoShortcut);
    this->addShortcut(forward10Shortcut);
    this->addShortcut(backward10Shortcut);
    QObject::connect(loopScalingShortcut, SIGNAL(triggered()), this, SLOT(onLoopScalingShortcut()));
    QObject::connect(audioOnlyShortcut, SIGNAL(triggered()), this, SLOT(onAudioOnlyShortcut()));
    QObject::connect(videoShortcut, SIGNAL(triggered()), this, SLOT(onVideoShortcut()));
    QObject::connect(forward10Shortcut, SIGNAL(triggered()), this, SLOT(onForward10ButtonClick()));
    QObject::connect(backward10Shortcut, SIGNAL(triggered()), this,
            SLOT(onBackward10ButtonClick()));

    QObject::connect(PlaylistVideoProxy::getInstance(), SIGNAL(added(PlaylistVideoModel*)), this,
            SLOT(onPlaylistVideoAdded(PlaylistVideoModel*)));
    QObject::connect(PlaylistVideoProxy::getInstance(),
            SIGNAL(deleted(QString,PlaylistListItemModel::Type)), this,
            SLOT(onPlaylistVideoDeleted(QString, PlaylistListItemModel::Type)));
    QObject::connect(PlaylistVideoProxy::getInstance(),
            SIGNAL(deletedAll(PlaylistListItemModel::Type)), this,
            SLOT(onPlaylistVideoDeletedAll(PlaylistListItemModel::Type)));

    playVideo();
}

void PlayerPage::onForeignWindowBoundingChanged(bool isBound)
{
    foreignWindowControl->setVisible(isBound);

    if (isBound) {
        playerContext->setWindowData(foreignWindowControl->windowHandle(),
                foreignWindowControl->windowId(), foreignWindowControl->windowGroup());
    } else {
        playerContext->resetWindowHandle();
    }
}

void PlayerPage::onCcForeignWindowBoundingChanged(bool isBound)
{
    ccForeignWindowControl->setVisible(isBound);

    if (isBound) {
        playerContext->setCcWindowData(ccForeignWindowControl->windowHandle(),
                ccForeignWindowControl->windowId(), ccForeignWindowControl->windowGroup());
    } else {
        playerContext->resetCcWindowHandle();
    }
}

void PlayerPage::onOrientationChanged()
{
    adjustInfoScreen();
    playerContext->resizeFullScreenVideo();
}

void PlayerPage::onTouch(bb::cascades::TouchEvent *e)
{
    if (QObject::sender() == root
            && (actionBarsVisible
                    || e->propagationPhase() == bb::cascades::PropagationPhase::Bubbling)) {
        return;
    }

    if (e->touchType() != bb::cascades::TouchType::Up) {
        //|| e->propagationPhase() == bb::cascades::PropagationPhase::Bubbling) {
        return;
    }

    toggleInfosVisibility();
}

void PlayerPage::toggleInfosVisibility()
{
    actionBarsVisible = !actionBarsVisible;
    if (actionBarsVisible) {
        showInfos();
    } else {
        hideInfos();
    }
}

void PlayerPage::showInfos()
{
    actionBarsVisible = true;
    infoContainer->setVisible(true);
    this->setActionBarVisibility(bb::cascades::ChromeVisibility::Overlay);
}

void PlayerPage::hideInfos()
{
    actionBarsVisible = false;
    if (progressSlider->isEnabled()) {
        progressSlider->requestFocus();
    } else {
        closeButton->requestFocus();
    }
    infoContainer->setVisible(false);
    this->setActionBarVisibility(bb::cascades::ChromeVisibility::Hidden);
}

void PlayerPage::onCloseInfoContainerClick()
{
    hideInfos();
}

void PlayerPage::onBackward10ButtonClick()
{
    if (progressSlider->isEnabled()) {
        playerContext->skipXSecondsBackward(10);
    }
}

void PlayerPage::onForward10ButtonClick()
{
    if (progressSlider->isEnabled()) {
        playerContext->skipXSecondsForward(10);
    }
}

void PlayerPage::onMediaStateChanged(bb::multimedia::MediaState::Type state)
{
    switch (state) {
        case bb::multimedia::MediaState::Paused:
        case bb::multimedia::MediaState::Stopped:
            playActionItem->setTitle("Play");
            playActionItem->setImageSource(QString("asset:///images/ic_play.png"));
            break;
        case bb::multimedia::MediaState::Started:
            playActionItem->setTitle("Pause");
            playActionItem->setImageSource(QString("asset:///images/ic_pause.png"));
            break;
    }

    if (state == bb::multimedia::MediaState::Stopped) {
        // mmrenderer reports the same "Stopped" state both when playback
        // genuinely reaches the end of the file and when it stops for any
        // other reason (user pressed pause/the play-pause button, a
        // transient playback error, etc). Only treat this as "video
        // finished" -- and therefore auto-advance/replay -- when the
        // reported position is actually close to the known duration.
        // Otherwise leave state handling to whoever caused the stop (e.g.
        // onPlayActionItemClick already updates the button for a user
        // pause) so a pause/hiccup mid-video doesn't jump to another video.
        unsigned int positionMs = playerContext->getPosition();
        bool reachedEnd = !isLiveStream && duration > 0 && !playingPartialRemux
                && positionMs >= (unsigned int) (duration * 1000 - 1500);

        if (playingPartialRemux && !isLiveStream && !remuxSwapping && !seekPending) {
            // "Stopped" near the end of a partial file == the player ran out
            // of downloaded data. The player's own position is unreliable
            // right at the end, so use the last tick as well.
            unsigned int seenMs = positionMs > lastPositionMs ? positionMs : lastPositionMs;
            qDebug() << "[bbtube][chunk] player stopped at" << positionMs / 1000.0 << "s (last tick"
                     << lastPositionMs / 1000.0 << "s), file ends at" << remuxPlayableSeconds
                     << "s, next file ready:" << (pendingSwapPath.isEmpty() ? "no" : "yes");
            if (seenMs / 1000.0 >= remuxPlayableSeconds - 4.0) {
                remuxRanDry = true;
                if (!pendingSwapPath.isEmpty()) {
                    swapToPendingFile();
                } else if (!remuxStallToastShown) {
                    remuxStallToastShown = true;
                    UIUtils::toastInfo("Buffering...");
                    maybeRequestRemuxMerge();
                }
            }
        }

        if (reachedEnd) {
            if (appSettings->isAutoplay() || playerContext->getPlaylistId() > 0) {
                onNextActionItemClick();
            } else {
                showInfos();
                progressSlider->setValue(0);
            }
        }
    }
}

void PlayerPage::onPlayActionItemClick()
{
    switch (playerContext->getMediaState()) {
        case bb::multimedia::MediaState::Paused:
        case bb::multimedia::MediaState::Stopped:
        case bb::multimedia::MediaState::Prepared: {
            bb::multimedia::MediaError::Type error = playerContext->play();

            if (error == bb::multimedia::MediaError::None) {
                playerContext->acquire();
                playActionItem->setTitle("Pause");
                playActionItem->setImageSource(QString("asset:///images/ic_pause.png"));
            } else {
                UIUtils::toastError("Source unavailable");
            }
        }
            break;
        case bb::multimedia::MediaState::Started:
            playerContext->pause();
            playActionItem->setTitle("Play");
            playActionItem->setImageSource(QString("asset:///images/ic_play.png"));
            break;
    }
}

void PlayerPage::onNextActionItemClick()
{
    setNextVideoId();
    if (nextVideoId == "") {
        return;
    }

    nextActionItem->setEnabled(false);
    previousActionItem->setEnabled(false);
    upNextListView->setEnabled(false);
    if (playerContext->getMediaState() == bb::multimedia::MediaState::Started) {
        playerContext->pause();
    }
    hideInfos();
    overlay->setVisible(true);
    playVideoByIdOrUrl(nextVideoId);
}

void PlayerPage::onPreviousActionItemClick()
{
    setPrevVideoId();
    if (prevVideoId == "") {
        return;
    }

    watched.removeAt(watched.count() - 1);

    nextActionItem->setEnabled(false);
    previousActionItem->setEnabled(false);
    upNextListView->setEnabled(false);

    hideInfos();
    overlay->setVisible(true);
    playVideoByIdOrUrl(prevVideoId);
}

void PlayerPage::onMetadataReceived(VideoMetadata videoMetadata, StorageData storageData)
{
    if (storageData.instances.count() == 0) {
        UIUtils::toastError("Source unavailable");
        previousActionItem->setEnabled(true);
        nextActionItem->setEnabled(true);
    } else {
        this->alreadyPlaying = false;
        this->videoMetadata = videoMetadata;
        this->storageData = storageData;
        playerContext->setMetadata(videoMetadata, storageData, audioOnly);

        playVideo();
    }

    overlay->setVisible(false);
    upNextListView->setEnabled(true);
}

void PlayerPage::playVideo()
{
    if (!alreadyPlaying) {
        // A new video (e.g. next in the playlist): stop downloading the previous one.
        clearRemuxSessions();
        pendingRemuxQualityLabel = "";
    }
    setInfos();

    if (storageData.instances.count() > 0) {
        duration = storageData.instances[0].duration;
        isLiveStream = duration == 0;

        QString url;
        SingleVideoStorageData data;

        if (isLiveStream || !audioOnly || storageData.audio.url == "") {
            if (alreadyPlaying) {
                data = playerContext->getSelectedStorageData();
            } else {
                data = storageData.instances[getIndexOfDefaultQuality()];
                playerContext->setSelectedStorageData(data);
            }

            setAudioOnly(false);
            url = data.url;
            quality = isLiveStream ? "LIVE" : data.quality;
            setProgressSliderEnabled(!isLiveStream);
        } else {
            url = storageData.audio.url;
            quality = "Audio";
            setProgressSliderEnabled(false);
        }

        if (isLiveStream) {
            qualityActionItem->setEnabled(false);

        } else {
            qualityActionItem->setEnabled(
                    storageData.instances.count() > 1 || storageData.audio.url != "");
        }
        if (progressSlider->isEnabled()) {
            progressSlider->requestFocus();
        } else {
            closeButton->requestFocus();
        }

        qualityActionItem->setTitle(quality);
        scalingActionItem->setEnabled(!audioOnly);
        downloadActionItem->setEnabled(!isLiveStream);
        timecodeActionItem->setEnabled(
                progressSlider->isEnabled() && videoMetadata.timecodes.count() > 0);
        passedTime->setVisible(!isLiveStream);
        remainingTime->setVisible(!isLiveStream);
        progressSlider->setToValue(duration);
        audioBackground->setVisible(audioOnly);
        updateRepeatActionItem();
        ccActionItem->setEnabled(storageData.captions.count() > 0);

        if (alreadyPlaying) {
            onMediaStateChanged(playerContext->getMediaState());
            progressSlider->setValue(playerContext->getPosition());

            if (playerContext->getCcLanguage() != "") {
                for (int i = 0; i < storageData.captions.count(); i++) {
                    if (storageData.captions[i].languageCode == playerContext->getCcLanguage()) {
                        ccActionItem->setTitle(storageData.captions[i].languageName);
                        playerContext->setClosedCaptionUrl(
                                getCcPath(storageData.captions[i].languageCode));

                        break;
                    }
                }
            } else {
                playerContext->resetClosedCaptionUrl();
            }
        } else {
            ccActionItem->setTitle("Off");
            playerContext->resetClosedCaptionUrl();
            playerContext->setCcLanguage("");

            if (!isLiveStream && !data.hasEmbeddedAudio && storageData.audio.url != "") {
                // Adaptive video-only stream (e.g. 720p/1080p): BB10's
                // mmrenderer can't play this and a separate audio track at
                // the same time, so remux them into one local file first.
                // startPlaybackAt() runs once the remux head is ready
                // (StreamingRemuxSession::headReady) instead of here.
                playVideoWithRemux(data);
            } else {
                startPlaybackAt(url);
            }
        }
    }
}

void PlayerPage::startPlaybackAt(QString url, int offsetMs, bool partial)
{
    playingPartialRemux = partial;
    playingStartSec = offsetMs / 1000.0;
    lastPositionMs = (unsigned int) offsetMs;
    bb::multimedia::MediaError::Type error = playerContext->play(url);
    if (error == bb::multimedia::MediaError::None) {
        // A remuxed window of the video: positions are video time.
        bool windowed = partial || offsetMs > 0;
        playerContext->setTimeline(offsetMs, windowed ? duration : 0);
        playerContext->setPartialSource(partial);
        playerContext->setSeekIntercept(windowed);

        addToHistory();

        if (watched.count() == 0 || watched.last() != videoMetadata.video.videoId) {
            watched.append(videoMetadata.video.videoId);

            setPrevVideoId();
        }

        if (playerContext->isContinuePlaying() && progressSlider->isEnabled()) {
            playerContext->seekTime(
                    (0.0f
                            + VideoViewedPercentProxy::getInstance()->getPercent(
                                    videoMetadata.video.videoId)) * duration / 10000);
            playerContext->setContinuePlaying(false);
        }
    } else {
        qDebug() << error << url;
        UIUtils::toastError("Source unavailable");
    }
}

// How early (seconds before the loaded file runs out) the next, longer merged
// file is requested. It grows with how long the last merge took.
static const double REMUX_SWAP_LEAD_BASE_SECONDS = 4.0;
// A quality switch only takes over once the new file reaches this far past
// the current position (so the switch doesn't immediately run dry).
static const double REMUX_QUALITY_SWITCH_AHEAD_SECONDS = 8.0;
// A seek into stored material loads a file that starts at most this far
// before the target (keeps merged files, and therefore loading, small).
static const double REMUX_SEEK_FILE_BACK_SECONDS = 60.0;

void PlayerPage::playVideoWithRemux(SingleVideoStorageData videoData)
{
    pendingRemuxQualityLabel = ""; // "" == this is an initial playback, not a mid-playback quality switch
    double startAt = 0;
    if (playerContext->isContinuePlaying() && duration > 0) {
        // Resume where the person stopped: download from there, not from 0:00.
        startAt = (0.0
                + VideoViewedPercentProxy::getInstance()->getPercent(videoMetadata.video.videoId))
                * duration / 10000.0 / 1000.0;
        if (startAt < 0) startAt = 0;
    }
    startRemuxSession(videoData, startAt);
}

void PlayerPage::changeQualityWithRemux(QString newQuality, SingleVideoStorageData videoData)
{
    pendingRemuxQualityLabel = newQuality;
    startRemuxSession(videoData, playerContext->getPosition() / 1000.0);
}

ChunkedRemuxSession *PlayerPage::playingSession()
{
    // While a quality switch is under way the loaded file still comes from the old session.
    return retiringRemuxSession ? retiringRemuxSession : remuxSession;
}

void PlayerPage::showSeekLoading(bool show)
{
    if (!seekLoadingOverlay) return;
    seekLoadingOverlay->setVisible(show);
    if (show) seekLoadingSpinner->start(); else seekLoadingSpinner->stop();
}

void PlayerPage::clearRemuxSessions()
{
    // cancel() has to run synchronously before deleteLater(): it aborts the
    // in-flight replies and disconnects them from the session, so no slot
    // can fire into a half-destroyed object.
    if (remuxSession) {
        QObject::disconnect(remuxSession, 0, this, 0);
        remuxSession->cancel();
        remuxSession->deleteLater();
        remuxSession = 0;
    }
    if (retiringRemuxSession) {
        QObject::disconnect(retiringRemuxSession, 0, this, 0);
        retiringRemuxSession->cancel();
        retiringRemuxSession->deleteLater();
        retiringRemuxSession = 0;
    }
    if (remuxWatchdog) remuxWatchdog->stop();
    remuxPlaybackStarted = false;
    playingStartChunk = 0;
    playingChunkCount = 0;
    playingStartSec = 0;
    remuxPlayableSeconds = 0;
    playingPartialRemux = false;
    remuxStallToastShown = false;
    pendingSwapPath = "";
    pendingSwapSession = 0;
    remuxRanDry = false;
    remuxSwapping = false;
    finalCopyRequested = false;
    initialStartChunk = -1;
    seekPending = false;
    seekMergeRequested = false;
    showSeekLoading(false);
}

// Drops merged/staging files nobody will need again (this app never cleaned
// remux_cache before; a single video is tens of MB).
static void cleanupRemuxCache(const QString &dirPath)
{
    QDir dir(dirPath);
    QFileInfoList files = dir.entryInfoList(QDir::Files);
    QDateTime cutoff = QDateTime::currentDateTime().addDays(-1);
    for (int i = 0; i < files.size(); i++) {
        if (files[i].lastModified() < cutoff) {
            QFile::remove(files[i].absoluteFilePath());
        }
    }
}

void PlayerPage::startRemuxSession(SingleVideoStorageData videoData, double startAtSeconds)
{
    QString base = remuxBaseNameFor(videoMetadata.video.videoId, videoData.quality);
    QString cacheDir = remuxCacheDir();
    cleanupRemuxCache(cacheDir);

    if (pendingRemuxQualityLabel != "" && remuxSession && !remuxSession->hasFailed()
            && !remuxSession->isDownloadComplete() && playingPartialRemux) {
        // Mid-playback quality switch while the current quality's file is
        // still growing: keep that session alive (it feeds the file the
        // player is reading) until the new quality's first file takes over.
        if (retiringRemuxSession) {
            QObject::disconnect(retiringRemuxSession, 0, this, 0);
            retiringRemuxSession->cancel();
            retiringRemuxSession->deleteLater();
        }
        retiringRemuxSession = remuxSession; // stays connected: its merged files keep the old quality playing
        remuxSession = 0;
    } else {
        // Initial playback / new video, or the old file is already complete.
        bool keepPlayingState = (pendingRemuxQualityLabel != "");
        int s0 = playingStartChunk, c0 = playingChunkCount;
        double st0 = playingStartSec, pl0 = remuxPlayableSeconds;
        bool part0 = playingPartialRemux, started0 = remuxPlaybackStarted;
        clearRemuxSessions();
        if (keepPlayingState) {
            playingStartChunk = s0;
            playingChunkCount = c0;
            playingStartSec = st0;
            remuxPlayableSeconds = pl0;
            playingPartialRemux = part0;
            remuxPlaybackStarted = started0;
        }
    }

    remuxStartAtSec = startAtSeconds;
    initialStartChunk = -1;
    remuxSession = new ChunkedRemuxSession(ApplicationUI::networkManager, videoData.url,
            storageData.audio.url, cacheDir, base, this, 5.0, startAtSeconds);

    QObject::connect(remuxSession, SIGNAL(progress(int)), this, SLOT(onRemuxProgress(int)));
    QObject::connect(remuxSession, SIGNAL(mergedReady(QString, int, int, bool)), this,
            SLOT(onRemuxMerged(QString, int, int, bool)));
    QObject::connect(remuxSession, SIGNAL(failed(QString)), this, SLOT(onRemuxFailed(QString)));

    remuxSession->start();
}

void PlayerPage::markMergeRequested()
{
    remuxMergeClock.start();
    remuxMergeTimed = true;
}

// Decides whether it is time to ask for a (newer) merged file.
//
// A newer file for the CURRENT playback is only ever swapped in when the
// player has run out of the one it has (see swapToPendingFile()), so it has
// to exist by then: it is requested while the player is still `lead` seconds
// from the end, where `lead` grows with how long the last merge took. Exactly
// one merge is made per swap -- earlier versions re-merged after every
// downloaded chunk (copying tens of MB each time) while one was already
// waiting. The very first file is requested as soon as its chunk is stored.
void PlayerPage::maybeRequestRemuxMerge()
{
    if (seekPending) return; // seek handling drives merging while a seek is outstanding

    unsigned int posMs = (unsigned int) playerContext->getPosition();
    if (lastPositionMs > posMs) posMs = lastPositionMs;
    double posSec = posMs / 1000.0;
    double lead = REMUX_SWAP_LEAD_BASE_SECONDS + 2.0 * remuxLastMergeSeconds;
    if (lead > 30.0) lead = 30.0;

    // The file in the player: extend it when it is about to run out.
    ChunkedRemuxSession *ps = playingSession();
    if (ps && remuxPlaybackStarted && playingPartialRemux && pendingSwapPath.isEmpty()
            && !ps->isMerging() && ps->headsReady()) {
        int end = playingStartChunk + playingChunkCount;
        if (ps->runEnd(playingStartChunk) > end && remuxPlayableSeconds - posSec < lead) {
            markMergeRequested();
            ps->requestMerge(playingStartChunk);
        }
    }

    if (!remuxSession || !remuxSession->headsReady()) return;
    if (initialStartChunk < 0) initialStartChunk = remuxSession->chunkIndexAt(remuxStartAtSec);

    if (pendingRemuxQualityLabel != "") {
        // Quality switch: wait until the new file reaches well past the
        // current position, or is complete.
        if (remuxSession->isMerging() || !remuxSession->isChunkStored(initialStartChunk)) return;
        int end = remuxSession->runEnd(initialStartChunk);
        double coveredEnd = remuxSession->chunkEndSeconds(end - 1);
        if (end == remuxSession->chunkCount()
                || coveredEnd >= posSec + REMUX_QUALITY_SWITCH_AHEAD_SECONDS + 2.0) {
            markMergeRequested();
            remuxSession->requestMerge(initialStartChunk);
        }
        return;
    }
    if (!remuxPlaybackStarted) {
        if (!remuxSession->isMerging() && remuxSession->isChunkStored(initialStartChunk)) {
            markMergeRequested();
            remuxSession->requestMerge(initialStartChunk);
        }
        return;
    }
    maybeRequestFinalCopy();
}

// Once the whole video is stored and the person is playing a file that does not
// start at 0:00, still produce the complete cached file for next time.
void PlayerPage::maybeRequestFinalCopy()
{
    if (finalCopyRequested || !remuxSession || seekPending || remuxSession->isMerging()) return;
    if (!remuxSession->isDownloadComplete() || remuxSession != playingSession()) return;
    if (playingStartChunk == 0 && !playingPartialRemux) return; // already playing the final file
    finalCopyRequested = true;
    remuxSession->requestMerge(0);
}

void PlayerPage::onRemuxProgress(int chunkIndex)
{
    Q_UNUSED(chunkIndex);
    if (seekPending) {
        if (!seekMergeRequested && remuxSession && remuxSession->isChunkStored(seekChunk)) {
            requestSeekMerge();
        }
        return;
    }
    maybeRequestRemuxMerge();
}

void PlayerPage::requestSeekMerge()
{
    if (!remuxSession || seekChunk < 0) return;
    seekMergeRequested = true;
    int a = remuxSession->fileStartFor(seekChunk, REMUX_SEEK_FILE_BACK_SECONDS);
    qDebug() << "[bbtube][chunk] seek: chunk" << seekChunk << "is stored, merging file starting at chunk" << a;
    markMergeRequested();
    remuxSession->requestMerge(a);
}

// Hands a merged file to the player.
//   resumeFileSeconds: where to resume, in the file's own timeline
//   resumeAbsSeconds:  the same instant in video time (ignored for cached final files)
void PlayerPage::loadMergedFile(QString label, QString path, ChunkedRemuxSession *session,
        int startChunk, int chunkCount, bool isFinal, double resumeFileSeconds,
        double resumeAbsSeconds, bool forcePlay)
{
    bool reachesEnd = isFinal || chunkCount < 0
            || (session && startChunk + chunkCount >= session->chunkCount());
    double startSec = (chunkCount < 0 || !session) ? 0.0 : session->chunkStartSeconds(startChunk);
    double endSec = reachesEnd ? 1e9 : session->chunkEndSeconds(startChunk + chunkCount - 1);

    playingStartChunk = startChunk;
    playingChunkCount = chunkCount;
    playingStartSec = startSec;
    remuxPlayableSeconds = endSec;
    playingPartialRemux = !reachesEnd;
    bool windowed = playingPartialRemux || startSec > 0.01;

    int offsetMs = (int) (startSec * 1000.0 + 0.5);
    int absMs = (int) ((startSec + resumeFileSeconds) * 1000.0 + 0.5);
    if (resumeAbsSeconds >= 0) absMs = (int) (resumeAbsSeconds * 1000.0 + 0.5);

    remuxSwapping = true;
    playerContext->setTimeline(offsetMs, windowed ? duration : 0);
    playerContext->setPartialSource(playingPartialRemux);
    playerContext->setSeekIntercept(windowed);
    changeQuality(label, path, forcePlay, absMs);
    remuxSwapping = false;

    remuxStallToastShown = false;
    remuxRanDry = false;
    lastPositionMs = (unsigned int) absMs;
    lastPositionTickClock.start();
    if (windowed && !remuxWatchdog->isActive()) remuxWatchdog->start(1000);
    qDebug() << "[bbtube][chunk] loaded" << path << ": chunks" << startChunk << "+" << chunkCount
             << ", file starts at video" << startSec << "s, ends at" << (reachesEnd ? -1.0 : endSec)
             << "s, resuming at" << absMs / 1000.0 << "s, forcePlay" << forcePlay;
}

// A longer file is ready. Hand it to the player right away only if the
// player is already out of data; otherwise keep it until it is.
void PlayerPage::queuePendingSwap(ChunkedRemuxSession *session, QString path, int startChunk,
        int chunkCount, bool isFinal)
{
    int curEnd = playingStartChunk + playingChunkCount;
    // Only a file that begins at or before the current one and reaches further is a continuation.
    if (startChunk > playingStartChunk || startChunk + chunkCount <= curEnd) {
        qDebug() << "[bbtube][chunk] ignoring merged file chunks" << startChunk << "+" << chunkCount
                 << "- not an extension of" << playingStartChunk << "+" << playingChunkCount;
        return;
    }
    pendingSwapPath = path;
    pendingSwapStart = startChunk;
    pendingSwapCount = chunkCount;
    pendingSwapFinal = isFinal;
    pendingSwapSession = session;

    double posSec = lastPositionMs / 1000.0;
    bool started = playerContext->getMediaState() == bb::multimedia::MediaState::Started;
    if (remuxRanDry || (started && remuxPlayableSeconds - posSec < 0.35)) {
        swapToPendingFile();
    } else {
        qDebug() << "[bbtube][chunk] next file ready (chunks" << startChunk << "+" << chunkCount
                 << "), waiting for the player to reach the end of the current one at"
                 << remuxPlayableSeconds << "s; player at" << posSec << "s";
    }
}

void PlayerPage::swapToPendingFile()
{
    if (pendingSwapPath.isEmpty() || pendingSwapSession.isNull() || remuxSwapping) return;
    QString path = pendingSwapPath;
    int start = pendingSwapStart, count = pendingSwapCount;
    bool isFinal = pendingSwapFinal;
    ChunkedRemuxSession *session = pendingSwapSession;
    pendingSwapPath = "";
    pendingSwapSession = 0;

    // The old file ended exactly at a chunk boundary == a keyframe of the new
    // file. Resume there (not at the player's own idea of the position, which
    // a mid-GOP seek would turn into a rewind to the previous keyframe).
    int boundary = playingStartChunk + playingChunkCount;
    double resume = session->fileResumeSeconds(start, boundary);
    qDebug() << "[bbtube][chunk] swapping: old file ended at" << remuxPlayableSeconds
             << "s (player at" << lastPositionMs / 1000.0 << "s), boundary chunk" << boundary;
    loadMergedFile(quality, path, session, start, count, isFinal, resume, -1.0, true);

    maybeRequestRemuxMerge();
}

void PlayerPage::onRemuxMerged(QString path, int startChunk, int chunkCount, bool isFinal)
{
    ChunkedRemuxSession *src = qobject_cast<ChunkedRemuxSession *>(QObject::sender());

    if (remuxMergeTimed) {
        remuxLastMergeSeconds = remuxMergeClock.elapsed() / 1000.0;
        remuxMergeTimed = false;
    }

    // A longer file of the quality that is still playing (during a quality switch).
    if (src && src == retiringRemuxSession) {
        if (!seekPending) queuePendingSwap(src, path, startChunk, chunkCount, isFinal);
        return;
    }
    if (!remuxSession) return;

    double posSec = playerContext->getPosition() / 1000.0;

    // ---- the person seeked somewhere the loaded file does not contain ----
    if (seekPending && chunkCount >= 0) {
        double T = seekTargetMs / 1000.0;
        double fs = remuxSession->chunkStartSeconds(startChunk);
        bool reachesEnd = isFinal || startChunk + chunkCount >= remuxSession->chunkCount();
        double fe = reachesEnd ? 1e9 : remuxSession->chunkEndSeconds(startChunk + chunkCount - 1);
        if (T >= fs - 0.001 && T < fe - 0.5) {
            QString label = pendingRemuxQualityLabel != "" ? pendingRemuxQualityLabel : quality;
            pendingRemuxQualityLabel = "";
            pendingSwapPath = "";
            pendingSwapSession = 0;
            remuxPlaybackStarted = true;
            bool resumePlay = seekResumePlay;
            seekPending = false;
            seekMergeRequested = false;
            showSeekLoading(false);
            loadMergedFile(label, path, remuxSession, startChunk, chunkCount, isFinal, T - fs, T,
                    resumePlay);
            if (retiringRemuxSession) {
                QObject::disconnect(retiringRemuxSession, 0, this, 0);
                retiringRemuxSession->cancel();
                retiringRemuxSession->deleteLater();
                retiringRemuxSession = 0;
            }
            progressSlider->setValue((int) seekTargetMs);
            maybeRequestRemuxMerge();
        } else {
            // Stale file (built before the target chunk was stored, or the
            // target sits right at its end): ask again once more data arrived.
            qDebug() << "[bbtube][chunk] merged file does not cover the seek target" << T << "s";
            seekMergeRequested = false;
            if (remuxSession->isChunkStored(seekChunk)) requestSeekMerge();
        }
        return;
    }

    if (pendingRemuxQualityLabel != "") {
        // ---- quality switch: the new quality's first file ----
        double fs = chunkCount < 0 ? 0.0 : remuxSession->chunkStartSeconds(startChunk);
        bool reachesEnd = isFinal || chunkCount < 0
                || startChunk + chunkCount >= remuxSession->chunkCount();
        double fe = reachesEnd ? 1e9 : remuxSession->chunkEndSeconds(startChunk + chunkCount - 1);
        if (!reachesEnd && fe < posSec + 4.0) {
            // The player outran this file while it was being merged; wait for a longer one.
            maybeRequestRemuxMerge();
            return;
        }
        QString newQuality = pendingRemuxQualityLabel;
        pendingRemuxQualityLabel = "";
        // A different quality is a different file with its own keyframes:
        // resume on the keyframe nearest to where the reload will finish.
        double target = posSec + 1.5;
        double resumeFile = 0.0, resumeAbs = -1.0;
        if (chunkCount >= 0) {
            int b = remuxSession->chunkIndexAt(target);
            if (b + 1 < startChunk + chunkCount
                    && remuxSession->chunkEndSeconds(b) - target < target - remuxSession->chunkStartSeconds(b)) {
                b++; // the next boundary is closer
            }
            if (b < startChunk) b = startChunk;
            if (b >= startChunk + chunkCount) b = startChunk + chunkCount - 1;
            resumeFile = remuxSession->fileResumeSeconds(startChunk, b);
            (void) fs;
        } else {
            resumeAbs = target; // complete cached file: plain position
        }
        pendingSwapPath = "";
        pendingSwapSession = 0;
        remuxPlaybackStarted = true;
        loadMergedFile(newQuality, path, remuxSession, startChunk, chunkCount, isFinal, resumeFile,
                resumeAbs, false);
        if (retiringRemuxSession) {
            QObject::disconnect(retiringRemuxSession, 0, this, 0);
            retiringRemuxSession->cancel();
            retiringRemuxSession->deleteLater();
            retiringRemuxSession = 0;
        }
    } else if (!remuxPlaybackStarted) {
        // ---- the very first file ----
        remuxPlaybackStarted = true;
        bool reachesEnd = isFinal || chunkCount < 0
                || startChunk + chunkCount >= remuxSession->chunkCount();
        double startSec = chunkCount < 0 ? 0.0 : remuxSession->chunkStartSeconds(startChunk);
        double endSec = reachesEnd ? 1e9 : remuxSession->chunkEndSeconds(startChunk + chunkCount - 1);
        playingStartChunk = startChunk;
        playingChunkCount = chunkCount;
        playingStartSec = startSec;
        remuxPlayableSeconds = endSec;
        playingPartialRemux = !reachesEnd;
        startPlaybackAt(path, (int) (startSec * 1000.0 + 0.5), playingPartialRemux);
        remuxStallToastShown = false;
        remuxRanDry = false;
        lastPositionTickClock.start();
        if ((playingPartialRemux || startSec > 0.01) && !remuxWatchdog->isActive()) remuxWatchdog->start(1000);
        qDebug() << "[bbtube][chunk] first file loaded:" << path << "chunks" << startChunk << "+" << chunkCount
                 << ", video time" << startSec << "to" << (reachesEnd ? -1.0 : endSec) << "s";
    } else if (playingPartialRemux) {
        queuePendingSwap(src, path, startChunk, chunkCount, isFinal);
    } else {
        qDebug() << "[bbtube][chunk] merged file ready" << path << "(not needed for playback)";
    }

    // The player may already be close to the end of this file (slow link).
    maybeRequestRemuxMerge();
}

void PlayerPage::onRemuxFailed(QString errorMessage)
{
    qDebug() << "[bbtube][chunk] failed:" << errorMessage;
    if (retiringRemuxSession && QObject::sender() == retiringRemuxSession) {
        return; // old quality stops growing; whatever it already merged keeps playing
    }
    if (!remuxPlaybackStarted) {
        // Nothing has played yet -- there is nothing to fall back on.
        UIUtils::toastError("Source unavailable");
    } else {
        // Something is already playing; keep whatever was downloaded so far
        // watchable. Make one last merge of everything that arrived.
        UIUtils::toastError("Download interrupted");
        if (seekPending) {
            seekPending = false;
            seekMergeRequested = false;
            showSeekLoading(false);
            if (seekResumePlay) playerContext->play();
        }
        if (remuxSession && playingPartialRemux) {
            remuxSession->requestMerge(playingStartChunk);
        }
    }
    pendingRemuxQualityLabel = "";
    if (retiringRemuxSession) {
        QObject::disconnect(retiringRemuxSession, 0, this, 0);
        retiringRemuxSession->cancel();
        retiringRemuxSession->deleteLater();
        retiringRemuxSession = 0;
    }
}

// ---------------------------------------------------------------------------
// Seeking
// ---------------------------------------------------------------------------
void PlayerPage::onSeekRequested(unsigned int positionMs)
{
    seekWithinPlayable(positionMs);
}

// A seek inside the loaded file is a plain seek. Anything else -- later than
// what is downloaded, or in a part the loaded file does not contain -- moves
// the download head there, shows a loading indicator, and plays from there as
// soon as a file covering it exists. Everything downloaded earlier is kept.
void PlayerPage::seekWithinPlayable(unsigned int positionMs)
{
    double t = positionMs / 1000.0;
    bool windowed = playingPartialRemux || playingStartSec > 0.01;
    ChunkedRemuxSession *s = remuxSession;
    if (!windowed || !s || !s->headsReady() || isLiveStream) {
        playerContext->seekTimeDirect(positionMs);
        return;
    }
    if (t >= playingStartSec && t <= remuxPlayableSeconds - 1.5 && !seekPending) {
        playerContext->seekTimeDirect(positionMs);
        return;
    }
    beginRemuxSeek(positionMs);
}

void PlayerPage::beginRemuxSeek(unsigned int positionMs)
{
    ChunkedRemuxSession *s = remuxSession;
    double t = positionMs / 1000.0;
    if (t * 1000.0 > duration) t = duration / 1000.0;

    if (!seekPending) {
        bb::multimedia::MediaState::Type st = playerContext->getMediaState();
        seekResumePlay = (st == bb::multimedia::MediaState::Started) || remuxRanDry;
        playerContext->pause(); // freezes the old file: no more position ticks while loading
    }
    seekPending = true;
    seekMergeRequested = false;
    seekTargetMs = (unsigned int) (t * 1000.0);
    pendingSwapPath = "";
    pendingSwapSession = 0;
    remuxRanDry = false;
    showSeekLoading(true);
    if (!manualSeeking) progressSlider->setValue((int) seekTargetMs);

    seekChunk = s->seekTo(t);
    qDebug() << "[bbtube][chunk] seek requested to" << t << "s -> chunk" << seekChunk
             << (s->isChunkStored(seekChunk) ? "(already stored)" : "(downloading from there)");
    if (s->isChunkStored(seekChunk)) {
        requestSeekMerge();
    }
}

// Once a second while a remux file is playing: catches a player that has
// stopped (or frozen) at the end of its file without the state change we
// normally react to, so the next file is still swapped in.
void PlayerPage::onRemuxWatchdog()
{
    if (!(playingPartialRemux) || seekPending || remuxSwapping || !remuxPlaybackStarted) return;

    bb::multimedia::MediaState::Type st = playerContext->getMediaState();
    unsigned int posMs = (unsigned int) playerContext->getPosition();
    if (lastPositionMs > posMs) posMs = lastPositionMs;
    bool nearEnd = posMs / 1000.0 >= remuxPlayableSeconds - 4.0;
    bool stuck = (st == bb::multimedia::MediaState::Stopped)
            || (st == bb::multimedia::MediaState::Started && lastPositionTickClock.elapsed() > 2500);
    if (!nearEnd || !stuck) return;

    if (!remuxRanDry) {
        remuxRanDry = true;
        qDebug() << "[bbtube][chunk] watchdog: player is stuck at" << posMs / 1000.0 << "s (state" << st
                 << ") near the end of the file at" << remuxPlayableSeconds << "s; pending next file:"
                 << (pendingSwapPath.isEmpty() ? "no" : "yes");
    }
    if (!pendingSwapPath.isEmpty()) {
        swapToPendingFile();
    } else {
        if (!remuxStallToastShown) {
            remuxStallToastShown = true;
            UIUtils::toastInfo("Buffering...");
        }
        maybeRequestRemuxMerge();
    }
}

QString PlayerPage::remuxCacheDir()
{
    QDir dir(QDir::homePath() + "/remux_cache");
    if (!dir.exists()) {
        dir.mkpath(".");
    }
    return dir.absolutePath();
}

QString PlayerPage::remuxBaseNameFor(QString videoId, QString quality)
{
    QString safeQuality = quality;
    safeQuality.replace(QRegExp("[^A-Za-z0-9]"), "_");
    // "_c" (chunked): files written by older builds under the plain
    // <id>_<quality>.mp4 name were pre-allocated and may be incomplete.
    return videoId + "_" + safeQuality + "_c";
}

void PlayerPage::setInfos()
{
    title->setText(videoMetadata.video.title);

    QStringList subtitleParts;
    if (videoMetadata.video.channelTitle != "") {
        subtitleParts.append(videoMetadata.video.channelTitle);
    }
    if (videoMetadata.video.viewsCount != "") {
        subtitleParts.append(videoMetadata.video.viewsCount);
    }
    if (videoMetadata.video.dateUploadedAgo != "") {
        subtitleParts.append(videoMetadata.video.dateUploadedAgo);
    }

    subtitle->setText(subtitleParts.join(" • "));

    UpdatableDataModel<VideoListItemModel*> *model = new UpdatableDataModel<VideoListItemModel*>();
    QList<VideoListItemModel*> relatedWatched;
    QList<VideoListItemModel*> relatedUnwatched;
    QList<VideoListItemModel*> list;
    VideoListItemModel *item;

    if (videoMetadata.relatedVideos.nextVideo.videoId != "") {
        item = VideoListItemModel::mapVideo(&videoMetadata.relatedVideos.nextVideo);

        if (watched.contains(videoMetadata.relatedVideos.nextVideo.videoId)) {
            relatedWatched.append(item);
        } else {
            relatedUnwatched.append(item);
        }
    }

    for (int i = 0; i < videoMetadata.relatedVideos.otherVideos.count(); i++) {
        item = VideoListItemModel::mapVideo(&videoMetadata.relatedVideos.otherVideos[i]);

        if (watched.contains(videoMetadata.relatedVideos.otherVideos[i].videoId)) {
            relatedWatched.append(item);
        } else {
            relatedUnwatched.append(item);
        }
    }

    list.append(relatedUnwatched);
    list.append(relatedWatched);
    model->append(list);

    if (!alreadyPlaying) {
        QVariantMap metadata;
        metadata[bb::multimedia::MetaData::Title] = videoMetadata.video.title;
        metadata[bb::multimedia::MetaData::Artist] = videoMetadata.video.channelTitle;
        playerContext->setNpcMetadata(metadata);
    }

    ActionItemService::setShareVideoText(shareVideoActionItem, videoMetadata.video.title,
            videoMetadata.video.videoId);
    ActionItemService::updateAddToFavoritesActionItem(addToFavoritesActionItem,
            PlaylistVideoProxy::getInstance()->contains(videoMetadata.video.videoId,
                    PlaylistListItemModel::Favorites));
    ActionItemService::updateAddToWatchLaterActionItem(addToWatchLaterActionItem,
            PlaylistVideoProxy::getInstance()->contains(videoMetadata.video.videoId,
                    PlaylistListItemModel::WatchLater));

    upNextListView->setEnabled(true);
    upNextListView->setDataModel(model);

    setPrevVideoId();
    setNextVideoId();
}

void PlayerPage::onPlayerPositionChanged(unsigned int position)
{
    if (isLiveStream) {
        return;
    }
    if (seekPending) {
        return; // the old file is frozen; the slider shows the seek target
    }

    if (!manualSeeking && !trackpadFocusInSlider) {
        progressSlider->setValue(position);
    }

    if (position != lastPositionMs) {
        lastPositionTickClock.start();
    }
    lastPositionMs = position;

    if (remuxSession || retiringRemuxSession) {
        maybeRequestRemuxMerge();
    }
    // Normally the swap happens when the player reports the end of the file
    // (onMediaStateChanged); this catches a player that is still "Started"
    // but about to run out.
    if (playingPartialRemux && !pendingSwapPath.isEmpty() && !remuxSwapping
            && playerContext->getMediaState() == bb::multimedia::MediaState::Started
            && remuxPlayableSeconds - position / 1000.0 < 0.35) {
        swapToPendingFile();
    }
}

void PlayerPage::onProgressSliderValueChanged(float position)
{
    if (isLiveStream) {
        return;
    }

    if (sliderDoubleTap) {
        seekWithinPlayable((unsigned int) position);
        sliderDoubleTap = false;
    } else if (manualSeeking && !trackpadFocusInSlider) {
        seekWithinPlayable((unsigned int) position);
        manualSeeking = false;
    } else if (!trackpadFocusInSlider) {
        manualSeeking = false;
    }
}

void PlayerPage::onProgressSliderImmediateValueChanged(float position)
{
    if (isLiveStream) {
        return;
    }

    if (!trackpadFocusInSlider) {
        manualSeeking = movingInSlider;
    }

    int timeComponentsCount = 0;
    int dur = duration / 1000;
    int durationSecs = dur;
    int intPosition = (int) position / 1000;

    if (duration - position < 1000) {
        intPosition = durationSecs;
    }
    while (dur > 0) {
        timeComponentsCount++;
        dur /= 60;
    }

    if (timeComponentsCount < 2) {
        timeComponentsCount = 2;
    }

    QString passedTimeString;
    QString remainingTimeString;
    int remTime = durationSecs - intPosition;

    for (int i = 0; i < timeComponentsCount; i++) {
        passedTimeString = QString::number(intPosition % 60).rightJustified(2, '0') + ":"
                + passedTimeString;
        remainingTimeString = QString::number(remTime % 60).rightJustified(2, '0') + ":"
                + remainingTimeString;

        intPosition /= 60;
        remTime /= 60;
    }

    passedTimeString.truncate(passedTimeString.length() - 1);
    remainingTimeString.truncate(remainingTimeString.length() - 1);

    passedTime->setText(passedTimeString);
    remainingTime->setText("-" + remainingTimeString);
}

void PlayerPage::onProgressSliderTouch(bb::cascades::TouchEvent * e)
{
    if (!progressSlider->isEnabled()) {
        return;
    }
    if (e->touchType() == bb::cascades::TouchType::Move) {
        movingInSlider = true;
        trackpadFocusInSlider = false;
    } else if (e->touchType() == bb::cascades::TouchType::Up) {
        movingInSlider = false;
    }
}

void PlayerPage::onProgressSliderTrackpadEvent(bb::cascades::TrackpadEvent* e)
{
    if (!actionBarsVisible) {
        showInfos();

        return;
    }

    if (e->trackpadEventType() == bb::cascades::TrackpadEventType::Press) {
        if (!trackpadFocusInSlider) {
            trackpadFocusInSlider = true;
            manualSeeking = true;
        } else {
            trackpadFocusInSlider = false;
            onProgressSliderValueChanged(progressSlider->value());
        }
    }
}

void PlayerPage::onCloseButtonTrackpadEvent(bb::cascades::TrackpadEvent* e)
{
    if (!actionBarsVisible) {
        showInfos();
    }
}

void PlayerPage::onProgressSliderFocusedChanged(bool focused)
{
    if (isLiveStream || !progressSlider->isEnabled()) {
        return;
    }

    if (trackpadFocusInSlider && !focused) {
        trackpadFocusInSlider = false;
        onProgressSliderValueChanged(progressSlider->value());
    }
}

void PlayerPage::onUpnextVideoListItemClick(QVariantList indexPath)
{
    VideoListItemModel *item = upNextListView->dataModel()->data(indexPath).value<
            VideoListItemModel*>();
    hideInfos();
    overlay->setVisible(true);
    upNextListView->setEnabled(false);
    playVideoByIdOrUrl(item->id);
    playerContext->stopPlaylist();
}

void PlayerPage::onQualityActionItemClick()
{
    bb::system::SystemListDialog *videoQualityDialog = new bb::system::SystemListDialog("Change",
            "Cancel");

    videoQualityDialog->setTitle("Quality");
    videoQualityDialog->setBody(
            "NOTE: in case of switching from video to audio the playback will be rewound, fast-forwarding will be disabled.");

    for (int i = 0; i < storageData.instances.count(); i++) {
        QString label = storageData.instances[i].quality;

        if (playerContext->hasKeyboard() && i == getIndexOfDefaultQuality()) {
            label += " (key: V)";
        }

        videoQualityDialog->appendItem(label, true, storageData.instances[i].quality == quality);
    }

    if (storageData.audio.url != "") {
        videoQualityDialog->appendItem(
                QString("Audio only") + (playerContext->hasKeyboard() ? " (key: A)" : ""), true,
                audioOnly);
    }

    bool success = connect(videoQualityDialog, SIGNAL(finished(bb::system::SystemUiResult::Type)),
            this, SLOT(onQualityDialogFinished(bb::system::SystemUiResult::Type)));

    if (success) {
        videoQualityDialog->show();
    } else {
        videoQualityDialog->deleteLater();
    }
}

void PlayerPage::onQualityDialogFinished(bb::system::SystemUiResult::Type type)
{
    bb::system::SystemListDialog *videoQualityDialog = qobject_cast<bb::system::SystemListDialog *>(
            QObject::sender());

    if (type == bb::system::SystemUiResult::ConfirmButtonSelection) {
        int selected = videoQualityDialog->selectedIndices()[0];
        QString url;
        QString newQuality;

        if (selected < storageData.instances.count()) {
            SingleVideoStorageData storage = storageData.instances[selected];
            newQuality = storage.quality;
            setAudioOnly(false);
            playerContext->setSelectedStorageData(storage);

            if (!storage.hasEmbeddedAudio && storageData.audio.url != "") {
                // Adaptive video-only stream: needs the same remux step as
                // initial playback before it's watchable with sound.
                if (quality != newQuality) {
                    changeQualityWithRemux(newQuality, storage);
                }
                videoQualityDialog->deleteLater();
                return;
            }
            url = storage.url;
        } else {
            url = storageData.audio.url;
            newQuality = "Audio";
            setAudioOnly(true);
        }

        if (quality != newQuality) {
            changeQuality(newQuality, url);
        }
    }

    videoQualityDialog->deleteLater();
}

void PlayerPage::onScalingActionItemClick()
{
    bb::system::SystemListDialog *scalingDialog = new bb::system::SystemListDialog("Change",
            "Cancel");

    scalingDialog->setTitle("Scaling Method");
    if (playerContext->hasKeyboard()) {
        scalingDialog->setBody("Hint: press F to cycle through all options");
    }
    scalingDialog->appendItem(getScalingMethodString(bb::cascades::ScalingMethod::AspectFit), true,
            playerContext->getScalingMethod() == bb::cascades::ScalingMethod::AspectFit);
    scalingDialog->appendItem(getScalingMethodString(bb::cascades::ScalingMethod::AspectFill), true,
            playerContext->getScalingMethod() == bb::cascades::ScalingMethod::AspectFill);
    scalingDialog->appendItem(getScalingMethodString(bb::cascades::ScalingMethod::Fill), true,
            playerContext->getScalingMethod() == bb::cascades::ScalingMethod::Fill);
    scalingDialog->appendItem(getScalingMethodString(bb::cascades::ScalingMethod::None), true,
            playerContext->getScalingMethod() == bb::cascades::ScalingMethod::None);

    bool success = connect(scalingDialog, SIGNAL(finished(bb::system::SystemUiResult::Type)), this,
            SLOT(onScalingDialogFinished(bb::system::SystemUiResult::Type)));

    if (success) {
        scalingDialog->show();
    } else {
        scalingDialog->deleteLater();
    }
}

void PlayerPage::onScalingDialogFinished(bb::system::SystemUiResult::Type type)
{
    bb::system::SystemListDialog *scalingQualityDialog =
            qobject_cast<bb::system::SystemListDialog *>(QObject::sender());

    if (type == bb::system::SystemUiResult::ConfirmButtonSelection) {
        int selected = scalingQualityDialog->selectedIndices()[0];
        bb::cascades::ScalingMethod::Type oldMethod = playerContext->getScalingMethod();
        bb::cascades::ScalingMethod::Type method;

        switch (selected) {
            case 0:
                method = bb::cascades::ScalingMethod::AspectFit;
                break;
            case 1:
                method = bb::cascades::ScalingMethod::AspectFill;
                break;
            case 2:
                method = bb::cascades::ScalingMethod::Fill;
                break;
            case 3:
                method = bb::cascades::ScalingMethod::None;
                break;
        }

        if (method != oldMethod) {
            updateScaling(method);
        }
    }

    scalingQualityDialog->deleteLater();
}

void PlayerPage::onCcActionItemClick()
{
    bb::system::SystemListDialog *ccDialog = new bb::system::SystemListDialog("Change", "Cancel");

    ccDialog->setTitle("Closed Caption");
    ccDialog->appendItem("Off", true, playerContext->getCcLanguage() == "");

    for (int i = 0; i < storageData.captions.count(); i++) {
        ccDialog->appendItem(storageData.captions[i].languageName, true,
                playerContext->getCcLanguage() == storageData.captions[i].languageCode);
    }

    bool success = connect(ccDialog, SIGNAL(finished(bb::system::SystemUiResult::Type)), this,
            SLOT(onCcDialogFinished(bb::system::SystemUiResult::Type)));

    if (success) {
        ccDialog->show();
    } else {
        ccDialog->deleteLater();
    }
}

void PlayerPage::onCcDialogFinished(bb::system::SystemUiResult::Type type)
{
    bb::system::SystemListDialog *ccDialog = qobject_cast<bb::system::SystemListDialog *>(
            QObject::sender());

    if (type == bb::system::SystemUiResult::ConfirmButtonSelection) {
        int selected = ccDialog->selectedIndices()[0];
        if (selected == 0) {
            playerContext->setCcLanguage("");
            ccActionItem->setTitle("Off");
            playerContext->resetClosedCaptionUrl();
        } else {
            ClosedCaptionData *ccData = &storageData.captions[selected - 1];

            playerContext->setCcLanguage(ccData->languageCode);
            ccActionItem->setTitle(ccData->languageName);

            QFile file(getCcPath(ccData->languageCode));
            if (!ccData->isLoaded) {
                QEventLoop loop;
                QNetworkRequest getCcRequest(ccData->url);
                QNetworkReply *getCcReply = ApplicationUI::networkManager->get(getCcRequest);
                QObject::connect(getCcReply, SIGNAL(finished()), &loop, SLOT(quit()));
                loop.exec();

                if (!getCcReply->error()) {

                    if (file.open(QIODevice::WriteOnly)) {
                        file.write(CCUtils::fix(QString::fromUtf8(getCcReply->readAll())).toUtf8());
                        file.close();

                        ccData->isLoaded = true;
                    } else {
                        qDebug() << "can't open file";
                    }
                }

                getCcReply->deleteLater();
            }

            if (ccData->isLoaded) {
                playerContext->setClosedCaptionUrl(getCcPath(ccData->languageCode));
            }
        }
    }

    ccDialog->deleteLater();
}

void PlayerPage::onTimecodeActionItemClick()
{
    TimecodeSheet *sheet = new TimecodeSheet();
}

void PlayerPage::onDownloadActionItemClick()
{
    bb::system::SystemListDialog *downloadDialog = new bb::system::SystemListDialog("Download",
            "Cancel");

    downloadDialog->setTitle("Choose source");
    downloadDialog->setBody(
            "Download will start in the browser. Go to Menu->Save Page to save the file. The video's title will be copied to Clipboard for convenience. NOTE: all file sizes are approximate.");

    QString fileSize;
    int mbSize;

    for (int i = 0; i < storageData.instances.count(); i++) {
        if (storageData.instances[i].contentLength == 0) {
            fileSize = "?";
        } else {
            mbSize = storageData.instances[i].contentLength / 1024 / 1024;
            fileSize = mbSize > 0 ? QString::number(mbSize) : "<1";
        }

        downloadDialog->appendItem(
                "Video - " + storageData.instances[i].quality + " (.mp4, " + fileSize + " MB)",
                true, storageData.instances[i].quality == quality);
    }

    if (storageData.audio.url != "") {
        if (storageData.audio.contentLength == 0) {
            fileSize = "?";
        } else {
            mbSize = storageData.audio.contentLength / 1024 / 1024;
            fileSize = mbSize > 0 ? QString::number(mbSize) : "<1";
        }

        downloadDialog->appendItem("Audio (.m4a, " + fileSize + " MB)", true, audioOnly);
    }

    bool success = connect(downloadDialog, SIGNAL(finished(bb::system::SystemUiResult::Type)), this,
            SLOT(onDownloadDialogFinished(bb::system::SystemUiResult::Type)));

    if (success) {
        downloadDialog->show();
    } else {
        downloadDialog->deleteLater();
    }
}

void PlayerPage::onDownloadDialogFinished(bb::system::SystemUiResult::Type type)
{
    bb::system::SystemListDialog *downloadDialog = qobject_cast<bb::system::SystemListDialog *>(
            QObject::sender());

    if (type == bb::system::SystemUiResult::ConfirmButtonSelection) {
        int selected = downloadDialog->selectedIndices()[0];
        QString url;

        if (selected < storageData.instances.count()) {
            url = storageData.instances[selected].url;
        } else {
            url = storageData.audio.url;
        }

        bb::system::Clipboard clipboard;
        QString fileNameForClipboard =
                videoMetadata.video.title.replace('/', '_').replace('\\', '_').replace(':', '_').replace(
                        '*', '_').replace('"', '_').replace('?', '_').replace('<', '_').replace('>',
                        '_').replace('|', '_');
        clipboard.insert(QString("text/plain"), fileNameForClipboard.toUtf8());

        bb::system::InvokeRequest request;
        request.setAction("bb.action.OPEN");
        request.setTarget("sys.browser");
        request.setUri(QUrl::fromPercentEncoding(url.toUtf8()));

        bb::system::InvokeTargetReply* reply = (new bb::system::InvokeManager)->invoke(request);
        reply->deleteLater();
    }

    downloadDialog->deleteLater();
}

void PlayerPage::onChannelActionItemClick(QVariantList indexPath)
{
    VideoListItemModel *item = upNextListView->dataModel()->data(indexPath).value<
            VideoListItemModel*>();
    hideInfos();
    overlay->setVisible(true);
    youtubeClient->channel(item->channelId);
}

void PlayerPage::onVideoChannelActionItemClick()
{
    hideInfos();
    overlay->setVisible(true);
    youtubeClient->channel(videoMetadata.video.channelId);
}

void PlayerPage::onOpenVideoInBrowserActionItemClick()
{
    ActionItemService::openVideoInBrowser(videoMetadata.video.videoId);
}

void PlayerPage::onCopyVideoLinkActionItemClick()
{
    ActionItemService::copyVideoLink(videoMetadata.video.videoId);
}

void PlayerPage::onCopyStreamUrlActionItemClick()
{
    bb::system::Clipboard clipboard;
    QString link = playerContext->getSourceUrl();
    int expire = QUrl(link).queryItemValue("expire").toInt();
    int now = QDateTime::currentDateTimeUtc().toTime_t();

    clipboard.insert(QString("text/plain"), link.toUtf8());

    UIUtils::toastInfo(
            QString("The link was copied to Clipboard. It expires in %1 hour(s)").arg(
                    (expire - now) / 3600));
}

void PlayerPage::onAddToFavoritesActionItemClick()
{
    bool isFavorite = !PlaylistVideoProxy::getInstance()->contains(videoMetadata.video.videoId,
            PlaylistListItemModel::Favorites);
    int now = QDateTime::currentDateTimeUtc().toTime_t();

    if (isFavorite) {
        PlaylistVideoModel video(videoMetadata.video.videoId,
                (int) PlaylistListItemModel::Favorites, videoMetadata.video.title,
                videoMetadata.video.channelId, videoMetadata.video.channelTitle, now, now, 0);
        DbHelper::createPlaylistVideo(&video);
        PlaylistVideoProxy::getInstance()->add(&video);
    } else {
        DbHelper::deletePlaylistVideo(videoMetadata.video.videoId,
                (int) PlaylistListItemModel::Favorites);
        PlaylistVideoProxy::getInstance()->deleteById(videoMetadata.video.videoId,
                PlaylistListItemModel::Favorites);
    }

    ActionItemService::updateAddToFavoritesActionItem(addToFavoritesActionItem, isFavorite);
    UIUtils::toastInfo(isFavorite ? "Added to Favorites" : "Removed from Favorites");
}

void PlayerPage::onAddToWatchLaterActionItemClick()
{
    bool isWatchLater = !PlaylistVideoProxy::getInstance()->contains(videoMetadata.video.videoId,
            PlaylistListItemModel::WatchLater);
    int now = QDateTime::currentDateTimeUtc().toTime_t();

    if (isWatchLater) {
        PlaylistVideoModel video(videoMetadata.video.videoId,
                (int) PlaylistListItemModel::WatchLater, videoMetadata.video.title,
                videoMetadata.video.channelId, videoMetadata.video.channelTitle, now, now, 0);
        DbHelper::createPlaylistVideo(&video);
        PlaylistVideoProxy::getInstance()->add(&video);
    } else {
        DbHelper::deletePlaylistVideo(videoMetadata.video.videoId,
                (int) PlaylistListItemModel::WatchLater);
        PlaylistVideoProxy::getInstance()->deleteById(videoMetadata.video.videoId,
                PlaylistListItemModel::WatchLater);
    }

    ActionItemService::updateAddToWatchLaterActionItem(addToWatchLaterActionItem, isWatchLater);
    UIUtils::toastInfo(isWatchLater ? "Added to Watch Later" : "Removed from Watch Later");
}

void PlayerPage::addToHistory()
{
    bool isHistory = !PlaylistVideoProxy::getInstance()->contains(videoMetadata.video.videoId,
            PlaylistListItemModel::History);
    int now = QDateTime::currentDateTimeUtc().toTime_t();
    PlaylistVideoModel video(videoMetadata.video.videoId, (int) PlaylistListItemModel::History,
            videoMetadata.video.title, videoMetadata.video.channelId,
            videoMetadata.video.channelTitle, now, now, 0);

    if (isHistory) {
        DbHelper::createPlaylistVideo(&video);
        PlaylistVideoProxy::getInstance()->add(&video);
    }

    // this method will update video's data in all playlists
    DbHelper::updatePlaylistVideo(&video);
    PlaylistVideoProxy::getInstance()->update(&video);
}

void PlayerPage::onChannelDataReceived(ChannelPageData channelData)
{
    overlay->setVisible(false);

    ChannelPage *channelPage = new ChannelPage(channelData, this->navigationPane);
    this->navigationPane->insert(1, channelPage);

    for (int i = navigationPane->count() - 1; i >= 2; i--) {
        bb::cascades::Page *page = navigationPane->at(i);

        navigationPane->remove(page);
        page->deleteLater();
    }

    this->navigationPane->navigateTo(channelPage);

}

void PlayerPage::onBrowserActionItemClick()
{
    bb::system::InvokeRequest request;
    request.setAction("bb.action.OPEN");
    request.setTarget("sys.browser");
    request.setUri("https://m.youtube.com/watch?v=" + videoMetadata.video.videoId);

    bb::system::InvokeTargetReply* reply = (new bb::system::InvokeManager)->invoke(request);
    reply->deleteLater();
}

void PlayerPage::onNpcPrev()
{
    onPreviousActionItemClick();
}

void PlayerPage::onNpcNext()
{
    onNextActionItemClick();
}

void PlayerPage::playVideoFromOutside(QString url)
{
    playerContext->pause();
    hideInfos();
    overlay->setVisible(true);
    playerContext->stopPlaylist();
    playVideoByIdOrUrl(url);
}

void PlayerPage::playVideoFromPlaylist(QString url)
{
    hideInfos();
    overlay->setVisible(true);
    playVideoByIdOrUrl(url);
}

void PlayerPage::onPlayAudioOnlyActionItemClick(QVariantList indexPath)
{
    setAudioOnly(true);
    onUpnextVideoListItemClick(indexPath);
}

void PlayerPage::setAudioOnly(bool audioOnly)
{
    this->audioOnly = audioOnly;
    playerContext->setAudioOnly(audioOnly);
}

QString PlayerPage::getScalingMethodString(bb::cascades::ScalingMethod::Type type)
{
    switch (type) {
        case bb::cascades::ScalingMethod::AspectFit:
            return "Aspect Fit";
        case bb::cascades::ScalingMethod::AspectFill:
            return "Aspect Fill";
        case bb::cascades::ScalingMethod::Fill:
            return "Fill";
        case bb::cascades::ScalingMethod::None:
            return "Original Size";
        default:
            return QString();
    }
}

void PlayerPage::onLoopScalingShortcut()
{
    if (scalingActionItem->isEnabled()) {
        bb::cascades::ScalingMethod::Type oldMethod = playerContext->getScalingMethod();
        bb::cascades::ScalingMethod::Type method;

        switch (oldMethod) {
            case bb::cascades::ScalingMethod::AspectFit:
                method = bb::cascades::ScalingMethod::AspectFill;
                break;
            case bb::cascades::ScalingMethod::AspectFill:
                method = bb::cascades::ScalingMethod::Fill;
                break;
            case bb::cascades::ScalingMethod::Fill:
                method = bb::cascades::ScalingMethod::None;
                break;
            case bb::cascades::ScalingMethod::None:
                method = bb::cascades::ScalingMethod::AspectFit;
                break;
        }

        updateScaling(method);
    }
}

void PlayerPage::onAudioOnlyShortcut()
{
    if (qualityActionItem->isEnabled() && storageData.audio.url != "") {
        QString newQuality = "Audio";

        if (newQuality != quality) {
            setAudioOnly(true);
            changeQuality(newQuality, storageData.audio.url);
        }
    }
}

void PlayerPage::onVideoShortcut()
{
    if (qualityActionItem->isEnabled()) {
        SingleVideoStorageData storage = storageData.instances[getIndexOfDefaultQuality()];

        if (storage.quality != quality) {
            setAudioOnly(false);
            playerContext->setSelectedStorageData(storage);
            changeQuality(storage.quality, storage.url);
        }
    }
}

void PlayerPage::updateScaling(bb::cascades::ScalingMethod::Type newMethod)
{
    playerContext->setScalingMethod(newMethod);
    playerContext->setPlayerScalingMethod(newMethod);
    playerContext->resizeFullScreenVideo();
    scalingActionItem->setTitle(getScalingMethodString(newMethod));
}

void PlayerPage::adjustInfoScreen()
{
    UIConfig *ui = subtitleContainer->ui();

    if (playerContext->getScreenHeight() > 1400) {
        //Special case for Passport
        subtitleContainer->setBottomPadding(ui->du(55));
    } else if (playerContext->getScreenHeight() > 800) {
        subtitleContainer->setBottomPadding(ui->du(70));
    } else {
        subtitleContainer->setBottomPadding(ui->du(20));
    }
}

void PlayerPage::onDoubleTappedHandler(bb::cascades::DoubleTapEvent* e)
{
    if (progressSlider->isEnabled()) {
        sliderDoubleTap = true;
    }
}

void PlayerPage::changeQuality(QString newQuality, QString url, bool forcePlay, int seekMs)
{
    quality = newQuality;
    qualityActionItem->setTitle(quality);
    setProgressSliderEnabled(!audioOnly);
    audioBackground->setVisible(audioOnly);
    scalingActionItem->setEnabled(!audioOnly);
    timecodeActionItem->setEnabled(
            progressSlider->isEnabled() && videoMetadata.timecodes.count() > 0);

    if (progressSlider->isEnabled()) {
        progressSlider->requestFocus();
    } else {
        closeButton->requestFocus();
    }

    if (audioOnly) {
        // adjust CC position
        playerContext->resizeFullScreenVideo();
    }

    bb::multimedia::MediaError::Type error = playerContext->changeQuality(url, forcePlay, seekMs);
    if (error != bb::multimedia::MediaError::None) {
        UIUtils::toastError("Source unavailable");
    }
}

int PlayerPage::getIndexOfDefaultQuality()
{
    for (int i = storageData.instances.count() - 1; i >= 0; i--) {
        if (QString::compare(storageData.instances[i].quality, appSettings->defaultQuality())
                <= 0) {
            return i;
        }
    }

    for (int i = 0; i < storageData.instances.count(); i++) {
        if (QString::compare(storageData.instances[i].quality, appSettings->defaultQuality()) > 0) {
            return i;
        }
    }

    return storageData.instances.count() - 1;
}

void PlayerPage::onRepeatActionItemClick()
{
    bb::multimedia::RepeatMode::Type newType;

    switch (playerContext->getRepeatMode()) {
        case bb::multimedia::RepeatMode::None:
            newType =
                    playerContext->getPlaylistId() > 0 ?
                            bb::multimedia::RepeatMode::All : bb::multimedia::RepeatMode::Track;
            break;
        case bb::multimedia::RepeatMode::All:
            newType = bb::multimedia::RepeatMode::Track;
            break;
        case bb::multimedia::RepeatMode::Track:
            newType = bb::multimedia::RepeatMode::None;
        default:
            break;
    }

    playerContext->setRepeatMode(newType);
    updateRepeatActionItem();
    setNextVideoId();
    setPrevVideoId();
    UIUtils::toastInfo(repeatActionItem->title());
}

void PlayerPage::updateRepeatActionItem()
{
    switch (playerContext->getRepeatMode()) {
        case bb::multimedia::RepeatMode::None:
            repeatActionItem->setImageSource(QUrl("asset:///images/ic_repeat_off.png"));
            repeatActionItem->setTitle("Repeat off");

            break;
        case bb::multimedia::RepeatMode::Track:
            repeatActionItem->setImageSource(QUrl("asset:///images/ic_repeat_one.png"));
            repeatActionItem->setTitle("Repeat one");

            break;
        case bb::multimedia::RepeatMode::All:
            repeatActionItem->setImageSource(QUrl("asset:///images/ic_repeat_all.png"));
            repeatActionItem->setTitle("Repeat list");

            break;
        default:
            break;
    }
}

QString PlayerPage::getCcPath(QString languageCode)
{
    QString appFolder(QDir::homePath());
    appFolder.chop(4);

    return QString("%1tmp/%2.xml").arg(appFolder, languageCode);
}

void PlayerPage::onPlaylistActionItemClick()
{
    PlaylistSheet *sheet = new PlaylistSheet(navigationPane);
}

void PlayerPage::onPlaylistChanged(int playlistId)
{
    if (playlistId == 0 && playlistActionItem) {
        this->removeAction(playlistActionItem);
        playlistActionItem = 0;
        playerContext->setRepeatMode(bb::multimedia::RepeatMode::None);
        setNextVideoId();
        setPrevVideoId();
    }
}

void PlayerPage::setPrevVideoId()
{
    prevVideoId = "";
    if (playerContext->getPlaylistId() > 0) {
        prevVideoId = playerContext->getPrevVideoFromPlaylist();
    } else if (watched.count() > 1) {
        prevVideoId = watched[watched.count() - 2];
    }
    previousActionItem->setEnabled(prevVideoId != "");
    playerContext->setPrevEnabled(prevVideoId != "");
}

void PlayerPage::setNextVideoId()
{
    nextVideoId = "";
    if (playerContext->getPlaylistId() > 0) {
        nextVideoId = playerContext->getNextVideoFromPlaylist();
    } else if (upNextListView->dataModel()->childCount(QVariantList()) > 0) {
        nextVideoId = upNextListView->dataModel()->data(QVariantList() << 0).value<
                VideoListItemModel*>()->id;
    }
    nextActionItem->setEnabled(nextVideoId != "");
    playerContext->setNextEnabled(nextVideoId != "");
}

void PlayerPage::onPlaylistVideoAdded(PlaylistVideoModel* video)
{
    if (video->playlistId != playerContext->getPlaylistId()) {
        return;
    }

    setPrevVideoId();
    setNextVideoId();
}

void PlayerPage::onPlaylistVideoDeleted(QString videoId, PlaylistListItemModel::Type playlistType)
{
    if ((int) playlistType != playerContext->getPlaylistId()) {
        return;
    }

    setPrevVideoId();
    setNextVideoId();
}

void PlayerPage::onPlaylistVideoDeletedAll(PlaylistListItemModel::Type playlistType)
{
    if ((int) playlistType != playerContext->getPlaylistId()) {
        return;
    }

    setPrevVideoId();
    setNextVideoId();
}

void PlayerPage::setProgressSliderEnabled(bool value)
{
    progressSlider->setEnabled(value);
    backward10Button->setVisible(value);
    forward10Button->setVisible(value);
}
