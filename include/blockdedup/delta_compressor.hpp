#pragma once

#include "blockdedup/metadata.hpp"
#include "blockdedup/image_similarity.hpp"
#include <string>
#include <vector>
#include <cstdint>

namespace blockdedup {

struct CompressionReport {
    std::string masterPath;
    std::string targetPath;
    std::string deltaPath;
    size_t originalBytes = 0;
    size_t deltaBytes = 0;
    size_t changedPixels = 0;
    size_t totalPixels = 0;
    double foregroundPercentage = 0.0;
    double spaceSavingsPercent = 0.0;
    double psnrDb = 0.0;
};

class DeltaCompressor {
public:
    // Expand master canvas with missing content from target image
    static void expandMasterCanvas(
        ImageBuffer& master,
        const ImageBuffer& target,
        int dx, int dy
    );

    // Compress target image as a delta against pre-loaded master image with spatial alignment
    static bool createDeltaWithMaster(
        const ImageBuffer& master,
        const std::string& masterPath,
        const std::string& targetPath,
        const std::string& outputDeltaPath,
        uint8_t tolerance = 28,
        SpatialAlignment alignment = {},
        CompressionReport* report = nullptr
    );

    // Compress target image as a delta against master image
    static bool createDelta(
        const std::string& masterPath,
        const std::string& targetPath,
        const std::string& outputDeltaPath,
        uint8_t tolerance = 28,
        SpatialAlignment alignment = {},
        CompressionReport* report = nullptr
    );

    // Reconstruct target image from master image and delta file
    static bool reconstructImage(
        const std::string& masterPath,
        const std::string& deltaPath,
        const std::string& outputImagePath
    );

    // Compute Peak Signal-to-Noise Ratio (PSNR) between original and reconstructed image
    static double computePsnr(const ImageBuffer& original, const ImageBuffer& reconstructed);
};

} // namespace blockdedup
