#pragma once

#include <filesystem>
#include <string>
#include <system_error>

namespace blockdedup {

struct OutputPaths {
    std::filesystem::path archive;
    std::filesystem::path restored;
    // True when a resolved output path is the photo folder. Callers must not delete it.
    bool clashesWithSource = false;
};

inline bool samePath(const std::filesystem::path& a, const std::filesystem::path& b) {
    namespace fs = std::filesystem;
    std::error_code errorA;
    std::error_code errorB;
    fs::path left = fs::weakly_canonical(a, errorA);
    fs::path right = fs::weakly_canonical(b, errorB);
    if (errorA) left = fs::absolute(a).lexically_normal();
    if (errorB) right = fs::absolute(b).lexically_normal();
    return left == right;
}

inline std::filesystem::path normalizedFolder(const std::filesystem::path& input) {
    std::filesystem::path folder = input.lexically_normal();
    if (folder.filename().empty()) folder = folder.parent_path();
    return folder;
}

// Sibling folders next to the photo folder: <name>_archive and <name>_restored.
inline OutputPaths outputPathsFor(const std::filesystem::path& input) {
    namespace fs = std::filesystem;
    fs::path folder = normalizedFolder(input);
    fs::path parent = folder.parent_path();
    if (parent.empty()) parent = ".";

    const std::string name = folder.filename().string();
    OutputPaths out;
    out.archive = parent / (name + "_archive");
    out.restored = parent / (name + "_restored");
    out.clashesWithSource = name.empty()
        || samePath(out.archive, folder)
        || samePath(out.restored, folder);
    return out;
}

// trip_archive is rebuilt into trip. Any other folder name is rebuilt into <name>_restored.
inline std::filesystem::path restoreDestinationFor(const std::filesystem::path& archiveInput) {
    namespace fs = std::filesystem;
    fs::path archive = normalizedFolder(archiveInput);
    fs::path parent = archive.parent_path();
    if (parent.empty()) parent = ".";

    const std::string name = archive.filename().string();
    const std::string suffix = "_archive";
    if (name.size() > suffix.size()
        && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
        return parent / name.substr(0, name.size() - suffix.size());
    }
    return parent / (name + "_restored");
}

} // namespace blockdedup
