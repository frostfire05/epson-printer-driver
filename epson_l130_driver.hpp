/**
 * EPSON L130 Printer Driver
 * A C++ driver implementation targeting Linux (CUPS/USB) for the EPSON L130
 * inkjet printer. Implements ESC/P2 raster command set.
 *
 * Architecture:
 *   EpsonL130Driver          - Top-level public API
 *   UsbTransport             - USB bulk-transfer layer (libusb-1.0)
 *   EscP2Encoder             - Converts raw raster data to ESC/P2 commands
 *   PrintJob                 - Holds per-job settings and page data
 *   ColorProfile             - Basic CMY(K) colour handling
 */

#pragma once

#include "driver_errors.hpp"

#include <array>
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <stdexcept>

// ── Forward declarations ─────────────────────────────────────────────────────
struct libusb_device_handle;
struct libusb_context;

namespace epson_l130 {

// ═══════════════════════════════════════════════════════════════════════════
// Constants – EPSON L130 hardware specifications
// ═══════════════════════════════════════════════════════════════════════════

constexpr uint16_t USB_VENDOR_ID   = 0x04B8;   // Seiko Epson Corp.
constexpr uint16_t USB_PRODUCT_ID_L130  = 0x007C;   // EPSON L130 Series (standard hardware PID)
constexpr uint16_t USB_PRODUCT_ID_ALT   = 0x08A1;   // L210 / L350 / L130 variants
constexpr uint16_t USB_PRODUCT_ID       = USB_PRODUCT_ID_L130; // Default PID

inline bool isSupportedUsbDevice(uint16_t vendorId, uint16_t productId) {
    return vendorId == USB_VENDOR_ID &&
           (productId == USB_PRODUCT_ID_L130 || productId == USB_PRODUCT_ID_ALT);
}

// USB endpoints (bulk)
constexpr uint8_t EP_OUT = 0x01;   // host → printer
constexpr uint8_t EP_IN  = 0x82;   // printer → host (status)

constexpr int USB_TIMEOUT_MS = 5000;

// Printable area at 720 dpi (A4 media)
constexpr uint32_t MAX_DPI_X = 720;
constexpr uint32_t MAX_DPI_Y = 720;

// Supported resolutions
constexpr uint32_t DPI_DRAFT  = 180;
constexpr uint32_t DPI_NORMAL = 360;
constexpr uint32_t DPI_HIGH   = 720;

// Page dimensions in mm (A4)
constexpr float PAGE_WIDTH_MM  = 210.0f;
constexpr float PAGE_HEIGHT_MM = 297.0f;

// Margins in dots at 720 dpi
constexpr uint32_t MARGIN_LEFT_DOTS   = 85;
constexpr uint32_t MARGIN_RIGHT_DOTS  = 85;
constexpr uint32_t MARGIN_TOP_DOTS    = 120;
constexpr uint32_t MARGIN_BOTTOM_DOTS = 120;

// Maximum nozzle counts per colour
constexpr uint8_t NOZZLES_PER_COLOR = 59;
constexpr uint8_t COLOR_CHANNELS    = 4;  // K, C, M, Y

// ESC/P2 command bytes
constexpr uint8_t ESC = 0x1B;
constexpr uint8_t GS  = 0x1D;

// ═══════════════════════════════════════════════════════════════════════════
// Enumerations
// ═══════════════════════════════════════════════════════════════════════════

enum class MediaType : uint8_t {
    Plain         = 0,
    Matte         = 1,
    Glossy        = 2,
    Transparency  = 3,
    Envelope      = 4,
};

enum class PrintQuality : uint8_t {
    Draft  = 0,
    Normal = 1,
    High   = 2,
    Photo  = 3,
};

enum class ColorMode : uint8_t {
    Grayscale = 0,
    Color     = 1,
};

enum class PaperSize : uint8_t {
    A4     = 0,
    Letter = 1,
    Legal  = 2,
    B5     = 3,
    Custom = 15,
};

// ═══════════════════════════════════════════════════════════════════════════
// Data structures
// ═══════════════════════════════════════════════════════════════════════════

struct PrintSettings {
    PaperSize    paperSize    = PaperSize::A4;
    MediaType    mediaType    = MediaType::Plain;
    PrintQuality quality      = PrintQuality::Normal;
    ColorMode    colorMode    = ColorMode::Color;
    uint32_t     dpiX         = DPI_NORMAL;
    uint32_t     dpiY         = DPI_NORMAL;
    uint32_t     copies       = 1;
    bool         borderless   = false;
    bool         mirrorX      = false;   // iron-on transfers etc.
    uint16_t     customWidthMm  = 0;
    uint16_t     customHeightMm = 0;
};

// One scan line of raw pixel data (RGB or Grayscale, 8 bpp per channel)
struct ScanLine {
    std::vector<uint8_t> pixels; // width * 3 bytes for RGB, width * 1 for grey
    uint32_t             width   = 0;
    bool                 isRGB   = true;
};

// Progress callback: (current_line, total_lines)
using ProgressCallback = std::function<void(uint32_t, uint32_t)>;

// Exceptions: the full typed hierarchy lives in driver_errors.hpp
// (included at the top of this file). Do not define exceptions here.

// ═══════════════════════════════════════════════════════════════════════════
// USB transport layer
// ═══════════════════════════════════════════════════════════════════════════

class UsbTransport {
public:
    UsbTransport();
    ~UsbTransport();

    // Non-copyable
    UsbTransport(const UsbTransport&)            = delete;
    UsbTransport& operator=(const UsbTransport&) = delete;

    void open();
    void close();
    bool isOpen() const;

    // Bulk write to printer; throws UsbException on failure
    void write(const uint8_t* data, size_t length);
    void write(const std::vector<uint8_t>& data);

    // Bulk read printer status (returns bytes actually read)
    size_t read(uint8_t* buf, size_t maxLength);

    // Convenience: read IEEE 1284 device ID string
    std::string readDeviceId();

    // Read ESC/P2 printer status packet (ST2 format)
    std::vector<uint8_t> readStatusPacket();

    // Query detected product ID
    uint16_t getProductId() const { return productId_; }

private:
    libusb_context*       ctx_    = nullptr;
    libusb_device_handle* handle_ = nullptr;
    int                   interface_ = 0;
    bool                  kernelDetached_ = false;
    uint8_t               epOut_          = EP_OUT;
    uint8_t               epIn_           = EP_IN;
    uint16_t              productId_      = 0;

    void claimInterface();
    void releaseInterface();
};

// ═══════════════════════════════════════════════════════════════════════════
// ESC/P2 command encoder
// Translates high-level print operations into raw byte streams.
// ═══════════════════════════════════════════════════════════════════════════

class EscP2Encoder {
public:
    explicit EscP2Encoder(const PrintSettings& settings);

    // Job-level headers/footers
    std::vector<uint8_t> buildJobHeader() const;
    std::vector<uint8_t> buildPageHeader(uint32_t pageIndex) const;
    std::vector<uint8_t> buildPageFooter()  const;
    std::vector<uint8_t> buildJobFooter()   const;

    // Encode one raster scan line into ESC/P2 raster data command.
    // channels: 1 = K only, 4 = C M Y K
    // Returns encoded bytes ready to transmit.
    std::vector<uint8_t> encodeRasterLine(
        const uint8_t* pixels,
        uint32_t       pixelCount,
        uint8_t        colorChannel,   // 0=K 1=C 2=M 3=Y
        bool           compress = true
    ) const;

    // Vertical paper advance command (dots)
    std::vector<uint8_t> buildVerticalAdvance(uint32_t dots) const;

    // Form-feed / eject page
    std::vector<uint8_t> buildFormFeed() const;

    // Colour channel order the L130 expects
    static constexpr uint8_t CHANNEL_ORDER[4] = { 0, 1, 2, 3 }; // K C M Y

    uint32_t dotsPerLineX() const;
    uint32_t dotsPerLineY() const;

private:
    PrintSettings settings_;

    // Pack 8-bpp greyscale intensity into 1-bpp dot pattern via threshold
    static std::vector<uint8_t> threshold1bpp(const uint8_t* grey, uint32_t count);

    // EPSON PackBits compression (run-length)
    static std::vector<uint8_t> packBitsCompress(const uint8_t* data, size_t len);

    // Resolution code used in ESC/P2 headers
    uint8_t dpiCode(uint32_t dpi) const;

    // Media type code
    uint8_t mediaCode() const;

    // Quality code
    uint8_t qualityCode() const;
};

// ═══════════════════════════════════════════════════════════════════════════
// Colour profile – RGB → CMY(K) separation
// ═══════════════════════════════════════════════════════════════════════════

class ColorProfile {
public:
    ColorProfile() = default;

    // Convert an RGB triple to CMYK (0–255 each)
    static void rgbToCmyk(uint8_t r, uint8_t g, uint8_t b,
                          uint8_t& c, uint8_t& m, uint8_t& y, uint8_t& k);

    // Convert RGB scan line to 4-plane (C M Y K) scan lines.
    // Output planes must each be pre-allocated to pixelCount bytes.
    static void separateLine(const uint8_t* rgb, uint32_t pixelCount,
                             uint8_t* planeC, uint8_t* planeM,
                             uint8_t* planeY, uint8_t* planeK);

    // Error-diffusion (Floyd-Steinberg) dither a greyscale plane in place
    static void ditherFloydSteinberg(uint8_t* plane,
                                     uint32_t width, uint32_t height);
};

// ═══════════════════════════════════════════════════════════════════════════
// PrintJob – manages a single print job lifecycle
// ═══════════════════════════════════════════════════════════════════════════

class PrintJob {
public:
    explicit PrintJob(const PrintSettings& settings);

    // Add a raster page.  pixels = packed RGB or Grey rows, top-to-bottom.
    // width / height in pixels at the job's DPI.
    void addPage(const uint8_t* pixels, uint32_t width, uint32_t height);

    // Access accumulated encoded byte stream
    const std::vector<uint8_t>& encodedData() const;

    // Total encoded bytes
    size_t encodedSize() const;

    uint32_t pageCount() const;

private:
    PrintSettings           settings_;
    EscP2Encoder            encoder_;
    std::vector<uint8_t>    buffer_;
    uint32_t                pageCount_ = 0;

    void append(const std::vector<uint8_t>& bytes);
};

// ═══════════════════════════════════════════════════════════════════════════
// EpsonL130Driver – public API
// ═══════════════════════════════════════════════════════════════════════════

class EpsonL130Driver {
public:
    EpsonL130Driver();
    ~EpsonL130Driver();

    // Non-copyable
    EpsonL130Driver(const EpsonL130Driver&)            = delete;
    EpsonL130Driver& operator=(const EpsonL130Driver&) = delete;

    // ── Connection ─────────────────────────────────────────────────────────

    // Opens USB connection.  Throws UsbException if printer not found.
    void connect();

    // Closes USB connection.
    void disconnect();

    bool isConnected() const;
    uint16_t getProductId() const;

    // Returns the printer's IEEE 1284 device ID string.
    std::string queryDeviceId();

    // Returns human-readable printer status.
    std::string queryStatus();

    // ── Printing ───────────────────────────────────────────────────────────

    // High-level: encode + transmit a full raster image in one call.
    // pixels: packed RGB (3 bytes/px) or Greyscale (1 byte/px) rows.
    // width / height: image dimensions in pixels (must match DPI in settings).
    void printRaster(const uint8_t*       pixels,
                     uint32_t             width,
                     uint32_t             height,
                     const PrintSettings& settings,
                     ProgressCallback     onProgress = nullptr);

    // Lower-level: transmit a pre-built PrintJob.
    void submitJob(const PrintJob& job, ProgressCallback onProgress = nullptr);

    // ── Utility ────────────────────────────────────────────────────────────

    // Send a raw ESC/P2 byte sequence directly (advanced use).
    void sendRaw(const uint8_t* data, size_t length);

    // Perform a hardware self-test (sends ESC/P2 self-test command).
    void selfTest();

    // Reset printer to factory defaults.
    void reset();

    // Ink levels: returns values 0–100 for [K, C, M, Y].
    // Values are best-effort (not all firmware versions expose levels).
    std::array<uint8_t, 4> readInkLevels();

private:
    UsbTransport transport_;

    // Chunk-wise transmission with optional progress reporting
    void transmitBuffer(const std::vector<uint8_t>& buf,
                        ProgressCallback             onProgress,
                        size_t                       chunkSize = 16384);
};

} // namespace epson_l130
