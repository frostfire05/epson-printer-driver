/**
 * print_spooler.hpp
 * Priority-based print spooler with two backends:
 *   1. PDF export  — saves a minimal valid PDF file to disk.
 *   2. Simulation  — emulates the EPSON L130 in software, processing jobs
 *                    from a priority queue one at a time with live progress.
 *
 * Both backends are activated automatically when the real USB printer is
 * unavailable; the user picks which one at runtime.
 *
 * Thread-safety: the spooler's public API is NOT thread-safe. All calls
 * must come from the same thread (typically the main thread) unless noted.
 * The simulation internally uses a worker thread but exposes no raw threads
 * to the caller.
 */

#pragma once

#include "epson_l130_driver.hpp"
#include "driver_errors.hpp"

#include <string>
#include <vector>
#include <queue>
#include <unordered_map>
#include <functional>
#include <memory>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <chrono>
#include <optional>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <iostream>
#include <cstring>

namespace epson_l130 {

// ═══════════════════════════════════════════════════════════════════════════
// Job priority  (higher number = higher priority)
// ═══════════════════════════════════════════════════════════════════════════

enum class JobPriority : uint8_t {
    Low      = 1,
    Normal   = 5,
    High     = 8,
    Critical = 10,
};

inline const char* priorityName(JobPriority p) {
    switch (p) {
        case JobPriority::Low:      return "Low";
        case JobPriority::Normal:   return "Normal";
        case JobPriority::High:     return "High";
        case JobPriority::Critical: return "Critical";
        default:                    return "Unknown";
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Job status
// ═══════════════════════════════════════════════════════════════════════════

enum class JobStatus {
    Queued,
    Processing,
    Done,
    Failed,
    Cancelled,
};

inline const char* statusName(JobStatus s) {
    switch (s) {
        case JobStatus::Queued:     return "Queued";
        case JobStatus::Processing: return "Processing";
        case JobStatus::Done:       return "Done";
        case JobStatus::Failed:     return "Failed";
        case JobStatus::Cancelled:  return "Cancelled";
        default:                    return "Unknown";
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// SpoolEntry — one job in the queue
// ═══════════════════════════════════════════════════════════════════════════

struct SpoolEntry {
    std::string             jobId;
    std::string             label;           // human-readable name
    JobPriority             priority;
    PrintSettings           settings;
    std::vector<uint8_t>    pixels;          // packed RGB or grey
    uint32_t                width  = 0;
    uint32_t                height = 0;
    JobStatus               status = JobStatus::Queued;
    std::string             errorMessage;    // set on failure
    std::chrono::steady_clock::time_point enqueuedAt;
    std::chrono::steady_clock::time_point startedAt;
    std::chrono::steady_clock::time_point finishedAt;

    // For the priority queue comparator (higher priority value = top)
    bool operator<(const SpoolEntry& o) const {
        if (priority != o.priority)
            return static_cast<uint8_t>(priority) < static_cast<uint8_t>(o.priority);
        return enqueuedAt > o.enqueuedAt;  // earlier enqueue = higher priority on tie
    }
};

// Pointer wrapper so the priority_queue holds shared_ptrs (avoids copies)
struct SpoolEntryPtrCmp {
    bool operator()(const std::shared_ptr<SpoolEntry>& a,
                    const std::shared_ptr<SpoolEntry>& b) const {
        return *a < *b;
    }
};

// ═══════════════════════════════════════════════════════════════════════════
// Callbacks
// ═══════════════════════════════════════════════════════════════════════════

// Called when a job changes status
using JobStatusCallback = std::function<void(const std::string& jobId,
                                             JobStatus          newStatus,
                                             const std::string& message)>;

// Called with progress within a single job (0-100)
using JobProgressCallback = std::function<void(const std::string& jobId,
                                               uint32_t           percent)>;

// ═══════════════════════════════════════════════════════════════════════════
// PdfExporter — writes a minimal but valid PDF from raster pixel data
// ═══════════════════════════════════════════════════════════════════════════

class PdfExporter {
public:
    /**
     * Export one page of RGB pixel data to a PDF file.
     * Throws FileCreateFailed or PdfWriteError on failure.
     *
     * The PDF is written as:
     *   - A single-page document at A4 size (595 × 842 pt)
     *   - The image is embedded as a raw RGB XObject (uncompressed,
     *     self-contained — no external dependencies required).
     */
    static void exportPage(const std::string& outputPath,
                           const uint8_t*     pixels,
                           uint32_t           width,
                           uint32_t           height,
                           const PrintSettings& settings);

    /**
     * Export multiple pages to a single multi-page PDF.
     * Each element of pages is { pixels, width, height }.
     */
    struct PageData {
        const uint8_t* pixels;
        uint32_t       width;
        uint32_t       height;
    };

    static void exportPages(const std::string&          outputPath,
                            const std::vector<PageData>& pages,
                            const PrintSettings&         settings);

private:
    // Write a PDF integer cross-reference table and return the xref byte offset
    static size_t writeXref(std::ofstream& f,
                            const std::vector<size_t>& offsets);

    static std::string mmToPt(float mm);

    // Encode raw RGB bytes as PDF hex string (portable, no zlib needed)
    static std::string hexEncode(const uint8_t* data, size_t len);
};

// ═══════════════════════════════════════════════════════════════════════════
// PrinterSimulator — emulates the L130 without hardware
// ═══════════════════════════════════════════════════════════════════════════

class PrinterSimulator {
public:
    PrinterSimulator();
    ~PrinterSimulator();

    // Non-copyable
    PrinterSimulator(const PrinterSimulator&)            = delete;
    PrinterSimulator& operator=(const PrinterSimulator&) = delete;

    /**
     * Simulate printing one job synchronously (blocks until done or aborted).
     * Reports progress via onProgress (0–100) and logs via onLog.
     * Throws SimulationAborted if abort() is called from another thread.
     */
    void simulateJob(const SpoolEntry&    job,
                     JobProgressCallback  onProgress,
                     std::function<void(const std::string&)> onLog);

    /**
     * Signal the currently running simulateJob() to stop early.
     * Safe to call from any thread.
     */
    void abort();

    /** Simulated bytes-per-second throughput (for timing realism). */
    void setThroughput(size_t bytesPerSecond);

private:
    std::atomic<bool>  abortFlag_{false};
    size_t             throughput_ = 512 * 1024;  // 512 KB/s default

    void simulateTransmit(const std::vector<uint8_t>& data,
                          const std::string&           jobId,
                          JobProgressCallback          onProgress,
                          std::function<void(const std::string&)> onLog);
};

// ═══════════════════════════════════════════════════════════════════════════
// PrintSpooler — priority queue + dispatcher
// ═══════════════════════════════════════════════════════════════════════════

class PrintSpooler {
public:
    static constexpr size_t DEFAULT_QUEUE_CAPACITY = 32;

    enum class Mode {
        Simulate,   // emulate the printer in software
        SavePdf,    // write each job to a PDF file
    };

    explicit PrintSpooler(Mode   mode,
                          size_t queueCapacity = DEFAULT_QUEUE_CAPACITY);
    ~PrintSpooler();

    // Non-copyable
    PrintSpooler(const PrintSpooler&)            = delete;
    PrintSpooler& operator=(const PrintSpooler&) = delete;

    // ── Configuration ─────────────────────────────────────────────────────

    /** Directory where PDF files are saved (SavePdf mode). Default: "./" */
    void setPdfOutputDir(const std::string& dir);

    /** Register a callback for job status transitions. */
    void setStatusCallback(JobStatusCallback cb);

    /** Register a callback for within-job progress updates. */
    void setProgressCallback(JobProgressCallback cb);

    /** Register a callback for log/diagnostic messages. */
    void setLogCallback(std::function<void(const std::string&)> cb);

    // ── Queue management ──────────────────────────────────────────────────

    /**
     * Add a job to the priority queue.
     * Throws QueueFull or DuplicateJobId.
     * Returns the job ID.
     */
    std::string enqueue(const std::string&   label,
                        JobPriority          priority,
                        const PrintSettings& settings,
                        const uint8_t*       pixels,
                        uint32_t             width,
                        uint32_t             height);

    /**
     * Cancel a queued job by ID.
     * Throws JobNotFound if the ID is not in the queue.
     * Has no effect on a job already Processing or Done.
     */
    void cancel(const std::string& jobId);

    /** Cancel all queued jobs (leaves Processing/Done jobs untouched). */
    void cancelAll();

    /** Abort the currently processing job and re-queue the next one. */
    void abortCurrent();

    // ── Execution ─────────────────────────────────────────────────────────

    /**
     * Process all jobs synchronously, one at a time by priority.
     * Blocks until the queue is empty or stop() is called.
     * Prints a live queue status table to stdout as each job runs.
     */
    void processAll();

    /**
     * Start a background thread that calls processAll().
     * Returns immediately; call waitForAll() to join.
     */
    void startAsync();

    /** Wait for the background thread to finish. */
    void waitForAll();

    /** Signal the background thread to stop after the current job. */
    void stop();

    // ── Introspection ─────────────────────────────────────────────────────

    /** Total jobs currently in the queue (including Processing). */
    size_t queueSize() const;

    /** All jobs (pending + processing + done) in insertion order. */
    std::vector<std::shared_ptr<SpoolEntry>> allJobs() const;

    /** Find a specific job by ID. Returns nullptr if not found. */
    std::shared_ptr<SpoolEntry> findJob(const std::string& jobId) const;

    /** Print a formatted queue summary table to stdout. */
    void printQueueTable() const;

private:
    Mode        mode_;
    size_t      capacity_;
    std::string pdfDir_{"."};

    // Priority queue of pending jobs
    std::priority_queue<
        std::shared_ptr<SpoolEntry>,
        std::vector<std::shared_ptr<SpoolEntry>>,
        SpoolEntryPtrCmp>               pq_;

    // All jobs ever added (for status display and lookups)
    std::vector<std::shared_ptr<SpoolEntry>> allJobs_;
    mutable std::mutex                       jobsMutex_;

    // Currently active job
    std::shared_ptr<SpoolEntry> currentJob_;

    // Callbacks
    JobStatusCallback                       onStatus_;
    JobProgressCallback                     onProgress_;
    std::function<void(const std::string&)> onLog_;

    // Simulation backend
    PrinterSimulator simulator_;

    // Async execution
    std::thread       workerThread_;
    std::atomic<bool> stopRequested_{false};

    // ── Private helpers ───────────────────────────────────────────────────

    std::string generateJobId();
    void log(const std::string& msg);
    void notifyStatus(const std::shared_ptr<SpoolEntry>& job,
                      JobStatus newStatus, const std::string& msg = "");

    void processJob(std::shared_ptr<SpoolEntry> job);
    void processJobSimulate(std::shared_ptr<SpoolEntry> job);
    void processJobPdf(std::shared_ptr<SpoolEntry> job);

    // Print colored ANSI status line for a job
    static std::string jobStatusLine(const SpoolEntry& job, bool isCurrent);
};

// ═══════════════════════════════════════════════════════════════════════════
// FallbackDispatcher — top-level "printer not found" handler
// ═══════════════════════════════════════════════════════════════════════════

/**
 * Call this when EpsonL130Driver::connect() throws DeviceNotFound (or any
 * ConnectionError).  It prompts the user interactively to choose between
 * PDF export and simulation, creates a PrintSpooler in the selected mode,
 * and returns it ready to accept jobs.
 *
 * outputDir: where PDF files are saved (used only in SavePdf mode).
 */
std::unique_ptr<PrintSpooler> handlePrinterNotFound(
    const std::string& outputDir = ".");

} // namespace epson_l130
