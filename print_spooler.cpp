/**
 * print_spooler.cpp — implementation of PdfExporter, PrinterSimulator,
 * PrintSpooler, and FallbackDispatcher.
 */

#include "print_spooler.hpp"
#include "image_loader.hpp"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <chrono>
#include <filesystem>
#include <thread>

namespace epson_l130 {

// ═══════════════════════════════════════════════════════════════════════════
// ANSI colour helpers (terminal only; degrade gracefully if not a tty)
// ═══════════════════════════════════════════════════════════════════════════
namespace ansi {
    constexpr const char* RESET  = "\033[0m";
    constexpr const char* BOLD   = "\033[1m";
    constexpr const char* RED    = "\033[31m";
    constexpr const char* GREEN  = "\033[32m";
    constexpr const char* YELLOW = "\033[33m";
    constexpr const char* CYAN   = "\033[36m";
    constexpr const char* WHITE  = "\033[37m";
    constexpr const char* DIM    = "\033[2m";
}

// ═══════════════════════════════════════════════════════════════════════════
// PdfExporter
// ═══════════════════════════════════════════════════════════════════════════

// PDF page size: A4 = 595.28 × 841.89 pt
static constexpr float PDF_PAGE_W_PT = 595.28f;
static constexpr float PDF_PAGE_H_PT = 841.89f;

std::string PdfExporter::hexEncode(const uint8_t* data, size_t len) {
    // Hex-encoded stream: two hex chars per byte, newline every 64 bytes
    std::ostringstream oss;
    oss << std::hex << std::uppercase << std::setfill('0');
    for (size_t i = 0; i < len; ++i) {
        oss << std::setw(2) << static_cast<unsigned>(data[i]);
        if ((i + 1) % 32 == 0) oss << '\n';
    }
    oss << '>';
    return oss.str();
}

static std::string fmtFloat(float v) {
    std::ostringstream s;
    s << std::fixed << std::setprecision(2) << v;
    return s.str();
}

void PdfExporter::exportPage(const std::string&   outputPath,
                              const uint8_t*       pixels,
                              uint32_t             width,
                              uint32_t             height,
                              const PrintSettings& /*settings*/) {
    exportPages(outputPath, {{ pixels, width, height }}, {});
}

void PdfExporter::exportPages(const std::string&          outputPath,
                               const std::vector<PageData>& pages,
                               const PrintSettings&         /*settings*/) {
    if (pages.empty())
        throw PdfWriteError("No pages provided for PDF export.");

    std::ofstream f(outputPath, std::ios::binary | std::ios::trunc);
    if (!f)
        throw FileCreateFailed(outputPath);

    auto writeStr = [&](const std::string& s) {
        f.write(s.data(), static_cast<std::streamsize>(s.size()));
        if (!f) throw PdfWriteError("Write failed to " + outputPath);
    };

    std::vector<size_t> offsets;  // byte offsets of each object

    // ── PDF header ────────────────────────────────────────────────────────
    writeStr("%PDF-1.4\n%\xE2\xE3\xCF\xD3\n");

    // ── Helper: record offset ─────────────────────────────────────────────
    auto recordOffset = [&]() {
        offsets.push_back(static_cast<size_t>(f.tellp()));
    };

    // Object numbering (N pages):
    //   1                 = Catalog
    //   2                 = Pages
    //   3       … 2+N     = Page dicts
    //   3+N     … 2+2N    = Image XObjects (Flate-compressed RGB)
    //   3+2N    … 2+3N    = Content streams (draw the image on the page)
    const uint32_t N            = static_cast<uint32_t>(pages.size());
    const uint32_t objFirstPage = 3;
    const uint32_t objFirstImg  = 3 + N;
    const uint32_t objFirstCont = 3 + 2 * N;
    const uint32_t totalObjs    = 2 + 3 * N;

    // ── Object 1: Catalog ─────────────────────────────────────────────────
    recordOffset();
    writeStr("1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n");

    // ── Object 2: Pages ───────────────────────────────────────────────────
    recordOffset();
    std::ostringstream pagesKids;
    pagesKids << "2 0 obj\n<< /Type /Pages /Count " << N << " /Kids [";
    for (uint32_t i = 0; i < N; ++i)
        pagesKids << (objFirstPage + i) << " 0 R ";
    pagesKids << "] >>\nendobj\n";
    writeStr(pagesKids.str());

    // ── Page dicts ────────────────────────────────────────────────────────
    for (uint32_t i = 0; i < N; ++i) {
        recordOffset();
        std::ostringstream page;
        page << (objFirstPage + i) << " 0 obj\n"
             << "<< /Type /Page\n"
             << "   /Parent 2 0 R\n"
             << "   /MediaBox [0 0 " << fmtFloat(PDF_PAGE_W_PT) << " "
                                     << fmtFloat(PDF_PAGE_H_PT) << "]\n"
             << "   /Resources << /XObject << /Img" << i << " "
                                     << (objFirstImg + i) << " 0 R >> >>\n"
             << "   /Contents " << (objFirstCont + i) << " 0 R\n"
             << ">>\n"
             << "endobj\n";
        writeStr(page.str());
    }

    // ── Image XObjects: raw RGB, zlib-compressed (FlateDecode) ───────────
    for (uint32_t i = 0; i < N; ++i) {
        recordOffset();
        const auto& pg = pages[i];
        size_t pixBytes = static_cast<size_t>(pg.width) * pg.height * 3;
        std::vector<uint8_t> z = zlibCompress(pg.pixels, pixBytes);

        std::ostringstream hdr;
        hdr << (objFirstImg + i) << " 0 obj\n"
            << "<< /Type /XObject\n"
            << "   /Subtype /Image\n"
            << "   /Width " << pg.width << "\n"
            << "   /Height " << pg.height << "\n"
            << "   /ColorSpace /DeviceRGB\n"
            << "   /BitsPerComponent 8\n"
            << "   /Filter /FlateDecode\n"
            << "   /Length " << z.size() << "\n"
            << ">>\n"
            << "stream\n";
        writeStr(hdr.str());
        f.write(reinterpret_cast<const char*>(z.data()), static_cast<std::streamsize>(z.size()));
        if (!f) throw PdfWriteError("Write failed to " + outputPath);
        writeStr("\nendstream\nendobj\n");
    }

    // ── Content streams: fit image inside the 10 mm margins, keep aspect
    //    ratio, centre on the page ─────────────────────────────────────────
    const float marginPt = 28.35f;   // 10 mm
    const float boxW = PDF_PAGE_W_PT - 2 * marginPt;
    const float boxH = PDF_PAGE_H_PT - 2 * marginPt;
    for (uint32_t i = 0; i < N; ++i) {
        recordOffset();
        const auto& pg = pages[i];
        float scale = std::min(boxW / static_cast<float>(pg.width),
                               boxH / static_cast<float>(pg.height));
        float drawW = pg.width  * scale;
        float drawH = pg.height * scale;
        float x = (PDF_PAGE_W_PT - drawW) / 2;
        float y = (PDF_PAGE_H_PT - drawH) / 2;

        std::string drawOps =
            "q\n" + fmtFloat(drawW) + " 0 0 " + fmtFloat(drawH) + " " +
            fmtFloat(x) + " " + fmtFloat(y) + " cm\n"
            "/Img" + std::to_string(i) + " Do\n"
            "Q\n";

        std::ostringstream cs;
        cs << (objFirstCont + i) << " 0 obj\n"
           << "<< /Length " << drawOps.size() << " >>\n"
           << "stream\n"
           << drawOps
           << "endstream\n"
           << "endobj\n";
        writeStr(cs.str());
    }

    // ── Cross-reference table ─────────────────────────────────────────────
    size_t xrefOffset = static_cast<size_t>(f.tellp());

    std::ostringstream xref;
    xref << "xref\n"
         << "0 " << (totalObjs + 1) << "\n"
         << "0000000000 65535 f \n";   // object 0 (free list head)
    for (size_t off : offsets) {
        xref << std::setfill('0') << std::setw(10) << off
             << " 00000 n \n";
    }
    writeStr(xref.str());

    // ── Trailer ───────────────────────────────────────────────────────────
    std::ostringstream trailer;
    trailer << "trailer\n"
            << "<< /Size " << (totalObjs + 1)
            << " /Root 1 0 R >>\n"
            << "startxref\n"
            << xrefOffset << "\n"
            << "%%EOF\n";
    writeStr(trailer.str());

    if (!f.good())
        throw PdfWriteError("File stream reported errors after writing " + outputPath);

    f.close();
}

// ═══════════════════════════════════════════════════════════════════════════
// PrinterSimulator
// ═══════════════════════════════════════════════════════════════════════════

PrinterSimulator::PrinterSimulator()  = default;
PrinterSimulator::~PrinterSimulator() = default;

void PrinterSimulator::abort() {
    abortFlag_.store(true, std::memory_order_release);
}

void PrinterSimulator::setThroughput(size_t bps) {
    throughput_ = bps;
}

void PrinterSimulator::simulateJob(
    const SpoolEntry&    job,
    JobProgressCallback  onProgress,
    std::function<void(const std::string&)> onLog)
{
    abortFlag_.store(false, std::memory_order_release);

    // Build the ESC/P2 byte stream (we don't transmit it, but we measure it
    // to simulate realistic timing)
    EscP2Encoder encoder(job.settings);

    auto log = [&](const std::string& msg) {
        if (onLog) onLog("[SIM:" + job.jobId + "] " + msg);
    };

    log("Initialising ESC/P2 job header…");
    auto header = encoder.buildJobHeader();
    size_t totalBytes = header.size();

    // Estimate: 4 channels × height scan lines, each ~(width/8) bytes
    uint32_t channels = (job.settings.colorMode == ColorMode::Color) ? 4u : 1u;
    size_t lineBytes  = static_cast<size_t>(job.width / 8 + 20) * channels;
    totalBytes       += lineBytes * job.height + 128;  // +footer

    log("Simulated job size: ~" + std::to_string(totalBytes / 1024) + " KB");
    log("Simulated speed:     " + std::to_string(throughput_ / 1024) + " KB/s");

    // ── Phase 1: job header ───────────────────────────────────────────────
    log("Sending job header (" + std::to_string(header.size()) + " bytes)…");
    simulateTransmit(header, job.jobId, nullptr, onLog);
    if (abortFlag_.load()) throw SimulationAborted(job.jobId);

    // ── Phase 2: page header ──────────────────────────────────────────────
    auto pageHdr = encoder.buildPageHeader(0);
    log("Sending page header…");
    simulateTransmit(pageHdr, job.jobId, nullptr, onLog);
    if (abortFlag_.load()) throw SimulationAborted(job.jobId);

    // ── Phase 3: scan lines ───────────────────────────────────────────────
    log("Printing " + std::to_string(job.height) + " scan lines × " +
        std::to_string(channels) + " channel(s)…");

    bool isColor = (job.settings.colorMode == ColorMode::Color);
    uint32_t bytesPerPx = isColor ? 3u : 1u;
    std::vector<uint8_t> planeC(job.width), planeM(job.width);
    std::vector<uint8_t> planeY(job.width), planeK(job.width);

    size_t bytesSent = header.size() + pageHdr.size();

    for (uint32_t row = 0; row < job.height; ++row) {
        if (abortFlag_.load()) throw SimulationAborted(job.jobId);

        const uint8_t* rowPx = job.pixels.data() + row * job.width * bytesPerPx;

        std::vector<uint8_t> rowBytes;
        if (isColor) {
            ColorProfile::separateLine(rowPx, job.width,
                planeC.data(), planeM.data(), planeY.data(), planeK.data());
            auto lK = encoder.encodeRasterLine(planeK.data(), job.width, 0);
            auto lC = encoder.encodeRasterLine(planeC.data(), job.width, 1);
            auto lM = encoder.encodeRasterLine(planeM.data(), job.width, 2);
            auto lY = encoder.encodeRasterLine(planeY.data(), job.width, 3);
            rowBytes.insert(rowBytes.end(), lK.begin(), lK.end());
            rowBytes.insert(rowBytes.end(), lC.begin(), lC.end());
            rowBytes.insert(rowBytes.end(), lM.begin(), lM.end());
            rowBytes.insert(rowBytes.end(), lY.begin(), lY.end());
        } else {
            auto lK = encoder.encodeRasterLine(rowPx, job.width, 0);
            rowBytes = std::move(lK);
        }

        // Simulate transmission time for this row
        size_t rowSz = rowBytes.size();
        bytesSent += rowSz;

        // Sleep proportional to data size / throughput
        double sleepSec = static_cast<double>(rowSz) / throughput_;
        if (sleepSec > 0.0) {
            auto us = static_cast<long long>(sleepSec * 1e6);
            std::this_thread::sleep_for(std::chrono::microseconds(us));
        }

        // Progress every 2% or on the last line
        uint32_t pct = static_cast<uint32_t>(
            static_cast<double>(row + 1) / job.height * 100.0);
        static uint32_t lastPct = 999;
        if (pct != lastPct || row == job.height - 1) {
            lastPct = pct;
            if (onProgress) onProgress(job.jobId, pct);
        }
    }

    // ── Phase 4: page footer + job footer ────────────────────────────────
    auto pageFtr = encoder.buildPageFooter();
    auto jobFtr  = encoder.buildJobFooter();
    simulateTransmit(pageFtr, job.jobId, nullptr, onLog);
    simulateTransmit(jobFtr,  job.jobId, nullptr, onLog);

    log("Job complete. Total simulated bytes: " +
        std::to_string(bytesSent / 1024) + " KB");
}

void PrinterSimulator::simulateTransmit(
    const std::vector<uint8_t>&             data,
    const std::string&                      jobId,
    JobProgressCallback                     /*onProgress*/,
    std::function<void(const std::string&)> /*onLog*/)
{
    if (data.empty()) return;
    double sleepSec = static_cast<double>(data.size()) / throughput_;
    if (sleepSec > 0.0) {
        auto us = static_cast<long long>(sleepSec * 1e6);
        std::this_thread::sleep_for(std::chrono::microseconds(us));
    }
    if (abortFlag_.load()) throw SimulationAborted(jobId);
}

// ═══════════════════════════════════════════════════════════════════════════
// PrintSpooler
// ═══════════════════════════════════════════════════════════════════════════

PrintSpooler::PrintSpooler(Mode mode, size_t capacity)
    : mode_(mode), capacity_(capacity) {}

PrintSpooler::~PrintSpooler() {
    stop();
    if (workerThread_.joinable())
        workerThread_.join();
}

void PrintSpooler::setPdfOutputDir(const std::string& dir) { pdfDir_ = dir; }
void PrintSpooler::setStatusCallback(JobStatusCallback cb)  { onStatus_   = std::move(cb); }
void PrintSpooler::setProgressCallback(JobProgressCallback cb) { onProgress_ = std::move(cb); }
void PrintSpooler::setLogCallback(std::function<void(const std::string&)> cb) {
    onLog_ = std::move(cb);
}

// ── enqueue ───────────────────────────────────────────────────────────────

std::string PrintSpooler::enqueue(
    const std::string&   label,
    JobPriority          priority,
    const PrintSettings& settings,
    const uint8_t*       pixels,
    uint32_t             width,
    uint32_t             height)
{
    std::lock_guard<std::mutex> lk(jobsMutex_);

    if (pq_.size() >= capacity_)
        throw QueueFull(capacity_);

    // Validate settings
    if (width == 0 || height == 0)
        throw InvalidSettings("Width and height must be non-zero.");
    if (settings.dpiX == 0 || settings.dpiY == 0)
        throw InvalidSettings("DPI values must be non-zero.");
    if (pixels == nullptr)
        throw InvalidSettings("Pixel data pointer is null.");

    std::string jobId = generateJobId();

    // Check for duplicate (shouldn't happen with timestamp-based IDs, but be safe)
    for (auto& j : allJobs_)
        if (j->jobId == jobId)
            throw DuplicateJobId(jobId);

    size_t bytesPerPx = (settings.colorMode == ColorMode::Color) ? 3u : 1u;
    size_t pixelBytes = static_cast<size_t>(width) * height * bytesPerPx;

    auto entry = std::make_shared<SpoolEntry>();
    entry->jobId      = jobId;
    entry->label      = label;
    entry->priority   = priority;
    entry->settings   = settings;
    entry->pixels     = std::vector<uint8_t>(pixels, pixels + pixelBytes);
    entry->width      = width;
    entry->height     = height;
    entry->status     = JobStatus::Queued;
    entry->enqueuedAt = std::chrono::steady_clock::now();

    pq_.push(entry);
    allJobs_.push_back(entry);

    log("Enqueued job [" + jobId + "] '" + label + "' priority=" +
        priorityName(priority) + " (" + std::to_string(pixelBytes/1024) + " KB)");

    return jobId;
}

// ── cancel ────────────────────────────────────────────────────────────────

void PrintSpooler::cancel(const std::string& jobId) {
    std::lock_guard<std::mutex> lk(jobsMutex_);
    for (auto& j : allJobs_) {
        if (j->jobId == jobId) {
            if (j->status == JobStatus::Queued) {
                j->status = JobStatus::Cancelled;
                log("Cancelled job [" + jobId + "]");
            }
            return;
        }
    }
    throw JobNotFound(jobId);
}

void PrintSpooler::cancelAll() {
    std::lock_guard<std::mutex> lk(jobsMutex_);
    for (auto& j : allJobs_)
        if (j->status == JobStatus::Queued)
            j->status = JobStatus::Cancelled;
    log("Cancelled all queued jobs.");
}

void PrintSpooler::abortCurrent() {
    simulator_.abort();
}

// ── processAll ───────────────────────────────────────────────────────────

void PrintSpooler::processAll() {
    log("Spooler started — mode: " +
        std::string(mode_ == Mode::Simulate ? "SIMULATION" : "SAVE PDF"));

    while (!stopRequested_.load()) {
        // Find the next non-cancelled pending job from the priority queue
        std::shared_ptr<SpoolEntry> job;
        {
            std::lock_guard<std::mutex> lk(jobsMutex_);

            // Rebuild pq without cancelled entries at the top
            while (!pq_.empty()) {
                auto top = pq_.top();
                if (top->status == JobStatus::Cancelled) {
                    pq_.pop();
                    continue;
                }
                if (top->status == JobStatus::Queued) {
                    job = top;
                    pq_.pop();
                    break;
                }
                pq_.pop();  // already Done/Failed
            }
        }

        if (!job) break;  // queue empty

        printQueueTable();
        processJob(job);
        printQueueTable();
    }

    log("Spooler finished.");
}

void PrintSpooler::startAsync() {
    stopRequested_.store(false);
    workerThread_ = std::thread([this]{ processAll(); });
}

void PrintSpooler::waitForAll() {
    if (workerThread_.joinable())
        workerThread_.join();
}

void PrintSpooler::stop() {
    stopRequested_.store(true);
    simulator_.abort();
}

// ── processJob ────────────────────────────────────────────────────────────

void PrintSpooler::processJob(std::shared_ptr<SpoolEntry> job) {
    notifyStatus(job, JobStatus::Processing, "Starting…");
    job->startedAt = std::chrono::steady_clock::now();

    try {
        if (mode_ == Mode::Simulate)
            processJobSimulate(job);
        else
            processJobPdf(job);

        job->finishedAt = std::chrono::steady_clock::now();
        notifyStatus(job, JobStatus::Done, "Completed successfully.");
    }
    catch (const SimulationAborted& e) {
        job->finishedAt  = std::chrono::steady_clock::now();
        job->errorMessage = e.what();
        notifyStatus(job, JobStatus::Cancelled, "Aborted: " + std::string(e.what()));
    }
    catch (const DriverException& e) {
        job->finishedAt   = std::chrono::steady_clock::now();
        job->errorMessage = e.what();
        notifyStatus(job, JobStatus::Failed, "Error: " + std::string(e.what()));
        std::cerr << ansi::RED << "[ERROR] Job [" << job->jobId << "] failed: "
                  << e.what() << ansi::RESET << '\n';
    }
    catch (const std::exception& e) {
        job->finishedAt   = std::chrono::steady_clock::now();
        job->errorMessage = e.what();
        notifyStatus(job, JobStatus::Failed,
                     "Unexpected error: " + std::string(e.what()));
        std::cerr << ansi::RED << "[ERROR] Unexpected error in job ["
                  << job->jobId << "]: " << e.what() << ansi::RESET << '\n';
    }
}

void PrintSpooler::processJobSimulate(std::shared_ptr<SpoolEntry> job) {
    auto progress = [this](const std::string& jid, uint32_t pct) {
        // Inline progress bar
        int filled = pct / 5;
        std::string bar = "[";
        for (int i = 0; i < 20; ++i)
            bar += (i < filled) ? '#' : '-';
        bar += "] " + std::to_string(pct) + "%";
        std::cout << "\r  " << ansi::CYAN << bar << ansi::RESET
                  << "  job=" << jid << "   " << std::flush;
        if (onProgress_) onProgress_(jid, pct);
    };

    auto logCb = [this](const std::string& msg) { log(msg); };

    simulator_.simulateJob(*job, progress, logCb);
    std::cout << '\n';  // end progress line
}

void PrintSpooler::processJobPdf(std::shared_ptr<SpoolEntry> job) {
    // Sanitise label into a valid filename
    std::string fname = job->label;
    for (char& c : fname)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_')
            c = '_';

    // Create the output folder if needed (e.g. when run without `make run`)
    std::error_code ec;
    std::filesystem::create_directories(pdfDir_, ec);
    if (ec) throw FileCreateFailed(pdfDir_);

    std::string path = pdfDir_ + "/" + fname + "_" + job->jobId + ".pdf";
    log("Saving PDF: " + path);

    bool isColor = (job->settings.colorMode == ColorMode::Color);
    if (!isColor) {
        // Convert greyscale to RGB for PDF export (PDF exporter expects RGB)
        size_t n = static_cast<size_t>(job->width) * job->height;
        std::vector<uint8_t> rgb(n * 3);
        for (size_t i = 0; i < n; ++i) {
            rgb[i * 3 + 0] = job->pixels[i];
            rgb[i * 3 + 1] = job->pixels[i];
            rgb[i * 3 + 2] = job->pixels[i];
        }
        PdfExporter::exportPage(path, rgb.data(), job->width, job->height, job->settings);
    } else {
        PdfExporter::exportPage(path, job->pixels.data(),
                                job->width, job->height, job->settings);
    }

    log("PDF saved: " + path);
    if (onProgress_) onProgress_(job->jobId, 100);
}

// ── introspection ─────────────────────────────────────────────────────────

size_t PrintSpooler::queueSize() const {
    std::lock_guard<std::mutex> lk(jobsMutex_);
    return pq_.size();
}

std::vector<std::shared_ptr<SpoolEntry>> PrintSpooler::allJobs() const {
    std::lock_guard<std::mutex> lk(jobsMutex_);
    return allJobs_;
}

std::shared_ptr<SpoolEntry> PrintSpooler::findJob(const std::string& jobId) const {
    std::lock_guard<std::mutex> lk(jobsMutex_);
    for (auto& j : allJobs_)
        if (j->jobId == jobId) return j;
    return nullptr;
}

void PrintSpooler::printQueueTable() const {
    std::lock_guard<std::mutex> lk(jobsMutex_);

    std::cout << "\n" << ansi::BOLD
              << "╔══════════════════════════════════════════════════════════╗\n"
              << "║              PRINT SPOOLER — QUEUE STATUS               ║\n"
              << "╚══════════════════════════════════════════════════════════╝"
              << ansi::RESET << "\n";

    std::cout << ansi::DIM
              << std::left
              << std::setw(14) << "Job ID"
              << std::setw(20) << "Label"
              << std::setw(10) << "Priority"
              << std::setw(12) << "Status"
              << std::setw(8)  << "Pages"
              << "\n"
              << std::string(64, '-') << ansi::RESET << "\n";

    for (const auto& j : allJobs_) {
        const char* color = ansi::WHITE;
        switch (j->status) {
            case JobStatus::Queued:     color = ansi::YELLOW; break;
            case JobStatus::Processing: color = ansi::CYAN;   break;
            case JobStatus::Done:       color = ansi::GREEN;  break;
            case JobStatus::Failed:     color = ansi::RED;    break;
            case JobStatus::Cancelled:  color = ansi::DIM;    break;
        }

        bool isCurrent = currentJob_ && currentJob_->jobId == j->jobId;
        std::cout << color
                  << std::left
                  << std::setw(14) << j->jobId.substr(0, 12)
                  << std::setw(20) << j->label.substr(0, 18)
                  << std::setw(10) << priorityName(j->priority)
                  << std::setw(12) << (isCurrent
                                       ? "▶ Processing"
                                       : statusName(j->status))
                  << std::setw(8)  << 1   // pages per job (always 1 here)
                  << ansi::RESET << "\n";

        // Show error inline
        if (!j->errorMessage.empty())
            std::cout << ansi::RED << "   ↳ " << j->errorMessage
                      << ansi::RESET << "\n";
    }
    std::cout << "\n";
}

// ── private helpers ───────────────────────────────────────────────────────

std::string PrintSpooler::generateJobId() {
    // Timestamp-based ID: "J-<epoch_ms>"
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    // Add incrementing suffix to avoid collisions within same millisecond
    static std::atomic<uint32_t> counter{0};
    return "J-" + std::to_string(ms) + "-" + std::to_string(counter.fetch_add(1));
}

void PrintSpooler::log(const std::string& msg) {
    if (onLog_) {
        onLog_(msg);
    } else {
        auto now = std::chrono::system_clock::now();
        auto t   = std::chrono::system_clock::to_time_t(now);
        std::cout << ansi::DIM << "[" << std::put_time(std::localtime(&t), "%H:%M:%S")
                  << "] " << msg << ansi::RESET << "\n";
    }
}

void PrintSpooler::notifyStatus(
    const std::shared_ptr<SpoolEntry>& job,
    JobStatus newStatus, const std::string& msg)
{
    job->status = newStatus;
    if (newStatus == JobStatus::Processing)
        currentJob_ = job;
    else if (currentJob_ && currentJob_->jobId == job->jobId)
        currentJob_ = nullptr;

    log("[" + job->jobId + "] → " + statusName(newStatus) +
        (msg.empty() ? "" : ": " + msg));

    if (onStatus_) onStatus_(job->jobId, newStatus, msg);
}

// ═══════════════════════════════════════════════════════════════════════════
// FallbackDispatcher
// ═══════════════════════════════════════════════════════════════════════════

std::unique_ptr<PrintSpooler> handlePrinterNotFound(const std::string& outputDir) {
    std::cout << "\n"
              << ansi::YELLOW << ansi::BOLD
              << "  ⚠  EPSON L130 not connected.\n"
              << ansi::RESET
              << "\n"
              << "  Choose an option:\n"
              << "    1) Save as PDF  — write each job to a PDF file in: " << outputDir << "\n"
              << "    2) Simulation   — emulate the printer in software with\n"
              << "                      a live priority queue\n"
              << "    q) Quit\n"
              << "\n"
              << "  Your choice: ";
    std::cout.flush();

    std::string choice;
    while (true) {
        if (!std::getline(std::cin, choice)) {   // EOF (Ctrl+D) or closed stdin
            std::cout << "\n  Input closed. Exiting.\n";
            return nullptr;
        }
        // Trim whitespace
        choice.erase(0, choice.find_first_not_of(" \t"));
        choice.erase(choice.find_last_not_of(" \t\r\n") + 1);

        if (choice == "1") {
            std::cout << ansi::GREEN << "  → PDF export mode selected.\n" << ansi::RESET;
            auto spooler = std::make_unique<PrintSpooler>(PrintSpooler::Mode::SavePdf);
            spooler->setPdfOutputDir(outputDir);
            return spooler;
        }
        if (choice == "2") {
            std::cout << ansi::CYAN << "  → Simulation mode selected.\n" << ansi::RESET;
            return std::make_unique<PrintSpooler>(PrintSpooler::Mode::Simulate);
        }
        if (choice == "q" || choice == "Q") {
            std::cout << "  Exiting.\n";
            return nullptr;
        }
        std::cout << "  Invalid choice. Enter 1, 2, or q: ";
    }
}

} // namespace epson_l130
