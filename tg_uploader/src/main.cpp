#include "config.h"
#include "td_client.h"
#include "uploader.h"
#include "scanner.h"
#include "folder_lock.h"
#include "utils.h"
#include "video_info.h"
#include "bot_uploader.h"
#include "web_server.h"
#include "user_auth.h"
#include <csignal>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <unistd.h>

namespace fs = std::filesystem;

// Global state for signal handler cleanup (also accessed from uploader.cpp via extern)
std::string g_locked_folder;

static void signal_handler(int sig) {
    if (!g_locked_folder.empty()) {
        remove_folder_lock(g_locked_folder);
        g_locked_folder.clear();
    }
    // Re-raise with default handler to get proper exit code
    std::signal(sig, SIG_DFL);
    std::raise(sig);
}

static void print_usage() {
    std::cout << "Usage:\n"
              << "  tg_uploader upload     --account NAME [--dir PATH] [--concurrent N] [--recursive] [--no-recursive]\n"
              << "  tg_uploader bot-upload [--dir PATH] [--recursive] [--no-recursive]\n"
              << "  tg_uploader fix-big    [--dir PATH] [--recursive] [--no-recursive]\n"
              << "  tg_uploader scan       --account NAME [--full] [--resume]\n"
              << "  tg_uploader fix-thumbs --account NAME [--force] [--range N]\n"
              << "  tg_uploader login      --account NAME\n"
              << "  tg_uploader accounts\n"
              << "  tg_uploader web-serve  [--port 8088]\n"
              << "\nbot-upload requires .account_configs/bots.json and a running telegram-bot-api server.\n"
              << "web-serve starts an embedded HTTP control panel (reads JWT_SECRET env).\n";
}

static std::string get_arg(int argc, char* argv[], const std::string& flag, const std::string& def = "") {
    for (int i = 1; i < argc - 1; i++) {
        if (flag == argv[i]) return argv[i + 1];
    }
    return def;
}

static bool has_flag(int argc, char* argv[], const std::string& flag) {
    for (int i = 1; i < argc; i++) {
        if (flag == argv[i]) return true;
    }
    return false;
}

static void cmd_accounts() {
    auto accounts = list_accounts();
    if (accounts.empty()) {
        std::cout << "No accounts configured. Edit .account_configs/accounts.json\n";
        return;
    }

    std::printf("%-10s %-18s %-20s %s\n", "Name", "Phone", "Channel", "TDLib DB");
    std::cout << std::string(68, '-') << "\n";
    for (const auto& acc : accounts) {
        std::string db_status = fs::is_directory(acc.session_dir) ? "OK" : "NEEDS LOGIN";
        std::printf("%-10s %-18s %-20s %s\n",
                    acc.name.c_str(), acc.phone.c_str(),
                    acc.channel_id.c_str(), db_status.c_str());
    }
}

static void cmd_login(const std::string& account_name) {
    auto config = load_account(account_name);
    std::cout << "Logging in account: " << config.name
              << " (" << config.phone << ")\n";

    TdClient client(config);
    if (client.login()) {
        std::cout << "Login successful for " << config.name << "\n";
    } else {
        std::cerr << "Login failed for " << config.name << "\n";
    }
    client.close();
}

static void cmd_scan(const std::string& account_name, bool resume, bool full) {
    scan_account_channel(account_name, resume, full);
}

static void cmd_fix_thumbs(const std::string& account_name, bool force, int range) {
    auto config = load_account(account_name);
    std::cout << "[" << config.name << "] Connecting for thumbnail fix...\n";

    TdClient client(config);
    if (!client.login()) {
        std::cerr << "Error: not authorized for '" << config.name << "'. Run:\n"
                  << "  tg_uploader login --account " << config.name << "\n";
        return;
    }

    int64_t chat_id = client.find_channel(config.channel_id);
    if (chat_id == 0) {
        std::cerr << "Error: channel not found: " << config.channel_id << "\n";
        client.close();
        return;
    }

    std::string base = get_base_dir();
    std::string db_path = base + "/data/scanner.db";
    std::string thumb_dir = base + "/data/thumbs";

    fs::create_directories(base + "/data/thumbs");

    ChannelScanner scanner(client, db_path, thumb_dir);
    int fixed = scanner.fix_thumbs(chat_id, force, range);

    std::cout << "\033[35m[FIX-THUMB]\033[0m Fixed " << fixed << " thumbnails.\n";
    client.close();
}

// BIG_THRESHOLD must match BOT_FILE_SIZE_LIMIT in bot_uploader.cpp (and the
// matching limit in uploader.cpp): 4000 parts × 512 KB = ~1.953 GiB
static constexpr int64_t BIG_THRESHOLD = 4000LL * 512 * 1024;
static constexpr int64_t MIN_REMAINDER_SIZE = 10LL * 1024 * 1024; // 10 MB

// Generate a ctbrec-style 10×9 contact sheet jpg with timestamped tiles.
// Outer -vf uses double quotes so the text='%{pts\:hms}' single quotes pass
// through to ffmpeg verbatim — ffmpeg 5.x's drawtext rejects an unquoted
// %{...} expansion as ambiguous between text and textfile.
static bool make_contact_sheet(const std::string& video_path, const std::string& out_jpg) {
    VideoInfo info = get_video_info(video_path);
    constexpr int cs_cols = 10, cs_rows = 9;
    constexpr int cs_frames = cs_cols * cs_rows;
    double cs_interval = (info.duration > 0)
        ? info.duration / (cs_frames + 1) : 5.0;
    constexpr int tile_w = 3840 / cs_cols; // 384 px per tile

    std::string cmd = "ffmpeg -y -loglevel error -i "
        + shell_escape(video_path)
        + " -vf \"fps=1/" + std::to_string(cs_interval)
        + ",scale=" + std::to_string(tile_w) + ":-1"
        + ",drawtext=fontfile=/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
        + ":text='%{pts\\:hms}':fontcolor=white:fontsize=14"
        + ":box=1:boxcolor=black@0.5:boxborderw=3:x=5:y=h-th-5"
        + ",tile=" + std::to_string(cs_cols) + "x" + std::to_string(cs_rows)
        + ":padding=1:color=0x333333\""
        + " -frames:v 1 -q:v 2 " + shell_escape(out_jpg) + " 2>/dev/null";
    int rc = cancellable_system(cmd, g_cancel_fixbig);
    return rc == 0 && fs::exists(out_jpg);
}

static void cmd_fix_big(const std::string& dir_override, bool recursive) {
    auto settings = load_settings();

    std::string upload_dir = dir_override.empty() ? settings.default_upload_dir : dir_override;
    if (upload_dir.empty() || !fs::is_directory(upload_dir)) {
        std::cerr << "Error: directory not found: " << upload_dir << "\n";
        return;
    }

    // 1. Scan for .big files
    std::vector<std::string> big_files;
    try {
        if (recursive) {
            for (auto& entry : fs::recursive_directory_iterator(
                     upload_dir, fs::directory_options::skip_permission_denied)) {
                if (entry.is_regular_file() && entry.path().extension() == ".big") {
                    big_files.push_back(entry.path().string());
                }
            }
        } else {
            for (auto& entry : fs::directory_iterator(upload_dir)) {
                if (entry.is_regular_file() && entry.path().extension() == ".big") {
                    big_files.push_back(entry.path().string());
                }
            }
        }
    } catch (const fs::filesystem_error& e) {
        std::cerr << "\033[31m[ERROR]\033[0m Directory scan failed: " << e.what() << "\n";
        return;
    }

    std::sort(big_files.begin(), big_files.end());

    if (big_files.empty()) {
        std::cout << "\033[36m[FIX-BIG]\033[0m No .big files found in " << upload_dir << "\n";
        return;
    }

    std::cout << "\033[36m[FIX-BIG]\033[0m Found " << big_files.size() << " oversized files\n";

    int fixed = 0, failed = 0, remainders = 0;

    for (size_t i = 0; i < big_files.size(); i++) {
        if (g_cancel_fixbig.load()) { std::cout << "\n[FIX-BIG] Canceled, stopping.\n"; break; }
        const std::string& big_path = big_files[i];
        fs::path big_p(big_path);
        fs::path parent_dir = big_p.parent_path();

        // Derive original video path: xxx.mp4.big → xxx.mp4
        std::string original_name = big_p.stem().string(); // "xxx.mp4"
        fs::path original_path = parent_dir / original_name;

        std::cout << "\n\033[36m[FIX-BIG]\033[0m [" << (i + 1) << "/" << big_files.size() << "] "
                  << big_p.filename().string();

        // The .big file can be one of two things:
        //   - Bot uploader (current): empty marker, original video sits at original_path
        //   - TDLib uploader (legacy): the renamed video itself
        // Pick whichever actually has the video content.
        int64_t big_size = static_cast<int64_t>(fs::file_size(big_path));
        std::string source_video;
        if (big_size == 0) {
            if (!fs::exists(original_path)) {
                std::cerr << "\n\033[31m[ERROR]\033[0m " << big_p.filename().string()
                          << " is an empty marker but " << original_name
                          << " is missing, skipping\n";
                failed++;
                continue;
            }
            source_video = original_path.string();
            std::cout << " (marker)";
        } else {
            source_video = big_path;
        }

        // Get file size and duration from the actual video
        int64_t file_size = static_cast<int64_t>(fs::file_size(source_video));
        VideoInfo info = get_video_info(source_video);

        if (info.duration <= 0) {
            std::cerr << "\n\033[31m[ERROR]\033[0m Cannot get duration for " << big_p.filename().string()
                      << ", skipping\n";
            failed++;
            continue;
        }

        // Tell bot-upload to skip every file we'll touch during this iteration.
        // part2 path computed up front so the guard covers it from the start.
        fs::path orig_stem_for_lock = fs::path(original_name).stem();
        std::string part2_path_str = (parent_dir
            / (orig_stem_for_lock.string() + "_part2.mp4")).string();
        ActivePathGuard guard({source_video, big_path, original_path.string(), part2_path_str});

        std::cout << ": " << format_file_size(file_size) << ", "
                  << std::fixed << std::setprecision(1) << info.duration << "s";

        // 2. Calculate trim duration
        double bitrate = static_cast<double>(file_size) / info.duration; // bytes/sec
        double ratio = 0.95;
        std::string tmp_part1;
        bool trim_ok = false;
        double final_trim_duration = 0;

        for (int attempt = 0; attempt < 3; attempt++) {
            double target_size = static_cast<double>(BIG_THRESHOLD) * ratio;
            double trim_duration = target_size / bitrate;

            std::cout << " → trim to " << std::fixed << std::setprecision(1) << trim_duration << "s\n";
            std::cout << "\033[36m[FIX-BIG]\033[0m Trimming...\n";

            // Build tmp path in same directory (avoids cross-device rename and /tmp space issues)
            tmp_part1 = (parent_dir / (".tg_fixbig_" + std::to_string(::getpid()) + "_" + original_name)).string();

            std::string cmd = "ffmpeg -y -loglevel error -i "
                              + shell_escape(source_video) + " -t "
                              + std::to_string(trim_duration)
                              + " -c copy " + shell_escape(tmp_part1) + " 2>&1";
            int rc = cancellable_system(cmd, g_cancel_fixbig);
            if (g_cancel_fixbig.load()) {
                std::cerr << "\033[33m[FIX-BIG]\033[0m Trim canceled\n";
                try { if (fs::exists(tmp_part1)) fs::remove(tmp_part1); } catch (...) {}
                break;
            }
            if (rc != 0 || !fs::exists(tmp_part1)) {
                std::cerr << "\033[31m[ERROR]\033[0m ffmpeg trim failed\n";
                break;
            }

            int64_t part1_size = static_cast<int64_t>(fs::file_size(tmp_part1));
            if (part1_size <= BIG_THRESHOLD) {
                std::cout << "\033[36m[FIX-BIG]\033[0m Part1: " << format_file_size(part1_size) << " (ok)";
                trim_ok = true;
                final_trim_duration = trim_duration;
                break;
            }

            // Part1 still too big — reduce ratio and retry
            std::cerr << "\033[33m[WARNING]\033[0m Part1 still too big: "
                      << format_file_size(part1_size) << ", retrying with lower ratio\n";
            try { fs::remove(tmp_part1); } catch (...) {}
            ratio -= 0.05;
        }

        if (!trim_ok) {
            std::cerr << "\033[31m[ERROR]\033[0m Could not trim " << big_p.filename().string()
                      << " to fit, skipping\n";
            try { if (!tmp_part1.empty()) fs::remove(tmp_part1); } catch (...) {}
            failed++;
            continue;
        }

        // 3. Extract remainder (part2)
        fs::path orig_stem = fs::path(original_name).stem();   // "xxx"
        std::string part2_name = orig_stem.string() + "_part2.mp4";
        fs::path part2_path = parent_dir / part2_name;

        // -ss BEFORE -i for fast input seek (keyframe-accurate with -c copy)
        std::string cmd2 = "ffmpeg -y -loglevel error -ss "
                           + std::to_string(final_trim_duration)
                           + " -i " + shell_escape(source_video)
                           + " -c copy " + shell_escape(part2_path.string()) + " 2>&1";
        int rc2 = cancellable_system(cmd2, g_cancel_fixbig);

        if (rc2 == 0 && fs::exists(part2_path)) {
            int64_t part2_size = static_cast<int64_t>(fs::file_size(part2_path));
            if (part2_size > MIN_REMAINDER_SIZE) {
                std::cout << ", Part2: " << format_file_size(part2_size)
                          << " → saved as " << part2_name << "\n";
                remainders++;

                make_contact_sheet(part2_path.string(),
                                   (parent_dir / (orig_stem.string() + "_part2.jpg")).string());
            } else {
                std::cout << ", Part2: " << format_file_size(part2_size) << " (too small, deleted)\n";
                try { fs::remove(part2_path); } catch (...) {}
            }
        } else {
            std::cout << ", Part2: extraction failed (ignored)\n";
        }

        // 4. Replace original video with trimmed part1, then retire the .big to .big.done
        //    Rename overwrites if original_path exists (marker mode — original is the source).
        //    Replace first so .big is kept if rename fails (no data loss). Then move .big
        //    out of the way so the next fix-big run doesn't pick it up, but leave it on
        //    disk so the user can review/delete it manually.
        try {
            fs::rename(tmp_part1, original_path);
            std::string done_marker = big_path + ".done";
            try {
                fs::rename(big_path, done_marker);
            } catch (const std::exception& e) {
                std::cerr << "\033[33m[WARNING]\033[0m Could not retire " << big_p.filename().string()
                          << " to .big.done: " << e.what() << "\n";
            }
            std::cout << "\033[36m[FIX-BIG]\033[0m Saved: " << original_path.filename().string()
                      << " (old marker → " << fs::path(done_marker).filename().string() << ")\n";

            // The original .jpg (if any) is a contact sheet of the full uncut
            // video — it shows content that now lives in part2. Regenerate it
            // to match the trimmed part1.
            fs::path part1_jpg = parent_dir / (orig_stem.string() + ".jpg");
            if (fs::exists(part1_jpg)) {
                if (make_contact_sheet(original_path.string(), part1_jpg.string())) {
                    std::cout << "\033[36m[FIX-BIG]\033[0m Regenerated sheet: "
                              << part1_jpg.filename().string() << "\n";
                } else {
                    std::cerr << "\033[33m[WARNING]\033[0m Sheet regen failed for "
                              << part1_jpg.filename().string() << "\n";
                }
            }
            fixed++;
        } catch (const std::exception& e) {
            std::cerr << "\033[31m[ERROR]\033[0m Replace failed: " << e.what() << "\n";
            try { fs::remove(tmp_part1); } catch (...) {}
            failed++;
        }
    }

    std::cout << "\n\033[36m[FIX-BIG]\033[0m Done. " << fixed << " fixed, "
              << failed << " failed, " << remainders << " remainders saved\n";
}

static void cmd_bot_upload(const std::string& dir_override, bool recursive) {
    BotConfig bot_config;
    try {
        bot_config = load_bot_config();
    } catch (const std::exception& e) {
        std::cerr << "Error loading bot config: " << e.what() << "\n";
        return;
    }

    auto settings = load_settings();
    std::string upload_dir = dir_override.empty() ? settings.default_upload_dir : dir_override;
    if (upload_dir.empty() || !fs::is_directory(upload_dir)) {
        std::cerr << "Error: directory not found: "
                  << (upload_dir.empty() ? "(none — set DEFAULT_UPLOAD_DIR in .env or use --dir)" : upload_dir)
                  << "\n";
        return;
    }

    BotUploader uploader(bot_config, settings);
    uploader.upload_directory(upload_dir, recursive);
}

static void cmd_upload(const std::string& account_name, const std::string& dir_override,
                        bool recursive, int concurrent) {
    auto config = load_account(account_name);
    auto settings = load_settings();

    std::string upload_dir = dir_override.empty() ? settings.default_upload_dir : dir_override;
    if (upload_dir.empty() || !fs::is_directory(upload_dir)) {
        std::cerr << "Error: directory not found: " << upload_dir << "\n";
        return;
    }

    std::cout << "[" << config.name << "] Connecting...\n";

    TdClient client(config);
    if (!client.login()) {
        std::cerr << "Error: not authorized for '" << config.name << "'. Run:\n"
                  << "  tg_uploader login --account " << config.name << "\n";
        return;
    }

    std::cout << "[" << config.name << "] Authorized. Finding channel: "
              << config.channel_id << "\n";

    int64_t chat_id = client.find_channel(config.channel_id);
    if (chat_id == 0) {
        std::cerr << "Error: channel not found: " << config.channel_id << "\n";
        client.close();
        return;
    }

    Uploader uploader(client, config, settings, chat_id, concurrent);
    uploader.upload_directory(upload_dir, recursive);

    client.close();
}

// Non-static wrappers exposed to web_server.cpp (declared in web_server.cpp's forward decls).
// Web server calls these from a worker thread; they share the same g_cancel flag.
void run_fix_big_task(const std::string& dir, bool recursive) {
    cmd_fix_big(dir, recursive);
}
void run_bot_upload_task(const std::string& dir, bool recursive) {
    cmd_bot_upload(dir, recursive);
}
// Bulk: run bot-upload across a list of dirs sequentially. Stops cleanly if
// the per-job cancel flag is set between dirs.
void run_bot_upload_multi(const std::vector<std::string>& dirs, bool recursive) {
    for (size_t i = 0; i < dirs.size(); i++) {
        if (g_cancel_botupload.load()) {
            std::cout << "[INFO] Bulk upload canceled at " << (i+1) << "/"
                      << dirs.size() << "\n";
            break;
        }
        std::cout << "\n[INFO] === Folder " << (i+1) << "/" << dirs.size()
                  << " · " << dirs[i] << " ===\n";
        cmd_bot_upload(dirs[i], recursive);
    }
    std::cout << "[INFO] Bulk upload complete (" << dirs.size() << " folder(s) processed)\n";
}
void run_fix_big_multi(const std::vector<std::string>& dirs, bool recursive) {
    for (size_t i = 0; i < dirs.size(); i++) {
        if (g_cancel_fixbig.load()) {
            std::cout << "[FIX-BIG] Bulk fix-big canceled at " << (i+1) << "/"
                      << dirs.size() << "\n";
            break;
        }
        std::cout << "\n[FIX-BIG] === Folder " << (i+1) << "/" << dirs.size()
                  << " · " << dirs[i] << " ===\n";
        cmd_fix_big(dirs[i], recursive);
    }
    std::cout << "[FIX-BIG] Bulk complete (" << dirs.size() << " folder(s) processed)\n";
}

static void cmd_web_serve(int port) {
    if (!user_auth_init()) {
        std::cerr << "[WEB] Warning: users.json missing — login will fail until you create one.\n";
    }
    std::string static_dir = get_base_dir() + "/web/static";
    run_web_server(port, static_dir);
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    // Register signal handlers for cleanup
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // Set base dir to where .account_configs/ and .env live
    // Priority: CTG_BASE_DIR env var > exe dir (if has .account_configs) > cwd
    const char* env_base = std::getenv("CTG_BASE_DIR");
    if (env_base && fs::is_directory(std::string(env_base))) {
        set_base_dir(std::string(env_base));
    } else {
        std::string exe_path = fs::canonical(fs::path(argv[0])).parent_path().string();
        if (fs::is_directory(exe_path + "/.account_configs")) {
            set_base_dir(exe_path);
        } else {
            set_base_dir(fs::current_path().string());
        }
    }

    std::string command = argv[1];

    if (command == "accounts") {
        cmd_accounts();
    } else if (command == "login") {
        std::string account = get_arg(argc, argv, "--account");
        if (account.empty()) account = get_arg(argc, argv, "-a");
        if (account.empty()) {
            std::cerr << "Error: --account required\n";
            return 1;
        }
        cmd_login(account);
    } else if (command == "scan") {
        std::string account = get_arg(argc, argv, "--account");
        if (account.empty()) account = get_arg(argc, argv, "-a");
        if (account.empty()) {
            std::cerr << "Error: --account required\n";
            return 1;
        }
        bool resume = has_flag(argc, argv, "--resume");
        bool full = has_flag(argc, argv, "--full");
        cmd_scan(account, resume, full);
    } else if (command == "fix-thumbs") {
        std::string account = get_arg(argc, argv, "--account");
        if (account.empty()) account = get_arg(argc, argv, "-a");
        if (account.empty()) {
            std::cerr << "Error: --account required\n";
            return 1;
        }
        bool force = has_flag(argc, argv, "--force");
        std::string range_str = get_arg(argc, argv, "--range");
        int range = 30;
        if (!range_str.empty()) {
            try { range = std::stoi(range_str); }
            catch (...) { std::cerr << "Error: invalid --range value: " << range_str << "\n"; return 1; }
        }
        cmd_fix_thumbs(account, force, range);
    } else if (command == "fix-big") {
        std::string dir = get_arg(argc, argv, "--dir");
        if (dir.empty()) dir = get_arg(argc, argv, "-d");

        bool recursive = true;
        if (has_flag(argc, argv, "--no-recursive")) recursive = false;
        if (has_flag(argc, argv, "--recursive") || has_flag(argc, argv, "-r")) recursive = true;

        cmd_fix_big(dir, recursive);
    } else if (command == "bot-upload") {
        std::string dir = get_arg(argc, argv, "--dir");
        if (dir.empty()) dir = get_arg(argc, argv, "-d");

        bool recursive = true;
        if (has_flag(argc, argv, "--no-recursive")) recursive = false;
        if (has_flag(argc, argv, "--recursive") || has_flag(argc, argv, "-r")) recursive = true;

        cmd_bot_upload(dir, recursive);
    } else if (command == "upload") {
        std::string account = get_arg(argc, argv, "--account");
        if (account.empty()) account = get_arg(argc, argv, "-a");
        if (account.empty()) {
            std::cerr << "Error: --account required\n";
            return 1;
        }
        std::string dir = get_arg(argc, argv, "--dir");
        if (dir.empty()) dir = get_arg(argc, argv, "-d");

        bool recursive = true;
        if (has_flag(argc, argv, "--no-recursive")) recursive = false;
        if (has_flag(argc, argv, "--recursive") || has_flag(argc, argv, "-r")) recursive = true;

        int concurrent = 1;
        std::string conc_str = get_arg(argc, argv, "--concurrent");
        if (conc_str.empty()) conc_str = get_arg(argc, argv, "-c");
        if (!conc_str.empty()) {
            try { concurrent = std::stoi(conc_str); }
            catch (...) { std::cerr << "Error: invalid --concurrent value: " << conc_str << "\n"; return 1; }
            if (concurrent < 1) concurrent = 1;
        }

        cmd_upload(account, dir, recursive, concurrent);
    } else if (command == "web-serve") {
        int port = 8088;
        std::string port_str = get_arg(argc, argv, "--port");
        if (port_str.empty()) port_str = get_arg(argc, argv, "-p");
        if (!port_str.empty()) {
            try { port = std::stoi(port_str); }
            catch (...) { std::cerr << "Error: invalid --port value: " << port_str << "\n"; return 1; }
        }
        cmd_web_serve(port);
    } else {
        print_usage();
        return 1;
    }

    return 0;
}
