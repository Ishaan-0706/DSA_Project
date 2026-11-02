#include "blockdedup/image_similarity.hpp"
#define STB_IMAGE_IMPLEMENTATION
#include "third_party/stb_image.h"
#ifdef BLOCKDEDUP_HAS_EXIV2
#include <exiv2/exiv2.hpp>
#include <exiv2/preview.hpp>
#endif
#include <cmath>
#include <iostream>
#include <algorithm>

namespace blockdedup {

ImageBuffer ImageSimilarityScanner::loadImage(const std::string& filepath, int desiredChannels) {
    ImageBuffer buf;
    int w = 0, h = 0, c = 0;
    unsigned char* raw = stbi_load(filepath.c_str(), &w, &h, &c, desiredChannels);
#ifdef BLOCKDEDUP_HAS_EXIV2
    if (!raw) {
        // Fallback: If it's a camera RAW file, extract embedded JPEG preview using Exiv2
        try {
            auto exivImg = Exiv2::ImageFactory::open(filepath);
            if (exivImg.get()) {
                exivImg->readMetadata();
                Exiv2::PreviewManager pm(*exivImg);
                auto previewList = pm.getPreviewProperties();
                if (!previewList.empty()) {
                    // Pick the highest resolution preview
                    auto preview = pm.getPreviewImage(previewList.back());
                    raw = stbi_load_from_memory(preview.pData(), preview.size(), &w, &h, &c, desiredChannels);
                }
            }
        } catch (...) {
            // Ignore exiv2 preview extraction failures
        }
    }
#endif

    if (!raw) {
        return buf;
    }
    buf.width = w;
    buf.height = h;
    buf.channels = (desiredChannels > 0) ? desiredChannels : c;
    buf.data.assign(raw, raw + (w * h * buf.channels));
    stbi_image_free(raw);
    return buf;
}

ImageBuffer ImageSimilarityScanner::downscale(const ImageBuffer& src, int targetW, int targetH) {
    ImageBuffer dst;
    if (!src.isValid() || targetW <= 0 || targetH <= 0) return dst;

    dst.width = targetW;
    dst.height = targetH;
    dst.channels = src.channels;
    dst.data.resize(targetW * targetH * src.channels);

    double xRatio = static_cast<double>(src.width) / targetW;
    double yRatio = static_cast<double>(src.height) / targetH;

    for (int y = 0; y < targetH; ++y) {
        int srcY = std::min(static_cast<int>(y * yRatio), src.height - 1);
        for (int x = 0; x < targetW; ++x) {
            int srcX = std::min(static_cast<int>(x * xRatio), src.width - 1);
            size_t srcIdx = (srcY * src.width + srcX) * src.channels;
            size_t dstIdx = (y * targetW + x) * dst.channels;
            for (int c = 0; c < src.channels; ++c) {
                dst.data[dstIdx + c] = src.data[srcIdx + c];
            }
        }
    }
    return dst;
}

uint64_t ImageSimilarityScanner::computeDHash(const ImageBuffer& img) {
    // Standard dHash: resize to 9x8, convert to grayscale, compare adjacent horizontal pixels (8x8 = 64 bits)
    ImageBuffer thumb = downscale(img, 9, 8);
    if (!thumb.isValid()) return 0;

    // Convert to grayscale
    std::vector<uint8_t> gray(9 * 8);
    for (int i = 0; i < 9 * 8; ++i) {
        int idx = i * thumb.channels;
        int r = thumb.data[idx];
        int g = (thumb.channels > 1) ? thumb.data[idx + 1] : r;
        int b = (thumb.channels > 2) ? thumb.data[idx + 2] : r;
        gray[i] = static_cast<uint8_t>(0.299 * r + 0.587 * g + 0.114 * b);
    }

    uint64_t hash = 0;
    int bit = 0;
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            uint8_t left = gray[y * 9 + x];
            uint8_t right = gray[y * 9 + x + 1];
            if (left > right) {
                hash |= (1ULL << bit);
            }
            bit++;
        }
    }
    return hash;
}

int ImageSimilarityScanner::hammingDistance(uint64_t h1, uint64_t h2) {
    return __builtin_popcountll(h1 ^ h2);
}

double ImageSimilarityScanner::computeBackgroundMatchRatio(const ImageBuffer& imgA, const ImageBuffer& imgB, int tolerance) {
    if (!imgA.isValid() || !imgB.isValid()) return 0.0;

    // Fast compare on standardized 64x64 thumbnails
    ImageBuffer tA = downscale(imgA, 64, 64);
    ImageBuffer tB = downscale(imgB, 64, 64);

    int totalPixels = 64 * 64;
    int matchingPixels = 0;

    for (int i = 0; i < totalPixels; ++i) {
        int idx = i * tA.channels;
        bool match = true;
        for (int c = 0; c < std::min(tA.channels, tB.channels); ++c) {
            int diff = std::abs(static_cast<int>(tA.data[idx + c]) - static_cast<int>(tB.data[idx + c]));
            if (diff > tolerance) {
                match = false;
                break;
            }
        }
        if (match) {
            matchingPixels++;
        }
    }

    return static_cast<double>(matchingPixels) / totalPixels;
}

ImageBuffer ImageSimilarityScanner::rotate(const ImageBuffer& src, double angleDeg) {
    if (!src.isValid() || std::abs(angleDeg) < 0.01) return src;

    ImageBuffer dst;
    dst.width = src.width;
    dst.height = src.height;
    dst.channels = src.channels;
    dst.data.assign(src.data.size(), 0);

    double rad = -angleDeg * 3.14159265358979323846 / 180.0;
    double cosA = std::cos(rad);
    double sinA = std::sin(rad);

    double cx = src.width / 2.0;
    double cy = src.height / 2.0;

    for (int y = 0; y < src.height; ++y) {
        for (int x = 0; x < src.width; ++x) {
            double rx = (x - cx) * cosA - (y - cy) * sinA + cx;
            double ry = (x - cx) * sinA + (y - cy) * cosA + cy;

            int x0 = static_cast<int>(std::floor(rx));
            int y0 = static_cast<int>(std::floor(ry));
            int x1 = x0 + 1;
            int y1 = y0 + 1;

            if (x0 >= 0 && x1 < src.width && y0 >= 0 && y1 < src.height) {
                double fx = rx - x0;
                double fy = ry - y0;
                size_t dIdx = (y * src.width + x) * src.channels;

                size_t idx00 = (y0 * src.width + x0) * src.channels;
                size_t idx01 = (y0 * src.width + x1) * src.channels;
                size_t idx10 = (y1 * src.width + x0) * src.channels;
                size_t idx11 = (y1 * src.width + x1) * src.channels;

                for (int c = 0; c < src.channels; ++c) {
                    double p0 = src.data[idx00 + c] * (1.0 - fx) + src.data[idx01 + c] * fx;
                    double p1 = src.data[idx10 + c] * (1.0 - fx) + src.data[idx11 + c] * fx;
                    dst.data[dIdx + c] = static_cast<uint8_t>(std::clamp(p0 * (1.0 - fy) + p1 * fy, 0.0, 255.0));
                }
            }
        }
    }
    return dst;
}

SpatialAlignment ImageSimilarityScanner::estimateAlignment(
    const ImageBuffer& masterThumb,
    const ImageBuffer& targetThumb,
    int fullW, int fullH,
    int tolerance
) {
    SpatialAlignment best;
    best.matchScore = 0.0;
    best.isAligned = false;
    best.dx = 0;
    best.dy = 0;
    best.angleDeg = 0.0;
    if (!masterThumb.isValid() || !targetThumb.isValid()) return best;

    int tW = masterThumb.width;
    int tH = masterThumb.height;
    double scaleX = static_cast<double>(fullW) / tW;
    double scaleY = static_cast<double>(fullH) / tH;

    // First check direct alignment at (0, 0)
    int count0 = 0, match0 = 0;
    for (int y = 0; y < tH; ++y) {
        for (int x = 0; x < tW; ++x) {
            size_t idx = (y * tW + x) * masterThumb.channels;
            count0++;
            int dr = std::abs((int)masterThumb.data[idx] - (int)targetThumb.data[idx]);
            int dg = std::abs((int)masterThumb.data[idx + 1] - (int)targetThumb.data[idx + 1]);
            int db = std::abs((int)masterThumb.data[idx + 2] - (int)targetThumb.data[idx + 2]);
            if (dr <= tolerance && dg <= tolerance && db <= tolerance) {
                match0++;
            }
        }
    }
    double score0 = static_cast<double>(match0) / count0;
    best.matchScore = score0;
    best.isAligned = true;

    // Search small shifts (+/- 6 pixels in thumbnail, ~10% full resolution)
    int maxShift = std::min(6, tW / 10);
    double angles[] = {0.0, -1.5, 1.5};

    for (double ang : angles) {
        ImageBuffer rotTarget = (std::abs(ang) > 0.01) ? rotate(targetThumb, ang) : targetThumb;

        for (int dy = -maxShift; dy <= maxShift; ++dy) {
            for (int dx = -maxShift; dx <= maxShift; ++dx) {
                if (dx == 0 && dy == 0 && std::abs(ang) < 0.01) continue;

                int overlapCount = 0;
                int matchCount = 0;

                int startY = std::max(0, -dy);
                int endY = std::min(tH, tH - dy);
                int startX = std::max(0, -dx);
                int endX = std::min(tW, tW - dx);

                for (int y = startY; y < endY; ++y) {
                    int ty = y + dy;
                    for (int x = startX; x < endX; ++x) {
                        int tx = x + dx;
                        size_t mIdx = (y * tW + x) * masterThumb.channels;
                        size_t tIdx = (ty * tW + tx) * rotTarget.channels;

                        overlapCount++;
                        int dr = std::abs((int)masterThumb.data[mIdx] - (int)rotTarget.data[tIdx]);
                        int dg = std::abs((int)masterThumb.data[mIdx + 1] - (int)rotTarget.data[tIdx + 1]);
                        int db = std::abs((int)masterThumb.data[mIdx + 2] - (int)rotTarget.data[tIdx + 2]);

                        if (dr <= tolerance && dg <= tolerance && db <= tolerance) {
                            matchCount++;
                        }
                    }
                }

                if (overlapCount > (tW * tH * 3) / 4) {
                    double score = static_cast<double>(matchCount) / overlapCount;
                    // Only switch from (0,0) if there's a significant improvement (>5% better)
                    if (score > best.matchScore + 0.05) {
                        best.matchScore = score;
                        best.dx = static_cast<int>(std::round(dx * scaleX));
                        best.dy = static_cast<int>(std::round(dy * scaleY));
                        best.angleDeg = ang;
                        best.isAligned = true;
                    }
                }
            }
        }
    }

    return best;
}

std::vector<VisualGroup> ImageSimilarityScanner::groupSimilarImages(
    const std::vector<ImageMetadata>& candidates,
    double minBackgroundRatio
) {
    std::vector<VisualGroup> groups;
    if (candidates.empty()) return groups;

    struct CachedCandidate {
        ImageMetadata meta;
        ImageBuffer thumbnail;
        uint64_t dHash = 0;
        bool assigned = false;
    };

    std::vector<CachedCandidate> rawCached(candidates.size());
    std::vector<bool> isValid(candidates.size(), false);

    #pragma omp parallel for schedule(dynamic)
    for (size_t idx = 0; idx < candidates.size(); ++idx) {
        ImageBuffer full = loadImage(candidates[idx].filepath, 3);
        if (full.isValid()) {
            ImageBuffer thumb = downscale(full, 64, 64);
            uint64_t h = computeDHash(full);
            rawCached[idx] = {candidates[idx], std::move(thumb), h, false};
            isValid[idx] = true;
        }
    }

    std::vector<CachedCandidate> cached;
    for (size_t idx = 0; idx < candidates.size(); ++idx) {
        if (isValid[idx]) {
            cached.push_back(std::move(rawCached[idx]));
        }
    }

    // Select the best representative master for each cluster (medoid with highest total similarity)
    while (true) {
        // Find unassigned candidates
        std::vector<size_t> unassigned;
        for (size_t i = 0; i < cached.size(); ++i) {
            if (!cached[i].assigned) unassigned.push_back(i);
        }
        if (unassigned.empty()) break;

        // Find candidate with maximum connectivity to remaining unassigned pool
        size_t bestMasterIdx = unassigned[0];
        int maxConnected = -1;

        for (size_t uIdx : unassigned) {
            int connected = 0;
            for (size_t vIdx : unassigned) {
                if (uIdx == vIdx) continue;
                double ratio = computeBackgroundMatchRatio(cached[uIdx].thumbnail, cached[vIdx].thumbnail, 35);
                if (ratio >= minBackgroundRatio) {
                    connected++;
                }
            }
            if (connected > maxConnected) {
                maxConnected = connected;
                bestMasterIdx = uIdx;
            }
        }

        VisualGroup grp;
        grp.master = cached[bestMasterIdx].meta;
        cached[bestMasterIdx].assigned = true;

        double sumSim = 0.0;
        int count = 0;

        for (size_t j : unassigned) {
            if (j == bestMasterIdx || cached[j].assigned) continue;

            double ratio = computeBackgroundMatchRatio(cached[bestMasterIdx].thumbnail, cached[j].thumbnail, 35);
            SpatialAlignment align;

            // Only admit candidates that have strong overlap (>= 70%) with this specific master
            if (ratio >= minBackgroundRatio) {
                align.matchScore = ratio;
                align.isAligned = true;
                align.dx = 0;
                align.dy = 0;
                align.angleDeg = 0.0;
                grp.slaves.push_back(cached[j].meta);
                grp.slaveAlignments.push_back(align);
                cached[j].assigned = true;
                sumSim += ratio;
                count++;
            } else {
                align = estimateAlignment(cached[bestMasterIdx].thumbnail, cached[j].thumbnail, cached[bestMasterIdx].meta.width, cached[bestMasterIdx].meta.height, 35);
                if (align.matchScore >= minBackgroundRatio) {
                    grp.slaves.push_back(cached[j].meta);
                    grp.slaveAlignments.push_back(align);
                    cached[j].assigned = true;
                    sumSim += align.matchScore;
                    count++;
                }
            }
        }

        grp.averageSimilarity = (count > 0) ? (sumSim / count) : 1.0;
        groups.push_back(std::move(grp));
    }

    return groups;
}

} // namespace blockdedup
