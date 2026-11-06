#include <catch2/catch_test_macros.hpp>
#include "blockdedup/output_paths.hpp"

#include <filesystem>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;

TEST_CASE("sibling folder names", "[paths]") {
    auto paths = blockdedup::outputPathsFor("/photos/trip");
    REQUIRE(paths.archive == "/photos/trip_archive");
    REQUIRE(paths.restored == "/photos/trip_restored");
    REQUIRE_FALSE(paths.clashesWithSource);
}

TEST_CASE("trailing slash still uses the folder name", "[paths]") {
    auto paths = blockdedup::outputPathsFor("trip/");
    REQUIRE(paths.archive.filename() == "trip_archive");
    REQUIRE(paths.restored.filename() == "trip_restored");
    REQUIRE_FALSE(paths.clashesWithSource);
}

TEST_CASE("archive folder restores into the original folder name", "[paths]") {
    REQUIRE(blockdedup::restoreDestinationFor("/photos/trip_archive") == "/photos/trip");
    REQUIRE(blockdedup::restoreDestinationFor("/photos/trip_archive/") == "/photos/trip");
    REQUIRE(blockdedup::restoreDestinationFor("/photos/bundle") == "/photos/bundle_restored");
}

TEST_CASE("refuses when the archive path is the source folder", "[paths]") {
    fs::path root = fs::temp_directory_path() / ("blockdedup-paths-" + std::to_string(::getpid()));
    struct Cleanup {
        fs::path root;
        ~Cleanup() {
            std::error_code ec;
            fs::remove(root / "vacation_archive", ec);
            fs::remove_all(root, ec);
        }
    } cleanup{root};

    fs::remove_all(root);
    fs::create_directories(root / "vacation");
    fs::create_directory_symlink(root / "vacation", root / "vacation_archive");

    auto paths = blockdedup::outputPathsFor(root / "vacation");
    REQUIRE(paths.clashesWithSource);
}
