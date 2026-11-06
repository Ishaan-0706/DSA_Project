#include "blockdedup/metadata.hpp"
#include "blockdedup/image_tree.hpp"
#include "blockdedup/image_similarity.hpp"
#include "blockdedup/delta_compressor.hpp"

#include <iostream>
#include <vector>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <random>
#include <atomic>

#ifdef BLOCKDEDUP_HAS_OPENMP
#include <omp.h>
#endif
#include "third_party/stb_image_write.h"

namespace fs = std::filesystem;
using namespace blockdedup;

void printUsage(const char* prog) {
    std::cout << "Usage: " << prog << " <command> [arguments...]\n\n"
              << "Commands:\n"
              << "  inspect <image_path>               Parse & inspect EXIF/XMP & GPS telemetry\n"
              << "  tree <image_dir>                   Build & display Spatiotemporal Cluster Tree\n"
              << "  scan <image_dir>                   Tree clustering + visual scanning (find Masters & Slaves)\n"
              << "  compress <image_dir> <out_archive> [tolerance]\n"
              << "                                     Generate packaged .meta archive. Default tolerance 28\n"
              << "  decompress <master> <delta> <out>  Reconstruct one image from a master and a delta\n"
              << "  restore <archive> <out_dir>        Reconstruct every image from a .meta or legacy archive\n"
              << "  demo [image_dir]                   Inspect, cluster, compress, and restore\n\n";
}

void commandInspect(const std::string& path) {
    auto metaOpt = MetadataExtractor::extract(path);
    if (!metaOpt) {
        std::cerr << "Failed to extract metadata from: " << path << "\n";
        return;
    }
    const auto& m = *metaOpt;
    std::cout << "========================================\n";
    std::cout << "File:        " << m.filepath << "\n";
    std::cout << "MIME Type:   " << m.mimeType << "\n";
    std::cout << "Dimensions:  " << m.width << " x " << m.height << " px\n";
    std::cout << "File Size:   " << (m.fileSize / 1024) << " KB\n";
    std::cout << "Device:      " << m.make << " " << m.model << "\n";
    std::cout << "Timestamp:   " << (m.timestampStr.empty() ? "None" : m.timestampStr) << "\n";
    if (m.gps.valid) {
        std::cout << "GPS:         " << std::fixed << std::setprecision(6) 
                  << m.gps.latitude << ", " << m.gps.longitude 
                  << " (Alt: " << m.gps.altitude << " m)\n";
    } else {
        std::cout << "GPS:         No GPS coordinates\n";
    }
    std::cout << "Exposure:    " << m.exposureTime << ", " << m.fNumber << ", ISO " << m.iso << "\n";
    std::cout << "Total Tags:  " << m.allTags.size() << "\n";
    std::cout << "========================================\n";
}

std::vector<ImageMetadata> collectImages(const std::string& dir) {
    std::vector<ImageMetadata> list;
    if (!fs::exists(dir)) return list;

    std::error_code ec;
    for (const auto& entry : fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec)) {
        if (!entry.is_regular_file()) continue;
        std::string pStr = entry.path().string();
        if (pStr.find("compressed") != std::string::npos || pStr.find(".bdelta") != std::string::npos || pStr.find(".meta") != std::string::npos) {
            continue;
        }

        std::string ext = entry.path().extension().string();
        for (auto& c : ext) c = tolower(c);
        if (ext == ".jpg" || ext == ".jpeg" || ext == ".png" || ext == ".bmp" || 
            ext == ".webp" || ext == ".cr2" || ext == ".nef" || ext == ".arw" || ext == ".dng") {
            auto meta = MetadataExtractor::extract(entry.path().string());
            if (meta) list.push_back(*meta);
        }
    }
    return list;
}

void commandTree(const std::string& dir) {
    auto images = collectImages(dir);
    if (images.empty()) {
        std::cout << "No images found in " << dir << "\n";
        return;
    }
    SpatiotemporalClusterTree tree(50.0, 300);
    tree.build(images);
    tree.printTree();
}

void commandScan(const std::string& dir) {
    auto images = collectImages(dir);
    if (images.empty()) {
        std::cout << "No images found in " << dir << "\n";
        return;
    }

    SpatiotemporalClusterTree tree(50.0, 300);
    tree.build(images);
    tree.printTree();

    auto candidates = tree.getCandidateClusters();
    std::cout << "=== Visual Scanning & Master Selection ===\n";
    int groupIdx = 1;
    for (const auto& candList : candidates) {
        auto visualGroups = ImageSimilarityScanner::groupSimilarImages(candList, 0.50);
        for (const auto& grp : visualGroups) {
            std::cout << "\nVisual Group #" << groupIdx++ << ":\n";
            std::cout << "  [MASTER] " << grp.master.filename << " (" << grp.master.width << "x" << grp.master.height << ")\n";
            if (grp.slaves.empty()) {
                std::cout << "    (No near-duplicates found; stored standalone)\n";
            } else {
                for (const auto& slv : grp.slaves) {
                    std::cout << "    * [DELTA] " << slv.filename 
                              << " -> Can be stored as delta of " << grp.master.filename << "\n";
                }
            }
        }
    }
    std::cout << "==========================================\n";
}

bool commandCompress(const std::string& dir, const std::string& outArchiveTarget, uint8_t tolerance = 28) {
    auto images = collectImages(dir);
    if (images.empty()) {
        std::cerr << "Error: No images found in " << dir << "\n";
        return false;
    }

    std::string finalArchivePath = MetadataArchive::ensureMetaExtension(outArchiveTarget);

    if (fs::exists(finalArchivePath)) {
        std::cerr << "Error: Archive file already exists: " << finalArchivePath 
                  << ". Aborting to prevent accidental overwriting.\n";
        return false;
    }

    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint64_t> dist(100000, 999999);
    fs::path stagingDir = fs::temp_directory_path() / ("blockdedup_stage_" + std::to_string(dist(gen)));

    std::error_code ec;
    fs::create_directories(stagingDir, ec);
    if (ec) {
        std::cerr << "Error: Failed to create temporary staging directory: " << ec.message() << "\n";
        return false;
    }

    ArchiveManifest manifest;
    manifest.formatVersion = "1.0";
    manifest.formatTag = "blockdedup.meta";
    manifest.creationTimestamp = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    manifest.totalOriginalImages = images.size();
    manifest.tolerance = tolerance;

    SpatiotemporalClusterTree tree(50.0, 300);
    tree.build(images);
    auto clusters = tree.getNamedClusters();

    bool pipelineSuccess = true;
    for (const auto& cluster : clusters) {
        fs::path clusterOutDir = stagingDir / cluster.folderName;
        fs::create_directories(clusterOutDir, ec);

        auto visualGroups = ImageSimilarityScanner::groupSimilarImages(cluster.images, 0.40);
        for (const auto& grp : visualGroups) {
            ImageBuffer masterImg = ImageSimilarityScanner::loadImage(grp.master.filepath, 3);
            if (!masterImg.isValid()) {
                std::cerr << "Error: Failed to decode master image: " << grp.master.filepath << "\n";
                pipelineSuccess = false;
                break;
            }
            const int loadedW = masterImg.width;
            const int loadedH = masterImg.height;

            for (size_t sIdx = 0; sIdx < grp.slaves.size(); ++sIdx) {
                if (sIdx < grp.slaveAlignments.size()) {
                    const auto& align = grp.slaveAlignments[sIdx];
                    if (align.dx != 0 || align.dy != 0) {
                        ImageBuffer slaveImg = ImageSimilarityScanner::loadImage(grp.slaves[sIdx].filepath, 3);
                        if (slaveImg.isValid()) {
                            DeltaCompressor::expandMasterCanvas(masterImg, slaveImg, align.dx, align.dy);
                        }
                    }
                }
            }

            fs::path masterOut = clusterOutDir / ("master_" + grp.master.filename);
            if (masterImg.width != loadedW || masterImg.height != loadedH) {
                int written = stbi_write_jpg(masterOut.string().c_str(), masterImg.width, masterImg.height, masterImg.channels, masterImg.data.data(), 95);
                if (written == 0 || !fs::exists(masterOut)) {
                    std::cerr << "Error: Failed to write master image: " << masterOut.string() << "\n";
                    pipelineSuccess = false;
                    break;
                }
            } else {
                std::error_code mEc;
                fs::copy_file(grp.master.filepath, masterOut, fs::copy_options::overwrite_existing, mEc);
                if (mEc || !fs::exists(masterOut)) {
                    std::cerr << "Error: Failed to copy master image: " << mEc.message() << "\n";
                    pipelineSuccess = false;
                    break;
                }
            }

            ImageManifestEntry mEntry;
            mEntry.originalFilename = grp.master.filename;
            mEntry.clusterFolder = cluster.folderName;
            mEntry.originalSizeBytes = grp.master.fileSize;
            mEntry.sha256Checksum = MetadataArchive::computeSha256(grp.master.filepath);
            mEntry.isMaster = true;
            mEntry.masterReference = "";
            manifest.entries.push_back(mEntry);

            if (grp.slaves.empty()) {
                continue;
            }

            enum class SlaveStorageType { NONE, DELTA, STANDALONE };
            struct SlaveResult {
                SlaveStorageType type = SlaveStorageType::NONE;
                bool success = false;
                std::string error;
            };
            std::vector<SlaveResult> slaveResults(grp.slaves.size());

            #pragma omp parallel for schedule(dynamic)
            for (size_t sIdx = 0; sIdx < grp.slaves.size(); ++sIdx) {
                const auto& slave = grp.slaves[sIdx];
                SpatialAlignment align;
                if (sIdx < grp.slaveAlignments.size()) {
                    align = grp.slaveAlignments[sIdx];
                }

                std::string masterStored = "master_" + grp.master.filename;
                fs::path deltaOut = clusterOutDir / (masterStored + "@@" + slave.filename + ".bdelta");
                CompressionReport report;
                bool deltaOk = DeltaCompressor::createDeltaWithMaster(
                    masterImg, grp.master.filepath, slave.filepath, deltaOut.string(), tolerance, align, &report);

                if (deltaOk && report.deltaBytes < report.originalBytes) {
                    slaveResults[sIdx].type = SlaveStorageType::DELTA;
                    slaveResults[sIdx].success = true;
                } else {
                    // Fallback to standalone original copy
                    std::error_code rmEc;
                    if (deltaOk) {
                        fs::remove(deltaOut, rmEc);
                    }
                    std::error_code cpEc;
                    fs::path standaloneOut = clusterOutDir / slave.filename;
                    fs::copy_file(slave.filepath, standaloneOut, fs::copy_options::overwrite_existing, cpEc);
                    if (!cpEc && fs::exists(standaloneOut)) {
                        slaveResults[sIdx].type = SlaveStorageType::STANDALONE;
                        slaveResults[sIdx].success = true;
                    } else {
                        slaveResults[sIdx].type = SlaveStorageType::NONE;
                        slaveResults[sIdx].success = false;
                        slaveResults[sIdx].error = cpEc ? cpEc.message() : "Failed to copy slave as standalone file";
                    }
                }
            }

            // Record into manifest ONLY if successfully stored
            for (size_t sIdx = 0; sIdx < grp.slaves.size(); ++sIdx) {
                const auto& slave = grp.slaves[sIdx];
                const auto& res = slaveResults[sIdx];
                if (!res.success) {
                    std::cerr << "Error: Image '" << slave.filename << "' could not be stored in archive: " << res.error << "\n";
                    pipelineSuccess = false;
                    break;
                }

                ImageManifestEntry sEntry;
                sEntry.originalFilename = slave.filename;
                sEntry.clusterFolder = cluster.folderName;
                sEntry.originalSizeBytes = slave.fileSize;
                sEntry.sha256Checksum = MetadataArchive::computeSha256(slave.filepath);
                sEntry.isMaster = false;
                sEntry.masterReference = (res.type == SlaveStorageType::DELTA) ? grp.master.filename : "";
                manifest.entries.push_back(sEntry);
            }
        }
        if (!pipelineSuccess) break;
    }

    if (!pipelineSuccess) {
        fs::remove_all(stagingDir, ec);
        std::cerr << "Error: Archive generation failed during image processing. Aborting.\n";
        return false;
    }

    std::string errorMsg;
    bool packaged = MetadataArchive::createArchiveFromDirectory(stagingDir.string(), finalArchivePath, &manifest, &errorMsg);
    fs::remove_all(stagingDir, ec);

    if (!packaged) {
        std::cerr << "Error: Failed to package archive: " << errorMsg << "\n";
        return false;
    }

    std::cout << "Archive generated successfully: " << fs::path(finalArchivePath).filename().string() << "\n";
    return true;
}

void commandDecompress(const std::string& master, const std::string& delta, const std::string& out) {
    std::cout << "Reconstructing image from:\n"
              << "  Master: " << master << "\n"
              << "  Delta:  " << delta << "\n"
              << "  Output: " << out << "\n";
    if (DeltaCompressor::reconstructImage(master, delta, out)) {
        std::cout << "Successfully reconstructed to: " << out << " (" << (fs::file_size(out) / 1024) << " KB)\n";
    } else {
        std::cerr << "Failed to reconstruct image.\n";
    }
}

bool commandRestoreArchive(const std::string& archivePath, const std::string& reconDir) {
    if (!fs::exists(archivePath)) {
        std::cerr << "Error: Archive path does not exist: " << archivePath << "\n";
        return false;
    }

    fs::path workingSourceDir;
    bool isTemporaryDir = false;
    std::error_code ec;

    if (fs::is_regular_file(archivePath)) {
        if (!MetadataArchive::isValidArchive(archivePath)) {
            std::cerr << "Error: Invalid, corrupted, or truncated archive file: " << archivePath << "\n";
            return false;
        }

        std::random_device rd;
        std::mt19937_64 gen(rd());
        std::uniform_int_distribution<uint64_t> dist(100000, 999999);
        fs::path tempExtract = fs::temp_directory_path() / ("blockdedup_recon_" + std::to_string(dist(gen)));

        std::string err;
        if (!MetadataArchive::extractArchive(archivePath, tempExtract.string(), &err)) {
            std::cerr << "Error: Failed to extract archive: " << err << "\n";
            fs::remove_all(tempExtract, ec);
            return false;
        }

        workingSourceDir = tempExtract;
        isTemporaryDir = true;
    } else if (fs::is_directory(archivePath)) {
        workingSourceDir = fs::path(archivePath);
    } else {
        std::cerr << "Error: Unsupported archive path type: " << archivePath << "\n";
        return false;
    }

    fs::create_directories(reconDir, ec);

    std::vector<fs::path> clusterDirs;
    for (const auto& entry : fs::directory_iterator(workingSourceDir, ec)) {
        if (entry.is_directory()) {
            clusterDirs.push_back(entry.path());
        }
    }
    std::sort(clusterDirs.begin(), clusterDirs.end());

    std::atomic<size_t> totalRestored{0};
    std::atomic<size_t> deltaFailures{0};
    size_t standaloneFailures = 0;
    size_t masterFailures = 0;

    for (const auto& cDir : clusterDirs) {
        std::vector<fs::path> masters;
        std::vector<fs::path> deltas;
        std::vector<fs::path> standalones;

        for (const auto& file : fs::directory_iterator(cDir, ec)) {
            std::string name = file.path().filename().string();
            if (name.size() > 7 && name.substr(name.size() - 7) == ".bdelta") {
                deltas.push_back(file.path());
            } else if (name.rfind("master_", 0) == 0) {
                masters.push_back(file.path());
            } else if (file.is_regular_file() && name != ARCHIVE_MANIFEST_FILENAME) {
                standalones.push_back(file.path());
            }
        }

        // Copy standalone files
        for (const auto& sa : standalones) {
            fs::path dest = fs::path(reconDir) / sa.filename();
            std::error_code copyEc;
            fs::copy_file(sa, dest, fs::copy_options::overwrite_existing, copyEc);
            if (copyEc || !fs::exists(dest)) {
                std::cerr << "Error: Failed to copy standalone image: " << sa.filename().string() << "\n";
                standaloneFailures++;
            } else {
                totalRestored++;
            }
        }

        // Restore original names for masters
        for (const auto& m : masters) {
            std::string mName = m.filename().string();
            std::string origName = mName.substr(7);
            fs::path dest = fs::path(reconDir) / origName;
            std::error_code copyEc;
            fs::copy_file(m, dest, fs::copy_options::overwrite_existing, copyEc);
            if (copyEc || !fs::exists(dest)) {
                std::cerr << "Error: Failed to copy master image: " << m.filename().string() << "\n";
                masterFailures++;
            } else {
                totalRestored++;
            }
        }

        // Decompress all deltas against available master images
        #pragma omp parallel for schedule(dynamic)
        for (size_t dIdx = 0; dIdx < deltas.size(); ++dIdx) {
            const auto& d = deltas[dIdx];
            std::string dName = d.filename().string();
            std::string stem = dName.substr(0, dName.size() - 7);
            auto marker = stem.find("@@");
            std::string targetName = marker == std::string::npos ? stem : stem.substr(marker + 2);
            fs::path dest = fs::path(reconDir) / targetName;

            bool restored = false;
            if (marker != std::string::npos) {
                fs::path masterPath = cDir / stem.substr(0, marker);
                restored = DeltaCompressor::reconstructImage(masterPath.string(), d.string(), dest.string());
            } else {
                for (const auto& m : masters) {
                    if (DeltaCompressor::reconstructImage(m.string(), d.string(), dest.string())) {
                        restored = true;
                        break;
                    }
                }
            }

            if (restored && fs::exists(dest)) {
                totalRestored++;
            } else {
                deltaFailures++;
                #pragma omp critical
                {
                    std::cerr << "Error: Failed to reconstruct delta: " << dName << "\n";
                }
            }
        }
    }

    bool manifestVerified = true;
    auto manifestOpt = MetadataArchive::readManifest(workingSourceDir.string());
    if (manifestOpt) {
        if (totalRestored.load() != manifestOpt->totalOriginalImages) {
            std::cerr << "Error: Restored image count (" << totalRestored.load() 
                      << ") does not match manifest count (" << manifestOpt->totalOriginalImages << ")!\n";
            manifestVerified = false;
        }

        std::vector<std::string> mismatches;
        if (!MetadataArchive::verifyRestoredImages(reconDir, *manifestOpt, mismatches)) {
            std::cerr << "Error: " << mismatches.size() << " restored files failed verification:\n";
            for (const auto& m : mismatches) {
                std::cerr << "  - " << m << "\n";
            }
            manifestVerified = false;
        }
    }

    if (isTemporaryDir) {
        fs::remove_all(workingSourceDir, ec);
    }

    bool overallSuccess = (standaloneFailures == 0 && masterFailures == 0 &&
                           deltaFailures.load() == 0 && manifestVerified);

    if (!overallSuccess) {
        std::cerr << "Restoration FAILED: Restoration completed with errors. Target may be incomplete.\n";
        return false;
    }

    std::cout << "Restoration completed successfully: " << totalRestored.load() << " images restored to " << reconDir << "\n";
    return true;
}

#ifndef BLOCKDEDUP_TEST_RUNNER
int main(int argc, char* argv[]) {
    if (argc < 2) {
        printUsage(argv[0]);
        return 1;
    }

    std::string cmd = argv[1];
    if (cmd == "inspect" && argc >= 3) {
        commandInspect(argv[2]);
    } else if (cmd == "tree" && argc >= 3) {
        commandTree(argv[2]);
    } else if (cmd == "scan" && argc >= 3) {
        commandScan(argv[2]);
    } else if (cmd == "compress" && argc >= 4) {
        uint8_t tol = (argc >= 5) ? static_cast<uint8_t>(std::stoi(argv[4])) : 28;
        if (!commandCompress(argv[2], argv[3], tol)) {
            return 1;
        }
    } else if (cmd == "decompress" && argc >= 5) {
        commandDecompress(argv[2], argv[3], argv[4]);
    } else if (cmd == "restore" && argc >= 4) {
        if (!commandRestoreArchive(argv[2], argv[3])) {
            return 1;
        }
    } else if (cmd == "demo") {
        std::string dir = (argc >= 3) ? argv[2] : "sample";
        std::cout << "\n>>> STEP 1: Inspect Sample Image Telemetry <<<\n";
        commandInspect(dir + "/IMG_0001.JPG");
        commandInspect(dir + "/IMG_0003.JPG");

        std::cout << "\n>>> STEP 2: Spatiotemporal Tree Clustering <<<\n";
        commandTree(dir);

        std::cout << "\n>>> STEP 3: Visual Scanning & Master/Slave Grouping <<<\n";
        commandScan(dir);

        fs::path in = dir;
        fs::path archive = in.parent_path() / (in.filename().string() + "_archive.meta");
        fs::path restored = in.parent_path() / (in.filename().string() + "_restored");
        std::cout << "\n>>> STEP 4: Compress <<<\n";
        if (!commandCompress(dir, archive.string(), 28)) {
            return 1;
        }
        std::cout << "\n>>> STEP 5: Restore <<<\n";
        if (!commandRestoreArchive(archive.string(), restored.string())) {
            return 1;
        }
    } else {
        printUsage(argv[0]);
        return 1;
    }

    return 0;
}
#endif
