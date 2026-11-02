#include <catch2/catch_test_macros.hpp>
#include "blockdedup/metadata.hpp"
#include "blockdedup/image_tree.hpp"
#include "blockdedup/image_similarity.hpp"
#include "blockdedup/delta_compressor.hpp"
#include "third_party/stb_image_write.h"

#include <filesystem>
#include <vector>

using namespace blockdedup;

TEST_CASE("GpsCoordinates distance calculation", "[gps]") {
    GpsCoordinates loc1;
    loc1.latitude = 28.6139;
    loc1.longitude = 77.2090;
    loc1.valid = true;

    GpsCoordinates loc2;
    loc2.latitude = 28.6140;
    loc2.longitude = 77.2091;
    loc2.valid = true;

    double dist = loc1.distanceTo(loc2);
    REQUIRE(dist > 0.0);
    REQUIRE(dist < 50.0); // Within ~15-20 meters
}

TEST_CASE("SpatiotemporalClusterTree clustering", "[tree]") {
    SpatiotemporalClusterTree tree(50.0, 300);

    ImageMetadata img1;
    img1.filename = "photo1.jpg";
    img1.filepath = "sample/burst_set/burst_01_background.jpg";
    img1.gps.latitude = 28.6041;
    img1.gps.longitude = 77.2125;
    img1.gps.valid = true;
    img1.timestamp = 1000;
    img1.hasTimestamp = true;

    ImageMetadata img2;
    img2.filename = "photo2.jpg";
    img2.filepath = "sample/burst_set/burst_02_person_left.jpg";
    img2.gps.latitude = 28.6041;
    img2.gps.longitude = 77.2125;
    img2.gps.valid = true;
    img2.timestamp = 1004;
    img2.hasTimestamp = true;

    tree.insert(img1);
    tree.insert(img2);

    auto clusters = tree.getCandidateClusters();
    REQUIRE(clusters.size() == 1);
    REQUIRE(clusters[0].size() == 2);
}

TEST_CASE("Near-duplicate pixels survive compress and restore", "[delta]") {
    namespace fs = std::filesystem;
    const int W = 16;
    const int H = 16;
    const int C = 3;
    fs::path dir = fs::temp_directory_path() / "blockdedup_delta_test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    fs::path masterPath = dir / "master.png";
    fs::path targetPath = dir / "target.png";
    fs::path deltaPath = dir / "target.png.bdelta";
    fs::path outPath = dir / "out.png";

    std::vector<uint8_t> master(W * H * C, 128);
    std::vector<uint8_t> target = master;
    target[(8 * W + 8) * C] = 220;
    target[(8 * W + 8) * C + 1] = 10;
    target[(8 * W + 8) * C + 2] = 10;

    REQUIRE(stbi_write_png(masterPath.string().c_str(), W, H, C, master.data(), W * C) != 0);
    REQUIRE(stbi_write_png(targetPath.string().c_str(), W, H, C, target.data(), W * C) != 0);

    ImageMetadata a;
    a.filepath = masterPath.string();
    a.filename = "master.png";
    a.width = W;
    a.height = H;
    ImageMetadata b = a;
    b.filepath = targetPath.string();
    b.filename = "target.png";

    auto groups = ImageSimilarityScanner::groupSimilarImages({a, b}, 0.40);
    REQUIRE(groups.size() == 1);
    REQUIRE(groups[0].slaves.size() == 1);

    CompressionReport report;
    REQUIRE(DeltaCompressor::createDelta(
        masterPath.string(), targetPath.string(), deltaPath.string(), 0, {}, &report));
    REQUIRE(report.changedPixels >= 1);
    REQUIRE(DeltaCompressor::reconstructImage(
        masterPath.string(), deltaPath.string(), outPath.string()));

    auto got = ImageSimilarityScanner::loadImage(outPath.string(), 3);
    auto want = ImageSimilarityScanner::loadImage(targetPath.string(), 3);
    REQUIRE(got.width == W);
    REQUIRE(got.height == H);
    REQUIRE(got.data == want.data);

    fs::remove_all(dir);
}
