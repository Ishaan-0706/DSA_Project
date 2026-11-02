# BlockDedup Image Archival System

A C++20 similarity-aware image archival and deduplication tool that groups similar photos using EXIF telemetry/trees and compresses near-duplicates as compact master/delta representations.

---

## How to Build

Run the build script or use CMake:

```bash
# Using build script
bash sample/build.sh

# Or standard CMake
cmake -S . -B build -DBUILD_TESTING=OFF
cmake --build build
```

Requirements: C++20 compiler (`g++`/`clang++`), `cmake`, `libexiv2-dev`, `zlib1g-dev`, OpenMP.

---

## How to Use

The main executable is `./build/image_dedup_cli`.

### 1. Inspect Image Metadata
Inspect EXIF/XMP tags, dimensions, exposure, and GPS telemetry:
```bash
./build/image_dedup_cli inspect sample/dataset/IMG_0011.JPG
```

### 2. View Spatiotemporal Cluster Tree
View how images are grouped by location, device, and photo session:
```bash
./build/image_dedup_cli tree sample/dataset
```

### 3. Scan Visual Groups
Scan candidate clusters to identify master images and potential deltas:
```bash
./build/image_dedup_cli scan sample/dataset
```

### 4. Archive & Compress Image Directory
Sort images into category-specific folders and compress similar frames into Master + `.bdelta` files:
```bash
./build/image_dedup_cli compress <input_dir> <output_archive_dir> [tolerance]

# Example:
./build/image_dedup_cli compress sample/dataset sample/archive_dataset 28
```
*(Default tolerance is `28`. Tolerance controls noise threshold before a pixel is marked as foreground).*

### 5. Decompress / Reconstruct Image
Reconstruct any original image on-demand from its Master and `.bdelta` file:
```bash
./build/image_dedup_cli decompress <master_path> <delta_path> <output_image_path>

# Example:
./build/image_dedup_cli decompress \
  sample/archive_dataset/cluster_1_Moon_Series_3072x4096_Session_Filesystem_Time/master_aligned_010.jpg \
  sample/archive_dataset/cluster_1_Moon_Series_3072x4096_Session_Filesystem_Time/aligned_001.jpg.bdelta \
  reconstructed_001.jpg
```

---

## What is Currently Implemented

* **EXIF & Telemetry Parser (`src/metadata.cpp`)**: Extracts EXIF/XMP tags, converts rational GPS degrees/minutes/seconds to decimal coordinates, computes Haversine distances, and parses timestamps.
* **Camera RAW & Embedded Preview Support (`src/image_similarity.cpp`)**: Transparently decodes JPEGs, PNGs, and extracts full-resolution previews from camera RAW files (`.CR2`, `.NEF`, `.ARW`).
* **Spatiotemporal Cluster Tree (`src/image_tree.cpp`)**: Hierarchical tree that automatically partitions collections by GPS radius ($\le 50\text{m}$), device/format type, and time window ($\le 300\text{s}$).
* **Visual Scanner (`src/image_similarity.cpp`)**: Multi-threaded perceptual scanner using dHash and background pixel overlap to verify scene sharing and elect Master frames.
* **Optimized Delta Compressor (`src/delta_compressor.cpp`)**:
  * Global exposure compensation alignment across frames.
  * Dark-sky / black-background ISO noise suppression.
  * Signed residual delta encoding centered at 128 under zlib Level 9 (achieving **>85% space savings** on similar frames like astrophotography/bursts).
  * Safe archival fallback (never keeps an expanded delta if larger than the original file).
  * High fidelity decoder with verified reconstruction quality (>35 dB PSNR).
