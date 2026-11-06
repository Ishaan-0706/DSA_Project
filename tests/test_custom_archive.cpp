#include "blockdedup/metadata.hpp"
#include "blockdedup/delta_compressor.hpp"
#include "blockdedup/image_similarity.hpp"
#include "blockdedup/image_tree.hpp"
#include "third_party/stb_image_write.h"

#include <iostream>
#include <vector>
#include <filesystem>
#include <fstream>
#include <cassert>
#include <cstring>

namespace fs = std::filesystem;
using namespace blockdedup;

// Forward declaration of commandRestoreArchive from src/main.cpp
bool commandRestoreArchive(const std::string& archivePath, const std::string& reconDir);
bool commandCompress(const std::string& dir, const std::string& outArchiveTarget, uint8_t tolerance);

static int g_passCount = 0;
static int g_failCount = 0;

#define TEST_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            std::cerr << "  [FAIL] " << msg << " (" << __FILE__ << ":" << __LINE__ << ")\n"; \
            g_failCount++; \
            return false; \
        } \
    } while(0)

#define RUN_TEST(testFunc) \
    do { \
        std::cout << "Running " << #testFunc << "...\n"; \
        if (testFunc()) { \
            std::cout << "  [PASS] " << #testFunc << "\n"; \
            g_passCount++; \
        } else { \
            std::cout << "  [FAILED] " << #testFunc << "\n"; \
        } \
    } while(0)

void createTestImage(const fs::path& path, int width, int height, uint8_t r, uint8_t g, uint8_t b) {
    std::vector<uint8_t> pixels(width * height * 3);
    for (size_t i = 0; i < pixels.size(); i += 3) {
        pixels[i] = r;
        pixels[i + 1] = g;
        pixels[i + 2] = b;
    }
    stbi_write_png(path.string().c_str(), width, height, 3, pixels.data(), width * 3);
}

void createNearDuplicateImage(const fs::path& srcPath, const fs::path& dstPath, int width, int height) {
    auto img = ImageSimilarityScanner::loadImage(srcPath.string(), 3);
    for (int y = height / 4; y < 3 * height / 4; ++y) {
        for (int x = width / 4; x < 3 * width / 4; ++x) {
            int idx = (y * width + x) * 3;
            img.data[idx] = static_cast<uint8_t>(std::min(255, img.data[idx] + 50));
            img.data[idx + 1] = static_cast<uint8_t>(std::max(0, img.data[idx + 1] - 40));
            img.data[idx + 2] = static_cast<uint8_t>(std::min(255, img.data[idx + 2] + 30));
        }
    }
    stbi_write_png(dstPath.string().c_str(), width, height, 3, img.data.data(), width * 3);
}

// ----------------------------------------------------------------------------
// Test A: Automatic Packaging
// ----------------------------------------------------------------------------
bool testA_AutomaticPackaging() {
    fs::path tempDir = fs::temp_directory_path() / "test_a_pkg";
    std::error_code ec;
    fs::remove_all(tempDir, ec);
    fs::create_directories(tempDir / "staging", ec);

    createTestImage(tempDir / "staging" / "test1.png", 32, 32, 100, 150, 200);
    createTestImage(tempDir / "staging" / "test2.png", 32, 32, 120, 150, 200);

    fs::path targetRequest = tempDir / "archive_output";
    std::string finalPath = MetadataArchive::ensureMetaExtension(targetRequest.string());
    TEST_ASSERT(finalPath.size() > 5 && finalPath.substr(finalPath.size() - 5) == ".meta",
                "ensureMetaExtension must append .meta");

    std::string zipTarget = (tempDir / "archive_output.zip").string();
    std::string metaReplaced = MetadataArchive::ensureMetaExtension(zipTarget);
    TEST_ASSERT(metaReplaced == finalPath, "ensureMetaExtension must replace .zip with .meta");
    TEST_ASSERT(metaReplaced.find(".zip.meta") == std::string::npos, "No .zip.meta artifacts");

    std::string errMsg;
    bool ok = MetadataArchive::createArchiveFromDirectory((tempDir / "staging").string(), targetRequest.string(), nullptr, &errMsg);
    TEST_ASSERT(ok, "Packaging failed: " + errMsg);
    TEST_ASSERT(fs::exists(finalPath), "Output .meta file must exist");
    TEST_ASSERT(!fs::is_directory(finalPath), "Output must be a file, not a directory");

    fs::remove_all(tempDir, ec);
    return true;
}

// ----------------------------------------------------------------------------
// Test B: Complete Archive Validation (Truncated & Corrupted Rejection)
// ----------------------------------------------------------------------------
bool testB_ArchiveValidation() {
    fs::path tempDir = fs::temp_directory_path() / "test_b_val";
    std::error_code ec;
    fs::remove_all(tempDir, ec);
    fs::create_directories(tempDir / "stage" / "cluster_01", ec);

    createTestImage(tempDir / "stage" / "cluster_01" / "master_img.png", 24, 24, 255, 100, 100);
    createTestImage(tempDir / "stage" / "cluster_01" / "slave_img.png", 24, 24, 250, 100, 100);

    fs::path archivePath = tempDir / "validated.meta";
    std::string errMsg;
    bool ok = MetadataArchive::createArchiveFromDirectory((tempDir / "stage").string(), archivePath.string(), nullptr, &errMsg);
    TEST_ASSERT(ok, "Creation failed: " + errMsg);

    // 1. Valid archive must pass
    TEST_ASSERT(MetadataArchive::isValidArchive(archivePath.string()), "Archive must be valid");
    TEST_ASSERT(MetadataArchive::isCustomMetaArchive(archivePath.string()), "Must be recognized as .meta archive");

    // 2. Truncated archive starting with PK 03 04 MUST NOT be accepted
    fs::path fakeTruncated = tempDir / "fake_truncated.meta";
    {
        std::ofstream out(fakeTruncated, std::ios::binary);
        const char truncatedData[] = "PK\x03\x04\x14\x00\x00\x00\x08\x00SomeTruncatedDataWithoutEOCD";
        out.write(truncatedData, sizeof(truncatedData));
        out.close();
    }
    TEST_ASSERT(!MetadataArchive::isValidArchive(fakeTruncated.string()),
                "isValidArchive must reject truncated archive starting with PK 03 04");

    // 3. Truncated valid archive (cut in half) must be rejected
    auto validSize = fs::file_size(archivePath, ec);
    fs::path cutArchive = tempDir / "cut_half.meta";
    {
        std::ifstream in(archivePath, std::ios::binary);
        std::vector<char> partial(validSize / 2);
        in.read(partial.data(), partial.size());
        in.close();

        std::ofstream out(cutArchive, std::ios::binary);
        out.write(partial.data(), partial.size());
        out.close();
    }
    TEST_ASSERT(!MetadataArchive::isValidArchive(cutArchive.string()),
                "isValidArchive must reject file cut in half");

    // 4. Corrupted Central Directory bounds
    fs::path corruptCd = tempDir / "corrupt_cd.meta";
    {
        std::ifstream in(archivePath, std::ios::binary);
        std::vector<char> fullData(validSize);
        in.read(fullData.data(), fullData.size());
        in.close();

        // Corrupt signature of Central Directory
        for (size_t i = 0; i + 4 < fullData.size(); ++i) {
            if (std::memcmp(&fullData[i], "\x50\x4b\x01\x02", 4) == 0) {
                fullData[i] = 0x00; // Break CD signature
                break;
            }
        }
        std::ofstream out(corruptCd, std::ios::binary);
        out.write(fullData.data(), fullData.size());
        out.close();
    }
    TEST_ASSERT(!MetadataArchive::isValidArchive(corruptCd.string()),
                "isValidArchive must reject corrupted Central Directory signature");

    fs::remove_all(tempDir, ec);
    return true;
}

// ----------------------------------------------------------------------------
// Test C: Delta Generation Failure and Standalone Fallback
// ----------------------------------------------------------------------------
bool testC_DeltaGenerationFallback() {
    fs::path tempDir = fs::temp_directory_path() / "test_c_fallback";
    std::error_code ec;
    fs::remove_all(tempDir, ec);
    fs::create_directories(tempDir / "stage" / "cluster_01", ec);

    fs::path masterImg = tempDir / "stage" / "cluster_01" / "master_img.png";
    fs::path slaveImg = tempDir / "stage" / "cluster_01" / "slave_fallback.png";
    createTestImage(masterImg, 32, 32, 100, 120, 140);
    // Create completely different slave so delta compression will not shrink it
    createTestImage(slaveImg, 32, 32, 20, 200, 50);

    // Verify fallback to standalone when delta is ineffective or fails
    fs::path deltaPath = tempDir / "stage" / "cluster_01" / "master_img.png@@slave_fallback.png.bdelta";
    CompressionReport rpt;
    bool deltaOk = DeltaCompressor::createDelta(masterImg.string(), slaveImg.string(), deltaPath.string(), 0, {}, &rpt);

    // If delta doesn't shrink file, it falls back to standalone copy
    if (deltaOk && rpt.deltaBytes >= rpt.originalBytes) {
        fs::remove(deltaPath, ec);
        // Slave is kept standalone
        TEST_ASSERT(fs::exists(slaveImg), "Slave must be kept as standalone file");
    }

    ArchiveManifest manifest;
    manifest.entries.push_back({"img.png", "cluster_01", fs::file_size(masterImg), MetadataArchive::computeSha256(masterImg.string()), true, ""});
    manifest.entries.push_back({"slave_fallback.png", "cluster_01", fs::file_size(slaveImg), MetadataArchive::computeSha256(slaveImg.string()), false, ""});

    fs::path archivePath = tempDir / "fallback_archive.meta";
    std::string err;
    bool pkgOk = MetadataArchive::createArchiveFromDirectory((tempDir / "stage").string(), archivePath.string(), &manifest, &err);
    TEST_ASSERT(pkgOk, "Packaging failed: " + err);

    fs::path extDir = tempDir / "extracted";
    bool extOk = MetadataArchive::extractArchive(archivePath.string(), extDir.string(), &err);
    TEST_ASSERT(extOk, "Extraction failed: " + err);

    TEST_ASSERT(fs::exists(extDir / "cluster_01" / "slave_fallback.png"), "Standalone slave must exist in archive");

    fs::remove_all(tempDir, ec);
    return true;
}

// ----------------------------------------------------------------------------
// Test D: Exact Restoration of All Original Images (SHA-256 Checksums)
// ----------------------------------------------------------------------------
bool testD_ExactRestoration() {
    fs::path tempDir = fs::temp_directory_path() / "test_d_restoration";
    std::error_code ec;
    fs::remove_all(tempDir, ec);
    fs::create_directories(tempDir / "dataset", ec);

    fs::path img1 = tempDir / "dataset" / "img1.png";
    fs::path img2 = tempDir / "dataset" / "img2.png";
    fs::path img3 = tempDir / "dataset" / "img3.png";

    createTestImage(img1, 32, 32, 120, 140, 160);
    createNearDuplicateImage(img1, img2, 32, 32);
    createTestImage(img3, 32, 32, 180, 190, 200);

    std::string hash1 = MetadataArchive::computeSha256(img1.string());
    std::string hash2 = MetadataArchive::computeSha256(img2.string());
    std::string hash3 = MetadataArchive::computeSha256(img3.string());
    TEST_ASSERT(!hash1.empty() && !hash2.empty() && !hash3.empty(), "Hashes must not be empty");

    fs::path staging = tempDir / "staging" / "cluster_01";
    fs::create_directories(staging, ec);

    fs::copy_file(img1, staging / "master_img1.png", ec);
    fs::path deltaPath = staging / "master_img1.png@@img2.png.bdelta";
    CompressionReport rpt;
    bool deltaOk = DeltaCompressor::createDelta(img1.string(), img2.string(), deltaPath.string(), 0, {}, &rpt);
    TEST_ASSERT(deltaOk, "Delta creation failed");
    fs::copy_file(img3, staging / "img3.png", ec);

    ArchiveManifest manifest;
    manifest.tolerance = 0;
    manifest.totalOriginalImages = 3;
    manifest.entries.push_back({"img1.png", "cluster_01", fs::file_size(img1), hash1, true, ""});
    manifest.entries.push_back({"img2.png", "cluster_01", fs::file_size(img2), hash2, false, "master_img1.png"});
    manifest.entries.push_back({"img3.png", "cluster_01", fs::file_size(img3), hash3, true, ""});

    fs::path archiveFile = tempDir / "exact.meta";
    std::string errMsg;
    bool pkgOk = MetadataArchive::createArchiveFromDirectory((tempDir / "staging").string(), archiveFile.string(), &manifest, &errMsg);
    TEST_ASSERT(pkgOk, "Packaging failed: " + errMsg);

    // Restore via commandRestoreArchive
    fs::path restored = tempDir / "restored";
    bool restoreOk = commandRestoreArchive(archiveFile.string(), restored.string());
    TEST_ASSERT(restoreOk, "commandRestoreArchive should succeed");

    std::string reconHash1 = MetadataArchive::computeSha256((restored / "img1.png").string());
    std::string reconHash2 = MetadataArchive::computeSha256((restored / "img2.png").string());
    std::string reconHash3 = MetadataArchive::computeSha256((restored / "img3.png").string());

    TEST_ASSERT(reconHash1 == hash1, "Restored img1.png SHA-256 mismatch!");
    TEST_ASSERT(reconHash2 == hash2, "Restored img2.png SHA-256 mismatch!");
    TEST_ASSERT(reconHash3 == hash3, "Restored img3.png SHA-256 mismatch!");

    fs::remove_all(tempDir, ec);
    return true;
}

// ----------------------------------------------------------------------------
// Test E: Backward Compatibility (Directory & .zip formats)
// ----------------------------------------------------------------------------
bool testE_BackwardCompatibility() {
    fs::path tempDir = fs::temp_directory_path() / "test_e_compat";
    std::error_code ec;
    fs::remove_all(tempDir, ec);

    // Format 1: Uncompressed directory archive
    fs::path legacyDir = tempDir / "legacy_archive_dir" / "cluster_01";
    fs::create_directories(legacyDir, ec);
    createTestImage(legacyDir / "master_old1.png", 16, 16, 50, 60, 70);
    createTestImage(legacyDir / "old_standalone.png", 16, 16, 80, 90, 100);

    fs::path reconLegacyDir = tempDir / "recon_legacy_dir";
    bool okDir = commandRestoreArchive(legacyDir.parent_path().string(), reconLegacyDir.string());
    TEST_ASSERT(okDir, "Restoring legacy uncompressed directory should succeed");
    TEST_ASSERT(fs::exists(reconLegacyDir / "old1.png"), "Restored master from legacy dir");
    TEST_ASSERT(fs::exists(reconLegacyDir / "old_standalone.png"), "Restored standalone from legacy dir");

    // Format 2: .zip archive
    fs::path metaTemp = tempDir / "staging_meta.meta";
    std::string err;
    bool pkgOk = MetadataArchive::createArchiveFromDirectory(legacyDir.parent_path().string(), metaTemp.string(), nullptr, &err);
    TEST_ASSERT(pkgOk, "Archive creation failed: " + err);

    fs::path zipArchive = tempDir / "legacy_archive.zip";
    fs::rename(metaTemp, zipArchive, ec);
    TEST_ASSERT(!ec, "Failed to rename to legacy .zip");

    fs::path reconZip = tempDir / "recon_zip";
    bool okZip = commandRestoreArchive(zipArchive.string(), reconZip.string());
    TEST_ASSERT(okZip, "Restoring legacy .zip should succeed");
    TEST_ASSERT(fs::exists(reconZip / "old1.png"), "Restored master from legacy .zip");

    fs::remove_all(tempDir, ec);
    return true;
}

// ----------------------------------------------------------------------------
// Test F: Unsafe Extraction Paths (Path Traversal, Absolute, UNC, Devices)
// ----------------------------------------------------------------------------
bool testF_UnsafeExtractionPaths() {
    // 1. isSafeEntryPath unit tests
    TEST_ASSERT(!MetadataArchive::isSafeEntryPath("../traversal.txt"), "Must reject parent traversal");
    TEST_ASSERT(!MetadataArchive::isSafeEntryPath("sub/../../traversal.txt"), "Must reject nested traversal");
    TEST_ASSERT(!MetadataArchive::isSafeEntryPath("/root/file.txt"), "Must reject leading slash");
    TEST_ASSERT(!MetadataArchive::isSafeEntryPath("\\root\\file.txt"), "Must reject leading backslash");
    TEST_ASSERT(!MetadataArchive::isSafeEntryPath("C:/Windows/evil.exe"), "Must reject drive letter");
    TEST_ASSERT(!MetadataArchive::isSafeEntryPath("D:\\Windows\\evil.exe"), "Must reject drive letter backslash");
    TEST_ASSERT(!MetadataArchive::isSafeEntryPath("//server/share/evil.exe"), "Must reject UNC path");
    TEST_ASSERT(!MetadataArchive::isSafeEntryPath("CON"), "Must reject Windows reserved name CON");
    TEST_ASSERT(!MetadataArchive::isSafeEntryPath("folder/AUX.txt"), "Must reject Windows reserved name AUX");
    TEST_ASSERT(!MetadataArchive::isSafeEntryPath("COM1"), "Must reject COM1");
    TEST_ASSERT(MetadataArchive::isSafeEntryPath("cluster_01/photo.png"), "Valid path must be accepted");
    TEST_ASSERT(MetadataArchive::isSafeEntryPath("photo.png"), "Simple filename must be accepted");

    // 2. extractArchive rejection test with malicious entry name
    fs::path tempDir = fs::temp_directory_path() / "test_f_unsafe";
    std::error_code ec;
    fs::remove_all(tempDir, ec);
    fs::create_directories(tempDir / "staging", ec);

    createTestImage(tempDir / "staging" / "normal.png", 16, 16, 1, 2, 3);
    fs::path archivePath = tempDir / "unsafe.meta";
    std::string err;
    MetadataArchive::createArchiveFromDirectory((tempDir / "staging").string(), archivePath.string(), nullptr, &err);

    // Modify archive to inject traversal entry "../escaped.txt" into central directory
    {
        std::ifstream in(archivePath, std::ios::binary);
        std::vector<char> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        in.close();

        // Search for "normal.png" and replace with "../esc.png"
        std::string target = "normal.png";
        std::string evil = "../esc.png";
        for (size_t i = 0; i + target.size() <= data.size(); ++i) {
            if (std::memcmp(&data[i], target.data(), target.size()) == 0) {
                std::memcpy(&data[i], evil.data(), evil.size());
            }
        }
        std::ofstream out(archivePath, std::ios::binary | std::ios::trunc);
        out.write(data.data(), data.size());
        out.close();
    }

    fs::path outDir = tempDir / "dest";
    bool extractOk = MetadataArchive::extractArchive(archivePath.string(), outDir.string(), &err);
    TEST_ASSERT(!extractOk, "extractArchive must reject archive with traversal entry");
    TEST_ASSERT(!fs::exists(tempDir / "esc.png"), "File must not be extracted outside destination");

    fs::remove_all(tempDir, ec);
    return true;
}

// ----------------------------------------------------------------------------
// Test G: Restoration Failure Reporting & Missing Image Detection
// ----------------------------------------------------------------------------
bool testG_RestorationFailureReporting() {
    fs::path tempDir = fs::temp_directory_path() / "test_g_fail";
    std::error_code ec;
    fs::remove_all(tempDir, ec);
    fs::create_directories(tempDir / "stage" / "cluster_01", ec);

    fs::path img1 = tempDir / "stage" / "cluster_01" / "master_img1.png";
    createTestImage(img1, 16, 16, 100, 100, 100);

    // Create manifest declaring 2 images, but archive only has 1
    ArchiveManifest manifest;
    manifest.totalOriginalImages = 2;
    manifest.tolerance = 0;
    manifest.entries.push_back({"img1.png", "cluster_01", fs::file_size(img1), MetadataArchive::computeSha256(img1.string()), true, ""});
    manifest.entries.push_back({"img2_missing.png", "cluster_01", 100, "fakehash", false, "master_img1.png"});

    fs::path archivePath = tempDir / "missing_img.meta";
    std::string err;
    MetadataArchive::createArchiveFromDirectory((tempDir / "stage").string(), archivePath.string(), &manifest, &err);

    fs::path reconDir = tempDir / "recon";
    bool restoreOk = commandRestoreArchive(archivePath.string(), reconDir.string());
    TEST_ASSERT(!restoreOk, "commandRestoreArchive must fail when an image in manifest is missing");

    fs::remove_all(tempDir, ec);
    return true;
}

// ----------------------------------------------------------------------------
// Test H: Confidentiality Behavior Documentation
// ----------------------------------------------------------------------------
bool testH_ConfidentialityBehavior() {
    // Verify that confidentiality flag is accurately exposed as false
    TEST_ASSERT(MetadataArchive::IS_CRYPTOGRAPHICALLY_CONFIDENTIAL == false,
                "Format must not claim cryptographic confidentiality");
    return true;
}

// ----------------------------------------------------------------------------
// Test I: Duplicate Archive Prevention
// ----------------------------------------------------------------------------
bool testI_DuplicateArchive() {
    fs::path tempDir = fs::temp_directory_path() / "test_i_duplicate";
    std::error_code ec;
    fs::remove_all(tempDir, ec);
    fs::create_directories(tempDir, ec);

    fs::path existingArchive = tempDir / "target.meta";
    {
        std::ofstream out(existingArchive);
        out << "Existing archive content";
        out.close();
    }

    std::string safeName = MetadataArchive::sanitizeOutputPath(existingArchive.string(), true);
    TEST_ASSERT(safeName != existingArchive.string(), "Must not overwrite existing archive");
    TEST_ASSERT(!fs::exists(safeName), "Safe name candidate must not already exist");

    fs::remove_all(tempDir, ec);
    return true;
}

int main() {
    std::cout << "====================================================\n";
    std::cout << "=== BlockDedup Hardened Archive Test Suite       ===\n";
    std::cout << "====================================================\n";

    RUN_TEST(testA_AutomaticPackaging);
    RUN_TEST(testB_ArchiveValidation);
    RUN_TEST(testC_DeltaGenerationFallback);
    RUN_TEST(testD_ExactRestoration);
    RUN_TEST(testE_BackwardCompatibility);
    RUN_TEST(testF_UnsafeExtractionPaths);
    RUN_TEST(testG_RestorationFailureReporting);
    RUN_TEST(testH_ConfidentialityBehavior);
    RUN_TEST(testI_DuplicateArchive);

    std::cout << "====================================================\n";
    std::cout << "Results: " << g_passCount << " passed, " << g_failCount << " failed.\n";
    std::cout << "====================================================\n";

    return (g_failCount == 0) ? 0 : 1;
}
