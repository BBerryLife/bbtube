#ifndef SETTINGSSHEET_HPP_
#define SETTINGSSHEET_HPP_

#include "AppSettings.hpp"
#include "src/utils/BaseSheet.hpp"
#include "src/auth/GoogleAuthManager.hpp"

#include <QObject>
#include <bb/cascades/ToggleButton>
#include <bb/cascades/Button>
#include <bb/cascades/Label>
#include <bb/cascades/TextField>
#include <bb/cascades/Dropdown>

using namespace bb::cascades;

class SettingsSheet: public BaseSheet
{
    Q_OBJECT
public:
    SettingsSheet();
    virtual ~SettingsSheet()
    {
    }
private slots:
    void saveActionClick();
    void googleAccountButtonClick();
    void onGoogleLoginStateChanged(bool loggedIn, QString email);
private:
    void refreshGoogleAccountUi();

    AppSettings *appSettings;
    ToggleButton *autoplayButton;
    TextField *playbackTimeoutTextField;
    DropDown *tabDropdown;
    DropDown *qualityDropdown;
    DropDown *trendingRegionDropdown;
    ToggleButton *debugLogButton;
    Label *googleAccountStatusLabel;
    Button *googleAccountButton;
};

#endif /* SETTINGSSHEET_HPP_ */
