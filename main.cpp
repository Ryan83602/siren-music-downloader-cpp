#include <iostream>
#include <string>
#include <vector>
#include <map>
#include <regex>
#include <sstream>
#include <filesystem>
#include <fstream>
#include <thread>
#include <future>
#include <chrono>
#include <algorithm>
#include <cctype>
#include <mutex>
#include <clocale>
#include <condition_variable>
#include <ctime>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

using namespace std;
using json = nlohmann::json;
namespace fs = std::filesystem;

#ifdef _WIN32
#include <windows.h>
static void enable_utf8_console() {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    setvbuf(stdout, nullptr, _IOFBF, 4096);
    setvbuf(stderr, nullptr, _IOFBF, 4096);
}
#else
static void enable_utf8_console() {}
#endif

#define COLOR_RESET   "\033[0m"
#define COLOR_RED     "\033[31m"
#define COLOR_YELLOW  "\033[33m"
#define COLOR_WHITE   "\033[37m"

static mutex log_mutex;
static ofstream log_file;

void init_log_file() {
    log_file.open("log.txt", ios::app);
}

void close_log_file() {
    if (log_file.is_open()) {
        log_file.flush();
        log_file.close();
    }
}

static void write_log(const string& level, const string& msg, const string& color) {
    lock_guard<mutex> lock(log_mutex);
    if (level == "INFO") {
        cout << color << "[INFO] " << msg << COLOR_RESET << endl;
    } else if (level == "WARN") {
        cout << color << "[WARN] " << msg << COLOR_RESET << endl;
    } else if (level == "ERR") {
        cerr << color << "[ERR] " << msg << COLOR_RESET << endl;
    }
    if (log_file.is_open()) {
        log_file << "[" << level << "] " << msg << endl;
        log_file.flush();
    }
}

static void log_info(const string& msg) { write_log("INFO", msg, COLOR_WHITE); }
static void log_warn(const string& msg) { write_log("WARN", msg, COLOR_YELLOW); }
static void log_err(const string& msg) { write_log("ERR", msg, COLOR_RED); }

struct Config {
    string base_api_url = "https://monster-siren.hypergryph.com/api";
    int max_album_workers = 3;
    int max_download_workers = 5;
    int retries = 3;
    int base_delay_seconds = 1;
    int request_timeout = 15;
    int chunk_size = 8192;
    long long min_audio_size_bytes = 10240;
    map<string, string> supported_audio_types = {
        {"audio/mpeg", "mp3"},
        {"audio/wav", "wav"},
        {"audio/flac", "flac"},
        {"audio/ogg", "ogg"},
        {"audio/x-wav", "wav"},
        {"audio/aac", "aac"},
        {"audio/mp4", "m4a"},
        {"audio/x-m4a", "m4a"}
    };
};

Config CFG;

const string HEADERS_API =
    "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/115.0.0.0 Safari/537.36\r\n"
    "Accept: application/json, text/plain, */*\r\n";

class Semaphore {
public:
    explicit Semaphore(int count) : count_(count) {}

    void acquire() {
        unique_lock<mutex> lock(mu_);
        cv_.wait(lock, [this] { return count_ > 0; });
        --count_;
    }

    void release() {
        lock_guard<mutex> lock(mu_);
        ++count_;
        cv_.notify_one();
    }

private:
    mutex mu_;
    condition_variable cv_;
    int count_;
};

string trim(const string& s) {
    size_t start = 0;
    while (start < s.size() && isspace(static_cast<unsigned char>(s[start]))) ++start;
    size_t end = s.size();
    while (end > start && isspace(static_cast<unsigned char>(s[end - 1]))) --end;
    return s.substr(start, end - start);
}

string sanitize_filename(const string& name) {
    string result;
    for (unsigned char ch : name) {
        if (ch == '\\' || ch == '/' || ch == ':' || ch == '*' || ch == '?' ||
            ch == '"' || ch == '<' || ch == '>' || ch == '|') {
            result.push_back('_');
        } else {
            result.push_back(static_cast<char>(ch));
        }
    }
    return trim(result);
}

string get_json_string(const json& obj, const string& key, const string& default_value = "") {
    if (obj.contains(key) && obj[key].is_string()) {
        return obj[key].get<string>();
    }
    return default_value;
}

int get_json_int(const json& obj, const string& key, int default_value = 0) {
    if (obj.contains(key) && obj[key].is_number_integer()) {
        return obj[key].get<int>();
    }
    return default_value;
}

string get_content_type(const string& headers) {
    istringstream iss(headers);
    string line;
    while (getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t colon = line.find(':');
        if (colon == string::npos) continue;
        string key = line.substr(0, colon);
        string val = line.substr(colon + 1);
        key = trim(key);
        transform(key.begin(), key.end(), key.begin(), ::tolower);
        if (key == "content-type") {
            size_t semi = val.find(';');
            if (semi != string::npos) val = val.substr(0, semi);
            return trim(val);
        }
    }
    return "";
}

static size_t write_callback(void* contents, size_t size, size_t nmemb, string* out) {
    size_t total = size * nmemb;
    out->append(static_cast<char*>(contents), total);
    return total;
}

static size_t header_callback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t total = size * nmemb;
    string* headers = static_cast<string*>(userp);
    headers->append(static_cast<char*>(contents), total);
    return total;
}

string http_get(const string& url, const string& headers, long timeout_sec = 15,
                long* out_status = nullptr, string* out_headers = nullptr) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        if (out_status) *out_status = 0;
        return "";
    }

    string response;
    string response_headers;
    long http_code = 0;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_sec);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

    struct curl_slist* list = nullptr;
    istringstream hs(headers);
    string line;
    while (getline(hs, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) {
            list = curl_slist_append(list, line.c_str());
        }
    }
    if (list) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list);
    }

    if (out_headers) {
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_callback);
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response_headers);
    }

    CURLcode res = curl_easy_perform(curl);
    if (res == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    } else {
        http_code = 0;
        response.clear();
    }

    curl_slist_free_all(list);
    curl_easy_cleanup(curl);

    if (out_status) *out_status = http_code;
    if (out_headers) *out_headers = response_headers;
    return response;
}

string http_get_retry(const string& url, const string& headers, long timeout_sec = 15,
                      long* out_status = nullptr, string* out_headers = nullptr) {
    int retries = CFG.retries;
    int delay = CFG.base_delay_seconds;

    for (int attempt = 1; attempt <= retries; ++attempt) {
        long status = 0;
        string resp_headers;
        string body = http_get(url, headers, timeout_sec, &status, &resp_headers);

        if (status >= 200 && status < 300) {
            if (out_status) *out_status = status;
            if (out_headers) *out_headers = resp_headers;
            return body;
        }

        if (status == 404 || status == 410) {
            log_err("Resource not found (" + to_string(status) + "): " + url);
            if (out_status) *out_status = status;
            return "";
        }

        if (status == 0) {
            log_warn("Connection failed (" + url + ") [" + to_string(attempt) + "/" + to_string(retries) + "]");
        } else {
            log_warn("HTTP " + to_string(status) + " (" + url + ") [" + to_string(attempt) + "/" + to_string(retries) + "]");
        }

        if (attempt < retries) {
            this_thread::sleep_for(chrono::seconds(delay * attempt));
        }
    }

    log_err("Request failed: " + url);
    if (out_status) *out_status = 0;
    if (out_headers) out_headers->clear();
    return "";
}

json safe_get_json(const string& url) {
    string body = http_get_retry(url, HEADERS_API, CFG.request_timeout);
    if (body.empty()) return json();
    try {
        return json::parse(body);
    } catch (...) {
        log_err("JSON parse failed: " + url);
        return json();
    }
}

string extract_date_from_cover(const string& cover_url) {
    if (cover_url.empty()) return "";
    regex re(R"(/pic/(\d{4})(\d{2})(\d{2})/)");
    smatch match;
    if (regex_search(cover_url, match, re)) {
        return match[1].str() + match[2].str() + match[3].str();
    }
    return "";
}

string parse_date_str(const json& val) {
    if (val.is_null()) return "";

    if (val.is_number()) {
        double ts = val.get<double>();
        if (ts <= 0) return "";
        if (ts > 1e11) ts /= 1000.0;
        time_t t = static_cast<time_t>(ts);
        struct tm tmv;
#ifdef _WIN32
        localtime_s(&tmv, &t);
#else
        localtime_r(&t, &tmv);
#endif
        char buf[16];
        strftime(buf, sizeof(buf), "%Y%m%d", &tmv);
        return string(buf);
    }

    if (val.is_string()) {
        string s = val.get<string>();
        if (s.empty()) return "";
        bool all_digits = all_of(s.begin(), s.end(), [](unsigned char c) { return isdigit(c); });
        if (all_digits) {
            long long ts = stoll(s);
            if (ts > 0) {
                if (ts > 1e11) ts /= 1000;
                time_t t = static_cast<time_t>(ts);
                struct tm tmv;
#ifdef _WIN32
                localtime_s(&tmv, &t);
#else
                localtime_r(&t, &tmv);
#endif
                char buf[16];
                strftime(buf, sizeof(buf), "%Y%m%d", &tmv);
                return string(buf);
            }
        }
        if (s.find('-') != string::npos) {
            string date_part = s.substr(0, 10);
            date_part.erase(remove(date_part.begin(), date_part.end(), '-'), date_part.end());
            return date_part;
        }
    }

    return "";
}

bool is_file_downloaded(const string& filepath) {
    error_code ec;
    fs::path p = fs::u8path(filepath);
    if (!fs::exists(p, ec)) return false;
    uintmax_t size = fs::file_size(p, ec);
    if (ec) return false;
    return size >= static_cast<uintmax_t>(CFG.min_audio_size_bytes);
}

bool check_song_audio_exists(const string& folder_name, const string& song_name) {
    fs::path folder = fs::u8path(folder_name);
    if (!fs::is_directory(folder)) return false;
    try {
        for (const auto& entry : fs::directory_iterator(folder)) {
            if (!entry.is_regular_file()) continue;
            string ext = entry.path().extension().string();
            if (entry.path().stem().string() == song_name && ext != ".lrc" && is_file_downloaded(entry.path().string())) {
                return true;
            }
        }
    } catch (...) {
        return false;
    }
    return false;
}

string resolve_audio_extension(const string& headers, const string& url) {
    string content_type = get_content_type(headers);
    if (!content_type.empty()) {
        auto it = CFG.supported_audio_types.find(content_type);
        if (it != CFG.supported_audio_types.end()) {
            return it->second;
        }
    }

    string clean = url;
    size_t q = clean.find('?');
    if (q != string::npos) clean = clean.substr(0, q);

    fs::path p(clean);
    string ext = p.extension().string();
    if (!ext.empty() && ext.front() == '.') ext = ext.substr(1);
    if (ext.empty()) return "wav";
    if (ext.size() <= 4) return ext;
    return "wav";
}

void download_file(const string& url, string filepath, bool resolve_ext) {
    if (is_file_downloaded(filepath)) {
        log_info("  [SKIP] Already exists: " + fs::path(filepath).filename().string());
        return;
    }

    long status = 0;
    string headers;
    string body = http_get_retry(url, HEADERS_API, CFG.request_timeout, &status, &headers);

    if (body.empty() && status != 200) {
        log_err("  [FAILED] Download error: " + fs::path(filepath).filename().string());
        return;
    }

    if (resolve_ext) {
        string ext = resolve_audio_extension(headers, url);
        fs::path p = fs::u8path(filepath);
        p.replace_extension(ext);
        filepath = p.string();
    }

    ofstream file(fs::u8path(filepath), ios::binary);
    if (!file) {
        log_err("  [FAILED] Cannot write file: " + fs::path(filepath).filename().string());
        return;
    }

    file.write(body.data(), body.size());
    file.close();

    error_code ec;
    uintmax_t size = fs::file_size(fs::u8path(filepath), ec);
    if (!ec && size > 0) {
        log_info("  [SUCCESS] Downloaded: " + fs::path(filepath).filename().string());
    } else {
        log_err("  [FAILED] File empty or too small: " + fs::path(filepath).filename().string());
        fs::remove(fs::u8path(filepath), ec);
    }
}

struct DownloadTask {
    string url;
    string filepath;
    bool resolve_ext;
};

string process_one_album(const json& album) {
    string album_cid = get_json_string(album, "cid", "");
    string api_url = CFG.base_api_url;

    json album_detail = safe_get_json(api_url + "/album/" + album_cid + "/detail");
    if (album_detail.empty() || get_json_int(album_detail, "code", -1) != 0) {
        album_detail = safe_get_json(api_url + "/album/" + album_cid + "/data");
    }

    json album_data = (album_detail.contains("data") && album_detail["data"].is_object())
                      ? album_detail["data"] : json::object();

    string album_name_raw = get_json_string(album_data, "name",
                                           get_json_string(album, "name", "Unknown_Album"));
    string album_name = sanitize_filename(album_name_raw);

    string cover_url = get_json_string(album_data, "coverUrl", "");
    if (cover_url.empty()) cover_url = get_json_string(album_data, "coverDeUrl", "");
    if (cover_url.empty()) cover_url = get_json_string(album, "coverUrl", "");
    if (cover_url.empty()) cover_url = get_json_string(album, "coverDeUrl", "");

    string date_str = extract_date_from_cover(cover_url);

    if (date_str.empty()) {
        json raw_time;
        if (album.contains("publishTime")) raw_time = album["publishTime"];
        else if (album.contains("date")) raw_time = album["date"];
        else if (album.contains("releaseTime")) raw_time = album["releaseTime"];
        else if (album_data.contains("publishTime")) raw_time = album_data["publishTime"];
        else if (album_data.contains("date")) raw_time = album_data["date"];
        else if (album_data.contains("releaseTime")) raw_time = album_data["releaseTime"];
        date_str = parse_date_str(raw_time);
        if (date_str.empty()) date_str = "Unknown Date";
    }

    string folder_name = "[" + date_str + "] " + album_name;
    fs::path folder = fs::u8path(folder_name);
    error_code ec;
    fs::create_directories(folder, ec);

    log_info("Processing album: " + folder_name);

    json songs = (album_data.contains("songs") && album_data["songs"].is_array())
                 ? album_data["songs"] : json::array();

    vector<DownloadTask> tasks;

    for (const auto& song : songs) {
        string song_cid = get_json_string(song, "cid", "");
        string song_name_raw = get_json_string(song, "name", "Unknown_Song");
        string song_name = sanitize_filename(song_name_raw);

        bool audio_downloaded = check_song_audio_exists(folder_name, song_name);
        string lrc_path = (folder / fs::u8path(song_name + ".lrc")).string();
        bool lrc_downloaded = is_file_downloaded(lrc_path);

        if (audio_downloaded && lrc_downloaded) {
            log_info("  [SKIP] Already complete: " + song_name);
            continue;
        }

        json song_detail = safe_get_json(api_url + "/song/" + song_cid);
        if (song_detail.empty() || get_json_int(song_detail, "code", -1) != 0) {
            log_warn("  [FAILED] Failed to get song details for " + song_name + ", skipping...");
            continue;
        }

        json song_data = (song_detail.contains("data") && song_detail["data"].is_object())
                         ? song_detail["data"] : json::object();

        string source_url = get_json_string(song_data, "sourceUrl", "");
        string lyric_url = get_json_string(song_data, "lyricUrl", "");

        if (source_url.empty()) {
            log_warn("  [WARN] No audio URL found for song " + song_name);
            continue;
        }

        string audio_filepath = (folder / fs::u8path(song_name + ".tmp")).string();

        if (!is_file_downloaded(audio_filepath)) {
            tasks.push_back({source_url, audio_filepath, true});
        } else {
            log_info("  [SKIP] Audio already exists: " + fs::path(audio_filepath).filename().string());
        }

        if (!lyric_url.empty() && !is_file_downloaded(lrc_path)) {
            tasks.push_back({lyric_url, lrc_path, false});
        } else if (!lyric_url.empty()) {
            log_info("  [SKIP] Lyrics already exists: " + fs::path(lrc_path).filename().string());
        }
    }

    if (!tasks.empty()) {
        Semaphore sem(CFG.max_download_workers);
        vector<future<void>> futures;
        for (const auto& task : tasks) {
            futures.emplace_back(async(launch::async, [&sem, task]() {
                sem.acquire();
                download_file(task.url, task.filepath, task.resolve_ext);
                sem.release();
            }));
        }
        for (auto& fut : futures) {
            fut.get();
        }
    }

    return folder_name;
}

int main(int argc, char* argv[]) {
    setlocale(LC_ALL, "en_US.UTF-8");
    enable_utf8_console();
    init_log_file();

    curl_global_init(CURL_GLOBAL_ALL);

    log_info("Fetching album list...");
    json albums_res = safe_get_json(CFG.base_api_url + "/albums");

    if (albums_res.empty() || get_json_int(albums_res, "code", -1) != 0) {
        log_err("Failed to fetch album list!");
        curl_global_cleanup();
        close_log_file();
        return 1;
    }

    vector<json> albums;
    if (albums_res.contains("data") && albums_res["data"].is_array()) {
        for (const auto& album : albums_res["data"]) {
            albums.push_back(album);
        }
    }

    log_info("Found " + to_string(albums.size()) + " albums, starting...");

    Semaphore sem(CFG.max_album_workers);
    vector<future<string>> futures;

    for (const auto& album : albums) {
        futures.emplace_back(async(launch::async, [&sem, album]() -> string {
            sem.acquire();
            string folder;
            try {
                folder = process_one_album(album);
            } catch (const exception& e) {
                log_err("Album processing exception: " + string(e.what()));
            }
            sem.release();
            return folder;
        }));
    }

    for (auto& fut : futures) {
        string folder;
        try {
            folder = fut.get();
            if (!folder.empty()) {
                log_info("Album completed: " + folder);
            }
        } catch (...) {}
    }

    log_info("All tasks completed!");
    curl_global_cleanup();
    close_log_file();
    return 0;
}