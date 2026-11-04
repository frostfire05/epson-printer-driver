/**
 * EPSON L130 Printer Driver — Implementation
 *
 * Compile with:
 *   g++ -std=c++17 -O2 -Wall -Wextra \
 *       epson_l130_driver.cpp -o epson_l130_driver \
 *       $(pkg-config --cflags --libs libusb-1.0)
 *
 * Requires: libusb-1.0-dev
 *   sudo apt install libusb-1.0-0-dev      (Debian/Ubuntu)
 *   sudo dnf install libusb1-devel         (Fedora)
 */

#include "epson_l130_driver.hpp"

#include <libusb-1.0/libusb.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <chrono>

#include <fstream>
#include <ctime>

namespace epson_l130 {

static std::ofstream g_debugLog;
static std::string g_debugLogPath = "epson_driver_debug.log";

void initDebugLog(const std::string& filename) {
    if (g_debugLog.is_open()) g_debugLog.close();
    g_debugLogPath = filename;
    g_debugLog.open(filename, std::ios::out | std::ios::trunc);
    logDebug("=== EPSON L130 Driver Debug Log Started ===", true);
}

void logDebug(const std::string& msg, bool toConsole) {
    if (!g_debugLog.is_open()) {
        g_debugLog.open(g_debugLogPath, std::ios::out | std::ios::app);
    }
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm;
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char timeBuf[64];
    std::strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S", &tm);
    std::ostringstream oss;
    oss << "[" << timeBuf << "." << std::setfill('0') << std::setw(3) << ms.count() << "] " << msg;
    
    if (g_debugLog.is_open()) {
        g_debugLog << oss.str() << std::endl;
    }
    if (toConsole) {
        std::cout << "[DEBUG] " << msg << std::endl;
    }
}


// ═══════════════════════════════════════════════════════════════════════════
// Internal helpers
// ═══════════════════════════════════════════════════════════════════════════

namespace {

// Append bytes to a vector
void push(std::vector<uint8_t>& v, std::initializer_list<uint8_t> bytes) {
    v.insert(v.end(), bytes.begin(), bytes.end());
}

void pushU16LE(std::vector<uint8_t>& v, uint16_t val) {
    v.push_back(val & 0xFF);
    v.push_back((val >> 8) & 0xFF);
}

[[maybe_unused]] void pushU32LE(std::vector<uint8_t>& v, uint32_t val) {
    v.push_back(val & 0xFF);
    v.push_back((val >> 8) & 0xFF);
    v.push_back((val >> 16) & 0xFF);
    v.push_back((val >> 24) & 0xFF);
}

} // anonymous namespace

// ═══════════════════════════════════════════════════════════════════════════
// UsbTransport
// ═══════════════════════════════════════════════════════════════════════════

UsbTransport::UsbTransport() {
    int r = libusb_init(&ctx_);
    if (r < 0)
        throw UsbException("libusb_init failed", r);
}

UsbTransport::~UsbTransport() {
    close();
    if (ctx_) {
        libusb_exit(ctx_);
        ctx_ = nullptr;
    }
}

void UsbTransport::open() {
    if (handle_) return;   // already open

    // Enumerate manually: libusb_open_device_with_vid_pid() returns NULL for
    // both "not present" and "no permission", so we can't tell them apart.
    libusb_device** list = nullptr;
    ssize_t count = libusb_get_device_list(ctx_, &list);
    if (count < 0)
        throw UsbException("Could not enumerate USB devices", static_cast<int>(count));

    libusb_device* found = nullptr;
    for (ssize_t i = 0; i < count; ++i) {
        libusb_device_descriptor desc{};
        if (libusb_get_device_descriptor(list[i], &desc) == 0 &&
            isSupportedUsbDevice(desc.idVendor, desc.idProduct)) {
            found = list[i];
            productId_ = desc.idProduct;
            break;
        }
    }

    if (found) {
        libusb_config_descriptor* config = nullptr;
        if (libusb_get_active_config_descriptor(found, &config) == 0 ||
            libusb_get_config_descriptor(found, 0, &config) == 0) {
            for (int i = 0; i < config->bNumInterfaces; ++i) {
                const libusb_interface* iface = &config->interface[i];
                for (int a = 0; a < iface->num_altsetting; ++a) {
                    const libusb_interface_descriptor* alt = &iface->altsetting[a];
                    if (alt->bInterfaceClass == LIBUSB_CLASS_PRINTER || config->bNumInterfaces == 1) {
                        interface_ = alt->bInterfaceNumber;
                    }
                    for (int e = 0; e < alt->bNumEndpoints; ++e) {
                        const libusb_endpoint_descriptor* ep = &alt->endpoint[e];
                        if ((ep->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) == LIBUSB_TRANSFER_TYPE_BULK) {
                            if ((ep->bEndpointAddress & LIBUSB_ENDPOINT_IN) != 0) {
                                epIn_ = ep->bEndpointAddress;
                            } else {
                                epOut_ = ep->bEndpointAddress;
                            }
                        }
                    }
                }
            }
            libusb_free_config_descriptor(config);
        }
    }

        int r = found ? libusb_open(found, &handle_) : LIBUSB_ERROR_NO_DEVICE;
    libusb_free_device_list(list, 1);

    if (r == LIBUSB_ERROR_NO_DEVICE || !found) { handle_ = nullptr; throw DeviceNotFound(); }
    if (r == LIBUSB_ERROR_ACCESS)              { handle_ = nullptr; throw PermissionDenied(); }
    if (r < 0)                                 { handle_ = nullptr; throw UsbException("Failed to open printer", r); }

    logDebug("USB device opened successfully. VID=0x04B8 PID=0x" + 
             [&]{ std::ostringstream h; h << std::hex << productId_; return h.str(); }() + 
             " Interface=" + std::to_string(interface_) + 
             " BulkOut=0x" + [&]{ std::ostringstream h; h << std::hex << (int)epOut_; return h.str(); }() + 
             " BulkIn=0x" + [&]{ std::ostringstream h; h << std::hex << (int)epIn_; return h.str(); }());

    try {
        claimInterface();
    } catch (...) {
        libusb_close(handle_);
        handle_ = nullptr;
        throw;
    }
}

void UsbTransport::close() {
    if (!handle_) return;
    releaseInterface();
    libusb_close(handle_);
    handle_ = nullptr;
}

bool UsbTransport::isOpen() const {
    return handle_ != nullptr;
}

void UsbTransport::claimInterface() {
    // Detach any kernel driver (e.g. usblp) so we can claim the interface
    if (libusb_kernel_driver_active(handle_, interface_) == 1) {
        int r = libusb_detach_kernel_driver(handle_, interface_);
        if (r == LIBUSB_ERROR_ACCESS) throw PermissionDenied();
        if (r < 0)                    throw KernelDriverConflict(r);
        kernelDetached_ = true;
    }

    int r = libusb_claim_interface(handle_, interface_);
    if (r == LIBUSB_ERROR_BUSY)   throw DeviceBusy();
    if (r == LIBUSB_ERROR_ACCESS) throw PermissionDenied();
    if (r < 0)
        throw UsbException("Failed to claim USB interface", r);
}

void UsbTransport::releaseInterface() {
    if (!handle_) return;
    libusb_release_interface(handle_, interface_);
    if (kernelDetached_) {
        libusb_attach_kernel_driver(handle_, interface_);
        kernelDetached_ = false;
    }
}

void UsbTransport::write(const uint8_t* data, size_t length) {
    if (!handle_) throw UsbException("USB device not open", 0);

    size_t totalTransferred = 0;
    int consecutiveTimeouts = 0;
    const int maxTimeouts = 20; // Allow printer buffer draining during head movement

    while (totalTransferred < length) {
        int transferred = 0;
        int r = libusb_bulk_transfer(
            handle_,
            epOut_,
            const_cast<uint8_t*>(data + totalTransferred),
            static_cast<int>(length - totalTransferred),
            &transferred,
            USB_TIMEOUT_MS
        );

        if (transferred > 0) {
            totalTransferred += static_cast<size_t>(transferred);
            consecutiveTimeouts = 0;
        }

        if (r == 0) {
            continue;
        }

        if (r == LIBUSB_ERROR_NO_DEVICE) {
            throw PrinterOffline();
        }

        if (r == LIBUSB_ERROR_TIMEOUT) {
            ++consecutiveTimeouts;
            if (consecutiveTimeouts >= maxTimeouts) {
                throw UsbTransferError("write", r, length, totalTransferred);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        throw UsbTransferError("write", r, length, totalTransferred);
    }
}

void UsbTransport::write(const std::vector<uint8_t>& data) {
    write(data.data(), data.size());
}

size_t UsbTransport::read(uint8_t* buf, size_t maxLength) {
    if (!handle_) throw UsbException("USB device not open", 0);

    int transferred = 0;
    int r = libusb_bulk_transfer(
        handle_,
        epIn_,
        buf,
        static_cast<int>(maxLength),
        &transferred,
        USB_TIMEOUT_MS
    );

    if (r == LIBUSB_ERROR_NO_DEVICE)
        throw PrinterOffline();
    if (r < 0 && r != LIBUSB_ERROR_TIMEOUT)
        throw UsbTransferError("read", r, maxLength, static_cast<size_t>(transferred));

    return static_cast<size_t>(transferred);
}

uint8_t UsbTransport::readPortStatus() {
    if (!handle_) return 0xFF;
    uint8_t status = 0;
    int r = libusb_control_transfer(
        handle_,
        LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_CLASS | LIBUSB_RECIPIENT_INTERFACE,
        0x01, // GET_PORT_STATUS
        0,
        interface_,
        &status,
        1,
        USB_TIMEOUT_MS
    );
    if (r < 0) return 0xFF;
    return status;
}

std::string UsbTransport::formatPortStatus(uint8_t status) {
    if (status == 0xFF) return "Port status unavailable / unsupported";
    std::string res;
    bool paperOut = (status & 0x20) != 0;
    bool selected = (status & 0x10) != 0;
    bool notError = (status & 0x08) != 0;
    res += (paperOut ? "[ALERT: NO PAPER IN TRAY] " : "[Paper Loaded] ");
    res += (selected ? "[Online] " : "[Offline] ");
    res += (notError ? "[Hardware OK]" : "[ALERT: HARDWARE ERROR STATE]");
    return res;
}

std::string UsbTransport::readDeviceId() {
    if (!handle_) throw UsbException("USB device not open", 0);

    // IEEE 1284 Device ID via USB Class request (GET_DEVICE_ID)
    uint8_t buf[1024] = {};
    int r = libusb_control_transfer(
        handle_,
        LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_CLASS | LIBUSB_RECIPIENT_INTERFACE,
        0x00,   // GET_DEVICE_ID
        0x0000,
        static_cast<uint16_t>(interface_),
        buf,
        sizeof(buf) - 1,
        USB_TIMEOUT_MS
    );

    if (r < 0)
        throw UsbException("Failed to read IEEE 1284 device ID", r);

    // First two bytes are big-endian length
    if (r < 2) return {};
    int len = (buf[0] << 8) | buf[1];
    len = std::min(len, r) - 2;
    if (len <= 0) return {};
    return std::string(reinterpret_cast<char*>(buf + 2),
                       static_cast<size_t>(len));
}

std::vector<uint8_t> UsbTransport::readStatusPacket() {
    uint8_t buf[64] = {};
    size_t got = read(buf, sizeof(buf));
    return std::vector<uint8_t>(buf, buf + got);
}

// ═══════════════════════════════════════════════════════════════════════════
// EscP2Encoder
// ═══════════════════════════════════════════════════════════════════════════

EscP2Encoder::EscP2Encoder(const PrintSettings& settings)
    : settings_(settings) {}

// Resolution → ESC/P2 unit code
// ESC/P2 uses 1/3600-inch base units; the header encodes units and dpi.
uint8_t EscP2Encoder::dpiCode(uint32_t dpi) const {
    switch (dpi) {
        case 180: return 0x0A;
        case 360: return 0x05;
        case 720: return 0x02;
        default:  return 0x05;   // fallback to 360
    }
}

uint8_t EscP2Encoder::mediaCode() const {
    switch (settings_.mediaType) {
        case MediaType::Plain:        return 0x00;
        case MediaType::Matte:        return 0x01;
        case MediaType::Glossy:       return 0x02;
        case MediaType::Transparency: return 0x03;
        case MediaType::Envelope:     return 0x04;
        default:                      return 0x00;
    }
}

uint8_t EscP2Encoder::qualityCode() const {
    switch (settings_.quality) {
        case PrintQuality::Draft:  return 0x00;
        case PrintQuality::Normal: return 0x01;
        case PrintQuality::High:   return 0x02;
        case PrintQuality::Photo:  return 0x03;
        default:                   return 0x01;
    }
}

uint32_t EscP2Encoder::dotsPerLineX() const {
    // A4 at given dpi minus margins
    float widthInch = (PAGE_WIDTH_MM / 25.4f);
    uint32_t total  = static_cast<uint32_t>(widthInch * settings_.dpiX);
    uint32_t mLeft  = MARGIN_LEFT_DOTS  * settings_.dpiX / MAX_DPI_X;
    uint32_t mRight = MARGIN_RIGHT_DOTS * settings_.dpiX / MAX_DPI_X;
    return total - mLeft - mRight;
}

uint32_t EscP2Encoder::dotsPerLineY() const {
    float heightInch = (PAGE_HEIGHT_MM / 25.4f);
    uint32_t total   = static_cast<uint32_t>(heightInch * settings_.dpiY);
    uint32_t mTop    = MARGIN_TOP_DOTS    * settings_.dpiY / MAX_DPI_Y;
    uint32_t mBot    = MARGIN_BOTTOM_DOTS * settings_.dpiY / MAX_DPI_Y;
    return total - mTop - mBot;
}

// ─────────────────────────────────────────────────────────────────────────
// Build ESC/P2 job header
// ESC/P2 reference: Epson "ESC/P2 Raster Commands" reference manual
// ─────────────────────────────────────────────────────────────────────────

std::vector<uint8_t> EscP2Encoder::buildJobHeader() const {
    std::vector<uint8_t> cmd;

    // 1. Initialise printer — ESC @
    push(cmd, {ESC, '@'});

    // 2. Set Units — ESC ( U (length 1, base unit selector: 10 = 1/360", 5 = 1/720")
    uint8_t baseUnitSelector = (settings_.dpiY == 720) ? 0x05 : 0x0A;
    push(cmd, {ESC, '(', 'U', 0x01, 0x00, baseUnitSelector});

    // 3. Set Page Length in basic units — ESC ( C (length 2, 16-bit page length)
    uint16_t pageH_units = static_cast<uint16_t>((PAGE_HEIGHT_MM / 25.4f) * settings_.dpiY);
    push(cmd, {ESC, '(', 'C', 0x02, 0x00});
    pushU16LE(cmd, pageH_units);

    // 4. Set Page Format (Margins) — ESC ( c (length 4, 16-bit top and bottom margins)
    uint16_t topMargin_units = static_cast<uint16_t>(MARGIN_TOP_DOTS * settings_.dpiY / MAX_DPI_Y);
    uint16_t botMargin_units = static_cast<uint16_t>(pageH_units - (MARGIN_BOTTOM_DOTS * settings_.dpiY / MAX_DPI_Y));
    push(cmd, {ESC, '(', 'c', 0x04, 0x00});
    pushU16LE(cmd, topMargin_units);
    pushU16LE(cmd, botMargin_units);

    // 5. Set Page Dimensions — ESC ( S (length 4, 16-bit width then height)
    uint16_t pageW_units = static_cast<uint16_t>((PAGE_WIDTH_MM / 25.4f) * settings_.dpiX);
    push(cmd, {ESC, '(', 'S', 0x04, 0x00});
    pushU16LE(cmd, pageW_units);
    pushU16LE(cmd, pageH_units);

    // 6. Set Media Type & Quality — ESC ( m
    push(cmd, {ESC, '(', 'm', 0x02, 0x00, mediaCode(), qualityCode()});

    // 7. Select Color Mode — ESC ( K
    uint8_t colParam = (settings_.colorMode == ColorMode::Grayscale) ? 0x00 : 0x01;
    push(cmd, {ESC, '(', 'K', 0x02, 0x00, colParam, 0x00});

    // 8. Select Graphics Mode — ESC ( G (1 byte length, mode 1)
    // CRITICAL: This must be the final command before raster transfer begins!
    push(cmd, {ESC, '(', 'G', 0x01, 0x00, 0x01});

    return cmd;
}

std::vector<uint8_t> EscP2Encoder::buildPageHeader(uint32_t /*pageIndex*/) const {
    std::vector<uint8_t> cmd;

    // Top margin advance
    uint32_t topDots = MARGIN_TOP_DOTS * settings_.dpiY / MAX_DPI_Y;
    if (topDots > 0) {
        auto adv = buildVerticalAdvance(topDots);
        cmd.insert(cmd.end(), adv.begin(), adv.end());
    }
    return cmd;
}

std::vector<uint8_t> EscP2Encoder::buildPageFooter() const {
    std::vector<uint8_t> cmd;
    push(cmd, {0x0D, 0x0C}); // Carriage Return + Form Feed
    return cmd;
}

std::vector<uint8_t> EscP2Encoder::buildJobFooter() const {
    std::vector<uint8_t> cmd;
    // ESC @ — reset printer (cleans up spool buffers)
    push(cmd, {ESC, '@'});
    return cmd;
}

// ─────────────────────────────────────────────────────────────────────────
// ESC ( i — raster data command
// Format:
//   ESC ( i  len_lo len_hi  colour  compression  bitsPerDot
//            lineWidth_lo lineWidth_hi  numLines_lo numLines_hi
//   followed by compressed data bytes
//
// colour:      0=K 1=C 2=M 3=Y
// compression: 0=raw 1=packbits
// bitsPerDot:  1
// ─────────────────────────────────────────────────────────────────────────

std::vector<uint8_t> EscP2Encoder::encodeRasterLine(
    const uint8_t* pixels,
    uint32_t       pixelCount,
    uint8_t        colorChannel,
    bool           compress) const
{
    // Convert 8-bpp intensities to 1-bpp dots via threshold (128)
    std::vector<uint8_t> dots = threshold1bpp(pixels, pixelCount);

    std::vector<uint8_t> payload;
    uint8_t compressionMode = 0;

    if (compress) {
        payload = packBitsCompress(dots.data(), dots.size());
        compressionMode = 1;
    } else {
        payload = dots;
        compressionMode = 0;
    }

    uint16_t bytesPerLine = static_cast<uint16_t>(dots.size());
    uint16_t numLines     = 1;

    std::vector<uint8_t> cmd;
    // Standard ESC/P2 raster command: ESC i [color] [comp] [bpp] [bytes_lo bytes_hi] [lines_lo lines_hi] [data]
    push(cmd, {ESC, 'i',
               colorChannel,
               compressionMode,
               0x01});               // 1 bit per dot
    pushU16LE(cmd, bytesPerLine);
    pushU16LE(cmd, numLines);
    cmd.insert(cmd.end(), payload.begin(), payload.end());

    return cmd;
}

std::vector<uint8_t> EscP2Encoder::buildVerticalAdvance(uint32_t dots) const {
    // ESC ( v — paper feed
    // For feeds ≤65535 dots we use a 2-byte form; larger use two commands.
    std::vector<uint8_t> cmd;
    while (dots > 0) {
        uint32_t step = std::min(dots, static_cast<uint32_t>(0xFFFF));
        push(cmd, {ESC, '(', 'v', 0x02, 0x00});
        pushU16LE(cmd, static_cast<uint16_t>(step));
        dots -= step;
    }
    return cmd;
}

std::vector<uint8_t> EscP2Encoder::buildFormFeed() const {
    std::vector<uint8_t> cmd;
    push(cmd, {0x0C});   // FF character — ejects current page
    return cmd;
}

// ─────────────────────────────────────────────────────────────────────────
// Private helpers
// ─────────────────────────────────────────────────────────────────────────

// 8-bpp greyscale → 1-bpp (MSB first, threshold = 128)
std::vector<uint8_t> EscP2Encoder::threshold1bpp(
    const uint8_t* grey, uint32_t count)
{
    uint32_t byteCount = (count + 7) / 8;
    std::vector<uint8_t> out(byteCount, 0);
    for (uint32_t i = 0; i < count; ++i) {
        if (grey[i] < 128) {  // dark pixel = dot
            out[i / 8] |= static_cast<uint8_t>(0x80 >> (i % 8));
        }
    }
    return out;
}

// EPSON PackBits (compatible with Apple PackBits / TIFF compression 32773)
std::vector<uint8_t> EscP2Encoder::packBitsCompress(
    const uint8_t* data, size_t len)
{
    std::vector<uint8_t> out;
    out.reserve(len + len / 128 + 1);

    size_t i = 0;
    while (i < len) {
        // Look for a run of identical bytes
        size_t runLen = 1;
        while (runLen < 128 && i + runLen < len && data[i + runLen] == data[i])
            ++runLen;

        if (runLen > 1) {
            // Repeated run: header = -(runLen-1), then byte
            out.push_back(static_cast<uint8_t>(-(static_cast<int>(runLen) - 1) & 0xFF));
            out.push_back(data[i]);
            i += runLen;
        } else {
            // Literal run
            size_t litStart = i;
            size_t litLen   = 1;
            while (litLen < 128 && i + litLen < len &&
                   (i + litLen + 1 >= len || data[i + litLen] != data[i + litLen + 1]))
            {
                ++litLen;
            }
            out.push_back(static_cast<uint8_t>(litLen - 1));
            for (size_t k = 0; k < litLen; ++k)
                out.push_back(data[litStart + k]);
            i += litLen;
        }
    }
    return out;
}

// ═══════════════════════════════════════════════════════════════════════════
// ColorProfile
// ═══════════════════════════════════════════════════════════════════════════

void ColorProfile::rgbToCmyk(uint8_t r, uint8_t g, uint8_t b,
                              uint8_t& c, uint8_t& m, uint8_t& y, uint8_t& k) {
    // Simple RGB → CMY → CMYK conversion (no ICC profile)
    uint8_t maxRGB = std::max({r, g, b});

    if (maxRGB == 0) {
        c = m = y = 0;
        k = 255;
        return;
    }

    float fr = r / 255.0f;
    float fg = g / 255.0f;
    float fb = b / 255.0f;

    float fk = 1.0f - std::max({fr, fg, fb});
    float fc, fm, fy;

    if (fk < 1.0f) {
        fc = (1.0f - fr - fk) / (1.0f - fk);
        fm = (1.0f - fg - fk) / (1.0f - fk);
        fy = (1.0f - fb - fk) / (1.0f - fk);
    } else {
        fc = fm = fy = 0.0f;
    }

    c = static_cast<uint8_t>(fc * 255.0f + 0.5f);
    m = static_cast<uint8_t>(fm * 255.0f + 0.5f);
    y = static_cast<uint8_t>(fy * 255.0f + 0.5f);
    k = static_cast<uint8_t>(fk * 255.0f + 0.5f);
}

void ColorProfile::separateLine(const uint8_t* rgb, uint32_t pixelCount,
                                 uint8_t* planeC, uint8_t* planeM,
                                 uint8_t* planeY, uint8_t* planeK) {
    for (uint32_t i = 0; i < pixelCount; ++i) {
        uint8_t r = rgb[i * 3 + 0];
        uint8_t g = rgb[i * 3 + 1];
        uint8_t b = rgb[i * 3 + 2];
        rgbToCmyk(r, g, b, planeC[i], planeM[i], planeY[i], planeK[i]);
    }
}

void ColorProfile::ditherFloydSteinberg(uint8_t* plane,
                                         uint32_t width, uint32_t height) {
    // In-place 8-bpp → 8-bpp (0 or 255) dither
    std::vector<int> errors(static_cast<size_t>(width * height), 0);

    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            uint32_t idx  = y * width + x;
            int      oldV = static_cast<int>(plane[idx]) + errors[idx];
            int      newV = (oldV >= 128) ? 255 : 0;
            plane[idx]    = static_cast<uint8_t>(newV);   // newV is always 0 or 255
            int      err  = oldV - newV;

            if (x + 1 < width)
                errors[idx + 1]         += err * 7 / 16;
            if (y + 1 < height) {
                if (x > 0)
                    errors[idx + width - 1] += err * 3 / 16;
                errors[idx + width]         += err * 5 / 16;
                if (x + 1 < width)
                    errors[idx + width + 1] += err * 1 / 16;
            }
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// PrintJob
// ═══════════════════════════════════════════════════════════════════════════

PrintJob::PrintJob(const PrintSettings& settings)
    : settings_(settings)
    , encoder_(settings)
{
    auto hdr = encoder_.buildJobHeader();
    append(hdr);
}

void PrintJob::addPage(const uint8_t* pixels, uint32_t width, uint32_t height) {
    // Page header
    auto ph = encoder_.buildPageHeader(pageCount_);
    append(ph);

    bool isColor = (settings_.colorMode == ColorMode::Color);
    uint32_t channels = isColor ? 4u : 1u;

    // Temporary plane buffers for one scan line
    std::vector<uint8_t> planeC(width), planeM(width);
    std::vector<uint8_t> planeY(width), planeK(width);
    std::vector<uint8_t> planeGrey(width);

    uint32_t bytesPerPixel = isColor ? 3u : 1u;

    for (uint32_t row = 0; row < height; ++row) {
        const uint8_t* rowPixels = pixels + row * width * bytesPerPixel;

        if (isColor) {
            ColorProfile::separateLine(rowPixels, width,
                                       planeC.data(), planeM.data(),
                                       planeY.data(), planeK.data());
            // Epson ESC/P2 color channels:
            // 0x00 = Black (K)
            // 0x01 = Magenta (M)
            // 0x02 = Cyan (C)
            // 0x04 = Yellow (Y)
            auto lineK = encoder_.encodeRasterLine(planeK.data(), width, 0x00);
            auto lineC = encoder_.encodeRasterLine(planeC.data(), width, 0x02);
            auto lineM = encoder_.encodeRasterLine(planeM.data(), width, 0x01);
            auto lineY = encoder_.encodeRasterLine(planeY.data(), width, 0x04);
            append(lineK); append(lineC);
            append(lineM); append(lineY);
        } else {
            // Greyscale: channel 0x00 (Black)
            std::copy(rowPixels, rowPixels + width, planeGrey.data());
            auto lineK = encoder_.encodeRasterLine(planeGrey.data(), width, 0x00);
            append(lineK);
        }

        // Advance to next raster line: Carriage Return + 1-dot vertical feed
        append({0x0D});
        auto adv = encoder_.buildVerticalAdvance(1);
        append(adv);

        (void)channels; // suppress unused warning
    }

    auto pf = encoder_.buildPageFooter();
    append(pf);
    ++pageCount_;
}

const std::vector<uint8_t>& PrintJob::encodedData() const { return buffer_; }
size_t PrintJob::encodedSize() const { return buffer_.size(); }
uint32_t PrintJob::pageCount() const { return pageCount_; }

void PrintJob::append(const std::vector<uint8_t>& bytes) {
    buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
}

// ═══════════════════════════════════════════════════════════════════════════
// EpsonL130Driver
// ═══════════════════════════════════════════════════════════════════════════

EpsonL130Driver::EpsonL130Driver() = default;
EpsonL130Driver::~EpsonL130Driver() {
    try { disconnect(); } catch (...) {}
}

void EpsonL130Driver::connect() {
    transport_.open();
}

void EpsonL130Driver::disconnect() {
    transport_.close();
}

bool EpsonL130Driver::isConnected() const {
    return transport_.isOpen();
}

uint16_t EpsonL130Driver::getProductId() const {
    return transport_.getProductId();
}

uint8_t EpsonL130Driver::readPortStatus() {
    return transport_.readPortStatus();
}

std::string EpsonL130Driver::queryPortStatusString() {
    uint8_t s = transport_.readPortStatus();
    return transport_.formatPortStatus(s);
}

std::string EpsonL130Driver::queryDeviceId() {
    return transport_.readDeviceId();
}

std::string EpsonL130Driver::queryStatus() {
    // Send ESC/P2 status request — ESC (s
    uint8_t statusReq[] = {ESC, '(', 's', 0x01, 0x00, 0x00};
    transport_.write(statusReq, sizeof(statusReq));

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    auto pkt = transport_.readStatusPacket();

    if (pkt.empty()) return "No response from printer";

    std::ostringstream oss;
    oss << "Status packet (" << pkt.size() << " bytes): ";
    for (uint8_t b : pkt)
        oss << std::hex << std::setw(2) << std::setfill('0') << (int)b << ' ';
    return oss.str();
}

std::array<uint8_t, 4> EpsonL130Driver::readInkLevels() {
    // Remote control sequence to query ink levels (Epson service command TI)
    std::vector<uint8_t> cmd;
    // Enter remote mode
    push(cmd, {ESC, '(', 'R', 0x08, 0x00, 0x00,
               'R', 'E', 'M', 'O', 'T', 'E', '1', 0x00});
    // TI command: read ink levels (one variant)
    push(cmd, {'T', 'I', 0x00, 0x00});
    // Exit remote mode
    push(cmd, {ESC, 0x00, 0x00, 0x00});

    transport_.write(cmd);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    auto resp = transport_.readStatusPacket();

    std::array<uint8_t, 4> levels = {255, 255, 255, 255}; // unknown = 255
    // Response format is firmware-dependent; attempt basic parse
    // Byte offsets for L-series:  [K, C, M, Y] somewhere after a header
    if (resp.size() >= 8) {
        levels[0] = resp[4];  // K
        levels[1] = resp[5];  // C
        levels[2] = resp[6];  // M
        levels[3] = resp[7];  // Y
    }
    return levels;
}

void EpsonL130Driver::selfTest() {
    // ESC ( V  self-test
    uint8_t st[] = {ESC, '(', 'V', 0x02, 0x00, 0x00, 0x01};
    transport_.write(st, sizeof(st));
}

void EpsonL130Driver::reset() {
    uint8_t r[] = {ESC, '@'};
    transport_.write(r, sizeof(r));
}

void EpsonL130Driver::sendRaw(const uint8_t* data, size_t length) {
    transport_.write(data, length);
}

void EpsonL130Driver::printRaster(
    const uint8_t*       pixels,
    uint32_t             width,
    uint32_t             height,
    const PrintSettings& settings,
    ProgressCallback     onProgress)
{
    PrintJob job(settings);
    job.addPage(pixels, width, height);
    submitJob(job, onProgress);
}

void EpsonL130Driver::submitJob(const PrintJob& job, ProgressCallback onProgress) {
    if (!isConnected())
        throw PrintException("Printer not connected — call connect() first");

    const auto& data = job.encodedData();
    logDebug("Starting transmission of " + std::to_string(data.size()) + " bytes to printer...");
    
    // Check hardware status before print
    uint8_t preStatus = transport_.readPortStatus();
    logDebug("Pre-print Port Status: " + transport_.formatPortStatus(preStatus));
    if ((preStatus & 0x20) != 0) {
        logDebug("WARNING: Printer reports NO PAPER in tray before printing!", true);
    }

    transmitBuffer(data, onProgress);
    logDebug("Transmission complete (100% of bytes written to USB pipe).");

    // Check status after transmission
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    uint8_t postStatus = transport_.readPortStatus();
    logDebug("Post-print Port Status: " + transport_.formatPortStatus(postStatus));
    
    std::string postSt2 = queryStatus();
    logDebug("Post-print ST2 Query: " + postSt2);
}

void EpsonL130Driver::transmitBuffer(
    const std::vector<uint8_t>& buf,
    ProgressCallback             onProgress,
    size_t                       chunkSize)
{
    size_t total   = buf.size();
    size_t offset  = 0;
    uint32_t chunk = 0;
    uint32_t totalChunks = static_cast<uint32_t>((total + chunkSize - 1) / chunkSize);

    while (offset < total) {
        size_t toSend = std::min(chunkSize, total - offset);
        transport_.write(buf.data() + offset, toSend);
        offset += toSend;
        ++chunk;

        if (onProgress)
            onProgress(chunk, totalChunks);
    }
}

} // namespace epson_l130
