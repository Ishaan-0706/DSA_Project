#pragma once

#include "blockdedup/metadata.hpp"
#include <string>
#include <vector>
#include <cstdint>

namespace blockdedup {

struct ImageBuffer {
    int width = 0;
    int height = 0;
    int channels = 0;
    std::vector<uint8_t> data; // Interleaved RGB/RGBA pixels

    bool isValid() const { return !data.empty() && width > 0 && height > 0; }
    size_t size() const { return data.size(); }
};

struct SpatialAlignment {
    int dx = 0;           // Shift in X (target relative to master)
    int dy = 0;           // Shift in Y (target relative to master)
    double angleDeg = 0.0;// In-plane rotation angle (-5.0 to +5.0 degrees)
    double matchScore = 0.0; // Overlap background similarity [0.0 to 1.0]
    bool isAligned = false;
};

struct VisualGroup {
    ImageMetadata master;
    std::vector<ImageMetadata> slaves; // Images that will be stored as deltas against the master
    std::vector<SpatialAlignment> slaveAlignments; // Spatial alignment per slave
    double averageSimilarity = 0.0;
};

class ImageSimilarityScanner {
public:
    // Load image file into uncompressed RGB buffer
    static ImageBuffer loadImage(const std::string& filepath, int desiredChannels = 3);

    // Downscale buffer to fixed thumbnail (e.g. 64x64 or 128x128)
    static ImageBuffer downscale(const ImageBuffer& src, int targetW, int targetH);

    // Compute 64-bit difference hash (dHash)
    static uint64_t computeDHash(const ImageBuffer& img);

    // Compute Hamming distance between two 64-bit hashes
    static int hammingDistance(uint64_t h1, uint64_t h2);

    // Rotate an image buffer by angleDeg (bilinear interpolation with zero/black padding)
    static ImageBuffer rotate(const ImageBuffer& src, double angleDeg);

    // Detect 2D translation and slight rotation alignment between two images
    static SpatialAlignment estimateAlignment(
        const ImageBuffer& masterThumb,
        const ImageBuffer& targetThumb,
        int fullW, int fullH,
        int tolerance = 30
    );

    // Compute background overlap ratio between two images [0.0 to 1.0]
    static double computeBackgroundMatchRatio(const ImageBuffer& imgA, const ImageBuffer& imgB, int tolerance = 25);

    // Given candidate images from the tree, perform visual scanning to partition into confirmed Visual Groups
    static std::vector<VisualGroup> groupSimilarImages(
        const std::vector<ImageMetadata>& candidates,
        double minBackgroundRatio = 0.40
    );
};

} // namespace blockdedup
