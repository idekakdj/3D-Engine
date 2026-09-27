// aether-cook — batch importer/cooker for a content directory.
//
//   aether-cook <content_dir> <cooked_dir> [--force] [--compress | --no-compress] [--no-mips] [-j N]
//
// Imports every supported source (glTF/GLB, images) under <content_dir> in parallel on the
// JobSystem, writes one .aeasset per sub-asset plus <cooked_dir>/asset_db.json, skips
// sources whose stamps (size + mtime, else content hash), import settings and importer
// version are unchanged, prunes records of deleted sources, and prints a summary.
// Textures (ADR-0009): by default every texture gets a full CPU mip chain and is block-
// compressed (BC7 colour/data, BC5 normal maps) - ImportSettings::cooking(). --no-compress
// keeps RGBA8 with mips; --no-mips gives the editor-mode import (RGBA8, mip 0 only).
// Exit codes: 0 success, 1 any import failed, 2 usage error.
#include "aether/assets/asset_database.h"
#include "aether/assets/importers.h"
#include "aether/core/job_system.h"
#include "aether/core/time.h"

#include <charconv>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace aether;

namespace {

void print_usage() {
    std::fputs("usage: aether-cook <content_dir> <cooked_dir> [--force] [--compress|--no-compress] [--no-mips] [-j N]\n"
               "  --force        re-import every source even if it is up to date\n"
               "  --compress     BC7 (colour/data) + BC5 (normal maps) textures with full mip chains (default)\n"
               "  --no-compress  RGBA8 textures with full mip chains\n"
               "  --no-mips      RGBA8 textures, mip 0 only (editor-mode import; implies --no-compress)\n"
               "  -j N           worker threads (default: hardware threads - 1)\n",
               stderr);
}

std::string narrow(const fs::path& p) {
    const std::u8string s = p.u8string();
    return std::string(reinterpret_cast<const char*>(s.data()), s.size());
}

int run(const std::vector<fs::path>& args) {
    std::vector<fs::path> positional;
    bool                  force = false;
    bool                  compress = true;
    bool                  mips = true;
    u32                   jobs = 0;
    for (usize i = 0; i < args.size(); ++i) {
        const std::string a = narrow(args[i]);
        if (a == "--force") {
            force = true;
        } else if (a == "--compress") {
            compress = true;
        } else if (a == "--no-compress") {
            compress = false;
        } else if (a == "--no-mips") {
            mips = false;
        } else if (a == "-j" || a == "--jobs") {
            if (i + 1 >= args.size()) {
                print_usage();
                return 2;
            }
            const std::string n = narrow(args[++i]);
            const auto [ptr, ec] = std::from_chars(n.data(), n.data() + n.size(), jobs);
            if (ec != std::errc{} || ptr != n.data() + n.size()) {
                std::fprintf(stderr, "aether-cook: bad job count '%s'\n", n.c_str());
                return 2;
            }
        } else if (a.starts_with("-j") && a.size() > 2) {
            const auto [ptr, ec] = std::from_chars(a.data() + 2, a.data() + a.size(), jobs);
            if (ec != std::errc{} || ptr != a.data() + a.size()) {
                std::fprintf(stderr, "aether-cook: bad job count '%s'\n", a.c_str());
                return 2;
            }
        } else if (a == "-h" || a == "--help") {
            print_usage();
            return 0;
        } else if (!a.empty() && a[0] == '-') {
            std::fprintf(stderr, "aether-cook: unknown option '%s'\n", a.c_str());
            print_usage();
            return 2;
        } else {
            positional.push_back(args[i]);
        }
    }
    if (positional.size() != 2) {
        print_usage();
        return 2;
    }
    std::error_code ec;
    if (!fs::is_directory(positional[0], ec)) {
        std::fprintf(stderr, "aether-cook: content directory not found: %s\n", narrow(positional[0]).c_str());
        return 2;
    }

    const f64 start = now_seconds();
    JobSystem::initialize(jobs);
    const u32 workers = JobSystem::worker_count();

    assets::ImportSettings settings = assets::ImportSettings::cooking();
    settings.generate_mips = mips;
    settings.compress_textures = mips && compress;

    assets::AssetDatabase db;
    if (auto r = db.open(positional[0], positional[1], settings); !r) {
        std::fprintf(stderr, "aether-cook: %s\n", r.error().message.c_str());
        JobSystem::shutdown();
        return 1;
    }
    assets::ScanOptions options;
    options.force = force;
    options.parallel = true;
    const assets::ScanReport report = db.scan(options);

    for (const assets::ImportOutcome& oc : report.outcomes) {
        switch (oc.status) {
        case assets::ImportStatus::Imported:
            std::printf("  cooked    %s (%u assets)\n", oc.source_path.c_str(), oc.assets_written);
            for (const std::string& w : oc.warnings) std::printf("    warning: %s\n", w.c_str());
            break;
        case assets::ImportStatus::UpToDate: break;
        case assets::ImportStatus::Failed:
            std::fprintf(stderr, "  FAILED    %s: %s\n", oc.source_path.c_str(), oc.error.message.c_str());
            break;
        }
    }
    int exit_code = report.failed > 0 ? 1 : 0;
    if (auto r = db.save(); !r) {
        std::fprintf(stderr, "aether-cook: cannot write asset database: %s\n", r.error().message.c_str());
        exit_code = 1;
    }
    JobSystem::shutdown();

    std::printf("aether-cook: %u sources: %u cooked, %u up to date (%u hashed), %u failed, %u removed; %u assets "
                "written, %zu in database; textures: %s (%.2f s, %u workers)\n",
                report.sources_found, report.imported, report.up_to_date, report.hashed, report.failed,
                report.removed, report.assets_written, db.asset_count(),
                settings.compress_textures ? "BC7/BC5 + mips" : (settings.generate_mips ? "RGBA8 + mips" : "RGBA8"),
                now_seconds() - start, workers);
    return exit_code;
}

} // namespace

#if defined(_WIN32)
int wmain(int argc, wchar_t** argv) {
    std::vector<fs::path> args;
    for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
    return run(args);
}
#else
int main(int argc, char** argv) {
    std::vector<fs::path> args;
    for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
    return run(args);
}
#endif
