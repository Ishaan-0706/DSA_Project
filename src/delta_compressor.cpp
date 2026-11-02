#include "blockdedup/delta_compressor.hpp"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "third_party/stb_image_write.h"
#include <zlib.h>
#include <fstream>
#include <iostream>
#include <cmath>
#include <cstring>
#include <filesystem>

namespace blockdedup {

namespace {

constexpr const char* BDELTA_MAGIC = "BDELTA03";
constexpr size_t MAGIC_LEN = 8;

} // namespace

void DeltaCompressor::expandMasterCanvas(
    ImageBuffer& master,
    const ImageBuffer& target,
    int dx, int dy
) {
    if (!master.isValid() || !target.isValid()) return;
    if (dx == 0 && dy == 0 && master.width == target.width && master.height == target.height) return;

    // Safety limit: only expand if offset is modest (<= 5% of frame dimensions)
    if (std::abs(dx) > master.width * 0.05 || std::abs(dy) > master.height * 0.05) {
        return; // Reject excessive offset to avoid bloat
    }

    int newMinX = std::min(0, dx);
    int newMaxX = std::max(master.width, dx + target.width);
    int newMinY = std::min(0, dy);
    int newMaxY = std::max(master.height, dy + target.height);

    int newW = newMaxX - newMinX;
    int newH = newMaxY - newMinY;

    if (newW <= master.width && newH <= master.height) return;

    // Reject canvas expansion if total pixel count increases by more than 10%
    if (static_cast<double>(newW) * newH > static_cast<double>(master.width) * master.height * 1.10) {
        return;
    }

    ImageBuffer expanded;
    expanded.width = newW;
    expanded.height = newH;
    expanded.channels = master.channels;
    expanded.data.assign(newW * newH * master.channels, 0);

    // Blit existing master into expanded canvas
    int masterOffsetX = -newMinX;
    int masterOffsetY = -newMinY;
    for (int y = 0; y < master.height; ++y) {
        size_t srcRow = y * master.width * master.channels;
        size_t dstRow = ((y + masterOffsetY) * newW + masterOffsetX) * master.channels;
        std::memcpy(&expanded.data[dstRow], &master.data[srcRow], master.width * master.channels);
    }

    // Stitch missing border pixels from target into expanded canvas
    int targetOffsetX = dx - newMinX;
    int targetOffsetY = dy - newMinY;
    for (int y = 0; y < target.height; ++y) {
        int eY = y + targetOffsetY;
        for (int x = 0; x < target.width; ++x) {
            int eX = x + targetOffsetX;
            size_t eIdx = (eY * newW + eX) * master.channels;
            if (expanded.data[eIdx] == 0 && expanded.data[eIdx + 1] == 0 && expanded.data[eIdx + 2] == 0) {
                size_t tIdx = (y * target.width + x) * target.channels;
                expanded.data[eIdx] = target.data[tIdx];
                expanded.data[eIdx + 1] = target.data[tIdx + 1];
                expanded.data[eIdx + 2] = target.data[tIdx + 2];
            }
        }
    }

    master = std::move(expanded);
}

bool DeltaCompressor::createDeltaWithMaster(
    const ImageBuffer& inMaster,
    const std::string& masterPath,
    const std::string& targetPath,
    const std::string& outputDeltaPath,
    uint8_t tolerance,
    SpatialAlignment alignment,
    CompressionReport* report
) {
    if (!inMaster.isValid()) {
        std::cerr << "Invalid master image provided.\n";
        return false;
    }

    ImageBuffer target = ImageSimilarityScanner::loadImage(targetPath, 3);
    if (!target.isValid()) {
        std::cerr << "Error loading target image: " << targetPath << "\n";
        return false;
    }

    // Apply rotation alignment if detected
    if (std::abs(alignment.angleDeg) > 0.01) {
        target = ImageSimilarityScanner::rotate(target, -alignment.angleDeg);
    }

    // Align master dimensions if necessary
    ImageBuffer masterAligned;
    const ImageBuffer* masterPtr = &inMaster;
    // Note: If master was canvas-expanded, do NOT downscale it back to target size;
    // the target registers within master's expanded canvas at (dx, dy).
    // Only downscale if master is smaller or differently proportioned and unshifted.
    if ((inMaster.width != target.width || inMaster.height != target.height) && 
        (inMaster.width < target.width || inMaster.height < target.height)) {
        masterAligned = ImageSimilarityScanner::downscale(inMaster, target.width, target.height);
        masterPtr = &masterAligned;
    }
    const ImageBuffer& master = *masterPtr;

    int W = target.width;
    int H = target.height;
    int C = 3;
    size_t totalPixels = static_cast<size_t>(W) * H;
    int dx = alignment.dx;
    int dy = alignment.dy;

    // Fast global exposure offset calculation to align atmospheric transparency / brightness drift
    long long diffSum = 0;
    size_t sampleStep = std::max<size_t>(1, totalPixels / 5000);
    size_t sampleCount = 0;
    for (int y = 0; y < H; y += static_cast<int>(std::sqrt(sampleStep))) {
        int my = y + dy;
        if (my < 0 || my >= master.height) continue;
        for (int x = 0; x < W; x += static_cast<int>(std::sqrt(sampleStep))) {
            int mx = x + dx;
            if (mx < 0 || mx >= master.width) continue;

            size_t tIdx = (y * W + x) * C;
            size_t mIdx = (my * master.width + mx) * C;
            diffSum += (static_cast<int>(target.data[tIdx]) - static_cast<int>(master.data[mIdx])) +
                       (static_cast<int>(target.data[tIdx + 1]) - static_cast<int>(master.data[mIdx + 1])) +
                       (static_cast<int>(target.data[tIdx + 2]) - static_cast<int>(master.data[mIdx + 2]));
            sampleCount += 3;
        }
    }
    int globalOffset = (sampleCount > 0) ? std::round(static_cast<double>(diffSum) / sampleCount) : 0;
    globalOffset = std::clamp(globalOffset, -127, 127);
    int8_t offset8 = static_cast<int8_t>(globalOffset);

    // Build binary mask and foreground stream
    size_t maskBytes = (totalPixels + 7) / 8;
    std::vector<uint8_t> mask(maskBytes, 0);
    std::vector<uint8_t> foreground;
    foreground.reserve(totalPixels * C / 4);

    size_t changedPixels = 0;

    for (int y = 0; y < H; ++y) {
        int my = y + dy;
        for (int x = 0; x < W; ++x) {
            int mx = x + dx;
            size_t i = y * W + x;
            size_t tIdx = i * C;

            if (mx < 0 || mx >= master.width || my < 0 || my >= master.height) {
                // Out of master canvas bound: must be saved as foreground
                mask[i / 8] |= (1 << (i % 8));
                foreground.push_back(target.data[tIdx]);
                foreground.push_back(target.data[tIdx + 1]);
                foreground.push_back(target.data[tIdx + 2]);
                changedPixels++;
                continue;
            }

            size_t mIdx = (my * master.width + mx) * C;
            int mR = std::clamp(static_cast<int>(master.data[mIdx]) + globalOffset, 0, 255);
            int mG = std::clamp(static_cast<int>(master.data[mIdx + 1]) + globalOffset, 0, 255);
            int mB = std::clamp(static_cast<int>(master.data[mIdx + 2]) + globalOffset, 0, 255);

            int diffR = static_cast<int>(target.data[tIdx]) - mR;
            int diffG = static_cast<int>(target.data[tIdx + 1]) - mG;
            int diffB = static_cast<int>(target.data[tIdx + 2]) - mB;
            int dr = std::abs(diffR);
            int dg = std::abs(diffG);
            int db = std::abs(diffB);

            // Dark-sky threshold
            bool isDarkSky = (master.data[mIdx] < 45 && master.data[mIdx + 1] < 45 && master.data[mIdx + 2] < 45) &&
                             (target.data[tIdx] < 45 && target.data[tIdx + 1] < 45 && target.data[tIdx + 2] < 45);
            int effTol = isDarkSky ? 45 : tolerance;

            if (dr > effTol || dg > effTol || db > effTol) {
                mask[i / 8] |= (1 << (i % 8));
                uint8_t resR = static_cast<uint8_t>(std::clamp(diffR + 128, 0, 255));
                uint8_t resG = static_cast<uint8_t>(std::clamp(diffG + 128, 0, 255));
                uint8_t resB = static_cast<uint8_t>(std::clamp(diffB + 128, 0, 255));
                foreground.push_back(resR);
                foreground.push_back(resG);
                foreground.push_back(resB);
                changedPixels++;
            }
        }
    }

    // Compute metadata delta
    auto metaMaster = MetadataExtractor::extract(masterPath);
    auto metaTarget = MetadataExtractor::extract(targetPath);
    std::string metaDeltaStr;
    if (metaMaster && metaTarget) {
        auto diff = MetadataExtractor::computeMetadataDelta(*metaTarget, *metaMaster);
        for (const auto& [k, v] : diff) {
            metaDeltaStr += k + "=" + v + "\n";
        }
    }

    // Assemble uncompressed stream
    // Format BDELTA03: W(4), H(4), C(4), tolerance(1), offset8(1), dx(2), dy(2), angle(4), changedPixels(4), metaLen(4)
    uint32_t metaLen = static_cast<uint32_t>(metaDeltaStr.size());
    uint32_t w32 = static_cast<uint32_t>(W);
    uint32_t h32 = static_cast<uint32_t>(H);
    uint32_t c32 = static_cast<uint32_t>(C);
    uint32_t changed32 = static_cast<uint32_t>(changedPixels);
    int16_t dx16 = static_cast<int16_t>(dx);
    int16_t dy16 = static_cast<int16_t>(dy);
    float angle32 = static_cast<float>(alignment.angleDeg);

    std::vector<uint8_t> uncompressed;
    auto appendU32 = [&](uint32_t v) {
        uint8_t b[4] = {
            static_cast<uint8_t>(v & 0xFF),
            static_cast<uint8_t>((v >> 8) & 0xFF),
            static_cast<uint8_t>((v >> 16) & 0xFF),
            static_cast<uint8_t>((v >> 24) & 0xFF)
        };
        uncompressed.insert(uncompressed.end(), b, b + 4);
    };

    appendU32(w32);
    appendU32(h32);
    appendU32(c32);
    uncompressed.push_back(tolerance);
    uncompressed.push_back(static_cast<uint8_t>(offset8));
    uncompressed.push_back(static_cast<uint8_t>(dx16 & 0xFF));
    uncompressed.push_back(static_cast<uint8_t>((dx16 >> 8) & 0xFF));
    uncompressed.push_back(static_cast<uint8_t>(dy16 & 0xFF));
    uncompressed.push_back(static_cast<uint8_t>((dy16 >> 8) & 0xFF));
    uint32_t angleBits;
    std::memcpy(&angleBits, &angle32, sizeof(angleBits));
    appendU32(angleBits);

    appendU32(changed32);
    appendU32(metaLen);
    uncompressed.insert(uncompressed.end(), metaDeltaStr.begin(), metaDeltaStr.end());
    uncompressed.insert(uncompressed.end(), mask.begin(), mask.end());
    uncompressed.insert(uncompressed.end(), foreground.begin(), foreground.end());

    // Compress with zlib Level 9 (Z_BEST_COMPRESSION)
    uLong sourceLen = static_cast<uLong>(uncompressed.size());
    uLongf destLen = compressBound(sourceLen);
    std::vector<uint8_t> compressed(destLen);

    z_stream strm;
    strm.zalloc = Z_NULL;
    strm.zfree = Z_NULL;
    strm.opaque = Z_NULL;
    if (deflateInit2(&strm, Z_BEST_COMPRESSION, Z_DEFLATED, 15, 9, Z_DEFAULT_STRATEGY) != Z_OK) {
        std::cerr << "zlib deflateInit2 failed.\n";
        return false;
    }
    strm.next_in = uncompressed.data();
    strm.avail_in = sourceLen;
    strm.next_out = compressed.data();
    strm.avail_out = destLen;
    deflate(&strm, Z_FINISH);
    destLen = strm.total_out;
    deflateEnd(&strm);
    compressed.resize(destLen);

    // Write file: MAGIC (8) + uncompressedLen (8) + compressed data
    std::ofstream ofs(outputDeltaPath, std::ios::binary);
    if (!ofs) {
        std::cerr << "Failed to open output delta file: " << outputDeltaPath << "\n";
        return false;
    }
    ofs.write(BDELTA_MAGIC, MAGIC_LEN);
    uint64_t uLen64 = sourceLen;
    ofs.write(reinterpret_cast<const char*>(&uLen64), sizeof(uLen64));
    ofs.write(reinterpret_cast<const char*>(compressed.data()), destLen);
    ofs.close();

    size_t targetOrigSize = std::filesystem::file_size(targetPath);
    size_t deltaFileSize = MAGIC_LEN + sizeof(uLen64) + destLen;

    if (report) {
        report->masterPath = masterPath;
        report->targetPath = targetPath;
        report->deltaPath = outputDeltaPath;
        report->originalBytes = targetOrigSize;
        report->deltaBytes = deltaFileSize;
        report->changedPixels = changedPixels;
        report->totalPixels = totalPixels;
        report->foregroundPercentage = (100.0 * changedPixels) / totalPixels;
        report->spaceSavingsPercent = (1.0 - (static_cast<double>(deltaFileSize) / targetOrigSize)) * 100.0;
    }

    return true;
}

bool DeltaCompressor::createDelta(
    const std::string& masterPath,
    const std::string& targetPath,
    const std::string& outputDeltaPath,
    uint8_t tolerance,
    SpatialAlignment alignment,
    CompressionReport* report
) {
    ImageBuffer master = ImageSimilarityScanner::loadImage(masterPath, 3);
    if (!master.isValid()) {
        std::cerr << "Error loading master image: " << masterPath << "\n";
        return false;
    }
    return createDeltaWithMaster(master, masterPath, targetPath, outputDeltaPath, tolerance, alignment, report);
}

bool DeltaCompressor::reconstructImage(
    const std::string& masterPath,
    const std::string& deltaPath,
    const std::string& outputImagePath
) {
    ImageBuffer master = ImageSimilarityScanner::loadImage(masterPath, 3);
    if (!master.isValid()) {
        std::cerr << "Failed to load master image: " << masterPath << "\n";
        return false;
    }

    std::ifstream ifs(deltaPath, std::ios::binary);
    if (!ifs) {
        std::cerr << "Failed to open delta file: " << deltaPath << "\n";
        return false;
    }

    char magic[MAGIC_LEN];
    ifs.read(magic, MAGIC_LEN);
    bool isV3 = (std::memcmp(magic, "BDELTA03", MAGIC_LEN) == 0);
    bool isV2 = (std::memcmp(magic, "BDELTA02", MAGIC_LEN) == 0);
    if (!isV3 && !isV2) {
        std::cerr << "Invalid delta file signature.\n";
        return false;
    }

    uint64_t uncompressedLen = 0;
    ifs.read(reinterpret_cast<char*>(&uncompressedLen), sizeof(uncompressedLen));

    std::vector<uint8_t> compressed(
        (std::istreambuf_iterator<char>(ifs)),
        std::istreambuf_iterator<char>()
    );
    ifs.close();

    std::vector<uint8_t> uncompressed(uncompressedLen);
    uLongf destLen = static_cast<uLongf>(uncompressedLen);

    if (uncompress(uncompressed.data(), &destLen, compressed.data(), static_cast<uLong>(compressed.size())) != Z_OK) {
        std::cerr << "zlib decompression failed.\n";
        return false;
    }

    // Unpack header
    size_t ptr = 0;
    auto readU32 = [&]() -> uint32_t {
        uint32_t v = uncompressed[ptr] |
                     (uncompressed[ptr + 1] << 8) |
                     (uncompressed[ptr + 2] << 16) |
                     (uncompressed[ptr + 3] << 24);
        ptr += 4;
        return v;
    };

    int W = static_cast<int>(readU32());
    int H = static_cast<int>(readU32());
    int C = static_cast<int>(readU32());
    uint8_t tolerance = uncompressed[ptr++];
    (void)tolerance;
    int8_t globalOffset = static_cast<int8_t>(uncompressed[ptr++]);

    int dx = 0;
    int dy = 0;
    float angle32 = 0.0f;
    if (isV3) {
        int16_t dx16 = uncompressed[ptr] | (uncompressed[ptr + 1] << 8);
        ptr += 2;
        int16_t dy16 = uncompressed[ptr] | (uncompressed[ptr + 1] << 8);
        ptr += 2;
        dx = dx16;
        dy = dy16;
        uint32_t angleBits = readU32();
        std::memcpy(&angle32, &angleBits, sizeof(angle32));
    }

    uint32_t changedPixels = readU32();
    (void)changedPixels;
    uint32_t metaLen = readU32();

    ptr += metaLen;

    // Align master dimensions if necessary
    // Only downscale if master is smaller than reconstructed target
    if (master.width < W || master.height < H) {
        master = ImageSimilarityScanner::downscale(master, W, H);
    }

    size_t totalPixels = static_cast<size_t>(W) * H;
    size_t maskBytes = (totalPixels + 7) / 8;
    const uint8_t* maskPtr = uncompressed.data() + ptr;
    ptr += maskBytes;
    const uint8_t* fgPtr = uncompressed.data() + ptr;

    // Blend: master background + foreground deltas
    ImageBuffer reconstructed;
    reconstructed.width = W;
    reconstructed.height = H;
    reconstructed.channels = C;
    reconstructed.data.resize(totalPixels * C);

    size_t fgOffset = 0;
    for (int y = 0; y < H; ++y) {
        int my = y + dy;
        for (int x = 0; x < W; ++x) {
            int mx = x + dx;
            size_t i = y * W + x;
            size_t idx = i * C;
            bool isForeground = (maskPtr[i / 8] & (1 << (i % 8))) != 0;

            int mR = 0, mG = 0, mB = 0;
            bool inMaster = (mx >= 0 && mx < master.width && my >= 0 && my < master.height);
            if (inMaster) {
                size_t mIdx = (my * master.width + mx) * C;
                mR = std::clamp(static_cast<int>(master.data[mIdx]) + globalOffset, 0, 255);
                mG = std::clamp(static_cast<int>(master.data[mIdx + 1]) + globalOffset, 0, 255);
                mB = std::clamp(static_cast<int>(master.data[mIdx + 2]) + globalOffset, 0, 255);
            }

            if (isForeground) {
                if (inMaster) {
                    int diffR = static_cast<int>(fgPtr[fgOffset]) - 128;
                    int diffG = static_cast<int>(fgPtr[fgOffset + 1]) - 128;
                    int diffB = static_cast<int>(fgPtr[fgOffset + 2]) - 128;
                    reconstructed.data[idx] = static_cast<uint8_t>(std::clamp(mR + diffR, 0, 255));
                    reconstructed.data[idx + 1] = static_cast<uint8_t>(std::clamp(mG + diffG, 0, 255));
                    reconstructed.data[idx + 2] = static_cast<uint8_t>(std::clamp(mB + diffB, 0, 255));
                } else {
                    reconstructed.data[idx] = fgPtr[fgOffset];
                    reconstructed.data[idx + 1] = fgPtr[fgOffset + 1];
                    reconstructed.data[idx + 2] = fgPtr[fgOffset + 2];
                }
                fgOffset += 3;
            } else {
                reconstructed.data[idx] = static_cast<uint8_t>(mR);
                reconstructed.data[idx + 1] = static_cast<uint8_t>(mG);
                reconstructed.data[idx + 2] = static_cast<uint8_t>(mB);
            }
        }
    }

    if (std::abs(angle32) > 0.01f) {
        reconstructed = ImageSimilarityScanner::rotate(reconstructed, angle32);
    }

    // Save image
    std::string ext = std::filesystem::path(outputImagePath).extension().string();
    int success = 0;
    if (ext == ".png" || ext == ".PNG") {
        success = stbi_write_png(outputImagePath.c_str(), W, H, C, reconstructed.data.data(), W * C);
    } else {
        success = stbi_write_jpg(outputImagePath.c_str(), W, H, C, reconstructed.data.data(), 92);
    }

    return success != 0;
}

double DeltaCompressor::computePsnr(const ImageBuffer& original, const ImageBuffer& reconstructed) {
    if (original.width != reconstructed.width || original.height != reconstructed.height ||
        original.channels != reconstructed.channels) {
        return 0.0;
    }

    size_t totalValues = original.data.size();
    if (totalValues == 0) return 0.0;

    double mse = 0.0;
    for (size_t i = 0; i < totalValues; ++i) {
        double diff = static_cast<double>(original.data[i]) - static_cast<double>(reconstructed.data[i]);
        mse += diff * diff;
    }
    mse /= totalValues;

    if (mse < 1e-10) return 99.99; // Perfect match
    return 10.0 * std::log10((255.0 * 255.0) / mse);
}

} // namespace blockdedup
