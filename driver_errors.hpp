/**
 * driver_errors.hpp
 * Comprehensive, typed error hierarchy for the EPSON L130 driver.
 *
 * Every error category has its own class so callers can catch exactly
 * what they care about, and every constructor takes a human-readable
 * message that is reported verbatim to the user.
 *
 * Hierarchy
 * ─────────
 * std::exception
 *  └─ DriverException          (base — catches everything from this driver)
 *      ├─ ConnectionError      (USB / device-not-found errors)
 *      │   ├─ DeviceNotFound
 *      │   ├─ DeviceBusy
 *      │   ├─ PermissionDenied
 *      │   ├─ KernelDriverConflict
 *      │   └─ UsbTransferError
 *      ├─ PrintError           (job / page / data errors)
 *      │   ├─ InvalidSettings
 *      │   ├─ PageTooLarge
 *      │   ├─ EmptyJob
 *      │   └─ EncodingError
 *      ├─ SpoolerError         (queue / scheduling errors)
 *      │   ├─ QueueFull
 *      │   ├─ DuplicateJobId
 *      │   └─ JobNotFound
 *      ├─ SimulationError      (emulation-specific errors)
 *      │   └─ SimulationAborted
 *      ├─ PdfExportError       (PDF save errors)
 *      │   ├─ FileCreateFailed
 *      │   └─ PdfWriteError
 *      └─ HardwareError        (printer-side faults reported in status)
 *          ├─ PaperJam
 *          ├─ OutOfPaper
 *          ├─ InkLow
 *          └─ PrinterOffline
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <optional>

namespace epson_l130 {

// ═══════════════════════════════════════════════════════════════════════════
// Base
// ═══════════════════════════════════════════════════════════════════════════

class DriverException : public std::runtime_error {
public:
    explicit DriverException(const std::string& msg,
                             const std::string& context = "")
        : std::runtime_error(format(msg, context))
        , message_(msg)
        , context_(context)
    {}

    // Plain message without context decoration
    const std::string& userMessage() const noexcept { return message_; }
    // Optional context string (function name, file name, job id …)
    const std::string& context()     const noexcept { return context_; }

private:
    std::string message_;
    std::string context_;

    static std::string format(const std::string& msg, const std::string& ctx) {
        return ctx.empty() ? msg : msg + "  [context: " + ctx + "]";
    }
};

// ═══════════════════════════════════════════════════════════════════════════
// Connection errors  (USB / device enumeration)
// ═══════════════════════════════════════════════════════════════════════════

class ConnectionError : public DriverException {
public:
    ConnectionError(const std::string& msg, const std::string& ctx = "")
        : DriverException(msg, ctx) {}
};

/// Printer not detected on any USB bus.
class DeviceNotFound : public ConnectionError {
public:
    DeviceNotFound()
        : ConnectionError(
            "EPSON L130 not found. Check that the printer is powered on "
            "and connected via USB, then try again.") {}
};

/// Another process (e.g. CUPS) already holds the USB interface.
class DeviceBusy : public ConnectionError {
public:
    DeviceBusy()
        : ConnectionError(
            "Printer is busy — another process has claimed the USB interface. "
            "Stop CUPS ('sudo systemctl stop cups') or unload usblp "
            "('sudo rmmod usblp') and try again.") {}
};

/// Insufficient OS permissions to open the USB device.
class PermissionDenied : public ConnectionError {
public:
    PermissionDenied()
        : ConnectionError(
            "Permission denied opening USB device. "
            "Either run with sudo, or add a udev rule:\n"
            "  echo 'ATTRS{idVendor}==\"04b8\", "
            "MODE=\"0666\", GROUP=\"plugdev\"' "
            "| sudo tee /etc/udev/rules.d/99-epson-l130.rules\n"
            "  sudo udevadm control --reload-rules && sudo udevadm trigger\n"
            "Then log out and back in.") {}
};

/// The kernel usblp driver is attached and could not be detached.
class KernelDriverConflict : public ConnectionError {
public:
    explicit KernelDriverConflict(int libusbErr)
        : ConnectionError(
            "Failed to detach the kernel usblp driver (libusb error " +
            std::to_string(libusbErr) + "). "
            "Try: sudo rmmod usblp")
        , libusbError(libusbErr) {}
    int libusbError;
};

/// A USB bulk transfer failed mid-job.
class UsbTransferError : public ConnectionError {
public:
    UsbTransferError(const std::string& direction, int libusbErr,
                     size_t attempted, size_t transferred)
        : ConnectionError(
            "USB " + direction + " transfer failed (libusb error " +
            std::to_string(libusbErr) + "): sent " +
            std::to_string(transferred) + " of " +
            std::to_string(attempted) + " bytes. "
            "Check the USB cable and try a different port.")
        , libusbError(libusbErr)
        , attempted(attempted)
        , transferred(transferred)
    {}
    int    libusbError;
    size_t attempted;
    size_t transferred;
};

/// Generic libusb failure that doesn't fit a more specific class above.
class UsbException : public ConnectionError {
public:
    UsbException(const std::string& msg, int libusbErr)
        : ConnectionError(msg + " (libusb error " + std::to_string(libusbErr) + ")")
        , libusbError(libusbErr) {}
    int libusbError;
};

// ═══════════════════════════════════════════════════════════════════════════
// Print / job errors
// ═══════════════════════════════════════════════════════════════════════════

class PrintError : public DriverException {
public:
    PrintError(const std::string& msg, const std::string& ctx = "")
        : DriverException(msg, ctx) {}
};

/// Generic job error (e.g. submitting while not connected).
class PrintException : public PrintError {
public:
    explicit PrintException(const std::string& msg) : PrintError(msg) {}
};

/// A PrintSettings field has an invalid or contradictory value.
class InvalidSettings : public PrintError {
public:
    explicit InvalidSettings(const std::string& detail)
        : PrintError("Invalid print settings: " + detail) {}
};

/// The raster image is larger than the maximum printable area.
class PageTooLarge : public PrintError {
public:
    PageTooLarge(uint32_t wGot, uint32_t hGot, uint32_t wMax, uint32_t hMax)
        : PrintError(
            "Page dimensions " + std::to_string(wGot) + "×" +
            std::to_string(hGot) + " px exceed the maximum printable area " +
            std::to_string(wMax) + "×" + std::to_string(hMax) +
            " px at the requested DPI. Reduce DPI or crop the image.")
    {}
};

/// A job was submitted with no pages.
class EmptyJob : public PrintError {
public:
    EmptyJob() : PrintError("Cannot submit an empty print job (no pages added).") {}
};

/// A file chosen for printing could not be opened or decoded.
class FileLoadError : public PrintError {
public:
    FileLoadError(const std::string& path, const std::string& reason)
        : PrintError("Cannot load '" + path + "': " + reason, path) {}
};

/// ESC/P2 encoding encountered unexpected data.
class EncodingError : public PrintError {
public:
    explicit EncodingError(const std::string& detail)
        : PrintError("ESC/P2 encoding error: " + detail) {}
};

// ═══════════════════════════════════════════════════════════════════════════
// Spooler / queue errors
// ═══════════════════════════════════════════════════════════════════════════

class SpoolerError : public DriverException {
public:
    SpoolerError(const std::string& msg, const std::string& ctx = "")
        : DriverException(msg, ctx) {}
};

/// The spool queue has reached its capacity limit.
class QueueFull : public SpoolerError {
public:
    explicit QueueFull(size_t capacity)
        : SpoolerError(
            "Print queue is full (" + std::to_string(capacity) +
            " jobs). Cancel or wait for a job to finish before adding more.") {}
};

/// A job with the same ID was already enqueued.
class DuplicateJobId : public SpoolerError {
public:
    explicit DuplicateJobId(const std::string& jobId)
        : SpoolerError("A job with ID '" + jobId + "' is already in the queue.",
                       jobId) {}
};

/// Tried to cancel / query a job ID that does not exist.
class JobNotFound : public SpoolerError {
public:
    explicit JobNotFound(const std::string& jobId)
        : SpoolerError("Job '" + jobId + "' not found in the queue.", jobId) {}
};

// ═══════════════════════════════════════════════════════════════════════════
// Simulation errors
// ═══════════════════════════════════════════════════════════════════════════

class SimulationError : public DriverException {
public:
    SimulationError(const std::string& msg, const std::string& ctx = "")
        : DriverException(msg, ctx) {}
};

/// The simulation was externally aborted (user cancelled).
class SimulationAborted : public SimulationError {
public:
    explicit SimulationAborted(const std::string& jobId)
        : SimulationError("Simulation aborted for job '" + jobId + "'.", jobId) {}
};

// ═══════════════════════════════════════════════════════════════════════════
// PDF export errors
// ═══════════════════════════════════════════════════════════════════════════

class PdfExportError : public DriverException {
public:
    PdfExportError(const std::string& msg, const std::string& ctx = "")
        : DriverException(msg, ctx) {}
};

/// Could not create / open the output file.
class FileCreateFailed : public PdfExportError {
public:
    explicit FileCreateFailed(const std::string& path)
        : PdfExportError(
            "Cannot create output file '" + path + "'. "
            "Check the path exists and you have write permission.", path) {}
};

/// A write to the PDF byte stream failed.
class PdfWriteError : public PdfExportError {
public:
    explicit PdfWriteError(const std::string& detail)
        : PdfExportError("PDF write error: " + detail) {}
};

// ═══════════════════════════════════════════════════════════════════════════
// Hardware / status errors
// ═══════════════════════════════════════════════════════════════════════════

class HardwareError : public DriverException {
public:
    HardwareError(const std::string& msg, const std::string& ctx = "")
        : DriverException(msg, ctx) {}
};

class PaperJam : public HardwareError {
public:
    PaperJam()
        : HardwareError(
            "Paper jam detected. Open the printer cover, remove jammed paper, "
            "and press the resume button.") {}
};

class OutOfPaper : public HardwareError {
public:
    OutOfPaper()
        : HardwareError(
            "Out of paper. Load paper into the input tray and press resume.") {}
};

class InkLow : public HardwareError {
public:
    explicit InkLow(const std::string& channel)
        : HardwareError("Ink low: " + channel + " cartridge needs replacement.") {}
};

class PrinterOffline : public HardwareError {
public:
    PrinterOffline()
        : HardwareError(
            "Printer is offline or unresponsive. "
            "Check the power switch and USB connection.") {}
};

} // namespace epson_l130
