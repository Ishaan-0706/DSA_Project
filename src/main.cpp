#include "blockdedup/metadata.hpp"
#include "blockdedup/image_tree.hpp"
#include "blockdedup/image_similarity.hpp"
#include "blockdedup/delta_compressor.hpp"
#include "blockdedup/output_paths.hpp"

#include <iostream>
#include <vector>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace blockdedup;

void printUsage(const char* prog) {
    std::cout << "Usage: " << prog << "\n"
              << "       " << prog << " <command> [arguments...]\n\n"
              << "With no command, choose archive or restore.\n"
              << "Archive writes <name>_archive, then removes the photo folder.\n"
              << "Restore rebuilds trip_archive back into trip, then removes the archive.\n\n"
              << "Commands:\n"
              << "  (no command) | tui                 Archive a folder, or restore an archive\n"
              << "  inspect <image_path>               Parse & inspect EXIF/XMP & GPS telemetry\n"
              << "  tree <image_dir>                   Build & display Spatiotemporal Cluster Tree\n"
              << "  scan <image_dir>                   Tree clustering + visual scanning (find Masters & Slaves)\n"
              << "  compress <image_dir> <out_dir> [tolerance]\n"
              << "                                     Master/delta archive. Default tolerance 28\n"
              << "  decompress <master> <delta> <out>  Reconstruct one image from a master and a delta\n"
              << "  restore <archive_dir> <out_dir>    Reconstruct every image in an archive\n"
              << "  demo [image_dir]                   Inspect, cluster, compress, and restore\n"
              << "  help                               Show this list\n\n";
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
        // Skip output directories or delta files
        if (pStr.find("compressed") != std::string::npos || pStr.find(".bdelta") != std::string::npos) {
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
    SpatiotemporalClusterTree tree(50.0, 300); // 50m radius, 5 min window
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

#include <atomic>
#ifdef BLOCKDEDUP_HAS_OPENMP
#include <omp.h>
#endif
#include "third_party/stb_image_write.h"

void commandCompress(const std::string& dir, const std::string& outDir, uint8_t tolerance = 28) {
    auto images = collectImages(dir);
    if (images.empty()) {
        std::cout << "No images found in " << dir << "\n";
        return;
    }
    fs::create_directories(outDir);

    std::cout << "=== Starting Archival Pipeline ===\n";
    std::cout << "Discovered " << images.size() << " image files.\n";

    std::cout << "Building Spatiotemporal Cluster Tree...\n";
    SpatiotemporalClusterTree tree(50.0, 300);
    tree.build(images);
    auto clusters = tree.getNamedClusters();

    std::cout << "Tree partitioned images into " << clusters.size() << " distinct category/session clusters.\n";

    size_t totalOriginal = 0;
    size_t totalCompressed = 0;
    size_t masterCount = 0;
    size_t deltaCount = 0;

    struct FolderStats {
        std::string folderName;
        std::string description;
        size_t originalBytes = 0;
        size_t archivedBytes = 0;
        size_t masters = 0;
        size_t deltas = 0;
    };
    std::vector<FolderStats> clusterStats;

    for (const auto& cluster : clusters) {
        fs::path clusterOutDir = fs::path(outDir) / cluster.folderName;
        fs::create_directories(clusterOutDir);

        FolderStats stats;
        stats.folderName = cluster.folderName;
        stats.description = cluster.description;

        std::cout << "\n--------------------------------------------------\n";
        std::cout << ">>> Processing: " << cluster.description << "\n";
        std::cout << "    Destination Folder: " << clusterOutDir.string() << "\n";
        std::cout << "    Candidate Images:   " << cluster.images.size() << "\n";

        auto visualGroups = ImageSimilarityScanner::groupSimilarImages(cluster.images, 0.40);
        for (const auto& grp : visualGroups) {
            // Check if master needs canvas expansion from any aligned slaves
            ImageBuffer masterImg = ImageSimilarityScanner::loadImage(grp.master.filepath, 3);
            if (!masterImg.isValid()) {
                std::cerr << "  Failed to decode master image: " << grp.master.filepath << "\n";
                continue;
            }
            const int loadedW = masterImg.width;
            const int loadedH = masterImg.height;

            // Expand master canvas with missing borders from any slaves that extend beyond (capped)
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

            // Copy/save master image
            fs::path masterOut = clusterOutDir / ("master_" + grp.master.filename);
            size_t mSize = 0;
            if (masterImg.width != loadedW || masterImg.height != loadedH) {
                stbi_write_jpg(masterOut.string().c_str(), masterImg.width, masterImg.height, masterImg.channels, masterImg.data.data(), 95);
            } else {
                fs::copy_file(grp.master.filepath, masterOut, fs::copy_options::overwrite_existing);
            }
            mSize = fs::file_size(masterOut);
            totalOriginal += grp.master.fileSize;
            totalCompressed += mSize;
            stats.originalBytes += grp.master.fileSize;
            stats.archivedBytes += mSize;
            masterCount++;
            stats.masters++;

            std::cout << "\n  [MASTER SAVED] " << masterOut.filename().string() 
                      << " (" << masterImg.width << "x" << masterImg.height << ", "
                      << std::fixed << std::setprecision(2) << (mSize / 1024.0 / 1024.0) << " MB)\n";

            if (grp.slaves.empty()) {
                continue;
            }

            std::cout << "  Encoding " << grp.slaves.size() << " images as deltas with spatial alignment (tolerance: " << (int)tolerance << ")...\n";

            std::atomic<size_t> grpOrig{0};
            std::atomic<size_t> grpComp{0};
            std::atomic<size_t> grpDeltas{0};
            std::atomic<size_t> progress{0};

            #pragma omp parallel for schedule(dynamic)
            for (size_t sIdx = 0; sIdx < grp.slaves.size(); ++sIdx) {
                const auto& slave = grp.slaves[sIdx];
                SpatialAlignment align;
                if (sIdx < grp.slaveAlignments.size()) {
                    align = grp.slaveAlignments[sIdx];
                }

                // shortcut: a filename containing @@ links to the wrong master, rename that file.
                std::string masterStored = "master_" + grp.master.filename;
                fs::path deltaOut = clusterOutDir / (masterStored + "@@" + slave.filename + ".bdelta");
                CompressionReport report;
                if (DeltaCompressor::createDeltaWithMaster(masterImg, grp.master.filepath, slave.filepath, deltaOut.string(), tolerance, align, &report)) {
                    if (report.deltaBytes < report.originalBytes) {
                        grpOrig += report.originalBytes;
                        grpComp += report.deltaBytes;
                        grpDeltas++;
                    } else {
                        // Delta did not shrink file; keep original file standalone
                        std::error_code ec;
                        fs::remove(deltaOut, ec);
                        fs::copy_file(slave.filepath, clusterOutDir / slave.filename, fs::copy_options::overwrite_existing, ec);
                        grpOrig += report.originalBytes;
                        grpComp += report.originalBytes;
                    }
                }

                size_t p = ++progress;
                if (p % 10 == 0 || p == grp.slaves.size()) {
                    #pragma omp critical
                    {
                        std::cout << "    [Progress] " << p << " / " << grp.slaves.size() << " deltas generated\n";
                    }
                }
            }

            totalOriginal += grpOrig.load();
            totalCompressed += grpComp.load();
            deltaCount += grpDeltas.load();
            stats.originalBytes += grpOrig.load();
            stats.archivedBytes += grpComp.load();
            stats.deltas += grpDeltas.load();
        }

        clusterStats.push_back(stats);
    }

    auto toSizeStr = [](size_t bytes) -> std::string {
        std::ostringstream ss;
        if (bytes >= 1024 * 1024 * 1024) {
            ss << std::fixed << std::setprecision(2) << (bytes / 1024.0 / 1024.0 / 1024.0) << " GB (" << bytes << " bytes)";
        } else if (bytes >= 1024 * 1024) {
            ss << std::fixed << std::setprecision(2) << (bytes / 1024.0 / 1024.0) << " MB (" << bytes << " bytes)";
        } else {
            ss << std::fixed << std::setprecision(1) << (bytes / 1024.0) << " KB (" << bytes << " bytes)";
        }
        return ss.str();
    };

    std::cout << "\n================ ARCHIVAL SUMMARY ================\n";
    std::cout << "Total Images Processed: " << images.size() << "\n";
    std::cout << "Master Images Stored:   " << masterCount << "\n";
    std::cout << "Delta Files Created:    " << deltaCount << "\n";
    std::cout << "--------------------------------------------------\n";
    std::cout << "Per-Cluster Breakdown:\n";
    for (const auto& fs : clusterStats) {
        double sav = (fs.originalBytes > 0) ? (1.0 - static_cast<double>(fs.archivedBytes) / fs.originalBytes) * 100.0 : 0.0;
        std::cout << "  * [" << fs.folderName << "]\n"
                  << "      Type / Description: " << fs.description << "\n"
                  << "      Masters: " << fs.masters << ", Deltas: " << fs.deltas << "\n"
                  << "      Original: " << toSizeStr(fs.originalBytes) 
                  << " -> Archived: " << toSizeStr(fs.archivedBytes) 
                  << " (Saved: " << std::fixed << std::setprecision(1) << sav << "%)\n";
    }
    std::cout << "--------------------------------------------------\n";
    std::cout << "Overall Original Size:  " << toSizeStr(totalOriginal) << "\n";
    std::cout << "Overall Archived Size:  " << toSizeStr(totalCompressed) << "\n";
    double totalSavings = (totalOriginal > 0) ? (1.0 - static_cast<double>(totalCompressed) / totalOriginal) * 100.0 : 0.0;
    std::cout << "Net Disk Space Saved:   " << std::fixed << std::setprecision(2) << totalSavings << "%\n";
    std::cout << "==================================================\n";
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

void commandRestoreArchive(const std::string& archiveDir, const std::string& reconDir) {
    if (!fs::exists(archiveDir)) {
        std::cerr << "Archive directory does not exist: " << archiveDir << "\n";
        return;
    }

    fs::create_directories(reconDir);
    std::cout << "=== Reconstructing Dataset from Archive ===\n";
    std::cout << "Archive Directory: " << archiveDir << "\n";
    std::cout << "Target Directory:  " << reconDir << "\n\n";

    std::vector<fs::path> clusterDirs;
    for (const auto& entry : fs::directory_iterator(archiveDir)) {
        if (entry.is_directory()) {
            clusterDirs.push_back(entry.path());
        }
    }
    std::sort(clusterDirs.begin(), clusterDirs.end());

    size_t totalRestored = 0;
    size_t mastersCount = 0;
    size_t deltasCount = 0;
    size_t standalonesCount = 0;

    for (const auto& cDir : clusterDirs) {
        std::cout << ">>> Reconstructing cluster: " << cDir.filename().string() << "\n";

        // Find all masters in this cluster
        std::vector<fs::path> masters;
        std::vector<fs::path> deltas;
        std::vector<fs::path> standalones;

        for (const auto& file : fs::directory_iterator(cDir)) {
            std::string name = file.path().filename().string();
            if (name.size() > 7 && name.substr(name.size() - 7) == ".bdelta") {
                deltas.push_back(file.path());
            } else if (name.rfind("master_", 0) == 0) {
                masters.push_back(file.path());
            } else if (file.is_regular_file()) {
                standalones.push_back(file.path());
            }
        }

        // Copy standalone files
        for (const auto& sa : standalones) {
            fs::path dest = fs::path(reconDir) / sa.filename();
            fs::copy_file(sa, dest, fs::copy_options::overwrite_existing);
            standalonesCount++;
            totalRestored++;
        }

        // Restore original names for masters
        for (const auto& m : masters) {
            std::string mName = m.filename().string();
            std::string origName = mName.substr(7); // strip "master_"
            fs::path dest = fs::path(reconDir) / origName;
            fs::copy_file(m, dest, fs::copy_options::overwrite_existing);
            mastersCount++;
            totalRestored++;
        }

        // Decompress all deltas against available master images
        std::cout << "  Restoring " << deltas.size() << " delta images...\n";
        std::atomic<size_t> dProgress{0};

        #pragma omp parallel for schedule(dynamic)
        for (size_t dIdx = 0; dIdx < deltas.size(); ++dIdx) {
            const auto& d = deltas[dIdx];
            std::string dName = d.filename().string();
            std::string stem = dName.substr(0, dName.size() - 7); // strip ".bdelta"
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

            if (restored) {
                #pragma omp atomic
                deltasCount++;
                #pragma omp atomic
                totalRestored++;
            } else {
                #pragma omp critical
                {
                    std::cerr << "  [Error] Failed to reconstruct delta: " << dName << "\n";
                }
            }

            size_t p = ++dProgress;
            if (p % 10 == 0 || p == deltas.size()) {
                #pragma omp critical
                {
                    std::cout << "    [Progress] " << p << " / " << deltas.size() << " deltas restored\n";
                }
            }
        }
    }

    std::cout << "\n================ RECONSTRUCTION SUMMARY ================\n";
    std::cout << "Total Images Restored:  " << totalRestored << "\n";
    std::cout << "  - Direct Masters:     " << mastersCount << "\n";
    std::cout << "  - Decompressed Deltas:" << deltasCount << "\n";
    std::cout << "  - Standalone Copies:  " << standalonesCount << "\n";
    std::cout << "Output Folder:          " << reconDir << "\n";
    std::cout << "========================================================\n";
}

std::string trimCopy(std::string text) {
    auto notSpace = [](unsigned char c) { return std::isspace(c) == 0; };
    auto begin = std::find_if(text.begin(), text.end(), notSpace);
    auto end = std::find_if(text.rbegin(), text.rend(), notSpace).base();
    if (begin >= end) return {};
    return std::string(begin, end);
}

std::string normalizeFolderInput(std::string line) {
    line = trimCopy(std::move(line));
    if (line.size() >= 2) {
        char open = line.front();
        char close = line.back();
        if ((open == '"' && close == '"') || (open == '\'' && close == '\'')) {
            line = trimCopy(line.substr(1, line.size() - 2));
        }
    }
    if (line == "~" || line.rfind("~/", 0) == 0) {
        const char* home = std::getenv("HOME");
        if (home != nullptr && home[0] != '\0') line.replace(0, 1, home);
    }
    return line;
}

bool pathOccupied(const fs::path& path) {
    std::error_code ec;
    fs::file_status status = fs::symlink_status(path, ec);
    return !ec && status.type() != fs::file_type::not_found;
}

bool readLine(std::string& line) {
    if (!std::getline(std::cin, line)) {
        std::cout << "\n";
        return false;
    }
    line = normalizeFolderInput(std::move(line));
    return true;
}

bool quitWord(const std::string& line) {
    return line.empty() || line == "q" || line == "Q";
}

bool readFolder(const char* prompt, fs::path& input) {
    while (true) {
        std::cout << prompt << std::flush;
        std::string line;
        if (!readLine(line)) return false;
        if (quitWord(line)) return false;

        fs::path raw(line);
        std::error_code ec;
        if (!fs::exists(raw, ec) || ec) {
            std::cout << "No folder at that path. Check the path and try again.\n";
            continue;
        }
        if (!fs::is_directory(raw, ec) || ec) {
            std::cout << "That path is a file, not a folder. Give a folder path.\n";
            continue;
        }
        input = fs::absolute(raw).lexically_normal();
        if (input.filename().empty()) input = input.parent_path();
        return true;
    }
}

bool confirmedYes(const char* prompt) {
    std::cout << prompt << std::flush;
    std::string answer;
    if (!readLine(answer)) {
        std::cout << "Stopped. Nothing was changed.\n";
        return false;
    }
    if (answer != "y" && answer != "Y") {
        std::cout << "Stopped. Nothing was changed.\n";
        return false;
    }
    return true;
}

size_t countStoredFiles(const fs::path& root) {
    size_t count = 0;
    std::error_code ec;
    if (!fs::exists(root, ec) || ec) return 0;
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
    fs::recursive_directory_iterator end;
    for (; it != end; it.increment(ec)) {
        if (ec) {
            ec.clear();
            continue;
        }
        if (it->is_regular_file()) count++;
    }
    return count;
}

int runArchive() {
    std::cout << "\nThe photo folder is removed after the archive is written.\n"
              << "Only <name>_archive is left. Choose Restore later to get the photos back.\n\n";

    fs::path input;
    if (!readFolder("Photo folder: ", input)) return 0;

    std::vector<ImageMetadata> images = collectImages(input.string());
    if (images.empty()) {
        std::cout << "No photos in that folder.\n"
                  << "This looks for jpg, png, webp, bmp, and common camera raw files.\n";
        return 1;
    }

    OutputPaths outs = outputPathsFor(input);
    if (outs.clashesWithSource || samePath(outs.archive, input)) {
        std::cout << "Stopped. The archive path is the same as the photo folder, so nothing was deleted.\n";
        return 1;
    }

    std::cout << "\nPhotos found: " << images.size() << "\n\n"
              << "Archive:  " << outs.archive.string() << "\n"
              << "Removes:  " << input.string() << "\n";

    if (pathOccupied(outs.archive)) {
        if (!confirmedYes("\nReplace the existing archive? [y/N] ")) return 0;
        outs = outputPathsFor(input);
        if (outs.clashesWithSource || samePath(outs.archive, input)) {
            std::cout << "Stopped. The archive path is the same as the photo folder, so nothing was deleted.\n";
            return 1;
        }
        std::error_code rm;
        fs::remove_all(outs.archive, rm);
        if (rm) {
            std::cout << "Could not replace the archive folder.\n"
                      << rm.message() << "\n"
                      << "Nothing new was written. The photo folder was not changed.\n";
            return 1;
        }
    }

    std::cout << "\nArchiving…\n";
    commandCompress(input.string(), outs.archive.string(), 28);
    size_t stored = countStoredFiles(outs.archive);
    if (!pathOccupied(outs.archive) || stored < images.size()) {
        std::cout << "\nThe archive does not contain every photo, so the photo folder was kept.\n"
                  << "Archive: " << outs.archive.string() << "\n";
        return 1;
    }

    std::error_code rm;
    fs::remove_all(input, rm);
    if (rm || pathOccupied(input)) {
        std::cout << "\nThe archive was written, but the photo folder could not be removed.\n"
                  << "Archive: " << outs.archive.string() << "\n"
                  << "Photos:  " << input.string() << "\n";
        return 1;
    }

    fs::path restored = outs.restored;
    if (pathOccupied(restored) && !samePath(restored, input) && !samePath(restored, outs.archive)) {
        std::error_code restoredRm;
        fs::remove_all(restored, restoredRm);
        if (restoredRm || pathOccupied(restored)) {
            std::cout << "\nDone. The photo folder was removed, but the old restored folder is still there.\n"
                      << "Archive:  " << outs.archive.string() << "\n"
                      << "Restored: " << restored.string() << "\n";
            return 1;
        }
    }

    std::cout << "\nDone.\n\n"
              << "Archive: " << outs.archive.string() << "\n"
              << "Removed: " << input.string() << "\n\n"
              << "Only the archive is left. Choose Restore later and give that archive path.\n";
    return 0;
}

int runRestore() {
    std::cout << "\nGive the archive folder. trip_archive is rebuilt into trip, next to it.\n"
              << "The archive is removed after the photos are back.\n\n";

    fs::path archive;
    if (!readFolder("Archive folder: ", archive)) return 0;

    fs::path dest = restoreDestinationFor(archive);
    if (samePath(dest, archive)) {
        std::cout << "Stopped. The restored photos would replace the archive, so nothing was written.\n";
        return 1;
    }

    size_t stored = countStoredFiles(archive);
    std::cout << "\nArchive: " << archive.string() << "\n"
              << "Photos:  " << dest.string() << "\n"
              << "Removes: " << archive.string() << " after the photos are back\n";

    if (pathOccupied(dest)) {
        if (!confirmedYes("\nReplace the photos already in that folder? [y/N] ")) return 0;
        if (samePath(dest, archive)) {
            std::cout << "Stopped. The restored photos would replace the archive, so nothing was written.\n";
            return 1;
        }
        std::error_code rm;
        fs::remove_all(dest, rm);
        if (rm) {
            std::cout << "Could not replace the photo folder.\n" << rm.message() << "\n";
            return 1;
        }
    }

    std::cout << "\nRestoring every photo…\n";
    commandRestoreArchive(archive.string(), dest.string());
    size_t restoredCount = collectImages(dest.string()).size();
    if (restoredCount == 0 || countStoredFiles(dest) < stored) {
        std::cout << "\nThe photos were not all rebuilt, so the archive was kept.\n"
                  << "Archive: " << archive.string() << "\n"
                  << "Photos:  " << dest.string() << "\n";
        return 1;
    }

    std::error_code rm;
    fs::remove_all(archive, rm);
    if (rm || pathOccupied(archive)) {
        std::cout << "\nThe photos were rebuilt, but the archive could not be removed.\n"
                  << "Photos:  " << dest.string() << "\n"
                  << "Archive: " << archive.string() << "\n";
        return 1;
    }

    std::cout << "\nDone.\n\n"
              << "Photos:  " << dest.string() << "\n"
              << "         " << restoredCount << (restoredCount == 1 ? " photo\n" : " photos\n")
              << "Removed: " << archive.string() << "\n";
    return 0;
}

int runTui() {
    std::cout << "BlockDedup\n\n"
              << "1  Archive photos\n"
              << "   The photo folder is removed. Only <name>_archive is left.\n\n"
              << "2  Restore photos\n"
              << "   Give the archive folder. trip_archive is rebuilt into trip,\n"
              << "   then the archive is removed.\n\n"
              << "Press Enter on an empty line to quit.\n\n";

    while (true) {
        std::cout << "Choice: " << std::flush;
        std::string choice;
        if (!readLine(choice)) return 0;
        if (quitWord(choice)) return 0;
        if (choice == "1") return runArchive();
        if (choice == "2") return runRestore();
        std::cout << "Type 1 to archive, or 2 to restore.\n";
    }
}

int main(int argc, char* argv[]) {
    if (argc < 2 || std::string(argv[1]) == "tui") {
        if (!isatty(STDIN_FILENO)) {
            printUsage(argv[0]);
            return 1;
        }
        return runTui();
    }

    std::string cmd = argv[1];
    if (cmd == "help" || cmd == "-h" || cmd == "--help") {
        printUsage(argv[0]);
        return 0;
    }
    if (cmd == "inspect" && argc >= 3) {
        commandInspect(argv[2]);
    } else if (cmd == "tree" && argc >= 3) {
        commandTree(argv[2]);
    } else if (cmd == "scan" && argc >= 3) {
        commandScan(argv[2]);
    } else if (cmd == "compress" && argc >= 4) {
        uint8_t tol = (argc >= 5) ? static_cast<uint8_t>(std::stoi(argv[4])) : 28;
        commandCompress(argv[2], argv[3], tol);
    } else if (cmd == "decompress" && argc >= 5) {
        commandDecompress(argv[2], argv[3], argv[4]);
    } else if (cmd == "restore" && argc >= 4) {
        commandRestoreArchive(argv[2], argv[3]);
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
        fs::path archive = in.parent_path() / (in.filename().string() + "_archive");
        fs::path restored = in.parent_path() / (in.filename().string() + "_restored");
        std::cout << "\n>>> STEP 4: Compress <<<\n";
        commandCompress(dir, archive.string(), 28);
        std::cout << "\n>>> STEP 5: Restore <<<\n";
        commandRestoreArchive(archive.string(), restored.string());
    } else {
        printUsage(argv[0]);
        return 1;
    }

    return 0;
}
