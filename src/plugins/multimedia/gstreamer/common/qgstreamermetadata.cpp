// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#include "qgstreamermetadata_p.h"
#include <QtMultimedia/qmediametadata.h>
#include <QtMultimedia/qtvideo.h>
#include <QtCore/qdebug.h>
#include <QtCore/qdatetime.h>
#include <QtCore/qlocale.h>
#include <QtCore/qtimezone.h>
#include <QtGui/qimage.h>

#include <gst/gstversion.h>
#include <common/qgst_handle_types_p.h>
#include <common/qgstutils_p.h>

QT_BEGIN_NAMESPACE

namespace {

namespace MetadataLookupImpl {

#ifdef __cpp_lib_constexpr_algorithms
#  define constexpr_lookup constexpr
#else
#  define constexpr_lookup /*constexpr*/
#endif

struct MetadataKeyValuePair
{
    const char *tag;
    QMediaMetaData::Key key;
};

constexpr const char *toTag(const char *t)
{
    return t;
}
constexpr const char *toTag(const MetadataKeyValuePair &kv)
{
    return kv.tag;
}

constexpr QMediaMetaData::Key toKey(QMediaMetaData::Key k)
{
    return k;
}
constexpr QMediaMetaData::Key toKey(const MetadataKeyValuePair &kv)
{
    return kv.key;
}

constexpr auto compareByKey = [](const auto &lhs, const auto &rhs) {
    return toKey(lhs) < toKey(rhs);
};

constexpr auto compareByTag = [](const auto &lhs, const auto &rhs) {
    return std::strcmp(toTag(lhs), toTag(rhs)) < 0;
};

constexpr_lookup auto makeLookupTable()
{
    std::array<MetadataKeyValuePair, 22> lookupTable{ {
            { GST_TAG_TITLE, QMediaMetaData::Title },
            { GST_TAG_COMMENT, QMediaMetaData::Comment },
            { GST_TAG_DESCRIPTION, QMediaMetaData::Description },
            { GST_TAG_GENRE, QMediaMetaData::Genre },
            { GST_TAG_DATE_TIME, QMediaMetaData::Date },
            { GST_TAG_DATE, QMediaMetaData::Date },

            { GST_TAG_LANGUAGE_CODE, QMediaMetaData::Language },

            { GST_TAG_ORGANIZATION, QMediaMetaData::Publisher },
            { GST_TAG_COPYRIGHT, QMediaMetaData::Copyright },

            // Media
            { GST_TAG_DURATION, QMediaMetaData::Duration },

            // Audio
            { GST_TAG_BITRATE, QMediaMetaData::AudioBitRate },
            { GST_TAG_AUDIO_CODEC, QMediaMetaData::AudioCodec },

            // Music
            { GST_TAG_ALBUM, QMediaMetaData::AlbumTitle },
            { GST_TAG_ALBUM_ARTIST, QMediaMetaData::AlbumArtist },
            { GST_TAG_ARTIST, QMediaMetaData::ContributingArtist },
            { GST_TAG_TRACK_NUMBER, QMediaMetaData::TrackNumber },

            { GST_TAG_PREVIEW_IMAGE, QMediaMetaData::ThumbnailImage },
            { GST_TAG_IMAGE, QMediaMetaData::CoverArtImage },

            // Image/Video
            { "resolution", QMediaMetaData::Resolution },
            { GST_TAG_IMAGE_ORIENTATION, QMediaMetaData::Orientation },

            // Video
            { GST_TAG_VIDEO_CODEC, QMediaMetaData::VideoCodec },

            // Movie
            { GST_TAG_PERFORMER, QMediaMetaData::LeadPerformer },
    } };

    std::sort(lookupTable.begin(), lookupTable.end(),
              [](const MetadataKeyValuePair &lhs, const MetadataKeyValuePair &rhs) {
                  return std::string_view(lhs.tag) < std::string_view(rhs.tag);
              });
    return lookupTable;
}

constexpr_lookup auto gstTagToMetaDataKey = makeLookupTable();
constexpr_lookup auto metaDataKeyToGstTag = [] {
    auto array = gstTagToMetaDataKey;
    std::sort(array.begin(), array.end(), compareByKey);
    return array;
}();

} // namespace MetadataLookupImpl

QMediaMetaData::Key tagToKey(const char *tag)
{
    if (tag == nullptr)
        return QMediaMetaData::Key(-1);

    using namespace MetadataLookupImpl;
    auto foundIterator = std::lower_bound(gstTagToMetaDataKey.begin(), gstTagToMetaDataKey.end(),
                                          tag, compareByTag);
    if (std::strcmp(foundIterator->tag, tag) == 0)
        return foundIterator->key;

    return QMediaMetaData::Key(-1);
}

const char *keyToTag(QMediaMetaData::Key key)
{
    using namespace MetadataLookupImpl;
    auto foundIterator = std::lower_bound(metaDataKeyToGstTag.begin(), metaDataKeyToGstTag.end(),
                                          key, compareByKey);
    if (foundIterator->key == key)
        return foundIterator->tag;

    return nullptr;
}

#undef constexpr_lookup

QtVideo::Rotation parseRotationTag(const char *string)
{
    using namespace std::string_view_literals;

    if (string == "rotate-90"sv)
        return QtVideo::Rotation::Clockwise90;
    if (string == "rotate-180"sv)
        return QtVideo::Rotation::Clockwise180;
    if (string == "rotate-270"sv)
        return QtVideo::Rotation::Clockwise270;
    if (string == "rotate-0"sv)
        return QtVideo::Rotation::None;

    qCritical() << "cannot parse orientation: {}" << string;
    return QtVideo::Rotation::None;
}

QDateTime parseDate(const GValue &val)
{
    Q_ASSERT(G_VALUE_TYPE(&val) == G_TYPE_DATE);

    const GDate *date = (const GDate *)g_value_get_boxed(&val);
    if (!g_date_valid(date))
        return {};

    int year = g_date_get_year(date);
    int month = g_date_get_month(date);
    int day = g_date_get_day(date);
    return QDateTime(QDate(year, month, day), QTime());
}

QDateTime parseDateTime(const GValue &val)
{
    Q_ASSERT(G_VALUE_TYPE(&val) == GST_TYPE_DATE_TIME);

    const GstDateTime *dateTime = (const GstDateTime *)g_value_get_boxed(&val);
    int year = gst_date_time_has_year(dateTime) ? gst_date_time_get_year(dateTime) : 0;
    int month = gst_date_time_has_month(dateTime) ? gst_date_time_get_month(dateTime) : 0;
    int day = gst_date_time_has_day(dateTime) ? gst_date_time_get_day(dateTime) : 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    float tz = 0;
    if (gst_date_time_has_time(dateTime)) {
        hour = gst_date_time_get_hour(dateTime);
        minute = gst_date_time_get_minute(dateTime);
        second = gst_date_time_get_second(dateTime);
        tz = gst_date_time_get_time_zone_offset(dateTime);
    }
    return QDateTime{
        QDate(year, month, day),
        QTime(hour, minute, second),
        QTimeZone(tz * 60 * 60),
    };
}

QImage parseImage(const GValue &val)
{
    Q_ASSERT(G_VALUE_TYPE(&val) == GST_TYPE_SAMPLE);

    GstSample *sample = (GstSample *)g_value_get_boxed(&val);
    GstCaps *caps = gst_sample_get_caps(sample);
    if (caps && !gst_caps_is_empty(caps)) {
        GstStructure *structure = gst_caps_get_structure(caps, 0);
        const gchar *name = gst_structure_get_name(structure);
        if (QByteArray(name).startsWith("image/")) {
            GstBuffer *buffer = gst_sample_get_buffer(sample);
            if (buffer) {
                GstMapInfo info;
                gst_buffer_map(buffer, &info, GST_MAP_READ);
                QImage image = QImage::fromData(info.data, info.size, name);
                gst_buffer_unmap(buffer, &info);
                return image;
            }
        }
    }

    return {};
}

std::optional<double> parseFractionAsDouble(const GValue &val)
{
    Q_ASSERT(G_VALUE_TYPE(&val) == GST_TYPE_FRACTION);

    int nom = gst_value_get_fraction_numerator(&val);
    int denom = gst_value_get_fraction_denominator(&val);
    if (denom == 0)
        return std::nullopt;
    return double(nom) / double(denom);
}

// internal
void addTagToMetaData(const GstTagList *list, const gchar *tag, void *userdata)
{
    QMediaMetaData &metadata = *reinterpret_cast<QMediaMetaData *>(userdata);

    QMediaMetaData::Key key = tagToKey(tag);
    if (key == QMediaMetaData::Key(-1))
        return;

    GValue val{};
    gst_tag_list_copy_value(&val, list, tag);

    GType type = G_VALUE_TYPE(&val);

    if (type == G_TYPE_STRING) {
        const gchar *str_value = g_value_get_string(&val);

        switch (key) {
        case QMediaMetaData::Language: {
            metadata.insert(key,
                            QVariant::fromValue(QLocale::codeToLanguage(
                                    QString::fromUtf8(str_value), QLocale::ISO639Part2)));
            break;
        }
        case QMediaMetaData::Orientation: {
            metadata.insert(key, QVariant::fromValue(parseRotationTag(str_value)));
            break;
        }
        default:
            metadata.insert(key, QString::fromUtf8(str_value));
            break;
        };
    } else if (type == G_TYPE_INT) {
        metadata.insert(key, g_value_get_int(&val));
    } else if (type == G_TYPE_UINT) {
        metadata.insert(key, g_value_get_uint(&val));
    } else if (type == G_TYPE_LONG) {
        metadata.insert(key, qint64(g_value_get_long(&val)));
    } else if (type == G_TYPE_BOOLEAN) {
        metadata.insert(key, g_value_get_boolean(&val));
    } else if (type == G_TYPE_CHAR) {
        metadata.insert(key, g_value_get_schar(&val));
    } else if (type == G_TYPE_DOUBLE) {
        metadata.insert(key, g_value_get_double(&val));
    } else if (type == G_TYPE_DATE) {
        if (!metadata.keys().contains(key)) {
            QDateTime date = parseDate(val);
            if (date.isValid())
                metadata.insert(key, date);
        }
    } else if (type == GST_TYPE_DATE_TIME) {
        metadata.insert(key, parseDateTime(val));
    } else if (type == GST_TYPE_SAMPLE) {
        QImage image = parseImage(val);
        if (!image.isNull())
            metadata.insert(key, image);
    } else if (type == GST_TYPE_FRACTION) {
        std::optional<double> fraction = parseFractionAsDouble(val);

        if (fraction)
            metadata.insert(key, *fraction);
    }

    g_value_unset(&val);
}

} // namespace

QMediaMetaData taglistToMetaData(const GstTagList *tagList)
{
    QMediaMetaData m;
    if (tagList)
        gst_tag_list_foreach(tagList, reinterpret_cast<GstTagForeachFunc>(&addTagToMetaData), &m);
    return m;
}

QMediaMetaData taglistToMetaData(const QGstTagListHandle &handle)
{
    return taglistToMetaData(handle.get());
}

static void applyMetaDataToTagSetter(const QMediaMetaData &metadata, GstTagSetter *element)
{
    gst_tag_setter_reset_tags(element);

    for (QMediaMetaData::Key key : metadata.keys()) {
        const char *tagName = keyToTag(key);
        if (!tagName)
            continue;
        const QVariant &tagValue = metadata.value(key);

        auto setTag = [&](const auto &value) {
            gst_tag_setter_add_tags(element, GST_TAG_MERGE_REPLACE, tagName, value, nullptr);
        };

        switch (tagValue.typeId()) {
        case QMetaType::QString:
            setTag(tagValue.toString().toUtf8().constData());
            break;
        case QMetaType::Int:
        case QMetaType::LongLong:
            setTag(tagValue.toInt());
            break;
        case QMetaType::Double:
            setTag(tagValue.toDouble());
            break;
        case QMetaType::QDate:
        case QMetaType::QDateTime: {
            QDateTime date = tagValue.toDateTime();

            QGstGstDateTimeHandle dateTime{
                gst_date_time_new(date.offsetFromUtc() / 60. / 60., date.date().year(),
                                  date.date().month(), date.date().day(), date.time().hour(),
                                  date.time().minute(), date.time().second()),
                QGstGstDateTimeHandle::HasRef,
            };

            setTag(dateTime.get());
            break;
        }
        default: {
            if (tagValue.typeId() == qMetaTypeId<QLocale::Language>()) {
                QByteArray language = QLocale::languageToCode(tagValue.value<QLocale::Language>(),
                                                              QLocale::ISO639Part2)
                                              .toUtf8();
                setTag(language.constData());
            }

            break;
        }
        }
    }
}

void applyMetaDataToTagSetter(const QMediaMetaData &metadata, const QGstElement &element)
{
    GstTagSetter *tagSetter = qGstSafeCast<GstTagSetter>(element.element());
    if (tagSetter)
        applyMetaDataToTagSetter(metadata, tagSetter);
    else
        qWarning() << "applyMetaDataToTagSetter failed: element not a GstTagSetter"
                   << element.name();
}

void applyMetaDataToTagSetter(const QMediaMetaData &metadata, const QGstBin &bin)
{
    GstIterator *elements = gst_bin_iterate_all_by_interface(bin.bin(), GST_TYPE_TAG_SETTER);
    GValue item = {};

    while (gst_iterator_next(elements, &item) == GST_ITERATOR_OK) {
        GstElement *element = static_cast<GstElement *>(g_value_get_object(&item));
        if (!element)
            continue;

        GstTagSetter *tagSetter = qGstSafeCast<GstTagSetter>(element);

        if (tagSetter)
            applyMetaDataToTagSetter(metadata, tagSetter);
    }

    gst_iterator_free(elements);
}

QT_END_NAMESPACE
