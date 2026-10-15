#ifndef APPSETTINGS_HPP_
#define APPSETTINGS_HPP_

#include "src/db/DbHelper.hpp"
#include "src/utils/FileLogger.hpp"
#include "src/settings/TrendingRegions.hpp"

class AppSettings: public QObject
{
Q_OBJECT
public:
    AppSettings(QObject *parent) :
            QObject(parent)
    {
        autoplay = DbHelper::isAutoplay();
        playbackTimeout = DbHelper::getPlaybackTimeout();
        tab = DbHelper::defaultTab();
        quality = DbHelper::defaultQuality();
        debugLogToFile = DbHelper::isDebugLogToFile();
        trendingRegionCode = DbHelper::trendingRegion();
    }
    ~AppSettings()
    {
    }
    bool isAutoplay()
    {
        return autoplay;
    }
    void setAutoplay(bool value)
    {
        if (autoplay != value) {
            autoplay = value;
            DbHelper::setAutoplay(value);
            emit autoplayChanged(value);
        }
    }
    int getPlaybackTimeout()
    {
        return playbackTimeout;
    }
    void setPlaybackTimeout(int value)
    {
        if (playbackTimeout != value) {
            playbackTimeout = value;
            DbHelper::setPlaybackTimeout(value);
            emit playbackTimeoutChanged(value);
        }
    }
    QString defaultTab()
    {
        return tab;
    }
    void setDefaultTab(QString value)
    {
        if (tab != value) {
            tab = value;
            DbHelper::setDefautTab(value);
        }
    }
    QString defaultQuality()
    {
        return quality;
    }
    void setDefaultQuality(QString value)
    {
        if (quality != value) {
            quality = value;
            DbHelper::setDefautQuality(value);
        }
    }
    bool isDebugLogToFile()
    {
        return debugLogToFile;
    }
    void setDebugLogToFile(bool value)
    {
        if (debugLogToFile != value) {
            debugLogToFile = value;
            DbHelper::setDebugLogToFile(value);
            FileLogger::setEnabled(value);
            emit debugLogToFileChanged(value);
        }
    }
    // Region whose YouTube Charts "Trending" list the Trending tab shows
    // (lowercase ISO code, always one of TRENDING_REGIONS). Until the user
    // picks one explicitly this follows the device locale.
    QString trendingRegion()
    {
        if (TrendingRegions::isSupported(trendingRegionCode)) {
            return trendingRegionCode.toLower();
        }
        return TrendingRegions::fromSystemLocale();
    }
    void setTrendingRegion(QString value)
    {
        value = value.toLower();
        if (!TrendingRegions::isSupported(value)) {
            return;
        }
        if (trendingRegion() != value || trendingRegionCode != value) {
            bool changed = (trendingRegion() != value);
            trendingRegionCode = value;
            DbHelper::setTrendingRegion(value);
            if (changed) {
                emit trendingRegionChanged(value);
            }
        }
    }
signals:
    void trendingRegionChanged(QString value);
    void autoplayChanged(bool value);
    void playbackTimeoutChanged(int value);
    void debugLogToFileChanged(bool value);
private:
    bool autoplay;
    int playbackTimeout;
    QString tab;
    QString quality;
    bool debugLogToFile;
    QString trendingRegionCode;
};

#endif /* APPSETTINGS_HPP_ */
