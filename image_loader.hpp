/**
 * image_loader.hpp
 * Turns a file on disk into print-ready raster pages.
 *
 *   Images (PNG, JPG, BMP, GIF, TGA, PSD, PPM/PGM) are decoded with
 *   stb_image (bundled, public domain, no install needed).
 *
 *   PDF files are rendered page-by-page with `pdftoppm` (Ubuntu package
 *   poppler-utils, usually pre-installed).
 *
 * Every page is scaled to fit the printable area of an A4 sheet at the
 * job's DPI, keeping its aspect ratio ("fit to page").
 */

#pragma once

#include "epson_l130_driver.hpp"
#include "driver_errors.hpp"

#include <string>
#include <vector>

namespace epson_l130 {

struct RasterPage {
    std::vector<uint8_t> pixels;   // RGB (3 B/px) or grey (1 B/px), per colorMode
    uint32_t             width  = 0;
    uint32_t             height = 0;
};

/**
 * Load a file and return one RasterPage per printed page.
 *   firstPage/lastPage select a page range for PDFs (1-based, 0 = no limit);
 *   they are ignored for image files, which always yield one page.
 * Pixels are RGB if settings.colorMode == Color, greyscale otherwise.
 * Throws FileLoadError with a readable reason on any failure.
 */
std::vector<RasterPage> loadPrintableFile(const std::string&   path,
                                          const PrintSettings& settings,
                                          uint32_t             firstPage = 0,
                                          uint32_t             lastPage  = 0);

/**
 * Clean up a path as typed or dragged into the terminal (quotes, "\ ",
 * leading ~/) and check it is a readable file. Returns the cleaned path;
 * throws FileLoadError if it can't be used.
 */
std::string resolvePrintablePath(const std::string& rawPath);

/** True if the path ends in .pdf (case-insensitive). */
bool isPdfPath(const std::string& path);

/** zlib-compress a buffer (used for FlateDecode streams in exported PDFs). */
std::vector<uint8_t> zlibCompress(const uint8_t* data, size_t len);

} // namespace epson_l130
