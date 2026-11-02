#include "blockdedup/image_tree.hpp"
#include <iostream>
#include <algorithm>
#include <cmath>
#include <filesystem>

namespace blockdedup {

SpatiotemporalClusterTree::SpatiotemporalClusterTree(double maxSpatialDistanceMeters, int64_t maxTemporalGapSeconds)
    : maxDistanceMeters_(maxSpatialDistanceMeters),
      maxTemporalGapSeconds_(maxTemporalGapSeconds),
      root_(std::make_unique<TreeNode>(NodeType::ROOT, "Repository Root")) {}

void SpatiotemporalClusterTree::insertGpsImage(TreeNode* spatialRoot, const ImageMetadata& meta) {
    TreeNode* matchedSpatialNode = nullptr;
    double bestDist = 1e9;

    for (auto& child : spatialRoot->children) {
        if (child->type == NodeType::SPATIAL_CLUSTER) {
            double dist = child->centerGps.distanceTo(meta.gps);
            if (dist >= 0 && dist <= maxDistanceMeters_ && dist < bestDist) {
                bestDist = dist;
                matchedSpatialNode = child.get();
            }
        }
    }

    if (!matchedSpatialNode) {
        std::string locLabel = "Location (" + std::to_string(meta.gps.latitude) + ", " +
                               std::to_string(meta.gps.longitude) + ")";
        auto newSpatial = std::make_unique<TreeNode>(NodeType::SPATIAL_CLUSTER, locLabel);
        newSpatial->centerGps = meta.gps;
        matchedSpatialNode = newSpatial.get();
        spatialRoot->children.push_back(std::move(newSpatial));
    }

    // Now insert into a temporal session node inside this spatial cluster
    TreeNode* matchedSessionNode = nullptr;
    for (auto& child : matchedSpatialNode->children) {
        if (child->type == NodeType::TEMPORAL_SESSION) {
            // Check if meta.timestamp is within temporal gap
            if (std::abs(meta.timestamp - child->minTimestamp) <= maxTemporalGapSeconds_ ||
                std::abs(meta.timestamp - child->maxTimestamp) <= maxTemporalGapSeconds_ ||
                (meta.timestamp >= child->minTimestamp && meta.timestamp <= child->maxTimestamp)) {
                matchedSessionNode = child.get();
                break;
            }
        }
    }

    if (!matchedSessionNode) {
        std::string timeLabel = "Session @ " + meta.timestampStr;
        auto newSession = std::make_unique<TreeNode>(NodeType::TEMPORAL_SESSION, timeLabel);
        newSession->minTimestamp = meta.timestamp;
        newSession->maxTimestamp = meta.timestamp;
        matchedSessionNode = newSession.get();
        matchedSpatialNode->children.push_back(std::move(newSession));
    } else {
        matchedSessionNode->minTimestamp = std::min(matchedSessionNode->minTimestamp, meta.timestamp);
        matchedSessionNode->maxTimestamp = std::max(matchedSessionNode->maxTimestamp, meta.timestamp);
    }

    matchedSessionNode->images.push_back(meta);
}

void SpatiotemporalClusterTree::insertNonGpsImage(TreeNode* nonGpsRoot, const ImageMetadata& meta) {
    std::string ext = std::filesystem::path(meta.filepath).extension().string();
    for (char& c : ext) c = toupper(c);
    std::string formatTag = (ext == ".CR2" || ext == ".NEF" || ext == ".ARW" || ext == ".DNG") ? "RAW_" + ext.substr(1) : "JPEG";

    std::string deviceKey;
    if (meta.make.empty() && meta.model.empty()) {
        deviceKey = "Moon_Series_" + std::to_string(meta.width) + "x" + std::to_string(meta.height);
    } else {
        std::string baseDev;
        if (!meta.make.empty() && meta.model.find(meta.make) != std::string::npos) {
            baseDev = meta.model;
        } else {
            baseDev = meta.make + " " + meta.model;
        }
        deviceKey = baseDev + "_" + formatTag;
    }
    TreeNode* matchedDeviceNode = nullptr;

    for (auto& child : nonGpsRoot->children) {
        if (child->label == deviceKey) {
            matchedDeviceNode = child.get();
            break;
        }
    }

    if (!matchedDeviceNode) {
        auto newDevice = std::make_unique<TreeNode>(NodeType::SPATIAL_CLUSTER, deviceKey);
        matchedDeviceNode = newDevice.get();
        nonGpsRoot->children.push_back(std::move(newDevice));
    }

    // Insert into temporal session under device node
    TreeNode* matchedSessionNode = nullptr;
    for (auto& child : matchedDeviceNode->children) {
        if (child->type == NodeType::TEMPORAL_SESSION) {
            if (std::abs(meta.timestamp - child->minTimestamp) <= maxTemporalGapSeconds_ ||
                std::abs(meta.timestamp - child->maxTimestamp) <= maxTemporalGapSeconds_ ||
                (meta.timestamp >= child->minTimestamp && meta.timestamp <= child->maxTimestamp)) {
                matchedSessionNode = child.get();
                break;
            }
        }
    }

    if (!matchedSessionNode) {
        std::string timeLabel = "Session @ " + meta.timestampStr;
        auto newSession = std::make_unique<TreeNode>(NodeType::TEMPORAL_SESSION, timeLabel);
        newSession->minTimestamp = meta.timestamp;
        newSession->maxTimestamp = meta.timestamp;
        matchedSessionNode = newSession.get();
        matchedDeviceNode->children.push_back(std::move(newSession));
    } else {
        matchedSessionNode->minTimestamp = std::min(matchedSessionNode->minTimestamp, meta.timestamp);
        matchedSessionNode->maxTimestamp = std::max(matchedSessionNode->maxTimestamp, meta.timestamp);
    }

    matchedSessionNode->images.push_back(meta);
}

void SpatiotemporalClusterTree::insert(const ImageMetadata& meta) {
    if (meta.gps.valid) {
        // Find or create GPS sub-root
        TreeNode* gpsRoot = nullptr;
        for (auto& child : root_->children) {
            if (child->label == "Geo-tagged Clusters") {
                gpsRoot = child.get();
                break;
            }
        }
        if (!gpsRoot) {
            auto node = std::make_unique<TreeNode>(NodeType::ROOT, "Geo-tagged Clusters");
            gpsRoot = node.get();
            root_->children.push_back(std::move(node));
        }
        insertGpsImage(gpsRoot, meta);
    } else {
        // Find or create Non-GPS sub-root
        TreeNode* nonGpsRoot = nullptr;
        for (auto& child : root_->children) {
            if (child->label == "Non-Geo Clusters (Grouped by Device & Time)") {
                nonGpsRoot = child.get();
                break;
            }
        }
        if (!nonGpsRoot) {
            auto node = std::make_unique<TreeNode>(NodeType::ROOT, "Non-Geo Clusters (Grouped by Device & Time)");
            nonGpsRoot = node.get();
            root_->children.push_back(std::move(node));
        }
        insertNonGpsImage(nonGpsRoot, meta);
    }
}

void SpatiotemporalClusterTree::build(const std::vector<ImageMetadata>& metadataList) {
    for (const auto& meta : metadataList) {
        insert(meta);
    }
}

void SpatiotemporalClusterTree::collectLeaves(const TreeNode* node, std::vector<std::vector<ImageMetadata>>& clusters) const {
    if (!node) return;
    if (!node->images.empty()) {
        clusters.push_back(node->images);
    }
    for (const auto& child : node->children) {
        collectLeaves(child.get(), clusters);
    }
}

std::vector<std::vector<ImageMetadata>> SpatiotemporalClusterTree::getCandidateClusters() const {
    std::vector<std::vector<ImageMetadata>> clusters;
    collectLeaves(root_.get(), clusters);
    return clusters;
}

namespace {
std::string sanitizeDirName(const std::string& input) {
    std::string s;
    for (char c : input) {
        if (std::isalnum(c) || c == '-' || c == '_') {
            s.push_back(c);
        } else if (c == ' ' || c == ':' || c == '@' || c == '(' || c == ')' || c == '.') {
            if (s.empty() || s.back() != '_') s.push_back('_');
        }
    }
    while (!s.empty() && s.back() == '_') s.pop_back();
    return s.empty() ? "Cluster" : s;
}
} // namespace

std::vector<ImageCluster> SpatiotemporalClusterTree::getNamedClusters() const {
    std::vector<ImageCluster> result;
    int clusterId = 1;

    for (const auto& catNode : root_->children) {
        for (const auto& devNode : catNode->children) {
            for (const auto& sessNode : devNode->children) {
                if (!sessNode->images.empty()) {
                    std::string rawName = devNode->label + "_" + sessNode->label;
                    std::string cleanName = "cluster_" + std::to_string(clusterId++) + "_" + sanitizeDirName(rawName);
                    result.push_back({cleanName, devNode->label + " (" + sessNode->label + ")", sessNode->images});
                }
            }
        }
    }
    return result;
}

void SpatiotemporalClusterTree::printNode(const TreeNode* node, int indent) const {
    if (!node) return;
    std::string pad(indent * 2, ' ');
    std::cout << pad << "|-- [" << node->label << "]";
    if (!node->images.empty()) {
        std::cout << " (" << node->images.size() << " images)";
        for (const auto& img : node->images) {
            std::cout << "\n" << pad << "    * " << img.filename << " (" << img.width << "x" << img.height 
                      << ", " << (img.fileSize / 1024) << " KB)";
        }
    }
    std::cout << "\n";
    for (const auto& child : node->children) {
        printNode(child.get(), indent + 1);
    }
}

void SpatiotemporalClusterTree::printTree() const {
    std::cout << "\n=== Spatiotemporal Cluster Tree ===\n";
    printNode(root_.get(), 0);
    std::cout << "====================================\n\n";
}

} // namespace blockdedup
