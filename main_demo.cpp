/**
 * main_demo.cpp — EPSON L130 Driver + Spooler Demo
 *
 * Flow:
 *   1. Try to connect to the real printer.
 *   2. If printer is found → print directly as before.
 *   3. If printer is NOT found → ask user: PDF or Simulation?
 *      Then present a multi-job submission interface where the user can
 *      queue several jobs with priorities before processing begins.
 *
 * Build and run:  make && ./epson_demo     (see README.md)
 */

#include "epson_l130_driver.hpp"
#include "print_spooler.hpp"
#include "driver_errors.hpp"
#include "image_loader.hpp"

#include <iostream>
#include <iomanip>
#include <string>
#include <vector>
#include <cmath>
#include <algorithm>
#include <csignal>
#include <atomic>

using namespace epson_l130;

// ═══════════════════════════════════════════════════════════════════════════
// Signal handler — Ctrl+C aborts the current simulation job gracefully
// ═══════════════════════════════════════════════════════════════════════════

static PrintSpooler* g_spooler = nullptr;
static std::atomic<bool> g_interrupted{false};

static void onSigInt(int) {
    g_interrupted.store(true);
    if (g_spooler) g_spooler->stop();
}

// ═══════════════════════════════════════════════════════════════════════════
// Image generators
// ═══════════════════════════════════════════════════════════════════════════

// Full-colour HSV sweep (RGB, 3 bytes/px)
static std::vector<uint8_t> makeColorTest(uint32_t w, uint32_t h) {
    std::vector<uint8_t> img(w * h * 3);
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            float hue = static_cast<float>(x) / w;
            float val = static_cast<float>(y) / h;
            float hh  = hue * 6.0f;
            int   hi  = static_cast<int>(hh);
            float f   = hh - hi;
            float p   = val * 0.0f;
            float q   = val * (1.0f - f);
            float t   = val * f;
            float r, g, b;
            switch (hi % 6) {
                case 0: r=val; g=t;   b=p;   break;
                case 1: r=q;   g=val; b=p;   break;
                case 2: r=p;   g=val; b=t;   break;
                case 3: r=p;   g=q;   b=val; break;
                case 4: r=t;   g=p;   b=val; break;
                default:r=val; g=p;   b=q;   break;
            }
            uint32_t i = (y * w + x) * 3;
            img[i+0] = static_cast<uint8_t>(r * 255.0f);
            img[i+1] = static_cast<uint8_t>(g * 255.0f);
            img[i+2] = static_cast<uint8_t>(b * 255.0f);
        }
    }
    return img;
}

// Greyscale gradient (1 byte/px)
static std::vector<uint8_t> makeGreyRamp(uint32_t w, uint32_t h) {
    std::vector<uint8_t> img(w * h);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x)
            img[y * w + x] = static_cast<uint8_t>(
                static_cast<float>(x) / w * 255.0f);
    return img;
}

// Solid colour block (RGB)
static std::vector<uint8_t> makeSolidColor(uint32_t w, uint32_t h,
                                            uint8_t r, uint8_t g, uint8_t b) {
    std::vector<uint8_t> img(w * h * 3);
    for (uint32_t i = 0; i < w * h; ++i) {
        img[i*3+0] = r;
        img[i*3+1] = g;
        img[i*3+2] = b;
    }
    return img;
}

// ═══════════════════════════════════════════════════════════════════════════
// Interactive multi-job queue builder
// ═══════════════════════════════════════════════════════════════════════════

static void printJobMenu() {
    std::cout <<
        "\n"
        "  ── Add a print job ──────────────────────────────────────\n"
        "  Choose job type:\n"
        "    1) Colour HSV sweep\n"
        "    2) Greyscale ramp\n"
        "    3) Solid red block\n"
        "    4) Solid blue block\n"
        "    5) Solid green block\n"
        "    6) Print a file  (PNG, JPG, BMP, GIF, PDF …)\n"
        "    d) Done — start processing queue\n"
        "    s) Show current queue\n"
        "    c) Cancel a job by ID\n"
        "    q) Quit without printing\n"
        "  Choice: ";
    std::cout.flush();
}

static JobPriority askPriority() {
    std::cout <<
        "  Priority:\n"
        "    1) Low\n"
        "    2) Normal  (default)\n"
        "    3) High\n"
        "    4) Critical\n"
        "  Choice [1-4]: ";
    std::cout.flush();

    std::string s;
    std::getline(std::cin, s);
    s.erase(0, s.find_first_not_of(" \t"));
    if (s == "1") return JobPriority::Low;
    if (s == "3") return JobPriority::High;
    if (s == "4") return JobPriority::Critical;
    return JobPriority::Normal;
}

static std::string askLabel(const std::string& defaultLabel) {
    std::cout << "  Job label [" << defaultLabel << "]: ";
    std::cout.flush();
    std::string s;
    std::getline(std::cin, s);
    s.erase(0, s.find_first_not_of(" \t"));
    s.erase(s.find_last_not_of(" \t\r\n") + 1);
    return s.empty() ? defaultLabel : s;
}

// ═══════════════════════════════════════════════════════════════════════════
// Choosing a file to print
// ═══════════════════════════════════════════════════════════════════════════

static std::string askLine(const std::string& prompt) {
    std::cout << prompt;
    std::cout.flush();
    std::string s;
    std::getline(std::cin, s);
    s.erase(0, s.find_first_not_of(" \t"));
    s.erase(s.find_last_not_of(" \t\r\n") + 1);
    return s;
}

static std::string baseName(const std::string& path) {
    std::string p = path;
    if (p.size() >= 2 && (p.front() == '\'' || p.front() == '"') && p.back() == p.front())
        p = p.substr(1, p.size() - 2);
    auto slash = p.find_last_of('/');
    return slash == std::string::npos ? p : p.substr(slash + 1);
}

// Parses "", "3" or "2-5" into (first, last); 0 means "no limit".
static bool parsePageRange(const std::string& s, uint32_t& first, uint32_t& last) {
    first = last = 0;
    if (s.empty()) return true;
    try {
        auto dash = s.find('-');
        if (dash == std::string::npos) {
            first = last = static_cast<uint32_t>(std::stoul(s));
        } else {
            std::string a = s.substr(0, dash), b = s.substr(dash + 1);
            if (!a.empty()) first = static_cast<uint32_t>(std::stoul(a));
            if (!b.empty()) last  = static_cast<uint32_t>(std::stoul(b));
        }
    } catch (const std::exception&) {
        return false;
    }
    return !(first == 0 && last == 0 && s != "-");
}

struct FileRequest {
    std::string             path;
    PrintSettings           settings;
    std::vector<RasterPage> pages;
    uint32_t                firstPage = 1;   // real page number of pages[0]
};

// Asks for a file, page range and colour mode, then loads it.
// Throws FileLoadError (a PrintError) with a readable reason on failure.
static FileRequest askForFile(const std::string& presetPath = "") {
    FileRequest req;
    if (presetPath.empty()) {
        std::cout << "\n  Tip: you can drag the file from the Files app into this window.\n";
        req.path = askLine("  File path: ");
    } else {
        req.path = presetPath;
    }
    req.path = resolvePrintablePath(req.path);   // fail fast on a bad path

    uint32_t first = 0, last = 0;
    req.firstPage = 1;
    if (isPdfPath(req.path)) {
        while (!parsePageRange(askLine("  Pages (e.g. 1-3 or 2; Enter = all): "), first, last))
            std::cout << "  Please type a page number or a range like 2-5.\n";
        if (first) req.firstPage = first;
    }

    std::string cm = askLine("  Colour or greyscale? [C/g]: ");
    req.settings.colorMode = (cm == "g" || cm == "G") ? ColorMode::Grayscale
                                                       : ColorMode::Color;
    req.settings.dpiX = req.settings.dpiY = DPI_NORMAL;

    std::cout << "  Loading …\n";
    req.pages = loadPrintableFile(req.path, req.settings, first, last);
    std::cout << "  ✓ Loaded " << req.pages.size()
              << (req.pages.size() == 1 ? " page" : " pages")
              << " (" << req.pages[0].width << "×" << req.pages[0].height
              << " px at " << req.settings.dpiX << " dpi, fitted to A4)\n";
    return req;
}

static void runInteractiveQueue(PrintSpooler& spooler) {
    // Use a small image size for simulation speed (300×200 pixels)
    // In a real driver you'd use the full DPI-based dimensions.
    constexpr uint32_t W = 300;
    constexpr uint32_t H = 200;

    PrintSettings colSettings;
    colSettings.colorMode = ColorMode::Color;
    colSettings.dpiX = colSettings.dpiY = DPI_NORMAL;

    PrintSettings greySettings;
    greySettings.colorMode = ColorMode::Grayscale;
    greySettings.dpiX = greySettings.dpiY = DPI_NORMAL;

    bool running = true;
    while (running && !g_interrupted.load()) {
        printJobMenu();
        std::string choice;
        if (!std::getline(std::cin, choice)) {   // EOF (Ctrl+D) or closed stdin
            std::cout << "\n  Input closed.\n";
            choice = spooler.queueSize() > 0 ? "d" : "q";
        }
        choice.erase(0, choice.find_first_not_of(" \t"));
        choice.erase(choice.find_last_not_of(" \t\r\n") + 1);

        try {
            if (choice == "1") {
                auto px  = makeColorTest(W, H);
                auto lbl = askLabel("ColourSweep");
                auto pri = askPriority();
                auto id  = spooler.enqueue(lbl, pri, colSettings, px.data(), W, H);
                std::cout << "  ✓ Queued as [" << id << "]\n";
            }
            else if (choice == "2") {
                auto px  = makeGreyRamp(W, H);
                auto lbl = askLabel("GreyRamp");
                auto pri = askPriority();
                auto id  = spooler.enqueue(lbl, pri, greySettings, px.data(), W, H);
                std::cout << "  ✓ Queued as [" << id << "]\n";
            }
            else if (choice == "3") {
                auto px  = makeSolidColor(W, H, 220, 50, 50);
                auto lbl = askLabel("SolidRed");
                auto pri = askPriority();
                auto id  = spooler.enqueue(lbl, pri, colSettings, px.data(), W, H);
                std::cout << "  ✓ Queued as [" << id << "]\n";
            }
            else if (choice == "4") {
                auto px  = makeSolidColor(W, H, 50, 50, 220);
                auto lbl = askLabel("SolidBlue");
                auto pri = askPriority();
                auto id  = spooler.enqueue(lbl, pri, colSettings, px.data(), W, H);
                std::cout << "  ✓ Queued as [" << id << "]\n";
            }
            else if (choice == "5") {
                auto px  = makeSolidColor(W, H, 50, 200, 50);
                auto lbl = askLabel("SolidGreen");
                auto pri = askPriority();
                auto id  = spooler.enqueue(lbl, pri, colSettings, px.data(), W, H);
                std::cout << "  ✓ Queued as [" << id << "]\n";
            }
            else if (choice == "6") {
                FileRequest req = askForFile();
                size_t freeSlots = PrintSpooler::DEFAULT_QUEUE_CAPACITY - spooler.queueSize();
                if (req.pages.size() > freeSlots)
                    throw QueueFull(PrintSpooler::DEFAULT_QUEUE_CAPACITY);

                auto lbl = askLabel(baseName(req.path));
                auto pri = askPriority();
                for (size_t p = 0; p < req.pages.size(); ++p) {
                    std::string pageLbl = req.pages.size() == 1
                        ? lbl : lbl + " p" + std::to_string(req.firstPage + p);
                    const auto& pg = req.pages[p];
                    auto id = spooler.enqueue(pageLbl, pri, req.settings,
                                              pg.pixels.data(), pg.width, pg.height);
                    std::cout << "  ✓ Queued as [" << id << "]  " << pageLbl << "\n";
                }
            }
            else if (choice == "s" || choice == "S") {
                spooler.printQueueTable();
            }
            else if (choice == "c" || choice == "C") {
                std::cout << "  Job ID to cancel: ";
                std::string id;
                std::getline(std::cin, id);
                id.erase(0, id.find_first_not_of(" \t"));
                id.erase(id.find_last_not_of(" \t\r\n") + 1);
                spooler.cancel(id);
                std::cout << "  ✓ Job [" << id << "] cancelled.\n";
            }
            else if (choice == "d" || choice == "D") {
                if (spooler.queueSize() == 0) {
                    std::cout << "  Queue is empty. Add at least one job first.\n";
                } else {
                    running = false;
                }
            }
            else if (choice == "q" || choice == "Q") {
                std::cout << "  Cancelling all jobs and quitting.\n";
                spooler.cancelAll();
                return;
            }
            else {
                std::cout << "  Unknown option.\n";
            }
        }
        // ── Per-operation error handling ──────────────────────────────────
        catch (const QueueFull& e) {
            std::cerr << "\n  ✖ Queue full: " << e.userMessage() << "\n";
        }
        catch (const DuplicateJobId& e) {
            std::cerr << "\n  ✖ Duplicate ID: " << e.userMessage() << "\n";
        }
        catch (const InvalidSettings& e) {
            std::cerr << "\n  ✖ Bad settings: " << e.userMessage() << "\n";
        }
        catch (const FileLoadError& e) {
            std::cerr << "\n  ✖ " << e.userMessage() << "\n";
        }
        catch (const JobNotFound& e) {
            std::cerr << "\n  ✖ Job not found: " << e.userMessage() << "\n";
        }
        catch (const SpoolerError& e) {
            std::cerr << "\n  ✖ Spooler error: " << e.what() << "\n";
        }
        catch (const DriverException& e) {
            std::cerr << "\n  ✖ Driver error: " << e.what() << "\n";
        }
    }

    std::cout << "\n  Processing queue ("
              << spooler.queueSize() << " jobs) — press Ctrl+C to abort.\n\n";
    spooler.processAll();
}

// ═══════════════════════════════════════════════════════════════════════════
// main
// ═══════════════════════════════════════════════════════════════════════════

int main() {
    std::signal(SIGINT, onSigInt);

    EpsonL130Driver driver;

    // ── Attempt connection ────────────────────────────────────────────────
    bool printerAvailable = false;
    try {
        std::cout << "Connecting to EPSON L130 …\n";
        driver.connect();
        printerAvailable = true;
        std::cout << "\033[32mConnected.\033[0m\n";
    }
    catch (const DeviceNotFound&) {
        std::cerr << "\033[33m[WARN] EPSON L130 not found on USB.\033[0m\n";
    }
    catch (const PermissionDenied& e) {
        std::cerr << "\033[31m[ERROR] " << e.userMessage() << "\033[0m\n";
        // Still allow fallback
    }
    catch (const DeviceBusy& e) {
        std::cerr << "\033[31m[ERROR] " << e.userMessage() << "\033[0m\n";
    }
    catch (const KernelDriverConflict& e) {
        std::cerr << "\033[31m[ERROR] " << e.userMessage() << "\033[0m\n";
    }
    catch (const ConnectionError& e) {
        std::cerr << "\033[31m[ERROR] USB connection failed: "
                  << e.what() << "\033[0m\n";
    }
    catch (const DriverException& e) {
        std::cerr << "\033[31m[ERROR] " << e.what() << "\033[0m\n";
    }

    // ── Real printer path ─────────────────────────────────────────────────
    if (printerAvailable) {
        try {
            std::string devId = driver.queryDeviceId();
            std::cout << "Device ID: " << devId << "\n";
            std::cout << "Status:    " << driver.queryStatus() << "\n";

            auto ink = driver.readInkLevels();
            const char* names[] = {"K","C","M","Y"};
            std::cout << "Ink:       ";
            for (int i = 0; i < 4; ++i)
                std::cout << names[i] << "=" << static_cast<int>(ink[i]) << "  ";
            std::cout << "\n\n";

            // ── Print a chosen file, or test pages if none given ──────────
            std::cout << "Tip: you can drag the file from the Files app into this window.\n";
            bool printedFile = false;
            while (!printedFile) {
                try {
                    std::string path = askLine("File to print (Enter = test pages): ");
                    if (path.empty()) break;
                    FileRequest req = askForFile(path);
                    for (size_t p = 0; p < req.pages.size(); ++p) {
                        const auto& pg = req.pages[p];
                        std::cout << "Printing page " << (req.firstPage + p) << " ("
                                  << (p + 1) << " of " << req.pages.size() << ") …\n";
                        driver.printRaster(pg.pixels.data(), pg.width, pg.height, req.settings,
                            [](uint32_t c, uint32_t t) {
                                std::cout << "\r  " << c * 100 / t << "%   " << std::flush;
                            });
                        std::cout << "\n  Done.\n";
                    }
                    printedFile = true;
                } catch (const FileLoadError& e) {
                    std::cerr << "\033[31m  ✖ " << e.userMessage()
                              << "\033[0m\n  Try again, or press Enter for test pages.\n";
                }
            }
            if (printedFile) { driver.disconnect(); return 0; }

            // ── Print two test pages ──────────────────────────────────────
            PrintSettings s;
            s.colorMode = ColorMode::Color;
            s.dpiX = s.dpiY = DPI_NORMAL;

            EscP2Encoder enc(s);
            uint32_t w = enc.dotsPerLineX();
            uint32_t h = enc.dotsPerLineY();

            std::cout << "Printing colour test page (" << w << "×" << h << ") …\n";
            auto colPx = makeColorTest(w, h);
            driver.printRaster(colPx.data(), w, h, s, [](uint32_t c, uint32_t t){
                std::cout << "\r  " << c * 100 / t << "%   " << std::flush;
            });
            std::cout << "\n  Done.\n";

            s.colorMode = ColorMode::Grayscale;
            s.dpiX = s.dpiY = DPI_HIGH;
            EscP2Encoder enc2(s);
            w = enc2.dotsPerLineX(); h = enc2.dotsPerLineY();

            std::cout << "Printing greyscale page (" << w << "×" << h << ") …\n";
            auto greyPx = makeGreyRamp(w, h);
            driver.printRaster(greyPx.data(), w, h, s, nullptr);
            std::cout << "  Done.\n";
        }
        catch (const PaperJam& e) {
            std::cerr << "\033[31m[HARDWARE] " << e.userMessage() << "\033[0m\n";
        }
        catch (const OutOfPaper& e) {
            std::cerr << "\033[31m[HARDWARE] " << e.userMessage() << "\033[0m\n";
        }
        catch (const InkLow& e) {
            std::cerr << "\033[33m[HARDWARE] " << e.userMessage() << "\033[0m\n";
        }
        catch (const PrinterOffline& e) {
            std::cerr << "\033[31m[HARDWARE] " << e.userMessage() << "\033[0m\n";
        }
        catch (const UsbTransferError& e) {
            std::cerr << "\033[31m[USB] " << e.what()
                      << " (sent " << e.transferred << "/" << e.attempted << " bytes)"
                      << "\033[0m\n";
        }
        catch (const EmptyJob& e) {
            std::cerr << "\033[31m[JOB] " << e.userMessage() << "\033[0m\n";
        }
        catch (const InvalidSettings& e) {
            std::cerr << "\033[31m[SETTINGS] " << e.userMessage() << "\033[0m\n";
        }
        catch (const PageTooLarge& e) {
            std::cerr << "\033[31m[PAGE] " << e.userMessage() << "\033[0m\n";
        }
        catch (const EncodingError& e) {
            std::cerr << "\033[31m[ENCODE] " << e.userMessage() << "\033[0m\n";
        }
        catch (const DriverException& e) {
            std::cerr << "\033[31m[DRIVER] " << e.what() << "\033[0m\n";
        }
        catch (const std::exception& e) {
            std::cerr << "\033[31m[UNEXPECTED] " << e.what() << "\033[0m\n";
        }

        driver.disconnect();
        return 0;
    }

    // ── Fallback path: PDF or Simulation ─────────────────────────────────
    auto spooler = handlePrinterNotFound("./pdf_output");
    if (!spooler) return 0;   // user chose quit

    g_spooler = spooler.get();

    // Register callbacks for rich status reporting
    spooler->setStatusCallback([](const std::string& id,
                                   JobStatus status,
                                   const std::string& msg) {
        const char* col = "\033[0m";
        switch (status) {
            case JobStatus::Processing: col = "\033[36m"; break;
            case JobStatus::Done:       col = "\033[32m"; break;
            case JobStatus::Failed:     col = "\033[31m"; break;
            case JobStatus::Cancelled:  col = "\033[33m"; break;
            default: break;
        }
        std::cout << col << "  [STATUS] " << id << " → "
                  << statusName(status)
                  << (msg.empty() ? "" : ": " + msg)
                  << "\033[0m\n";
    });

    spooler->setLogCallback([](const std::string& msg) {
        auto now = std::chrono::system_clock::now();
        auto t   = std::chrono::system_clock::to_time_t(now);
        std::cout << "\033[2m[" << std::put_time(std::localtime(&t), "%H:%M:%S")
                  << "] " << msg << "\033[0m\n";
    });

    // Interactive job submission
    runInteractiveQueue(*spooler);

    // Summary
    std::cout << "\n\033[1m── Job Summary ──────────────────────────────────\033[0m\n";
    for (auto& j : spooler->allJobs()) {
        const char* col = "\033[37m";
        if (j->status == JobStatus::Done)      col = "\033[32m";
        if (j->status == JobStatus::Failed)    col = "\033[31m";
        if (j->status == JobStatus::Cancelled) col = "\033[33m";

        auto dur = std::chrono::duration_cast<std::chrono::milliseconds>(
                       j->finishedAt - j->startedAt).count();

        std::cout << col
                  << "  [" << j->jobId.substr(0, 12) << "] "
                  << std::left << std::setw(20) << j->label
                  << std::setw(10) << statusName(j->status);
        if (j->status == JobStatus::Done)
            std::cout << "  " << dur << " ms";
        if (!j->errorMessage.empty())
            std::cout << "\n    ↳ " << j->errorMessage;
        std::cout << "\033[0m\n";
    }

    g_spooler = nullptr;
    return g_interrupted.load() ? 130 : 0;  // 130 = killed by Ctrl+C
}
