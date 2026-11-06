#pragma once

#include <string>
#include <vector>
#include <map>
#include <optional>
#include <chrono>
#include <cmath>

namespace blockdedup {

struct GpsCoordinates {
    double latitude = 0.0;     // Decimal degrees (-90 to +90)
    double longitude = 0.0;    // Decimal degrees (-180 to +180)
    double altitude = 0.0;     // Meters above sea level
    bool valid = false;

    // Haversine distance in meters
    double distanceTo(const GpsCoordinates& other) const {
        if (!valid || !other.valid) return -1.0;
        constexpr double R = 6371000.0; // Earth radius in meters
        constexpr double PI = 3.14159265358979323846;
        double lat1 = latitude * PI / 180.0;
        double lat2 = other.latitude * PI / 180.0;
        double dLat = (other.latitude - latitude) * PI / 180.0;
        double dLon = (other.longitude - longitude) * PI / 180.0;

        double a = std::sin(dLat / 2.0) * std::sin(dLat / 2.0) +
                   std::cos(lat1) * std::cos(lat2) *
                   std::sin(dLon / 2.0) * std::sin(dLon / 2.0);
        double c = 2.0 * std::atan2(std::sqrt(a), std::sqrt(1.0 - a));
        return R * c;
    }
};

struct ImageMetadata {
    std::string filepath;
    std::string filename;
    std::string mimeType;
    int width = 0;
    int height = 0;
    size_t fileSize = 0;

    // Timestamp telemetry
    bool hasTimestamp = false;
    int64_t timestamp = 0; // Unix epoch seconds
    std::string timestampStr;

    // GPS telemetry
    GpsCoordinates gps;

    // Device telemetry
    std::string make;
    std::string model;
    std::string exposureTime;
    std::string fNumber;
    std::string iso;

    // Key-value store of all extracted EXIF/XMP tags for delta diffing
    std::map<std::string, std::string> allTags;
};

class MetadataExtractor {
public:
    static std::optional<ImageMetadata> extract(const std::string& filepath);

    // Compute key-value differences between target and base metadata
    static std::map<std::string, std::string> computeMetadataDelta(
        const ImageMetadata& target,
        const ImageMetadata& base
    );
};

} // namespace blockdedup
