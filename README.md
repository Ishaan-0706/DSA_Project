# BlockDedup

A C++20 tool that finds near-duplicate photos and stores them as small deltas against a master image, so an archive takes less disk space. Every image can be rebuilt from its master plus its delta.

## How it works

1. **Group by place and time.** Photos taken close together (within 50 m and 300 s, or from the same device) are grouped using their EXIF/GPS data. If EXIF is missing, file times are used.
2. **Find similar photos.** Inside each group, a perceptual hash (dHash) and a background pixel comparison find photos of the same scene. The largest image in each set becomes the master.
3. **Store deltas.** Each similar photo is aligned to its master (exposure, shift, rotation). The difference is compressed with zlib and saved as a `.bdelta` file.
4. **Stay safe.** If a delta is not smaller than the original photo, the photo is stored as a normal file.

## Requirements

- GCC 11+ or Clang 13+ (C++20), CMake 3.20+, zlib, OpenMP
- libexiv2 (optional, for richer EXIF and GPS data)
- Python 3 with Pillow (only to make the test dataset)

```sh
# Fedora
sudo dnf install -y cmake gcc-c++ zlib-devel libgomp exiv2 exiv2-devel python3-pillow

# Ubuntu / Debian
sudo apt install -y cmake g++ zlib1g-dev libomp-dev libexiv2-dev exiv2 python3-pil
```

## Build

```sh
cmake -S . -B build
cmake --build build -j$(nproc)
```

The program is built at `./build/image_dedup_cli`. Add `-DBUILD_TESTING=OFF` to the first command to skip the unit tests.

## Commands

| Command | What it does |
|---|---|
| `inspect <image>` | Show EXIF, GPS and camera data for one image |
| `tree <image_dir>` | Show the place-and-time groups |
| `scan <image_dir>` | Show which images are masters and which are deltas |
| `compress <image_dir> <out_dir> [tolerance]` | Write the archive. `tolerance` is the pixel noise limit (default 28) |
| `decompress <master> <delta> <out_image>` | Rebuild one image |
| `restore <archive_dir> <out_dir>` | Rebuild every image in an archive |
| `demo [image_dir]` | Run inspect, tree, scan, compress and restore in one go |
| `(no command)` or `tui` | Archive a folder, or restore an archive later |

## Interactive

Run the program with no arguments and choose archive or restore.

```sh
./build/image_dedup_cli
```

**Archive.** Give a photo folder. An archive is the smaller copy written next to it, named `<folder>_archive`. One full photo is kept for each group of similar shots. The other shots are stored as differences from that photo. After the archive holds every photo, the original folder is removed. No restored folder is written.

**Restore.** Choose restore and give the archive folder. `trip_archive` is rebuilt into `trip`, next to the archive. After every file is back, the archive folder is removed.

## Try it

**1. Make a test dataset.** This downloads 20 photos, then makes variants of each one (resized, cropped, brighter and recompressed).

```sh
mkdir -p masters
for i in $(seq 10 29); do
  curl -sL "https://picsum.photos/id/${i}/1200/800" -o "masters/photo_${i}.jpg"
done
python3 make_dataset.py
```

**2. Run the pipeline.**

```sh
./build/image_dedup_cli scan dataset                            # see the groups
./build/image_dedup_cli compress dataset archive_dataset 28     # build the archive
./build/image_dedup_cli restore archive_dataset restored_dataset
```

**3. Check the result.**

```sh
du -sh dataset archive_dataset      # compare sizes
ls restored_dataset | wc -l         # all images should be back
```

To rebuild a single image, pass the master and its delta file from the archive:

```sh
./build/image_dedup_cli decompress \
  archive_dataset/<cluster>/master_<name>.png \
  archive_dataset/<cluster>/master_<name>.png@@<image>.bdelta \
  rebuilt.png
```

## Tests

```sh
ctest --test-dir build --output-on-failure
```

Tests use [Catch2](https://github.com/catchorg/Catch2), which CMake downloads for you.

## License

MIT. See [LICENSE](LICENSE).