#pragma once

#include <string>
#include <vector>
#include <map>
#include <optional>
#include <chrono>
#include <cmath>
#include <cstdint>

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

// ============================================================================
// Archive Packaging & Custom Extension (.meta) Support
// ============================================================================

inline constexpr const char* CUSTOM_ARCHIVE_EXTENSION = ".meta";
inline constexpr const char* ZIP_ARCHIVE_EXTENSION = ".zip";
inline constexpr const char* ARCHIVE_MANIFEST_FILENAME = "metadata_manifest.json";

// Note on Confidentiality:
// The .meta archive format uses standard ZIP compression with a custom extension.
// It provides format obfuscation against accidental opening, but NOT cryptographic
// confidentiality. Archive utilities (e.g. unzip, 7-zip) can inspect inner filenames
// if renamed. Authenticated encryption (e.g. AES-256-GCM) is required for true confidentiality.
inline constexpr bool IS_CRYPTOGRAPHICALLY_CONFIDENTIAL = false;

struct ImageManifestEntry {
    std::string originalFilename;
    std::string clusterFolder;
    size_t originalSizeBytes = 0;
    std::string sha256Checksum;
    bool isMaster = false;
    std::string masterReference; // Master filename if this is a delta, empty if standalone
};

struct ArchiveManifest {
    std::string formatVersion = "1.0";
    std::string formatTag = "blockdedup.meta";
    int64_t creationTimestamp = 0;
    size_t totalOriginalImages = 0;
    uint8_t tolerance = 0;
    std::vector<ImageManifestEntry> entries;
};

class MetadataArchive {
public:
    static constexpr bool IS_CRYPTOGRAPHICALLY_CONFIDENTIAL = false;

    // Ensure filename ends with .meta, replacing .zip or appending if missing
    static std::string ensureMetaExtension(const std::string& path);

    // Prevent accidental overwriting of existing archives; generates non-colliding name if needed
    static std::string sanitizeOutputPath(const std::string& requestedPath, bool preventOverwrite = true);

    // Calculate SHA-256 hash for byte-level integrity verification
    static std::string computeSha256(const std::string& filepath);
    static std::string computeSha256(const uint8_t* data, size_t length);

    // Safe extraction path verification (rejects traversal, UNC, drive letters, absolute paths)
    static bool isSafeEntryPath(const std::string& entryPath);

    // Automatically package an existing generated directory into a ZIP archive saved with .meta extension
    static bool createArchiveFromDirectory(
        const std::string& stagingDir,
        const std::string& outputArchivePath,
        const ArchiveManifest* manifest = nullptr,
        std::string* errorMsg = nullptr
    );

    // Extract a .meta (or .zip) archive into destination directory with strict safety checks
    static bool extractArchive(
        const std::string& archivePath,
        const std::string& destinationDir,
        std::string* errorMsg = nullptr
    );

    // Format identification and complete structural validation
    static bool isValidArchive(const std::string& archivePath);
    static bool isCustomMetaArchive(const std::string& archivePath);

    // Manifest read/write/verification
    static bool writeManifest(const std::string& directory, const ArchiveManifest& manifest);
    static std::optional<ArchiveManifest> readManifest(const std::string& directory);
    static bool verifyRestoredImages(
        const std::string& restoredDir,
        const ArchiveManifest& manifest,
        std::vector<std::string>& mismatchedFiles
    );
};

} // namespace blockdedup
