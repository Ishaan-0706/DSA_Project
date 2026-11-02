#pragma once

#include "blockdedup/metadata.hpp"
#include <vector>
#include <memory>
#include <string>
#include <functional>

namespace blockdedup {

// Tree Node Types
enum class NodeType {
    ROOT,
    SPATIAL_CLUSTER,
    TEMPORAL_SESSION,
    LEAF
};

struct TreeNode {
    NodeType type;
    std::string label;
    GpsCoordinates centerGps;
    int64_t minTimestamp = 0;
    int64_t maxTimestamp = 0;

    std::vector<ImageMetadata> images;
    std::vector<std::unique_ptr<TreeNode>> children;

    TreeNode(NodeType t, const std::string& l) : type(t), label(l) {}
};

struct ImageCluster {
    std::string folderName;
    std::string description;
    std::vector<ImageMetadata> images;
};

class SpatiotemporalClusterTree {
public:
    // Configuration thresholds:
    // maxSpatialDistanceMeters: images within this distance (e.g., 50 meters) belong to same location cluster
    // maxTemporalGapSeconds: consecutive images taken within this time gap (e.g., 300s = 5 mins) belong to same session
    SpatiotemporalClusterTree(double maxSpatialDistanceMeters = 50.0, int64_t maxTemporalGapSeconds = 300);

    // Insert an image metadata into the tree
    void insert(const ImageMetadata& meta);

    // Insert a batch of image metadata
    void build(const std::vector<ImageMetadata>& metadataList);

    // Retrieve all candidate clusters
    std::vector<std::vector<ImageMetadata>> getCandidateClusters() const;

    // Retrieve named clusters with dedicated folder names for multi-type archiving
    std::vector<ImageCluster> getNamedClusters() const;

    // Print ASCII tree visualization
    void printTree() const;

    const TreeNode* getRoot() const { return root_.get(); }

private:
    double maxDistanceMeters_;
    int64_t maxTemporalGapSeconds_;
    std::unique_ptr<TreeNode> root_;

    void insertGpsImage(TreeNode* spatialRoot, const ImageMetadata& meta);
    void insertNonGpsImage(TreeNode* nonGpsRoot, const ImageMetadata& meta);
    void collectLeaves(const TreeNode* node, std::vector<std::vector<ImageMetadata>>& clusters) const;
    void printNode(const TreeNode* node, int indent) const;
};

} // namespace blockdedup
