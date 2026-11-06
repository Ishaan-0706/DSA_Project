#include "blockdedup/metadata.hpp"
#ifdef BLOCKDEDUP_HAS_EXIV2
#include <exiv2/exiv2.hpp>
#endif
#include <zlib.h>

#include <filesystem>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <iostream>
#include <fstream>
#include <vector>
#include <cstring>
#include <array>
#include <algorithm>
#include <random>
#include <cctype>

namespace fs = std::filesystem;

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

// ============================================================================
// Internal SHA-256 Implementation (RFC 6234 standard)
// ============================================================================

class Sha256 {
private:
    uint32_t state[8];
    uint64_t count;
    uint8_t buffer[64];

    static inline uint32_t rotr(uint32_t x, uint32_t n) {
        return (x >> n) | (x << (32 - n));
    }
    static inline uint32_t ch(uint32_t x, uint32_t y, uint32_t z) {
        return (x & y) ^ (~x & z);
    }
    static inline uint32_t maj(uint32_t x, uint32_t y, uint32_t z) {
        return (x & y) ^ (x & z) ^ (y & z);
    }
    static inline uint32_t sig0(uint32_t x) {
        return rotr(x, 2) ^ rotr(x, 13) ^ rotr(x, 22);
    }
    static inline uint32_t sig1(uint32_t x) {
        return rotr(x, 6) ^ rotr(x, 11) ^ rotr(x, 25);
    }
    static inline uint32_t gam0(uint32_t x) {
        return rotr(x, 7) ^ rotr(x, 18) ^ (x >> 3);
    }
    static inline uint32_t gam1(uint32_t x) {
        return rotr(x, 17) ^ rotr(x, 19) ^ (x >> 10);
    }

    void transform(const uint8_t data[64]) {
        static const uint32_t K[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
        };

        uint32_t W[64];
        for (int t = 0; t < 16; ++t) {
            W[t] = (static_cast<uint32_t>(data[t * 4]) << 24) |
                   (static_cast<uint32_t>(data[t * 4 + 1]) << 16) |
                   (static_cast<uint32_t>(data[t * 4 + 2]) << 8) |
                   (static_cast<uint32_t>(data[t * 4 + 3]));
        }
        for (int t = 16; t < 64; ++t) {
            W[t] = gam1(W[t - 2]) + W[t - 7] + gam0(W[t - 15]) + W[t - 16];
        }

        uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
        uint32_t e = state[4], f = state[5], g = state[6], h = state[7];

        for (int t = 0; t < 64; ++t) {
            uint32_t T1 = h + sig1(e) + ch(e, f, g) + K[t] + W[t];
            uint32_t T2 = sig0(a) + maj(a, b, c);
            h = g; g = f; f = e; e = d + T1;
            d = c; c = b; b = a; a = T1 + T2;
        }

        state[0] += a; state[1] += b; state[2] += c; state[3] += d;
        state[4] += e; state[5] += f; state[6] += g; state[7] += h;
    }

public:
    Sha256() { reset(); }

    void reset() {
        state[0] = 0x6a09e667; state[1] = 0xbb67ae85;
        state[2] = 0x3c6ef372; state[3] = 0xa54ff53a;
        state[4] = 0x510e527f; state[5] = 0x9b05688c;
        state[6] = 0x1f83d9ab; state[7] = 0x5be0cd19;
        count = 0;
    }

    void update(const uint8_t* data, size_t len) {
        size_t index = (count >> 3) & 0x3F;
        count += static_cast<uint64_t>(len) << 3;
        size_t partLen = 64 - index;
        size_t i = 0;

        if (len >= partLen) {
            std::memcpy(&buffer[index], data, partLen);
            transform(buffer);
            for (i = partLen; i + 63 < len; i += 64) {
                transform(&data[i]);
            }
            index = 0;
        }
        std::memcpy(&buffer[index], &data[i], len - i);
    }

    std::string finalize() {
        uint8_t finalCount[8];
        for (int i = 0; i < 8; ++i) {
            finalCount[i] = static_cast<uint8_t>((count >> ((7 - i) * 8)) & 0xFF);
        }

        uint8_t pad = 0x80;
        update(&pad, 1);
        uint8_t zero = 0x00;
        while ((count & 0x1F8) != 0x1C0) {
            update(&zero, 1);
        }
        update(finalCount, 8);

        std::ostringstream ss;
        ss << std::hex << std::setfill('0');
        for (int i = 0; i < 8; ++i) {
            ss << std::setw(8) << state[i];
        }
        return ss.str();
    }
};

// ============================================================================
// PKZIP Binary Structures (Standard Little-Endian)
// ============================================================================

#pragma pack(push, 1)
struct ZipLocalHeader {
    uint32_t signature = 0x04034b50;
    uint16_t versionNeeded = 20;
    uint16_t flags = 0;
    uint16_t method = 8; // 8 = Deflate, 0 = Stored
    uint16_t modTime = 0;
    uint16_t modDate = 0;
    uint32_t crc32Val = 0;
    uint32_t compressedSize = 0;
    uint32_t uncompressedSize = 0;
    uint16_t fileNameLength = 0;
    uint16_t extraFieldLength = 0;
};

struct ZipCentralHeader {
    uint32_t signature = 0x02014b50;
    uint16_t versionMadeBy = 20;
    uint16_t versionNeeded = 20;
    uint16_t flags = 0;
    uint16_t method = 8;
    uint16_t modTime = 0;
    uint16_t modDate = 0;
    uint32_t crc32Val = 0;
    uint32_t compressedSize = 0;
    uint32_t uncompressedSize = 0;
    uint16_t fileNameLength = 0;
    uint16_t extraFieldLength = 0;
    uint16_t commentLength = 0;
    uint16_t diskNumberStart = 0;
    uint16_t internalAttrs = 0;
    uint32_t externalAttrs = 0;
    uint32_t localHeaderOffset = 0;
};

struct ZipEOCD {
    uint32_t signature = 0x06054b50;
    uint16_t diskNumber = 0;
    uint16_t startDisk = 0;
    uint16_t numEntriesThisDisk = 0;
    uint16_t totalEntries = 0;
    uint32_t centralDirSize = 0;
    uint32_t centralDirOffset = 0;
    uint16_t commentLength = 0;
};
#pragma pack(pop)

bool deflateRaw(const std::vector<uint8_t>& src, std::vector<uint8_t>& dst) {
    if (src.empty()) {
        dst.clear();
        return true;
    }

    z_stream strm;
    std::memset(&strm, 0, sizeof(strm));
    if (deflateInit2(&strm, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        return false;
    }

    dst.resize(deflateBound(&strm, static_cast<uLong>(src.size())));
    strm.next_in = const_cast<Bytef*>(src.data());
    strm.avail_in = static_cast<uInt>(src.size());
    strm.next_out = dst.data();
    strm.avail_out = static_cast<uInt>(dst.size());

    int ret = deflate(&strm, Z_FINISH);
    if (ret != Z_STREAM_END) {
        deflateEnd(&strm);
        return false;
    }

    dst.resize(strm.total_out);
    deflateEnd(&strm);
    return true;
}

bool inflateRaw(const uint8_t* src, size_t srcSize, std::vector<uint8_t>& dst, size_t uncompressedSize) {
    dst.resize(uncompressedSize);
    if (uncompressedSize == 0) return true;

    z_stream strm;
    std::memset(&strm, 0, sizeof(strm));
    if (inflateInit2(&strm, -MAX_WBITS) != Z_OK) {
        return false;
    }

    strm.next_in = const_cast<Bytef*>(src);
    strm.avail_in = static_cast<uInt>(srcSize);
    strm.next_out = dst.data();
    strm.avail_out = static_cast<uInt>(dst.size());

    int ret = inflate(&strm, Z_FINISH);
    if (ret != Z_STREAM_END && ret != Z_OK) {
        inflateEnd(&strm);
        return false;
    }

    dst.resize(strm.total_out);
    inflateEnd(&strm);
    return (dst.size() == uncompressedSize);
}

std::string toZipPath(const fs::path& relPath) {
    std::string s = relPath.generic_string();
    return s;
}

} // namespace

// ============================================================================
// MetadataExtractor Implementation (Preserved Original Functionality)
// ============================================================================

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

        const Exiv2::XmpData& xmpData = image->xmpData();
        for (auto it = xmpData.begin(); it != xmpData.end(); ++it) {
            meta.allTags[it->key()] = it->value().toString();
        }
#endif

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
            delta[k] = v;
        }
    }
    return delta;
}

// ============================================================================
// MetadataArchive Implementation
// ============================================================================

std::string MetadataArchive::ensureMetaExtension(const std::string& path) {
    fs::path p(path);
    std::string ext = p.extension().string();
    std::string lowerExt = ext;
    for (auto& c : lowerExt) c = std::tolower(c);

    if (lowerExt == CUSTOM_ARCHIVE_EXTENSION) {
        return p.string();
    }
    if (lowerExt == ZIP_ARCHIVE_EXTENSION) {
        p.replace_extension(CUSTOM_ARCHIVE_EXTENSION);
        return p.string();
    }
    return p.string() + CUSTOM_ARCHIVE_EXTENSION;
}

std::string MetadataArchive::sanitizeOutputPath(const std::string& requestedPath, bool preventOverwrite) {
    std::string target = ensureMetaExtension(requestedPath);
    if (!preventOverwrite || !fs::exists(target)) {
        return target;
    }

    fs::path orig(target);
    fs::path parent = orig.parent_path();
    std::string stem = orig.stem().string();
    std::string ext = orig.extension().string();

    int counter = 1;
    while (true) {
        std::ostringstream ss;
        ss << stem << "_" << std::setw(3) << std::setfill('0') << counter << ext;
        fs::path candidate = parent.empty() ? fs::path(ss.str()) : (parent / ss.str());
        if (!fs::exists(candidate)) {
            return candidate.string();
        }
        counter++;
    }
}

std::string MetadataArchive::computeSha256(const uint8_t* data, size_t length) {
    Sha256 sha;
    sha.update(data, length);
    return sha.finalize();
}

std::string MetadataArchive::computeSha256(const std::string& filepath) {
    std::ifstream file(filepath, std::ios::binary);
    if (!file.is_open()) return "";

    Sha256 sha;
    std::vector<uint8_t> buffer(64 * 1024);
    while (file.good()) {
        file.read(reinterpret_cast<char*>(buffer.data()), buffer.size());
        std::streamsize bytes = file.gcount();
        if (bytes > 0) {
            sha.update(buffer.data(), static_cast<size_t>(bytes));
        }
    }
    return sha.finalize();
}

bool MetadataArchive::isSafeEntryPath(const std::string& entryPath) {
    if (entryPath.empty()) return false;

    // Reject control characters
    for (char c : entryPath) {
        if (static_cast<unsigned char>(c) < 32) return false;
    }

    std::string normalized = entryPath;
    for (char& c : normalized) {
        if (c == '\\') c = '/';
    }

    // Reject leading slash (absolute unix or root-relative windows)
    if (normalized.front() == '/') return false;

    // Reject drive letters and alternate data streams
    if (normalized.find(':') != std::string::npos) return false;

    // Reject UNC path prefix
    if (normalized.rfind("//", 0) == 0) return false;

    // Reject directory traversal segments
    fs::path p(normalized);
    for (const auto& part : p) {
        std::string s = part.string();
        if (s == "..") return false;

        // Check Windows reserved device names
        std::string stem = part.stem().string();
        std::string upperStem = stem;
        for (char& c : upperStem) c = std::toupper(c);
        if (upperStem == "CON" || upperStem == "PRN" || upperStem == "AUX" || upperStem == "NUL" ||
            (upperStem.size() == 4 && (upperStem.rfind("COM", 0) == 0 || upperStem.rfind("LPT", 0) == 0) &&
             std::isdigit(upperStem[3]))) {
            return false;
        }
    }

    return true;
}

bool MetadataArchive::isValidArchive(const std::string& archivePath) {
    std::error_code ec;
    if (!fs::exists(archivePath, ec) || !fs::is_regular_file(archivePath, ec)) {
        return false;
    }
    auto sz = fs::file_size(archivePath, ec);
    if (ec || sz < sizeof(ZipEOCD)) {
        return false;
    }

    std::ifstream in(archivePath, std::ios::binary);
    if (!in.is_open()) return false;

    // Locate EOCD record in last 65KB
    size_t searchSize = std::min<size_t>(65535 + sizeof(ZipEOCD), sz);
    in.seekg(-static_cast<std::streamoff>(searchSize), std::ios::end);
    if (!in.good()) return false;

    std::vector<char> tail(searchSize);
    in.read(tail.data(), searchSize);
    if (!in.good() && !in.eof()) return false;

    size_t eocdOffset = std::string::npos;
    for (size_t i = searchSize - sizeof(ZipEOCD) + 1; i > 0; --i) {
        if (std::memcmp(&tail[i - 1], "\x50\x4b\x05\x06", 4) == 0) {
            eocdOffset = (sz - searchSize) + (i - 1);
            break;
        }
    }

    if (eocdOffset == std::string::npos) {
        return false;
    }

    in.seekg(eocdOffset, std::ios::beg);
    ZipEOCD eocd;
    in.read(reinterpret_cast<char*>(&eocd), sizeof(eocd));
    if (!in.good() || eocd.signature != 0x06054b50) {
        return false;
    }

    // Validate EOCD bounds
    if (eocd.diskNumber != 0 || eocd.startDisk != 0 ||
        eocd.numEntriesThisDisk != eocd.totalEntries) {
        return false;
    }

    uint64_t cdStart = eocd.centralDirOffset;
    uint64_t cdSize = eocd.centralDirSize;
    if (cdStart + cdSize > eocdOffset || cdStart + cdSize > sz) {
        return false;
    }

    // Traverse all Central Directory entries and validate their local headers
    in.seekg(cdStart, std::ios::beg);
    uint32_t entriesCount = 0;

    while (entriesCount < eocd.totalEntries) {
        if (!in.good()) return false;
        std::streampos curPos = in.tellg();
        if (curPos < 0 || static_cast<uint64_t>(curPos) + sizeof(ZipCentralHeader) > cdStart + cdSize) {
            return false;
        }

        ZipCentralHeader ch;
        in.read(reinterpret_cast<char*>(&ch), sizeof(ch));
        if (!in.good() || ch.signature != 0x02014b50) {
            return false;
        }

        if (ch.fileNameLength == 0) return false;
        uint64_t entryTotalLen = sizeof(ZipCentralHeader) + ch.fileNameLength + ch.extraFieldLength + ch.commentLength;
        if (static_cast<uint64_t>(curPos) + entryTotalLen > cdStart + cdSize) {
            return false;
        }

        std::string filename(ch.fileNameLength, '\0');
        in.read(filename.data(), ch.fileNameLength);
        if (!in.good()) return false;

        in.seekg(ch.extraFieldLength + ch.commentLength, std::ios::cur);
        std::streampos nextCdPos = in.tellg();

        if (static_cast<uint64_t>(ch.localHeaderOffset) + sizeof(ZipLocalHeader) > sz) {
            return false;
        }

        in.seekg(ch.localHeaderOffset, std::ios::beg);
        ZipLocalHeader lh;
        in.read(reinterpret_cast<char*>(&lh), sizeof(lh));
        if (!in.good() || lh.signature != 0x04034b50) {
            return false;
        }

        uint64_t dataEnd = static_cast<uint64_t>(ch.localHeaderOffset) + sizeof(ZipLocalHeader) +
                           lh.fileNameLength + lh.extraFieldLength + ch.compressedSize;
        if (dataEnd > sz) {
            return false;
        }

        if (ch.method != 0 && ch.method != 8) {
            return false;
        }

        entriesCount++;
        in.seekg(nextCdPos, std::ios::beg);
    }

    return (entriesCount == eocd.totalEntries);
}

bool MetadataArchive::isCustomMetaArchive(const std::string& archivePath) {
    fs::path p(archivePath);
    std::string ext = p.extension().string();
    for (auto& c : ext) c = std::tolower(c);
    if (ext != CUSTOM_ARCHIVE_EXTENSION) return false;
    return isValidArchive(archivePath);
}

bool MetadataArchive::writeManifest(const std::string& directory, const ArchiveManifest& manifest) {
    fs::path p = fs::path(directory) / ARCHIVE_MANIFEST_FILENAME;
    std::ofstream out(p, std::ios::trunc);
    if (!out.is_open()) return false;

    out << "{\n";
    out << "  \"formatVersion\": \"" << manifest.formatVersion << "\",\n";
    out << "  \"formatTag\": \"" << manifest.formatTag << "\",\n";
    out << "  \"creationTimestamp\": " << manifest.creationTimestamp << ",\n";
    out << "  \"totalOriginalImages\": " << manifest.totalOriginalImages << ",\n";
    out << "  \"tolerance\": " << static_cast<int>(manifest.tolerance) << ",\n";
    out << "  \"entries\": [\n";
    for (size_t i = 0; i < manifest.entries.size(); ++i) {
        const auto& e = manifest.entries[i];
        out << "    {\n";
        out << "      \"originalFilename\": \"" << e.originalFilename << "\",\n";
        out << "      \"clusterFolder\": \"" << e.clusterFolder << "\",\n";
        out << "      \"originalSizeBytes\": " << e.originalSizeBytes << ",\n";
        out << "      \"sha256Checksum\": \"" << e.sha256Checksum << "\",\n";
        out << "      \"isMaster\": " << (e.isMaster ? "true" : "false") << ",\n";
        out << "      \"masterReference\": \"" << e.masterReference << "\"\n";
        out << "    }" << (i + 1 < manifest.entries.size() ? "," : "") << "\n";
    }
    out << "  ]\n";
    out << "}\n";
    return out.good();
}

std::optional<ArchiveManifest> MetadataArchive::readManifest(const std::string& directory) {
    fs::path p = fs::path(directory) / ARCHIVE_MANIFEST_FILENAME;
    if (!fs::exists(p)) return std::nullopt;

    std::ifstream in(p);
    if (!in.is_open()) return std::nullopt;

    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    ArchiveManifest manifest;

    auto extractStr = [&](const std::string& key, const std::string& src) -> std::string {
        std::string pattern = "\"" + key + "\": \"";
        auto pos = src.find(pattern);
        if (pos == std::string::npos) return "";
        pos += pattern.size();
        auto endPos = src.find("\"", pos);
        if (endPos == std::string::npos) return "";
        return src.substr(pos, endPos - pos);
    };

    auto extractInt = [&](const std::string& key, const std::string& src) -> int64_t {
        std::string pattern = "\"" + key + "\": ";
        auto pos = src.find(pattern);
        if (pos == std::string::npos) return 0;
        pos += pattern.size();
        try {
            return std::stoll(src.substr(pos));
        } catch (...) {
            return 0;
        }
    };

    manifest.formatVersion = extractStr("formatVersion", content);
    manifest.formatTag = extractStr("formatTag", content);
    manifest.creationTimestamp = extractInt("creationTimestamp", content);
    manifest.totalOriginalImages = static_cast<size_t>(extractInt("totalOriginalImages", content));
    manifest.tolerance = static_cast<uint8_t>(extractInt("tolerance", content));

    auto entriesPos = content.find("\"entries\": [");
    if (entriesPos != std::string::npos) {
        size_t cur = entriesPos;
        while (true) {
            auto itemStart = content.find("{", cur);
            if (itemStart == std::string::npos) break;
            auto itemEnd = content.find("}", itemStart);
            if (itemEnd == std::string::npos) break;

            std::string itemStr = content.substr(itemStart, itemEnd - itemStart + 1);
            ImageManifestEntry e;
            e.originalFilename = extractStr("originalFilename", itemStr);
            e.clusterFolder = extractStr("clusterFolder", itemStr);
            e.originalSizeBytes = static_cast<size_t>(extractInt("originalSizeBytes", itemStr));
            e.sha256Checksum = extractStr("sha256Checksum", itemStr);
            e.isMaster = (itemStr.find("\"isMaster\": true") != std::string::npos);
            e.masterReference = extractStr("masterReference", itemStr);

            if (!e.originalFilename.empty()) {
                manifest.entries.push_back(e);
            }
            cur = itemEnd + 1;
        }
    }
    return manifest;
}

bool MetadataArchive::verifyRestoredImages(
    const std::string& restoredDir,
    const ArchiveManifest& manifest,
    std::vector<std::string>& mismatchedFiles
) {
    mismatchedFiles.clear();
    for (const auto& entry : manifest.entries) {
        fs::path p = fs::path(restoredDir) / entry.originalFilename;
        if (!fs::exists(p)) {
            mismatchedFiles.push_back(entry.originalFilename + " (missing)");
            continue;
        }
        if (entry.sha256Checksum.empty()) continue;

        std::string actualHash = computeSha256(p.string());
        // For lossless archives (tolerance == 0), master files, or standalone copies, SHA-256 must match exactly
        if (manifest.tolerance == 0 || entry.isMaster || entry.masterReference.empty()) {
            if (actualHash != entry.sha256Checksum) {
                mismatchedFiles.push_back(entry.originalFilename + " (checksum mismatch)");
            }
        }
    }
    return mismatchedFiles.empty();
}

bool MetadataArchive::createArchiveFromDirectory(
    const std::string& stagingDir,
    const std::string& outputArchivePath,
    const ArchiveManifest* manifest,
    std::string* errorMsg
) {
    if (!fs::exists(stagingDir) || !fs::is_directory(stagingDir)) {
        if (errorMsg) *errorMsg = "Source staging directory does not exist: " + stagingDir;
        return false;
    }

    if (manifest) {
        if (!writeManifest(stagingDir, *manifest)) {
            if (errorMsg) *errorMsg = "Failed to write archive manifest.";
            return false;
        }
    }

    std::string finalTarget = ensureMetaExtension(outputArchivePath);

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint32_t> dist(100000, 999999);
    fs::path finalPath(finalTarget);
    fs::path tempArchivePath = finalPath.parent_path() / (finalPath.stem().string() + ".tmp_" + std::to_string(dist(gen)) + ".zip");

    std::ofstream zipOut(tempArchivePath, std::ios::binary | std::ios::trunc);
    if (!zipOut.is_open()) {
        if (errorMsg) *errorMsg = "Cannot create temporary packaging file: " + tempArchivePath.string();
        return false;
    }

    struct StoredEntry {
        std::string zipPath;
        uint32_t crc32Val = 0;
        uint32_t compressedSize = 0;
        uint32_t uncompressedSize = 0;
        uint16_t method = 8;
        uint32_t localHeaderOffset = 0;
        bool isDirectory = false;
    };
    std::vector<StoredEntry> entries;

    try {
        std::error_code ec;
        for (const auto& dirEntry : fs::recursive_directory_iterator(stagingDir, fs::directory_options::skip_permission_denied, ec)) {
            fs::path rel = fs::relative(dirEntry.path(), stagingDir, ec);
            std::string zPath = toZipPath(rel);

            if (dirEntry.is_directory()) {
                if (!zPath.empty() && zPath.back() != '/') {
                    zPath += '/';
                }
                StoredEntry se;
                se.zipPath = zPath;
                se.isDirectory = true;
                se.method = 0;
                se.localHeaderOffset = static_cast<uint32_t>(zipOut.tellp());

                ZipLocalHeader lh;
                lh.method = 0;
                lh.crc32Val = 0;
                lh.compressedSize = 0;
                lh.uncompressedSize = 0;
                lh.fileNameLength = static_cast<uint16_t>(zPath.size());
                lh.extraFieldLength = 0;

                zipOut.write(reinterpret_cast<const char*>(&lh), sizeof(lh));
                zipOut.write(zPath.data(), zPath.size());
                if (!zipOut.good()) {
                    zipOut.close();
                    fs::remove(tempArchivePath, ec);
                    if (errorMsg) *errorMsg = "Disk write error while writing directory header: " + zPath;
                    return false;
                }
                entries.push_back(se);
                continue;
            }

            if (!dirEntry.is_regular_file()) continue;

            std::ifstream fileIn(dirEntry.path(), std::ios::binary);
            if (!fileIn.is_open()) {
                zipOut.close();
                fs::remove(tempArchivePath, ec);
                if (errorMsg) *errorMsg = "Cannot read source file for packaging: " + dirEntry.path().string();
                return false;
            }

            std::vector<uint8_t> rawData((std::istreambuf_iterator<char>(fileIn)), std::istreambuf_iterator<char>());
            fileIn.close();

            uLong crc = crc32(0L, Z_NULL, 0);
            if (!rawData.empty()) {
                crc = crc32(crc, rawData.data(), static_cast<uInt>(rawData.size()));
            }

            std::vector<uint8_t> compData;
            bool useDeflate = deflateRaw(rawData, compData) && (compData.size() < rawData.size());
            uint16_t method = useDeflate ? 8 : 0;
            const uint8_t* writePtr = useDeflate ? compData.data() : rawData.data();
            size_t writeSize = useDeflate ? compData.size() : rawData.size();

            StoredEntry se;
            se.zipPath = zPath;
            se.isDirectory = false;
            se.crc32Val = static_cast<uint32_t>(crc);
            se.uncompressedSize = static_cast<uint32_t>(rawData.size());
            se.compressedSize = static_cast<uint32_t>(writeSize);
            se.method = method;
            se.localHeaderOffset = static_cast<uint32_t>(zipOut.tellp());

            ZipLocalHeader lh;
            lh.method = method;
            lh.crc32Val = se.crc32Val;
            lh.compressedSize = se.compressedSize;
            lh.uncompressedSize = se.uncompressedSize;
            lh.fileNameLength = static_cast<uint16_t>(zPath.size());
            lh.extraFieldLength = 0;

            zipOut.write(reinterpret_cast<const char*>(&lh), sizeof(lh));
            zipOut.write(zPath.data(), zPath.size());
            if (writeSize > 0) {
                zipOut.write(reinterpret_cast<const char*>(writePtr), writeSize);
            }
            if (!zipOut.good()) {
                zipOut.close();
                fs::remove(tempArchivePath, ec);
                if (errorMsg) *errorMsg = "Disk write error while packaging file: " + zPath;
                return false;
            }
            entries.push_back(se);
        }

        uint32_t centralDirOffset = static_cast<uint32_t>(zipOut.tellp());
        for (const auto& se : entries) {
            ZipCentralHeader ch;
            ch.method = se.method;
            ch.crc32Val = se.crc32Val;
            ch.compressedSize = se.compressedSize;
            ch.uncompressedSize = se.uncompressedSize;
            ch.fileNameLength = static_cast<uint16_t>(se.zipPath.size());
            ch.extraFieldLength = 0;
            ch.commentLength = 0;
            ch.externalAttrs = se.isDirectory ? 0x10 : 0x20;
            ch.localHeaderOffset = se.localHeaderOffset;

            zipOut.write(reinterpret_cast<const char*>(&ch), sizeof(ch));
            zipOut.write(se.zipPath.data(), se.zipPath.size());
            if (!zipOut.good()) {
                zipOut.close();
                fs::remove(tempArchivePath, ec);
                if (errorMsg) *errorMsg = "Disk write error while writing central directory.";
                return false;
            }
        }
        uint32_t centralDirSize = static_cast<uint32_t>(zipOut.tellp()) - centralDirOffset;

        ZipEOCD eocd;
        eocd.numEntriesThisDisk = static_cast<uint16_t>(entries.size());
        eocd.totalEntries = static_cast<uint16_t>(entries.size());
        eocd.centralDirSize = centralDirSize;
        eocd.centralDirOffset = centralDirOffset;

        zipOut.write(reinterpret_cast<const char*>(&eocd), sizeof(eocd));
        if (!zipOut.good()) {
            zipOut.close();
            fs::remove(tempArchivePath, ec);
            if (errorMsg) *errorMsg = "Disk write error while finalizing archive.";
            return false;
        }
        zipOut.flush();
        zipOut.close();

        // Complete structural and boundary validation of the temporary archive
        if (!isValidArchive(tempArchivePath.string())) {
            fs::remove(tempArchivePath, ec);
            if (errorMsg) *errorMsg = "Complete structural validation failed on generated archive.";
            return false;
        }

        if (finalPath.has_parent_path()) {
            fs::create_directories(finalPath.parent_path(), ec);
        }
        fs::rename(tempArchivePath, finalPath, ec);
        if (ec) {
            fs::copy_file(tempArchivePath, finalPath, fs::copy_options::overwrite_existing, ec);
            fs::remove(tempArchivePath, ec);
        }

        if (!isValidArchive(finalPath.string())) {
            fs::remove(finalPath, ec);
            if (errorMsg) *errorMsg = "Archive verification failed after publishing.";
            return false;
        }

        return true;

    } catch (const std::exception& e) {
        std::error_code ec;
        fs::remove(tempArchivePath, ec);
        if (errorMsg) *errorMsg = std::string("Packaging exception: ") + e.what();
        return false;
    }
}

bool MetadataArchive::extractArchive(
    const std::string& archivePath,
    const std::string& destinationDir,
    std::string* errorMsg
) {
    if (!isValidArchive(archivePath)) {
        if (errorMsg) *errorMsg = "Archive file is invalid, corrupted, or truncated: " + archivePath;
        return false;
    }

    std::ifstream in(archivePath, std::ios::binary);
    if (!in.is_open()) {
        if (errorMsg) *errorMsg = "Failed to open archive: " + archivePath;
        return false;
    }

    in.seekg(0, std::ios::end);
    size_t fileSize = static_cast<size_t>(in.tellg());

    size_t searchSize = std::min<size_t>(65535 + sizeof(ZipEOCD), fileSize);
    in.seekg(-static_cast<std::streamoff>(searchSize), std::ios::end);
    std::vector<char> buffer(searchSize);
    in.read(buffer.data(), searchSize);

    size_t eocdOffset = std::string::npos;
    for (size_t i = searchSize - sizeof(ZipEOCD) + 1; i > 0; --i) {
        if (std::memcmp(&buffer[i - 1], "\x50\x4b\x05\x06", 4) == 0) {
            eocdOffset = (fileSize - searchSize) + (i - 1);
            break;
        }
    }

    if (eocdOffset == std::string::npos) {
        if (errorMsg) *errorMsg = "Corrupted archive: End of Central Directory record not found.";
        return false;
    }

    in.seekg(eocdOffset, std::ios::beg);
    ZipEOCD eocd;
    in.read(reinterpret_cast<char*>(&eocd), sizeof(eocd));
    if (eocd.signature != 0x06054b50) {
        if (errorMsg) *errorMsg = "Corrupted archive header in: " + archivePath;
        return false;
    }

    std::error_code ec;
    fs::create_directories(destinationDir, ec);
    fs::path canonicalDest = fs::weakly_canonical(destinationDir, ec);

    std::vector<fs::path> extractedFiles;
    auto cleanupExtracted = [&]() {
        for (auto it = extractedFiles.rbegin(); it != extractedFiles.rend(); ++it) {
            std::error_code rmEc;
            fs::remove(*it, rmEc);
        }
    };

    in.seekg(eocd.centralDirOffset, std::ios::beg);
    for (uint16_t i = 0; i < eocd.totalEntries; ++i) {
        ZipCentralHeader ch;
        in.read(reinterpret_cast<char*>(&ch), sizeof(ch));
        if (ch.signature != 0x02014b50) {
            cleanupExtracted();
            if (errorMsg) *errorMsg = "Corrupted Central Directory header in: " + archivePath;
            return false;
        }

        std::string filename(ch.fileNameLength, '\0');
        in.read(filename.data(), ch.fileNameLength);

        in.seekg(ch.extraFieldLength + ch.commentLength, std::ios::cur);
        std::streampos nextCdPos = in.tellg();

        // Safe path validation
        if (!isSafeEntryPath(filename)) {
            cleanupExtracted();
            if (errorMsg) *errorMsg = "Unsafe archive entry path rejected: " + filename;
            return false;
        }

        fs::path outPath = fs::path(destinationDir) / filename;
        fs::path canonicalOut = fs::weakly_canonical(outPath, ec);
        std::string destStr = canonicalDest.generic_string();
        std::string outStr = canonicalOut.generic_string();

        if (outStr.size() < destStr.size() || outStr.compare(0, destStr.size(), destStr) != 0) {
            cleanupExtracted();
            if (errorMsg) *errorMsg = "Path traversal boundary escape rejected: " + filename;
            return false;
        }
        if (outStr.size() > destStr.size() && outStr[destStr.size()] != '/') {
            cleanupExtracted();
            if (errorMsg) *errorMsg = "Prefix collision path rejected: " + filename;
            return false;
        }

        if (!filename.empty() && filename.back() == '/') {
            fs::create_directories(outPath, ec);
            extractedFiles.push_back(outPath);
            in.seekg(nextCdPos, std::ios::beg);
            continue;
        }

        fs::create_directories(outPath.parent_path(), ec);

        in.seekg(ch.localHeaderOffset, std::ios::beg);
        ZipLocalHeader lh;
        in.read(reinterpret_cast<char*>(&lh), sizeof(lh));
        if (lh.signature != 0x04034b50) {
            cleanupExtracted();
            if (errorMsg) *errorMsg = "Corrupted Local File header in: " + archivePath;
            return false;
        }
        in.seekg(lh.fileNameLength + lh.extraFieldLength, std::ios::cur);

        std::vector<uint8_t> compressedData(ch.compressedSize);
        if (ch.compressedSize > 0) {
            in.read(reinterpret_cast<char*>(compressedData.data()), ch.compressedSize);
            if (!in.good()) {
                cleanupExtracted();
                if (errorMsg) *errorMsg = "Failed to read compressed payload for: " + filename;
                return false;
            }
        }

        std::vector<uint8_t> uncompressedData;
        if (ch.method == 0) {
            uncompressedData = std::move(compressedData);
        } else if (ch.method == 8) {
            if (!inflateRaw(compressedData.data(), compressedData.size(), uncompressedData, ch.uncompressedSize)) {
                cleanupExtracted();
                if (errorMsg) *errorMsg = "Decompression failed for entry: " + filename;
                return false;
            }
        } else {
            cleanupExtracted();
            if (errorMsg) *errorMsg = "Unsupported compression method (" + std::to_string(ch.method) + ") in entry: " + filename;
            return false;
        }

        uLong computedCrc = crc32(0L, Z_NULL, 0);
        if (!uncompressedData.empty()) {
            computedCrc = crc32(computedCrc, uncompressedData.data(), static_cast<uInt>(uncompressedData.size()));
        }
        if (static_cast<uint32_t>(computedCrc) != ch.crc32Val) {
            cleanupExtracted();
            if (errorMsg) *errorMsg = "CRC-32 checksum mismatch for entry: " + filename;
            return false;
        }

        std::ofstream outFile(outPath, std::ios::binary | std::ios::trunc);
        if (!outFile.is_open()) {
            cleanupExtracted();
            if (errorMsg) *errorMsg = "Failed to write extracted file: " + outPath.string();
            return false;
        }
        if (!uncompressedData.empty()) {
            outFile.write(reinterpret_cast<const char*>(uncompressedData.data()), uncompressedData.size());
            if (!outFile.good()) {
                outFile.close();
                cleanupExtracted();
                if (errorMsg) *errorMsg = "Disk write error while extracting: " + outPath.string();
                return false;
            }
        }
        outFile.close();
        extractedFiles.push_back(outPath);

        in.seekg(nextCdPos, std::ios::beg);
    }

    return true;
}

} // namespace blockdedup
