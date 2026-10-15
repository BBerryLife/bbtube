#ifndef TRENDINGREGIONS_HPP_
#define TRENDINGREGIONS_HPP_

#include <QString>
#include <QLocale>

// Regions for which YouTube Charts (charts.youtube.com) publishes a
// "Trending" chart. List taken from YouTube Help > "YouTube Charts &
// Insights" > "Available locations" (checked 2026-10-15), sorted by name.
// Codes are lowercase ISO 3166-1 alpha-2, as used in the Charts URLs and in
// the chart_params_country_code request parameter.
//
// C++03/GNU++98 only (matches bbtube's QNX/gcc 4.6.3 toolchain).
struct TrendingRegion
{
    const char *code;
    const char *name;
};

static const TrendingRegion TRENDING_REGIONS[] = {
    { "ar", "Argentina" },
    { "au", "Australia" },
    { "at", "Austria" },
    { "be", "Belgium" },
    { "bo", "Bolivia" },
    { "br", "Brazil" },
    { "ca", "Canada" },
    { "cl", "Chile" },
    { "co", "Colombia" },
    { "cr", "Costa Rica" },
    { "cz", "Czech Republic" },
    { "dk", "Denmark" },
    { "do", "Dominican Republic" },
    { "ec", "Ecuador" },
    { "eg", "Egypt" },
    { "sv", "El Salvador" },
    { "ee", "Estonia" },
    { "fi", "Finland" },
    { "fr", "France" },
    { "de", "Germany" },
    { "gt", "Guatemala" },
    { "hn", "Honduras" },
    { "hk", "Hong Kong" },
    { "hu", "Hungary" },
    { "is", "Iceland" },
    { "in", "India" },
    { "id", "Indonesia" },
    { "ie", "Ireland" },
    { "il", "Israel" },
    { "it", "Italy" },
    { "jp", "Japan" },
    { "ke", "Kenya" },
    { "lu", "Luxembourg" },
    { "my", "Malaysia" },
    { "mx", "Mexico" },
    { "nl", "Netherlands" },
    { "nz", "New Zealand" },
    { "ni", "Nicaragua" },
    { "ng", "Nigeria" },
    { "no", "Norway" },
    { "pa", "Panama" },
    { "py", "Paraguay" },
    { "pe", "Peru" },
    { "ph", "Philippines" },
    { "pl", "Poland" },
    { "pt", "Portugal" },
    { "ro", "Romania" },
    { "ru", "Russia" },
    { "sa", "Saudi Arabia" },
    { "rs", "Serbia" },
    { "sg", "Singapore" },
    { "za", "South Africa" },
    { "kr", "South Korea" },
    { "es", "Spain" },
    { "se", "Sweden" },
    { "ch", "Switzerland" },
    { "tw", "Taiwan" },
    { "tz", "Tanzania" },
    { "th", "Thailand" },
    { "tr", "Turkey" },
    { "ug", "Uganda" },
    { "ua", "Ukraine" },
    { "ae", "United Arab Emirates" },
    { "gb", "United Kingdom" },
    { "us", "United States" },
    { "uy", "Uruguay" },
    { "vn", "Vietnam" },
    { "zw", "Zimbabwe" }
};
static const int TRENDING_REGIONS_COUNT = sizeof(TRENDING_REGIONS) / sizeof(TRENDING_REGIONS[0]);

class TrendingRegions
{
public:
    static const char *fallbackCode()
    {
        return "us";
    }

    static bool isSupported(const QString &code)
    {
        return !nameFor(code).isEmpty();
    }

    // Human readable name, or "" if the code isn't a supported region.
    static QString nameFor(const QString &code)
    {
        QString lower = code.toLower();
        for (int i = 0; i < TRENDING_REGIONS_COUNT; i++) {
            if (lower == QString(TRENDING_REGIONS[i].code)) {
                return QString::fromLatin1(TRENDING_REGIONS[i].name);
            }
        }
        return QString();
    }

    // Best guess from the device's locale ("vi_VN" -> "vn"); falls back to
    // the US chart when the device region isn't one Charts supports. Note a
    // device set to English (US) will report "us" regardless of where the
    // user physically is -- hence the explicit setting.
    static QString fromSystemLocale()
    {
        QString localeName = QLocale::system().name(); // e.g. "en_US"
        int underscore = localeName.indexOf('_');
        if (underscore >= 0) {
            QString code = localeName.mid(underscore + 1).toLower();
            if (isSupported(code)) {
                return code;
            }
        }
        return QString::fromLatin1(fallbackCode());
    }
};

#endif /* TRENDINGREGIONS_HPP_ */
