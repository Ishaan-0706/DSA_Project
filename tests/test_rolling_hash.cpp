#include <catch2/catch_test_macros.hpp>
#include "blockdedup/metadata.hpp"
#include "blockdedup/image_tree.hpp"
#include "blockdedup/image_similarity.hpp"

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
