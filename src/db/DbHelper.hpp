#ifndef DbHelper_HPP_
#define DbHelper_HPP_

#include "src/models/ChannelListItemModel.hpp"
#include "src/models/PlaylistListItemModel.hpp"
#include "src/models/PlaylistVideoModel.hpp"

#include <QtSql/QtSql>
#include <QStringList>
#include <QMap>

class DbHelper
{
private:
    static QSqlDatabase db;
    static QSqlDatabase getDbContext();
    static void createScriptsTableIfNeeded();
    static void applyScripts();
public:
    static void transaction();
    static void commit();
    static bool isAutoplay();
    static void setAutoplay(bool value);
    static int getEqualizerPreset();
    static void setEqualizerPreset(int value);
    static int getPlaybackTimeout();
    static void setPlaybackTimeout(int value);
    static QString defaultTab();
    static void setDefautTab(QString tab);
    static QString defaultQuality();
    static void setDefautQuality(QString quality);
    static bool isDebugLogToFile();
    static void setDebugLogToFile(bool value);
    static QString trendingRegion();
    static void setTrendingRegion(QString value);
    static void runMigrations();

    static void createChannel(const ChannelListItemModel *channel);
    static void updateChannelGeneralInfo(const ChannelListItemModel *channel);
    static void updateChannelActivityInfo(const ChannelListItemModel *channel);
    static void markAllChannelsAsRead(int currentTime);
    static void deleteChannel(QString channelId);
    static QList<ChannelListItemModel*> getChannelList(ChannelListItemModel::SortBy =
            ChannelListItemModel::LastActivity, ChannelListItemModel::SortDirection =
            ChannelListItemModel::Desc);
    static QStringList getChannelIdList();

    static QList<PlaylistListItemModel*> getPlaylistList();
    static PlaylistListItemModel *getPlaylistById(int playlistId);

    static QList<PlaylistVideoModel*> getAllPlaylistVideoIds();
    static QList<PlaylistVideoModel*> getListByPlaylistId(int playlistId,
            PlaylistVideoModel::SortBy = PlaylistVideoModel::DateAdded,
            PlaylistVideoModel::SortDirection = PlaylistVideoModel::Desc);
    static void createPlaylistVideo(const PlaylistVideoModel *video);
    static void deletePlaylistVideo(QString videoId, int playlistId);
    static void updatePlaylistVideo(const PlaylistVideoModel *video);
    static void deleteAllFromPlaylist(int playlistId);

    static QMap<QString, int> getViewedPercents();
    static void setViewedPercent(QString videoId, int value);
    static void deleteViewedPercent(QString videoId);
    static void deleteAllViewedPercents();

    // Google session (see src/auth/GoogleAuthManager). All values stored
    // here are already encrypted/opaque by the time they reach the DB --
    // see CookieCrypto -- except googleEmail, which is stored in the clear
    // since it's only used for display ("Logged in as ...") and isn't
    // sensitive on its own.
    static QString getGoogleAuthValue(QString key);
    static void setGoogleAuthValue(QString key, QString value);
    static void clearGoogleAuth();
};

#endif /* DbHelper_HPP_ */
