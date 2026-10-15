#ifndef TrendingData_HPP_
#define TrendingData_HPP_

#include "VideoMetadata.hpp"

class TrendingDataCategory
{
public:
    QString title;
    QString categoryKey;
};

class TrendingData
{
public:
    QList<SingleVideoMetadata> videos;
    QList<TrendingDataCategory> categories;
    // Region (lowercase ISO code) the list actually came from -- can differ
    // from the requested one if the client fell back to the US chart.
    QString region;
};

#endif /* TrendingData_HPP_ */
