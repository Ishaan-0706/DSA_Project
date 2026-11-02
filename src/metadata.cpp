#include "blockdedup/metadata.hpp"
#ifdef BLOCKDEDUP_HAS_EXIV2
#include <exiv2/exiv2.hpp>
#endif
#include <filesystem>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <iostream>

namespace blockdedup {

namespace {

#ifdef BLOCKDEDUP_HAS_EXIV2
// Helper to convert GPS rational string (e.g. "28/1 34/1 1234/100") to decimal degrees
double parseGpsCoordinate(const Exiv2::Value& val, const std::string& ref) {
    if (val.count() < 3) return 0.0;
    
    double deg = val.toFloat(0);
    double min = val.toFloat(1);
    double sec = val.toFloat(2);

    double decimal = deg + (min / 60.0) + (sec / 3600.0);
    if (ref == "S" || ref == "W" || ref == "s" || ref == "w") {
        decimal = -decimal;
    }
    return decimal;
}

// Helper to parse EXIF timestamp "YYYY:MM:DD HH:MM:SS" into epoch seconds
int64_t parseExifTimestamp(const std::string& str) {
    if (str.size() < 19) return 0;
    std::tm tm = {};
    // Parse YYYY:MM:DD HH:MM:SS
    int year = 0, month = 0, day = 0, hour = 0, min = 0, sec = 0;
    if (sscanf(str.c_str(), "%d:%d:%d %d:%d:%d", &year, &month, &day, &hour, &min, &sec) == 6) {
        tm.tm_year = year - 1900;
        tm.tm_mon = month - 1;
        tm.tm_mday = day;
        tm.tm_hour = hour;
        tm.tm_min = min;
        tm.tm_sec = sec;
        tm.tm_isdst = -1;
        time_t t = mktime(&tm);
        return (t != -1) ? static_cast<int64_t>(t) : 0;
    }
    return 0;
}
#endif

} // namespace

std::optional<ImageMetadata> MetadataExtractor::extract(const std::string& filepath) {
    try {
        if (!std::filesystem::exists(filepath)) {
            return std::nullopt;
        }

        ImageMetadata meta;
        meta.filepath = filepath;
        meta.filename = std::filesystem::path(filepath).filename().string();
        meta.fileSize = std::filesystem::file_size(filepath);

#ifdef BLOCKDEDUP_HAS_EXIV2
        auto image = Exiv2::ImageFactory::open(filepath);
        if (!image.get()) return std::nullopt;

        image->readMetadata();

        meta.mimeType = image->mimeType();
        meta.width = image->pixelWidth();
        meta.height = image->pixelHeight();

        // 1. Extract EXIF data
        const Exiv2::ExifData& exifData = image->exifData();
        std::string latRef = "N", lonRef = "E";
        bool hasLat = false, hasLon = false;
        double parsedLat = 0.0, parsedLon = 0.0;

        for (auto it = exifData.begin(); it != exifData.end(); ++it) {
            std::string key = it->key();
            std::string valStr = it->print(&exifData);
            meta.allTags[key] = valStr;

            if (key == "Exif.Image.Make") {
                meta.make = valStr;
            } else if (key == "Exif.Image.Model") {
                meta.model = valStr;
            } else if (key == "Exif.Photo.DateTimeOriginal" || 
                      (meta.timestampStr.empty() && key == "Exif.Image.DateTime")) {
                meta.timestampStr = valStr;
                meta.timestamp = parseExifTimestamp(it->value().toString());
                if (meta.timestamp > 0) meta.hasTimestamp = true;
            } else if (key == "Exif.Photo.ExposureTime") {
                meta.exposureTime = valStr;
            } else if (key == "Exif.Photo.FNumber") {
                meta.fNumber = valStr;
            } else if (key == "Exif.Photo.ISOSpeedRatings") {
                meta.iso = valStr;
            } else if (key == "Exif.GPSInfo.GPSLatitudeRef") {
                latRef = it->value().toString();
            } else if (key == "Exif.GPSInfo.GPSLongitudeRef") {
                lonRef = it->value().toString();
            } else if (key == "Exif.GPSInfo.GPSLatitude") {
                parsedLat = parseGpsCoordinate(it->value(), latRef);
                hasLat = true;
            } else if (key == "Exif.GPSInfo.GPSLongitude") {
                parsedLon = parseGpsCoordinate(it->value(), lonRef);
                hasLon = true;
            } else if (key == "Exif.GPSInfo.GPSAltitude") {
                meta.gps.altitude = it->value().toFloat();
            }
        }

        if (hasLat && hasLon) {
            meta.gps.latitude = parsedLat;
            meta.gps.longitude = parsedLon;
            meta.gps.valid = true;
        }

        // 2. Extract XMP tags if present
        const Exiv2::XmpData& xmpData = image->xmpData();
        for (auto it = xmpData.begin(); it != xmpData.end(); ++it) {
            meta.allTags[it->key()] = it->value().toString();
        }
#endif

        // Fallback: If no timestamp in EXIF, use filesystem modification time
        if (!meta.hasTimestamp) {
            auto ftime = std::filesystem::last_write_time(filepath);
            auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                ftime - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now()
            );
            meta.timestamp = std::chrono::system_clock::to_time_t(sctp);
            meta.hasTimestamp = true;
            meta.timestampStr = "(Filesystem Time)";
        }

        return meta;

#ifdef BLOCKDEDUP_HAS_EXIV2
    } catch (const Exiv2::Error& e) {
        std::cerr << "Exiv2 error for " << filepath << ": " << e.what() << "\n";
        return std::nullopt;
#endif
    } catch (const std::exception& e) {
        std::cerr << "Exception reading " << filepath << ": " << e.what() << "\n";
        return std::nullopt;
    }
}

std::map<std::string, std::string> MetadataExtractor::computeMetadataDelta(
    const ImageMetadata& target,
    const ImageMetadata& base
) {
    std::map<std::string, std::string> delta;
    for (const auto& [k, v] : target.allTags) {
        auto it = base.allTags.find(k);
        if (it == base.allTags.end() || it->second != v) {
            delta[k] = v; // Modified or new tag
        }
    }
    return delta;
}

} // namespace blockdedup
