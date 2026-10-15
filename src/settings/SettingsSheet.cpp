#include "SettingsSheet.hpp"
#include "src/applicationui.hpp"
#include "src/auth/GoogleLoginSheet.hpp"
#include "src/utils/FileLogger.hpp"

#include <bb/cascades/Page>
#include <bb/cascades/Container>
#include <bb/cascades/ToggleButton>
#include <bb/cascades/Button>
#include <bb/cascades/TextField>
#include <bb/cascades/Label>
#include <bb/cascades/TitleBar>
#include <bb/cascades/ActionItem>
#include <bb/cascades/StackLayout>
#include <bb/cascades/StackLayoutProperties>
#include <bb/cascades/LayoutOrientation>
#include <bb/cascades/UIConfig>
#include <bb/cascades/SystemDefaults>
#include <bb/cascades/VerticalAlignment>
#include <bb/cascades/Divider>
#include <bb/cascades/TextFieldInputMode>
#include <bb/cascades/Dropdown>
#include <bb/cascades/Option>

SettingsSheet::SettingsSheet() :
        BaseSheet()
{
    appSettings = ApplicationUI::appSettings;

    Page *content = new Page();
    Container *container = Container::create();
    UIConfig *ui = container->ui();
    container->setTopPadding(ui->du(2));
    container->setRightPadding(ui->du(1));
    container->setBottomPadding(ui->du(1));
    container->setLeftPadding(ui->du(1));

    tabDropdown = DropDown::create().title("Default Tab");
    container->add(tabDropdown);
    QStringList options;
    options << "Search" << "Channels" << "Playlists" << "Recommended" << "Trending";
    for (int i = 0; i < options.count(); i++) {
        tabDropdown->add(
                Option::create().text(options[i]).value(options[i]).selected(
                        appSettings->defaultTab() == options[i]));
    }

    container->add(Divider::create());

    qualityDropdown = DropDown::create().title("Default Quality");
    container->add(qualityDropdown);

    options.clear();
    options << "240p" << "360p" << "720p";
    for (int i = 0; i < options.count(); i++) {
        qualityDropdown->add(
                Option::create().text(options[i]).value(options[i]).selected(
                        appSettings->defaultQuality() == options[i]));
    }

    trendingRegionDropdown = DropDown::create().title("Trending Region");
    container->add(trendingRegionDropdown);

    QString currentRegion = appSettings->trendingRegion();
    for (int i = 0; i < TRENDING_REGIONS_COUNT; i++) {
        QString code = QString::fromLatin1(TRENDING_REGIONS[i].code);
        trendingRegionDropdown->add(
                Option::create().text(QString::fromLatin1(TRENDING_REGIONS[i].name)).value(code).selected(
                        currentRegion == code));
    }

    Container *autoplayContainer = Container::create();
    StackLayout *autoplayLayout = new StackLayout();
    autoplayLayout->setOrientation(LayoutOrientation::LeftToRight);
    autoplayContainer->setLayout(autoplayLayout);
    Container *autoplayLabelContainer = Container::create().layoutProperties(
            StackLayoutProperties::create().spaceQuota(1));
    autoplayLabelContainer->setVerticalAlignment(VerticalAlignment::Center);
    Label *autoplayLabel = Label::create().text("Autoplay");
    autoplayLabel->textStyle()->setBase(SystemDefaults::TextStyles::titleText());
    autoplayLabelContainer->add(autoplayLabel);
    autoplayButton = ToggleButton::create().checked(appSettings->isAutoplay());
    autoplayContainer->add(autoplayLabelContainer);
    autoplayContainer->add(autoplayButton);
    container->add(autoplayContainer);

    Container *playbackTimeoutContainer = Container::create().topMargin(ui->du(2));
    StackLayout *playbackTimeoutLayout = new StackLayout();
    playbackTimeoutLayout->setOrientation(LayoutOrientation::LeftToRight);
    playbackTimeoutContainer->setLayout(playbackTimeoutLayout);
    Container *playbackTimeoutLabelContainer = Container::create().layoutProperties(
            StackLayoutProperties::create().spaceQuota(1));
    playbackTimeoutLabelContainer->setVerticalAlignment(VerticalAlignment::Center);
    Label *playbackTimeoutLabel = Label::create().text("Pause playback after").multiline(true);
    playbackTimeoutLabel->textStyle()->setBase(SystemDefaults::TextStyles::titleText());
    playbackTimeoutLabelContainer->add(playbackTimeoutLabel);
    playbackTimeoutTextField =
            TextField::create().hintText("minutes").preferredWidth(ui->du(20)).text(
                    QString::number(appSettings->getPlaybackTimeout())).inputMode(
                    TextFieldInputMode::NumbersAndPunctuation);
    playbackTimeoutContainer->add(playbackTimeoutLabelContainer);
    playbackTimeoutContainer->add(playbackTimeoutTextField);
    container->add(playbackTimeoutContainer);
    Container *playbackTimeoutCommentContainer = Container::create();
    playbackTimeoutCommentContainer->setTopMargin(ui->du(1));
    playbackTimeoutCommentContainer->setHorizontalAlignment(HorizontalAlignment::Fill);
    Label *playbackTimeoutCommentLabel = Label::create().multiline(true);
    playbackTimeoutCommentLabel->setText(
            "number of minutes playback will stop after while the app is in the background; from 0-1440 range, 0 means \"No Timeout\"");
    playbackTimeoutCommentLabel->textStyle()->setBase(SystemDefaults::TextStyles::subtitleText());
    playbackTimeoutCommentContainer->add(playbackTimeoutCommentLabel);
    container->add(playbackTimeoutCommentContainer);

    container->add(Divider::create());

    Container *debugLogLabelContainer = Container::create().topMargin(ui->du(1));
    Label *debugLogTitleLabel = Label::create().text("Export Debug Log");
    debugLogTitleLabel->textStyle()->setBase(SystemDefaults::TextStyles::titleText());
    debugLogLabelContainer->add(debugLogTitleLabel);
    container->add(debugLogLabelContainer);

    Container *debugLogContainer = Container::create().topMargin(ui->du(1));
    StackLayout *debugLogLayout = new StackLayout();
    debugLogLayout->setOrientation(LayoutOrientation::LeftToRight);
    debugLogContainer->setLayout(debugLogLayout);
    Container *debugLogButtonLabelContainer = Container::create().layoutProperties(
            StackLayoutProperties::create().spaceQuota(1));
    debugLogButtonLabelContainer->setVerticalAlignment(VerticalAlignment::Center);
    Label *debugLogButtonLabel = Label::create().text("Save debug log to file").multiline(true);
    debugLogButtonLabel->textStyle()->setBase(SystemDefaults::TextStyles::titleText());
    debugLogButtonLabelContainer->add(debugLogButtonLabel);
    debugLogButton = ToggleButton::create().checked(appSettings->isDebugLogToFile());
    debugLogContainer->add(debugLogButtonLabelContainer);
    debugLogContainer->add(debugLogButton);
    container->add(debugLogContainer);

    Container *debugLogCommentContainer = Container::create();
    debugLogCommentContainer->setTopMargin(ui->du(1));
    debugLogCommentContainer->setHorizontalAlignment(HorizontalAlignment::Fill);
    Label *debugLogCommentLabel = Label::create().multiline(true);
    debugLogCommentLabel->setText(
            "when on, app log messages are appended to a file so they can be reviewed "
                    "after the fact even if the console log was cut short. saved to:\n"
            + FileLogger::logFilePath());
    debugLogCommentLabel->textStyle()->setBase(SystemDefaults::TextStyles::subtitleText());
    debugLogCommentContainer->add(debugLogCommentLabel);
    container->add(debugLogCommentContainer);

    container->add(Divider::create());

    Container *googleAccountLabelContainer = Container::create().topMargin(ui->du(1));
    Label *googleAccountTitleLabel = Label::create().text("Google Account");
    googleAccountTitleLabel->textStyle()->setBase(SystemDefaults::TextStyles::titleText());
    googleAccountLabelContainer->add(googleAccountTitleLabel);
    container->add(googleAccountLabelContainer);

    Container *googleAccountCommentContainer = Container::create();
    googleAccountCommentContainer->setTopMargin(ui->du(1));
    googleAccountCommentContainer->setHorizontalAlignment(HorizontalAlignment::Fill);
    googleAccountStatusLabel = Label::create().multiline(true);
    googleAccountStatusLabel->textStyle()->setBase(SystemDefaults::TextStyles::subtitleText());
    googleAccountCommentContainer->add(googleAccountStatusLabel);
    container->add(googleAccountCommentContainer);

    googleAccountButton = Button::create().topMargin(ui->du(1));
    QObject::connect(googleAccountButton, SIGNAL(clicked()), this,
            SLOT(googleAccountButtonClick()));
    container->add(googleAccountButton);

    QObject::connect(ApplicationUI::googleAuthManager, SIGNAL(loginStateChanged(bool, QString)),
            this, SLOT(onGoogleLoginStateChanged(bool, QString)));

    refreshGoogleAccountUi();

    TitleBar *titleBar = new TitleBar(TitleBarKind::Default);
    ActionItem *closeAction = ActionItem::create().title("Cancel");
    ActionItem *saveAction = ActionItem::create().title("Done");
    QObject::connect(closeAction, SIGNAL(triggered()), this, SLOT(closeActionClick()));
    QObject::connect(saveAction, SIGNAL(triggered()), this, SLOT(saveActionClick()));
    titleBar->setTitle("Settings");
    titleBar->setDismissAction(closeAction);
    titleBar->setAcceptAction(saveAction);

    content->setTitleBar(titleBar);
    content->setContent(container);
    this->setContent(content);

    open();
}

void SettingsSheet::saveActionClick()
{
    appSettings->setAutoplay(autoplayButton->isChecked());
    bool parsedOk = false;
    int playbackTimeout = playbackTimeoutTextField->text().trimmed().toInt(&parsedOk);
    if (!parsedOk) {
        playbackTimeout = 0;
    } else {
        if (playbackTimeout < 0) {
            playbackTimeout = 0;
        } else if (playbackTimeout > 1440) {
            playbackTimeout = 1440;
        }
    }

    appSettings->setPlaybackTimeout(playbackTimeout);
    appSettings->setDefaultTab(tabDropdown->selectedValue().toString());
    appSettings->setDefaultQuality(qualityDropdown->selectedValue().toString());
    appSettings->setTrendingRegion(trendingRegionDropdown->selectedValue().toString());
    appSettings->setDebugLogToFile(debugLogButton->isChecked());

    closeSheet();
}

void SettingsSheet::refreshGoogleAccountUi()
{
    GoogleAuthManager *auth = ApplicationUI::googleAuthManager;
    if (auth->isLoggedIn()) {
        QString email = auth->email();
        if (email.isEmpty()) {
            googleAccountStatusLabel->setText("Signed in to Google.");
        } else {
            googleAccountStatusLabel->setText("Signed in as " + email + ".");
        }
        googleAccountButton->setText("Log out");
    } else {
        googleAccountStatusLabel->setText(
                "Sign in with a real Google account to help avoid YouTube's "
                        "\"Sign in to confirm you're not a bot\" restriction, which can "
                        "block higher-quality video streams.");
        googleAccountButton->setText("Login to Google");
    }
}

void SettingsSheet::googleAccountButtonClick()
{
    if (ApplicationUI::googleAuthManager->isLoggedIn()) {
        ApplicationUI::googleAuthManager->logout();
        // logout() emits loginStateChanged, which onGoogleLoginStateChanged
        // handles by calling refreshGoogleAccountUi() -- no need to do it
        // again here.
    } else {
        new GoogleLoginSheet(); // self-parenting BaseSheet, closes/deletes itself
    }
}

void SettingsSheet::onGoogleLoginStateChanged(bool loggedIn, QString email)
{
    Q_UNUSED(loggedIn);
    Q_UNUSED(email);
    refreshGoogleAccountUi();
}
