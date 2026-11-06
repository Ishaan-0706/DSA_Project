#include <iostream>
#include <string>
#include <iomanip>
#include <exiv2/exiv2.hpp>

void processImageMetadata(const std::string& filepath) {
    try {
        // Open the image file (automatically detects format: JPEG, PNG, RAW, etc.)
        Exiv2::Image::UniquePtr image = Exiv2::ImageFactory::open(filepath);
        if (!image.get()) {
            std::cerr << "Failed to open image file: " << filepath << "\n";
            return;
        }

        // Read metadata from the file
        image->readMetadata();

        // Basic Image Attributes
        std::cout << "========================================\n";
        std::cout << "File:       " << filepath << "\n";
        std::cout << "MIME Type:  " << image->mimeType() << "\n";
        std::cout << "Dimensions: " << image->pixelWidth() << " x " << image->pixelHeight() << " px\n";
        std::cout << "========================================\n\n";

        // 1. Process EXIF Metadata
        Exiv2::ExifData& exifData = image->exifData();
        if (exifData.empty()) {
            std::cout << "No EXIF metadata found.\n";
        } else {
            std::cout << "--- Key EXIF Metadata ---\n";
            
            // Common EXIF keys of interest (valid tag names)
            const std::string targetKeys[] = {
                "Exif.Image.Make",
                "Exif.Image.Model",
                "Exif.Photo.DateTimeOriginal",
                "Exif.Photo.ExposureTime",
                "Exif.Photo.FNumber",
                "Exif.Photo.ISOSpeedRatings",
                "Exif.GPSInfo.GPSLatitudeRef",
                "Exif.GPSInfo.GPSLatitude",
                "Exif.GPSInfo.GPSLongitudeRef",
                "Exif.GPSInfo.GPSLongitude"
            };

            for (const auto& key : targetKeys) {
                try {
                    Exiv2::ExifKey exifKey(key);
                    auto pos = exifData.findKey(exifKey);
                    if (pos != exifData.end()) {
                        std::cout << std::left << std::setw(32) << pos->key() 
                                  << " : " << pos->print(&exifData) << "\n";
                    }
                } catch (const Exiv2::Error& e) {
                    std::cerr << "Warning for key '" << key << "': " << e.what() << "\n";
                }
            }

            std::cout << "\nTotal EXIF Tags: " << exifData.count() << "\n";
        }

        // 2. Process XMP Metadata (Common in PNG and edited RAW files)
        Exiv2::XmpData& xmpData = image->xmpData();
        if (!xmpData.empty()) {
            std::cout << "\n--- XMP Metadata Summary ---\n";
            std::cout << "Total XMP Tags: " << xmpData.count() << "\n";
            
            // Print first few XMP keys as sample
            size_t count = 0;
            for (auto it = xmpData.begin(); it != xmpData.end() && count < 5; ++it, ++count) {
                std::cout << std::left << std::setw(32) << it->key() 
                          << " : " << it->value().toString() << "\n";
            }
        }

    } catch (const Exiv2::Error& e) {
        std::cerr << "Exiv2 Error (" << static_cast<int>(e.code()) << "): " << e.what() << "\n";
    } catch (const std::exception& e) {
        std::cerr << "Standard Exception: " << e.what() << "\n";
    }
}

int main(int argc, char* argv[]) {
    std::string file = (argc > 1) ? argv[1] : "sample/IMG_0003.JPG";
    processImageMetadata(file);
    return 0;
}
