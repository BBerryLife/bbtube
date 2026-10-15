#ifndef ITEMRENDERERPARSER_HPP_
#define ITEMRENDERERPARSER_HPP_

#include "src/parser/models/ChannelData.hpp"
#include "src/parser/models/SearchData.hpp"
#include "src/parser/models/VideoMetadata.hpp"

#include <bb/data/JsonDataAccess>
#include <QStringList>

// ItemRendererParser: converts InnerTube JSON renderer blocks into our
// internal data models.
//
// KEY FIXES vs. the original (2021) implementation:
//  - getSimpleOrRunText() helper added: many text fields that used to be
//    guaranteed "simpleText" (viewCountText, shortViewCountText, etc.) can
//    now arrive as {"runs":[...]} instead (e.g. live videos show
//    "1.2K watching now" via runs). The old code only read simpleText and
//    silently produced empty strings for these cases.
//  - videoPrimaryIndex / videoSecondaryIndex are now initialized to -1 and
//    bounds-checked before use; the old code used uninitialized ints which
//    could read garbage memory / crash if a renderer was missing.
//  - populateVideoMetadata tolerates missing sections (e.g. no related
//    videos, no description run list) instead of indexing blindly.
//  - getVideo() and getFromChannelVideo() now also handle the channel/owner
//    fields appearing as "longBylineText" (used in channel-page video grids)
//    in addition to "ownerText" (used in search/home results).
class ItemRendererParser
{
private:
    static QString getTitleFromRuns(QVariantList runs)
    {
        QStringList parts;

        for (int i = 0; i < runs.count(); i++) {
            parts.append(runs[i].toMap()["text"].toString());
        }

        return parts.join("");
    }

    // Many fields can appear either as {"simpleText": "..."} or as
    // {"runs": [{"text": "..."}, ...]}. This helper normalizes both.
    static QString getSimpleOrRunText(const QVariantMap &textObject)
    {
        if (textObject.contains("simpleText")) {
            return textObject["simpleText"].toString();
        }
        if (textObject.contains("runs")) {
            return getTitleFromRuns(textObject["runs"].toList());
        }
        return "";
    }

    static QString getBestThumbnailUrl(const QVariantMap *map, const QString &key = "thumbnail")
    {
        QVariantList thumbnails = (*map)[key].toMap()["thumbnails"].toList();
        if (thumbnails.isEmpty()) {
            return "";
        }
        // Thumbnails are ordered smallest -> largest; take the last (highest res)
        return thumbnails.last().toMap()["url"].toString();
    }

    static ChannelData getChannel(QVariantMap *map)
    {
        ChannelData data;

        data.channelId = (*map)["channelId"].toString();
        data.thumbnailUrl = getBestThumbnailUrl(map);
        data.title = getSimpleOrRunText((*map)["title"].toMap());

        return data;
    }

    static SingleVideoMetadata getFromCompactVideo(QVariantMap *map)
    {
        SingleVideoMetadata data;

        data.videoId = (*map)["videoId"].toString();
        data.channelId = (*map)["channelId"].toString();

        QVariantList bylineRuns = (*map)["shortBylineText"].toMap()["runs"].toList();
        if (!bylineRuns.isEmpty()) {
            data.channelTitle = bylineRuns[0].toMap()["text"].toString();
        }

        data.dateUploadedAgo = getSimpleOrRunText((*map)["publishedTimeText"].toMap());
        data.thumbnailUrl = getBestThumbnailUrl(map);
        data.title = getSimpleOrRunText((*map)["title"].toMap());
        data.viewsCount = getSimpleOrRunText((*map)["viewCountText"].toMap());
        data.shortViewsCount = getSimpleOrRunText((*map)["shortViewCountText"].toMap());
        data.lengthText = getSimpleOrRunText((*map)["lengthText"].toMap());

        return data;
    }

    static SingleVideoMetadata getVideoInternal(const QVariantMap *map);

    static SearchDataSection getSection(QVariantMap *map)
    {
        SearchDataSection data;

        data.title = getSimpleOrRunText((*map)["title"].toMap());
        QVariantList items =
                (*map)["content"].toMap()["verticalListRenderer"].toMap()["items"].toList();

        for (int i = 0; i < items.size(); i++) {
            QVariantMap tempMap = items[i].toMap()["videoRenderer"].toMap();
            if (tempMap.isEmpty()) {
                continue;
            }
            data.videos.append(getVideoInternal(&tempMap));
        }
        return data;
    }

public:
    static void populateSearchData(SearchData *searchData, const QString *json)
    {
        bb::data::JsonDataAccess jda;
        QVariantMap map = jda.loadFromBuffer(*json).toMap();

        QVariantMap sectionListRenderer =
                map["contents"].toMap()["twoColumnSearchResultsRenderer"].toMap()
                ["primaryContents"].toMap()["sectionListRenderer"].toMap();

        // Fallback: some search responses nest one level differently
        // (estimatedResults wrapper) — try alternate path if empty.
        if (sectionListRenderer.isEmpty()) {
            sectionListRenderer = map["contents"].toMap()["sectionListRenderer"].toMap();
        }

        QVariantList contentsList = sectionListRenderer["contents"].toList();
        QVariantList contents;

        for (int i = 0; i < contentsList.size(); i++) {
            QVariantMap contentsItem = contentsList[i].toMap();

            if (contentsItem.contains("itemSectionRenderer")) {
                contents = contentsItem["itemSectionRenderer"].toMap()["contents"].toList();
            }
        }

        for (int i = 0; i < contents.size(); i++) {
            QVariantMap item = contents[i].toMap();
            if (item.isEmpty()) continue;

            QString key = item.keys()[0];
            QVariantMap tempMap = item[key].toMap();

            if (key == "channelRenderer") {
                searchData->channels.append(getChannel(&tempMap));
            } else if (key == "shelfRenderer") {
                searchData->sections.append(getSection(&tempMap));
            } else if (key == "videoRenderer") {
                SingleVideoMetadata video = getVideoInternal(&tempMap);
                searchData->videos.append(video);
            }
            // "radioRenderer" (mix playlists) and "playlistRenderer" are
            // intentionally skipped — not supported by this client.
        }

        QVariantList searchParamsGroups =
                sectionListRenderer["subMenu"].toMap()["searchSubMenuRenderer"].toMap()["groups"].toList();
        for (int i = 0; i < searchParamsGroups.count(); i++) {
            QVariantMap renderer = searchParamsGroups[i].toMap()["searchFilterGroupRenderer"].toMap();
            SearchParamGroup group;
            QVariantList filters = renderer["filters"].toList();

            group.title = getSimpleOrRunText(renderer["title"].toMap());
            group.isRemovable = false;

            for (int j = 0; j < filters.count(); j++) {
                QVariantMap filterRenderer = filters[j].toMap()["searchFilterRenderer"].toMap();
                SearchParamOption option;

                option.title = getSimpleOrRunText(filterRenderer["label"].toMap());

                QString filterStatus = filterRenderer["status"].toString();

                if (filterStatus == "FILTER_STATUS_DISABLED") {
                    option.enabled = false;
                    option.selected = false;
                    option.urlParams = "";
                } else if (filterStatus == "FILTER_STATUS_SELECTED") {
                    option.enabled = true;
                    option.selected = true;
                    option.urlParams = "";
                    if (filterRenderer.contains("navigationEndpoint")) {
                        group.paramsToReset =
                                filterRenderer["navigationEndpoint"].toMap()["searchEndpoint"].toMap()["params"].toString();
                        group.isRemovable = true;
                    }
                } else {
                    option.enabled = true;
                    option.selected = false;
                    if (filterRenderer.contains("navigationEndpoint")) {
                        option.urlParams =
                                filterRenderer["navigationEndpoint"].toMap()["searchEndpoint"].toMap()["params"].toString();
                    }
                }

                group.options.append(option);
            }

            searchData->searchParamGroups.append(group);
        }
    }

    static void populateVideoMetadata(VideoMetadata *videoMetadata, const QString *json)
    {
        bb::data::JsonDataAccess jda;
        QVariantMap jsonMap = jda.loadFromBuffer(*json).toMap();

        if (!jsonMap.contains("contents")) {
            return;
        }

        QVariantMap map = jsonMap["contents"].toMap()["twoColumnWatchNextResults"].toMap();
        QVariantList primaryInfo = map["results"].toMap()["results"].toMap()["contents"].toList();

        int videoPrimaryIndex = -1;
        int videoSecondaryIndex = -1;

        for (int i = 0; i < primaryInfo.count(); i++) {
            QVariantMap tempMap = primaryInfo[i].toMap();
            if (tempMap.contains("videoPrimaryInfoRenderer")) {
                videoPrimaryIndex = i;
            } else if (tempMap.contains("videoSecondaryInfoRenderer")) {
                videoSecondaryIndex = i;
            }
        }

        // Video ID: prefer currentVideoEndpoint, but fall back to scanning
        // primaryInfo renderers if that's missing (seen on some A/B variants).
        videoMetadata->video.videoId =
                jsonMap["currentVideoEndpoint"].toMap()["watchEndpoint"].toMap()["videoId"].toString();

        if (videoSecondaryIndex >= 0) {
            QVariantMap secondaryRenderer =
                    primaryInfo[videoSecondaryIndex].toMap()["videoSecondaryInfoRenderer"].toMap();
            QVariantMap ownerRenderer = secondaryRenderer["owner"].toMap()["videoOwnerRenderer"].toMap();

            videoMetadata->video.channelId =
                    ownerRenderer["navigationEndpoint"].toMap()["browseEndpoint"].toMap()["browseId"].toString();

            QVariantList ownerTitleRuns = ownerRenderer["title"].toMap()["runs"].toList();
            if (!ownerTitleRuns.isEmpty()) {
                videoMetadata->video.channelTitle = ownerTitleRuns[0].toMap()["text"].toString();
            }

            // Description (used below for timecodes) — guard against missing field
            QVariantList descriptionRunList = secondaryRenderer["description"].toMap()["runs"].toList();
            int i = 0;
            while (i < descriptionRunList.count()) {
                QVariantMap descriptionRun = descriptionRunList[i].toMap();
                QVariantMap watchEndpoint =
                        descriptionRun["navigationEndpoint"].toMap()["watchEndpoint"].toMap();
                Timecode timecode;
                QStringList descriptionParts;

                if (watchEndpoint.contains("startTimeSeconds")) {
                    timecode.seconds = watchEndpoint["startTimeSeconds"].toInt();
                    timecode.time = descriptionRun["text"].toString();

                    int j = i + 1;
                    while (j < descriptionRunList.count()) {
                        QVariantMap nextDescriptionRun = descriptionRunList[j].toMap();
                        QString currText = nextDescriptionRun["text"].toString();
                        bool stop = currText.contains('\n');

                        descriptionParts.append(
                                currText.left(stop ? currText.indexOf('\n') : currText.length()));

                        if (stop || j == descriptionRunList.count() - 1) {
                            break;
                        }

                        j++;
                    }

                    timecode.description = descriptionParts.join("");
                    if (timecode.description.length() > 1
                            && (timecode.description[0] == ')' || timecode.description[0] == ']')) {
                        timecode.description = timecode.description.mid(1);
                    }

                    videoMetadata->timecodes.append(timecode);
                    descriptionParts.clear();
                    i = j + 1;
                } else {
                    i++;
                }
            }
        }

        if (videoPrimaryIndex >= 0) {
            QVariantMap primaryRenderer =
                    primaryInfo[videoPrimaryIndex].toMap()["videoPrimaryInfoRenderer"].toMap();

            videoMetadata->video.dateUploadedAgo = getSimpleOrRunText(primaryRenderer["dateText"].toMap());
            videoMetadata->video.title =
                    getTitleFromRuns(primaryRenderer["title"].toMap()["runs"].toList());

            // viewCount can be videoViewCountRenderer.viewCount as simpleText
            // OR (for premieres / live) as a "runs" array with "watching now".
            QVariantMap viewCountRenderer =
                    primaryRenderer["viewCount"].toMap()["videoViewCountRenderer"].toMap();
            videoMetadata->video.viewsCount = getSimpleOrRunText(viewCountRenderer["viewCount"].toMap());
            if (videoMetadata->video.viewsCount.isEmpty()) {
                videoMetadata->video.viewsCount =
                        getSimpleOrRunText(viewCountRenderer["originalViewCount"].toMap());
            }
        }

        videoMetadata->video.thumbnailUrl = "";
        videoMetadata->video.lengthText = "";
        videoMetadata->video.shortViewsCount = "";

        QVariantList secondaryResults =
                map["secondaryResults"].toMap()["secondaryResults"].toMap()["results"].toList();

        for (int i = 0; i < secondaryResults.size(); i++) {
            QVariantMap item = secondaryResults[i].toMap();
            if (item.isEmpty()) continue;

            QString key = item.keys()[0];

            if (key == "compactAutoplayRenderer") {
                QVariantList autoplayContents = item[key].toMap()["contents"].toList();
                if (!autoplayContents.isEmpty()) {
                    QVariantMap tempMap = autoplayContents[0].toMap()["compactVideoRenderer"].toMap();
                    if (!tempMap.isEmpty()) {
                        videoMetadata->relatedVideos.nextVideo = getFromCompactVideo(&tempMap);
                    }
                }
            } else if (key == "compactVideoRenderer") {
                QVariantMap tempMap = item[key].toMap();
                videoMetadata->relatedVideos.otherVideos.append(getFromCompactVideo(&tempMap));
            }
            // "itemSectionRenderer" (sometimes wraps ad slots / continuation)
            // and "compactPlaylistRenderer" are intentionally ignored.
        }
    }

    static SingleVideoMetadata getFromChannelVideo(QVariantMap *map)
    {
        SingleVideoMetadata data;

        data.videoId = (*map)["videoId"].toString();
        data.title = getSimpleOrRunText((*map)["title"].toMap());
        data.dateUploadedAgo = getSimpleOrRunText((*map)["publishedTimeText"].toMap());
        data.shortViewsCount = getSimpleOrRunText((*map)["shortViewCountText"].toMap());
        if (data.shortViewsCount.isEmpty()) {
            data.shortViewsCount = getSimpleOrRunText((*map)["viewCountText"].toMap());
        }
        data.thumbnailUrl = getBestThumbnailUrl(map);

        QVariantList overlays = (*map)["thumbnailOverlays"].toList();
        for (int i = 0; i < overlays.count(); i++) {
            QVariantMap overlay = overlays[i].toMap();
            if (overlay.contains("thumbnailOverlayTimeStatusRenderer")) {
                data.lengthText = getSimpleOrRunText(
                        overlay["thumbnailOverlayTimeStatusRenderer"].toMap()["text"].toMap());
                break;
            }
        }

        return data;
    }

    // Extract the plain-text "duration" badge from a lockupViewModel's
    // thumbnail overlays (thumbnailBottomOverlayViewModel.badges[]), if
    // present. YouTube's badge shape varies slightly, so we look for a
    // nested "text" string inside each badge rather than a single fixed path.
    static QString extractLockupDuration(const QVariantMap &lockupViewModel)
    {
        QVariantList overlays = lockupViewModel["contentImage"].toMap()["thumbnailViewModel"]
                .toMap()["overlays"].toList();

        for (int i = 0; i < overlays.count(); i++) {
            QVariantMap overlay = overlays[i].toMap();
            if (!overlay.contains("thumbnailBottomOverlayViewModel")) {
                continue;
            }

            QVariantList badges =
                    overlay["thumbnailBottomOverlayViewModel"].toMap()["badges"].toList();
            for (int j = 0; j < badges.count(); j++) {
                QVariantMap badge = badges[j].toMap();
                // Common shape: { "thumbnailBadgeViewModel": { "text": "12:34" } }
                foreach(const QString &key, badge.keys()) {
                    QVariantMap inner = badge[key].toMap();
                    if (inner.contains("text")) {
                        return inner["text"].toString();
                    }
                }
            }
        }

        return QString();
    }

    // Extract subtitle text parts (views count, upload date, etc.) from a
    // lockupViewModel's metadataRows. Each row has "metadataParts", and each
    // part typically nests a "text": { "content": "..." }.
    static QStringList extractLockupMetadataParts(const QVariantMap &lockupViewModel)
    {
        QStringList parts;

        QVariantList metadataRows = lockupViewModel["metadata"].toMap()["lockupMetadataViewModel"]
                .toMap()["metadata"].toMap()["contentMetadataViewModel"].toMap()
                ["metadataRows"].toList();

        for (int i = 0; i < metadataRows.count(); i++) {
            QVariantList metadataParts = metadataRows[i].toMap()["metadataParts"].toList();

            for (int j = 0; j < metadataParts.count(); j++) {
                QVariantMap part = metadataParts[j].toMap();
                QString text = part["text"].toMap()["content"].toString();
                if (!text.isEmpty()) {
                    parts.append(text);
                }
            }
        }

        return parts;
    }

public:
    // Parse a "lockupViewModel" (YouTube's newer View Model architecture,
    // which has replaced "videoRenderer" in channel/home video grids as of
    // ~2025) into a SingleVideoMetadata. Shared by ChannelPageParser and
    // RecommendedPageParser (and any other caller hitting the new shape).
    static SingleVideoMetadata getVideoFromLockup(const QVariantMap &lockupViewModel)
    {
        SingleVideoMetadata data;

        // Lockups are not only videos: "Mix" / playlist / podcast cards use the
        // same shape, but their contentId is a *playlist* id (e.g. "RD...").
        // Treating that as a videoId produces a broken thumbnail
        // (i.ytimg.com/vi/<playlistId>/...), no duration (shown as a red
        // LIVE badge, since an empty length means live stream) and an item
        // that can't be played. Return an empty videoId so callers skip it.
        // An absent contentType is treated as a video to stay lenient.
        QString contentType = lockupViewModel["contentType"].toString();
        if (!contentType.isEmpty() && contentType != "LOCKUP_CONTENT_TYPE_VIDEO") {
            return data;
        }

        data.videoId = lockupViewModel["contentId"].toString();

        QVariantMap metadataViewModel =
                lockupViewModel["metadata"].toMap()["lockupMetadataViewModel"].toMap();
        data.title = metadataViewModel["title"].toMap()["content"].toString();

        QVariantList sources = lockupViewModel["contentImage"].toMap()["thumbnailViewModel"]
                .toMap()["image"].toMap()["sources"].toList();
        if (!sources.isEmpty()) {
            data.thumbnailUrl = sources.last().toMap()["url"].toString();
        }

        data.lengthText = extractLockupDuration(lockupViewModel);

        QStringList metadataParts = extractLockupMetadataParts(lockupViewModel);
        if (metadataParts.count() > 0) {
            data.shortViewsCount = metadataParts[0];
        }
        if (metadataParts.count() > 1) {
            data.dateUploadedAgo = metadataParts[1];
        }

        return data;
    }

    // Public wrapper retained for backward source-compatibility with
    // callers in RecommendedPageParser / TrendingPageParser that pass a
    // "videoRenderer" map (search/home-grid style).
    static SingleVideoMetadata getVideo(QVariantMap *map)
    {
        return getVideoInternal(map);
    }
};

// Implementation of the shared "videoRenderer" -> SingleVideoMetadata mapping.
// Used by search results, the home/recommended feed, and trending shelves —
// all three use the same "videoRenderer" JSON shape.
inline SingleVideoMetadata ItemRendererParser::getVideoInternal(const QVariantMap *map)
{
    SingleVideoMetadata data;

    data.videoId = (*map)["videoId"].toString();
    if (data.videoId.isEmpty()) {
        data.videoId =
                (*map)["navigationEndpoint"].toMap()["watchEndpoint"].toMap()["videoId"].toString();
    }

    // Channel info: "ownerText" is the common field; "longBylineText" /
    // "shortBylineText" appear in some grid contexts.
    QVariantMap ownerTextMap = (*map)["ownerText"].toMap();
    if (ownerTextMap.isEmpty()) {
        ownerTextMap = (*map)["longBylineText"].toMap();
    }
    if (ownerTextMap.isEmpty()) {
        ownerTextMap = (*map)["shortBylineText"].toMap();
    }

    QVariantList ownerRuns = ownerTextMap["runs"].toList();
    if (!ownerRuns.isEmpty()) {
        QVariantMap firstRun = ownerRuns[0].toMap();
        data.channelTitle = firstRun["text"].toString();
        data.channelId =
                firstRun["navigationEndpoint"].toMap()["browseEndpoint"].toMap()["browseId"].toString();
    }

    data.dateUploadedAgo = ItemRendererParser::getSimpleOrRunText((*map)["publishedTimeText"].toMap());
    data.thumbnailUrl = ItemRendererParser::getBestThumbnailUrl(map);

    QVariantMap titleMap = (*map)["title"].toMap();
    if (titleMap.contains("runs")) {
        QVariantList titleRuns = titleMap["runs"].toList();
        if (!titleRuns.isEmpty()) {
            data.title = titleRuns[0].toMap()["text"].toString();
        }
    } else {
        data.title = titleMap["simpleText"].toString();
    }

    data.viewsCount = ItemRendererParser::getSimpleOrRunText((*map)["viewCountText"].toMap());
    data.shortViewsCount = ItemRendererParser::getSimpleOrRunText((*map)["shortViewCountText"].toMap());
    data.lengthText = ItemRendererParser::getSimpleOrRunText((*map)["lengthText"].toMap());

    return data;
}

#endif /* ITEMRENDERERPARSER_HPP_ */
