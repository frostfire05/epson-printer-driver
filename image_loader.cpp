/**
 * image_loader.cpp — see image_loader.hpp
 */

#include "image_loader.hpp"

// ── Bundled third-party single-header libraries (public domain) ─────────────
// Compiled here only; their own warnings are silenced so they don't drown
// out warnings from our code.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "stb_image_write.h"
#pragma GCC diagnostic pop

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>

#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace fs = std::filesystem;

namespace epson_l130 {

namespace {

// ─────────────────────────────────────────────────────────────────────────
// Scaling: fit (w,h) into (maxW,maxH) keeping the aspect ratio.
// Downscaling averages each source block (no aliasing); upscaling is
// bilinear. Input and output are RGB.
// ─────────────────────────────────────────────────────────────────────────

std::vector<uint8_t> resizeRgb(const uint8_t* src, uint32_t sw, uint32_t sh,
                               uint32_t dw, uint32_t dh) {
    std::vector<uint8_t> dst(static_cast<size_t>(dw) * dh * 3);
    const double sx = static_cast<double>(sw) / dw;
    const double sy = static_cast<double>(sh) / dh;

    if (sx >= 1.0 && sy >= 1.0) {
        // Area average
        for (uint32_t y = 0; y < dh; ++y) {
            uint32_t y0 = static_cast<uint32_t>(y * sy);
            uint32_t y1 = std::max(y0 + 1, std::min(sh, static_cast<uint32_t>((y + 1) * sy)));
            for (uint32_t x = 0; x < dw; ++x) {
                uint32_t x0 = static_cast<uint32_t>(x * sx);
                uint32_t x1 = std::max(x0 + 1, std::min(sw, static_cast<uint32_t>((x + 1) * sx)));
                uint64_t acc[3] = {0, 0, 0};
                for (uint32_t yy = y0; yy < y1; ++yy) {
                    const uint8_t* row = src + (static_cast<size_t>(yy) * sw + x0) * 3;
                    for (uint32_t xx = x0; xx < x1; ++xx, row += 3) {
                        acc[0] += row[0]; acc[1] += row[1]; acc[2] += row[2];
                    }
                }
                uint64_t n = static_cast<uint64_t>(y1 - y0) * (x1 - x0);
                uint8_t* d = &dst[(static_cast<size_t>(y) * dw + x) * 3];
                d[0] = static_cast<uint8_t>(acc[0] / n);
                d[1] = static_cast<uint8_t>(acc[1] / n);
                d[2] = static_cast<uint8_t>(acc[2] / n);
            }
        }
    } else {
        // Bilinear
        for (uint32_t y = 0; y < dh; ++y) {
            double fy = std::max(0.0, (y + 0.5) * sy - 0.5);
            uint32_t y0 = std::min(sh - 1, static_cast<uint32_t>(fy));
            uint32_t y1 = std::min(sh - 1, y0 + 1);
            double ty = fy - y0;
            for (uint32_t x = 0; x < dw; ++x) {
                double fx = std::max(0.0, (x + 0.5) * sx - 0.5);
                uint32_t x0 = std::min(sw - 1, static_cast<uint32_t>(fx));
                uint32_t x1 = std::min(sw - 1, x0 + 1);
                double tx = fx - x0;
                for (int c = 0; c < 3; ++c) {
                    double a = src[(static_cast<size_t>(y0) * sw + x0) * 3 + c];
                    double b = src[(static_cast<size_t>(y0) * sw + x1) * 3 + c];
                    double e = src[(static_cast<size_t>(y1) * sw + x0) * 3 + c];
                    double f = src[(static_cast<size_t>(y1) * sw + x1) * 3 + c];
                    double v = (a * (1 - tx) + b * tx) * (1 - ty) + (e * (1 - tx) + f * tx) * ty;
                    dst[(static_cast<size_t>(y) * dw + x) * 3 + c] =
                        static_cast<uint8_t>(std::lround(std::min(255.0, std::max(0.0, v))));
                }
            }
        }
    }
    return dst;
}

// Fit an RGB image to the printable area and convert to the job's colour mode.
RasterPage toPrintPage(const uint8_t* rgb, uint32_t w, uint32_t h,
                       const PrintSettings& settings) {
    EscP2Encoder enc(settings);
    const uint32_t maxW = enc.dotsPerLineX();
    const uint32_t maxH = enc.dotsPerLineY();

    double scale = std::min(static_cast<double>(maxW) / w,
                            static_cast<double>(maxH) / h);
    uint32_t dw = std::max<uint32_t>(1, static_cast<uint32_t>(w * scale));
    uint32_t dh = std::max<uint32_t>(1, static_cast<uint32_t>(h * scale));
    dw = std::min(dw, maxW);
    dh = std::min(dh, maxH);

    RasterPage page;
    page.width  = dw;
    page.height = dh;
    std::vector<uint8_t> fitted = (dw == w && dh == h)
        ? std::vector<uint8_t>(rgb, rgb + static_cast<size_t>(w) * h * 3)
        : resizeRgb(rgb, w, h, dw, dh);

    if (settings.colorMode == ColorMode::Grayscale) {
        page.pixels.resize(static_cast<size_t>(dw) * dh);
        for (size_t i = 0; i < page.pixels.size(); ++i) {
            const uint8_t* p = &fitted[i * 3];
            page.pixels[i] = static_cast<uint8_t>((p[0] * 299 + p[1] * 587 + p[2] * 114) / 1000);
        }
    } else {
        page.pixels = std::move(fitted);
    }
    return page;
}

RasterPage loadImage(const std::string& path, const PrintSettings& settings) {
    int w = 0, h = 0, n = 0;
    std::unique_ptr<uint8_t, void (*)(void*)> data(
        stbi_load(path.c_str(), &w, &h, &n, 3), stbi_image_free);
    if (!data) {
        throw FileLoadError(path,
            std::string("not a supported image (") + stbi_failure_reason() +
            "). Supported: PNG, JPG, BMP, GIF, TGA, PSD, PPM/PGM, PDF.");
    }
    return toPrintPage(data.get(), static_cast<uint32_t>(w),
                       static_cast<uint32_t>(h), settings);
}

// Run a program with arguments (no shell → file names with spaces or quotes
// are safe). Returns the exit status; throws if the program can't start.
int runProgram(const std::vector<std::string>& args, const std::string& forPath) {
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, "/dev/null", O_WRONLY, 0);

    pid_t pid = 0;
    int rc = posix_spawnp(&pid, argv[0], &fa, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);

    if (rc == ENOENT)
        throw FileLoadError(forPath,
            "PDF support needs the 'pdftoppm' tool. Install it with:\n"
            "    sudo apt install poppler-utils");
    if (rc != 0)
        throw FileLoadError(forPath, std::string("could not start pdftoppm: ") + std::strerror(rc));

    int status = 0;
    if (waitpid(pid, &status, 0) < 0)
        throw FileLoadError(forPath, "pdftoppm did not finish");
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

std::vector<RasterPage> loadPdf(const std::string& path, const PrintSettings& settings,
                                uint32_t firstPage, uint32_t lastPage) {
    // Private temp directory, removed when we're done (even on error)
    std::string tmpl = (fs::temp_directory_path() / "epson_pdf_XXXXXX").string();
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    if (!mkdtemp(buf.data()))
        throw FileLoadError(path, std::string("cannot create temp folder: ") + std::strerror(errno));
    fs::path tmpDir(buf.data());
    struct Cleanup { fs::path p; ~Cleanup() { std::error_code ec; fs::remove_all(p, ec); } } cleanup{tmpDir};

    // Render at the job's DPI so the page prints at its real size
    std::vector<std::string> args = {
        "pdftoppm", "-r", std::to_string(settings.dpiX), "-png"
    };
    if (firstPage) { args.push_back("-f"); args.push_back(std::to_string(firstPage)); }
    if (lastPage)  { args.push_back("-l"); args.push_back(std::to_string(lastPage)); }
    args.push_back(path);
    args.push_back((tmpDir / "page").string());

    int exitCode = runProgram(args, path);
    if (exitCode != 0)
        throw FileLoadError(path,
            "pdftoppm could not read this PDF (exit code " + std::to_string(exitCode) +
            "). It may be damaged or password-protected.");

    // pdftoppm zero-pads page numbers, so name order == page order
    std::vector<fs::path> pngs;
    for (auto& e : fs::directory_iterator(tmpDir))
        if (e.path().extension() == ".png") pngs.push_back(e.path());
    std::sort(pngs.begin(), pngs.end());

    if (pngs.empty())
        throw FileLoadError(path, "no pages in the selected range");

    std::vector<RasterPage> pages;
    for (auto& p : pngs) pages.push_back(loadImage(p.string(), settings));
    return pages;
}

} // anonymous namespace

// ═══════════════════════════════════════════════════════════════════════════
// Public API
// ═══════════════════════════════════════════════════════════════════════════

bool isPdfPath(const std::string& path) {
    if (path.size() < 4) return false;
    std::string ext = path.substr(path.size() - 4);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".pdf";
}

std::string resolvePrintablePath(const std::string& rawPath) {
    std::string path = rawPath;
    // Strip quotes added when a file is dragged into the terminal
    if (path.size() >= 2 && (path.front() == '\'' || path.front() == '"') &&
        path.back() == path.front())
        path = path.substr(1, path.size() - 2);
    // Undo backslash-escaped spaces ("my\ photos") some terminals produce
    std::string unescaped;
    for (size_t i = 0; i < path.size(); ++i) {
        if (path[i] == '\\' && i + 1 < path.size()) ++i;
        unescaped += path[i];
    }
    path = unescaped;
    // Accept "~/..." like the shell does
    if (path.rfind("~/", 0) == 0) {
        if (const char* home = std::getenv("HOME")) path = std::string(home) + path.substr(1);
    }

    std::error_code ec;
    if (path.empty())
        throw FileLoadError(rawPath, "no file path given");
    if (!fs::exists(path, ec))
        throw FileLoadError(path, "file does not exist (check the path and spelling)");
    if (fs::is_directory(path, ec))
        throw FileLoadError(path, "that is a folder, not a file");
    if (access(path.c_str(), R_OK) != 0)
        throw FileLoadError(path, "no permission to read this file");
    return path;
}

std::vector<RasterPage> loadPrintableFile(const std::string&   rawPath,
                                          const PrintSettings& settings,
                                          uint32_t             firstPage,
                                          uint32_t             lastPage) {
    const std::string path = resolvePrintablePath(rawPath);
    if (firstPage && lastPage && firstPage > lastPage)
        throw FileLoadError(path, "first page is after last page");

    if (isPdfPath(path))
        return loadPdf(path, settings, firstPage, lastPage);
    return { loadImage(path, settings) };
}

std::vector<uint8_t> zlibCompress(const uint8_t* data, size_t len) {
    int outLen = 0;
    unsigned char* out = stbi_zlib_compress(const_cast<unsigned char*>(data),
                                            static_cast<int>(len), &outLen, 6);
    if (!out) throw PdfWriteError("compression failed (out of memory?)");
    std::vector<uint8_t> result(out, out + outLen);
    STBIW_FREE(out);
    return result;
}

} // namespace epson_l130
