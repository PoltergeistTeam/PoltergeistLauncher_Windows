// PTLauncher (PTL) — C++17, Win32 + Direct3D11 + Dear ImGui.
// Vanilla / Fabric / Forge / NeoForge / Quilt / OptiFine, Java auto-detect+download, offline / Ely.by / Microsoft,
// custom backgrounds from screenshots, crash explainer.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <commdlg.h>
#include <shellapi.h>
#include <objbase.h>
#include <shlobj.h>
#include <dwmapi.h>
#include <wincrypt.h>
#include <d3d11.h>
#include <atomic>
#include <thread>
#include <mutex>
#include <deque>
#include <map>
#include <set>
#include <optional>
#include <random>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cctype>
#include <chrono>
#include <tuple>
#include <regex>
#include <cstdio>
#include <ctime>
#include <memory>
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include <nlohmann/json.hpp>
#include <miniz.h>
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_ONLY_TGA
#include <stb_image.h>

namespace fs = std::filesystem;
#ifndef SM_CXPADDEDBORDER
#define SM_CXPADDEDBORDER 92
#endif
using json = nlohmann::json;
static const char* kVer = "0.0.6";
static const char* kAppName = "PTLauncher";
static const char* kRepoApi = "https://api.github.com/repos/PoltergeistTeam/PoltergeistLauncher_Windows/releases/latest";
static const char* kRepoUrl = "https://github.com/PoltergeistTeam/PoltergeistLauncher_Windows/releases/latest";

// ====== Настройка: Azure Client ID для входа через Microsoft (см. объяснение в ответе) ======
static const std::string kMsClientId = ""; // можно оставить пустым: ID вводится в приложении (вкладка «Аккаунт») и сохраняется в config.json
static std::mutex gIdMu; static std::string gMsId = kMsClientId;
static std::string MsId() { std::lock_guard<std::mutex> l(gIdMu); return gMsId; }
static void SetMsId(const std::string& v) { std::lock_guard<std::mutex> l(gIdMu); gMsId = v; }
static const std::string kAuthlibUrl = "https://github.com/yushijinhun/authlib-injector/releases/download/v1.2.5/authlib-injector-1.2.5.jar";

// ============================ Утилиты ============================
static std::wstring W(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), 0, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
static std::string U8(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), 0, 0, 0, 0);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, 0, 0);
    return s;
}
static fs::path P(const std::string& s) { return fs::path(W(s)); }
static std::string PS(const fs::path& p) { return U8(p.wstring()); }
static std::wstring Env(const wchar_t* n) {
    wchar_t b[2048];
    DWORD l = GetEnvironmentVariableW(n, b, 2048);
    return (l && l < 2048) ? std::wstring(b, l) : L"";
}
static std::string ReadAll(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss; ss << f.rdbuf();
    return ss.str();
}
static void WriteAll(const fs::path& p, const std::string& d) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    fs::path t = p; t += L".tmp";
    { std::ofstream f(t, std::ios::binary); f.write(d.data(), (std::streamsize)d.size()); }
    fs::rename(t, p, ec);
}
static json Parse(const std::string& s) { return json::parse(s, nullptr, false); }
static std::string RandHex(int n) {
    static std::random_device rd; static const char* h = "0123456789abcdef";
    std::string s; for (int i = 0; i < n * 2; i++) s += h[rd() % 16]; return s;
}
static std::vector<std::string> Split(const std::string& s, char c) {
    std::vector<std::string> r; std::string cur; std::stringstream ss(s);
    while (std::getline(ss, cur, c)) r.push_back(cur);
    return r;
}
static std::string Q(const std::string& a) { // quoting для CreateProcess
    if (!a.empty() && a.find_first_of(" \t\"") == std::string::npos) return a;
    std::string r = "\""; int bs = 0;
    for (char c : a) {
        if (c == '\\') { bs++; r += c; }
        else if (c == '"') { r.append(bs + 1, '\\'); r += '"'; bs = 0; }
        else { bs = 0; r += c; }
    }
    r.append(bs, '\\'); r += "\""; return r;
}
static std::string Sub(std::string s, const std::map<std::string, std::string>& m) {
    for (auto& [k, v] : m) {
        std::string key = "${" + k + "}"; size_t p = 0;
        while ((p = s.find(key, p)) != std::string::npos) { s.replace(p, key.size(), v); p += v.size(); }
    }
    return s;
}

// ============================ Глобальное состояние ============================
static fs::path gRoot;    // папка игры (версии, библиотеки, Java, instances)
static fs::path gCfgDir;  // папка настроек лаунчера (config.json) — всегда одна и та же
static HWND gHwnd; static std::atomic<bool> gGameRunning{false};
enum { WM_TRAY = WM_APP + 1, WM_APP_HIDE = WM_APP + 2, WM_APP_SHOW = WM_APP + 3 }; // трей: спрятать лаунчер в фон / показать
static std::mutex gMu;
static std::deque<std::string> gLog;
static std::string gStatus = "Готов к запуску";
static std::atomic<float> gProg{0};
static std::atomic<bool> gBusy{false}, gDelBusy{false};
static std::atomic<bool> gCancel{false};   // пользователь нажал «Остановить» / «Закрыть»
struct Cancelled : std::runtime_error { Cancelled() : std::runtime_error("Остановлено") {} };
static void CheckCancel() { if (gCancel) throw Cancelled(); }
// Процесс, который сейчас выполняет лаунчер (игра, установщик, патчер). Лежит в Job Object, чтобы убивались и дочерние процессы.
static std::mutex gProcMu; static DWORD gChildPid = 0; static HANDLE gChildProc = nullptr, gChildJob = nullptr;
static bool ChildIs(DWORD pid) { std::lock_guard<std::mutex> l(gProcMu); return pid && gChildPid == pid; }
static void ProcHardKill(DWORD pid) { // жёсткое завершение
    std::lock_guard<std::mutex> l(gProcMu); if (!pid || gChildPid != pid) return;
    if (gChildJob) TerminateJobObject(gChildJob, 1); else if (gChildProc) TerminateProcess(gChildProc, 1);
}
static BOOL CALLBACK EnumCloseWnd(HWND h, LPARAM lp) { DWORD pid = 0; GetWindowThreadProcessId(h, &pid); if (pid == (DWORD)lp && IsWindowVisible(h)) PostMessageW(h, WM_CLOSE, 0, 0); return TRUE; }
static void ProcSoftClose(DWORD pid) { if (pid) EnumWindows(EnumCloseWnd, (LPARAM)pid); } // как крестик на окне игры: Minecraft успевает сохранить мир
static bool gShowCrash = false;
static std::string gCrashTitle, gCrashBody;
static std::string gMsCode, gMsUri;
static std::string gAuthMsg; // сообщения входа в аккаунт (показываются только на вкладке «Аккаунты»)
struct Account { int type = 0; std::string name, uuid, token, refresh; }; // 0 offline, 1 Ely.by, 2 Microsoft
static std::optional<Account> gNewAcc;
static std::atomic<bool> gAuthBusy{false};
static char bNick[32] = "Player", bElyU[128], bElyP[160], bJava[260], bMsId[96], bJvm[512];

static std::atomic<bool> gLogOn{true};
static std::ofstream gLogFile; // полный лог сессии: <папка лаунчера>/logs/latest.log (прошлый запуск — previous.log)
static void Log(const std::string& s) {
    if (!gLogOn) return;
    std::lock_guard<std::mutex> l(gMu); gLog.push_back(s); if (gLog.size() > 3000) gLog.pop_front();
    if (gLogFile.is_open()) { SYSTEMTIME st; GetLocalTime(&st); char ts[24]; snprintf(ts, sizeof(ts), "[%02d:%02d:%02d] ", (int)st.wHour, (int)st.wMinute, (int)st.wSecond); gLogFile << ts << s << "\n"; gLogFile.flush(); }
}
static void Status(const std::string& s) { { std::lock_guard<std::mutex> l(gMu); gStatus = s; } Log(s); }
static void AuthMsg(const std::string& s) { { std::lock_guard<std::mutex> l(gMu); gAuthMsg = s; } Status(s); }
static void ShowError(const std::string& t, const std::string& b) { std::lock_guard<std::mutex> l(gMu); gShowCrash = true; gCrashTitle = t; gCrashBody = b; }

// ============================ HTTP (WinHTTP) ============================
struct Resp { int status = 0; std::string body; };
static thread_local std::string gNetUA;  // свой User-Agent для запроса (для optifine.net нужен «браузерный»)
static thread_local std::string gNetErr;  // причина последней сетевой ошибки
static std::mutex gReqMu; static std::set<HINTERNET> gReqs; // активные запросы: «Остановить» закрывает их, чтобы прервать зависшую загрузку
static void AbortHttp() { std::lock_guard<std::mutex> l(gReqMu); for (HINTERNET h : gReqs) WinHttpCloseHandle(h); gReqs.clear(); }
static Resp Http(const std::string& url, const std::string& method = "GET", const std::string& body = "", const std::string& hdr = "") {
    Resp r; gNetErr.clear(); if (gCancel) return r;
    std::wstring w = W(url);
    URL_COMPONENTSW uc{}; uc.dwStructSize = sizeof(uc);
    uc.dwHostNameLength = uc.dwUrlPathLength = uc.dwExtraInfoLength = (DWORD)-1;
    if (!WinHttpCrackUrl(w.c_str(), 0, 0, &uc)) { gNetErr = "неверный адрес (" + url + ")"; return r; }
    std::wstring host(uc.lpszHostName, uc.dwHostNameLength), path(uc.lpszUrlPath, uc.dwUrlPathLength + uc.dwExtraInfoLength);
    static HINTERNET ses = [] {
        std::wstring ua = W(std::string("PTLauncher/") + kVer + " (github.com/PoltergeistTeam/PoltergeistLauncher_Windows)");
        HINTERNET h = WinHttpOpen(ua.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, 0, 0, 0);
        if (!h) h = WinHttpOpen(ua.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, 0, 0, 0); // старые Windows (до 8.1)
        return h;
    }();
    HINTERNET con = WinHttpConnect(ses, host.c_str(), uc.nPort, 0);
    if (!con) { gNetErr = "WinHTTP: не удалось подключиться, код " + std::to_string(GetLastError()) + " (" + url + ")"; return r; }
    HINTERNET req = WinHttpOpenRequest(con, W(method).c_str(), path.c_str(), 0, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                       uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    if (req) {
        { std::lock_guard<std::mutex> l(gReqMu); gReqs.insert(req); }
        DWORD pol = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS; WinHttpSetOption(req, WINHTTP_OPTION_REDIRECT_POLICY, &pol, sizeof(pol)); // переходы http<->https
        if (!gNetUA.empty()) { std::wstring u = L"User-Agent: " + W(gNetUA); WinHttpAddRequestHeaders(req, u.c_str(), (DWORD)-1, WINHTTP_ADDREQ_FLAG_REPLACE | WINHTTP_ADDREQ_FLAG_ADD); }
        std::wstring h = W(hdr);
        BOOL ok = WinHttpSendRequest(req, h.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : h.c_str(), h.empty() ? 0 : (DWORD)-1,
                                     body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body.data(), (DWORD)body.size(), (DWORD)body.size(), 0)
                  && WinHttpReceiveResponse(req, 0);
        if (ok) {
            DWORD sc = 0, sz = sizeof(sc);
            WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &sc, &sz, WINHTTP_NO_HEADER_INDEX);
            r.status = (int)sc;
            if (sc != 200) gNetErr = "HTTP " + std::to_string(sc) + " (" + url + ")";
            for (;;) {
                if (gCancel) break;
                DWORD n = 0;
                if (!WinHttpQueryDataAvailable(req, &n) || !n) break;
                size_t o = r.body.size(); r.body.resize(o + n);
                DWORD rd = 0; WinHttpReadData(req, &r.body[o], n, &rd); r.body.resize(o + rd);
            }
        } else if (!gCancel) gNetErr = "WinHTTP: ошибка " + std::to_string(GetLastError()) + " (" + url + ")";
        bool own; { std::lock_guard<std::mutex> l(gReqMu); own = gReqs.erase(req) > 0; } // если «Остановить» уже закрыл запрос — второй раз не закрываем
        if (own) WinHttpCloseHandle(req);
    }
    WinHttpCloseHandle(con);
    if (gCancel) { r.status = 0; r.body.clear(); }
    else if (r.status != 200 && r.status != 204 && !gNetErr.empty()) Log("[сеть] " + gNetErr); // причина сбоя попадает в лог
    return r;
}
static Resp PostJson(const std::string& url, const json& j) {
    return Http(url, "POST", j.dump(), "Content-Type: application/json\r\nAccept: application/json\r\n");
}
static std::string UrlEnc(const std::string& s) {
    static const char* h = "0123456789ABCDEF"; std::string r;
    for (unsigned char c : s) { if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') r += (char)c; else { r += '%'; r += h[c >> 4]; r += h[c & 15]; } }
    return r;
}
static std::string Form(std::initializer_list<std::pair<std::string, std::string>> kv) {
    std::string r; for (auto& [k, v] : kv) { if (!r.empty()) r += '&'; r += k + "=" + UrlEnc(v); } return r;
}
static const char* kFormHdr = "Content-Type: application/x-www-form-urlencoded\r\n";

static bool Download(const std::string& url, const fs::path& dst, bool force = false) {
    std::error_code ec;
    if (!force && fs::exists(dst, ec) && fs::file_size(dst, ec) > 0) return true;
    for (int i = 0; i < 3; i++) {
        if (gCancel) return false;
        Resp r = Http(url);
        if (r.status == 200 && !r.body.empty()) { WriteAll(dst, r.body); return true; }
        Sleep(300);
    }
    return false;
}
struct Job { std::string url; fs::path dst; };
static void DownloadAll(const std::vector<Job>& jobs, float p0, float p1) { // 8 потоков
    size_t total = jobs.size(); if (!total) return;
    std::atomic<size_t> idx{0}, done{0};
    auto worker = [&] {
        for (;;) {
            if (gCancel) break;
            size_t i = idx++; if (i >= total) break;
            Download(jobs[i].url, jobs[i].dst);
            size_t d = ++done; if (d % 16 == 0 || d == total) gProg = p0 + (p1 - p0) * (float)d / (float)total;
        }
    };
    std::vector<std::thread> th; for (int i = 0; i < 8; i++) th.emplace_back(worker);
    for (auto& t : th) t.join();
}
static bool Unzip(const fs::path& zip, const fs::path& dst, bool skipMeta = false) {
    std::string data = ReadAll(zip); if (data.empty()) return false;
    mz_zip_archive z{};
    if (!mz_zip_reader_init_mem(&z, data.data(), data.size(), 0)) return false;
    for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&z); i++) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&z, i, &st) || mz_zip_reader_is_file_a_directory(&z, i)) continue;
        std::string name = st.m_filename;
        if (name.find("..") != std::string::npos) continue;
        if (skipMeta && name.rfind("META-INF", 0) == 0) continue;
        size_t sz = 0; void* p = mz_zip_reader_extract_to_heap(&z, i, &sz, 0);
        if (!p) continue;
        WriteAll(dst / P(name), std::string((const char*)p, sz));
        mz_free(p);
    }
    mz_zip_reader_end(&z);
    return true;
}

// ============================ Java ============================
struct JavaInfo { std::string exe; int major; };
static int JavaMajorFromHome(const fs::path& home) {
    std::string rel = ReadAll(home / "release");
    auto p = rel.find("JAVA_VERSION=\"");
    if (p == std::string::npos) return 0;
    std::string v = rel.substr(p + 14, 16);
    int a = atoi(v.c_str());
    if (a == 1) { auto d = v.find('.'); return d == std::string::npos ? 0 : atoi(v.c_str() + d + 1); }
    return a;
}
static std::vector<JavaInfo> FindJavas() {
    std::vector<fs::path> homes; std::error_code ec;
    auto add = [&](const fs::path& h) { if (fs::exists(h / "bin" / "java.exe", ec)) homes.push_back(h); };
    auto scan = [&](const fs::path& d) { if (!fs::is_directory(d, ec)) return; for (auto& e : fs::directory_iterator(d, ec)) if (e.is_directory(ec)) add(e.path()); };
    for (auto v : {L"ProgramFiles", L"ProgramFiles(x86)"}) {
        std::wstring pfw = Env(v); if (pfw.empty()) continue; fs::path pf = pfw;
        for (auto ven : {L"Java", L"Eclipse Adoptium", L"Microsoft", L"Zulu", L"BellSoft", L"Amazon Corretto", L"AdoptOpenJDK", L"Semeru", L"Eclipse Foundation"})
            scan(pf / ven);
    }
    std::wstring jh = Env(L"JAVA_HOME"); if (!jh.empty()) add(fs::path(jh));
    for (auto& e : fs::directory_iterator(gRoot / "runtime", ec)) scan(e.path());
    std::wstring ad = Env(L"APPDATA");
    fs::path mc = fs::path(ad) / ".minecraft" / "runtime"; // Java от официального лаунчера
    if (!ad.empty() && fs::is_directory(mc, ec))
        for (auto it = fs::recursive_directory_iterator(mc, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            if (it->path().filename() == L"java.exe") homes.push_back(it->path().parent_path().parent_path());
        }
    std::set<std::string> seen; std::vector<JavaInfo> out;
    for (auto& h : homes) {
        std::string key = PS(h); if (!seen.insert(key).second) continue;
        int m = JavaMajorFromHome(h); if (m <= 0) continue;
        out.push_back({PS(h / "bin" / "java.exe"), m});
    }
    std::sort(out.begin(), out.end(), [](auto& a, auto& b) { return a.major < b.major; });
    return out;
}
static std::string PickJava(int need) {
    auto js = FindJavas(); std::string best; int bm = 999;
    for (auto& j : js) if (j.major >= need && j.major < bm) { bm = j.major; best = j.exe; }
    return best;
}
static bool InstallJavaVer(int need) { // скачать и распаковать Temurin JRE нужной версии
    Status("Скачиваю Java " + std::to_string(need) + " (Temurin)...");
    fs::path zip = gRoot / "runtime" / ("temurin-" + std::to_string(need) + ".zip");
    std::string url = "https://api.adoptium.net/v3/binary/latest/" + std::to_string(need) + "/ga/windows/x64/jre/hotspot/normal/eclipse";
    if (!Download(url, zip, true)) return false;
    Unzip(zip, gRoot / "runtime" / std::to_string(need));
    std::error_code ec; fs::remove(zip, ec);
    return true;
}
static std::string EnsureJava(int need) {
    std::string j = PickJava(need); if (!j.empty()) return j;
    if (!InstallJavaVer(need)) return "";
    return PickJava(need);
}
static std::string JavaExe(std::string p) { // javaw.exe -> java.exe (нужен вывод в консоль)
    auto k = p.rfind("javaw.exe"); if (k != std::string::npos) p.replace(k, 9, "java.exe"); return p;
}

// ============================ Версии ============================
struct Ver { std::string id, type, url; };
static std::vector<Ver> gVers; static std::string gLatest; static std::mutex gVerMu;

static void FetchVersions() {
    fs::path c = gRoot / "version_manifest.json";
    Resp r = Http("https://piston-meta.mojang.com/mc/game/version_manifest_v2.json");
    std::string body = (r.status == 200) ? r.body : ReadAll(c);
    if (r.status == 200) WriteAll(c, body);
    json j = Parse(body);
    if (!j.contains("versions")) { Status("Нет интернета: список версий недоступен"); return; }
    std::lock_guard<std::mutex> l(gVerMu);
    gLatest = j["latest"].value("release", "");
    for (auto& v : j["versions"]) gVers.push_back({v.value("id", ""), v.value("type", ""), v.value("url", "")});
}
static std::vector<std::string> NeoVersions() { // версии NeoForge из maven-metadata.xml (по возрастанию)
    std::vector<std::string> r; Resp x = Http("https://maven.neoforged.net/releases/net/neoforged/neoforge/maven-metadata.xml");
    if (x.status != 200) return r;
    size_t p = 0;
    while ((p = x.body.find("<version>", p)) != std::string::npos) {
        size_t e = x.body.find("</version>", p); if (e == std::string::npos) break;
        r.push_back(x.body.substr(p + 9, e - p - 9)); p = e;
    }
    return r;
}
static std::string NeoMcOf(const std::string& v) { // 21.1.172 -> 1.21.1, 21.0.5 -> 1.21, 26.1.x -> 26.1
    int a = 0, b = 0; if (sscanf(v.c_str(), "%d.%d", &a, &b) != 2) return "";
    if (a >= 26) return std::to_string(a) + "." + std::to_string(b);
    return b == 0 ? "1." + std::to_string(a) : "1." + std::to_string(a) + "." + std::to_string(b);
}
// ---------- OptiFine: список сборок и скачивание ----------
struct OfV { std::string type, patch; }; // type: HD_U, HD_U_G..., patch: I6, H5...
static bool OfBetter(const OfV& a, const OfV& b) { // a новее/лучше b
    auto key = [](const OfV& o) {
        bool hdu = o.type == "HD_U", pre = o.patch.rfind("pre", 0) == 0;
        int letter = (!o.patch.empty() && isalpha((unsigned char)o.patch[0])) ? toupper((unsigned char)o.patch[0]) : 0, num = o.patch.size() > 1 ? atoi(o.patch.c_str() + 1) : 0;
        return std::make_tuple(hdu, !pre, letter, num);
    };
    return key(a) > key(b);
}
static std::string OfKey(const OfV& o) { return o.type + "_" + o.patch; }
static std::string OfLabel(const OfV& o) { std::string r = OfKey(o); for (char& c : r) if (c == '_') c = ' '; return r; }
static std::string OfFile(const std::string& mc, const OfV& o) { return "OptiFine_" + mc + "_" + o.type + "_" + o.patch + ".jar"; }
static const char* const kBrowserUA = "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36";
static const char* const kOfHdr = "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8\r\nAccept-Language: en-US,en;q=0.9\r\n";
static std::map<std::string, std::vector<OfV>> gOptiBuilds; // версия игры -> сборки OptiFine, лучшие первыми (под gVerMu)

static bool OfParseFile(const std::string& f, std::string& mc, OfV& o) { // OptiFine_1.20.1_HD_U_I6.jar
    static const std::regex re("^OptiFine_([0-9][0-9.]*)_([A-Za-z_]+?)_([A-Z][0-9]+)\\.jar$");
    std::smatch m; if (!std::regex_match(f, m, re)) return false;
    mc = m[1].str(); o.type = m[2].str(); o.patch = m[3].str(); return true;
}
static void OfAdd(std::map<std::string, std::vector<OfV>>& out, const std::string& mc, const OfV& o) {
    auto& v = out[mc]; for (auto& e : v) if (OfKey(e) == OfKey(o)) return; v.push_back(o);
}
// Официальный список: страница https://optifine.net/downloads (ссылки вида adloadx?f=OptiFine_1.20.1_HD_U_I6.jar)
static std::map<std::string, std::vector<OfV>> FetchOptiNet() {
    std::map<std::string, std::vector<OfV>> out;
    gNetUA = kBrowserUA;
    Resp r = Http("https://optifine.net/downloads", "GET", "", std::string(kOfHdr) + "Referer: https://optifine.net/\r\n");
    gNetUA.clear();
    if (r.status != 200 || r.body.empty()) return out;
    for (const char* key : {"adloadx?f=", "downloadx?f=", "download?f="}) {
        std::string k = key; size_t p = 0;
        while ((p = r.body.find(k, p)) != std::string::npos) {
            size_t b = p + k.size(), e = b;
            while (e < r.body.size() && std::string("\"'&<> \\\r\n").find(r.body[e]) == std::string::npos) e++;
            std::string f = r.body.substr(b, e - b); p = e;
            std::string mc; OfV o; if (OfParseFile(f, mc, o)) OfAdd(out, mc, o);
        }
    }
    return out;
}
// Основной источник — optifine.net; зеркало BMCLAPI добавляет то, чего нет на сайте (или работает, если сайт недоступен)
static std::map<std::string, std::vector<OfV>> FetchOptiAll() {
    std::map<std::string, std::vector<OfV>> out = FetchOptiNet();
    Resp r = Http("https://bmclapi2.bangbang93.com/optifine/versionList"); json j = Parse(r.body);
    if (j.is_array()) for (auto& x : j) {
        if (!x.is_object() || !x.contains("mcversion") || !x["mcversion"].is_string()) continue;
        std::string mc = x["mcversion"].get<std::string>(); 
        if (x.contains("type") && x.contains("patch") && x["type"].is_string() && x["patch"].is_string()) {
            OfV o{x["type"].get<std::string>(), x["patch"].get<std::string>()};
            if (!o.type.empty() && !o.patch.empty()) OfAdd(out, mc, o);
        }
    }
    for (auto& kv : out) std::sort(kv.second.begin(), kv.second.end(), [](const OfV& a, const OfV& b) { return OfBetter(a, b); });
    for (auto it = out.begin(); it != out.end();) { if (it->second.empty()) it = out.erase(it); else ++it; } // версии без сборок не показываем
    return out;
}
// Скачивание с optifine.net: страница adloadx содержит ссылку downloadx?f=...&x=<токен>, по ней отдаётся сам jar
static Resp OfHttpOfficial(const std::string& file, std::string& why) {
    Resp out; why.clear();
    gNetUA = kBrowserUA;
    std::string page = "https://optifine.net/adloadx?f=" + file;
    Resp pg = Http(page, "GET", "", std::string(kOfHdr) + "Referer: https://optifine.net/downloads\r\n");
    if (pg.status == 200 && !pg.body.empty()) {
        std::string href; size_t p = pg.body.find("downloadx?f=");
        if (p != std::string::npos) {
            size_t e = p; while (e < pg.body.size() && std::string("\"'<> \r\n").find(pg.body[e]) == std::string::npos) e++;
            href = pg.body.substr(p, e - p);
            for (size_t q; (q = href.find("&amp;")) != std::string::npos;) href.replace(q, 5, "&");
        }
        if (!href.empty()) out = Http(href.rfind("http", 0) == 0 ? href : "https://optifine.net/" + href, "GET", "", std::string(kOfHdr) + "Referer: " + page + "\r\n");
        else why = "на странице optifine.net нет ссылки на загрузку";
    } else { out.status = pg.status; why = gNetErr; }
    gNetUA.clear();
    return out;
}

static std::set<std::string> gQuiltGames, gNeoGames, gOptiGames, gFabricGames, gForgeGames; // какие версии игры поддерживает загрузчик (под gVerMu); пусто = список не получен
static std::atomic<bool> gLoadersLoading{true};
static fs::path LoaderCachePath() { return gCfgDir / "loader_cache.json"; }
static void LoadLoaderCache() { // последние известные списки: работают и без интернета
    json j = Parse(ReadAll(LoaderCachePath())); if (!j.is_object()) return;
    std::set<std::string> fab, forge, quilt, neo, opti; std::map<std::string, std::vector<OfV>> ob;
    auto rd = [&](const char* k, std::set<std::string>& dst) { if (j.contains(k) && j[k].is_array()) for (auto& x : j[k]) if (x.is_string()) dst.insert(x.get<std::string>()); };
    rd("fabric", fab); rd("forge", forge); rd("quilt", quilt); rd("neo", neo);
    if (j.contains("opti") && j["opti"].is_object())
        for (auto it = j["opti"].begin(); it != j["opti"].end(); ++it) if (it.value().is_array())
            for (auto& x : it.value()) if (x.is_object() && x.contains("t") && x.contains("p") && x["t"].is_string() && x["p"].is_string()) OfAdd(ob, it.key(), OfV{x["t"].get<std::string>(), x["p"].get<std::string>()});
    for (auto& kv : ob) { std::sort(kv.second.begin(), kv.second.end(), [](const OfV& a, const OfV& b) { return OfBetter(a, b); }); opti.insert(kv.first); }
    std::lock_guard<std::mutex> l(gVerMu); gFabricGames = fab; gForgeGames = forge; gQuiltGames = quilt; gNeoGames = neo; gOptiGames = opti; if (!ob.empty()) gOptiBuilds = ob;
}
static void SaveLoaderCache() {
    json j; std::lock_guard<std::mutex> l(gVerMu);
    j["fabric"] = gFabricGames; j["forge"] = gForgeGames; j["quilt"] = gQuiltGames; j["neo"] = gNeoGames; j["opti"] = json::object();
    for (auto& kv : gOptiBuilds) { json a = json::array(); for (auto& b : kv.second) a.push_back({{"t", b.type}, {"p", b.patch}}); j["opti"][kv.first] = a; }
    WriteAll(LoaderCachePath(), j.dump());
}
static void FetchLoaderLists() {
    std::set<std::string> fab, forge, quilt, neo, opti;
    Resp r = Http("https://meta.fabricmc.net/v2/versions/game");
    json j = Parse(r.body);
    if (j.is_array()) for (auto& x : j) if (x.is_object() && x.contains("version") && x["version"].is_string()) fab.insert(x["version"].get<std::string>());
    r = Http("https://files.minecraftforge.net/net/minecraftforge/forge/promotions_slim.json");
    j = Parse(r.body);
    if (j.is_object() && j.contains("promos") && j["promos"].is_object())
        for (auto it = j["promos"].begin(); it != j["promos"].end(); ++it) { size_t p = it.key().rfind('-'); if (p != std::string::npos) forge.insert(it.key().substr(0, p)); }
    r = Http("https://meta.quiltmc.org/v3/versions/game"); j = Parse(r.body);
    if (j.is_array()) for (auto& x : j) if (x.is_object() && x.contains("version") && x["version"].is_string()) quilt.insert(x["version"].get<std::string>());
    for (auto& v : NeoVersions()) { std::string m = NeoMcOf(v); if (!m.empty()) neo.insert(m); }
    auto ob = FetchOptiAll(); for (auto& kv : ob) if (!kv.second.empty()) opti.insert(kv.first);
    {
        std::lock_guard<std::mutex> l(gVerMu); // если запрос не удался (пусто) — остаётся прошлый список из кэша
        if (!fab.empty()) gFabricGames = fab; if (!forge.empty()) gForgeGames = forge; if (!quilt.empty()) gQuiltGames = quilt; if (!neo.empty()) gNeoGames = neo;
        if (!ob.empty()) { gOptiBuilds = ob; gOptiGames = opti; }
    }
    SaveLoaderCache(); gLoadersLoading = false;
    Log("Списки загрузчиков: Fabric " + std::to_string(fab.size()) + ", Forge " + std::to_string(forge.size()) + ", Quilt " + std::to_string(quilt.size()) + ", NeoForge " + std::to_string(neo.size()) + ", OptiFine " + std::to_string(opti.size()));
}
static bool RuleOk(const json& rules) {
    if (!rules.is_array() || rules.empty()) return true;
    bool ok = false;
    for (auto& r : rules) {
        bool m = true;
        if (r.contains("os")) {
            auto& o = r["os"];
            if (o.contains("name") && o["name"] != "windows") m = false;
            if (o.contains("arch") && o["arch"] == "x86") m = false;
        }
        if (r.contains("features")) m = false; // demo / custom resolution и т.п. — выключено
        if (m) ok = (r.value("action", "allow") == "allow");
    }
    return ok;
}
static json Merge(json p, const json& c) {
    for (auto it = c.begin(); it != c.end(); ++it) {
        const std::string& k = it.key(); const json& v = it.value();
        if (k == "libraries") { json l = v; for (auto& x : p["libraries"]) l.push_back(x); p["libraries"] = l; }
        else if (k == "arguments") { for (auto key : {"game", "jvm"}) if (v.contains(key)) for (auto& x : v[key]) p["arguments"][key].push_back(x); }
        else if (k != "inheritsFrom") p[k] = v;
    }
    return p;
}
static json LoadVersionJson(const std::string& id, int depth = 0) {
    fs::path p = gRoot / "versions" / P(id) / P(id + ".json");
    json j = Parse(ReadAll(p));
    if (j.is_discarded() || !j.is_object()) {
        std::string url;
        { std::lock_guard<std::mutex> l(gVerMu); for (auto& v : gVers) if (v.id == id) url = v.url; }
        if (url.empty()) return json();
        Resp r = Http(url); if (r.status != 200) return json();
        WriteAll(p, r.body); j = Parse(r.body);
        if (!j.is_object()) return json();
    }
    if (j.contains("inheritsFrom") && depth < 4) {
        json par = LoadVersionJson(j["inheritsFrom"].get<std::string>(), depth + 1);
        if (par.is_null()) return json();
        return Merge(par, j);
    }
    j["_base"] = j.value("id", id);
    return j;
}
static std::string MavenPath(std::string n) {
    std::string ext = "jar"; auto at = n.find('@');
    if (at != std::string::npos) { ext = n.substr(at + 1); n = n.substr(0, at); }
    auto p = Split(n, ':'); if (p.size() < 3) return "";
    std::string g = p[0]; std::replace(g.begin(), g.end(), '.', '/');
    std::string f = p[1] + "-" + p[2]; if (p.size() > 3) f += "-" + p[3];
    return g + "/" + p[1] + "/" + p[2] + "/" + f + "." + ext;
}
static std::string LibKey(const std::string& n) { auto p = Split(n, ':'); if (p.size() < 2) return n; return p[0] + ":" + p[1] + (p.size() > 3 ? ":" + p[3] : ""); }

struct Prep { std::string cp, assetsId, gameAssets; fs::path natives; };
static void Prepare(const json& v, const std::string& launchId, const fs::path& gameDir, Prep& o) {
    std::string base = v.value("_base", launchId);
    fs::path jar = gRoot / "versions" / P(base) / P(base + ".jar");
    if (v.contains("downloads") && v["downloads"].contains("client")) {
        Status("Проверяю клиент " + base);
        if (!Download(v["downloads"]["client"].value("url", ""), jar)) throw std::runtime_error("Не удалось скачать клиент игры");
    }
    std::vector<Job> jobs; std::vector<fs::path> nativeJars; std::set<std::string> seen; std::vector<std::string> cp;
    for (auto& lib : v["libraries"]) {
        if (!RuleOk(lib.contains("rules") ? lib["rules"] : json())) continue;
        std::string name = lib.value("name", ""), path, url;
        bool hasDl = lib.contains("downloads");
        if (hasDl && lib["downloads"].contains("artifact")) { auto& a = lib["downloads"]["artifact"]; path = a.value("path", ""); url = a.value("url", ""); }
        else if (!lib.contains("natives") && !name.empty()) { path = MavenPath(name); url = lib.value("url", "https://libraries.minecraft.net/") + path; }
        if (!path.empty() && seen.insert(name.empty() ? path : LibKey(name)).second) {
            fs::path f = gRoot / "libraries" / P(path);
            if (!fs::exists(f) && !url.empty()) jobs.push_back({url, f});
            cp.push_back(PS(f));
        }
        if (lib.contains("natives") && lib["natives"].contains("windows") && hasDl && lib["downloads"].contains("classifiers")) {
            std::string c = lib["natives"]["windows"].get<std::string>();
            auto k = c.find("${arch}"); if (k != std::string::npos) c.replace(k, 7, "64");
            auto& cls = lib["downloads"]["classifiers"];
            if (cls.contains(c)) {
                fs::path f = gRoot / "libraries" / P(cls[c].value("path", ""));
                if (!fs::exists(f)) jobs.push_back({cls[c].value("url", ""), f});
                nativeJars.push_back(f);
            }
        }
    }
    Status("Скачиваю библиотеки: " + std::to_string(jobs.size()) + " файлов");
    DownloadAll(jobs, 0.05f, 0.3f); CheckCancel();
    o.natives = gRoot / "versions" / P(launchId) / "natives";
    std::error_code ec; fs::create_directories(o.natives, ec);
    for (auto& nj : nativeJars) Unzip(nj, o.natives, true);

    // ассеты
    o.assetsId = v["assetIndex"].value("id", "legacy");
    fs::path ip = gRoot / "assets" / "indexes" / P(o.assetsId + ".json");
    Download(v["assetIndex"].value("url", ""), ip);
    json idx = Parse(ReadAll(ip));
    o.gameAssets = PS(gRoot / "assets");
    if (idx.contains("objects")) {
        bool virt = idx.value("virtual", false), mapRes = idx.value("map_to_resources", false);
        std::vector<Job> aj;
        for (auto it = idx["objects"].begin(); it != idx["objects"].end(); ++it) {
            std::string h = it.value().value("hash", ""); if (h.size() < 2) continue;
            fs::path d = gRoot / "assets" / "objects" / h.substr(0, 2) / h;
            if (!fs::exists(d, ec)) aj.push_back({"https://resources.download.minecraft.net/" + h.substr(0, 2) + "/" + h, d});
        }
        Status("Скачиваю ресурсы игры: " + std::to_string(aj.size()) + " файлов");
        DownloadAll(aj, 0.3f, 0.95f); CheckCancel();
        if (virt || mapRes) {
            fs::path dir = virt ? gRoot / "assets" / "virtual" / "legacy" : gameDir / "resources";
            for (auto it = idx["objects"].begin(); it != idx["objects"].end(); ++it) {
                std::string h = it.value().value("hash", ""); if (h.size() < 2) continue;
                fs::path dst = dir / P(it.key());
                if (!fs::exists(dst, ec)) { fs::create_directories(dst.parent_path(), ec); fs::copy_file(gRoot / "assets" / "objects" / h.substr(0, 2) / h, dst, ec); }
            }
            if (virt) o.gameAssets = PS(dir);
        }
    }
    cp.push_back(PS(jar));
    for (size_t i = 0; i < cp.size(); i++) o.cp += (i ? ";" : "") + cp[i];
}

// ============================ Fabric / Forge ============================
static std::string FindInstalled(const std::string& prefix, const std::string& suffix) {
    std::error_code ec;
    for (auto& e : fs::directory_iterator(gRoot / "versions", ec)) {
        std::string n = U8(e.path().filename().wstring());
        if (n.rfind(prefix, 0) == 0 && n.size() >= suffix.size() && n.compare(n.size() - suffix.size(), suffix.size(), suffix) == 0 && fs::exists(e.path() / (e.path().filename().wstring() + L".json"), ec)) return n;
    }
    return "";
}
static std::vector<std::string> LoaderProfiles(const std::string& loader, const std::string& mc) { // установленные профили загрузчика для версии игры
    std::vector<std::string> r; std::error_code ec;
    for (auto& e : fs::directory_iterator(gRoot / "versions", ec)) {
        if (!e.is_directory(ec)) continue;
        std::string n = U8(e.path().filename().wstring()); bool ok = false;
        auto starts = [&](const std::string& p) { return n.rfind(p, 0) == 0; };
        auto ends = [&](const std::string& x) { return n.size() >= x.size() && n.compare(n.size() - x.size(), x.size(), x) == 0; };
        if (loader == "Fabric") ok = starts("fabric-loader-") && ends("-" + mc);
        else if (loader == "Quilt") ok = starts("quilt-loader-") && ends("-" + mc);
        else if (loader == "Forge") ok = starts(mc + "-forge-");
        else if (loader == "OptiFine") ok = starts(mc + "-OptiFine_");
        else if (loader == "NeoForge" && starts("neoforge-")) {
            json j = Parse(ReadAll(e.path() / (e.path().filename().wstring() + L".json")));
            ok = j.is_object() && j.contains("inheritsFrom") && j["inheritsFrom"].is_string() && j["inheritsFrom"].get<std::string>() == mc;
        }
        if (ok) r.push_back(n);
    }
    return r;
}
static std::string InstallFabric(const std::string& mc) {
    Status("Устанавливаю Fabric...");
    Resp r = Http("https://meta.fabricmc.net/v2/versions/loader");
    json j = Parse(r.body); std::string loader;
    if (j.is_array()) for (auto& x : j) if (x.value("stable", false)) { loader = x.value("version", ""); break; }
    if (loader.empty()) return FindInstalled("fabric-loader-", "-" + mc);
    r = Http("https://meta.fabricmc.net/v2/versions/loader/" + mc + "/" + loader + "/profile/json");
    json pj = Parse(r.body);
    if (r.status != 200 || !pj.contains("id")) throw std::runtime_error("Fabric пока не поддерживает версию " + mc);
    std::string id = pj["id"].get<std::string>();
    WriteAll(gRoot / "versions" / P(id) / P(id + ".json"), r.body);
    return id;
}
static int RunProcess(const std::string& cmd, const fs::path& cwd, std::string& captured);
static std::string InstallForge(const std::string& mc, const std::string& javaExe) {
    Status("Устанавливаю Forge (первый раз это долго)...");
    Resp r = Http("https://files.minecraftforge.net/net/minecraftforge/forge/promotions_slim.json");
    json j = Parse(r.body); std::string fv;
    if (j.contains("promos")) {
        auto& pr = j["promos"];
        if (pr.contains(mc + "-recommended")) fv = pr[mc + "-recommended"].get<std::string>();
        else if (pr.contains(mc + "-latest")) fv = pr[mc + "-latest"].get<std::string>();
    }
    if (fv.empty()) { std::string have = FindInstalled(mc + "-forge-", ""); if (!have.empty()) return have; throw std::runtime_error("Для Minecraft " + mc + " не нашёл Forge"); }
    std::string id = mc + "-forge-" + fv;
    std::error_code ec;
    if (fs::exists(gRoot / "versions" / P(id) / P(id + ".json"), ec)) return id;
    std::string full = mc + "-" + fv;
    fs::path inst = gRoot / "cache" / P("forge-" + full + "-installer.jar");
    if (!Download("https://maven.minecraftforge.net/net/minecraftforge/forge/" + full + "/forge-" + full + "-installer.jar", inst))
        throw std::runtime_error("Не скачался установщик Forge");
    if (!fs::exists(gRoot / "launcher_profiles.json")) WriteAll(gRoot / "launcher_profiles.json", "{\"profiles\":{}}");
    std::string cap;
    int code = RunProcess(Q(javaExe) + " -jar " + Q(PS(inst)) + " --installClient " + Q(PS(gRoot)), gRoot, cap);
    if (!fs::exists(gRoot / "versions" / P(id) / P(id + ".json"), ec)) {
        std::string have = FindInstalled(mc + "-forge-", "");
        if (!have.empty()) return have;
        throw std::runtime_error("Установщик Forge завершился с кодом " + std::to_string(code) + ". Подробности: Настройки - Общие - «Посмотреть лог» (файл logs/latest.log).");
    }
    return id;
}

static std::string InstallNeoForge(const std::string& mc, const std::string& javaExe) {
    Status("Устанавливаю NeoForge (первый раз это долго)...");
    std::string fv;
    for (auto& v : NeoVersions()) if (NeoMcOf(v) == mc) { bool st = v.find('-') == std::string::npos; if (st || fv.empty() || fv.find('-') != std::string::npos) fv = v; }
    if (fv.empty()) { auto have = LoaderProfiles("NeoForge", mc); if (!have.empty()) return have[0]; throw std::runtime_error("Для Minecraft " + mc + " не нашёл NeoForge (поддерживается 1.20.2 и новее)"); }
    std::string id = "neoforge-" + fv; std::error_code ec;
    if (fs::exists(gRoot / "versions" / P(id) / P(id + ".json"), ec)) return id;
    fs::path inst = gRoot / "cache" / P("neoforge-" + fv + "-installer.jar");
    if (!Download("https://maven.neoforged.net/releases/net/neoforged/neoforge/" + fv + "/neoforge-" + fv + "-installer.jar", inst)) throw std::runtime_error("Не скачался установщик NeoForge");
    if (!fs::exists(gRoot / "launcher_profiles.json")) WriteAll(gRoot / "launcher_profiles.json", "{\"profiles\":{}}");
    std::string cap; int code = RunProcess(Q(javaExe) + " -jar " + Q(PS(inst)) + " --install-client " + Q(PS(gRoot)), gRoot, cap);
    if (!fs::exists(gRoot / "versions" / P(id) / P(id + ".json"), ec)) { cap.clear(); code = RunProcess(Q(javaExe) + " -jar " + Q(PS(inst)) + " --installClient " + Q(PS(gRoot)), gRoot, cap); }
    if (!fs::exists(gRoot / "versions" / P(id) / P(id + ".json"), ec)) {
        auto have = LoaderProfiles("NeoForge", mc); if (!have.empty()) return have[0];
        throw std::runtime_error("Установщик NeoForge завершился с кодом " + std::to_string(code) + ". Подробности: Настройки - Общие - «Посмотреть лог» (файл logs/latest.log).");
    }
    return id;
}
static std::string InstallQuilt(const std::string& mc) {
    Status("Устанавливаю Quilt...");
    Resp r = Http("https://meta.quiltmc.org/v3/versions/loader");
    json j = Parse(r.body); std::string loader;
    if (j.is_array()) for (auto& x : j) if (x.is_object() && x.contains("version") && x["version"].is_string()) { std::string v = x["version"].get<std::string>(); if (!v.empty() && v.find('-') == std::string::npos) { loader = v; break; } }
    if (loader.empty()) return FindInstalled("quilt-loader-", "-" + mc);
    r = Http("https://meta.quiltmc.org/v3/versions/loader/" + mc + "/" + loader + "/profile/json");
    json pj = Parse(r.body);
    if (r.status != 200 || !pj.is_object() || !pj.contains("id")) throw std::runtime_error("Quilt пока не поддерживает версию " + mc);
    std::string id = pj["id"].get<std::string>();
    WriteAll(gRoot / "versions" / P(id) / P(id + ".json"), r.body);
    return id;
}
static bool ExtractZipPrefix(const fs::path& zip, const std::string& prefix, const std::string& suffix, std::string& outName, std::string& outData) {
    std::string data = ReadAll(zip); mz_zip_archive z{};
    if (data.empty() || !mz_zip_reader_init_mem(&z, data.data(), data.size(), 0)) return false;
    bool found = false;
    for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&z) && !found; i++) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&z, i, &st) || mz_zip_reader_is_file_a_directory(&z, i)) continue;
        std::string n = st.m_filename;
        if (n.rfind(prefix, 0) == 0 && n.size() > suffix.size() && n.compare(n.size() - suffix.size(), suffix.size(), suffix) == 0) {
            size_t sz = 0; void* p = mz_zip_reader_extract_to_heap(&z, i, &sz, 0);
            if (p) { outName = n; outData.assign((const char*)p, sz); mz_free(p); found = true; }
        }
    }
    mz_zip_reader_end(&z); return found;
}
static bool ZipHasEntry(const fs::path& zip, const char* name) {
    std::string data = ReadAll(zip); mz_zip_archive z{};
    if (data.empty() || !mz_zip_reader_init_mem(&z, data.data(), data.size(), 0)) return false;
    bool ok = mz_zip_reader_locate_file(&z, name, nullptr, 0) >= 0;
    mz_zip_reader_end(&z); return ok;
}
// OptiFine: скачиваем jar с optifine.net (запасной вариант — зеркало BMCLAPI), прогоняем optifine.Patcher над ванильным клиентом и пишем профиль версии (как делает установщик OptiFine)
// want — выбранная сборка ("HD_U_I6"); пусто = автоматически лучшая
static std::string InstallOptiFine(const std::string& mc, const std::string& javaExe, const json& parent, const std::string& want = "") {
    Status("Устанавливаю OptiFine...");
    std::vector<OfV> cands;
    { std::lock_guard<std::mutex> l(gVerMu); auto it = gOptiBuilds.find(mc); if (it != gOptiBuilds.end()) cands = it->second; }
    if (cands.empty()) { // список ещё не загрузился или раньше не было сети
        auto ob = FetchOptiAll();
        { std::lock_guard<std::mutex> l(gVerMu); if (!ob.empty()) gOptiBuilds = ob; }
        auto it = ob.find(mc); if (it != ob.end()) cands = it->second;
    }
    if (cands.empty()) { // последний шанс: прямой запрос к зеркалу по версии игры
        Resp r = Http("https://bmclapi2.bangbang93.com/optifine/" + mc); json j = Parse(r.body);
        if (j.is_array()) for (auto& x : j) {
            if (!x.is_object() || !x.contains("type") || !x.contains("patch") || !x["type"].is_string() || !x["patch"].is_string()) continue;
            OfV o{x["type"].get<std::string>(), x["patch"].get<std::string>()};
            if (!o.type.empty() && !o.patch.empty()) OfAdd(*std::make_unique<std::map<std::string, std::vector<OfV>>>().get(), mc, o), cands.push_back(o);
        }
    }
    CheckCancel();
    std::sort(cands.begin(), cands.end(), [](const OfV& x, const OfV& y) { return OfBetter(x, y); }); // лучшие первыми
    if (!want.empty()) { std::vector<OfV> w; for (auto& c : cands) if (OfKey(c) == want) w.push_back(c); if (!w.empty()) cands = w; } // выбрана конкретная сборка
    if (cands.empty()) {
        auto have = LoaderProfiles("OptiFine", mc); if (!have.empty()) return have[0];
        throw std::runtime_error("Не получил список OptiFine для Minecraft " + mc + " (интернет / VPN / фаервол) или для этой версии OptiFine нет");
    }
    std::error_code ec;
    for (auto& c : cands) { std::string idc = mc + "-OptiFine_" + c.type + "_" + c.patch; if (fs::exists(gRoot / "versions" / P(idc) / P(idc + ".json"), ec)) return idc; }
    std::optional<OfV> best; std::string full, id, ofUrl, netWhy, curFile; fs::path inst;
    auto fetchOf = [&](bool force) {
        if (!force && fs::exists(inst, ec) && fs::file_size(inst, ec) > 0 && ZipHasEntry(inst, "optifine/Patcher.class")) return true;
        for (int a = 0; a < 4; a++) { // попытки 0-1: optifine.net, 2-3: зеркало BMCLAPI
            CheckCancel();
            bool official = a < 2; int tryNo = a % 2 + 1; const char* src = official ? "optifine.net" : "зеркало";
            Status("Скачиваю OptiFine " + full + " (" + src + ", попытка " + std::to_string(tryNo) + "/2)...");
            std::string why; Resp rr = official ? OfHttpOfficial(curFile, why) : Http(ofUrl);
            if (rr.status == 200 && !rr.body.empty()) {
                WriteAll(inst, rr.body);
                if (ZipHasEntry(inst, "optifine/Patcher.class")) return true;
                std::string head = rr.body.substr(0, 60); for (char& ch : head) if ((unsigned char)ch < 32 || (unsigned char)ch > 126) ch = '.';
                netWhy = std::string(src) + ": ответ не похож на jar (" + std::to_string(rr.body.size()) + " байт: " + head + ")";
            } else netWhy = std::string(src) + ": " + (!why.empty() ? why : gNetErr.empty() ? ("HTTP " + std::to_string(rr.status)) : gNetErr);
            Log("[OptiFine] " + full + " - " + netWhy);
            if (rr.status == 404) { if (official) { a = 1; continue; } break; } // на сайте нет — сразу на зеркало; на зеркале нет — дальше повторять бессмысленно
            Sleep(500);
        }
        return false;
    };
    { // перебираем сборки от лучшей к худшей, пока какая-то не скачается
        std::string allWhy; int tried = 0;
        for (auto& c : cands) {
            if (tried++ >= 8) break;
            full = mc + "_" + c.type + "_" + c.patch; id = mc + "-OptiFine_" + c.type + "_" + c.patch; curFile = OfFile(mc, c);
            ofUrl = "https://bmclapi2.bangbang93.com/optifine/" + mc + "/" + c.type + "/" + c.patch;
            inst = gRoot / "cache" / P("OptiFine_" + full + ".jar"); fs::create_directories(inst.parent_path(), ec);
            if (fetchOf(false)) { best = c; break; }
            fs::remove(inst, ec); allWhy += (allWhy.empty() ? "" : "; ") + c.type + "_" + c.patch + ": " + netWhy;
        }
        if (!best) throw std::runtime_error("Не скачался OptiFine для " + mc + " (" + allWhy + ")");
    }
    fs::path base = gRoot / "versions" / P(mc) / P(mc + ".jar");
    if (!fs::exists(base, ec) || fs::file_size(base, ec) == 0) throw std::runtime_error("Нет ванильного клиента " + mc + " для патча OptiFine");
    fs::path target = gRoot / "libraries" / "optifine" / "OptiFine" / P(full) / P("OptiFine-" + full + ".jar");
    fs::create_directories(target.parent_path(), ec);
    auto runPatch = [&](const std::string& jx, std::string& cap) {
        fs::remove(target, ec); cap.clear();
        int c = RunProcess(Q(jx) + " -Djava.awt.headless=true -cp " + Q(PS(inst)) + " optifine.Patcher " + Q(PS(base)) + " " + Q(PS(inst)) + " " + Q(PS(target)), gRoot, cap);
        return c;
    };
    auto patched = [&]() { std::error_code e2; return fs::exists(target, e2) && fs::file_size(target, e2) > 0; };
    std::string cap; int code = runPatch(javaExe, cap); CheckCancel();
    if (!patched() && cap.find("UnsupportedClassVersionError") != std::string::npos) { // Patcher собран под более новую Java
        size_t pv = cap.find("class file version "); int need = pv != std::string::npos ? atoi(cap.c_str() + pv + 19) - 44 : 0;
        if (need < 8) need = 21;
        std::string j2 = EnsureJava(need);
        if (!j2.empty()) { j2 = JavaExe(j2); code = runPatch(j2, cap); }
    }
    CheckCancel();
    if (!patched()) { // возможно, битый кэш: качаем заново и пробуем ещё раз
        if (fetchOf(true)) code = runPatch(javaExe, cap);
    }
    if (!patched()) {
        fs::remove(target, ec);
        std::string tail = cap; while (!tail.empty() && (tail.back() == '\n' || tail.back() == '\r' || tail.back() == ' ')) tail.pop_back();
        if (tail.size() > 300) tail = tail.substr(tail.size() - 300);
        throw std::runtime_error("Не удалось применить OptiFine (код " + std::to_string(code) + "). " + tail);
    }
    json libs = json::array(); { json o; o["name"] = "optifine:OptiFine:" + full; libs.push_back(o); }
    std::string lwName, lwData;
    if (ExtractZipPrefix(inst, "launchwrapper-of-", ".jar", lwName, lwData)) {
        std::string lv = lwName.substr(17, lwName.size() - 17 - 4);
        WriteAll(gRoot / "libraries" / "optifine" / "launchwrapper-of" / P(lv) / P("launchwrapper-of-" + lv + ".jar"), lwData);
        json o; o["name"] = "optifine:launchwrapper-of:" + lv; libs.push_back(o);
    } else { json o; o["name"] = "net.minecraft:launchwrapper:1.12"; libs.push_back(o); }
    json vj; vj["id"] = id; vj["inheritsFrom"] = mc; vj["mainClass"] = "net.minecraft.launchwrapper.Launch"; vj["libraries"] = libs;
    if (parent.contains("arguments")) vj["arguments"]["game"] = json::array({"--tweakClass", "optifine.OptiFineTweaker"});
    else {
        std::string ma = (parent.contains("minecraftArguments") && parent["minecraftArguments"].is_string()) ? parent["minecraftArguments"].get<std::string>() : std::string();
        vj["minecraftArguments"] = ma + " --tweakClass optifine.OptiFineTweaker";
    }
    WriteAll(gRoot / "versions" / P(id) / P(id + ".json"), vj.dump(2));
    return id;
}

// ============================ Выбор видеокарты (Windows) ============================
static void ApplyGpuPref(const std::string& exe, int mode) { // 0 авто, 1 встроенная (экономия), 2 дискретная (производительность)
    HKEY k; if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\DirectX\\UserGpuPreference", 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS) return;
    std::wstring name = W(exe);
    if (mode == 0) RegDeleteValueW(k, name.c_str());
    else { std::wstring v = mode == 2 ? L"GpuPreference=2;" : L"GpuPreference=1;"; RegSetValueExW(k, name.c_str(), 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t))); }
    RegCloseKey(k);
}

// ============================ Запуск процесса и разбор крашей ============================
static int RunProcess(const std::string& cmd, const fs::path& cwd, std::string& captured) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{}; si.cb = sizeof(si); si.dwFlags = STARTF_USESTDHANDLES; si.hStdOutput = wr; si.hStdError = wr;
    PROCESS_INFORMATION pi{};
    std::wstring cl = W(cmd);
    BOOL ok = CreateProcessW(nullptr, &cl[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, cwd.wstring().c_str(), &si, &pi);
    CloseHandle(wr);
    if (!ok) { CloseHandle(rd); captured = "CreateProcess failed: " + std::to_string(GetLastError()); return -1; }
    HANDLE job = CreateJobObjectW(nullptr, nullptr); // в одну «группу» с дочерними процессами: «Остановить/Закрыть» убирает всё дерево
    bool inJob = job && AssignProcessToJobObject(job, pi.hProcess);
    { std::lock_guard<std::mutex> l(gProcMu); gChildPid = pi.dwProcessId; gChildProc = pi.hProcess; gChildJob = inJob ? job : nullptr; }
    ResumeThread(pi.hThread);
    if (gCancel) ProcHardKill(pi.dwProcessId);
    std::string part; char buf[4096]; DWORD n;
    while (ReadFile(rd, buf, sizeof(buf), &n, nullptr) && n) {
        part.append(buf, n); size_t p;
        while ((p = part.find('\n')) != std::string::npos) {
            std::string line = part.substr(0, p); if (!line.empty() && line.back() == '\r') line.pop_back();
            Log(line); captured += line + "\n"; part.erase(0, p + 1);
        }
        if (captured.size() > 400000) captured.erase(0, 200000);
    }
    if (!part.empty()) { Log(part); captured += part; }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0; GetExitCodeProcess(pi.hProcess, &code);
    { std::lock_guard<std::mutex> l(gProcMu); gChildPid = 0; gChildProc = nullptr; gChildJob = nullptr; }
    if (job) CloseHandle(job);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread); CloseHandle(rd);
    return (int)code;
}
static std::pair<std::string, std::string> Analyze(int code, const std::string& t) {
    auto has = [&](const char* s) { return t.find(s) != std::string::npos; };
    if (has("UnsupportedClassVersionError") || has("class file version")) {
        int need = 0; auto p = t.find("class file version "); if (p != std::string::npos) { int v = atoi(t.c_str() + p + 19); need = v > 44 ? v - 44 : 0; }
        return {"Не та версия Java", "Игре или моду нужна Java " + (need ? std::to_string(need) : std::string("новее")) + ". В «Настройках» выбери «Авто» или подходящую Java."};
    }
    if (has("OutOfMemoryError") || has("Java heap space") || has("Could not reserve enough space"))
        return {"Не хватает памяти", "Игре не хватило оперативной памяти. Подними лимит в «Настройках» (для модов обычно 4–6 ГБ) и закрой тяжёлые программы. Если Java 32-бит — поставь 64-бит."};
    if (has("Unrecognized VM option") || has("Could not create the Java Virtual Machine") || has("Invalid maximum heap size"))
        return {"Java не смогла стартовать", "JVM отказалась запускаться: проблема в аргументах или в 32-битной Java. Попробуй «Авто» для Java и меньше ОЗУ."};
    if (has("Found duplicate mods") || has("DuplicateModsFoundException") || has("Duplicate mod"))
        return {"Дубли модов", "В папке mods лежат две копии одного мода. Оставь одну."};
    if (has("Incompatible mod set") || has("Mod resolution failed") || has("Missing or unsupported mandatory dependencies") || has("requires") && has("mod"))
        return {"Конфликт или нехватка зависимостей модов", "Какому-то моду нужна другая версия игры/загрузчика или другой мод (часто Fabric API). Прочитай в логе строки со словом «requires» или «Missing»."};
    if (has("Mixin apply failed") || has("MixinTransformerError") || has("InvalidInjectionException"))
        return {"Мод сломал игру (Mixin)", "Один из модов не совместим с этой версией или с другим модом. Убирай моды по одному, начиная с последних добавленных."};
    if (has("NoClassDefFoundError") || has("ClassNotFoundException"))
        return {"Не найден класс (мод/библиотека)", "Чаще всего мод собран не под эту версию игры или не под этот загрузчик (Fabric/Forge). Проверь версии модов."};
    if (has("UnsatisfiedLinkError") || has("Failed to locate library") || has("lwjgl") && has("natives"))
        return {"Сломались нативные библиотеки", "Удали папку versions/<версия>/natives в папке лаунчера и запусти заново."};
    if (has("GLFW error") || has("Pixel format not accelerated") || has("WGL") || has("Failed to create window") || has("OpenGL"))
        return {"Проблема с видеокартой / OpenGL", "Обнови драйвер видеокарты. На ноутбуке запускай Java на дискретной GPU. Не работает в удалённом рабочем столе."};
    if (has("UnknownHostException") || has("SSLHandshakeException") || has("Connection timed out") || has("Failed to download"))
        return {"Проблема с сетью", "Не получилось скачать что-то по сети. Проверь интернет, VPN, антивирус/фаервол."};
    if (has("Invalid session") || has("Failed to verify username") || has("Bad login"))
        return {"Проблема с аккаунтом", "Сессия недействительна. Перелогинься во вкладке «Аккаунт»."};
    if (code == -1073741819 || code == (int)0xC0000005)
        return {"Аварийное закрытие процесса (0xC0000005)", "Чаще всего виноваты драйвер видеокарты, оверлеи (Discord, MSI Afterburner, GeForce Experience) или антивирус. Отключи оверлеи и обнови драйвер."};
    std::string hint; size_t p = t.find("Exception in thread");
    if (p == std::string::npos) p = t.find("Caused by:");
    if (p != std::string::npos) { size_t e = t.find('\n', p); hint = "\n\nПохоже на:\n" + t.substr(p, std::min<size_t>(e == std::string::npos ? 300 : e - p, 300)); }
    return {"Игра закрылась с кодом " + std::to_string(code), "Точная причина не опознана. Открой Настройки - Общие - «Посмотреть лог» и ищи строки с Exception / Error." + hint};
}

// ============================ Аккаунты ============================
static bool ValidNick(const std::string& n) {
    if (n.size() < 3 || n.size() > 16) return false;
    for (unsigned char c : n) if (!(isalnum(c) || c == '_')) return false;
    return true;
}
static const char* kNickHint = "Ник: 3-16 символов, только латиница, цифры и _";
static std::string JStr(const json& j, const char* k) { return (j.is_object() && j.contains(k) && j[k].is_string()) ? j[k].get<std::string>() : std::string(); }
static std::string FirstLine(const std::string& s) { size_t p = s.find_first_of("\r\n"); return p == std::string::npos ? s : s.substr(0, p); }
static std::string OfflineUuid(const std::string& name) {
    std::string s = "OfflinePlayer:" + name; HCRYPTPROV pv = 0; HCRYPTHASH h = 0; BYTE d[16] = {0}; DWORD n = 16;
    CryptAcquireContextW(&pv, 0, 0, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT);
    CryptCreateHash(pv, CALG_MD5, 0, 0, &h);
    CryptHashData(h, (const BYTE*)s.data(), (DWORD)s.size(), 0);
    CryptGetHashParam(h, HP_HASHVAL, d, &n, 0);
    CryptDestroyHash(h); CryptReleaseContext(pv, 0);
    d[6] = (d[6] & 0x0f) | 0x30; d[8] = (d[8] & 0x3f) | 0x80;
    static const char* hx = "0123456789abcdef"; std::string r;
    for (int i = 0; i < 16; i++) { r += hx[d[i] >> 4]; r += hx[d[i] & 15]; }
    return r;
}
static Account MsChain(const std::string& msTok, const std::string& refresh) {
    json b1 = {{"Properties", {{"AuthMethod", "RPS"}, {"SiteName", "user.auth.xboxlive.com"}, {"RpsTicket", "d=" + msTok}}}, {"RelyingParty", "http://auth.xboxlive.com"}, {"TokenType", "JWT"}};
    Resp r = PostJson("https://user.auth.xboxlive.com/user/authenticate", b1);
    if (r.status == 0) throw std::runtime_error("Нет соединения с Xbox Live (интернет / VPN / фаервол)");
    json x = Parse(r.body);
    if (!x.contains("Token")) throw std::runtime_error("Xbox Live: ошибка " + std::to_string(r.status));
    std::string xbl = x["Token"].get<std::string>(), uhs = x["DisplayClaims"]["xui"][0]["uhs"].get<std::string>();
    json b2 = {{"Properties", {{"SandboxId", "RETAIL"}, {"UserTokens", json::array({xbl})}}}, {"RelyingParty", "rp://api.minecraftservices.com/"}, {"TokenType", "JWT"}};
    r = PostJson("https://xsts.auth.xboxlive.com/xsts/authorize", b2);
    x = Parse(r.body);
    if (!x.contains("Token")) {
        std::string why = "XSTS: ошибка " + std::to_string(r.status);
        if (x.contains("XErr")) { long long e = x["XErr"].get<long long>(); if (e == 2148916233LL) why = "У этой учётки Microsoft нет профиля Xbox"; else if (e == 2148916238LL) why = "Детская учётка: нужно разрешение родителя в семейной группе"; else if (e == 2148916235LL) why = "Xbox Live недоступен в стране этой учётки"; else if (e == 2148916236LL || e == 2148916237LL) why = "Нужна проверка возраста учётки Xbox (account.xbox.com)"; }
        throw std::runtime_error(why);
    }
    std::string xsts = x["Token"].get<std::string>();
    r = PostJson("https://api.minecraftservices.com/authentication/login_with_xbox", json{{"identityToken", "XBL3.0 x=" + uhs + ";" + xsts}});
    x = Parse(r.body);
    if (!x.contains("access_token")) {
        std::string em = JStr(x, "errorMessage");
        if (r.status == 403 || em.find("Invalid app registration") != std::string::npos)
            throw std::runtime_error("Mojang не одобрил этот Azure Client ID (Invalid app registration). Подай заявку: https://aka.ms/mce-reviewappid - до одобрения вход через Microsoft не заработает.");
        throw std::runtime_error("Minecraft Services: ошибка " + std::to_string(r.status) + (em.empty() ? "" : " (" + em + ")"));
    }
    std::string tok = x["access_token"].get<std::string>();
    r = Http("https://api.minecraftservices.com/minecraft/profile", "GET", "", "Authorization: Bearer " + tok + "\r\n");
    x = Parse(r.body);
    if (!x.contains("name")) throw std::runtime_error("На этой учётке нет купленной Minecraft Java Edition");
    Account a; a.type = 2; a.name = x["name"].get<std::string>(); a.uuid = x["id"].get<std::string>(); a.token = tok; a.refresh = refresh;
    return a;
}
static void MsRefresh(Account& a) {
    if (a.refresh.empty() || MsId().empty()) throw std::runtime_error("Нужно заново войти через Microsoft");
    Resp r = Http("https://login.microsoftonline.com/consumers/oauth2/v2.0/token", "POST",
                  Form({{"client_id", MsId()}, {"grant_type", "refresh_token"}, {"refresh_token", a.refresh}, {"scope", "XboxLive.signin offline_access"}}), kFormHdr);
    json j = Parse(r.body);
    if (!j.contains("access_token")) throw std::runtime_error("Сессия Microsoft истекла — войди заново");
    a = MsChain(j["access_token"].get<std::string>(), j.value("refresh_token", a.refresh));
}
static void MsLoginThread() {
    if (gAuthBusy.exchange(true)) return;
    struct Done { ~Done() { gAuthBusy = false; } } done_;
    try {
        if (MsId().empty()) throw std::runtime_error("Не задан Azure Client ID — вставь его в поле на вкладке «Аккаунт»");
        AuthMsg("Microsoft: запрашиваю код...");
        Resp r = Http("https://login.microsoftonline.com/consumers/oauth2/v2.0/devicecode", "POST",
                      Form({{"client_id", MsId()}, {"scope", "XboxLive.signin offline_access"}}), kFormHdr);
        json d = Parse(r.body);
        if (!d.contains("device_code")) {
            std::string ed = FirstLine(JStr(d, "error_description"));
            throw std::runtime_error("Microsoft не выдал код" + (ed.empty() ? std::string(". Проверь Client ID.") : ": " + ed));
        }
        std::string uri = d.value("verification_uri", "https://microsoft.com/link");
        { std::lock_guard<std::mutex> l(gMu); gMsCode = d.value("user_code", ""); gMsUri = uri; }
        ShellExecuteW(0, L"open", W(uri).c_str(), 0, 0, SW_SHOWNORMAL);
        int interval = d.value("interval", 5), left = d.value("expires_in", 900);
        while (left > 0) {
            Sleep(interval * 1000); left -= interval;
            Resp t = Http("https://login.microsoftonline.com/consumers/oauth2/v2.0/token", "POST",
                          Form({{"client_id", MsId()}, {"grant_type", "urn:ietf:params:oauth:grant-type:device_code"}, {"device_code", d["device_code"].get<std::string>()}}), kFormHdr);
            json j = Parse(t.body);
            if (j.contains("access_token")) {
                AuthMsg("Microsoft: вхожу в Xbox / Minecraft...");
                Account a = MsChain(j["access_token"].get<std::string>(), j.value("refresh_token", ""));
                { std::lock_guard<std::mutex> l(gMu); gNewAcc = a; gMsCode.clear(); }
                AuthMsg("Вошёл как " + a.name); return;
            }
            std::string e = j.value("error", "");
            if (e == "slow_down") interval += 5;
            else if (e != "authorization_pending") { std::string ed = FirstLine(JStr(j, "error_description")); throw std::runtime_error("Вход отменён или истёк (" + e + ")" + (ed.empty() ? "" : ": " + ed)); }
        }
        throw std::runtime_error("Время ожидания кода вышло");
    } catch (std::exception& e) { { std::lock_guard<std::mutex> l(gMu); gMsCode.clear(); } AuthMsg(std::string("Ошибка входа: ") + e.what()); }
}
static void ElyRefresh(Account& a) {
    Resp r = PostJson("https://authserver.ely.by/auth/validate", json{{"accessToken", a.token}});
    if (r.status == 200 || r.status == 204 || r.status == 0) return; // валиден (или нет сети - пробуем как есть)
    if (a.refresh.empty()) throw std::runtime_error("Сессия Ely.by истекла - войди заново во вкладке «Аккаунт»");
    r = PostJson("https://authserver.ely.by/auth/refresh", json{{"accessToken", a.token}, {"clientToken", a.refresh}});
    json j = Parse(r.body);
    if (!j.contains("accessToken")) throw std::runtime_error("Сессия Ely.by истекла - войди заново во вкладке «Аккаунт»");
    a.token = j["accessToken"].get<std::string>();
}
static void ElyLoginThread(std::string u, std::string p) {
    if (gAuthBusy.exchange(true)) return;
    struct Done { ~Done() { gAuthBusy = false; } } done_;
    try {
        AuthMsg("Ely.by: вхожу...");
        std::string ct = RandHex(16);
        Resp r = PostJson("https://authserver.ely.by/auth/authenticate", json{{"username", u}, {"password", p}, {"clientToken", ct}, {"requestUser", true}});
        if (r.status == 0) throw std::runtime_error("Нет соединения с authserver.ely.by (интернет / VPN / фаервол)");
        json j = Parse(r.body);
        if (!j.contains("accessToken")) {
            std::string em = JStr(j, "errorMessage");
            if (em.find("two factor") != std::string::npos || em.find("two-factor") != std::string::npos) em = "включена 2FA - пиши в поле пароля «пароль:код»";
            throw std::runtime_error("Ely.by: " + (em.empty() ? "ошибка " + std::to_string(r.status) : em));
        }
        if (!j.contains("selectedProfile") || !j["selectedProfile"].is_object()) throw std::runtime_error("У аккаунта Ely.by нет выбранного профиля");
        Account a; a.type = 1; a.token = j["accessToken"].get<std::string>(); a.refresh = ct;
        a.name = j["selectedProfile"].value("name", u); a.uuid = j["selectedProfile"].value("id", "");
        { std::lock_guard<std::mutex> l(gMu); gNewAcc = a; }
        AuthMsg("Вошёл как " + a.name + " (Ely.by)");
    } catch (std::exception& e) { AuthMsg(std::string("Ошибка входа: ") + e.what()); }
}

// ============================ Конфиг ============================
struct Config {
    std::string loader = "Vanilla", version, javaPath, bg, shotsDir;
    std::string optiBuild; // выбранная сборка OptiFine ("HD_U_I6"), пусто = автоматически лучшая
    std::string buildId;   // папка выбранной сборки в instances/ (loader/version/optiBuild — копия её настроек)
    std::string gameDir;   // пусто = стандартная
    std::string lastNick, skinPath, capePath; // ник последнего запуска (для приветствия), свои файлы скина и плаща
    bool snapshots = false, showBeta = false, showAlpha = false, slideshow = true, skinSlim = false, showCape = true;
    int ramMB = 4096, sel = -1, onLaunch = 0; // onLaunch: 0 оставить, 1 свернуть, 2 закрыть
    int lang = 0, theme = 0, gpu = 0;         // язык (0 ru, 1 en), тема оформления (0..4), видеокарта
    int style = 1, home = 1;                  // стиль: 0 Классический, 1 Жидкое стекло; главный экран: 0 Библиотека, 1 Игровой
    bool charScene = true; std::string charBg; // сцена за персонажем и своя картинка вместо неё
    bool rounded = true, modsOn = true, logsOn = true;
    long long playSec = 0; int launches = 0;  // общая статистика
    std::string jvmArgs;
    std::vector<Account> accs;
};
static Config cfg;
static std::atomic<long long> gStatSec{0}; static std::atomic<int> gStatLaunch{0}; // добавки к статистике из потока запуска
static void SaveConfig() {
    json j; j["loader"] = cfg.loader; j["version"] = cfg.version; j["java"] = cfg.javaPath; j["bg"] = cfg.bg; j["shots"] = cfg.shotsDir;
    j["snap"] = cfg.snapshots; j["slide"] = cfg.slideshow; j["ram"] = cfg.ramMB; j["sel"] = cfg.sel; j["msid"] = MsId(); j["gamedir"] = cfg.gameDir; j["beta"] = cfg.showBeta; j["alpha"] = cfg.showAlpha; j["onlaunch"] = cfg.onLaunch;
    j["style"] = cfg.style; j["home"] = cfg.home; j["scene"] = cfg.charScene; j["charbg"] = cfg.charBg; j["lang"] = cfg.lang; j["theme"] = cfg.theme; j["gpu"] = cfg.gpu; j["round"] = cfg.rounded; j["mods"] = cfg.modsOn; j["logs"] = cfg.logsOn; j["jvm"] = cfg.jvmArgs; j["ofbuild"] = cfg.optiBuild;
    j["build"] = cfg.buildId; j["lastnick"] = cfg.lastNick; j["skin"] = cfg.skinPath; j["cape"] = cfg.capePath; j["slim"] = cfg.skinSlim; j["showcape"] = cfg.showCape; j["playsec"] = cfg.playSec; j["launches"] = cfg.launches;
    for (auto& a : cfg.accs) j["accs"].push_back({{"type", a.type}, {"name", a.name}, {"uuid", a.uuid}, {"token", a.token}, {"refresh", a.refresh}});
    WriteAll(gCfgDir / "config.json", j.dump(2));
}
static void LoadConfig() {
    json j = Parse(ReadAll(gCfgDir / "config.json")); if (!j.is_object()) return;
    try {
        cfg.loader = j.value("loader", "Vanilla"); cfg.version = j.value("version", ""); cfg.javaPath = j.value("java", ""); cfg.bg = j.value("bg", ""); cfg.shotsDir = j.value("shots", "");
        cfg.snapshots = j.value("snap", false); cfg.slideshow = j.value("slide", true); cfg.ramMB = j.value("ram", 4096); cfg.sel = j.value("sel", -1);
        { std::string id = j.value("msid", ""); if (!id.empty()) SetMsId(id); }
        cfg.gameDir = j.value("gamedir", ""); cfg.showBeta = j.value("beta", false); cfg.showAlpha = j.value("alpha", false); cfg.onLaunch = j.value("onlaunch", 0);
        cfg.style = j.value("style", 1); cfg.home = j.value("home", 1); cfg.charScene = j.value("scene", true); cfg.charBg = j.value("charbg", ""); cfg.lang = j.value("lang", 0); cfg.gpu = j.value("gpu", 0); cfg.rounded = j.value("round", true);
        cfg.theme = j.contains("theme") ? j.value("theme", 0) : (j.value("light", false) ? 2 : 0); // старая светлая тема -> «Светлая»
        cfg.modsOn = j.value("mods", true); cfg.logsOn = j.value("logs", true); cfg.jvmArgs = j.value("jvm", ""); cfg.optiBuild = j.value("ofbuild", "");
        cfg.buildId = j.value("build", ""); cfg.lastNick = j.value("lastnick", ""); cfg.skinPath = j.value("skin", ""); cfg.capePath = j.value("cape", "");
        cfg.skinSlim = j.value("slim", false); cfg.showCape = j.value("showcape", true); cfg.playSec = j.value("playsec", 0LL); cfg.launches = j.value("launches", 0);
        if (j.contains("accs")) for (auto& a : j["accs"]) cfg.accs.push_back({a.value("type", 0), a.value("name", ""), a.value("uuid", ""), a.value("token", ""), a.value("refresh", "")});
    } catch (...) {}
    cfg.theme = std::clamp(cfg.theme, 0, 4); cfg.style = std::clamp(cfg.style, 0, 1); cfg.home = std::clamp(cfg.home, 0, 1); cfg.rounded = true; // скругления теперь всегда включены
}

// ============================ Экземпляры ============================
// Папка экземпляра зависит только от загрузчика и версии игры (а не от версии самого загрузчика):
// так моды и миры не «теряются» при обновлении Fabric/Forge и лежат ровно там, откуда стартует игра.
static const char* const kLoaders[] = {"Vanilla", "Fabric", "Forge", "NeoForge", "Quilt", "OptiFine"};
static const int kNLoaders = 6;
static std::string InstName(const std::string& loader, const std::string& mc) {
    return loader == "Fabric" ? mc + "-fabric" : loader == "Forge" ? mc + "-forge" : loader == "NeoForge" ? mc + "-neoforge" :
           loader == "Quilt" ? mc + "-quilt" : loader == "OptiFine" ? mc + "-optifine" : mc;
}
static void MigrateInstances() { // старые имена (fabric-loader-0.16.9-1.21.1, 1.20.1-forge-47.2.0) -> новые
    std::error_code ec; fs::path root = gRoot / "instances"; std::vector<std::string> names;
    for (auto& e : fs::directory_iterator(root, ec)) if (e.is_directory(ec)) names.push_back(U8(e.path().filename().wstring()));
    for (auto& n : names) {
        std::string mc, ld;
        if (n.rfind("fabric-loader-", 0) == 0) { std::string rest = n.substr(14); size_t p = rest.find('-'); if (p != std::string::npos) { ld = "Fabric"; mc = rest.substr(p + 1); } }
        else { size_t p = n.find("-forge-"); if (p != std::string::npos && p > 0) { ld = "Forge"; mc = n.substr(0, p); } }
        if (ld.empty() || mc.empty()) continue;
        fs::path to = root / P(InstName(ld, mc));
        if (!fs::exists(to, ec)) fs::rename(root / P(n), to, ec);
    }
}
struct InstInfo { std::string name, loader, ver; };
static InstInfo ParseInst(const std::string& n) { // разбор старых имён папок (1.20.1-fabric, fabric-loader-..., 1.20.1-forge-...)
    InstInfo r; r.name = n; r.loader = "Vanilla"; r.ver = n;
    auto ends = [&](const char* sfx) { size_t l = strlen(sfx); return n.size() > l && n.compare(n.size() - l, l, sfx) == 0; };
    if (ends("-neoforge")) { r.loader = "NeoForge"; r.ver = n.substr(0, n.size() - 9); }
    else if (ends("-quilt")) { r.loader = "Quilt"; r.ver = n.substr(0, n.size() - 6); }
    else if (ends("-optifine")) { r.loader = "OptiFine"; r.ver = n.substr(0, n.size() - 9); }
    else if (ends("-fabric")) { r.loader = "Fabric"; r.ver = n.substr(0, n.size() - 7); }
    else if (ends("-forge")) { r.loader = "Forge"; r.ver = n.substr(0, n.size() - 6); }
    else if (n.rfind("fabric-loader-", 0) == 0) { std::string rest = n.substr(14); size_t p = rest.find('-'); r.loader = "Fabric"; r.ver = p == std::string::npos ? rest : rest.substr(p + 1); }
    else { size_t p = n.find("-forge-"); if (p != std::string::npos && p > 0) { r.loader = "Forge"; r.ver = n.substr(0, p); } }
    return r;
}
// Сборка = своя папка instances/<id> + файл ptl_build.json (название, загрузчик, версия, статистика).
struct Build { std::string id, name, loader = "Vanilla", ver, ofBuild; long long playSec = 0, lastPlayed = 0, created = 0; int launches = 0; };
static fs::path BuildDir(const std::string& id) { return gRoot / "instances" / P(id); }
static void SaveBuild(const Build& b) {
    json j; j["name"] = b.name; j["loader"] = b.loader; j["version"] = b.ver; j["ofbuild"] = b.ofBuild; j["playSec"] = b.playSec; j["launches"] = b.launches; j["lastPlayed"] = b.lastPlayed; j["created"] = b.created;
    WriteAll(BuildDir(b.id) / "ptl_build.json", j.dump(2));
}
static bool LoadBuild(const std::string& id, Build& b) {
    std::error_code ec; if (!fs::is_directory(BuildDir(id), ec)) return false;
    b = Build(); b.id = id;
    json j = Parse(ReadAll(BuildDir(id) / "ptl_build.json"));
    if (j.is_object() && j.contains("loader") && j["loader"].is_string() && j.contains("version") && j["version"].is_string()) {
        b.loader = j["loader"].get<std::string>(); b.ver = j["version"].get<std::string>(); b.name = j.value("name", ""); b.ofBuild = j.value("ofbuild", "");
        b.playSec = j.value("playSec", 0LL); b.launches = j.value("launches", 0); b.lastPlayed = j.value("lastPlayed", 0LL); b.created = j.value("created", 0LL);
    } else { // старая папка без файла сборки: создаём его (миграция с версии 0.0.5)
        InstInfo ii = ParseInst(id); b.loader = ii.loader; b.ver = ii.ver; b.name = ii.loader == "Vanilla" ? ii.ver : ii.loader + " " + ii.ver;
        b.created = (long long)time(nullptr); SaveBuild(b);
    }
    if (b.name.empty()) b.name = id;
    return true;
}
static std::vector<Build> ListBuilds() {
    std::vector<Build> v; std::error_code ec;
    for (auto& e : fs::directory_iterator(gRoot / "instances", ec)) if (e.is_directory(ec)) { Build b; if (LoadBuild(U8(e.path().filename().wstring()), b)) v.push_back(b); }
    std::sort(v.begin(), v.end(), [](const Build& a, const Build& b) { return a.created != b.created ? a.created < b.created : a.id < b.id; });
    return v;
}
static std::string NewBuildId(const std::string& loader, const std::string& ver) { // уникальное имя папки
    std::error_code ec; std::string base = InstName(loader, ver), id = base;
    for (int i = 2; fs::exists(BuildDir(id), ec); i++) id = base + "-" + std::to_string(i);
    return id;
}
static void DeleteBuild(const Build& b) { // папка сборки удаляется, общие файлы версии — только если они больше никому не нужны
    std::error_code ec; fs::path vd = gRoot / "versions";
    fs::remove_all(BuildDir(b.id), ec);
    bool sameLoader = false, sameMc = false;
    for (auto& o : ListBuilds()) if (o.ver == b.ver) { sameMc = true; if (o.loader == b.loader) sameLoader = true; }
    if (b.loader != "Vanilla" && !sameLoader) for (auto& id : LoaderProfiles(b.loader, b.ver)) fs::remove_all(vd / P(id), ec);
    if (!sameMc) fs::remove_all(vd / P(b.ver), ec);
}

// ---------- Настройки конкретного экземпляра (Изменить экземпляр) ----------
struct InstCfg {
    bool memOv = false; int minMB = 512, maxMB = 4096;
    bool javaOv = false, skipCompat = false; std::string javaPath, jvmArgs;
    bool mcOv = false, fullscreen = false; int width = 854, height = 480;
    bool cmdOv = false; std::string wrapper, pre, post;
    bool envOv = false; std::string env; // KEY=VALUE по строкам
};
static fs::path InstCfgPath(const fs::path& d) { return d / "poltergeist_instance.json"; }
static InstCfg LoadInstCfg(const fs::path& d) {
    InstCfg c; json j = Parse(ReadAll(InstCfgPath(d))); if (!j.is_object()) return c;
    try {
        c.memOv = j.value("memOv", false); c.minMB = j.value("minMB", 512); c.maxMB = j.value("maxMB", 4096);
        c.javaOv = j.value("javaOv", false); c.skipCompat = j.value("skipCompat", false); c.javaPath = j.value("javaPath", ""); c.jvmArgs = j.value("jvmArgs", "");
        c.mcOv = j.value("mcOv", false); c.fullscreen = j.value("fullscreen", false); c.width = j.value("width", 854); c.height = j.value("height", 480);
        c.cmdOv = j.value("cmdOv", false); c.wrapper = j.value("wrapper", ""); c.pre = j.value("pre", ""); c.post = j.value("post", "");
        c.envOv = j.value("envOv", false); c.env = j.value("env", "");
    } catch (...) {}
    return c;
}
static void SaveInstCfg(const fs::path& d, const InstCfg& c) {
    json j; j["memOv"] = c.memOv; j["minMB"] = c.minMB; j["maxMB"] = c.maxMB; j["javaOv"] = c.javaOv; j["skipCompat"] = c.skipCompat; j["javaPath"] = c.javaPath; j["jvmArgs"] = c.jvmArgs;
    j["mcOv"] = c.mcOv; j["fullscreen"] = c.fullscreen; j["width"] = c.width; j["height"] = c.height;
    j["cmdOv"] = c.cmdOv; j["wrapper"] = c.wrapper; j["pre"] = c.pre; j["post"] = c.post; j["envOv"] = c.envOv; j["env"] = c.env;
    WriteAll(InstCfgPath(d), j.dump(2));
}
static std::vector<std::pair<std::string, std::string>> ParseEnvText(const std::string& t) {
    std::vector<std::pair<std::string, std::string>> r;
    for (auto line : Split(t, '\n')) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        size_t b = line.find_first_not_of(" \t"); if (b == std::string::npos || line[b] == '#') continue;
        size_t eq = line.find('=', b); if (eq == std::string::npos || eq == b) continue;
        std::string k = line.substr(b, eq - b); while (!k.empty() && k.back() == ' ') k.pop_back();
        r.push_back({k, line.substr(eq + 1)});
    }
    return r;
}
struct EnvGuard { // выставляет переменные окружения на время запуска и возвращает как было
    std::vector<std::pair<std::wstring, std::optional<std::wstring>>> old;
    void Set(const std::string& k, const std::string& v) {
        std::wstring wk = W(k); DWORD n = GetEnvironmentVariableW(wk.c_str(), nullptr, 0);
        if (n > 0) { std::wstring o(n, L'\0'); GetEnvironmentVariableW(wk.c_str(), &o[0], n); o.resize(n - 1); old.push_back({wk, o}); }
        else old.push_back({wk, std::nullopt});
        SetEnvironmentVariableW(wk.c_str(), W(v).c_str());
    }
    ~EnvGuard() { for (size_t i = old.size(); i-- > 0;) SetEnvironmentVariableW(old[i].first.c_str(), old[i].second ? old[i].second->c_str() : nullptr); }
};

static void SetGameDir(const std::string& d) { // пустая строка = стандартная папка
    if (gBusy) return;
    fs::path np = d.empty() ? gCfgDir : P(d);
    std::error_code ec; fs::create_directories(np, ec);
    if (ec || !fs::is_directory(np, ec)) { Status("Не удалось использовать эту папку"); return; }
    gRoot = np; cfg.gameDir = (np == gCfgDir) ? std::string() : d; SaveConfig(); MigrateInstances();
    Status("Папка игры: " + PS(gRoot));
}

// ============================ Запуск игры ============================
static void LaunchThread(Config c, Account acc) {
    gBusy = true; gProg = 0;
    try {
        if (acc.type == 2) { Status("Обновляю сессию Microsoft..."); MsRefresh(acc); { std::lock_guard<std::mutex> l(gMu); gNewAcc = acc; } }
        else if (acc.type == 1) { Status("Проверяю сессию Ely.by..."); ElyRefresh(acc); { std::lock_guard<std::mutex> l(gMu); gNewAcc = acc; } }
        fs::path gameDir = BuildDir(c.buildId); Build bm; if (!LoadBuild(c.buildId, bm)) { bm.id = c.buildId; bm.name = c.buildId; bm.loader = c.loader; bm.ver = c.version; }
        InstCfg ic = LoadInstCfg(gameDir); // свои настройки экземпляра
        if (ic.javaOv) { c.javaPath = ic.javaPath; c.jvmArgs = ic.jvmArgs; }
        int xmx = c.ramMB, xms = std::min(c.ramMB, 512);
        if (ic.memOv) { xmx = std::max(256, ic.maxMB); xms = std::clamp(ic.minMB, 128, xmx); }
        Status("Получаю описание версии " + c.version);
        json v = LoadVersionJson(c.version);
        if (v.is_null()) throw std::runtime_error("Не удалось получить версию " + c.version + " (нет интернета?)");
        int need = v.contains("javaVersion") ? v["javaVersion"].value("majorVersion", 8) : 8;
        std::string javaPick = c.javaPath.empty() ? EnsureJava(need) : JavaExe(c.javaPath);
        if (javaPick.empty()) throw std::runtime_error("Java не найдена и не скачалась");
        std::string launchId = c.version;
        Prep pre; Prepare(v, c.version, gameDir, pre); // сначала ваниль (нужна и Forge-установщику)
        if (c.loader == "Fabric") launchId = InstallFabric(c.version);
        else if (c.loader == "Forge") launchId = InstallForge(c.version, javaPick);
        else if (c.loader == "NeoForge") launchId = InstallNeoForge(c.version, javaPick);
        else if (c.loader == "Quilt") launchId = InstallQuilt(c.version);
        else if (c.loader == "OptiFine") launchId = InstallOptiFine(c.version, javaPick, v, c.optiBuild);
        CheckCancel();
        if (launchId.empty()) throw std::runtime_error("Не удалось установить " + c.loader);
        json lv = launchId == c.version ? v : LoadVersionJson(launchId);
        if (lv.is_null()) throw std::runtime_error("Не нашёл описание профиля " + launchId);
        int need2 = lv.contains("javaVersion") ? lv["javaVersion"].value("majorVersion", 8) : 8;
        std::string java = c.javaPath.empty() ? EnsureJava(need2) : javaPick;
        if (java.empty()) throw std::runtime_error("Java не найдена");
        if (!c.javaPath.empty() && !(ic.javaOv && ic.skipCompat)) { // проверка совместимости своей Java
            int jm = JavaMajorFromHome(P(java).parent_path().parent_path());
            if (jm > 0 && jm < need2) throw std::runtime_error("Выбранная Java " + std::to_string(jm) + " слишком старая: для " + launchId + " нужна Java " + std::to_string(need2) + ". Выбери другую Java или включи «Пропустить проверку совместимости» в настройках экземпляра.");
        }
        Prep pp; Prepare(lv, launchId, gameDir, pp); CheckCancel();
        std::error_code ec; fs::create_directories(gameDir, ec); // экземпляр «установлен» только после успешной подготовки

        std::map<std::string, std::string> vars = {
            {"auth_player_name", acc.name}, {"version_name", launchId}, {"game_directory", PS(gameDir)}, {"assets_root", PS(gRoot / "assets")},
            {"assets_index_name", pp.assetsId}, {"auth_uuid", acc.uuid}, {"auth_access_token", acc.type == 0 ? "0" : acc.token}, {"clientid", ""}, {"auth_xuid", ""},
            {"user_type", acc.type == 2 ? "msa" : (acc.type == 1 ? "mojang" : "legacy")}, {"version_type", lv.value("type", "release")},
            {"natives_directory", PS(pp.natives)}, {"launcher_name", kAppName}, {"launcher_version", kVer}, {"classpath", pp.cp},
            {"classpath_separator", ";"}, {"library_directory", PS(gRoot / "libraries")}, {"game_assets", pp.gameAssets},
            {"auth_session", acc.token}, {"user_properties", "{}"}};
        std::string base = lv.value("_base", c.version);
        auto addArgs = [&](const json& arr, std::vector<std::string>& out) {
            for (auto& a : arr) {
                auto push = [&](std::string s) {
                    if (s.rfind("-DignoreList", 0) == 0) { auto k = s.find("${version_name}"); if (k != std::string::npos) s.replace(k, 15, base); }
                    out.push_back(Sub(s, vars));
                };
                if (a.is_string()) push(a.get<std::string>());
                else if (a.is_object() && RuleOk(a.contains("rules") ? a["rules"] : json())) {
                    if (a["value"].is_string()) push(a["value"].get<std::string>());
                    else for (auto& x : a["value"]) push(x.get<std::string>());
                }
            }
        };
        std::vector<std::string> jvm, game;
        if (lv.contains("arguments")) { if (lv["arguments"].contains("jvm")) addArgs(lv["arguments"]["jvm"], jvm); if (lv["arguments"].contains("game")) addArgs(lv["arguments"]["game"], game); }
        else {
            jvm = {Sub("-Djava.library.path=${natives_directory}", vars), "-cp", pp.cp};
            for (auto& s : Split(lv.value("minecraftArguments", ""), ' ')) if (!s.empty()) game.push_back(Sub(s, vars));
        }
        if (ic.mcOv) { // параметры окна игры
            if (ic.fullscreen) game.push_back("--fullscreen");
            else { game.push_back("--width"); game.push_back(std::to_string(ic.width)); game.push_back("--height"); game.push_back(std::to_string(ic.height)); }
        }
        std::string cmd = Q(JavaExe(java)) + " -Xmx" + std::to_string(xmx) + "M -Xms" + std::to_string(xms) + "M -XX:+UseG1GC -XX:MaxGCPauseMillis=50";
        if (acc.type == 1) {
            fs::path ai = gRoot / "cache" / "authlib-injector.jar";
            if (!Download(kAuthlibUrl, ai)) throw std::runtime_error("Не скачался authlib-injector (нужен для Ely.by)");
            cmd += " " + Q("-javaagent:" + PS(ai) + "=https://authserver.ely.by/api/authlib-injector");
        }
        if (!c.jvmArgs.empty()) cmd += " " + c.jvmArgs;
        ApplyGpuPref(JavaExe(java), c.gpu);
        for (auto& s : jvm) cmd += " " + Q(s);
        cmd += " " + Q(lv.value("mainClass", "net.minecraft.client.main.Main"));
        for (auto& s : game) cmd += " " + Q(s);

        // пользовательские команды и переменные окружения экземпляра
        std::map<std::string, std::string> iv = {{"INST_NAME", bm.name}, {"INST_ID", launchId}, {"INST_DIR", PS(gameDir)}, {"INST_MC_DIR", PS(gameDir)},
                                                 {"INST_JAVA", JavaExe(java)}, {"INST_MC_VER", c.version}, {"INST_LOADER", c.loader}};
        auto sd = [&](std::string t) { for (auto& [k, val] : iv) { std::string key = "$" + k; size_t p = 0; while ((p = t.find(key, p)) != std::string::npos) { t.replace(p, key.size(), val); p += val.size(); } } return t; };
        EnvGuard eg; for (auto& [k, val] : iv) eg.Set(k, val);
        if (ic.envOv) for (auto& kv : ParseEnvText(ic.env)) eg.Set(kv.first, sd(kv.second));
        if (ic.cmdOv && !ic.wrapper.empty()) cmd = sd(ic.wrapper) + " " + cmd;
        if (ic.cmdOv && !ic.pre.empty()) {
            Status("Выполняю команду перед запуском...");
            std::string pc; int pcode = RunProcess("cmd.exe /S /C \"" + sd(ic.pre) + "\"", gameDir, pc);
            if (pcode != 0) throw std::runtime_error("Команда перед запуском завершилась с кодом " + std::to_string(pcode) + ", игра не запущена.");
        }

        CheckCancel();
        gProg = 1.0f; Status("Игра запущена (" + launchId + ")");
        gGameRunning = true; int act = c.onLaunch;
        if (act == 1 || act == 2) std::thread([act] { // свернуть/закрыть лаунчер, если игра успешно стартовала и не упала
            for (int i = 0; i < (act == 1 ? 8 : 40) && gGameRunning; i++) Sleep(500);
            if (!gGameRunning) return;
            if (act == 1) PostMessageW(gHwnd, WM_APP_HIDE, 0, 0); else PostMessageW(gHwnd, WM_CLOSE, 0, 0);
        }).detach();
        Log("Запуск: " + bm.name + " (" + c.loader + " " + c.version + "), профиль " + launchId + ", Java " + std::to_string(need2) + ", память " + std::to_string(xmx) + " МБ");
        auto t0 = std::chrono::steady_clock::now(); std::string cap; int code = RunProcess(cmd, gameDir, cap);
        long long secs = (long long)std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - t0).count();
        Log("Игра завершилась с кодом " + std::to_string(code) + " (" + std::to_string(secs) + " с)");
        if (secs >= 3) { bm.playSec += secs; bm.launches++; bm.lastPlayed = (long long)time(nullptr); SaveBuild(bm); gStatSec += secs; gStatLaunch += 1; }
        gGameRunning = false;
        if (ic.cmdOv && !ic.post.empty()) { std::string pc2; RunProcess("cmd.exe /S /C \"" + sd(ic.post) + "\"", gameDir, pc2); }
        if (act == 1) PostMessageW(gHwnd, WM_APP_SHOW, 0, 0); // игра закрылась - вернуть лаунчер из фона
        if (gCancel) Status("Игра закрыта"); // закрыта кнопкой «Закрыть» — это не крэш
        else if (code != 0) { auto [t, b] = Analyze(code, cap); Status("Игра завершилась с ошибкой: " + t); ShowError(t, b); }
        else Status("Игра закрыта");
    } catch (std::exception& e) {
        if (gCancel) { gProg = 0; Status("Остановлено"); }
        else { Status(std::string("Ошибка: ") + e.what()); ShowError("Не удалось запустить", e.what()); }
    }
    gGameRunning = false; gCancel = false; gBusy = false;
}
// Кнопка «Остановить» (идёт загрузка/установка) и «Закрыть» (игра уже запущена)
static void StopLaunch() {
    if (!gBusy || gCancel) return;
    bool game = gGameRunning;
    gCancel = true;
    Status(game ? "Закрываю игру..." : "Останавливаю...");
    AbortHttp(); // прерываем текущие загрузки
    DWORD pid; { std::lock_guard<std::mutex> l(gProcMu); pid = gChildPid; }
    if (pid) { if (game) ProcSoftClose(pid); else ProcHardKill(pid); } // игре сначала даём закрыться как обычно (сохранить мир)
    std::thread([pid] { // если процесс не завершился сам за ~8 секунд — добиваем
        for (int i = 0; i < 80 && ChildIs(pid); i++) Sleep(100);
        if (ChildIs(pid)) ProcHardKill(pid);
    }).detach();
}
static bool AddOffline(const std::string& name) {
    if (!ValidNick(name)) { AuthMsg(kNickHint); return false; }
    for (int i = 0; i < (int)cfg.accs.size(); i++) if (cfg.accs[i].type == 0 && cfg.accs[i].name == name) { cfg.sel = i; SaveConfig(); return true; }
    Account a; a.name = name; a.uuid = OfflineUuid(name); cfg.accs.push_back(a); cfg.sel = (int)cfg.accs.size() - 1; SaveConfig();
    return true;
}
static void StartLaunch() {
    if (gBusy || gDelBusy || cfg.buildId.empty()) return;
    if (cfg.accs.empty() && !AddOffline(bNick)) return; // нет аккаунтов - играем офлайн с ником из поля ввода
    if (cfg.sel < 0 || cfg.sel >= (int)cfg.accs.size()) cfg.sel = 0;
    cfg.lastNick = cfg.accs[cfg.sel].name; // для приветствия при следующем входе
    SaveConfig();
    gCancel = false;
    std::thread(LaunchThread, cfg, cfg.accs[cfg.sel]).detach();
}

// ============================ Фон: скриншоты ============================
static ID3D11Device* gDev; static ID3D11DeviceContext* gCtx; static IDXGISwapChain* gSwap; static ID3D11RenderTargetView* gRTV;
struct Tex { ID3D11ShaderResourceView* v = nullptr; int w = 0, h = 0; };
static Tex gBg[2]; static float gFade = 1, gSlideT = 0, gTime = 0;
static Tex gBlur[2]; static std::vector<unsigned char> gBlurData; static int gBlurW = 0, gBlurH = 0; // размытая копия фона для «жидкого стекла»
static ImVec2 gBgUV0(0, 0), gBgUV1(1, 1), gBgSz(1, 1); static bool gBgOK = false;               // как фон натянут на экран (для панелей-стекла)
static std::string gBgNow; static std::vector<std::string> gShots;
static std::mutex gImgMu; static std::vector<unsigned char> gImgData; static int gImgW, gImgH; static bool gImgReady = false;
static std::atomic<bool> gImgLoading{false};

static void LoadBgAsync(std::string path) {
    if (gImgLoading) return;
    gImgLoading = true; gBgNow = path;
    std::thread([path] {
        std::string bytes = ReadAll(P(path)); int w = 0, h = 0, ch = 0;
        unsigned char* d = bytes.empty() ? nullptr : stbi_load_from_memory((const unsigned char*)bytes.data(), (int)bytes.size(), &w, &h, &ch, 4);
        if (d) {
            int f = std::max(1, w / 1920); // быстрый box-downscale для 4K-скриншотов
            int nw = w / f, nh = h / f; std::vector<unsigned char> out((size_t)nw * nh * 4);
            for (int y = 0; y < nh; y++) for (int x = 0; x < nw; x++) for (int c = 0; c < 4; c++) {
                int s = 0; for (int dy = 0; dy < f; dy++) for (int dx = 0; dx < f; dx++) s += d[(((size_t)y * f + dy) * w + (x * f + dx)) * 4 + c];
                out[((size_t)y * nw + x) * 4 + c] = (unsigned char)(s / (f * f));
            }
            stbi_image_free(d);
            // маленькая сильно размытая копия: при растяжении билинейным фильтром получается мягкое «матовое стекло»
            int bw = 160, bh = std::max(1, (int)((long long)nh * bw / nw)); std::vector<unsigned char> sm((size_t)bw * bh * 4);
            for (int y = 0; y < bh; y++) for (int x = 0; x < bw; x++) {
                int x0 = x * nw / bw, x1 = std::max(x0 + 1, (x + 1) * nw / bw), y0 = y * nh / bh, y1 = std::max(y0 + 1, (y + 1) * nh / bh);
                int sx = std::max(1, (x1 - x0) / 6), sy2 = std::max(1, (y1 - y0) / 6);
                for (int c = 0; c < 4; c++) { int sum = 0, n = 0; for (int yy = y0; yy < y1; yy += sy2) for (int xx = x0; xx < x1; xx += sx) { sum += out[((size_t)yy * nw + xx) * 4 + c]; n++; } sm[((size_t)y * bw + x) * 4 + c] = (unsigned char)(sum / std::max(1, n)); }
            }
            for (int pass = 0; pass < 3; pass++) for (int dir = 0; dir < 2; dir++) {
                std::vector<unsigned char> t2 = sm;
                for (int y = 0; y < bh; y++) for (int x = 0; x < bw; x++) for (int c = 0; c < 4; c++) {
                    int sum = 0, n = 0; for (int k = -3; k <= 3; k++) { int xx = dir == 0 ? std::clamp(x + k, 0, bw - 1) : x, yy = dir == 1 ? std::clamp(y + k, 0, bh - 1) : y; sum += t2[((size_t)yy * bw + xx) * 4 + c]; n++; }
                    sm[((size_t)y * bw + x) * 4 + c] = (unsigned char)(sum / n);
                }
            }
            std::lock_guard<std::mutex> l(gImgMu); gImgData = std::move(out); gImgW = nw; gImgH = nh; gBlurData = std::move(sm); gBlurW = bw; gBlurH = bh; gImgReady = true;
        }
        gImgLoading = false;
    }).detach();
}
static ID3D11ShaderResourceView* MakeTex(const unsigned char* px, int w, int h) {
    D3D11_TEXTURE2D_DESC d{}; d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_IMMUTABLE; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd{px, (UINT)w * 4, 0}; ID3D11Texture2D* t = nullptr;
    if (FAILED(gDev->CreateTexture2D(&d, &sd, &t))) return nullptr;
    ID3D11ShaderResourceView* v = nullptr; gDev->CreateShaderResourceView(t, nullptr, &v); t->Release(); return v;
}
static void ScanShots() {
    gShots.clear(); std::error_code ec; std::vector<fs::path> dirs;
    if (!cfg.shotsDir.empty()) dirs.push_back(P(cfg.shotsDir));
    else dirs.push_back(fs::path(Env(L"APPDATA")) / ".minecraft" / "screenshots");
    for (auto& e : fs::directory_iterator(gRoot / "instances", ec)) dirs.push_back(e.path() / "screenshots");
    for (auto& d : dirs) for (auto& e : fs::directory_iterator(d, ec)) {
        std::wstring x = e.path().extension().wstring(); for (auto& ch : x) ch = (wchar_t)towlower(ch);
        if (x == L".png" || x == L".jpg" || x == L".jpeg" || x == L".bmp") gShots.push_back(PS(e.path()));
    }
}
static void NextShot() {
    if (gShots.empty()) return; static std::mt19937 rng{std::random_device{}()};
    for (int i = 0; i < 5; i++) { const std::string& s = gShots[rng() % gShots.size()]; if (s != gBgNow || gShots.size() == 1) { LoadBgAsync(s); return; } }
}
static void UpdateBackground(float dt) {
    {
        std::lock_guard<std::mutex> l(gImgMu);
        if (gImgReady) {
            if (gBg[1].v) gBg[1].v->Release();
            gBg[1] = gBg[0]; gBg[0] = {MakeTex(gImgData.data(), gImgW, gImgH), gImgW, gImgH};
            if (gBlur[1].v) gBlur[1].v->Release();
            gBlur[1] = gBlur[0]; gBlur[0] = {MakeTex(gBlurData.data(), gBlurW, gBlurH), gBlurW, gBlurH}; gBlurData.clear();
            gImgData.clear(); gImgData.shrink_to_fit(); gImgReady = false; gFade = 0;
        }
    }
    gFade = std::min(1.f, gFade + dt * 1.2f);
    if (cfg.slideshow && gShots.size() > 1) { gSlideT += dt; if (gSlideT > 18.f) { gSlideT = 0; NextShot(); } }
}
static std::string PickFolder(HWND hw) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    wchar_t disp[MAX_PATH] = {0};
    BROWSEINFOW bi{}; bi.hwndOwner = hw; bi.pszDisplayName = disp; bi.lpszTitle = L"Выбери папку для игры"; bi.ulFlags = BIF_NEWDIALOGSTYLE | BIF_RETURNONLYFSDIRS;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    std::string r;
    if (pidl) { wchar_t path[MAX_PATH]; if (SHGetPathFromIDListW(pidl, path)) r = U8(path); CoTaskMemFree(pidl); }
    return r;
}
static std::string PickImage(HWND hw) {
    wchar_t f[MAX_PATH] = {0}; OPENFILENAMEW o{}; o.lStructSize = sizeof(o); o.hwndOwner = hw;
    o.lpstrFilter = L"Изображения\0*.png;*.jpg;*.jpeg;*.bmp;*.tga\0Все файлы\0*.*\0"; o.lpstrFile = f; o.nMaxFile = MAX_PATH;
    o.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetOpenFileNameW(&o) ? U8(f) : "";
}

// ============================ UI ============================
enum { S_MAIN, S_ACC, S_BUILDS, S_CREATE, S_SET, S_LOG, S_SKINS, S_STATS, S_NEWS, S_MODS };
static int gScr = S_MAIN, gSetTab = 0; static float gScrFade = 1; static bool bInit = false;
static void Go(int s) { if (gScr != s) { gScr = s; gScrFade = 0; if (s == S_ACC) { std::lock_guard<std::mutex> l(gMu); gAuthMsg.clear(); } } if (s == S_CREATE) bInit = false; }
static float gS = 1.f; static int gThemeSig = -1;
static float gCapH = 0.f, gBtnX0 = 1e9f; // зона перетаскивания окна (свой заголовок) для WM_NCHITTEST
static ImFont *gFTitle = nullptr, *gFSub = nullptr; // крупные шрифты: вместо масштабирования, чтобы текст был чётким
static ImVec4 gAccent(0.87f, 0.42f, 0.53f, 1.f), gSoft(1.f, 1.f, 1.f, 0.10f);
static const char* T(const char* ru, const char* en) { return cfg.lang == 1 ? en : ru; }
static float R(float v) { return cfg.rounded ? v : 0.f; }
static ImU32 TextCol(float a = 1.f) { ImVec4 c = ImGui::GetStyleColorVec4(ImGuiCol_Text); c.w *= a; return ImGui::GetColorU32(c); }
static ImU32 OnAccent(float a = 1.f) {
    float l = .299f * gAccent.x + .587f * gAccent.y + .114f * gAccent.z;
    return ImGui::GetColorU32(l > 0.64f ? ImVec4(.08f, .09f, .12f, a) : ImVec4(1.f, 1.f, 1.f, a));
}
static void TD(const char* s) { ImGui::TextDisabled("%s", s); }

// ---------- Темы оформления ----------
struct Theme {
    const char *ru, *en, *dru, *den; bool light, glass; // glass: полупрозрачные панели поверх фона-скриншота
    ImVec4 accent, bg1, bg2, panel, border, pvBg, pvBar, pvBtn, pvSide; // pv* - цвета миниатюры в настройках
};
#define C8(r, g, b, a) ImVec4((r) / 255.f, (g) / 255.f, (b) / 255.f, (a) / 255.f)
static const Theme kThemes[5] = {
    {"Eternal", "Eternal", "Как раньше", "Like before", false, true, C8(222, 106, 134, 255), C8(64, 22, 36, 255), C8(24, 9, 15, 255), C8(30, 12, 18, 175), C8(255, 190, 205, 45), C8(30, 12, 18, 255), C8(125, 60, 82, 255), C8(66, 26, 38, 255), C8(66, 26, 38, 255)},
    {"Минимализм", "Minimal", "Плоский, без стекла", "Flat, no glass", false, false, C8(176, 178, 196, 255), C8(20, 20, 23, 255), C8(20, 20, 23, 255), C8(28, 28, 32, 255), C8(255, 255, 255, 24), C8(20, 20, 23, 255), C8(66, 66, 72, 255), C8(38, 38, 43, 255), C8(30, 30, 34, 255)},
    {"Светлая", "Light", "Светлый интерфейс", "Light interface", true, true, C8(196, 72, 108, 255), C8(244, 232, 238, 255), C8(230, 212, 222, 255), C8(252, 246, 249, 235), C8(0, 0, 0, 30), C8(252, 246, 249, 255), C8(226, 206, 216, 255), C8(255, 255, 255, 255), C8(255, 255, 255, 255)},
    {"AMOLED", "AMOLED", "Чисто чёрный фон", "Pure black background", false, false, C8(214, 214, 224, 255), C8(0, 0, 0, 255), C8(0, 0, 0, 255), C8(5, 5, 5, 255), C8(255, 255, 255, 26), C8(0, 0, 0, 255), C8(52, 52, 58, 255), C8(28, 28, 30, 255), C8(22, 22, 24, 255)},
    {"Тёмно-синяя", "Dark blue", "Ночная синева", "Night blue", false, true, C8(76, 132, 245, 255), C8(10, 22, 52, 255), C8(3, 8, 22, 255), C8(8, 17, 40, 200), C8(110, 150, 255, 45), C8(5, 12, 30, 255), C8(70, 120, 235, 255), C8(16, 34, 74, 255), C8(16, 34, 74, 255)},
};
static const Theme& Th() { return kThemes[std::clamp(cfg.theme, 0, 4)]; }

static void ApplyTheme() {
    const Theme& th = Th(); bool lt = th.light;
    gAccent = th.accent; gSoft = lt ? ImVec4(0.f, 0.f, 0.f, .07f) : ImVec4(1.f, 1.f, 1.f, th.glass ? .10f : .07f);
    ImGuiStyle& st = ImGui::GetStyle(); st = ImGuiStyle();
    if (lt) ImGui::StyleColorsLight(&st); else ImGui::StyleColorsDark(&st);
    float r = cfg.rounded ? 1.f : 0.f;
    st.FrameRounding = 10 * r; st.PopupRounding = 14 * r; st.GrabRounding = 10 * r; st.ScrollbarRounding = 10 * r; st.WindowRounding = 16 * r; st.ChildRounding = 12 * r;
    st.ScrollbarSize = 10; st.PopupBorderSize = 1; st.FrameBorderSize = 0; st.ItemSpacing = ImVec2(10, 11);
    st.FramePadding = ImVec2(10, 7); st.ItemSpacing = ImVec2(10, 10); st.ScaleAllSizes(gS);
    ImVec4* c = st.Colors; ImVec4 pop(th.pvBg.x, th.pvBg.y, th.pvBg.z, .98f);
    if (lt) {
        c[ImGuiCol_Text] = ImVec4(.09f, .10f, .14f, 1.f); c[ImGuiCol_TextDisabled] = ImVec4(.42f, .38f, .44f, 1.f);
        c[ImGuiCol_FrameBg] = ImVec4(0, 0, 0, .07f); c[ImGuiCol_FrameBgHovered] = ImVec4(0, 0, 0, .12f); c[ImGuiCol_FrameBgActive] = ImVec4(0, 0, 0, .16f);
        c[ImGuiCol_Button] = ImVec4(0, 0, 0, .10f);
    } else {
        c[ImGuiCol_FrameBg] = ImVec4(1, 1, 1, .08f); c[ImGuiCol_FrameBgHovered] = ImVec4(1, 1, 1, .14f); c[ImGuiCol_FrameBgActive] = ImVec4(1, 1, 1, .18f);
        c[ImGuiCol_Button] = ImVec4(1, 1, 1, .12f);
    }
    c[ImGuiCol_PopupBg] = pop; c[ImGuiCol_WindowBg] = pop; c[ImGuiCol_Border] = th.border;
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0); c[ImGuiCol_ScrollbarGrab] = lt ? ImVec4(0, 0, 0, .20f) : ImVec4(1, 1, 1, .18f);
    c[ImGuiCol_ScrollbarGrabHovered] = lt ? ImVec4(0, 0, 0, .32f) : ImVec4(1, 1, 1, .30f); c[ImGuiCol_ScrollbarGrabActive] = lt ? ImVec4(0, 0, 0, .42f) : ImVec4(1, 1, 1, .40f);
    c[ImGuiCol_CheckMark] = gAccent; c[ImGuiCol_SliderGrab] = gAccent; c[ImGuiCol_SliderGrabActive] = gAccent;
    c[ImGuiCol_Header] = ImVec4(gAccent.x, gAccent.y, gAccent.z, .35f); c[ImGuiCol_HeaderHovered] = ImVec4(gAccent.x, gAccent.y, gAccent.z, .5f);
    c[ImGuiCol_ButtonHovered] = ImVec4(gAccent.x, gAccent.y, gAccent.z, .45f); c[ImGuiCol_ButtonActive] = ImVec4(gAccent.x, gAccent.y, gAccent.z, .6f);
}
static float Anim(ImGuiID id, float target, float speed, float def = 0.f) {
    float& v = *ImGui::GetStateStorage()->GetFloatRef(id, def);
    v += (target - v) * std::min(1.f, ImGui::GetIO().DeltaTime * speed); return v;
}
static float AnimK(const char* k, float target, float speed, float def = 0.f) { return Anim(ImGui::GetID(k), target, speed, def); }

// ---------- Дизайн-система: два стиля (0 Классический, 1 Жидкое стекло) ----------
static bool GlassOn() { return cfg.style == 1; }
static ImU32 U32(const ImVec4& c) { return ImGui::ColorConvertFloat4ToU32(c); }
static ImU32 WA(float a) { return IM_COL32(255, 255, 255, (int)(std::clamp(a, 0.f, 1.f) * 255)); }
static ImU32 BA(float a) { return IM_COL32(0, 0, 0, (int)(std::clamp(a, 0.f, 1.f) * 255)); }
static ImU32 AccA(float a) { return U32(ImVec4(gAccent.x, gAccent.y, gAccent.z, std::clamp(a, 0.f, 1.f))); }
static ImVec4 Mix(const ImVec4& a, const ImVec4& b, float t) { return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t); }
static ImFont* FSub() { return gFSub ? gFSub : ImGui::GetFont(); }

// Универсальная поверхность. lvl: 0 - кнопка/чип, 1 - карточка, 2 - большая панель (с тенью). hot 0..1 - наведение/выбор. tint - своя подложка (для панелей).
static void Surface(ImDrawList* dl, ImVec2 a, ImVec2 b, float rr, int lvl = 1, float hot = 0.f, const ImVec4* tint = nullptr) {
    bool lt = Th().light, gl = GlassOn(); float h = b.y - a.y, w = b.x - a.x;
    if (lvl >= 2) for (int k = 4; k >= 1; k--) dl->AddRectFilled(ImVec2(a.x - k * 2.f, a.y - k + 5 * gS), ImVec2(b.x + k * 2.f, b.y + k * 3.f + 5 * gS), BA(lt ? .025f : .05f), rr + k * 3.f);
    if (gl && lvl >= 2 && gBgOK && gBlur[0].v) { // настоящее «матовое стекло»: размытая копия фона ровно под панелью
        auto uv = [&](ImVec2 p) { return ImVec2(gBgUV0.x + p.x / gBgSz.x * (gBgUV1.x - gBgUV0.x), gBgUV0.y + p.y / gBgSz.y * (gBgUV1.y - gBgUV0.y)); };
        ImVec2 u0 = uv(a), u1 = uv(b);
        if (gBlur[1].v && gFade < 1.f) dl->AddImageRounded((ImTextureID)(intptr_t)gBlur[1].v, a, b, u0, u1, IM_COL32_WHITE, rr);
        dl->AddImageRounded((ImTextureID)(intptr_t)gBlur[0].v, a, b, u0, u1, IM_COL32(255, 255, 255, (int)(255 * gFade)), rr);
        dl->AddRectFilled(a, b, U32(ImVec4(Th().bg2.x, Th().bg2.y, Th().bg2.z, lt ? .16f : .30f)), rr); // оттенок темы, как у фона
    }
    if (tint) dl->AddRectFilled(a, b, U32(*tint), rr);
    if (gl) {
        dl->AddRectFilled(a, b, lt ? WA(.34f + .14f * hot) : WA(.055f + .055f * hot), rr);                                                       // стеклянная заливка
        dl->AddRectFilled(ImVec2(a.x + 1, a.y + 1), ImVec2(b.x - 1, a.y + h * .5f), lt ? WA(.26f) : WA(.075f), rr, ImDrawFlags_RoundCornersTop);   // верхний блик
        dl->AddRectFilled(ImVec2(a.x + 1, b.y - h * .22f), ImVec2(b.x - 1, b.y - 1), lt ? WA(.14f) : WA(.035f), rr, ImDrawFlags_RoundCornersBottom); // свет снизу (преломление)
        dl->AddRect(ImVec2(a.x + 2, a.y + 2), ImVec2(b.x - 2, b.y - 2), lt ? WA(.35f) : WA(.05f), std::max(0.f, rr - 2.f), 0, 1.f);               // внутренняя кромка
        dl->AddRect(a, b, lt ? BA(.08f) : WA(.10f), rr, 0, 1.2f);                                                                                // общая кромка
        dl->PushClipRect(a, ImVec2(b.x, a.y + h * .45f), true); dl->AddRect(a, b, lt ? WA(.95f) : WA(.46f), rr, 0, 1.5f); dl->PopClipRect();      // яркая верхняя кромка
        dl->PushClipRect(ImVec2(a.x, b.y - h * .30f), b, true); dl->AddRect(a, b, lt ? BA(.12f) : WA(.17f), rr, 0, 1.2f); dl->PopClipRect();      // мягкая нижняя
        if (rr > 3.f && w > rr * 3.f) dl->AddLine(ImVec2(a.x + rr * .9f, a.y + 1.5f), ImVec2(b.x - rr * .9f, a.y + 1.5f), lt ? WA(.95f) : WA(.45f), 1.4f);
    } else {
        dl->AddRectFilled(a, b, lt ? BA(.06f + .04f * hot) : WA(.070f + .050f * hot), rr);
        dl->AddRectFilled(ImVec2(a.x + 1, a.y + 1), ImVec2(b.x - 1, a.y + h * .45f), lt ? WA(.20f) : WA(.030f), rr, ImDrawFlags_RoundCornersTop);
        dl->AddRect(a, b, U32(Th().border), rr, 0, 1.f);
    }
    if (hot > 0.01f) dl->AddRect(a, b, AccA(.60f * hot), rr, 0, 1.5f * gS);
}
static ImVec4 PanelTint(float a) { ImVec4 c = Th().panel; c.w *= a * (GlassOn() ? (gBgOK ? .30f : .55f) : 1.f); return c; }

static void DrawLogo(ImDrawList* dl, ImVec2 c, float s) { // логотип: призрак на акцентной плитке
    ImVec2 a(c.x - s / 2, c.y - s / 2), b(c.x + s / 2, c.y + s / 2); float rr = R(s * .3f), k = s * .5f; ImU32 w = WA(.96f);
    dl->AddRectFilled(a, b, U32(gAccent), rr);
    dl->AddRectFilled(ImVec2(a.x + 1, a.y + 1), ImVec2(b.x - 1, c.y), WA(.22f), rr, ImDrawFlags_RoundCornersTop);
    dl->AddCircleFilled(ImVec2(c.x, c.y - k * .12f), k * .42f, w);
    dl->AddRectFilled(ImVec2(c.x - k * .42f, c.y - k * .12f), ImVec2(c.x + k * .42f, c.y + k * .36f), w);
    for (int i = 0; i < 3; i++) dl->AddCircleFilled(ImVec2(c.x + (i - 1) * k * .28f, c.y + k * .36f), k * .14f, w);
    dl->AddCircleFilled(ImVec2(c.x - k * .16f, c.y - k * .1f), k * .07f, IM_COL32(60, 20, 40, 255));
    dl->AddCircleFilled(ImVec2(c.x + k * .16f, c.y - k * .1f), k * .07f, IM_COL32(60, 20, 40, 255));
}

static bool FancyBtn(const char* label, ImVec2 sz, ImVec4 col, bool enabled = true) {
    ImGui::PushID(label);
    const char* hid = strstr(label, "##"); // видимая часть — до "##"
    if (sz.x <= 0) sz.x = ImGui::GetContentRegionAvail().x;
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool clk = ImGui::InvisibleButton("##b", sz);
    bool hov = ImGui::IsItemHovered() && enabled, act = ImGui::IsItemActive() && enabled;
    float t = AnimK("h", hov ? 1.f : 0.f, 14.f);
    bool strong = col.w >= 0.85f; float rr = R(sz.y * .38f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (act) p.y += 1.f;
    ImVec2 q(p.x + sz.x, p.y + sz.y);
    if (strong) {
        ImVec4 c = col; float k = enabled ? 1.f : .45f; c.x *= k; c.y *= k; c.z *= k;
        c.x = std::min(1.f, c.x + .10f * t); c.y = std::min(1.f, c.y + .10f * t); c.z = std::min(1.f, c.z + .10f * t);
        if (enabled) for (int i = 3; i >= 1; i--) dl->AddRectFilled(ImVec2(p.x - i * 2.5f, p.y - i * 1.2f + 3 * gS), ImVec2(q.x + i * 2.5f, q.y + i * 2.5f + 3 * gS), U32(ImVec4(c.x, c.y, c.z, (.055f + .05f * t))), rr + i * 2.5f);
        dl->AddRectFilled(p, q, U32(c), rr);
        dl->AddRectFilled(ImVec2(p.x + 1, p.y + 1), ImVec2(q.x - 1, p.y + sz.y * .5f), WA(GlassOn() ? .26f : .10f), rr, ImDrawFlags_RoundCornersTop);
        if (GlassOn()) dl->AddRectFilled(ImVec2(p.x + 1, q.y - sz.y * .25f), ImVec2(q.x - 1, q.y - 1), WA(.07f), rr, ImDrawFlags_RoundCornersBottom);
        dl->AddRect(p, q, WA(GlassOn() ? .38f : .14f), rr, 0, 1.f);
    } else Surface(dl, p, q, rr, 0, enabled ? t : 0.f);
    ImVec2 ts = ImGui::CalcTextSize(label, hid);
    ImU32 tc = strong ? OnAccent(enabled ? 1.f : .55f) : TextCol(enabled ? 1.f : .5f);
    dl->AddText(ImVec2(p.x + (sz.x - ts.x) / 2, p.y + (sz.y - ts.y) / 2), tc, label, hid);
    ImGui::PopID();
    return clk && enabled;
}
static bool BackBtn(ImVec2 sz) { // кнопка «назад» со стрелкой
    bool clk = FancyBtn("##back", sz, gSoft);
    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax(), m((a.x + b.x) / 2, (a.y + b.y) / 2);
    float s = (b.y - a.y) * .22f, th = 2.f * gS; ImU32 col = TextCol(); ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddLine(ImVec2(m.x + s * 1.5f, m.y), ImVec2(m.x - s * 1.5f, m.y), col, th);
    dl->AddLine(ImVec2(m.x - s * 1.5f, m.y), ImVec2(m.x - s * .4f, m.y - s * 1.1f), col, th);
    dl->AddLine(ImVec2(m.x - s * 1.5f, m.y), ImVec2(m.x - s * .4f, m.y + s * 1.1f), col, th);
    return clk;
}
static void IconCheck(ImDrawList* dl, ImVec2 m, float s, ImU32 col) {
    float th = 2.5f * gS;
    dl->AddLine(ImVec2(m.x - s, m.y), ImVec2(m.x - s * .3f, m.y + s * .7f), col, th);
    dl->AddLine(ImVec2(m.x - s * .3f, m.y + s * .7f), ImVec2(m.x + s, m.y - s * .7f), col, th);
}
static void IconCross(ImDrawList* dl, ImVec2 m, float s, ImU32 col) {
    float th = 2.5f * gS;
    dl->AddLine(ImVec2(m.x - s * .8f, m.y - s * .8f), ImVec2(m.x + s * .8f, m.y + s * .8f), col, th);
    dl->AddLine(ImVec2(m.x - s * .8f, m.y + s * .8f), ImVec2(m.x + s * .8f, m.y - s * .8f), col, th);
}
static void IconTrash(ImDrawList* dl, ImVec2 m, float s, ImU32 col) { // корзина для мусора
    float th = std::max(1.5f, 1.6f * gS);
    ImVec2 body[4] = {ImVec2(m.x - s * .62f, m.y - s * .28f), ImVec2(m.x + s * .62f, m.y - s * .28f), ImVec2(m.x + s * .48f, m.y + s * .95f), ImVec2(m.x - s * .48f, m.y + s * .95f)};
    dl->AddPolyline(body, 4, col, ImDrawFlags_Closed, th);
    dl->AddLine(ImVec2(m.x - s * .88f, m.y - s * .5f), ImVec2(m.x + s * .88f, m.y - s * .5f), col, th);
    dl->AddLine(ImVec2(m.x - s * .28f, m.y - s * .5f), ImVec2(m.x - s * .28f, m.y - s * .86f), col, th);
    dl->AddLine(ImVec2(m.x + s * .28f, m.y - s * .5f), ImVec2(m.x + s * .28f, m.y - s * .86f), col, th);
    dl->AddLine(ImVec2(m.x - s * .28f, m.y - s * .86f), ImVec2(m.x + s * .28f, m.y - s * .86f), col, th);
    dl->AddLine(ImVec2(m.x - s * .2f, m.y + s * .05f), ImVec2(m.x - s * .16f, m.y + s * .68f), col, th * .8f);
    dl->AddLine(ImVec2(m.x + s * .2f, m.y + s * .05f), ImVec2(m.x + s * .16f, m.y + s * .68f), col, th * .8f);
}
static void DrawTrash(ImDrawList* dl, ImVec2 a, ImVec2 b, bool hov, bool enabled) {
    ImVec2 m((a.x + b.x) / 2, (a.y + b.y) / 2); float s = (b.y - a.y) * .30f;
    if (hov && enabled) dl->AddRectFilled(a, b, IM_COL32(225, 70, 70, 70), R((b.y - a.y) * .3f));
    IconTrash(dl, m, s, (hov && enabled) ? IM_COL32(255, 100, 100, 255) : IM_COL32(235, 85, 85, enabled ? 205 : 90));
}
static bool TrashBtn(const char* id, float sz, bool enabled = true) { // квадратная кнопка-корзина
    ImGui::PushID(id); ImVec2 p = ImGui::GetCursorScreenPos();
    bool clk = ImGui::InvisibleButton("##tr", ImVec2(sz, sz)), hov = ImGui::IsItemHovered();
    DrawTrash(ImGui::GetWindowDrawList(), p, ImVec2(p.x + sz, p.y + sz), hov, enabled);
    ImGui::PopID(); return clk && enabled;
}
static void Open(const fs::path& p) { std::error_code ec; fs::create_directories(p, ec); ShellExecuteW(0, L"open", p.wstring().c_str(), 0, 0, SW_SHOWNORMAL); }
static std::string AccLabel(const Account& a) { return std::string(a.type == 0 ? T("[офлайн] ", "[offline] ") : a.type == 1 ? "[Ely.by] " : "[Microsoft] ") + a.name; }

static void TabBar(const char* const* labels, int n, int& cur, bool fade = true) { // сегмент-переключатель: «таблетка» с плавающим акцентом
    float em = ImGui::GetFontSize(), h = em * 2.2f; ImVec2 s = ImGui::GetCursorScreenPos(); float pad = em * .25f, x = s.x + pad, tx = x, tw = 0, total = pad * 2;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    std::vector<float> ws(n); for (int i = 0; i < n; i++) { ws[i] = ImGui::CalcTextSize(labels[i]).x + em * 1.6f; total += ws[i]; }
    Surface(dl, s, ImVec2(s.x + total, s.y + h), R(h * .5f), 0);
    float ih = h - pad * 2;
    x = s.x + pad; for (int i = 0; i < n; i++) if (cur == i) { tx = x; tw = ws[i]; break; } else x += ws[i];
    float ux = AnimK("ux", tx, 14.f, tx), uw = AnimK("uw", tw, 14.f, tw);
    dl->AddRectFilled(ImVec2(ux, s.y + pad), ImVec2(ux + uw, s.y + pad + ih), AccA(.30f), R(ih * .5f));
    dl->AddRect(ImVec2(ux, s.y + pad), ImVec2(ux + uw, s.y + pad + ih), AccA(.75f), R(ih * .5f), 0, 1.3f);
    x = s.x + pad;
    for (int i = 0; i < n; i++) {
        ImVec2 ts = ImGui::CalcTextSize(labels[i]);
        ImGui::SetCursorScreenPos(ImVec2(x, s.y + pad)); ImGui::PushID(i);
        if (ImGui::InvisibleButton("##t", ImVec2(ws[i], ih)) && cur != i) { cur = i; if (fade) gScrFade = 0; }
        float t = AnimK("h", (ImGui::IsItemHovered() || cur == i) ? 1.f : 0.f, 12.f);
        dl->AddText(ImVec2(x + (ws[i] - ts.x) / 2, s.y + (h - ts.y) / 2), TextCol(.55f + .45f * t), labels[i]);
        x += ws[i]; ImGui::PopID();
    }
    ImGui::SetCursorScreenPos(ImVec2(s.x, s.y + h + em * .8f));
    ImGui::Dummy(ImVec2(1, 1));
}
// ---------- Версии под загрузчик и сборки ----------
// Показываем ТОЛЬКО те версии, под которые выбранный загрузчик реально можно поставить. Пустой список (не загрузился) = версий нет, а не «все подряд».
static bool VerShown(const Ver& v, const std::string& loader) { // вызывать под gVerMu
    if (v.type == "snapshot" && !cfg.snapshots) return false;
    if (v.type == "old_beta" && !cfg.showBeta) return false;
    if (v.type == "old_alpha" && !cfg.showAlpha) return false;
    const std::set<std::string>* s = loader == "Fabric" ? &gFabricGames : loader == "Forge" ? &gForgeGames : loader == "Quilt" ? &gQuiltGames :
                                     loader == "NeoForge" ? &gNeoGames : loader == "OptiFine" ? &gOptiGames : nullptr;
    return !s || s->count(v.id) > 0;
}
static std::vector<Build> gBuilds; static float gBuildsT = 99.f;
static std::mutex gPackMu; static std::string gPackSel; // id сборки, созданной из сборки модов (UI-поток выберет её)
static const Build* FindBuild(const std::string& id) { for (auto& b : gBuilds) if (b.id == id) return &b; return nullptr; }
static void SelectBuild(const std::string& id) {
    Build b; if (!LoadBuild(id, b)) return;
    cfg.buildId = b.id; cfg.loader = b.loader; cfg.version = b.ver; cfg.optiBuild = b.ofBuild; SaveConfig();
}
static void ScanBuilds(bool force) {
    { std::string pid; { std::lock_guard<std::mutex> l(gPackMu); pid.swap(gPackSel); } if (!pid.empty()) { SelectBuild(pid); force = true; } }
    gBuildsT += ImGui::GetIO().DeltaTime; if (!force && gBuildsT < 1.f) return; gBuildsT = 0;
    gBuilds = ListBuilds();
    if (gDelBusy) return;
    if (!cfg.buildId.empty() && !FindBuild(cfg.buildId)) { cfg.buildId.clear(); if (gBuilds.empty()) SaveConfig(); }
    if (cfg.buildId.empty() && !gBuilds.empty()) {
        const Build* pick = FindBuild(InstName(cfg.loader, cfg.version)); // прежний выбор из версии 0.0.5
        SelectBuild(pick ? pick->id : gBuilds[0].id);
    }
}
static fs::path CurrentInstanceDir() { return BuildDir(cfg.buildId); } // папка игры выбранной сборки
struct DelReq { Build b; };
static std::optional<DelReq> gDelAsk; // запрос на удаление (ждёт подтверждения в окне)
static void DeleteThread(Build b) {
    gDelBusy = true; Status(std::string(T("Удаляю сборку ", "Deleting build ")) + b.name + "...");
    DeleteBuild(b);
    Status(std::string(T("Сборка удалена: ", "Build deleted: ")) + b.name);
    gDelBusy = false;
}
static std::string FmtDur(long long s) {
    char t[64]; long long m = s / 60, h = m / 60; m %= 60;
    if (h > 0) snprintf(t, sizeof(t), cfg.lang == 1 ? "%lld h %lld min" : "%lld ч %lld мин", h, m);
    else snprintf(t, sizeof(t), cfg.lang == 1 ? "%lld min" : "%lld мин", m);
    return t;
}
// Выбор сборки OptiFine для версии игры mc: «Авто» + все сборки (лучшая первой). sel — выбранный ключ ("" = авто)
static bool OfBuildItems(const std::string& mc, std::string& sel) {
    std::vector<OfV> bl; { std::lock_guard<std::mutex> l(gVerMu); auto it = gOptiBuilds.find(mc); if (it != gOptiBuilds.end()) bl = it->second; }
    bool picked = false;
    if (ImGui::Selectable(T("Авто (лучшая сборка)", "Auto (best build)"), sel.empty())) { sel.clear(); picked = true; }
    for (size_t i = 0; i < bl.size(); i++) {
        ImGui::PushID((int)i);
        std::string lab = OfLabel(bl[i]) + (i == 0 ? T("   [новейшая]", "   [latest]") : "");
        if (ImGui::Selectable(lab.c_str(), OfKey(bl[i]) == sel)) { sel = OfKey(bl[i]); picked = true; }
        ImGui::PopID();
    }
    if (bl.empty()) ImGui::TextDisabled("%s", T("Список сборок ещё загружается...", "Build list is still loading..."));
    return picked;
}
static std::string OfBuildCaption(const std::string& mc, const std::string& sel) {
    if (sel.empty()) return T("Авто (лучшая сборка)", "Auto (best build)");
    std::lock_guard<std::mutex> l(gVerMu); auto it = gOptiBuilds.find(mc);
    if (it != gOptiBuilds.end()) for (auto& b : it->second) if (OfKey(b) == sel) return OfLabel(b);
    return sel;
}

// ---------- Мелкие иконки (рисуются линиями, без картинок и шрифтовых символов) ----------
static void MiniIcon(ImDrawList* dl, int id, ImVec2 c, float r, ImU32 col) {
    float th = std::max(1.5f, 1.5f * gS);
    switch (id) {
    case 0: dl->AddCircle(ImVec2(c.x, c.y - r * .38f), r * .3f, col, 0, th); dl->AddRect(ImVec2(c.x - r * .55f, c.y + r * .08f), ImVec2(c.x + r * .55f, c.y + r * .75f), col, R(r * .2f), 0, th); break; // скины
    case 1: for (int i = 0; i < 3; i++) { float h = r * (i == 0 ? .6f : i == 1 ? 1.2f : .9f), x = c.x - r * .7f + i * r * .5f; dl->AddRectFilled(ImVec2(x, c.y + r * .65f - h), ImVec2(x + r * .32f, c.y + r * .65f), col); } break; // статистика
    case 2: for (int i = 0; i < 3; i++) { float y = c.y - r * .5f + i * r * .5f; dl->AddLine(ImVec2(c.x - r * .65f, y), ImVec2(c.x + (i == 2 ? r * .1f : r * .65f), y), col, th); } break; // новости
    case 3: dl->AddRect(ImVec2(c.x - r * .65f, c.y - r * .2f), ImVec2(c.x + r * .25f, c.y + r * .7f), col, 0, 0, th); dl->AddRect(ImVec2(c.x - r * .25f, c.y - r * .7f), ImVec2(c.x + r * .65f, c.y + r * .2f), col, 0, 0, th); break; // сборки
    case 4: dl->AddRect(ImVec2(c.x - r * .75f, c.y - r * .35f), ImVec2(c.x + r * .75f, c.y + r * .6f), col, R(r * .15f), 0, th); dl->AddRectFilled(ImVec2(c.x - r * .75f, c.y - r * .6f), ImVec2(c.x - r * .15f, c.y - r * .3f), col); break; // папка
    case 5: { // шестерёнка: толстое кольцо с отверстием и 8 трапециевидных зубьев
        dl->AddCircle(c, r * .52f, col, 40, r * .34f);
        for (int i = 0; i < 8; i++) {
            float a = i * 0.7853982f, ca = cosf(a), sa = sinf(a), px = -sa, py = ca, ri = r * .6f, ro = r * .98f, wi = r * .21f, wo = r * .15f;
            dl->AddQuadFilled(ImVec2(c.x + ca * ri + px * wi, c.y + sa * ri + py * wi), ImVec2(c.x + ca * ro + px * wo, c.y + sa * ro + py * wo),
                              ImVec2(c.x + ca * ro - px * wo, c.y + sa * ro - py * wo), ImVec2(c.x + ca * ri - px * wi, c.y + sa * ri - py * wi), col);
        }
        break; }
    case 7: dl->AddTriangle(ImVec2(c.x - r * .85f, c.y - r * .05f), ImVec2(c.x, c.y - r * .8f), ImVec2(c.x + r * .85f, c.y - r * .05f), col, th); dl->AddRect(ImVec2(c.x - r * .6f, c.y - r * .05f), ImVec2(c.x + r * .6f, c.y + r * .75f), col, 0, 0, th); break; // главная
    default: { dl->PathArcTo(c, r * .6f, 0.5f, 5.4f); dl->PathStroke(col, 0, th); // обновить: дуга со стрелкой
               ImVec2 e(c.x + cosf(5.4f) * r * .6f, c.y + sinf(5.4f) * r * .6f); dl->AddTriangleFilled(ImVec2(e.x - r * .05f, e.y - r * .42f), ImVec2(e.x + r * .42f, e.y + r * .12f), ImVec2(e.x - r * .38f, e.y + r * .2f), col); break; }
    }
}
static bool SideBtn(const char* label, int icon, float w, float h) { // кнопка-«таблетка» с круглой иконкой слева (как на макете)
    bool clk = FancyBtn(label, ImVec2(w, h), gSoft);
    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax(); ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 c(a.x + h * .55f, (a.y + b.y) / 2); dl->AddCircleFilled(c, h * .32f, ImGui::GetColorU32(ImVec4(gAccent.x, gAccent.y, gAccent.z, .28f)));
    MiniIcon(dl, icon, c, h * .2f, ImGui::GetColorU32(gAccent));
    return clk;
}

// ============================ Скин: загрузка, текстуры, отрисовка ============================
// ============================ Скин: 3D-модель игрока ============================
// Ортографическая проекция: каждая грань куба превращается в параллелограмм, поэтому обычная
// «аффинная» текстурная картинка ImGui (AddImageQuad) рисует её без искажений. Без шейдеров и без GPU-кода.
struct SkinPx { std::vector<unsigned char> px; int w = 0, h = 0; }; // RGBA
static void SkinFill(SkinPx& s, int x, int y, int w, int h, int r, int g, int b, int a = 255) {
    for (int j = y; j < y + h; j++) for (int i = x; i < x + w; i++) {
        if (i < 0 || j < 0 || i >= s.w || j >= s.h) continue;
        unsigned char* p = &s.px[((size_t)j * s.w + i) * 4]; p[0] = (unsigned char)r; p[1] = (unsigned char)g; p[2] = (unsigned char)b; p[3] = (unsigned char)a;
    }
}
// Старый формат 64x32 -> 64x64: левые рука и нога зеркалом от правых (так же делает сама игра)
static void SkinCopyRect(SkinPx& s, int k, int x, int y, int dx, int dy, int w, int h) {
    x *= k; y *= k; dx *= k; dy *= k; w *= k; h *= k;
    for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) {
        int sx = x + i, sy = y + j, tx = x + dx + (w - 1 - i), ty = y + dy + j;
        if (sx < 0 || sy < 0 || sx >= s.w || sy >= s.h || tx < 0 || ty < 0 || tx >= s.w || ty >= s.h) continue;
        std::memcpy(&s.px[((size_t)ty * s.w + tx) * 4], &s.px[((size_t)sy * s.w + sx) * 4], 4);
    }
}
static void SkinFixLegacy(SkinPx& s) {
    if (s.w < 64 || s.h * 2 != s.w) return;
    int k = s.w / 64; std::vector<unsigned char> n((size_t)s.w * s.w * 4, 0);
    std::memcpy(n.data(), s.px.data(), s.px.size()); s.px.swap(n); s.h = s.w;
    SkinCopyRect(s, k, 4, 16, 16, 32, 4, 4);  SkinCopyRect(s, k, 8, 16, 16, 32, 4, 4);
    SkinCopyRect(s, k, 0, 20, 24, 32, 4, 12); SkinCopyRect(s, k, 4, 20, 16, 32, 4, 12);
    SkinCopyRect(s, k, 8, 20, 8, 32, 4, 12);  SkinCopyRect(s, k, 12, 20, 16, 32, 4, 12);
    SkinCopyRect(s, k, 44, 16, -8, 32, 4, 4); SkinCopyRect(s, k, 48, 16, -8, 32, 4, 4);
    SkinCopyRect(s, k, 40, 20, 0, 32, 4, 12); SkinCopyRect(s, k, 44, 20, -8, 32, 4, 12);
    SkinCopyRect(s, k, 48, 20, -16, 32, 4, 12); SkinCopyRect(s, k, 52, 20, -8, 32, 4, 12);
}
static bool SkinIsSlim(const SkinPx& s) { // у тонких (Alex) скинов правый столбец руки прозрачен
    if (s.w < 64 || s.h < 64) return false; int k = s.w / 64;
    const unsigned char* p = &s.px[((size_t)(20 * k) * s.w + 55 * k) * 4]; return p[3] == 0;
}
static SkinPx SkinDefault() { // офлайн-скин по умолчанию: «призрак» в винной толстовке с капюшоном
    SkinPx s; s.w = s.h = 64; s.px.assign(64 * 64 * 4, 0);
    struct C { int r, g, b; };
    const C SK{240, 200, 176}, SKD{214, 168, 146}, HR{44, 30, 58}, HRL{78, 54, 102}, HD{86, 30, 56}, HDL{122, 46, 78}, HDD{58, 18, 38}, PK{222, 106, 134}, WH{238, 238, 244},
            PN{34, 36, 58}, PNL{50, 54, 84}, PND{24, 26, 44}, BL{236, 150, 160}, MO{170, 92, 104};
    auto F = [&](int x, int y, int w, int h, const C& c) { SkinFill(s, x, y, w, h, c.r, c.g, c.b); };
    auto Box = [&](int ox, int oy, int w, int h, int d, const C& c) { // все 6 граней параллелепипеда по раскладке скина
        F(ox + d, oy, w, d, c); F(ox + d + w, oy, w, d, c); F(ox, oy + d, d, h, c); F(ox + d, oy + d, w, h, c); F(ox + d + w, oy + d, d, h, c); F(ox + 2 * d + w, oy + d, w, h, c);
    };
    // голова
    Box(0, 0, 8, 8, 8, SK); F(16, 0, 8, 8, SKD); F(8, 0, 8, 8, HR); F(24, 8, 8, 8, HR);
    F(10, 1, 3, 1, HRL); F(9, 3, 2, 1, HRL); F(25, 10, 3, 1, HRL); F(27, 13, 2, 1, HRL);
    F(8, 8, 8, 2, HR); F(8, 10, 1, 2, HR); F(15, 10, 1, 2, HR); F(11, 10, 2, 1, HR); F(9, 9, 2, 1, HRL);
    F(0, 8, 8, 2, HR); F(0, 10, 4, 6, HR); F(16, 8, 8, 2, HR); F(20, 10, 4, 6, HR);
    F(9, 11, 2, 1, HR); F(13, 11, 2, 1, HR);                                   // брови
    F(9, 12, 1, 1, WH); F(10, 12, 1, 1, PK); F(13, 12, 1, 1, PK); F(14, 12, 1, 1, WH); // глаза
    F(9, 13, 1, 1, BL); F(14, 13, 1, 1, BL); F(11, 14, 2, 1, MO);                // румянец и улыбка
    F(56, 11, 8, 5, HD); F(56, 11, 8, 1, HDL); F(32, 11, 3, 5, HD); F(53, 11, 3, 5, HD); // капюшон сзади (второй слой головы)
    // тело
    Box(16, 16, 8, 12, 4, HD); F(20, 20, 1, 12, HDD); F(27, 20, 1, 12, HDD);
    F(22, 20, 4, 1, SK); F(23, 21, 2, 1, SK);                                    // вырез горловины
    F(22, 22, 1, 3, WH); F(25, 22, 1, 3, WH); F(22, 25, 1, 1, PK); F(25, 25, 1, 1, PK); // шнурки
    F(21, 27, 6, 3, HDD); F(21, 27, 6, 1, HDL);                                  // карман-кенгуру
    F(20, 31, 8, 1, HDL); F(16, 31, 4, 1, HDL); F(28, 31, 4, 1, HDL); F(32, 31, 8, 1, HDL); // нижняя резинка
    { const char* G[7] = {".XXXX.", "XXXXXX", "XOXXOX", "XXXXXX", "XXXXXX", "XXXXXX", "X.XX.X"}; // призрак на спине
      for (int r = 0; r < 7; r++) for (int c = 0; c < 6; c++) { char ch = G[r][c]; if (ch == 'X') F(34 + c, 22 + r, 1, 1, WH); else if (ch == 'O') F(34 + c, 22 + r, 1, 1, PK); } }
    // руки: рукав, манжета, кисть
    for (int k = 0; k < 2; k++) {
        int ox = k == 0 ? 40 : 32, oy = k == 0 ? 16 : 48, fy = oy + 4;
        Box(ox, oy, 4, 12, 4, HD); F(ox + 8, oy, 4, 4, SK);
        F(ox + 8, fy, 4, 9, HDD); F(ox, fy + 9, 16, 1, HDL); F(ox, fy + 10, 16, 2, SK);
    }
    // ноги: джинсы и кроссовки
    for (int k = 0; k < 2; k++) {
        int ox = k == 0 ? 0 : 16, oy = k == 0 ? 16 : 48, fy = oy + 4;
        Box(ox, oy, 4, 12, 4, PN); F(ox + 8, oy, 4, 4, PK);
        F(ox + 5, fy + 1, 1, 6, PNL); F(ox + 8, fy, 4, 9, PND);
        F(ox, fy + 9, 16, 2, WH); F(ox, fy + 11, 16, 1, PK);
    }
    return s;
}

struct SV3 { float x, y, z; };
struct SFace { float x[4], y[4], u[4], v[4], z, shade; int tex; }; // tex: 0 скин, 1 плащ
struct SkinPose { float yaw = 0, pitch = 0, headYaw = 0, headPitch = 0, wave = 0, capeTilt = 0.12f, armSwing = 0; bool slim = false, cape = false; };
static SV3 sRotX(SV3 p, float a) { float c = cosf(a), s = sinf(a); return {p.x, p.y * c - p.z * s, p.y * s + p.z * c}; }
static SV3 sRotY(SV3 p, float a) { float c = cosf(a), s = sinf(a); return {p.x * c + p.z * s, p.y, -p.x * s + p.z * c}; }
static SV3 sRotZ(SV3 p, float a) { float c = cosf(a), s = sinf(a); return {p.x * c - p.y * s, p.x * s + p.y * c, p.z}; }
struct SPart { SV3 pivot{0, 0, 0}; float rx = 0, ry = 0, rz = 0; };
struct SView { float cx, cy, sc, yaw, pitch; };

static void SkinBox(std::vector<SFace>& out, const SView& V, const SPart& pt, int tex, SV3 c, float w, float h, float d, float infl, int u, int v, float texH, bool flip) {
    float hw = w / 2 + infl, hh = h / 2 + infl, hd = d / 2 + infl;
    struct Fd { SV3 q[4]; int ru, rv, rw, rh; float lit; };
    Fd fs_[6] = {
        {{{-hw, hh, hd}, {hw, hh, hd}, {hw, -hh, hd}, {-hw, -hh, hd}}, u + (int)d, v + (int)d, (int)w, (int)h, 0.92f},       // перед (+z)
        {{{hw, hh, -hd}, {-hw, hh, -hd}, {-hw, -hh, -hd}, {hw, -hh, -hd}}, u + (int)d + (int)w + (int)d, v + (int)d, (int)w, (int)h, 0.80f}, // спина (-z)
        {{{-hw, hh, -hd}, {-hw, hh, hd}, {-hw, -hh, hd}, {-hw, -hh, -hd}}, u, v + (int)d, (int)d, (int)h, 0.84f},           // правая сторона игрока (-x)
        {{{hw, hh, hd}, {hw, hh, -hd}, {hw, -hh, -hd}, {hw, -hh, hd}}, u + (int)d + (int)w, v + (int)d, (int)d, (int)h, 0.84f}, // левая сторона (+x)
        {{{-hw, hh, -hd}, {hw, hh, -hd}, {hw, hh, hd}, {-hw, hh, hd}}, u + (int)d, v, (int)w, (int)d, 1.0f},                 // верх
        {{{-hw, -hh, hd}, {hw, -hh, hd}, {hw, -hh, -hd}, {-hw, -hh, -hd}}, u + (int)d + (int)w, v, (int)w, (int)d, 0.60f}};  // низ
    for (auto& f : fs_) {
        SFace o; SV3 P3[4];
        for (int i = 0; i < 4; i++) {
            SV3 p = f.q[i]; if (flip) p = sRotY(p, 3.14159265f);
            p = {p.x + c.x - pt.pivot.x, p.y + c.y - pt.pivot.y, p.z + c.z - pt.pivot.z};
            p = sRotZ(sRotY(sRotX(p, pt.rx), pt.ry), pt.rz);
            p = {p.x + pt.pivot.x, p.y + pt.pivot.y - 16.f, p.z + pt.pivot.z};
            p = sRotX(sRotY(p, V.yaw), V.pitch); P3[i] = p;
        }
        SV3 e1{P3[3].x - P3[0].x, P3[3].y - P3[0].y, P3[3].z - P3[0].z}, e2{P3[1].x - P3[0].x, P3[1].y - P3[0].y, P3[1].z - P3[0].z};
        float nz = e1.x * e2.y - e1.y * e2.x; // z-компонента векторного произведения: >0 — грань смотрит на зрителя
        if (nz <= 0.0001f) continue;
        float u0 = f.ru / 64.f, u1 = (f.ru + f.rw) / 64.f, v0 = f.rv / texH, v1 = (f.rv + f.rh) / texH;
        const float uu[4] = {u0, u1, u1, u0}, vv[4] = {v0, v0, v1, v1};
        o.z = 0; for (int i = 0; i < 4; i++) { o.x[i] = V.cx + P3[i].x * V.sc; o.y[i] = V.cy - P3[i].y * V.sc; o.u[i] = uu[i]; o.v[i] = vv[i]; o.z += P3[i].z / 4; }
        o.shade = f.lit; o.tex = tex; out.push_back(o);
    }
}
// Собирает все видимые грани модели, от дальних к ближним
static void SkinBuildFaces(const SkinPose& P, float cx, float cy, float sc, std::vector<SFace>& out) {
    SView V{cx, cy, sc, P.yaw, P.pitch}; SPart none;
    float aw = P.slim ? 3.f : 4.f, ax = 4.f + aw / 2;
    SPart head; head.pivot = {0, 24, 0}; head.rx = P.headPitch; head.ry = P.headYaw;
    SPart ra; ra.pivot = {-ax, 22, 0}; ra.rx = -P.armSwing; ra.rz = -P.wave - 0.04f;
    SPart la; la.pivot = {ax, 22, 0}; la.rx = P.armSwing; la.rz = 0.04f;
    SPart rl; rl.pivot = {-2, 12, 0}; rl.rx = P.armSwing * 0.6f;
    SPart ll; ll.pivot = {2, 12, 0}; ll.rx = -P.armSwing * 0.6f;
    SkinBox(out, V, none, 0, {0, 18, 0}, 8, 12, 4, 0, 16, 16, 64, false);
    SkinBox(out, V, none, 0, {0, 18, 0}, 8, 12, 4, 0.25f, 16, 32, 64, false);
    SkinBox(out, V, head, 0, {0, 28, 0}, 8, 8, 8, 0, 0, 0, 64, false);
    SkinBox(out, V, head, 0, {0, 28, 0}, 8, 8, 8, 0.5f, 32, 0, 64, false);
    SkinBox(out, V, rl, 0, {-2, 6, 0}, 4, 12, 4, 0, 0, 16, 64, false);  SkinBox(out, V, rl, 0, {-2, 6, 0}, 4, 12, 4, 0.25f, 0, 32, 64, false);
    SkinBox(out, V, ll, 0, {2, 6, 0}, 4, 12, 4, 0, 16, 48, 64, false);  SkinBox(out, V, ll, 0, {2, 6, 0}, 4, 12, 4, 0.25f, 0, 48, 64, false);
    SkinBox(out, V, ra, 0, {-ax, 18, 0}, aw, 12, 4, 0, 40, 16, 64, false); SkinBox(out, V, ra, 0, {-ax, 18, 0}, aw, 12, 4, 0.25f, 40, 32, 64, false);
    SkinBox(out, V, la, 0, {ax, 18, 0}, aw, 12, 4, 0, 32, 48, 64, false);  SkinBox(out, V, la, 0, {ax, 18, 0}, aw, 12, 4, 0.25f, 48, 48, 64, false);
    if (P.cape) { SPart cp; cp.pivot = {0, 24, -2.8f}; cp.rx = P.capeTilt; SkinBox(out, V, cp, 1, {0, 16, -2.8f}, 10, 16, 1, 0, 0, 0, 32, true); }
    std::stable_sort(out.begin(), out.end(), [](const SFace& a, const SFace& b) { return a.z < b.z; });
}

static bool DecodeImg(const std::string& bytes, SkinPx& o) {
    int w = 0, h = 0, ch = 0; unsigned char* d = bytes.empty() ? nullptr : stbi_load_from_memory((const unsigned char*)bytes.data(), (int)bytes.size(), &w, &h, &ch, 4);
    if (!d) return false;
    o.w = w; o.h = h; o.px.assign(d, d + (size_t)w * h * 4); stbi_image_free(d); return w >= 22 && h >= 17;
}
static void CapeTo64(SkinPx& c) { // плащ старого формата 22x17 кладём в холст 64x32 (раскладка совпадает)
    if (c.w >= 64) return;
    SkinPx n; n.w = 64; n.h = 32; n.px.assign(64 * 32 * 4, 0);
    for (int y = 0; y < std::min(c.h, 32); y++) for (int x = 0; x < std::min(c.w, 64); x++) std::memcpy(&n.px[((size_t)y * 64 + x) * 4], &c.px[((size_t)y * c.w + x) * 4], 4);
    c = n;
}
struct SkinRes { bool hasSkin = false, hasCape = false, slim = false, local = false; SkinPx skin, cape; };
static std::mutex gSkinMu; static SkinRes gSkinRes; static bool gSkinReady = false; static std::atomic<bool> gSkinLoading{false}, gSkinReload{false};
static std::string gSkinKey, gSkinMsg;
static Tex gSkinTex, gCapeTex, gDefTex; static bool gSkinSlim = false, gSkinLocal = false, gHasCape = false;

static bool LoadSkinFile(const std::string& path, SkinPx& o) { return DecodeImg(ReadAll(P(path)), o); }
static void SkinLoadThread(int accType, std::string name, std::string token, std::string refresh, std::string uuid, std::string skinPath, std::string capePath) {
    SkinRes r; std::string msg;
    try {
        bool needSkin = true, needCape = true;
        if (!skinPath.empty() && LoadSkinFile(skinPath, r.skin)) { r.hasSkin = true; r.local = true; needSkin = false; }
        if (!capePath.empty() && LoadSkinFile(capePath, r.cape)) { r.hasCape = true; needCape = false; }
        if (accType == 1 && !name.empty() && (needSkin || needCape)) { // Ely.by
            if (needSkin) { Resp s = Http("https://skinsystem.ely.by/skins/" + UrlEnc(name) + ".png"); if (s.status == 200 && DecodeImg(s.body, r.skin)) r.hasSkin = true; }
            if (needCape) { Resp s = Http("https://skinsystem.ely.by/cloaks/" + UrlEnc(name) + ".png"); if (s.status == 200 && DecodeImg(s.body, r.cape)) r.hasCape = true; }
        } else if (accType == 2 && !token.empty() && (needSkin || needCape)) { // Microsoft: профиль Minecraft
            Resp pr = Http("https://api.minecraftservices.com/minecraft/profile", "GET", "", "Authorization: Bearer " + token + "\r\n");
            if (pr.status == 401 && !refresh.empty()) {
                Account a; a.type = 2; a.name = name; a.uuid = uuid; a.token = token; a.refresh = refresh;
                MsRefresh(a); { std::lock_guard<std::mutex> l(gMu); gNewAcc = a; }
                pr = Http("https://api.minecraftservices.com/minecraft/profile", "GET", "", "Authorization: Bearer " + a.token + "\r\n");
            }
            json j = Parse(pr.body);
            if (pr.status == 200 && j.is_object()) {
                if (needSkin && j.contains("skins") && j["skins"].is_array()) for (auto& s : j["skins"]) {
                    if (JStr(s, "state") != "ACTIVE") continue;
                    Resp img = Http(JStr(s, "url")); if (img.status == 200 && DecodeImg(img.body, r.skin)) { r.hasSkin = true; r.slim = JStr(s, "variant") == "SLIM"; } break;
                }
                if (needCape && j.contains("capes") && j["capes"].is_array()) for (auto& s : j["capes"]) {
                    if (JStr(s, "state") != "ACTIVE") continue;
                    Resp img = Http(JStr(s, "url")); if (img.status == 200 && DecodeImg(img.body, r.cape)) r.hasCape = true; break;
                }
            } else msg = T("Не удалось получить скин с аккаунта Microsoft", "Could not get the skin from the Microsoft account");
        }
    } catch (std::exception& e) { msg = e.what(); }
    if (r.hasSkin) { SkinFixLegacy(r.skin); if (!r.local && accType != 2) r.slim = SkinIsSlim(r.skin); }
    if (r.hasCape) CapeTo64(r.cape);
    { std::lock_guard<std::mutex> l(gSkinMu); gSkinRes = std::move(r); gSkinReady = true; if (!msg.empty()) gSkinMsg = msg; }
    gSkinLoading = false;
}
static Tex MakeTexUp(const SkinPx& s) { // увеличиваем «ближайшим соседом», чтобы пиксели оставались чёткими при линейной фильтрации
    int f = s.w >= 512 ? 1 : s.w >= 256 ? 2 : s.w >= 128 ? 4 : 8; int W2 = s.w * f, H2 = s.h * f;
    std::vector<unsigned char> up((size_t)W2 * H2 * 4);
    for (int y = 0; y < H2; y++) for (int x = 0; x < W2; x++) std::memcpy(&up[((size_t)y * W2 + x) * 4], &s.px[((size_t)(y / f) * s.w + (x / f)) * 4], 4);
    Tex t; t.v = MakeTex(up.data(), W2, H2); t.w = W2; t.h = H2; return t;
}
static void SkinTick() { // каждый кадр (UI-поток): запустить загрузку при смене аккаунта/файла, принять результат
    std::string key; const Account* a = (cfg.sel >= 0 && cfg.sel < (int)cfg.accs.size()) ? &cfg.accs[cfg.sel] : nullptr;
    key = std::to_string(a ? a->type : -1) + "|" + (a ? a->name : "") + "|" + cfg.skinPath + "|" + cfg.capePath;
    if ((key != gSkinKey || gSkinReload) && !gSkinLoading) {
        gSkinKey = key; gSkinReload = false; gSkinLoading = true;
        std::thread(SkinLoadThread, a ? a->type : -1, a ? a->name : std::string(), a ? a->token : std::string(), a ? a->refresh : std::string(), a ? a->uuid : std::string(), cfg.skinPath, cfg.capePath).detach();
    }
    if (!gDefTex.v) { SkinPx d = SkinDefault(); gDefTex = MakeTexUp(d); }
    std::lock_guard<std::mutex> l(gSkinMu);
    if (!gSkinReady) return; gSkinReady = false;
    if (gSkinTex.v) { gSkinTex.v->Release(); gSkinTex = Tex(); } if (gCapeTex.v) { gCapeTex.v->Release(); gCapeTex = Tex(); }
    if (gSkinRes.hasSkin) gSkinTex = MakeTexUp(gSkinRes.skin);
    gHasCape = gSkinRes.hasCape; if (gHasCape) gCapeTex = MakeTexUp(gSkinRes.cape);
    gSkinSlim = gSkinRes.slim; gSkinLocal = gSkinRes.local;
    gSkinRes = SkinRes();
}
static void MsUploadSkinThread(Account a, std::string path, bool slim) { // загрузка своего скина на аккаунт Microsoft (Mojang API)
    auto say = [](const std::string& s) { std::lock_guard<std::mutex> l(gSkinMu); gSkinMsg = s; };
    try {
        std::string bytes = ReadAll(P(path)); if (bytes.empty()) throw std::runtime_error("файл скина не читается");
        say(T("Загружаю скин...", "Uploading the skin..."));
        try { MsRefresh(a); { std::lock_guard<std::mutex> l(gMu); gNewAcc = a; } } catch (...) {} // свежий токен; если не вышло — пробуем со старым
        std::string bd = "----PTL" + RandHex(8);
        std::string body = "--" + bd + "\r\nContent-Disposition: form-data; name=\"variant\"\r\n\r\n" + (slim ? "slim" : "classic") + "\r\n--" + bd +
                           "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"skin.png\"\r\nContent-Type: image/png\r\n\r\n" + bytes + "\r\n--" + bd + "--\r\n";
        Resp r = Http("https://api.minecraftservices.com/minecraft/profile/skins", "POST", body, "Authorization: Bearer " + a.token + "\r\nContent-Type: multipart/form-data; boundary=" + bd + "\r\n");
        if (r.status == 200 || r.status == 204) { say(T("Скин применён к аккаунту Microsoft", "The skin was applied to the Microsoft account")); Log("Скин загружен на аккаунт " + a.name); }
        else { say(std::string(T("Не удалось загрузить скин: HTTP ", "Skin upload failed: HTTP ")) + std::to_string(r.status)); Log("[скин] загрузка: HTTP " + std::to_string(r.status) + " " + r.body.substr(0, 200)); }
    } catch (std::exception& e) { say(std::string(T("Ошибка: ", "Error: ")) + e.what()); }
}

static float gWaveT = -1.f; // время с начала приветственного взмаха рукой (-1 = не машет)
static bool gFollowOn = false; // слежение за курсором включается после приветственного взмаха
static void StartWave() { if (gWaveT < 0) gWaveT = 0.f; }
static float Smooth01(float x) { x = std::clamp(x, 0.f, 1.f); return x * x * (3 - 2 * x); }
static float gLookHY = 0, gLookHP = 0, gLookBY = 0;
// Рисует игрока. follow = смотрит за курсором и поворачивается к нему (как в инвентаре игры); иначе угол задаёт userYaw (вращение мышью)
static void DrawSkinModel(ImDrawList* dl, ImVec2 c, float sc, bool follow, float userYaw) {
    ImGuiIO& io = ImGui::GetIO(); float dt = std::min(io.DeltaTime, 0.05f);
    if (gWaveT >= 0.f) { gWaveT += dt; if (gWaveT > 2.8f) { gWaveT = -1.f; gFollowOn = true; } }
    SkinPose P; P.slim = gSkinLocal ? cfg.skinSlim : gSkinSlim; P.cape = cfg.showCape && gHasCape && gCapeTex.v;
    float tHY = 0, tHP = 0, tBY = 0;
    if (follow && !gFollowOn) tHY = 0.30f; // до конца приветствия: голова повёрнута на зрителя
    if (follow && gFollowOn && ImGui::IsMousePosValid()) {
        float hx = c.x, hy = c.y - 12 * sc; // голова
        float dx = std::clamp((io.MousePos.x - hx) / (sc * 14.f), -1.f, 1.f), dy = std::clamp((io.MousePos.y - hy) / (sc * 14.f), -1.f, 1.f);
        tHY = dx * 0.95f; tHP = dy * 0.5f; tBY = dx * 0.38f;
    }
    float k = std::min(1.f, dt * 9.f); gLookHY += (tHY - gLookHY) * k; gLookHP += (tHP - gLookHP) * k; gLookBY += (tBY - gLookBY) * k;
    P.yaw = (follow ? -0.30f : userYaw) + gLookBY; P.pitch = 0.10f;
    P.headYaw = gLookHY - gLookBY; P.headPitch = gLookHP;
    float tm = gTime;
    P.armSwing = 0.035f * sinf(tm * 1.7f); P.capeTilt = 0.10f + 0.04f * sinf(tm * 1.3f);
    if (gWaveT >= 0.f) P.wave = Smooth01(gWaveT / 0.4f) * Smooth01((2.8f - gWaveT) / 0.4f) * (2.45f + 0.28f * sinf(gWaveT * 11.f));
    { // тень под ногами
        ImVec2 pts[24]; float cy = c.y + 16.2f * sc, rx = 9.f * sc, ry = 1.7f * sc;
        for (int i = 0; i < 24; i++) { float a = i * 6.2831853f / 24; pts[i] = ImVec2(c.x + cosf(a) * rx, cy + sinf(a) * ry); }
        dl->AddConvexPolyFilled(pts, 24, IM_COL32(0, 0, 0, (int)(70 * ImGui::GetStyle().Alpha)));
    }
    static std::vector<SFace> faces; faces.clear(); SkinBuildFaces(P, c.x, c.y, sc, faces);
    for (auto& f : faces) {
        ID3D11ShaderResourceView* tv = f.tex == 0 ? (gSkinTex.v ? gSkinTex.v : gDefTex.v) : gCapeTex.v; if (!tv) continue;
        int sh = (int)(255 * f.shade), al255 = (int)(255 * ImGui::GetStyle().Alpha);
        dl->AddImageQuad((ImTextureID)(intptr_t)tv, ImVec2(f.x[0], f.y[0]), ImVec2(f.x[1], f.y[1]), ImVec2(f.x[2], f.y[2]), ImVec2(f.x[3], f.y[3]),
                         ImVec2(f.u[0], f.v[0]), ImVec2(f.u[1], f.v[1]), ImVec2(f.u[2], f.v[2]), ImVec2(f.u[3], f.v[3]), IM_COL32(sh, sh, sh, al255));
    }
}

// ============================ Проверка обновлений (GitHub Releases) ============================
static std::atomic<int> gUpdState{0}; // 0 не проверяли, 1 проверяю, 2 последняя версия, 3 есть новая, 4 ошибка, 5 релизов ещё нет
static std::string gUpdTag, gUpdUrl; static std::mutex gUpdMu;
static std::vector<int> VerNums(std::string s) { std::vector<int> v; if (!s.empty() && (s[0] == 'v' || s[0] == 'V')) s.erase(0, 1); for (auto& p : Split(s, '.')) v.push_back(atoi(p.c_str())); return v; }
static bool VerNewer(const std::string& a, const std::string& b) { auto x = VerNums(a), y = VerNums(b); size_t n = std::max(x.size(), y.size()); x.resize(n); y.resize(n); return x > y; }
static void UpdateCheckThread() {
    if (gUpdState == 1) return; gUpdState = 1;
    Resp r = Http(kRepoApi, "GET", "", "Accept: application/vnd.github+json\r\n");
    json j = Parse(r.body);
    if (r.status == 200 && j.is_object() && j.contains("tag_name") && j["tag_name"].is_string()) {
        std::string tag = j["tag_name"].get<std::string>(), url = JStr(j, "html_url");
        { std::lock_guard<std::mutex> l(gUpdMu); gUpdTag = tag; gUpdUrl = url.empty() ? kRepoUrl : url; }
        Log("Проверка обновлений: последний релиз " + tag + ", текущая версия " + kVer);
        gUpdState = VerNewer(tag, kVer) ? 3 : 2;
    } else if (r.status == 404) gUpdState = 5;
    else { Log("Проверка обновлений не удалась: HTTP " + std::to_string(r.status)); gUpdState = 4; }
}
static std::mutex gSzMu; static std::map<std::string, long long> gSizes; static std::set<std::string> gSzBusy;
static void SizeThread(std::string name) {
    std::error_code ec; long long sum = 0; fs::path d = gRoot / "instances" / P(name);
    for (auto it = fs::recursive_directory_iterator(d, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (it->is_regular_file(ec)) sum += (long long)it->file_size(ec);
    }
    std::lock_guard<std::mutex> l(gSzMu); gSizes[name] = sum; gSzBusy.erase(name);
}
static std::string FmtSize(long long b) {
    char t[48]; double v = (double)b;
    if (v < 1024.0) snprintf(t, sizeof(t), "%lld %s", b, T("Б", "B"));
    else if (v < 1048576.0) snprintf(t, sizeof(t), "%.1f %s", v / 1024.0, T("КБ", "KB"));
    else if (v < 1073741824.0) snprintf(t, sizeof(t), "%.1f %s", v / 1048576.0, T("МБ", "MB"));
    else snprintf(t, sizeof(t), "%.2f %s", v / 1073741824.0, T("ГБ", "GB"));
    return t;
}
static std::string FmtDate(const fs::path& p) { // дата создания папки = когда экземпляр скачан
    WIN32_FILE_ATTRIBUTE_DATA a{}; if (!GetFileAttributesExW(p.wstring().c_str(), GetFileExInfoStandard, &a)) return "?";
    FILETIME lf; SYSTEMTIME st; if (!FileTimeToLocalFileTime(&a.ftCreationTime, &lf) || !FileTimeToSystemTime(&lf, &st)) return "?";
    char t[48]; snprintf(t, sizeof(t), "%02d.%02d.%04d %02d:%02d", (int)st.wDay, (int)st.wMonth, (int)st.wYear, (int)st.wHour, (int)st.wMinute);
    return t;
}

// ---------- Java: список установленных и установка ----------
static const int kJavaVers[] = {8, 17, 21, 25};
static std::vector<JavaInfo> gJavas; static std::mutex gJavaMu; static std::atomic<bool> gJavaScan{false}; static std::atomic<int> gJavaInst{0};
static void RefreshJavas() {
    if (gJavaScan.exchange(true)) return;
    std::thread([] { auto j = FindJavas(); { std::lock_guard<std::mutex> l(gJavaMu); gJavas = j; } gJavaScan = false; }).detach();
}
static void JavaInstallThread(int v) {
    gJavaInst = v;
    bool ok = InstallJavaVer(v);
    Status(ok ? "Java " + std::to_string(v) + " установлена" : "Не удалось скачать Java " + std::to_string(v));
    { auto j = FindJavas(); std::lock_guard<std::mutex> l(gJavaMu); gJavas = j; }
    gJavaInst = 0;
}

// ---------- Моды (Modrinth) ----------
struct ModHit { std::string id, title, desc, author; long long dl = 0; };
static std::vector<ModHit> gModHits; static std::mutex gModMu; static std::string gModMsg, gModKey, gWantQ, gModChk;
static std::atomic<bool> gModBusy{false}, gModInst{false}, gModChkBusy{false};
static char bModQ[96];
// Установленные моды выбранного экземпляра: id проекта -> файлы в папке mods. Список хранится на диске рядом с папкой mods,
// поэтому «Установлено» остаётся после перезапуска, а удалённые вручную файлы пропадают из списка.
static std::map<std::string, std::vector<std::string>> gModFiles; static fs::path gModFilesDir; static float gModVerifyT = 99.f;
static fs::path ModManifest(const fs::path& inst) { return inst / "poltergeist_mods.json"; }
static void LoadModFiles(const fs::path& inst) { // вызывать под gModMu
    gModFiles.clear(); gModFilesDir = inst;
    json j = Parse(ReadAll(ModManifest(inst)));
    if (!j.is_object() || !j.contains("mods") || !j["mods"].is_object()) return;
    for (auto it = j["mods"].begin(); it != j["mods"].end(); ++it) {
        if (!it.value().is_object() || !it.value().contains("files") || !it.value()["files"].is_array()) continue;
        std::vector<std::string> f; for (auto& x : it.value()["files"]) if (x.is_string()) f.push_back(x.get<std::string>());
        if (!f.empty()) gModFiles[it.key()] = f;
    }
}
static void SaveModFiles() { // под gModMu
    json j; j["mods"] = json::object();
    for (auto& kv : gModFiles) { json o; o["files"] = kv.second; j["mods"][kv.first] = o; }
    WriteAll(ModManifest(gModFilesDir), j.dump(2));
}
static void ModsSync(const fs::path& inst) { // UI-поток: подгрузить список экземпляра и убрать моды, чьи файлы пропали с диска
    std::lock_guard<std::mutex> l(gModMu); std::error_code ec;
    if (inst != gModFilesDir) { LoadModFiles(inst); gModVerifyT = 99.f; }
    gModVerifyT += ImGui::GetIO().DeltaTime; if (gModVerifyT < 1.f) return; gModVerifyT = 0;
    bool ch = false;
    for (auto it = gModFiles.begin(); it != gModFiles.end();) {
        bool ok = true; for (auto& f : it->second) if (!fs::exists(inst / "mods" / P(f), ec)) ok = false;
        if (ok) ++it; else { it = gModFiles.erase(it); ch = true; }
    }
    if (ch) SaveModFiles();
}
static void ModRecord(const fs::path& inst, const std::string& pid, const std::string& fn) {
    std::lock_guard<std::mutex> l(gModMu);
    if (inst != gModFilesDir) LoadModFiles(inst);
    auto& v = gModFiles[pid]; if (std::find(v.begin(), v.end(), fn) == v.end()) v.push_back(fn);
    SaveModFiles();
}
static void ModDelete(const fs::path& inst, const std::string& pid) {
    std::lock_guard<std::mutex> l(gModMu); std::error_code ec;
    if (inst != gModFilesDir) LoadModFiles(inst);
    auto it = gModFiles.find(pid); if (it == gModFiles.end()) return;
    for (auto& f : it->second) fs::remove(inst / "mods" / P(f), ec);
    gModFiles.erase(it); SaveModFiles();
}
static std::string LoaderId(const std::string& l) { return l == "Fabric" ? "fabric" : l == "Forge" ? "forge" : l == "NeoForge" ? "neoforge" : l == "Quilt" ? "quilt" : ""; }
static void ModMsg(const std::string& s) { { std::lock_guard<std::mutex> l(gModMu); gModMsg = s; } Log(s); }
static void ModSearchThread(std::string q, std::string loader, std::string mc) {
    gModBusy = true;
    try {
        std::string cat = loader == "quilt" ? std::string("\"categories:quilt\",\"categories:fabric\"") : "\"categories:" + loader + "\"";
        std::string fac = "[[\"project_type:mod\"],[" + cat + "],[\"versions:" + mc + "\"]]";
        std::string url = "https://api.modrinth.com/v2/search?query=" + UrlEnc(q) + "&facets=" + UrlEnc(fac) + "&limit=40&index=" + (q.empty() ? "downloads" : "relevance");
        Resp r = Http(url); json j = Parse(r.body); std::vector<ModHit> out;
        if (r.status == 200 && j.is_object() && j.contains("hits") && j["hits"].is_array())
            for (auto& h : j["hits"]) {
                ModHit m; m.id = JStr(h, "project_id"); m.title = JStr(h, "title"); m.desc = JStr(h, "description"); m.author = JStr(h, "author");
                if (h.contains("downloads") && h["downloads"].is_number()) m.dl = h["downloads"].get<long long>();
                if (!m.id.empty()) out.push_back(m);
            }
        std::lock_guard<std::mutex> l(gModMu); gModHits = out;
        if (r.status != 200) gModMsg = T("Нет связи с Modrinth (интернет / VPN / фаервол)", "Cannot reach Modrinth (internet / VPN / firewall)");
        else if (out.empty()) gModMsg = T("Ничего не найдено для этой версии и загрузчика", "Nothing found for this version and loader");
        else gModMsg.clear();
    } catch (std::exception& e) { ModMsg(std::string("Modrinth: ") + e.what()); }
    gModBusy = false;
}
static bool InstallModRec(const std::string& pid, const std::string& loader, const std::string& mc, const fs::path& inst, std::set<std::string>& seen, int depth) {
    if (!seen.insert(pid).second) return true;
    Resp r = Http("https://api.modrinth.com/v2/project/" + UrlEnc(pid) + "/version?loaders=" + UrlEnc(loader == "quilt" ? std::string("[\"quilt\",\"fabric\"]") : "[\"" + loader + "\"]") + "&game_versions=" + UrlEnc("[\"" + mc + "\"]"));
    if (r.status == 0) throw std::runtime_error(T("Нет соединения с Modrinth", "No connection to Modrinth"));
    json a = Parse(r.body);
    if (r.status != 200 || !a.is_array() || a.empty()) return false;
    const json* pick = &a[0];
    for (auto& v : a) if (JStr(v, "version_type") == "release") { pick = &v; break; }
    if (!pick->is_object() || !pick->contains("files") || !(*pick)["files"].is_array() || (*pick)["files"].empty()) return false;
    const json& files = (*pick)["files"]; const json* f = &files[0];
    for (auto& x : files) if (x.is_object() && x.contains("primary") && x["primary"].is_boolean() && x["primary"].get<bool>()) { f = &x; break; }
    std::string fn = U8(fs::path(W(JStr(*f, "filename"))).filename().wstring()), url = JStr(*f, "url");
    if (fn.empty() || url.empty()) return false;
    fs::path dir = inst / "mods"; std::error_code ec; fs::create_directories(dir, ec);
    ModMsg(std::string(T("Скачиваю ", "Downloading ")) + fn);
    if (!Download(url, dir / P(fn))) throw std::runtime_error(std::string(T("Не удалось скачать ", "Failed to download ")) + fn);
    ModRecord(inst, pid, fn);
    if (depth < 3 && pick->contains("dependencies") && (*pick)["dependencies"].is_array())
        for (auto& d : (*pick)["dependencies"]) {
            if (JStr(d, "dependency_type") != "required") continue;
            std::string dp = JStr(d, "project_id"); if (dp.empty()) continue;
            try { InstallModRec(dp, loader, mc, inst, seen, depth + 1); } catch (...) {}
        }
    return true;
}
static void ModInstallThread(std::string pid, std::string title, std::string loader, std::string mc, std::string inst) {
    gModInst = true;
    try {
        std::set<std::string> seen;
        if (InstallModRec(pid, loader, mc, P(inst), seen, 0)) ModMsg(std::string(T("Установлено: ", "Installed: ")) + title);
        else ModMsg(cfg.lang == 1 ? "No versions of \"" + title + "\" for " + loader + " " + mc : "Для мода «" + title + "» нет версий под " + loader + " " + mc);
    } catch (std::exception& e) { ModMsg(std::string(T("Ошибка: ", "Error: ")) + e.what()); }
    gModInst = false;
}
static void ModrinthCheckThread() {
    if (gModChkBusy.exchange(true)) return;
    { std::lock_guard<std::mutex> l(gModMu); gModChk = T("Проверяю...", "Checking..."); }
    auto t0 = std::chrono::steady_clock::now(); Resp r = Http("https://api.modrinth.com/");
    int ms = (int)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    { std::lock_guard<std::mutex> l(gModMu); gModChk = r.status == 200 ? std::string(T("Modrinth доступен (", "Modrinth is reachable (")) + std::to_string(ms) + (cfg.lang == 1 ? " ms)" : " мс)")
                                                                          : std::string(T("Нет связи с Modrinth", "Cannot reach Modrinth")) + (r.status ? " (" + std::to_string(r.status) + ")" : ""); }
    gModChkBusy = false;
}

// ---------- Новости ----------
struct NewsItem { const char* ver; const char* ru; const char* en; };
static int gNewsTab = 0; // 0 новости лаунчера, 1 новости игры
static const NewsItem kNews[] = { // новые сверху
    {"v0.0.6", "Пофикшены мелкие баги, изменён интерфейс и добавлена полная поддержка Linux.", "Minor bugs fixed, the interface was redesigned and full Linux support was added."},
    {"v0.0.5", "Пофикшены некоторые баги, добавлено больше загрузчиков, моды полностью работают, доделаны настройки экземпляра.", "Some bugs fixed, more loaders added, mods fully working, instance settings completed."},
    {"v0.0.4", "Фикс визуала и добавление настроек экземпляра.", "Visual fixes and instance settings added."},
    {"v0.0.3", "Добавлено больше настроек. И моды.", "More settings added. And mods."},
    {"v0.0.2", "Hotfix Java. Скачивалась не та версия для 26.1-26.3.", "Java hotfix. The wrong Java version was downloaded for 26.1-26.3."},
    {"v0.0.1", "Добро пожаловать в ОБТ PTLauncher. Пока контента мало, с обновлениями будем добавлять его.", "Welcome to the PTLauncher open beta. There is little content for now, we will add more with updates."},
};
struct GNLine { const char* ru; const char* en; };
static const GNLine kGameNews[] = { // Minecraft 26.4 Snapshot 9 и 10
    {"Хотфикс краша. В Snapshot 10 исправили критический вылет игры при прокрутке меню мультиплеера (MC-313265), из-за которого сборку выпустили через три часа после девятого снапшота.",
     "Crash hotfix. Snapshot 10 fixes a critical crash when scrolling the multiplayer menu (MC-313265), which is why it was released three hours after snapshot 9."},
    {"Графический API Vulkan по умолчанию. Игра окончательно перешла на рендеринг через Vulkan вместо случайного выбора между ним и OpenGL (старый вариант всё ещё можно вернуть в настройках).",
     "Vulkan is now the default graphics API. The game now renders through Vulkan instead of randomly choosing between it and OpenGL (the old option can still be restored in the settings)."},
    {"Ускорение крафта и торговли. Добавлена новая горячая клавиша (по умолчанию R) для быстрого повтора последнего рецепта в книге или сделки у жителя.",
     "Faster crafting and trading. A new hotkey (R by default) quickly repeats the last recipe in the recipe book or the last trade with a villager."},
    {"Гибкая настройка тумана. Появился отдельный параметр, позволяющий настраивать отображение тумана (например, оставлять его только на небе, не затрагивая облака).",
     "Flexible fog settings. A separate option controls how fog is displayed (for example, keeping it only in the sky without affecting clouds)."},
    {"Улучшения книги рецептов и интерфейса. Вкладки книги теперь показывают названия категорий при наведении, сами рецепты подсвечиваются при фокусе, а управление с клавиатуры и диктор доработаны.",
     "Recipe book and interface improvements. Book tabs show category names on hover, recipes are highlighted on focus, and keyboard navigation and the narrator were improved."},
    {"Изменения мобов. Сокращено время случайного ускорения свиньи от удочки с морковью (с 7–49 до 7–35 секунд), а также внесены мелкие правки в поведение фантомов и лавомерок.",
     "Mob changes. The random speed boost of a pig on a carrot on a stick is shorter (7-35 seconds instead of 7-49), and phantoms and magma cubes received small behavior tweaks."},
    {"Настраиваемые раздатчики (для датапаков). Добавлен компонент minecraft:dispense_behavior и тип преобразования блоков minecraft:honeycomb (вощение блоков через раздатчик), что позволяет гибко задавать реакции предметов при выбрасывании из раздатчика.",
     "Customizable dispensers (for data packs). Added the minecraft:dispense_behavior component and the minecraft:honeycomb block conversion type (waxing blocks with a dispenser), allowing flexible item reactions when dispensed."},
};
static void NewsCard(const char* head, const std::vector<std::string>& lines, bool bullets) {
    float em = ImGui::GetFontSize(); ImDrawList* dl = ImGui::GetWindowDrawList();
    float aw = ImGui::GetContentRegionAvail().x, cw = std::min(aw, em * 46.f); ImVec2 p = ImGui::GetCursorScreenPos(); float x0 = p.x + (aw - cw) / 2;
    float ind = bullets ? em * 1.1f : 0.f, tw = cw - em * 2 - ind, h = em * .8f + em * 1.5f;
    for (auto& l : lines) h += ImGui::CalcTextSize(l.c_str(), nullptr, false, tw).y + em * .4f;
    h += em * .4f;
    Surface(dl, ImVec2(x0, p.y), ImVec2(x0 + cw, p.y + h), R(em * .8f), 1);
    dl->AddText(ImVec2(x0 + em, p.y + em * .8f), ImGui::GetColorU32(gAccent), head);
    float y = p.y + em * .8f + em * 1.5f;
    for (auto& l : lines) {
        float th = ImGui::CalcTextSize(l.c_str(), nullptr, false, tw).y;
        if (bullets) dl->AddText(ImVec2(x0 + em, y), ImGui::GetColorU32(gAccent), "•");
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(x0 + em + ind, y), TextCol(.9f), l.c_str(), nullptr, tw);
        y += th + em * .4f;
    }
    ImGui::Dummy(ImVec2(aw, h + em * .3f));
}

static bool Header(const char* title) { // заголовок экрана (акцентная полоска) + стрелка «назад» справа; true = нажали «назад»
    float em = ImGui::GetFontSize(); ImVec2 st = ImGui::GetCursorPos(); float w = ImGui::GetContentRegionAvail().x, bh = em * 2.3f, bw = em * 3.6f;
    ImDrawList* dl = ImGui::GetWindowDrawList(); ImVec2 sp = ImGui::GetCursorScreenPos();
    dl->AddRectFilled(ImVec2(sp.x, sp.y + bh * .12f), ImVec2(sp.x + 4.f * gS, sp.y + bh * .88f), U32(gAccent), 3.f);
    ImGui::PushFont(gFTitle); ImVec2 ts = ImGui::CalcTextSize(title);
    ImGui::SetCursorPos(ImVec2(st.x + em * .9f, st.y + (bh - ts.y) / 2)); ImGui::TextUnformatted(title); ImGui::PopFont();
    ImGui::SetCursorPos(ImVec2(st.x + w - bw, st.y)); bool back = BackBtn(ImVec2(bw, bh));
    ImGui::SetCursorPos(ImVec2(st.x, st.y + bh + em * .7f));
    return back;
}
static bool ComboI(const char* id, int& cur, const char* const* items, int n) {
    bool ch = false; ImGui::SetNextItemWidth(std::min(ImGui::GetFontSize() * 18.f, ImGui::GetContentRegionAvail().x));
    if (ImGui::BeginCombo(id, items[std::clamp(cur, 0, n - 1)])) {
        for (int i = 0; i < n; i++) if (ImGui::Selectable(items[i], cur == i)) { cur = i; ch = true; }
        ImGui::EndCombo();
    }
    return ch;
}
static void OpenUrl(const std::string& u) { ShellExecuteW(0, L"open", W(u).c_str(), 0, 0, SW_SHOWNORMAL); }

// ============================ Новый интерфейс: навигация, сцена за персонажем, главный экран ============================
static std::string AccTypeName(const Account& a) { return a.type == 0 ? T("Офлайн", "Offline") : a.type == 1 ? "Ely.by" : "Microsoft"; }
static float Hash01(int n) { unsigned x = (unsigned)n * 2654435761u; x ^= x >> 15; x *= 2246822519u; x ^= x >> 13; return (x & 0xFFFFu) / 65535.f; }

static void DrawFace(ImDrawList* dl, ImVec2 a, ImVec2 b, float rr) { // лицо игрока из текстуры скина (аватарка)
    ID3D11ShaderResourceView* tv = gSkinTex.v ? gSkinTex.v : gDefTex.v;
    if (!tv) { dl->AddRectFilled(a, b, U32(gAccent), rr); return; }
    dl->AddImageRounded((ImTextureID)(intptr_t)tv, a, b, ImVec2(8 / 64.f, 8 / 64.f), ImVec2(16 / 64.f, 16 / 64.f), IM_COL32_WHITE, rr);
    dl->AddImageRounded((ImTextureID)(intptr_t)tv, a, b, ImVec2(40 / 64.f, 8 / 64.f), ImVec2(48 / 64.f, 16 / 64.f), IM_COL32_WHITE, rr);
}

static void RoundedVGrad(ImDrawList* dl, ImVec2 a, ImVec2 b, float rr, ImVec4 top, ImVec4 bot) { // вертикальный градиент со скруглёнными углами
    float h = b.y - a.y; rr = std::min(rr, h * .5f - 1.f); if (rr < 1.f) rr = 1.f;
    auto C = [&](float y) { return U32(Mix(top, bot, std::clamp((y - a.y) / h, 0.f, 1.f))); };
    dl->AddRectFilled(a, ImVec2(b.x, a.y + rr), C(a.y + rr * .5f), rr, ImDrawFlags_RoundCornersTop);
    dl->AddRectFilled(ImVec2(a.x, b.y - rr), b, C(b.y - rr * .5f), rr, ImDrawFlags_RoundCornersBottom);
    dl->AddRectFilledMultiColor(ImVec2(a.x, a.y + rr), ImVec2(b.x, b.y - rr), C(a.y + rr), C(a.y + rr), C(b.y - rr), C(b.y - rr));
}

// Ночная пиксельная сцена: звёзды, луна, горы и искры
static void PixelScene(ImDrawList* dl, ImVec2 a, ImVec2 b, float rr) {
    float em = ImGui::GetFontSize(), w = b.x - a.x, h = b.y - a.y, px = std::max(2.f, floorf(em * .28f));
    ImVec4 top(.035f, .04f, .11f, 1.f), low = Mix(ImVec4(.10f, .05f, .17f, 1.f), ImVec4(gAccent.x * .55f, gAccent.y * .35f, gAccent.z * .45f, 1.f), .55f);
    RoundedVGrad(dl, a, b, rr, top, low);
    auto Sq = [&](float x, float y, float s, ImU32 c) { x = a.x + floorf((x - a.x) / px) * px; y = a.y + floorf((y - a.y) / px) * px; dl->AddRectFilled(ImVec2(x, y), ImVec2(x + s, y + s), c); };
    for (int i = 0; i < 46; i++) {
        float x = a.x + rr * .7f + Hash01(i * 7 + 1) * (w - rr * 1.4f), y = a.y + rr * .7f + Hash01(i * 7 + 2) * (h * .55f);
        float tw = .35f + .65f * (.5f + .5f * sinf(gTime * (.6f + Hash01(i * 7 + 3) * 1.5f) + i));
        Sq(x, y, px * (Hash01(i * 7 + 4) > .85f ? 1.5f : 1.f), WA(.8f * tw));
    }
    { ImVec2 mc(a.x + w * .78f, a.y + h * .22f); float r = std::max(em * 1.4f, h * .085f);
      for (int k = 4; k >= 1; k--) dl->AddCircleFilled(mc, r + k * r * .45f, AccA(.03f));
      dl->AddCircleFilled(mc, r, IM_COL32(246, 236, 214, 255));
      dl->AddCircleFilled(ImVec2(mc.x - r * .3f, mc.y - r * .2f), r * .22f, IM_COL32(226, 212, 186, 255));
      dl->AddCircleFilled(ImVec2(mc.x + r * .25f, mc.y + r * .3f), r * .15f, IM_COL32(226, 212, 186, 255)); }
    float ground = b.y - rr * 1.1f;
    for (int layer = 0; layer < 2; layer++) {
        ImU32 col = layer == 0 ? IM_COL32(34, 24, 66, 255) : IM_COL32(20, 14, 42, 255);
        for (float x = a.x + rr; x < b.x - rr; x += px) {
            float u = (x - a.x) / w, env = Smooth01(u / .10f) * Smooth01((1.f - u) / .10f);
            float hh = layer == 0 ? h * (.30f + .14f * sinf(u * 7.f + 1.f) + .07f * sinf(u * 17.f + 2.f)) : h * (.17f + .09f * sinf(u * 5.f + 4.f) + .05f * sinf(u * 23.f));
            hh = floorf(hh * env / px) * px; if (hh < 1.f) continue;
            dl->AddRectFilled(ImVec2(x, ground - hh), ImVec2(x + px, ground + 1.f), col);
        }
    }
    dl->AddRectFilled(ImVec2(a.x, ground), b, IM_COL32(10, 8, 22, 255), rr, ImDrawFlags_RoundCornersBottom);
    for (int i = 0; i < 28; i++) {
        float ph = fmodf(gTime * (.04f + .08f * Hash01(i * 11 + 1)) + Hash01(i * 11 + 2), 1.f);
        float x = a.x + rr + Hash01(i * 11 + 3) * (w - 2 * rr) + sinf(gTime * .9f + i) * px * 2.f, y = ground - ph * (h - rr * 2.2f);
        float al = sinf(ph * 3.14159f); ImU32 c = (i % 3) ? IM_COL32(255, 170, 90, (int)(220 * al)) : AccA(al);
        Sq(x, y, px * (i % 4 == 0 ? 1.5f : 1.f), c);
    }
}

static Tex gCharTex; static std::string gCharTexPath;
static void LoadCharTex() { // картинка «за персонажем» (грузится один раз при смене файла)
    if (gCharTexPath == cfg.charBg) return; gCharTexPath = cfg.charBg;
    if (gCharTex.v) { gCharTex.v->Release(); gCharTex = Tex(); }
    if (cfg.charBg.empty()) return;
    std::string bytes = ReadAll(P(cfg.charBg)); if (bytes.empty() || bytes.size() > 20u * 1024 * 1024) return;
    int w = 0, h = 0, ch = 0; unsigned char* d = stbi_load_from_memory((const unsigned char*)bytes.data(), (int)bytes.size(), &w, &h, &ch, 4); if (!d) return;
    int f = std::max(1, std::max(w, h) / 1600), nw = w / f, nh = h / f; std::vector<unsigned char> out((size_t)nw * nh * 4);
    for (int y = 0; y < nh; y++) for (int x = 0; x < nw; x++) for (int c = 0; c < 4; c++) {
        int s = 0; for (int dy = 0; dy < f; dy++) for (int dx = 0; dx < f; dx++) s += d[(((size_t)y * f + dy) * w + (x * f + dx)) * 4 + c];
        out[((size_t)y * nw + x) * 4 + c] = (unsigned char)(s / (f * f));
    }
    stbi_image_free(d); gCharTex.v = MakeTex(out.data(), nw, nh); gCharTex.w = nw; gCharTex.h = nh;
}
static void DrawStage(ImDrawList* dl, ImVec2 a, ImVec2 b, float rr) { // «сцена» за персонажем: своя картинка, пиксельная ночь или просто стекло
    LoadCharTex();
    if (gCharTex.v) {
        float ar = (float)gCharTex.w / gCharTex.h, sr = (b.x - a.x) / (b.y - a.y); ImVec2 u0(0, 0), u1(1, 1);
        if (ar > sr) { float k = sr / ar; u0.x = (1 - k) / 2; u1.x = 1 - u0.x; } else { float k = ar / sr; u0.y = (1 - k) / 2; u1.y = 1 - u0.y; }
        dl->AddImageRounded((ImTextureID)(intptr_t)gCharTex.v, a, b, u0, u1, IM_COL32_WHITE, rr);
        dl->AddRectFilled(a, b, BA(.18f), rr);
    } else if (cfg.charScene) PixelScene(dl, a, b, rr);
    else Surface(dl, a, b, rr, 1);
    dl->AddRect(a, b, GlassOn() ? WA(.24f) : WA(.10f), rr, 0, 1.3f);
}

static void StageView(ImDrawList* dl, ImVec2 a, ImVec2 b) { // сцена с игроком по центру (смотрит за курсором, по клику машет)
    float em = ImGui::GetFontSize(), rr = R(em * 1.1f);
    DrawStage(dl, a, b, rr);
    float sh = b.y - a.y, sc = std::min((sh - em * 4.6f) / 33.f, (b.x - a.x) / 24.f); if (sc < 1.f) sc = 1.f;
    ImVec2 c((a.x + b.x) / 2, a.y + em * 1.2f + 16.5f * sc);
    DrawSkinModel(dl, c, sc, true, 0.f);
    const Account* ac = (cfg.sel >= 0 && cfg.sel < (int)cfg.accs.size()) ? &cfg.accs[cfg.sel] : nullptr;
    std::string nick = ac ? ac->name : std::string(T("Гость", "Guest"));
    ImFont* f = FSub(); ImVec2 ns = ImGui::CalcTextSize(nick.c_str()); float tw = ns.x * (f->FontSize / ImGui::GetFontSize());
    float pw = tw + em * 2.2f, ph = em * 1.9f, py = c.y + 16.2f * sc + em * .7f; ImVec2 p0(c.x - pw / 2, py);
    dl->AddRectFilled(p0, ImVec2(p0.x + pw, p0.y + ph), BA(.38f), R(ph * .5f)); dl->AddRect(p0, ImVec2(p0.x + pw, p0.y + ph), WA(.18f), R(ph * .5f));
    dl->AddText(f, f->FontSize, ImVec2(c.x - tw / 2, p0.y + (ph - f->FontSize) / 2), IM_COL32(255, 255, 255, 235), nick.c_str());
}

template <class F> static bool OptionCard(const char* id, float cw, float ph, bool sel, const char* title, const char* desc, F preview) { // карточка выбора с миниатюрой
    float em = ImGui::GetFontSize(); ImDrawList* dl = ImGui::GetWindowDrawList(); ImGui::PushID(id);
    float ch = ph + em * 3.4f; ImVec2 p = ImGui::GetCursorScreenPos();
    bool clk = ImGui::InvisibleButton("##oc", ImVec2(cw, ch)); float t = AnimK("h", ImGui::IsItemHovered() ? 1.f : 0.f, 14.f);
    Surface(dl, p, ImVec2(p.x + cw, p.y + ch), R(em * .9f), 1, sel ? 1.f : t * .5f);
    preview(dl, ImVec2(p.x + em * .7f, p.y + em * .7f), ImVec2(p.x + cw - em * .7f, p.y + em * .7f + ph - em * .4f));
    dl->AddText(ImVec2(p.x + em * .9f, p.y + ph + em * .55f), TextCol(), title);
    dl->AddText(ImVec2(p.x + em * .9f, p.y + ph + em * 1.75f), TextCol(.55f), desc);
    if (sel) { ImVec2 m(p.x + cw - em * 1.3f, p.y + em * 1.3f); dl->AddCircleFilled(m, em * .6f, U32(gAccent)); IconCheck(dl, m, em * .26f, OnAccent()); }
    ImGui::PopID(); return clk;
}
static bool Toggle(const char* id, bool* v) { // переключатель-«таблетка»
    float em = ImGui::GetFontSize(); ImVec2 sz(em * 2.8f, em * 1.5f), p = ImGui::GetCursorScreenPos(); ImDrawList* dl = ImGui::GetWindowDrawList();
    bool clk = ImGui::InvisibleButton(id, sz); if (clk) *v = !*v;
    float t = Anim(ImGui::GetItemID(), *v ? 1.f : 0.f, 14.f, *v ? 1.f : 0.f);
    dl->AddRectFilled(p, ImVec2(p.x + sz.x, p.y + sz.y), WA(.16f), sz.y / 2); dl->AddRectFilled(p, ImVec2(p.x + sz.x, p.y + sz.y), AccA(.9f * t), sz.y / 2);
    dl->AddCircleFilled(ImVec2(p.x + sz.y / 2 + t * (sz.x - sz.y), p.y + sz.y / 2), sz.y / 2 - 3.f * gS, IM_COL32(255, 255, 255, 245));
    return clk;
}

// ---------- Левая навигация ----------
static void DrawNav(float x, float y, float w, float h) {
    float em = ImGui::GetFontSize(); ImDrawList* dl = ImGui::GetWindowDrawList(); bool compact = w < em * 6.f;
    ImVec4 tint = PanelTint(1.f); Surface(dl, ImVec2(x, y), ImVec2(x + w, y + h), R(em * 1.1f), 2, 0.f, &tint);
    struct It { int icon; const char *ru, *en; int scr; };
    const It items[] = {{7, "Главная", "Home", S_MAIN}, {3, "Моды и сборки", "Mods & modpacks", S_MODS}, {0, "Скины", "Skins", S_SKINS}, {1, "Статистика", "Statistics", S_STATS}, {2, "Новости", "News", S_NEWS}, {4, "Папки", "Folders", -1}};
    int cur = (gScr == S_BUILDS || gScr == S_CREATE) ? S_MAIN : gScr == S_LOG ? S_SET : gScr;
    float pad = em * .7f, ih = em * 2.7f, gap = em * .3f, iw = w - pad * 2, yy = y + pad; ImVec2 folderPos(x + w, y);
    auto Row = [&](int icon, const char* label, bool active, float ypos, ImVec2* rectMin) -> bool {
        ImGui::SetCursorScreenPos(ImVec2(x + pad, ypos)); ImGui::PushID(label);
        bool clk = ImGui::InvisibleButton("##n", ImVec2(iw, ih)); float t = AnimK("h", ImGui::IsItemHovered() ? 1.f : 0.f, 14.f);
        ImVec2 a(x + pad, ypos), b(a.x + iw, a.y + ih); if (rectMin) *rectMin = ImVec2(b.x, a.y);
        if (active) { dl->AddRectFilled(a, b, AccA(.22f), R(ih * .36f)); dl->AddRect(a, b, AccA(.55f), R(ih * .36f), 0, 1.2f); dl->AddRectFilled(ImVec2(a.x - pad * .55f, a.y + ih * .22f), ImVec2(a.x - pad * .55f + 3.f * gS, b.y - ih * .22f), U32(gAccent), 2.f); }
        else if (t > .01f) dl->AddRectFilled(a, b, WA(.07f * t), R(ih * .36f));
        ImVec2 c(compact ? (a.x + b.x) / 2 : a.x + ih * .55f, (a.y + b.y) / 2);
        MiniIcon(dl, icon, c, ih * .25f, active ? U32(gAccent) : TextCol(.65f + .35f * t));
        if (!compact) { ImVec2 ts = ImGui::CalcTextSize(label); dl->AddText(ImVec2(a.x + ih * 1.05f, c.y - ts.y / 2), TextCol(active ? 1.f : .7f + .3f * t), label); }
        ImGui::PopID(); return clk;
    };
    for (auto& it : items) {
        const char* lab = T(it.ru, it.en); ImVec2 rm;
        if (Row(it.icon, lab, it.scr >= 0 && it.scr == cur, yy, &rm)) { if (it.scr >= 0) Go(it.scr); else ImGui::OpenPopup("##folders"); }
        if (it.scr < 0) folderPos = rm;
        yy += ih + gap;
    }
    { float ay = y + h - pad - ih; if (Row(5, T("Настройки", "Settings"), cur == S_SET, ay, nullptr)) Go(S_SET); } // внизу только настройки
    ImGui::SetNextWindowPos(ImVec2(folderPos.x + 6, folderPos.y)); ImGui::SetNextWindowSize(ImVec2(em * 15, 0));
    if (ImGui::BeginPopup("##folders")) {
        fs::path gd = CurrentInstanceDir(); std::error_code ec;
        if (ImGui::Selectable(T("Папка игры (сборки)", "Game (build) folder"))) Open(gd);
        if (ImGui::Selectable(T("Папка mods", "Mods folder"))) Open(gd / "mods");
        if (fs::exists(gd / "shaderpacks", ec) && ImGui::Selectable(T("Папка shaderpacks", "Shaderpacks folder"))) Open(gd / "shaderpacks");
        if (fs::exists(gd / "screenshots", ec) && ImGui::Selectable(T("Скриншоты", "Screenshots"))) Open(gd / "screenshots");
        if (ImGui::Selectable(T("Папка лаунчера", "Launcher folder"))) Open(gCfgDir);
        ImGui::EndPopup();
    }
}

// ---------- Нижняя панель: статус + «Создать сборку | ИГРАТЬ | аккаунт» ----------
static void PlayBar(float W, float H, float rowH, float statusH) {
    float em = ImGui::GetFontSize(), sp = ImGui::GetStyle().ItemSpacing.x; ImDrawList* dl = ImGui::GetWindowDrawList();
    float yRow = H - rowH, yStatus = yRow - statusH;
    { // статус и полоса прогресса
        float w2 = std::min(W, em * 38.f); ImGui::SetCursorPos(ImVec2((W - w2) / 2, yStatus));
        ImVec2 p = ImGui::GetCursorScreenPos(); float h = em * .36f, pr = AnimK("prog", gProg.load(), 6.f); bool busy = gBusy;
        dl->AddRectFilled(ImVec2(p.x, p.y + 2), ImVec2(p.x + w2, p.y + 2 + h), WA(.10f), R(h / 2));
        if (pr > 0.005f) {
            for (int k = 3; k >= 1; k--) dl->AddRectFilled(ImVec2(p.x, p.y + 2 - k), ImVec2(p.x + w2 * pr, p.y + 2 + h + k), AccA(.07f), R(h / 2 + k));
            dl->AddRectFilled(ImVec2(p.x, p.y + 2), ImVec2(p.x + w2 * pr, p.y + 2 + h), U32(gAccent), R(h / 2));
            if (busy) { float sh = fmodf(gTime * 0.8f, 1.f); float sx = p.x + (w2 * pr) * sh; dl->AddRectFilled(ImVec2(sx, p.y + 2), ImVec2(std::min(sx + em * 2, p.x + w2 * pr), p.y + 2 + h), IM_COL32(255, 255, 255, 80), R(h / 2)); }
        }
        std::string st; { std::lock_guard<std::mutex> l(gMu); st = gStatus; }
        ImVec2 tsz = ImGui::CalcTextSize(st.c_str()); ImVec4 clip(p.x, p.y, p.x + w2, p.y + statusH);
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(p.x + std::max(0.f, (w2 - tsz.x) / 2), p.y + h + em * .45f), TextCol(.8f), st.c_str(), nullptr, w2, &clip);
        ImGui::Dummy(ImVec2(w2, statusH));
    }
    float total = std::min(W, em * 64.f), wl = (total - 2 * sp) * 0.27f, wc = total - 2 * sp - 2 * wl, x0 = (W - total) / 2; bool busy = gBusy;
    ImGui::SetCursorPos(ImVec2(x0, yRow));
    if (FancyBtn(T("Сборки", "Builds"), ImVec2(wl, rowH), gSoft)) Go(S_BUILDS);
    ImGui::SetCursorPos(ImVec2(x0 + wl + sp, yRow));
    if (busy) { // идёт загрузка/запуск: слева статус, справа «Остановить» (или «Закрыть», когда игра уже запущена)
        bool run = gGameRunning, req = gCancel;
        float sb = std::max(em * 7.f, wc * 0.40f), pw = wc - sb - sp;
        FancyBtn(run ? T("Игра запущена", "Game running") : T("Работаю...", "Working..."), ImVec2(pw, rowH), gAccent, false);
        ImGui::SetCursorPos(ImVec2(x0 + wl + sp + pw + sp, yRow));
        if (FancyBtn(req ? T("Останавливаю...", "Stopping...") : run ? T("Закрыть", "Close") : T("Остановить", "Stop"), ImVec2(sb, rowH), ImVec4(0.80f, 0.27f, 0.27f, 1.f), !req)) StopLaunch();
    } else {
        const char* pl = T("ИГРАТЬ", "PLAY");
        bool go = FancyBtn(pl, ImVec2(wc, rowH), gAccent, !gDelBusy);
        { ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax(), m((a.x + b.x) / 2, (a.y + b.y) / 2); float tw = ImGui::CalcTextSize(pl).x, s = em * .62f, cx = m.x - tw / 2 - em * 1.2f;
          dl->AddTriangleFilled(ImVec2(cx - s * .6f, m.y - s), ImVec2(cx - s * .6f, m.y + s), ImVec2(cx + s * .9f, m.y), OnAccent(gDelBusy ? .55f : 1.f)); }
        if (go) { if (cfg.buildId.empty()) Go(S_CREATE); else StartLaunch(); }
    }
    ImGui::SetCursorPos(ImVec2(x0 + wl + sp + wc + sp, yRow));
    { // карточка аккаунта с аватаркой
        ImVec2 p = ImGui::GetCursorScreenPos(); bool clk = ImGui::InvisibleButton("##accb", ImVec2(wl, rowH)); float t = AnimK("h", ImGui::IsItemHovered() ? 1.f : 0.f, 14.f);
        Surface(dl, p, ImVec2(p.x + wl, p.y + rowH), R(rowH * .38f), 0, t);
        const Account* ac = (cfg.sel >= 0 && cfg.sel < (int)cfg.accs.size()) ? &cfg.accs[cfg.sel] : nullptr;
        float fs = rowH * .62f; ImVec2 f0(p.x + rowH * .19f, p.y + (rowH - fs) / 2); DrawFace(dl, f0, ImVec2(f0.x + fs, f0.y + fs), R(fs * .25f));
        std::string nm = ac ? ac->name : std::string(T("Добавить аккаунт", "Add account")), sb = ac ? AccTypeName(*ac) : std::string(T("Нажми, чтобы войти", "Click to sign in"));
        float tx = f0.x + fs + em * .7f; ImVec4 clip(tx, p.y, p.x + wl - em * .5f, p.y + rowH);
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(tx, p.y + rowH * .5f - em * 1.05f), TextCol(), nm.c_str(), nullptr, 0.f, &clip);
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(tx, p.y + rowH * .5f + em * .05f), TextCol(.55f), sb.c_str(), nullptr, 0.f, &clip);
        if (clk) Go(S_ACC);
    }
}

static void PlayTimePill(ImDrawList* dl, ImVec2 topRight) { // «В игре: N мин» поверх сцены, справа сверху (клик - статистика)
    float em = ImGui::GetFontSize(); std::string pt = std::string(T("В игре: ", "Played: ")) + FmtDur(cfg.playSec);
    ImVec2 ts = ImGui::CalcTextSize(pt.c_str()); float h = em * 2.2f, w = ts.x + em * 2.4f; ImVec2 q(topRight.x - w - em * .9f, topRight.y + em * .9f);
    ImGui::SetCursorScreenPos(q); bool clk = ImGui::InvisibleButton("##playtime", ImVec2(w, h)); float t = AnimK("h", ImGui::IsItemHovered() ? 1.f : 0.f, 14.f);
    dl->AddRectFilled(q, ImVec2(q.x + w, q.y + h), BA(.40f), R(h * .5f)); dl->AddRect(q, ImVec2(q.x + w, q.y + h), WA(.18f + .20f * t), R(h * .5f), 0, 1.2f);
    dl->AddText(ImVec2(q.x + (w - ts.x) / 2, q.y + (h - ts.y) / 2), IM_COL32(255, 255, 255, 235), pt.c_str());
    if (clk) Go(S_STATS);
}
// ---------- Главный экран: «Игровой» (персонаж по центру) и «Библиотека» (сетка сборок + недавние миры) ----------
struct RWorld { std::string name, date; long long t = 0; };
static std::vector<RWorld> gRW; static float gRWT = 99.f; static std::string gRWBuild;
static std::string FmtWDate(const fs::path& p) {
    WIN32_FILE_ATTRIBUTE_DATA a{}; if (!GetFileAttributesExW(p.wstring().c_str(), GetFileExInfoStandard, &a)) return "?";
    FILETIME lf; SYSTEMTIME st; if (!FileTimeToLocalFileTime(&a.ftLastWriteTime, &lf) || !FileTimeToSystemTime(&lf, &st)) return "?";
    char t[48]; snprintf(t, sizeof(t), "%02d.%02d.%04d %02d:%02d", (int)st.wDay, (int)st.wMonth, (int)st.wYear, (int)st.wHour, (int)st.wMinute);
    return t;
}
static void ScanRecentWorlds() {
    gRWT += ImGui::GetIO().DeltaTime; if (gRWT < 3.f && gRWBuild == cfg.buildId) return; gRWT = 0; gRWBuild = cfg.buildId; gRW.clear();
    if (cfg.buildId.empty()) return; std::error_code ec;
    for (auto& e : fs::directory_iterator(CurrentInstanceDir() / "saves", ec)) {
        if (!e.is_directory(ec)) continue; RWorld w; w.name = U8(e.path().filename().wstring()); w.date = FmtWDate(e.path());
        auto ft = fs::last_write_time(e.path(), ec); w.t = (long long)ft.time_since_epoch().count(); gRW.push_back(w);
    }
    std::sort(gRW.begin(), gRW.end(), [](const RWorld& a, const RWorld& b) { return a.t > b.t; }); if (gRW.size() > 6) gRW.resize(6);
}
static ImVec4 LoaderColor(const std::string& l) {
    if (l == "Fabric") return C8(214, 196, 150, 255); if (l == "Forge") return C8(110, 130, 190, 255); if (l == "NeoForge") return C8(232, 146, 70, 255);
    if (l == "Quilt") return C8(160, 110, 210, 255); if (l == "OptiFine") return C8(210, 90, 90, 255); return C8(96, 176, 98, 255);
}
static void ScrMain() {
    float em = ImGui::GetFontSize(); ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 org = ImGui::GetCursorScreenPos(); float W = ImGui::GetContentRegionAvail().x, H = ImGui::GetContentRegionAvail().y, sp = ImGui::GetStyle().ItemSpacing.x;
    ScanBuilds(false);
    float rowH = em * 3.1f, statusH = em * 2.3f;
    float sy = 0.f, sh = std::max(em * 9.f, H - rowH - statusH - em * .5f);
    if (cfg.home == 1) { // ---- Игровой ----
        StageView(dl, ImVec2(org.x, org.y + sy), ImVec2(org.x + W, org.y + sy + sh));
        PlayTimePill(dl, ImVec2(org.x + W, org.y + sy));
    } else { // ---- Библиотека ----
        float gap = em * .9f, lw = std::min(std::max(em * 22.f, W * .60f), W - em * 16.f), gridH = sh * .62f;
        ImGui::SetCursorPos(ImVec2(0, sy));
        ImGui::BeginChild("libgrid", ImVec2(lw, gridH), 0, ImGuiWindowFlags_NoBackground);
        {
            ImDrawList* cdl = ImGui::GetWindowDrawList(); float aw = ImGui::GetContentRegionAvail().x - em * .8f; int cols = aw > em * 34.f ? 3 : 2; float cw = (aw - sp * (cols - 1)) / cols, ch = em * 5.2f;
            int n = (int)gBuilds.size() + 1;
            for (int i = 0; i < n; i++) {
                if (i % cols) ImGui::SameLine(0, sp);
                ImGui::PushID(i); ImVec2 p = ImGui::GetCursorScreenPos(); bool clk = ImGui::InvisibleButton("##c", ImVec2(cw, ch)); float t = AnimK("h", ImGui::IsItemHovered() ? 1.f : 0.f, 14.f);
                if (i == (int)gBuilds.size()) { // карточка «создать»
                    Surface(cdl, p, ImVec2(p.x + cw, p.y + ch), R(em * .9f), 0, t); ImVec2 m(p.x + cw / 2, p.y + ch / 2 - em * .45f); float s = em * .6f; ImU32 c = TextCol(.6f + .4f * t);
                    cdl->AddLine(ImVec2(m.x - s, m.y), ImVec2(m.x + s, m.y), c, 2.f * gS); cdl->AddLine(ImVec2(m.x, m.y - s), ImVec2(m.x, m.y + s), c, 2.f * gS);
                    const char* l = T("Создать сборку", "Create build"); ImVec2 ls = ImGui::CalcTextSize(l); cdl->AddText(ImVec2(p.x + (cw - ls.x) / 2, m.y + s + em * .5f), c, l);
                    if (clk) Go(S_CREATE);
                } else {
                    const Build& b = gBuilds[i]; bool sel = b.id == cfg.buildId;
                    Surface(cdl, p, ImVec2(p.x + cw, p.y + ch), R(em * .9f), 1, sel ? 1.f : t * .5f);
                    float ts = ch * .46f; ImVec2 t0(p.x + em * .9f, p.y + em * .9f); cdl->AddRectFilled(t0, ImVec2(t0.x + ts, t0.y + ts), U32(LoaderColor(b.loader)), R(ts * .3f));
                    { std::string c1 = b.loader.substr(0, 1); ImVec2 cs = ImGui::CalcTextSize(c1.c_str()); cdl->AddText(ImVec2(t0.x + (ts - cs.x) / 2, t0.y + (ts - cs.y) / 2), IM_COL32(20, 20, 28, 235), c1.c_str()); }
                    float tx = t0.x + ts + em * .7f; ImVec4 clip(tx, p.y, p.x + cw - em * 2.2f, p.y + ch);
                    cdl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(tx, t0.y), TextCol(), b.name.c_str(), nullptr, 0.f, &clip);
                    std::string sb = b.loader + " " + b.ver; cdl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(tx, t0.y + em * 1.25f), TextCol(.55f), sb.c_str(), nullptr, 0.f, &clip);
                    std::string pt = FmtDur(b.playSec); cdl->AddText(ImVec2(p.x + em * .9f, p.y + ch - em * 1.7f), TextCol(.45f), pt.c_str());
                    if (sel) { ImVec2 m(p.x + cw - em * 1.2f, p.y + em * 1.2f); cdl->AddCircleFilled(m, em * .55f, U32(gAccent)); IconCheck(cdl, m, em * .24f, OnAccent()); }
                    if (clk) SelectBuild(b.id);
                }
                ImGui::PopID();
            }
        }
        ImGui::EndChild();
        { // недавние миры выбранной сборки
            ScanRecentWorlds(); float wy = sy + gridH + gap * .5f; ImVec2 hp(org.x, org.y + wy);
            dl->AddText(hp, TextCol(.55f), T("НЕДАВНИЕ МИРЫ", "RECENT WORLDS"));
            float chipH = em * 3.2f, cy = org.y + wy + em * 1.6f, chipW = (lw - 2 * sp) / 3.f;
            if (gRW.empty()) dl->AddText(ImVec2(org.x, cy + em * .4f), TextCol(.45f), T("Миров пока нет: они появятся после первой игры.", "No worlds yet: they will appear after the first game."));
            for (int i = 0; i < (int)gRW.size() && i < 3; i++) {
                ImVec2 p(org.x + i * (chipW + sp), cy); Surface(dl, p, ImVec2(p.x + chipW, p.y + chipH), R(em * .8f), 0);
                ImVec2 m(p.x + chipH * .5f, p.y + chipH / 2); float s = em * .5f; dl->AddRect(ImVec2(m.x - s, m.y - s), ImVec2(m.x + s, m.y + s), AccA(.9f), R(2.f), 0, 1.6f * gS); dl->AddLine(ImVec2(m.x - s, m.y - s * .2f), ImVec2(m.x + s, m.y - s * .2f), AccA(.9f), 1.4f * gS);
                float tx = p.x + chipH; ImVec4 clip(tx, p.y, p.x + chipW - em * .5f, p.y + chipH);
                dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(tx, p.y + chipH * .5f - em * 1.05f), TextCol(), gRW[i].name.c_str(), nullptr, 0.f, &clip);
                dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(tx, p.y + chipH * .5f + em * .05f), TextCol(.5f), gRW[i].date.c_str(), nullptr, 0.f, &clip);
            }
        }
        StageView(dl, ImVec2(org.x + lw + gap, org.y + sy), ImVec2(org.x + W, org.y + sy + sh));
        PlayTimePill(dl, ImVec2(org.x + W, org.y + sy));
    }
    PlayBar(W, H, rowH, statusH);
}

// ---------- Скины ----------
static float gSkinUserYaw = -0.5f;
static void ScrSkins() {
    float em = ImGui::GetFontSize(); ImDrawList* dl = ImGui::GetWindowDrawList();
    if (Header(T("Скины (BETA)", "Skins (BETA)"))) { Go(S_MAIN); return; }
    ImVec2 org = ImGui::GetCursorScreenPos(); float aw = ImGui::GetContentRegionAvail().x, ah = ImGui::GetContentRegionAvail().y, lw = std::max(em * 16.f, aw * 0.42f);
    DrawStage(dl, org, ImVec2(org.x + lw, org.y + ah), R(em * 1.0f));
    DrawSkinModel(dl, ImVec2(org.x + lw / 2, org.y + ah / 2), (ah - em * 2) * .95f / 33.f, false, gSkinUserYaw);
    ImGui::SetCursorScreenPos(org); ImGui::InvisibleButton("##skinprev", ImVec2(lw, ah));
    if (ImGui::IsItemActive()) gSkinUserYaw += ImGui::GetIO().MouseDelta.x * 0.012f;
    { const char* hint = T("Потяни мышью, чтобы повернуть", "Drag to rotate"); ImVec2 hs = ImGui::CalcTextSize(hint); dl->AddText(ImVec2(org.x + (lw - hs.x) / 2, org.y + ah - em * 1.6f), TextCol(.5f), hint); }
    ImGui::SetCursorScreenPos(ImVec2(org.x + lw + em, org.y));
    ImGui::BeginChild("skinR", ImVec2(0, 0), 0, ImGuiWindowFlags_NoBackground);
    const Account* a = (cfg.sel >= 0 && cfg.sel < (int)cfg.accs.size()) ? &cfg.accs[cfg.sel] : nullptr;
    TD(T("Аккаунт", "Account")); ImGui::TextUnformatted(a ? AccLabel(*a).c_str() : T("не выбран", "not selected"));
    TD(T("Источник скина", "Skin source"));
    if (!cfg.skinPath.empty()) ImGui::TextWrapped("%s %s", T("Свой файл:", "Own file:"), PS(P(cfg.skinPath).filename()).c_str());
    else if (a && a->type != 0) ImGui::TextUnformatted(T("Скин аккаунта", "Account skin"));
    else ImGui::TextUnformatted(T("Стандартный скин", "Default skin"));
    float bw = std::min(em * 22.f, ImGui::GetContentRegionAvail().x), bhh = em * 2.1f;
    ImGui::Dummy(ImVec2(0, em * .2f));
    if (FancyBtn(T("Выбрать скин (PNG)...", "Choose skin (PNG)..."), ImVec2(bw, bhh), gSoft)) {
        std::string f = PickImage(gHwnd);
        if (!f.empty()) { cfg.skinPath = f; SkinPx t; if (LoadSkinFile(f, t)) { SkinFixLegacy(t); cfg.skinSlim = SkinIsSlim(t); } SaveConfig(); }
    }
    if (FancyBtn(T("Выбрать плащ (PNG)...", "Choose cape (PNG)..."), ImVec2(bw, bhh), gSoft)) { std::string f = PickImage(gHwnd); if (!f.empty()) { cfg.capePath = f; SaveConfig(); } }
    if (FancyBtn(T("Сбросить скин и плащ", "Reset skin and cape"), ImVec2(bw, bhh), gSoft, !cfg.skinPath.empty() || !cfg.capePath.empty())) { cfg.skinPath.clear(); cfg.capePath.clear(); SaveConfig(); }
    TD(T("Модель рук", "Arm model"));
    { int m = cfg.skinSlim ? 1 : 0, m0 = m; ImGui::BeginDisabled(cfg.skinPath.empty());
      ImGui::RadioButton(T("Классическая", "Classic"), &m, 0); ImGui::SameLine(); ImGui::RadioButton(T("Тонкая (Alex)", "Slim (Alex)"), &m, 1);
      ImGui::EndDisabled(); if (m != m0) { cfg.skinSlim = m == 1; SaveConfig(); } }
    if (ImGui::Checkbox(T("Показывать плащ", "Show cape"), &cfg.showCape)) SaveConfig();
    if (FancyBtn(T("Помахать", "Wave"), ImVec2(bw, bhh), gSoft)) StartWave();
    if (FancyBtn(T("Обновить с аккаунта", "Refresh from account"), ImVec2(bw, bhh), gSoft, !gSkinLoading)) gSkinReload = true;
    if (a && a->type == 2 && !cfg.skinPath.empty()) {
        if (FancyBtn(T("Применить скин к аккаунту Microsoft", "Apply the skin to the Microsoft account"), ImVec2(bw, bhh), gAccent)) std::thread(MsUploadSkinThread, *a, cfg.skinPath, cfg.skinSlim).detach();
    }
    { std::string m; { std::lock_guard<std::mutex> l(gSkinMu); m = gSkinMsg; } if (!m.empty()) { ImGui::PushTextWrapPos(0.f); ImGui::TextColored(gAccent, "%s", m.c_str()); ImGui::PopTextWrapPos(); } }
    ImGui::PushTextWrapPos(0.f);
    TD(T("Скины Ely.by и Microsoft подгружаются с аккаунта автоматически. Для офлайн-аккаунта свой скин виден только в лаунчере: в игре его покажет мод вроде CustomSkinLoader. Для Microsoft скин можно применить к аккаунту кнопкой выше.",
         "Ely.by and Microsoft skins are loaded from the account automatically. For an offline account your skin is visible only in the launcher: in-game it needs a mod such as CustomSkinLoader. For Microsoft you can apply the skin to the account with the button above."));
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
}

// ---------- Статистика ----------
static void ScrStats() {
    float em = ImGui::GetFontSize(); ImDrawList* dl = ImGui::GetWindowDrawList();
    if (Header(T("Статистика", "Statistics"))) { Go(S_MAIN); return; }
    ScanBuilds(false);
    float aw = ImGui::GetContentRegionAvail().x, sp = em, cw = (aw - 2 * sp) / 3, ch = em * 4.6f; ImVec2 p0 = ImGui::GetCursorScreenPos();
    std::string vals[3] = {FmtDur(cfg.playSec), std::to_string(cfg.launches), std::to_string(gBuilds.size())};
    const char* labs[3] = {T("Время в игре", "Time played"), T("Запусков игры", "Game launches"), T("Сборок", "Builds")};
    for (int i = 0; i < 3; i++) {
        ImVec2 p(p0.x + i * (cw + sp), p0.y); Surface(dl, p, ImVec2(p.x + cw, p.y + ch), R(em * .9f), 1);
        dl->AddText(ImVec2(p.x + em, p.y + em * .8f), TextCol(.6f), labs[i]);
        dl->AddText(gFSub, gFSub->FontSize, ImVec2(p.x + em, p.y + em * 2.1f), ImGui::GetColorU32(gAccent), vals[i].c_str());
    }
    ImGui::Dummy(ImVec2(aw, ch + em * .3f));
    TD(T("По сборкам", "By build"));
    std::vector<Build> bs = gBuilds; std::sort(bs.begin(), bs.end(), [](const Build& a, const Build& b) { return a.playSec > b.playSec; });
    ImGui::BeginChild("statl", ImVec2(0, 0), 0, ImGuiWindowFlags_NoBackground); ImDrawList* cdl = ImGui::GetWindowDrawList();
    if (bs.empty()) TD(T("Пока нет ни одной сборки.", "No builds yet."));
    for (int i = 0; i < (int)bs.size(); i++) {
        float w = ImGui::GetContentRegionAvail().x, rh = em * 2.8f; ImVec2 p = ImGui::GetCursorScreenPos();
        Surface(cdl, p, ImVec2(p.x + w, p.y + rh), R(em * .8f), 1);
        std::string left = bs[i].name + "   " + bs[i].loader + " " + bs[i].ver;
        std::string right = FmtDur(bs[i].playSec) + "   |   " + std::to_string(bs[i].launches) + T(" запусков", " launches");
        ImVec2 rs = ImGui::CalcTextSize(right.c_str()); ImVec4 clip(p.x, p.y, p.x + w - rs.x - em * 2, p.y + rh);
        cdl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(p.x + em, p.y + (rh - em) / 2), TextCol(), left.c_str(), nullptr, 0.f, &clip);
        cdl->AddText(ImVec2(p.x + w - rs.x - em, p.y + (rh - rs.y) / 2), TextCol(.6f), right.c_str());
        ImGui::Dummy(ImVec2(w, rh));
    }
    ImGui::EndChild();
}

// ---------- Новости (лаунчера и игры) ----------
static void ScrNews() {
    float em = ImGui::GetFontSize(), sp = ImGui::GetStyle().ItemSpacing.x;
    if (Header(T("Новости", "News"))) { Go(S_MAIN); return; }
    float sw = em * 12.f, shh = em * 2.f, aw = ImGui::GetContentRegionAvail().x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.f, (aw - (sw * 2 + sp)) / 2));
    if (FancyBtn(T("Новости лаунчера", "Launcher news"), ImVec2(sw, shh), gNewsTab == 0 ? gAccent : gSoft)) gNewsTab = 0;
    ImGui::SameLine();
    if (FancyBtn(T("Новости игры", "Game news"), ImVec2(sw, shh), gNewsTab == 1 ? gAccent : gSoft)) gNewsTab = 1;
    ImGui::BeginChild("newsl", ImVec2(0, 0), 0, ImGuiWindowFlags_NoBackground);
    if (gNewsTab == 0) { for (auto& n : kNews) NewsCard(n.ver, {T(n.ru, n.en)}, false); }
    else { std::vector<std::string> ls; for (auto& g : kGameNews) ls.push_back(T(g.ru, g.en)); NewsCard(T("Minecraft 26.4 Snapshot 9 и 10", "Minecraft 26.4 Snapshots 9 and 10"), ls, true); }
    ImGui::EndChild();
}

// ---------- Создание сборки ----------
static char bName[64]; static int bLd = 0; static std::string bVer, bOf;
static void FixCreateVer() { // оставить выбранную версию, если загрузчик её поддерживает; иначе — новейший релиз из доступных
    std::lock_guard<std::mutex> l(gVerMu); const std::string ld = kLoaders[bLd];
    for (auto& v : gVers) if (v.id == bVer && VerShown(v, ld)) return;
    bVer.clear();
    for (auto& v : gVers) if (v.type == "release" && VerShown(v, ld)) { bVer = v.id; return; }
    for (auto& v : gVers) if (VerShown(v, ld)) { bVer = v.id; return; }
}
static void ScrCreate() {
    float em = ImGui::GetFontSize(); ImGuiIO& io = ImGui::GetIO(); (void)io;
    if (Header(T("Создать сборку", "Create build"))) { Go(S_BUILDS); return; }
    if (!bInit) { bInit = true; bName[0] = 0; bLd = 0; bOf.clear(); bVer.clear(); FixCreateVer(); }
    if (bVer.empty()) FixCreateVer(); // список версий мог загрузиться только что
    TD(T("Название", "Name")); ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##bname", T("Например: Моя сборка (необязательно)", "For example: My build (optional)"), bName, sizeof(bName));
    TD(T("Загрузчик", "Loader"));
    ImGui::SetNextItemWidth(em * 14.f);
    if (ImGui::BeginCombo("##bld", kLoaders[bLd])) {
        for (int i = 0; i < kNLoaders; i++) if (ImGui::Selectable(kLoaders[i], bLd == i) && bLd != i) { bLd = i; bOf.clear(); bVer.clear(); FixCreateVer(); }
        ImGui::EndCombo();
    }
    ImGui::SameLine(); if (ImGui::Checkbox(T("Снапшоты", "Snapshots"), &cfg.snapshots)) FixCreateVer();
    if (std::string(kLoaders[bLd]) == "OptiFine" && !bVer.empty()) {
        TD(T("Сборка OptiFine", "OptiFine build")); ImGui::SetNextItemWidth(em * 22.f);
        if (ImGui::BeginCombo("##cofb", OfBuildCaption(bVer, bOf).c_str())) { OfBuildItems(bVer, bOf); ImGui::EndCombo(); }
    }
    TD(T("Версия игры (только те, на которые загрузчик можно установить)", "Game version (only those the loader supports)"));
    float btnH = em * 2.7f;
    ImGui::BeginChild("##cver", ImVec2(0, std::max(em * 5.f, ImGui::GetContentRegionAvail().y - btnH - em * 1.0f)), ImGuiChildFlags_Border, 0);
    {
        std::lock_guard<std::mutex> l(gVerMu); int n = 0; const std::string ld = kLoaders[bLd];
        for (auto& v : gVers) {
            if (!VerShown(v, ld)) continue; n++;
            std::string lab = v.id + (v.type == "snapshot" ? T("   [снапшот]", "   [snapshot]") : v.type == "old_beta" ? T("   [бета]", "   [beta]") : v.type == "old_alpha" ? T("   [альфа]", "   [alpha]") : "");
            ImGui::PushID(v.id.c_str()); if (ImGui::Selectable(lab.c_str(), v.id == bVer)) { if (bVer != v.id) bOf.clear(); bVer = v.id; } ImGui::PopID();
        }
        if (n == 0) {
            if (gVers.empty() || (ld != "Vanilla" && gLoadersLoading)) TD(T("Загружаю списки версий...", "Loading version lists..."));
            else TD(T("Список версий для этого загрузчика недоступен (проверь интернет / VPN).", "The version list for this loader is unavailable (check internet / VPN)."));
        }
    }
    ImGui::EndChild();
    float bw = em * 14.f; ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.f, (ImGui::GetContentRegionAvail().x - bw) / 2));
    if (FancyBtn(T("Создать", "Create"), ImVec2(bw, btnH), gAccent, !bVer.empty())) {
        Build b; b.loader = kLoaders[bLd]; b.ver = bVer; b.ofBuild = b.loader == "OptiFine" ? bOf : std::string();
        std::string nm = bName; while (!nm.empty() && nm.back() == ' ') nm.pop_back(); size_t f0 = nm.find_first_not_of(' '); nm = f0 == std::string::npos ? std::string() : nm.substr(f0);
        b.name = nm.empty() ? (b.loader == "Vanilla" ? b.ver : b.loader + " " + b.ver) : nm;
        b.id = NewBuildId(b.loader, b.ver); b.created = (long long)time(nullptr);
        std::error_code ec; fs::create_directories(BuildDir(b.id), ec); SaveBuild(b);
        ScanBuilds(true); SelectBuild(b.id); Status(std::string(T("Создана сборка: ", "Build created: ")) + b.name); Log("Создана сборка " + b.name + " (" + b.loader + " " + b.ver + "), папка " + b.id);
        Go(S_MAIN);
    }
}
// ---------- Аккаунты ----------
static void ScrAccounts() {
    float em = ImGui::GetFontSize(); ImGuiStyle& sty = ImGui::GetStyle();
    if (Header(T("Аккаунты", "Accounts"))) { Go(S_MAIN); return; }
    float sp = sty.ItemSpacing.x, cw = (ImGui::GetContentRegionAvail().x - 2 * sp) / 3, ch = em * 17.5f, bh = em * 2.2f;
    for (int i = 0; i < 3; i++) {
        if (i) ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_ChildBg, gSoft); ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(em, em));
        ImGui::BeginChild(i == 0 ? "c_ely" : i == 1 ? "c_off" : "c_ms", ImVec2(cw, ch), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleVar(); ImGui::PopStyleColor();
        auto Title = [&](const char* s) { ImGui::PushFont(gFSub); ImGui::TextColored(gAccent, "%s", s); ImGui::PopFont(); ImGui::Dummy(ImVec2(0, em * .1f)); };
        auto Bul = [&](const char* s) { ImGui::PushTextWrapPos(0.f); ImGui::BulletText("%s", s); ImGui::PopTextWrapPos(); };
        float by = ch - em - bh;
        if (i == 0) { // Ely.by
            Title("Ely.by");
            Bul(T("Скины поддерживаются", "Skins supported")); Bul(T("Для пиратской версии", "For non-licensed copies")); Bul(T("Требуется интернет", "Internet required"));
            ImGui::Dummy(ImVec2(0, em * .2f));
            ImGui::SetNextItemWidth(-1); ImGui::InputTextWithHint("##eu", T("Логин или e-mail", "Login or e-mail"), bElyU, sizeof(bElyU));
            ImGui::SetNextItemWidth(-1); ImGui::InputTextWithHint("##ep", T("Пароль (с 2FA: пароль:код)", "Password (with 2FA: password:code)"), bElyP, sizeof(bElyP), ImGuiInputTextFlags_Password);
            ImGui::SetCursorPosY(by);
            if (FancyBtn(T("Войти", "Sign in"), ImVec2(0, bh), gAccent, !gAuthBusy) && bElyU[0]) std::thread(ElyLoginThread, std::string(bElyU), std::string(bElyP)).detach();
        } else if (i == 1) { // офлайн
            Title(T("Офлайн", "Offline"));
            Bul(T("Нет поддержки скинов", "No skin support")); Bul(T("Пиратка / лицензия", "Non-licensed / licensed")); Bul(T("Интернет не требуется", "No internet required"));
            ImGui::Dummy(ImVec2(0, em * .2f));
            ImGui::SetNextItemWidth(-1); ImGui::InputText("##nick", bNick, sizeof(bNick));
            ImGui::PushTextWrapPos(0.f); ImGui::TextDisabled("%s", T(kNickHint, "Nick: 3-16 chars, Latin letters, digits and _")); ImGui::PopTextWrapPos();
            ImGui::SetCursorPosY(by);
            if (FancyBtn(T("Войти", "Sign in"), ImVec2(0, bh), gAccent) && AddOffline(bNick)) Go(S_MAIN);
        } else { // Microsoft
            Title("Microsoft");
            Bul(T("Есть поддержка скинов", "Skins supported")); Bul(T("Лицензия", "Licensed copy")); Bul(T("Требуется интернет", "Internet required"));
            ImGui::Dummy(ImVec2(0, em * .2f));
            if (bMsId[0] == 0) { std::string id = MsId(); snprintf(bMsId, sizeof(bMsId), "%s", id.c_str()); }
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputTextWithHint("##msid", "Azure Client ID (portal.azure.com)", bMsId, sizeof(bMsId))) SetMsId(bMsId);
            std::string code, uri; { std::lock_guard<std::mutex> l(gMu); code = gMsCode; uri = gMsUri; }
            if (!code.empty()) {
                ImGui::PushTextWrapPos(0.f); ImGui::TextWrapped(T("Открой %s и введи код:", "Open %s and enter the code:"), uri.c_str()); ImGui::PopTextWrapPos();
                ImGui::PushFont(gFTitle); ImGui::TextColored(gAccent, "%s", code.c_str()); ImGui::PopFont();
                ImGui::SameLine(); if (ImGui::SmallButton(T("Копировать", "Copy"))) ImGui::SetClipboardText(code.c_str());
            }
            ImGui::SetCursorPosY(by);
            if (FancyBtn(T("Войти", "Sign in"), ImVec2(0, bh), gAccent, !gAuthBusy)) { SetMsId(bMsId); SaveConfig(); std::thread(MsLoginThread).detach(); }
        }
        ImGui::EndChild();
    }
    TD(T("Сохранённые аккаунты", "Saved accounts"));
    float listH = std::max(em * 3.f, ImGui::GetContentRegionAvail().y - em * 2.6f); int del = -1;
    ImGui::BeginChild("accl", ImVec2(0, listH), 0, ImGuiWindowFlags_NoBackground);
    for (int i = 0; i < (int)cfg.accs.size(); i++) {
        ImGui::PushID(i);
        if (ImGui::Selectable(AccLabel(cfg.accs[i]).c_str(), cfg.sel == i, 0, ImVec2(ImGui::GetContentRegionAvail().x - em * 2.2f, 0))) cfg.sel = i;
        ImGui::SameLine(); if (ImGui::SmallButton("x")) del = i;
        ImGui::PopID();
    }
    if (cfg.accs.empty()) TD(T("Пока нет ни одного аккаунта", "No accounts yet"));
    ImGui::EndChild();
    if (del >= 0) { if (cfg.lastNick == cfg.accs[del].name) cfg.lastNick.clear(); cfg.accs.erase(cfg.accs.begin() + del); if (cfg.accs.empty()) cfg.sel = -1; else if (cfg.sel == del) cfg.sel = 0; else if (cfg.sel > del) cfg.sel--; SaveConfig(); }
    std::string st; { std::lock_guard<std::mutex> l(gMu); st = gAuthMsg; }
    if (!st.empty()) ImGui::TextWrapped("%s", st.c_str());
}

// ---------- Моды ----------
static void ModsBody() {
    float em = ImGui::GetFontSize(); ImDrawList* dl = ImGui::GetWindowDrawList();
    if (cfg.buildId.empty()) { TD(T("Сначала создай или выбери сборку.", "Create or select a build first.")); return; }
    std::string ld = LoaderId(cfg.loader);
    std::string key = gWantQ + "|" + ld + "|" + cfg.version;
    if (!ld.empty() && !cfg.version.empty() && key != gModKey && !gModBusy) { gModKey = key; std::thread(ModSearchThread, gWantQ, ld, cfg.version).detach(); }
    fs::path idir = CurrentInstanceDir(); ModsSync(idir);
    float total = ImGui::GetContentRegionAvail().x, lw = total * 0.34f;
    ImGui::BeginChild("modsL", ImVec2(lw, 0), 0, ImGuiWindowFlags_NoBackground);
    TD(T("Сайт для загрузки модов", "Mod download site")); ImGui::TextUnformatted("Modrinth");
    TD(T("Сборка", "Build")); { const Build* cb = FindBuild(cfg.buildId); ImGui::TextWrapped("%s", cb ? cb->name.c_str() : ""); }
    TD(T("Установка на", "Installing for")); ImGui::Text("%s  %s", cfg.loader.c_str(), cfg.version.c_str());
    ImGui::SetNextItemWidth(-1);
    bool go = ImGui::InputTextWithHint("##mq", T("Поиск...", "Search..."), bModQ, sizeof(bModQ), ImGuiInputTextFlags_EnterReturnsTrue);
    if (FancyBtn(T("Найти", "Search"), ImVec2(0, em * 2.f), gSoft)) go = true;
    if (go) { gWantQ = bModQ; gModKey.clear(); } // повторный поиск даже с тем же запросом
    std::string msg; { std::lock_guard<std::mutex> l(gModMu); msg = gModMsg; }
    if (ld.empty()) msg = T("Для модов выбери загрузчик Fabric, Forge, NeoForge или Quilt на главном экране.", "Pick the Fabric, Forge, NeoForge or Quilt loader on the main screen to use mods.");
    if (!msg.empty()) { ImGui::PushTextWrapPos(0.f); ImGui::TextColored(gAccent, "%s", msg.c_str()); ImGui::PopTextWrapPos(); }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("modsR", ImVec2(0, 0), 0, ImGuiWindowFlags_NoBackground);
    dl = ImGui::GetWindowDrawList();
    if (!ld.empty()) {
        std::vector<ModHit> hits; std::set<std::string> have;
        { std::lock_guard<std::mutex> l(gModMu); hits = gModHits; for (auto& kv : gModFiles) have.insert(kv.first); }
        if (gModBusy && hits.empty()) TD(T("Загрузка...", "Loading..."));
        bool busyI = gModInst;
        for (int i = 0; i < (int)hits.size(); i++) {
            auto& h = hits[i]; ImGui::PushID(i);
            bool d = have.count(h.id) > 0; // мод уже скачан в этот экземпляр
            ImVec2 p = ImGui::GetCursorScreenPos(); float w = ImGui::GetContentRegionAvail().x, bw = em * 8.5f, dw = em * 6.f, gp = em * .5f, btns = bw + (d ? dw + gp : 0.f), tw = std::max(em * 6.f, w - btns - em * 2.4f);
            ImVec2 ds = ImGui::CalcTextSize(h.desc.c_str(), nullptr, false, tw); float dh = std::min(ds.y, em * 2.4f), rh = em * .7f + em * 1.3f + dh + em * .7f;
            dl->AddRectFilled(p, ImVec2(p.x + w, p.y + rh), ImGui::GetColorU32(gSoft), R(em * .6f));
            dl->AddText(ImVec2(p.x + em, p.y + em * .7f), TextCol(), h.title.c_str());
            ImVec4 clip(p.x, p.y, p.x + em + tw, p.y + rh - em * .4f);
            dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(p.x + em, p.y + em * .7f + em * 1.3f), TextCol(.65f), h.desc.c_str(), nullptr, tw, &clip);
            float by = p.y + (rh - em * 2.f) / 2, bx = p.x + w - em * .8f - btns;
            ImGui::SetCursorScreenPos(ImVec2(bx, by));
            if (FancyBtn(d ? T("Установлено", "Installed") : T("Установить", "Install"), ImVec2(bw, em * 2.f), d ? gSoft : gAccent, !busyI && !d)) std::thread(ModInstallThread, h.id, h.title, ld, cfg.version, PS(idir)).detach();
            if (d) {
                ImGui::SetCursorScreenPos(ImVec2(bx + bw + gp, by));
                if (FancyBtn(T("Удалить", "Delete"), ImVec2(dw, em * 2.f), ImVec4(.90f, .30f, .30f, .55f), !busyI)) { ModDelete(idir, h.id); ModMsg(std::string(T("Удалено: ", "Removed: ")) + h.title); }
            }
            ImGui::SetCursorScreenPos(p); ImGui::Dummy(ImVec2(w, rh));
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
}

static void ScrLog() {
    float em = ImGui::GetFontSize();
    if (Header(T("Лог", "Log"))) { Go(S_MAIN); return; }
    if (FancyBtn(T("Открыть latest.log", "Open latest.log"), ImVec2(em * 14, em * 1.9f), gSoft)) ShellExecuteW(0, L"open", (gCfgDir / "logs" / "latest.log").wstring().c_str(), 0, 0, SW_SHOWNORMAL);
    ImGui::SameLine(); if (FancyBtn(T("Очистить окно", "Clear view"), ImVec2(em * 11, em * 1.9f), gSoft)) { std::lock_guard<std::mutex> l(gMu); gLog.clear(); }
    ImGui::BeginChild("log", ImVec2(0, 0), false);
    { std::lock_guard<std::mutex> l(gMu);
      ImGuiListClipper cl; cl.Begin((int)gLog.size());
      while (cl.Step()) for (int i = cl.DisplayStart; i < cl.DisplayEnd; i++) ImGui::TextUnformatted(gLog[i].c_str()); }
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4) ImGui::SetScrollHereY(1.f);
    ImGui::EndChild();
}

// ---------- Изменить экземпляр ----------
struct WorldInfo { std::string name, date; };
static std::vector<WorldInfo> gWorlds; static float gWorldsT = 99.f;
static std::map<std::string, long long> gWorldSz; static std::set<std::string> gWorldSzBusy; static std::mutex gWsMu;
static std::atomic<bool> gWorldBusy{false};
static std::string gWorldAsk, gWorldDel; // запрос на удаление мира
static InstCfg gEd; static std::string gEdName; static int gEdTab = 0, gEdNeedJava = 0;
static Build gEdBuild; static char eName[96];
static char eJava[260], eJvm[512], eWrap[512], ePre[512], ePost[512], eEnv[2048];
static std::vector<std::string> gComps; static float gCompsT = 99.f;

static bool ZipDir(const fs::path& dir, const std::string& top, const fs::path& zipPath) { // папка -> zip (внутри лежит папка top)
    std::error_code ec; fs::create_directories(zipPath.parent_path(), ec);
    FILE* f = _wfopen(zipPath.wstring().c_str(), L"wb"); if (!f) return false;
    mz_zip_archive z{};
    if (!mz_zip_writer_init_cfile(&z, f, 0)) { fclose(f); return false; }
    bool ok = true;
    for (auto it = fs::recursive_directory_iterator(dir, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        fs::path rel = fs::relative(it->path(), dir, ec); if (ec) continue;
        if (rel.filename() == L"session.lock") continue; // файл блокировки открытого мира
        std::string name = top + "/" + PS(rel); for (auto& ch : name) if (ch == '\\') ch = '/';
        std::string data = ReadAll(it->path());
        if (!mz_zip_writer_add_mem(&z, name.c_str(), data.data(), data.size(), MZ_DEFAULT_LEVEL)) { ok = false; break; }
    }
    if (ok) ok = mz_zip_writer_finalize_archive(&z) != 0;
    mz_zip_writer_end(&z); fclose(f);
    if (!ok) fs::remove(zipPath, ec);
    return ok;
}
static std::string FmtWriteDate(const fs::path& p) {
    WIN32_FILE_ATTRIBUTE_DATA a{}; if (!GetFileAttributesExW(p.wstring().c_str(), GetFileExInfoStandard, &a)) return "?";
    FILETIME lf; SYSTEMTIME st; if (!FileTimeToLocalFileTime(&a.ftLastWriteTime, &lf) || !FileTimeToSystemTime(&lf, &st)) return "?";
    char t[48]; snprintf(t, sizeof(t), "%02d.%02d.%04d %02d:%02d", (int)st.wDay, (int)st.wMonth, (int)st.wYear, (int)st.wHour, (int)st.wMinute);
    return t;
}
static void WorldSizeThread(std::string key, fs::path dir) {
    std::error_code ec; long long sum = 0;
    for (auto it = fs::recursive_directory_iterator(dir, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (it->is_regular_file(ec)) sum += (long long)it->file_size(ec);
    }
    std::lock_guard<std::mutex> l(gWsMu); gWorldSz[key] = sum; gWorldSzBusy.erase(key);
}
static void ScanWorlds(bool force) {
    gWorldsT += ImGui::GetIO().DeltaTime; if (!force && gWorldsT < 1.5f) return; gWorldsT = 0;
    std::vector<WorldInfo> v; std::error_code ec;
    for (auto& e : fs::directory_iterator(gRoot / "instances" / P(gEdName) / "saves", ec))
        if (e.is_directory(ec)) { WorldInfo w; w.name = U8(e.path().filename().wstring()); w.date = FmtWriteDate(e.path()); v.push_back(w); }
    std::sort(v.begin(), v.end(), [](const WorldInfo& a, const WorldInfo& b) { return a.name < b.name; });
    gWorlds = v;
}
static std::string PickSaveZip(HWND hw, const std::string& defName) {
    wchar_t f[MAX_PATH] = {0}; lstrcpynW(f, W(defName).c_str(), MAX_PATH);
    OPENFILENAMEW o{}; o.lStructSize = sizeof(o); o.hwndOwner = hw; o.lpstrFilter = L"ZIP\0*.zip\0"; o.lpstrFile = f; o.nMaxFile = MAX_PATH; o.lpstrDefExt = L"zip";
    o.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetSaveFileNameW(&o) ? U8(f) : "";
}
static void WorldTask(int kind, std::string inst, std::string world, std::string outPath) { // 0 бэкап, 1 экспорт, 2 удаление
    gWorldBusy = true; std::error_code ec;
    fs::path dir = gRoot / "instances" / P(inst) / "saves" / P(world);
    if (kind == 2) {
        Status(std::string(T("Удаляю мир: ", "Deleting world: ")) + world); fs::remove_all(dir, ec);
        Status(std::string(T("Мир удалён: ", "World deleted: ")) + world);
    } else {
        fs::path z; std::string what;
        if (kind == 0) {
            SYSTEMTIME st; GetLocalTime(&st); char ts[64];
            snprintf(ts, sizeof(ts), "%04d-%02d-%02d_%02d-%02d-%02d", (int)st.wYear, (int)st.wMonth, (int)st.wDay, (int)st.wHour, (int)st.wMinute, (int)st.wSecond);
            z = gRoot / "instances" / P(inst) / "poltergeist_backups" / P(world + "_" + ts + ".zip"); what = T("Резервная копия", "Backup");
        } else { z = P(outPath); what = T("Экспорт мира", "World export"); }
        Status(what + ": " + world + "...");
        bool ok = ZipDir(dir, world, z);
        Status(ok ? what + std::string(T(" готов: ", " done: ")) + PS(z.filename()) : what + T(" не удался", " failed"));
    }
    gWorldBusy = false;
}
static void InstEditorOpen(const std::string& name) {
    gEdName = name; LoadBuild(name, gEdBuild); snprintf(eName, sizeof(eName), "%s", gEdBuild.name.c_str()); gEd = LoadInstCfg(BuildDir(name));
    snprintf(eJava, sizeof(eJava), "%s", gEd.javaPath.c_str()); snprintf(eJvm, sizeof(eJvm), "%s", gEd.jvmArgs.c_str());
    snprintf(eWrap, sizeof(eWrap), "%s", gEd.wrapper.c_str()); snprintf(ePre, sizeof(ePre), "%s", gEd.pre.c_str()); snprintf(ePost, sizeof(ePost), "%s", gEd.post.c_str());
    snprintf(eEnv, sizeof(eEnv), "%s", gEd.env.c_str());
    gEdTab = 0; gWorldsT = 99.f; gCompsT = 99.f; gWorldAsk.clear();
    { std::lock_guard<std::mutex> l(gWsMu); gWorldSz.clear(); }
    InstInfo ii; ii.name = name; ii.loader = gEdBuild.loader; ii.ver = gEdBuild.ver; gEdNeedJava = 0;
    json vj = Parse(ReadAll(gRoot / "versions" / P(ii.ver) / P(ii.ver + ".json")));
    if (vj.is_object() && vj.contains("javaVersion") && vj["javaVersion"].is_object() && vj["javaVersion"].contains("majorVersion") && vj["javaVersion"]["majorVersion"].is_number_integer())
        gEdNeedJava = vj["javaVersion"]["majorVersion"].get<int>();
    RefreshJavas();
}
static void InstEditorBody(bool& close) {
    float em = ImGui::GetFontSize(); InstInfo ii; ii.name = gEdName; ii.loader = gEdBuild.loader; ii.ver = gEdBuild.ver; fs::path dir = BuildDir(gEdName);
    ImGuiIO& io = ImGui::GetIO(); bool ch = false;
    ImGui::PushFont(gFSub); ImGui::TextColored(gAccent, "%s  %s", ii.loader.c_str(), ii.ver.c_str()); ImGui::PopFont();
    const char* labs[7] = {T("Основное", "General"), T("Миры", "Worlds"), T("Память", "Memory"), "Java", "Minecraft", T("Команды", "Commands"), T("Окружение", "Environment")};
    TabBar(labs, 7, gEdTab, false);
    ImGui::BeginChild("##ebody", ImVec2(0, -(em * 2.8f)), 0, ImGuiWindowFlags_NoBackground);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (gEdTab == 1) { // ---- Миры ----
        ScanWorlds(false); bool busyW = gWorldBusy;
        if (FancyBtn(T("Папка saves", "Saves folder"), ImVec2(em * 10, em * 1.9f), gSoft)) Open(dir / "saves");
        ImGui::SameLine(); if (FancyBtn(T("Папка бэкапов", "Backups folder"), ImVec2(em * 11, em * 1.9f), gSoft)) Open(dir / "poltergeist_backups");
        TD(T("Список локальных миров. Бэкап кладётся в папку бэкапов, экспорт — в zip, который выберешь сам.", "Local worlds. A backup goes to the backups folder, export saves a zip where you choose."));
        if (gWorlds.empty()) TD(T("В этом экземпляре пока нет миров.", "No worlds in this instance yet."));
        for (int i = 0; i < (int)gWorlds.size(); i++) {
            const WorldInfo& w = gWorlds[i]; ImGui::PushID(i);
            float aw = ImGui::GetContentRegionAvail().x, rh = em * 3.0f, bh = em * 1.9f, bw = em * 7.f, tw = em * 2.2f, gp = em * .4f; ImVec2 p = ImGui::GetCursorScreenPos();
            float x = p.x + aw - em * .6f - tw, by = p.y + (rh - bh) / 2;
            dl->AddRectFilled(p, ImVec2(p.x + aw, p.y + rh), ImGui::GetColorU32(gSoft), R(em * .6f));
            std::string szs = "...";
            { std::lock_guard<std::mutex> l(gWsMu); auto f = gWorldSz.find(w.name);
              if (f != gWorldSz.end()) szs = FmtSize(f->second); else if (gWorldSzBusy.insert(w.name).second) std::thread(WorldSizeThread, w.name, dir / "saves" / P(w.name)).detach(); }
            float maxw = std::max(em * 4.f, aw - (bw * 2 + tw + gp * 3 + em * 1.6f));
            ImVec4 clip(p.x, p.y, p.x + em + maxw, p.y + rh);
            dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(p.x + em, p.y + em * .45f), TextCol(), w.name.c_str(), nullptr, 0.f, &clip);
            std::string sub = w.date + "  |  " + szs;
            dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(p.x + em, p.y + em * 1.55f), TextCol(.55f), sub.c_str(), nullptr, 0.f, &clip);
            ImGui::SetCursorScreenPos(ImVec2(x - gp * 2 - bw * 2, by));
            if (FancyBtn(T("Бэкап", "Backup"), ImVec2(bw, bh), gSoft, !busyW)) std::thread(WorldTask, 0, gEdName, w.name, std::string()).detach();
            ImGui::SetCursorScreenPos(ImVec2(x - gp - bw, by));
            if (FancyBtn(T("Экспорт", "Export"), ImVec2(bw, bh), gSoft, !busyW)) { std::string out = PickSaveZip(gHwnd, w.name + ".zip"); if (!out.empty()) std::thread(WorldTask, 1, gEdName, w.name, out).detach(); }
            ImGui::SetCursorScreenPos(ImVec2(x, p.y + (rh - tw) / 2));
            if (TrashBtn("del", tw, !busyW && !gGameRunning)) gWorldAsk = w.name;
            ImGui::SetCursorScreenPos(p); ImGui::Dummy(ImVec2(aw, rh));
            ImGui::PopID();
        }
        if (busyW) { std::string st; { std::lock_guard<std::mutex> l(gMu); st = gStatus; } ImGui::TextColored(gAccent, "%s", st.c_str()); }
    } else if (gEdTab == 0) { // ---- Основное ----
        auto Row = [&](const char* k, const std::string& v) { ImGui::TextDisabled("%s", k); ImGui::SameLine(em * 11.f); ImGui::TextWrapped("%s", v.c_str()); };
        TD(T("Название сборки", "Build name")); ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("##ename", eName, sizeof(eName))) { std::string nm = eName; while (!nm.empty() && nm.back() == ' ') nm.pop_back(); if (!nm.empty()) { gEdBuild.name = nm; SaveBuild(gEdBuild); } }
        Row(T("Minecraft", "Minecraft"), ii.ver); Row(T("Загрузчик", "Loader"), ii.loader);
        Row(T("Нужна Java", "Required Java"), gEdNeedJava > 0 ? std::to_string(gEdNeedJava) : std::string("-"));
        Row(T("Папка", "Folder"), PS(dir));
        if (FancyBtn(T("Выбрать для запуска", "Select for launch"), ImVec2(em * 14, em * 1.9f), gAccent)) { SelectBuild(gEdBuild.id); close = true; }
        ImGui::Dummy(ImVec2(0, em * .3f));
        gCompsT += io.DeltaTime;
        if (gCompsT > 1.f) {
            gCompsT = 0; gComps.clear(); std::error_code ec;
            if (fs::exists(gRoot / "versions" / P(ii.ver) / P(ii.ver + ".json"), ec)) gComps.push_back(ii.ver);
            if (ii.loader != "Vanilla") for (auto& id : LoaderProfiles(ii.loader, ii.ver)) gComps.push_back(id);
        }
        TD(T("Установленные компоненты (патч-файлы)", "Installed components (patch files)"));
        if (gComps.empty()) TD(T("Пока ничего не установлено — всё скачается при первом запуске.", "Nothing installed yet — everything downloads on first launch."));
        for (int i = 0; i < (int)gComps.size(); i++) {
            const std::string id = gComps[i]; ImGui::PushID(i);
            float aw = ImGui::GetContentRegionAvail().x, rh = em * 2.6f, tw = em * 2.2f; ImVec2 p = ImGui::GetCursorScreenPos();
            dl->AddRectFilled(p, ImVec2(p.x + aw, p.y + rh), ImGui::GetColorU32(gSoft), R(em * .6f));
            std::string lab = (id == ii.ver ? std::string("Minecraft  ") : ii.loader + "  ") + id;
            dl->AddText(ImVec2(p.x + em, p.y + (rh - em) / 2), TextCol(), lab.c_str());
            ImGui::SetCursorScreenPos(ImVec2(p.x + aw - em * .6f - tw, p.y + (rh - tw) / 2));
            if (TrashBtn("rm", tw, !gBusy && !gGameRunning && !gDelBusy)) { std::error_code ec; fs::remove_all(gRoot / "versions" / P(id), ec); gCompsT = 99.f; }
            ImGui::SetCursorScreenPos(p); ImGui::Dummy(ImVec2(aw, rh));
            ImGui::PopID();
        }
        TD(T("Удалённый компонент скачается заново при следующем запуске.", "A removed component is downloaded again on the next launch."));
    } else if (gEdTab == 2) { // ---- Память ----
        MEMORYSTATUSEX ms{sizeof(ms)}; GlobalMemoryStatusEx(&ms); int maxMb = std::max(2048, (int)(ms.ullTotalPhys / (1024 * 1024) * 3 / 4));
        ch |= ImGui::Checkbox(T("Свои настройки памяти для этого экземпляра", "Override memory for this instance"), &gEd.memOv);
        if (!gEd.memOv) ImGui::TextDisabled(T("Сейчас используется общая настройка: %d МБ (вкладка Java в настройках)", "Global setting in use: %d MB (Java tab in settings)"), cfg.ramMB);
        ImGui::BeginDisabled(!gEd.memOv);
        TD(T("Минимум (Xms)", "Minimum (Xms)")); ImGui::SetNextItemWidth(-1); ch |= ImGui::SliderInt("##emin", &gEd.minMB, 128, maxMb, T("%d МБ", "%d MB"));
        TD(T("Максимум (Xmx)", "Maximum (Xmx)")); ImGui::SetNextItemWidth(-1); ch |= ImGui::SliderInt("##emax", &gEd.maxMB, 256, maxMb, T("%d МБ", "%d MB"));
        gEd.maxMB = std::min(gEd.maxMB, maxMb); gEd.minMB = std::min(gEd.minMB, gEd.maxMB);
        ImGui::EndDisabled();
    } else if (gEdTab == 3) { // ---- Java ----
        ch |= ImGui::Checkbox(T("Свои настройки Java для этого экземпляра", "Override Java for this instance"), &gEd.javaOv);
        ImGui::BeginDisabled(!gEd.javaOv);
        TD("Java"); ImGui::SetNextItemWidth(-1);
        std::string cur = gEd.javaPath.empty() ? std::string(T("Авто (подберу и скачаю сам)", "Auto (pick and download automatically)")) : gEd.javaPath;
        if (ImGui::BeginCombo("##ejava", cur.c_str())) {
            if (ImGui::Selectable(T("Авто (подберу и скачаю сам)", "Auto (pick and download automatically)"), gEd.javaPath.empty())) { gEd.javaPath.clear(); eJava[0] = 0; ch = true; }
            std::lock_guard<std::mutex> l(gJavaMu);
            for (auto& j : gJavas) { std::string t = "Java " + std::to_string(j.major) + "  -  " + j.exe; if (ImGui::Selectable(t.c_str(), gEd.javaPath == j.exe)) { gEd.javaPath = j.exe; snprintf(eJava, sizeof(eJava), "%s", j.exe.c_str()); ch = true; } }
            ImGui::EndCombo();
        }
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputTextWithHint("##ejp", T("...или свой путь к java.exe", "...or your own path to java.exe"), eJava, sizeof(eJava))) { gEd.javaPath = eJava; ch = true; }
        TD(T("Аргументы JVM", "JVM arguments")); ImGui::SetNextItemWidth(-1);
        if (ImGui::InputTextWithHint("##ejvm", "-XX:+UseZGC -Dfile.encoding=UTF-8", eJvm, sizeof(eJvm))) { gEd.jvmArgs = eJvm; ch = true; }
        ch |= ImGui::Checkbox(T("Пропустить проверку совместимости Java", "Skip Java compatibility check"), &gEd.skipCompat);
        ImGui::EndDisabled();
    } else if (gEdTab == 4) { // ---- Minecraft ----
        ch |= ImGui::Checkbox(T("Свои настройки игры для этого экземпляра", "Override game settings for this instance"), &gEd.mcOv);
        ImGui::BeginDisabled(!gEd.mcOv);
        TD(T("Размер окна игры", "Game window size"));
        ImGui::SetNextItemWidth(em * 8); ch |= ImGui::InputInt("##ew", &gEd.width, 0, 0); ImGui::SameLine(); ImGui::TextUnformatted("x"); ImGui::SameLine();
        ImGui::SetNextItemWidth(em * 8); ch |= ImGui::InputInt("##eh", &gEd.height, 0, 0);
        gEd.width = std::clamp(gEd.width, 1, 16384); gEd.height = std::clamp(gEd.height, 1, 16384);
        ch |= ImGui::Checkbox(T("Запускать в полноэкранном режиме", "Start in fullscreen"), &gEd.fullscreen);
        ImGui::EndDisabled();
    } else if (gEdTab == 5) { // ---- Команды ----
        ch |= ImGui::Checkbox(T("Свои команды для этого экземпляра", "Custom commands for this instance"), &gEd.cmdOv);
        ImGui::BeginDisabled(!gEd.cmdOv);
        TD(T("Команда-обёртка (Wrapper) — ставится перед командой запуска Java", "Wrapper command — placed before the Java launch command")); ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("##ewrap", eWrap, sizeof(eWrap))) { gEd.wrapper = eWrap; ch = true; }
        TD(T("Команда перед запуском (игра ждёт её завершения)", "Pre-launch command (the game waits for it)")); ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("##epre", ePre, sizeof(ePre))) { gEd.pre = ePre; ch = true; }
        TD(T("Команда после выхода из игры", "Post-exit command")); ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("##epost", ePost, sizeof(ePost))) { gEd.post = ePost; ch = true; }
        ImGui::EndDisabled();
        ImGui::PushTextWrapPos(0.f);
        TD(T("Переменные: $INST_DIR и $INST_MC_DIR (папка экземпляра), $INST_NAME, $INST_ID, $INST_MC_VER, $INST_LOADER, $INST_JAVA. Они же доступны как переменные окружения (%INST_DIR%). Команды выполняются через cmd.exe; пути с пробелами бери в кавычки.",
              "Variables: $INST_DIR and $INST_MC_DIR (instance folder), $INST_NAME, $INST_ID, $INST_MC_VER, $INST_LOADER, $INST_JAVA. Also available as environment variables (%INST_DIR%). Commands run via cmd.exe; quote paths with spaces."));
        ImGui::PopTextWrapPos();
    } else { // ---- Переменные окружения ----
        ch |= ImGui::Checkbox(T("Свои переменные окружения для этого экземпляра", "Custom environment variables for this instance"), &gEd.envOv);
        ImGui::BeginDisabled(!gEd.envOv);
        if (ImGui::InputTextMultiline("##eenv", eEnv, sizeof(eEnv), ImVec2(-1, em * 10))) { gEd.env = eEnv; ch = true; }
        ImGui::EndDisabled();
        TD(T("По одной переменной в строке: ИМЯ=значение. Строки с # игнорируются.", "One variable per line: NAME=value. Lines starting with # are ignored."));
    }
    ImGui::EndChild();
    if (ch) SaveInstCfg(dir, gEd);
    if (!gWorldAsk.empty()) { gWorldDel = gWorldAsk; gWorldAsk.clear(); ImGui::OpenPopup("###wdel"); }
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x / 2, io.DisplaySize.y / 2), ImGuiCond_Always, ImVec2(.5f, .5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(em * 28, io.DisplaySize.x - em * 2), 0));
    std::string wid = std::string(T("Удалить мир?", "Delete world?")) + "###wdel";
    if (ImGui::BeginPopupModal(wid.c_str(), nullptr, ImGuiWindowFlags_NoResize)) {
        ImGui::PushFont(gFSub); ImGui::TextColored(ImVec4(1.f, .45f, .4f, 1.f), "%s", gWorldDel.c_str()); ImGui::PopFont();
        ImGui::Separator();
        ImGui::TextWrapped("%s", T("Мир будет удалён безвозвратно. Если нужна копия — сначала сделай бэкап.", "The world will be deleted permanently. Make a backup first if you want a copy."));
        ImGui::Dummy(ImVec2(0, em * .5f));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(.85f, .25f, .25f, .75f)); ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(.90f, .30f, .30f, .90f)); ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(.95f, .35f, .35f, 1.f));
        bool yes = ImGui::Button(T("Удалить", "Delete"), ImVec2(em * 8, 0));
        ImGui::PopStyleColor(3); ImGui::SameLine();
        bool no = ImGui::Button(T("Отмена", "Cancel"), ImVec2(em * 8, 0));
        if (yes && !gWorldBusy) std::thread(WorldTask, 2, gEdName, gWorldDel, std::string()).detach();
        if (yes || no) { ImGui::CloseCurrentPopup(); gWorldsT = 99.f; }
        ImGui::EndPopup();
    }
    if (ImGui::Button(T("Закрыть", "Close"), ImVec2(em * 8, 0))) close = true;
}



// ---------- Сборки модов (Modrinth modpack, файл .mrpack) ----------
// «Установить» создаёт новую сборку игры: версия и загрузчик берутся из самой сборки модов, моды и конфиги скачиваются в её папку.
static std::vector<ModHit> gPackHits; static std::string gPackKey = "\x01", gPackQ; static std::atomic<bool> gPackSearching{false}, gPackBusy{false}; static char bPackQ[96];
static void PackSearchThread(std::string q) {
    gPackSearching = true;
    try {
        std::string fac = "[[\"project_type:modpack\"]]";
        std::string url = "https://api.modrinth.com/v2/search?query=" + UrlEnc(q) + "&facets=" + UrlEnc(fac) + "&limit=40&index=" + (q.empty() ? "downloads" : "relevance");
        Resp r = Http(url); json j = Parse(r.body); std::vector<ModHit> out;
        if (r.status == 200 && j.is_object() && j.contains("hits") && j["hits"].is_array())
            for (auto& h : j["hits"]) {
                ModHit m; m.id = JStr(h, "project_id"); m.title = JStr(h, "title"); m.desc = JStr(h, "description"); m.author = JStr(h, "author");
                if (h.contains("downloads") && h["downloads"].is_number()) m.dl = h["downloads"].get<long long>();
                if (!m.id.empty()) out.push_back(m);
            }
        std::lock_guard<std::mutex> l(gModMu); gPackHits = out;
        if (r.status != 200) gModMsg = T("Нет связи с Modrinth (интернет / VPN / фаервол)", "Cannot reach Modrinth (internet / VPN / firewall)");
        else if (out.empty()) gModMsg = T("Ничего не найдено", "Nothing found");
        else gModMsg.clear();
    } catch (std::exception& e) { ModMsg(std::string("Modrinth: ") + e.what()); }
    gPackSearching = false;
}
static bool SafeRel(std::string& rel) { // путь внутри папки сборки: без «..», без абсолютных путей и диска
    std::replace(rel.begin(), rel.end(), '\\', '/');
    if (rel.empty() || rel[0] == '/' || rel.find(':') != std::string::npos) return false;
    for (auto& part : Split(rel, '/')) if (part == "..") return false;
    return true;
}
static void PackInstallThread(std::string pid, std::string title) {
    gPackBusy = true;
    try {
        ModMsg(std::string(T("Ищу версию сборки: ", "Looking for the pack version: ")) + title);
        Resp r = Http("https://api.modrinth.com/v2/project/" + UrlEnc(pid) + "/version");
        json a = Parse(r.body);
        if (r.status != 200 || !a.is_array() || a.empty()) throw std::runtime_error(T("У этой сборки нет доступных версий", "This pack has no available versions"));
        const json* pick = &a[0]; for (auto& v : a) if (JStr(v, "version_type") == "release") { pick = &v; break; }
        std::string url;
        if (pick->is_object() && pick->contains("files") && (*pick)["files"].is_array()) {
            const json* f = nullptr;
            for (auto& x : (*pick)["files"]) { std::string n = JStr(x, "filename"); if (n.size() > 7 && n.compare(n.size() - 7, 7, ".mrpack") == 0) { f = &x; break; } }
            if (!f && !(*pick)["files"].empty()) f = &(*pick)["files"][0];
            if (f) url = JStr(*f, "url");
        }
        if (url.empty()) throw std::runtime_error(T("В сборке нет файла для скачивания", "The pack has no downloadable file"));
        std::error_code ec; fs::path tmp = gCfgDir / "tmp"; fs::create_directories(tmp, ec); fs::path zipP = tmp / "pack.mrpack";
        ModMsg(std::string(T("Скачиваю сборку: ", "Downloading the pack: ")) + title);
        if (!Download(url, zipP, true)) throw std::runtime_error(T("Не удалось скачать файл сборки", "Failed to download the pack file"));
        std::string data = ReadAll(zipP); mz_zip_archive z{};
        if (data.empty() || !mz_zip_reader_init_mem(&z, data.data(), data.size(), 0)) throw std::runtime_error(T("Файл сборки повреждён", "The pack file is damaged"));
        std::string idx; { int k = mz_zip_reader_locate_file(&z, "modrinth.index.json", nullptr, 0); if (k >= 0) { size_t sz = 0; void* p = mz_zip_reader_extract_to_heap(&z, k, &sz, 0); if (p) { idx.assign((const char*)p, sz); mz_free(p); } } }
        json ix = Parse(idx);
        if (!ix.is_object()) { mz_zip_reader_end(&z); throw std::runtime_error(T("В сборке нет modrinth.index.json (не формат Modrinth)", "The pack has no modrinth.index.json (not a Modrinth pack)")); }
        std::string mc, loader = "Vanilla";
        if (ix.contains("dependencies") && ix["dependencies"].is_object()) {
            const json& d = ix["dependencies"]; mc = JStr(d, "minecraft");
            if (d.contains("fabric-loader")) loader = "Fabric"; else if (d.contains("quilt-loader")) loader = "Quilt"; else if (d.contains("neoforge")) loader = "NeoForge"; else if (d.contains("forge")) loader = "Forge";
        }
        if (mc.empty()) { mz_zip_reader_end(&z); throw std::runtime_error(T("В сборке не указана версия Minecraft", "The pack does not specify a Minecraft version")); }
        Build b; b.loader = loader; b.ver = mc; b.name = title; b.id = NewBuildId(loader, mc); b.created = (long long)time(nullptr);
        fs::path inst = BuildDir(b.id); fs::create_directories(inst, ec); SaveBuild(b);
        std::vector<std::pair<std::string, fs::path>> jobs;
        if (ix.contains("files") && ix["files"].is_array()) for (auto& f : ix["files"]) {
            if (!f.is_object()) continue;
            if (f.contains("env") && f["env"].is_object() && JStr(f["env"], "client") == "unsupported") continue; // мод только для сервера
            std::string rel = JStr(f, "path"); if (!SafeRel(rel)) continue;
            std::string u; if (f.contains("downloads") && f["downloads"].is_array()) for (auto& d : f["downloads"]) if (d.is_string()) { u = d.get<std::string>(); break; }
            if (!u.empty()) jobs.push_back({u, inst / P(rel)});
        }
        std::atomic<size_t> next{0}, done{0}, failed{0}; size_t total = jobs.size();
        auto worker = [&] {
            for (;;) {
                size_t i = next++; if (i >= total) break;
                std::error_code e2; fs::create_directories(jobs[i].second.parent_path(), e2);
                if (!Download(jobs[i].first, jobs[i].second)) { failed++; Log("[сборка] не скачался файл: " + jobs[i].first); }
                size_t d = ++done; if (d % 3 == 0 || d == total) { std::lock_guard<std::mutex> l(gModMu); gModMsg = std::string(T("Скачиваю файлы сборки: ", "Downloading pack files: ")) + std::to_string(d) + " / " + std::to_string(total); }
            }
        };
        std::vector<std::thread> th; for (int i = 0; i < 6; i++) th.emplace_back(worker); for (auto& t : th) t.join();
        for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&z); i++) { // overrides (конфиги, ресурспаки и т.п.) копируются поверх
            mz_zip_archive_file_stat st; if (!mz_zip_reader_file_stat(&z, i, &st) || mz_zip_reader_is_file_a_directory(&z, i)) continue;
            std::string n = st.m_filename, rel;
            if (n.rfind("overrides/", 0) == 0) rel = n.substr(10); else if (n.rfind("client-overrides/", 0) == 0) rel = n.substr(17); else continue;
            if (!SafeRel(rel)) continue;
            size_t sz = 0; void* p = mz_zip_reader_extract_to_heap(&z, i, &sz, 0); if (!p) continue;
            fs::path dst = inst / P(rel); std::error_code e3; fs::create_directories(dst.parent_path(), e3); WriteAll(dst, std::string((const char*)p, sz)); mz_free(p);
        }
        mz_zip_reader_end(&z); fs::remove(zipP, ec);
        { std::lock_guard<std::mutex> l(gPackMu); gPackSel = b.id; }
        std::string res = std::string(T("Сборка создана: ", "Pack installed: ")) + title + " (" + loader + " " + mc + T("). Нажми ИГРАТЬ", "). Press PLAY");
        if (failed > 0) res += std::string(T(" Не скачано файлов: ", " Files not downloaded: ")) + std::to_string((size_t)failed) + T(" (подробности в логе)", " (see the log)");
        ModMsg(res);
    } catch (std::exception& e) { ModMsg(std::string(T("Ошибка: ", "Error: ")) + e.what()); }
    gPackBusy = false;
}
static void PacksBody() {
    float em = ImGui::GetFontSize(); ImDrawList* dl = ImGui::GetWindowDrawList();
    if (gPackQ != gPackKey && !gPackSearching) { gPackKey = gPackQ; std::thread(PackSearchThread, gPackQ).detach(); }
    float total = ImGui::GetContentRegionAvail().x, lw = total * 0.34f;
    ImGui::BeginChild("packL", ImVec2(lw, 0), 0, ImGuiWindowFlags_NoBackground);
    TD(T("Сайт", "Site")); ImGui::TextUnformatted("Modrinth");
    ImGui::PushTextWrapPos(0.f);
    TD(T("«Установить» создаёт новую сборку игры с нужной версией, загрузчиком, модами и настройками из сборки модов.", "\"Install\" creates a new game build with the version, loader, mods and settings of the modpack."));
    ImGui::PopTextWrapPos();
    ImGui::SetNextItemWidth(-1);
    bool go = ImGui::InputTextWithHint("##pq", T("Поиск сборок...", "Search modpacks..."), bPackQ, sizeof(bPackQ), ImGuiInputTextFlags_EnterReturnsTrue);
    if (FancyBtn(T("Найти", "Search"), ImVec2(0, em * 2.f), gSoft)) go = true;
    if (go) { gPackQ = bPackQ; gPackKey.clear(); }
    std::string msg; { std::lock_guard<std::mutex> l(gModMu); msg = gModMsg; }
    if (!msg.empty()) { ImGui::PushTextWrapPos(0.f); ImGui::TextColored(gAccent, "%s", msg.c_str()); ImGui::PopTextWrapPos(); }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("packR", ImVec2(0, 0), 0, ImGuiWindowFlags_NoBackground);
    dl = ImGui::GetWindowDrawList();
    std::vector<ModHit> hits; { std::lock_guard<std::mutex> l(gModMu); hits = gPackHits; }
    if (gPackSearching && hits.empty()) TD(T("Загрузка...", "Loading..."));
    bool busyI = gPackBusy;
    for (int i = 0; i < (int)hits.size(); i++) {
        auto& h = hits[i]; ImGui::PushID(i);
        ImVec2 p = ImGui::GetCursorScreenPos(); float w = ImGui::GetContentRegionAvail().x, bw = em * 8.5f, tw = std::max(em * 6.f, w - bw - em * 2.6f);
        ImVec2 ds = ImGui::CalcTextSize(h.desc.c_str(), nullptr, false, tw); float dh = std::min(ds.y, em * 2.4f), rh = em * .7f + em * 1.3f + dh + em * .7f;
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + rh), ImGui::GetColorU32(gSoft), R(em * .6f));
        dl->AddText(ImVec2(p.x + em, p.y + em * .7f), TextCol(), h.title.c_str());
        ImVec4 clip(p.x, p.y, p.x + em + tw, p.y + rh - em * .4f);
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(p.x + em, p.y + em * .7f + em * 1.3f), TextCol(.65f), h.desc.c_str(), nullptr, tw, &clip);
        ImGui::SetCursorScreenPos(ImVec2(p.x + w - em * .8f - bw, p.y + (rh - em * 2.f) / 2));
        if (FancyBtn(busyI ? T("Устанавливаю...", "Installing...") : T("Установить", "Install"), ImVec2(bw, em * 2.f), gAccent, !busyI)) std::thread(PackInstallThread, h.id, h.title).detach();
        ImGui::SetCursorScreenPos(p); ImGui::Dummy(ImVec2(w, rh));
        ImGui::PopID();
    }
    ImGui::EndChild();
}
static int gModsTab = 0;
static void ScrMods() { // «Моды и сборки»: моды для выбранной сборки и сборки модов (modpack)
    if (Header(T("Моды и сборки", "Mods & modpacks"))) { Go(S_MAIN); return; }
    const char* labs[2] = {T("Моды", "Mods"), T("Сборки модов", "Modpacks")};
    TabBar(labs, 2, gModsTab);
    if (gModsTab == 1) { PacksBody(); return; }
    if (cfg.modsOn) ModsBody(); else TD(T("Моды отключены в настройках (Общие).", "Mods are turned off in the settings (General)."));
}

// ---------- Сборки (игры): список + «Создать» ----------
static void ScrBuilds() {
    float em = ImGui::GetFontSize(); ImGuiStyle& sty = ImGui::GetStyle(); ImGuiIO& io = ImGui::GetIO();
    if (Header(T("Сборки", "Builds"))) { Go(S_MAIN); return; }
    ScanBuilds(false);
    static std::string iSel; static int iModal = 0;
    float btnH = em * 2.7f;
    ImGui::BeginChild("bl", ImVec2(0, std::max(em * 4.f, ImGui::GetContentRegionAvail().y - btnH - em * .8f)), 0, ImGuiWindowFlags_NoBackground);
    ImDrawList* cdl = ImGui::GetWindowDrawList();
    TD(T("Нажми на сборку, чтобы выбрать её. Правая кнопка мыши: изменить, информация, удалить.", "Click a build to select it. Right-click: edit, information, delete."));
    if (gBuilds.empty()) TD(T("Пока нет ни одной сборки. Нажми «Создать».", "No builds yet. Press \"Create\"."));
    float aw = ImGui::GetContentRegionAvail().x, sp = sty.ItemSpacing.x; int cols = aw > em * 42.f ? 2 : 1; float cw = (aw - sp * (cols - 1)) / cols, rh = em * 4.4f;
    for (int i = 0; i < (int)gBuilds.size(); i++) {
        const Build b = gBuilds[i]; if (i % cols) ImGui::SameLine(0, sp);
        ImGui::PushID(i); ImVec2 p = ImGui::GetCursorScreenPos();
        bool clk = ImGui::InvisibleButton("##b", ImVec2(cw, rh)), hov = ImGui::IsItemHovered(), sel = b.id == cfg.buildId;
        float t = AnimK("h", hov ? 1.f : 0.f, 14.f);
        Surface(cdl, p, ImVec2(p.x + cw, p.y + rh), R(em * .9f), 1, t * .5f);
        if (sel) cdl->AddRect(p, ImVec2(p.x + cw, p.y + rh), ImGui::GetColorU32(gAccent), R(em * .7f), 0, 2.f * gS);
        ImVec4 clip(p.x, p.y, p.x + cw - em * 6.5f, p.y + rh);
        cdl->AddText(gFSub, gFSub->FontSize, ImVec2(p.x + em, p.y + em * .65f), TextCol(), b.name.c_str(), nullptr, 0.f, &clip);
        std::string sub = b.loader + "  " + b.ver + (b.loader == "OptiFine" && !b.ofBuild.empty() ? "  (" + b.ofBuild + ")" : std::string());
        cdl->AddText(ImVec2(p.x + em, p.y + em * 2.7f), TextCol(.6f), sub.c_str());
        if (sel) { const char* s = T("Выбрана", "Selected"); ImVec2 ss = ImGui::CalcTextSize(s); cdl->AddText(ImVec2(p.x + cw - ss.x - em, p.y + em * .8f), ImGui::GetColorU32(gAccent), s); }
        { std::string pt = FmtDur(b.playSec); ImVec2 ps = ImGui::CalcTextSize(pt.c_str()); cdl->AddText(ImVec2(p.x + cw - ps.x - em, p.y + em * 2.7f), TextCol(.45f), pt.c_str()); }
        if (clk) SelectBuild(b.id);
        if (ImGui::BeginPopupContextItem("##bctx")) {
            if (ImGui::Selectable(T("Выбрать", "Select"))) SelectBuild(b.id);
            if (ImGui::Selectable(T("Изменить", "Edit"))) { iSel = b.id; iModal = 2; }
            if (ImGui::Selectable(T("Информация", "Information"))) { iSel = b.id; iModal = 1; }
            if (ImGui::Selectable(T("Открыть папку", "Open folder"))) Open(BuildDir(b.id));
            ImGui::Separator();
            if (ImGui::Selectable(T("Удалить", "Delete"), false, gBusy || gGameRunning || gDelBusy ? ImGuiSelectableFlags_Disabled : 0)) gDelAsk = DelReq{b};
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
    { float bw = em * 14.f; ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.f, (ImGui::GetContentRegionAvail().x - bw) / 2)); if (FancyBtn(T("Создать", "Create"), ImVec2(bw, btnH), gAccent)) Go(S_CREATE); }
    if (iModal == 1) {
        { std::lock_guard<std::mutex> l(gSzMu); gSizes.erase(iSel); if (gSzBusy.insert(iSel).second) std::thread(SizeThread, iSel).detach(); }
        ImGui::OpenPopup("###iinfo"); iModal = 0;
    } else if (iModal == 2) { InstEditorOpen(iSel); ImGui::OpenPopup("###iset"); iModal = 0; }
    auto Center = [&] { ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x / 2, io.DisplaySize.y / 2), ImGuiCond_Always, ImVec2(.5f, .5f)); ImGui::SetNextWindowSize(ImVec2(std::min(em * 30, io.DisplaySize.x - em * 2), 0)); };
    Center();
    std::string t1 = std::string(T("Информация о сборке", "Build information")) + "###iinfo";
    if (ImGui::BeginPopupModal(t1.c_str(), nullptr, ImGuiWindowFlags_NoResize)) {
        Build ib; LoadBuild(iSel, ib); fs::path dir = BuildDir(iSel);
        auto Row = [&](const char* k, const std::string& v) { ImGui::TextDisabled("%s", k); ImGui::SameLine(em * 9.f); ImGui::TextWrapped("%s", v.c_str()); };
        Row(T("Название", "Name"), ib.name); Row(T("Версия игры", "Game version"), ib.ver); Row(T("Загрузчик", "Loader"), ib.loader);
        Row(T("Создана", "Created"), FmtDate(dir));
        std::string sz = "..."; { std::lock_guard<std::mutex> l(gSzMu); auto f = gSizes.find(iSel); if (f != gSizes.end()) sz = FmtSize(f->second); }
        Row(T("Размер", "Size"), sz); Row(T("Время в игре", "Time played"), FmtDur(ib.playSec)); Row(T("Запусков", "Launches"), std::to_string(ib.launches));
        Row(T("Папка", "Folder"), PS(dir));
        ImGui::Dummy(ImVec2(0, em * .5f));
        if (ImGui::Button(T("Закрыть", "Close"), ImVec2(em * 8, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    Center();
    ImGui::SetNextWindowSize(ImVec2(std::min(em * 54, io.DisplaySize.x - em * 2), std::min(em * 36, io.DisplaySize.y - em * 2)));
    std::string t2 = std::string(T("Изменить сборку", "Edit build")) + "###iset";
    if (ImGui::BeginPopupModal(t2.c_str(), nullptr, ImGuiWindowFlags_NoResize)) {
        bool cl = false; InstEditorBody(cl);
        if (cl) { ImGui::CloseCurrentPopup(); gBuildsT = 99.f; }
        ImGui::EndPopup();
    }
}

// ---------- Настройки ----------
static void ThemeCard(int i, float cw, float ph) {
    float em = ImGui::GetFontSize(); const Theme& th = kThemes[i]; ImDrawList* dl = ImGui::GetWindowDrawList(); ImGui::PushID(i);
    float ch = ph + em * 3.6f; ImVec2 p = ImGui::GetCursorScreenPos();
    bool clk = ImGui::InvisibleButton("##th", ImVec2(cw, ch)), sel = cfg.theme == i;
    float t = AnimK("h", ImGui::IsItemHovered() ? 1.f : 0.f, 14.f);
    dl->AddRectFilled(p, ImVec2(p.x + cw, p.y + ch), ImGui::GetColorU32(ImVec4(gSoft.x, gSoft.y, gSoft.z, std::min(1.f, gSoft.w + .08f * t))), R(em * .8f));
    dl->AddRect(p, ImVec2(p.x + cw, p.y + ch), sel ? ImGui::GetColorU32(gAccent) : ImGui::GetColorU32(ImVec4(gSoft.x, gSoft.y, gSoft.z, .25f)), R(em * .8f), 0, sel ? 2.f * gS : 1.f);
    ImVec2 a(p.x + em * .7f, p.y + em * .7f), b(p.x + cw - em * .7f, p.y + em * .7f + ph - em * 1.0f); // миниатюра интерфейса темы
    auto U = [](const ImVec4& c) { return ImGui::ColorConvertFloat4ToU32(c); };
    dl->AddRectFilled(a, b, U(th.pvBg), R(em * .6f));
    dl->AddRectFilled(ImVec2(a.x + em * .4f, a.y + em * .4f), ImVec2(a.x + em * 1.1f, b.y - em * .4f), U(th.pvSide), R(em * .3f));
    dl->AddRectFilled(ImVec2(a.x + em * 1.6f, a.y + em * .45f), ImVec2(b.x - em * .5f, a.y + em * 1.05f), U(th.pvBar), R(em * .3f));
    ImVec2 q0(a.x + em * 1.6f, a.y + em * 1.5f), q1(a.x + em * 1.6f + std::min(em * 6.f, (b.x - a.x) * .5f), a.y + em * 2.9f);
    dl->AddRectFilled(q0, q1, U(th.pvBtn), R(em * .5f)); dl->AddRect(q0, q1, U(ImVec4(th.pvBar.x, th.pvBar.y, th.pvBar.z, .55f)), R(em * .5f));
    dl->AddText(ImVec2(p.x + em * .8f, p.y + ph + em * .55f), TextCol(), T(th.ru, th.en));
    dl->AddText(ImVec2(p.x + em * .8f, p.y + ph + em * 1.75f), TextCol(.55f), T(th.dru, th.den));
    ImGui::PopID();
    if (clk) { cfg.theme = i; SaveConfig(); }
}
static void ScrSettings() {
    float em = ImGui::GetFontSize(); ImDrawList* dl = ImGui::GetWindowDrawList();
    if (Header(T("Настройки", "Settings"))) { Go(S_MAIN); return; }
    const char* labs[5] = {T("Общие", "General"), T("Кастомизация", "Customization"), T("Компоненты", "Components"), "Java", T("О приложении", "About")};
    TabBar(labs, 5, gSetTab);
    ImGui::BeginChild("setc", ImVec2(0, 0), 0, ImGuiWindowFlags_NoBackground);
    dl = ImGui::GetWindowDrawList();
    if (gSetTab == 0) { // ---- Общие ----
        TD(T("Папка игры", "Game folder"));
        { std::string gd = PS(gRoot); ImGui::TextWrapped("%s", gd.c_str()); }
        if (FancyBtn(T("Выбрать папку игры...", "Choose game folder..."), ImVec2(em * 14, em * 1.9f), gSoft, !gBusy)) { std::string f = PickFolder(gHwnd); if (!f.empty()) { SetGameDir(f); ScanShots(); } }
        if (!cfg.gameDir.empty()) { ImGui::SameLine(); if (FancyBtn(T("Вернуть стандартную", "Reset to default"), ImVec2(em * 12, em * 1.9f), gSoft, !gBusy)) { SetGameDir(""); ScanShots(); } }
        ImGui::Checkbox(T("Моды", "Mods"), &cfg.modsOn);
        if (ImGui::Checkbox(T("Логи", "Logs"), &cfg.logsOn)) gLogOn = cfg.logsOn;
        TD(T("Видеокарта", "Graphics card"));
        { const char* it[] = {T("Авто (решает Windows)", "Auto (Windows decides)"), T("Использовать дискретную", "Use discrete GPU"), T("Использовать встроенную", "Use integrated GPU")};
          int g = cfg.gpu == 2 ? 1 : cfg.gpu == 1 ? 2 : 0; if (ComboI("##gpu", g, it, 3)) cfg.gpu = g == 1 ? 2 : g == 2 ? 1 : 0; } // 0 авто, 1 встроенная, 2 дискретная
        if (FancyBtn(T("Проверка подключения к Modrinth", "Check Modrinth connection"), ImVec2(em * 19, em * 1.9f), gSoft, !gModChkBusy)) std::thread(ModrinthCheckThread).detach();
        { std::string chk; { std::lock_guard<std::mutex> l(gModMu); chk = gModChk; } if (!chk.empty()) ImGui::TextColored(gAccent, "%s", chk.c_str()); }
        if (FancyBtn(T("Посмотреть лог", "View log"), ImVec2(em * 12, em * 1.9f), gSoft)) Go(S_LOG);
        ImGui::SameLine(); if (FancyBtn(T("Открыть latest.log", "Open latest.log"), ImVec2(em * 14, em * 1.9f), gSoft)) ShellExecuteW(0, L"open", (gCfgDir / "logs" / "latest.log").wstring().c_str(), 0, 0, SW_SHOWNORMAL);
        TD(T("Язык лаунчера", "Launcher language"));
        { const char* it[] = {"Русский", "English"}; ComboI("##lang", cfg.lang, it, 2); }
        TD(T("После запуска игры", "After the game starts"));
        ImGui::RadioButton(T("Оставить лаунчер как есть", "Leave the launcher as is"), &cfg.onLaunch, 0);
        ImGui::RadioButton(T("Свернуть лаунчер в фон (значок у часов)", "Hide the launcher to background (tray icon)"), &cfg.onLaunch, 1);
        ImGui::RadioButton(T("Закрыть лаунчер", "Close the launcher"), &cfg.onLaunch, 2);
        TD(T("Показывать в списке версий", "Show in the version list"));
        ImGui::Checkbox(T("Снапшоты", "Snapshots"), &cfg.snapshots);
        ImGui::Checkbox(T("Бета-версии (old_beta)", "Beta versions (old_beta)"), &cfg.showBeta);
        ImGui::Checkbox(T("Альфа-версии (old_alpha)", "Alpha versions (old_alpha)"), &cfg.showAlpha);

    } else if (gSetTab == 1) { // ---- Кастомизация: стиль, главный экран, фон за персонажем, тема, фон ----
        float aw = ImGui::GetContentRegionAvail().x, sp = ImGui::GetStyle().ItemSpacing.x, cw2 = (aw - sp) / 2, pvh = em * 4.6f;
        TD(T("СТИЛЬ ОФОРМЛЕНИЯ", "INTERFACE STYLE"));
        for (int i = 0; i < 2; i++) {
            if (i) ImGui::SameLine(0, sp);
            auto pv = [&](ImDrawList* d, ImVec2 a, ImVec2 b) {
                float rr = R(em * .7f), w = b.x - a.x, h = b.y - a.y;
                if (i == 0) { RoundedVGrad(d, a, b, rr, ImVec4(.17f, .04f, .08f, 1.f), ImVec4(.05f, .01f, .03f, 1.f));
                    d->AddRectFilled(ImVec2(a.x + w * .08f, a.y + h * .18f), ImVec2(b.x - w * .08f, a.y + h * .38f), WA(.07f), R(em * .4f));
                    d->AddRectFilled(ImVec2(a.x + w * .08f, b.y - h * .42f), ImVec2(a.x + w * .38f, b.y - h * .16f), U32(Mix(gAccent, ImVec4(0, 0, 0, 1), .35f)), R(em * .45f));
                } else { RoundedVGrad(d, a, b, rr, ImVec4(.42f, .14f, .30f, 1.f), ImVec4(.20f, .06f, .15f, 1.f));
                    d->AddRectFilled(ImVec2(a.x + w * .08f, a.y + h * .18f), ImVec2(b.x - w * .08f, a.y + h * .38f), WA(.22f), R(em * .45f)); d->AddRect(ImVec2(a.x + w * .08f, a.y + h * .18f), ImVec2(b.x - w * .08f, a.y + h * .38f), WA(.45f), R(em * .45f));
                    d->AddRectFilled(ImVec2(a.x + w * .08f, b.y - h * .42f), ImVec2(a.x + w * .38f, b.y - h * .16f), WA(.22f), R(em * .45f)); d->AddRect(ImVec2(a.x + w * .08f, b.y - h * .42f), ImVec2(a.x + w * .38f, b.y - h * .16f), WA(.45f), R(em * .45f)); }
            };
            if (OptionCard(i == 0 ? "st0" : "st1", cw2, pvh, cfg.style == i, i == 0 ? T("Классический", "Classic") : T("Жидкое стекло", "Liquid glass"),
                           i == 0 ? T("Тёмный винный стиль, как раньше.", "Dark wine style, as before.") : T("Прозрачное стекло с бликами, как в iOS.", "Translucent glass with highlights, like iOS."), pv)) { cfg.style = i; SaveConfig(); }
        }
        ImGui::Dummy(ImVec2(0, em * .2f));
        TD(T("ГЛАВНЫЙ ЭКРАН", "HOME SCREEN"));
        for (int i = 0; i < 2; i++) {
            if (i) ImGui::SameLine(0, sp);
            auto pv = [&](ImDrawList* d, ImVec2 a, ImVec2 b) {
                float rr = R(em * .7f), w = b.x - a.x, h = b.y - a.y; RoundedVGrad(d, a, b, rr, ImVec4(.13f, .12f, .15f, 1.f), ImVec4(.07f, .07f, .09f, 1.f));
                if (i == 0) { for (int r = 0; r < 2; r++) for (int c = 0; c < 4; c++) { float x = a.x + w * .06f + c * w * .225f, y = a.y + h * .30f + r * h * .33f; d->AddRectFilled(ImVec2(x, y), ImVec2(x + w * .195f, y + h * .27f), WA(.14f), R(em * .25f)); }
                    d->AddRectFilled(ImVec2(a.x + w * .06f, a.y + h * .10f), ImVec2(a.x + w * .40f, a.y + h * .20f), WA(.12f), R(em * .25f)); }
                else { d->AddRectFilled(ImVec2(a.x + w * .46f, a.y + h * .12f), ImVec2(a.x + w * .54f, a.y + h * .62f), WA(.30f), R(em * .3f));
                    d->AddRectFilled(ImVec2(a.x + w * .06f, a.y + h * .72f), ImVec2(a.x + w * .72f, a.y + h * .90f), WA(.14f), R(em * .3f)); d->AddRectFilled(ImVec2(a.x + w * .76f, a.y + h * .72f), ImVec2(b.x - w * .06f, a.y + h * .90f), U32(Mix(gAccent, ImVec4(0, 0, 0, 1), .3f)), R(em * .3f));
                    d->AddRectFilled(ImVec2(a.x + w * .06f, a.y + h * .12f), ImVec2(a.x + w * .22f, a.y + h * .22f), WA(.14f), R(em * .25f)); }
            };
            if (OptionCard(i == 0 ? "hm0" : "hm1", cw2, pvh, cfg.home == i, i == 0 ? T("Библиотека", "Library") : T("Игровой", "Game"),
                           i == 0 ? T("Сетка сборок и недавние миры", "Build grid and recent worlds") : T("Персонаж и большая кнопка «Играть»", "Character and a big \"Play\" button"), pv)) { cfg.home = i; SaveConfig(); }
        }
        ImGui::Dummy(ImVec2(0, em * .3f));
        TD(T("ФОН ЗА ПЕРСОНАЖЕМ", "BACKGROUND BEHIND THE CHARACTER"));
        if (Toggle("##scene", &cfg.charScene)) SaveConfig();
        ImGui::SameLine(0, em * .8f); ImGui::BeginGroup(); ImGui::TextUnformatted(T("Ночная пиксельная сцена", "Night pixel scene"));
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + std::max(em * 10.f, aw - em * 5.f)); TD(T("Горы, луна и искры. Выключите, если нужен простой фон.", "Mountains, moon and sparks. Turn it off for a plain background.")); ImGui::PopTextWrapPos(); ImGui::EndGroup();
        ImGui::Dummy(ImVec2(0, em * .1f));
        if (FancyBtn(T("Своя картинка...", "Own picture..."), ImVec2(em * 12.f, em * 2.f), gSoft)) { std::string f = PickImage(gHwnd); if (!f.empty()) { cfg.charBg = f; SaveConfig(); } }
        ImGui::SameLine(); if (FancyBtn(T("Сбросить", "Reset"), ImVec2(em * 8.f, em * 2.f), gSoft, !cfg.charBg.empty())) { cfg.charBg.clear(); SaveConfig(); }
        if (!cfg.charBg.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", PS(P(cfg.charBg).filename()).c_str()); }
        TD(T("PNG, JPG, BMP или TGA (до 20 МБ). Встанет за персонажем на весь блок.", "PNG, JPG, BMP or TGA (up to 20 MB). It goes behind the character, filling the block."));
        ImGui::Dummy(ImVec2(0, em * .3f));
        TD(T("ТЕМА", "THEME"));
        { int cols = aw > em * 54 ? 3 : 2; float cw = (aw - sp * (cols - 1)) / cols;
          for (int i = 0; i < 5; i++) { if (i % cols) ImGui::SameLine(0, sp); ThemeCard(i, cw, em * 5.2f); } }
        ImGui::Dummy(ImVec2(0, em * .3f));
        if (Th().glass) {
            TD(T("ФОН", "BACKGROUND"));
            if (FancyBtn(T("Загрузить", "Upload"), ImVec2(em * 11, em * 1.9f), gSoft)) { std::string f = PickImage(gHwnd); if (!f.empty()) { cfg.bg = f; cfg.slideshow = false; LoadBgAsync(f); } }
            ImGui::SameLine(); if (FancyBtn(T("Скрины из майна", "Minecraft screenshots"), ImVec2(em * 14, em * 1.9f), gSoft)) { ScanShots(); cfg.slideshow = true; NextShot(); }
            ImGui::Checkbox(T("Слайд-шоу (каждые ~18 сек)", "Slideshow (every ~18 s)"), &cfg.slideshow);
            ImGui::TextDisabled(T("Найдено скриншотов: %d", "Screenshots found: %d"), (int)gShots.size());
        } else TD(T("У этой темы сплошной фон: картинка-фон не используется.", "This theme has a solid background: the background picture is not used."));
    } else if (gSetTab == 2) { // ---- Компоненты (Java) ----
        static bool first = true; if (first) { first = false; RefreshJavas(); }
        ImGui::TextColored(gAccent, "%s", T("Рекомендуется установить все Java!", "It is recommended to install all Java versions!"));
        std::vector<JavaInfo> js; { std::lock_guard<std::mutex> l(gJavaMu); js = gJavas; }
        int inst = gJavaInst;
        for (int v : kJavaVers) {
            bool have = false; for (auto& j : js) if (j.major == v) have = true;
            float bh = em * 2.f; ImVec2 p = ImGui::GetCursorScreenPos();
            std::string nm = "Java " + std::to_string(v); dl->AddText(ImVec2(p.x, p.y + (bh - em) / 2), TextCol(), nm.c_str());
            ImGui::Dummy(ImVec2(em * 6.f, bh)); ImGui::SameLine();
            ImGui::PushID(v);
            if (FancyBtn(have ? T("Установлено", "Installed") : (inst == v ? T("Установка...", "Installing...") : T("Установить", "Install")), ImVec2(em * 10, bh), have ? gSoft : gAccent, !have && !inst)) std::thread(JavaInstallThread, v).detach();
            ImGui::PopID();
            ImGui::SameLine(); ImVec2 q = ImGui::GetCursorScreenPos(); ImGui::Dummy(ImVec2(em * 2.f, bh));
            ImVec2 m(q.x + em, q.y + bh / 2);
            if (have) IconCheck(dl, m, em * .5f, ImGui::GetColorU32(ImVec4(.30f, .78f, .38f, 1.f))); else IconCross(dl, m, em * .5f, ImGui::GetColorU32(ImVec4(.92f, .32f, .32f, 1.f)));
        }
        if (gJavaScan) TD(T("Проверяю установленные Java...", "Scanning installed Java..."));

    } else if (gSetTab == 3) { // ---- Java ----
        TD("Java");
        static bool first = true; if (first) { first = false; RefreshJavas(); }
        ImGui::SetNextItemWidth(-1);
        std::string cur = cfg.javaPath.empty() ? std::string(T("Авто (подберу и скачаю сам)", "Auto (pick and download automatically)")) : cfg.javaPath;
        if (ImGui::BeginCombo("##java", cur.c_str())) {
            if (ImGui::Selectable(T("Авто (подберу и скачаю сам)", "Auto (pick and download automatically)"), cfg.javaPath.empty())) { cfg.javaPath.clear(); bJava[0] = 0; }
            std::lock_guard<std::mutex> l(gJavaMu);
            for (auto& j : gJavas) { std::string t = "Java " + std::to_string(j.major) + "  -  " + j.exe; if (ImGui::Selectable(t.c_str(), cfg.javaPath == j.exe)) { cfg.javaPath = j.exe; snprintf(bJava, sizeof(bJava), "%s", j.exe.c_str()); } }
            ImGui::EndCombo();
        }
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputTextWithHint("##jp", T("...или свой путь к java.exe", "...or your own path to java.exe"), bJava, sizeof(bJava))) cfg.javaPath = bJava;
        TD(T("Аргументы Java", "Java arguments"));
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputTextWithHint("##jvm", "-XX:+UseZGC -Dfile.encoding=UTF-8", bJvm, sizeof(bJvm))) cfg.jvmArgs = bJvm;
        TD(T("Добавляются после стандартных (-Xmx, G1GC)", "Appended after the defaults (-Xmx, G1GC)"));
        TD(T("Память (ОЗУ)", "Memory (RAM)"));
        MEMORYSTATUSEX ms{sizeof(ms)}; GlobalMemoryStatusEx(&ms); int maxMb = std::max(2048, (int)(ms.ullTotalPhys / (1024 * 1024) * 3 / 4));
        ImGui::SetNextItemWidth(-1); ImGui::SliderInt("##ram", &cfg.ramMB, 1024, maxMb, T("%d МБ", "%d MB")); cfg.ramMB = std::min(cfg.ramMB, maxMb);

    } else { // ---- О приложении ----
        float aw = ImGui::GetWindowWidth(); auto Ctr = [&](const std::string& s) { ImGui::SetCursorPosX(std::max(0.f, (aw - ImGui::CalcTextSize(s.c_str()).x) / 2)); };
        ImGui::Dummy(ImVec2(0, em * 1.2f));
        ImGui::PushFont(gFTitle); { std::string n = std::string(kAppName) + " (PTL)"; Ctr(n); ImGui::TextColored(gAccent, "%s", n.c_str()); } ImGui::PopFont();
        { std::string v = std::string("v") + kVer; Ctr(v); ImGui::TextDisabled("%s", v.c_str()); }
        { std::string c = "Poltergeist Team x Claude"; Ctr(c); ImGui::TextUnformatted(c.c_str()); }
        ImGui::Dummy(ImVec2(0, em * .8f));
        float bw = em * 20.f, bh = em * 2.6f; int us = gUpdState;
        ImGui::SetCursorPosX((aw - bw) / 2);
        bool go = FancyBtn(T("       Проверить обновления", "       Check for updates"), ImVec2(bw, bh), gAccent, us != 1);
        { ImVec2 a = ImGui::GetItemRectMin(); MiniIcon(ImGui::GetWindowDrawList(), 6, ImVec2(a.x + bh * .55f, a.y + bh / 2), bh * .26f, OnAccent()); }
        if (go) std::thread(UpdateCheckThread).detach();
        std::string tag, url; { std::lock_guard<std::mutex> l(gUpdMu); tag = gUpdTag; url = gUpdUrl; }
        std::string msg = us == 1 ? T("Проверяю...", "Checking...") : us == 2 ? std::string(T("У вас последняя версия (v", "You have the latest version (v")) + kVer + ")" :
                          us == 3 ? std::string(T("Доступна новая версия: ", "A new version is available: ")) + tag : us == 4 ? T("Не удалось проверить обновления. Проверь интернет.", "Could not check for updates. Check your internet connection.") :
                          us == 5 ? T("На GitHub пока нет опубликованных релизов.", "There are no published releases on GitHub yet.") : "";
        ImGui::Dummy(ImVec2(0, em * .3f));
        if (!msg.empty()) { Ctr(msg); ImGui::TextColored(us == 3 ? gAccent : ImGui::GetStyleColorVec4(ImGuiCol_Text), "%s", msg.c_str()); }
        if (us == 3) { ImGui::SetCursorPosX((aw - bw) / 2); if (FancyBtn(T("Открыть страницу загрузки", "Open the download page"), ImVec2(bw, bh), gSoft)) OpenUrl(url.empty() ? kRepoUrl : url); }
    }
    ImGui::EndChild();
}

static float EaseOutBack(float x) { float c1 = 1.70158f, c3 = c1 + 1.f; x = std::clamp(x, 0.f, 1.f); return 1.f + c3 * powf(x - 1.f, 3.f) + c1 * powf(x - 1.f, 2.f); }
static float gToastT = -1.f; static bool gToastDone = false;
static void DrawToast(float W) { // плашка-приветствие сверху, как уведомление на iPhone: выезжает, висит несколько секунд и уезжает
    const Theme& th = Th(); float em = ImGui::GetFontSize(), dt = std::min(ImGui::GetIO().DeltaTime, 0.05f);
    std::string nick; // приветствуем только выбранный сейчас аккаунт (удалённые больше не всплывают)
    if (cfg.sel >= 0 && cfg.sel < (int)cfg.accs.size()) nick = cfg.accs[cfg.sel].name;
    if (!gToastDone && gTime > 0.9f && !nick.empty()) { gToastDone = true; gToastT = 0.f; StartWave(); }
    if (!gToastDone && gTime > 0.9f && nick.empty()) gFollowOn = true; // приветствовать некого: сразу следим за курсором
    if (gToastT < 0.f) return;
    gToastT += dt; const float tIn = 0.5f, hold = 4.2f, tOut = 0.45f, total = tIn + hold + tOut;
    if (gToastT > total) { gToastT = -1.f; return; }
    SYSTEMTIME st; GetLocalTime(&st); int hr = st.wHour; const char* g;
    if (cfg.lang == 1) g = hr >= 5 && hr < 11 ? "Good morning" : hr < 17 && hr >= 11 ? "Good afternoon" : hr >= 17 && hr < 23 ? "Good evening" : "Good night";
    else g = hr >= 5 && hr < 11 ? "Доброе утро" : hr < 17 && hr >= 11 ? "Добрый день" : hr >= 17 && hr < 23 ? "Добрый вечер" : "Доброй ночи";
    std::string line = std::string(g) + ", " + nick;
    ImVec2 ls; ImGui::PushFont(gFSub); ls = ImGui::CalcTextSize(line.c_str()); ImGui::PopFont();
    float icon = em * 2.3f, pad = em * .9f, w = pad + icon + em * .8f + std::max(ls.x, em * 6.f) + pad * 1.3f, h = icon + pad * 1.6f;
    float topY = em * .9f, y;
    if (gToastT < tIn) y = -h - em + (topY + h + em) * EaseOutBack(gToastT / tIn);
    else if (gToastT > tIn + hold) { float k = (gToastT - tIn - hold) / tOut; y = topY - (topY + h + em * 2) * k * k * k; }
    else y = topY;
    float x = (W - w) / 2; ImDrawList* dl = ImGui::GetForegroundDrawList(); float rr = R(h * .42f);
    auto U = [](const ImVec4& c) { return ImGui::ColorConvertFloat4ToU32(c); };
    for (int k = 4; k >= 1; k--) dl->AddRectFilled(ImVec2(x - k * 3.f, y - k * 2.f + 4), ImVec2(x + w + k * 3.f, y + h + k * 3.f + 4), IM_COL32(0, 0, 0, 14), rr + k * 3.f);
    ImVec4 bgc = th.light ? ImVec4(1.f, 1.f, 1.f, .97f) : ImVec4(th.pvBg.x + .03f, th.pvBg.y + .03f, th.pvBg.z + .035f, .97f);
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), U(bgc), rr); dl->AddRect(ImVec2(x, y), ImVec2(x + w, y + h), U(th.light ? ImVec4(0, 0, 0, .12f) : ImVec4(1, 1, 1, .16f)), rr);
    ImVec2 i0(x + pad, y + (h - icon) / 2); DrawLogo(dl, ImVec2(i0.x + icon / 2, i0.y + icon / 2), icon);
    float tx = i0.x + icon + em * .8f; ImU32 tc = th.light ? IM_COL32(25, 26, 34, 255) : IM_COL32(245, 245, 248, 255);
    dl->AddText(ImVec2(tx, y + pad * .55f), th.light ? IM_COL32(90, 90, 100, 255) : IM_COL32(170, 170, 180, 255), kAppName);
    dl->AddText(gFSub, gFSub->FontSize, ImVec2(tx, y + pad * .55f + em * 1.15f), tc, line.c_str());
}

static void DrawCover(ImDrawList* dl, const Tex& t, float alpha, ImVec2 sz) {
    if (!t.v || alpha <= 0.f) return;
    float ar = (float)t.w / t.h, sr = sz.x / sz.y; ImVec2 a(0, 0), b(1, 1);
    if (ar > sr) { float k = sr / ar; a.x = (1 - k) / 2; b.x = 1 - a.x; } else { float k = ar / sr; a.y = (1 - k) / 2; b.y = 1 - a.y; }
    float z = 0.02f + 0.02f * (0.5f + 0.5f * sinf(gTime * 0.15f)); // медленный "Ken Burns"-зум
    float cx = (a.x + b.x) / 2, cy = (a.y + b.y) / 2, hw = (b.x - a.x) / 2 * (1 - z), hh = (b.y - a.y) / 2 * (1 - z);
    dl->AddImage((ImTextureID)(intptr_t)t.v, ImVec2(0, 0), sz, ImVec2(cx - hw, cy - hh), ImVec2(cx + hw, cy + hh), IM_COL32(255, 255, 255, (int)(alpha * 255)));
    gBgUV0 = ImVec2(cx - hw, cy - hh); gBgUV1 = ImVec2(cx + hw, cy + hh); gBgSz = sz; // для размытых панелей
}

static void DrawUI(float W, float H) {
    const Theme& th = Th(); bool lt = th.light; ImVec2 sz(W, H);
    auto U = [](const ImVec4& c) { return ImGui::ColorConvertFloat4ToU32(c); };
    ImDrawList* bg = ImGui::GetBackgroundDrawList(); gBgOK = false;
    if (!th.glass) bg->AddRectFilled(ImVec2(0, 0), sz, U(th.bg1)); // плоские темы: сплошной цвет, без картинки
    else {
        if (!gBg[0].v) {
            float k = (.5f + .5f * sinf(gTime * 0.2f)) * .5f;
            ImVec4 c1(th.bg1.x + (th.bg2.x - th.bg1.x) * k, th.bg1.y + (th.bg2.y - th.bg1.y) * k, th.bg1.z + (th.bg2.z - th.bg1.z) * k, 1.f);
            ImVec4 c2(th.bg2.x + (th.bg1.x - th.bg2.x) * k, th.bg2.y + (th.bg1.y - th.bg2.y) * k, th.bg2.z + (th.bg1.z - th.bg2.z) * k, 1.f);
            bg->AddRectFilledMultiColor(ImVec2(0, 0), sz, U(c1), U(c1), U(c2), U(c2));
        } else {
            if (gBg[1].v && gFade < 1.f) DrawCover(bg, gBg[1], 1.f, sz);
            DrawCover(bg, gBg[0], gFade, sz); gBgOK = gBlur[0].v != nullptr;
            bg->AddRectFilled(ImVec2(0, 0), sz, U(ImVec4(th.bg2.x, th.bg2.y, th.bg2.z, lt ? .20f : .42f))); // оттенок темы поверх скриншота
        }
        if (lt) bg->AddRectFilledMultiColor(ImVec2(0, 0), sz, IM_COL32(255, 255, 255, 110), IM_COL32(255, 255, 255, 30), IM_COL32(255, 255, 255, 30), IM_COL32(255, 255, 255, 110));
        else bg->AddRectFilledMultiColor(ImVec2(0, 0), sz, IM_COL32(0, 0, 0, 90), IM_COL32(0, 0, 0, 10), IM_COL32(0, 0, 0, 10), IM_COL32(0, 0, 0, 90));
    }

    ImGui::SetNextWindowPos(ImVec2(0, 0)); ImGui::SetNextWindowSize(sz);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0)); ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::Begin("root", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::PopStyleVar(2);
    float em = ImGui::GetFontSize(), intro = AnimK("intro", 1.f, 2.5f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float tbH = em * 2.8f; gCapH = tbH; // верхняя полоса: за неё окно можно таскать
    { // название и версия
        ImFont* f = FSub(); float tx = em * 1.2f; dl->AddText(f, f->FontSize, ImVec2(tx, tbH / 2 + em * .1f - f->FontSize / 2), TextCol(), kAppName);
        std::string vs = std::string("v") + kVer; ImVec2 vz = ImGui::CalcTextSize(vs.c_str()), nz = ImGui::CalcTextSize(kAppName); float vx = tx + nz.x * (f->FontSize / ImGui::GetFontSize()) + em * .8f;
        Surface(dl, ImVec2(vx, tbH / 2 + em * .1f - em * .75f), ImVec2(vx + vz.x + em * 1.2f, tbH / 2 + em * .1f + em * .75f), R(em * .75f), 0);
        dl->AddText(ImVec2(vx + em * .6f, tbH / 2 + em * .1f - vz.y / 2), TextCol(.65f), vs.c_str());
    }
    { // кнопки окна: свернуть / развернуть (или в окно) / закрыть
        bool maxed = IsZoomed(gHwnd) != 0;
        float bw = em * 2.6f, bh = em * 1.7f, gp = em * .3f, total = bw * 3 + gp * 2, x0 = W - em * 1.2f - total, y0 = (tbH - bh) / 2 + em * .2f;
        gBtnX0 = x0 - em * .5f;
        for (int i = 0; i < 3; i++) {
            ImVec2 a(x0 + i * (bw + gp), y0), b(a.x + bw, a.y + bh); bool close = i == 2;
            ImGui::SetCursorScreenPos(a); ImGui::PushID(100 + i);
            bool clk = ImGui::InvisibleButton("##wb", ImVec2(bw, bh));
            float t = AnimK("h", ImGui::IsItemHovered() ? 1.f : 0.f, 14.f);
            ImVec4 base = lt ? ImVec4(0, 0, 0, .08f) : ImVec4(1, 1, 1, .10f);
            ImVec4 hov = close ? ImVec4(.91f, .28f, .28f, .95f) : (lt ? ImVec4(0, 0, 0, .18f) : ImVec4(1, 1, 1, .22f));
            ImVec4 c(base.x + (hov.x - base.x) * t, base.y + (hov.y - base.y) * t, base.z + (hov.z - base.z) * t, base.w + (hov.w - base.w) * t);
            dl->AddRectFilled(a, b, ImGui::GetColorU32(c), R(bh * .32f));
            ImU32 icol = (close && t > .5f) ? IM_COL32(255, 255, 255, 255) : TextCol();
            float s2 = em * .36f, th = std::max(1.5f, 1.6f * gS), mx = (a.x + b.x) / 2, my = (a.y + b.y) / 2;
            if (i == 0) dl->AddLine(ImVec2(mx - s2, my + s2 * .4f), ImVec2(mx + s2, my + s2 * .4f), icol, th);
            else if (i == 1) {
                if (!maxed) dl->AddRect(ImVec2(mx - s2, my - s2), ImVec2(mx + s2, my + s2), icol, R(2.f), 0, th);
                else {
                    ImVec2 pts[5] = {ImVec2(mx - s2 * .4f, my - s2 * .4f), ImVec2(mx - s2 * .4f, my - s2), ImVec2(mx + s2, my - s2), ImVec2(mx + s2, my + s2 * .4f), ImVec2(mx + s2 * .4f, my + s2 * .4f)};
                    dl->AddPolyline(pts, 5, icol, 0, th);
                    dl->AddRect(ImVec2(mx - s2, my - s2 * .4f), ImVec2(mx + s2 * .4f, my + s2), icol, R(2.f), 0, th);
                }
            } else {
                dl->AddLine(ImVec2(mx - s2 * .85f, my - s2 * .85f), ImVec2(mx + s2 * .85f, my + s2 * .85f), icol, th);
                dl->AddLine(ImVec2(mx - s2 * .85f, my + s2 * .85f), ImVec2(mx + s2 * .85f, my - s2 * .85f), icol, th);
            }
            ImGui::PopID();
            if (clk) PostMessageW(gHwnd, close ? WM_CLOSE : WM_SYSCOMMAND, i == 0 ? SC_MINIMIZE : (i == 1 ? (maxed ? SC_RESTORE : SC_MAXIMIZE) : 0), 0); // через очередь, не посреди кадра
        }
    }

    bool compact = W < em * 58.f; float navW = compact ? em * 4.4f : em * 11.8f, nx = em * 1.0f, nh = H - tbH - em * 1.0f, slide = (1 - intro) * em * 3;
    DrawNav(nx - slide, tbH, navW, nh);
    float px = nx + navW + em * .9f - slide, pw = W - (nx + navW + em * .9f) - em * 1.0f, py = tbH, ph = nh;
    { ImVec4 pc = PanelTint(intro); Surface(dl, ImVec2(px, py), ImVec2(px + pw, py + ph), R(em * 1.1f), 2, 0.f, &pc); }
    ImGui::SetCursorPos(ImVec2(px + em, py + em));
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, intro);
    ImGui::BeginChild("panel", ImVec2(pw - em * 2, ph - em * 2), false, ImGuiWindowFlags_NoBackground);
    gScrFade = std::min(1.f, gScrFade + ImGui::GetIO().DeltaTime * 7.f);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (1 - gScrFade) * em * .8f);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, intro * gScrFade);
    ImGui::BeginChild("content", ImVec2(0, 0), false, ImGuiWindowFlags_NoBackground);
    switch (gScr) {
    case S_ACC: ScrAccounts(); break; case S_BUILDS: ScrBuilds(); break; case S_MODS: ScrMods(); break; case S_CREATE: ScrCreate(); break; case S_SET: ScrSettings(); break;
    case S_LOG: ScrLog(); break; case S_SKINS: ScrSkins(); break; case S_STATS: ScrStats(); break; case S_NEWS: ScrNews(); break; default: ScrMain(); }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::EndChild();
    ImGui::PopStyleVar();

    // окно с объяснением краша
    static std::string cT, cB;
    std::string popId = std::string(T("Что-то пошло не так", "Something went wrong")) + "###crash";
    { std::lock_guard<std::mutex> l(gMu); if (gShowCrash) { gShowCrash = false; cT = gCrashTitle; cB = gCrashBody; ImGui::OpenPopup("###crash"); } }
    ImGui::SetNextWindowPos(ImVec2(W / 2, H / 2), ImGuiCond_Always, ImVec2(.5f, .5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(em * 30, W - em * 2), 0));
    if (ImGui::BeginPopupModal(popId.c_str(), nullptr, ImGuiWindowFlags_NoResize)) {
        ImGui::PushFont(gFSub); ImGui::TextColored(ImVec4(1.f, .45f, .4f, 1.f), "%s", cT.c_str()); ImGui::PopFont();
        ImGui::Separator(); ImGui::TextWrapped("%s", cB.c_str()); ImGui::Dummy(ImVec2(0, em * .5f));
        if (cfg.logsOn) { if (ImGui::Button(T("Показать лог", "Show log"), ImVec2(em * 8, 0))) { Go(S_LOG); ImGui::CloseCurrentPopup(); } ImGui::SameLine(); }
        if (ImGui::Button(T("Закрыть", "Close"), ImVec2(em * 8, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }


    // подтверждение удаления сборки
    static DelReq dr;
    if (gDelAsk) { dr = *gDelAsk; gDelAsk.reset(); ImGui::OpenPopup("###delinst"); }
    ImGui::SetNextWindowPos(ImVec2(W / 2, H / 2), ImGuiCond_Always, ImVec2(.5f, .5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(em * 30, W - em * 2), 0));
    std::string delId = std::string(T("Удалить сборку?", "Delete build?")) + "###delinst";
    if (ImGui::BeginPopupModal(delId.c_str(), nullptr, ImGuiWindowFlags_NoResize)) {
        ImGui::PushFont(gFSub); ImGui::TextColored(ImVec4(1.f, .45f, .4f, 1.f), "%s", dr.b.name.c_str()); ImGui::PopFont();
        ImGui::TextDisabled("%s  %s", dr.b.loader.c_str(), dr.b.ver.c_str());
        ImGui::Separator();
        ImGui::TextWrapped("%s", T("Папка этой сборки будет удалена безвозвратно вместе с мирами, модами, настройками и скриншотами. Общие файлы игры (библиотеки и ресурсы) останутся.",
                                   "This build folder will be deleted permanently, including worlds, mods, settings and screenshots. Shared game files (libraries and assets) stay."));
        ImGui::Dummy(ImVec2(0, em * .5f));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(.85f, .25f, .25f, .75f)); ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(.90f, .30f, .30f, .90f)); ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(.95f, .35f, .35f, 1.f));
        bool yes = ImGui::Button(T("Удалить", "Delete"), ImVec2(em * 8, 0));
        ImGui::PopStyleColor(3); ImGui::SameLine();
        bool no = ImGui::Button(T("Отмена", "Cancel"), ImVec2(em * 8, 0));
        if (yes && !gBusy && !gGameRunning && !gDelBusy) std::thread(DeleteThread, dr.b).detach();
        if (yes || no) { ImGui::CloseCurrentPopup(); gBuildsT = 99.f; }
        ImGui::EndPopup();
    }
    ImGui::End();
    DrawToast(W);
}
// ============================ Win32 / D3D11 ============================
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
static void CreateRTV() { ID3D11Texture2D* b = nullptr; gSwap->GetBuffer(0, IID_PPV_ARGS(&b)); gDev->CreateRenderTargetView(b, nullptr, &gRTV); b->Release(); }
// ---------- Трей: «свернуть лаунчер в фон» ----------
static NOTIFYICONDATAW gNid; static bool gTray = false;
static void TrayAdd() {
    if (gTray) return;
    ZeroMemory(&gNid, sizeof(gNid)); gNid.cbSize = sizeof(gNid); gNid.hWnd = gHwnd; gNid.uID = 1;
    gNid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP; gNid.uCallbackMessage = WM_TRAY;
    gNid.hIcon = LoadIconW(nullptr, IDI_APPLICATION); lstrcpynW(gNid.szTip, L"PTLauncher", 64);
    gTray = Shell_NotifyIconW(NIM_ADD, &gNid) != FALSE;
}
static void TrayDel() { if (!gTray) return; Shell_NotifyIconW(NIM_DELETE, &gNid); gTray = false; }
static void HideToTray() { TrayAdd(); if (gTray) ShowWindow(gHwnd, SW_HIDE); else ShowWindow(gHwnd, SW_MINIMIZE); } // без значка в трее не прячем - иначе окно не вернуть
static void ShowFromTray() { TrayDel(); if (!IsWindowVisible(gHwnd)) ShowWindow(gHwnd, SW_SHOW); if (IsIconic(gHwnd)) ShowWindow(gHwnd, SW_RESTORE); SetForegroundWindow(gHwnd); }
static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (ImGui_ImplWin32_WndProcHandler(h, m, w, l)) return true;
    switch (m) {
    case WM_NCCALCSIZE: // системного заголовка и рамки нет: клиентская область = всё окно
        if (w) {
            if (IsZoomed(h)) { // развёрнутое окно выходит за экран на толщину рамки
                NCCALCSIZE_PARAMS* np = (NCCALCSIZE_PARAMS*)l;
                int fx = GetSystemMetrics(SM_CXFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER), fy = GetSystemMetrics(SM_CYFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
                np->rgrc[0].left += fx; np->rgrc[0].top += fy; np->rgrc[0].right -= fx; np->rgrc[0].bottom -= fy;
            }
            return 0;
        }
        break;
    case WM_NCHITTEST: { // края - изменение размера, верхняя полоса - перетаскивание, остальное - клиент (ImGui)
        POINT pt{(short)LOWORD(l), (short)HIWORD(l)}; RECT rc; GetWindowRect(h, &rc);
        if (!IsZoomed(h)) {
            int b = (int)(6 * gS); bool L = pt.x < rc.left + b, Rt = pt.x >= rc.right - b, Tp = pt.y < rc.top + b, Bt = pt.y >= rc.bottom - b;
            if (Tp && L) return HTTOPLEFT; if (Tp && Rt) return HTTOPRIGHT; if (Bt && L) return HTBOTTOMLEFT; if (Bt && Rt) return HTBOTTOMRIGHT;
            if (L) return HTLEFT; if (Rt) return HTRIGHT; if (Tp) return HTTOP; if (Bt) return HTBOTTOM;
        }
        ScreenToClient(h, &pt);
        if (pt.y >= 0 && pt.y < (LONG)gCapH && pt.x < (LONG)gBtnX0) return HTCAPTION;
        return HTCLIENT;
    }
    case WM_SIZE:
        if (gDev && w != SIZE_MINIMIZED) { if (gRTV) { gRTV->Release(); gRTV = nullptr; } gSwap->ResizeBuffers(0, LOWORD(l), HIWORD(l), DXGI_FORMAT_UNKNOWN, 0); CreateRTV(); }
        return 0;
    case WM_GETMINMAXINFO: { MINMAXINFO* mm = (MINMAXINFO*)l; mm->ptMinTrackSize.x = (LONG)(900 * gS); mm->ptMinTrackSize.y = (LONG)(600 * gS); return 0; }
    case WM_SYSCOMMAND: if ((w & 0xfff0) == SC_KEYMENU) return 0; break;
    case WM_APP_HIDE: HideToTray(); return 0;
    case WM_APP_SHOW: ShowFromTray(); return 0;
    case WM_TRAY:
        if (LOWORD(l) == WM_LBUTTONUP || LOWORD(l) == WM_LBUTTONDBLCLK) ShowFromTray();
        else if (LOWORD(l) == WM_RBUTTONUP) {
            POINT pt; GetCursorPos(&pt); HMENU mn = CreatePopupMenu();
            AppendMenuW(mn, MF_STRING, 1, W(T("Показать лаунчер", "Show launcher")).c_str()); AppendMenuW(mn, MF_STRING, 2, W(T("Закрыть", "Quit")).c_str());
            SetForegroundWindow(h); int cmd = (int)TrackPopupMenu(mn, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, h, nullptr); DestroyMenu(mn); PostMessageW(h, WM_NULL, 0, 0);
            if (cmd == 1) ShowFromTray(); else if (cmd == 2) PostMessageW(h, WM_CLOSE, 0, 0);
        }
        return 0;
    case WM_DESTROY: TrayDel(); PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}
static void OpenSessionLog() { // полный лог сессии: logs/latest.log (прошлый запуск сохраняется как previous.log)
    std::error_code ec; fs::path d = gCfgDir / "logs"; fs::create_directories(d, ec);
    fs::path cur = d / "latest.log", prev = d / "previous.log";
    if (fs::exists(cur, ec)) { fs::remove(prev, ec); fs::rename(cur, prev, ec); }
    gLogFile.open(cur, std::ios::binary | std::ios::trunc);
    Log(std::string(kAppName) + " v" + kVer + " запущен. Папка лаунчера: " + PS(gCfgDir));
}
int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR, int) {
    ImGui_ImplWin32_EnableDpiAwareness();
    float S = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY)); gS = S;
    { // папка данных теперь .ptlauncher; данные из старой .mclauncher переносим один раз
        fs::path ap = fs::path(Env(L"APPDATA")), old = ap / ".mclauncher"; gCfgDir = ap / ".ptlauncher"; std::error_code e0;
        if (!fs::exists(gCfgDir, e0) && fs::exists(old, e0)) { fs::rename(old, gCfgDir, e0); if (e0) gCfgDir = old; }
        gRoot = gCfgDir;
    }
    std::error_code ec; fs::create_directories(gCfgDir, ec);
    WNDCLASSEXW wc{sizeof(wc), CS_CLASSDC, WndProc, 0, 0, hi, nullptr, LoadCursor(nullptr, IDC_ARROW), nullptr, nullptr, L"PTL", nullptr};
    RegisterClassExW(&wc);
    gHwnd = CreateWindowW(L"PTL", L"PTLauncher", WS_OVERLAPPEDWINDOW, 100, 100, (int)(1100 * S), (int)(680 * S), nullptr, nullptr, hi, nullptr);

    { // без системного заголовка: тень окна от DWM + пересчёт рамки
        BOOL comp = FALSE; if (SUCCEEDED(DwmIsCompositionEnabled(&comp)) && comp) { MARGINS mg{1, 1, 1, 1}; DwmExtendFrameIntoClientArea(gHwnd, &mg); }
        SetWindowPos(gHwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
    DXGI_SWAP_CHAIN_DESC sd{}; sd.BufferCount = 2; sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = gHwnd; sd.SampleDesc.Count = 1; sd.Windowed = TRUE; sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    D3D_FEATURE_LEVEL fl; const D3D_FEATURE_LEVEL lv[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, lv, 2, D3D11_SDK_VERSION, &sd, &gSwap, &gDev, &fl, &gCtx))) {
        MessageBoxW(nullptr, L"Не удалось создать Direct3D 11", L"PTLauncher", MB_ICONERROR); return 1;
    }
    CreateRTV();
    ShowWindow(gHwnd, SW_SHOWDEFAULT); UpdateWindow(gHwnd);

    ImGui::CreateContext(); ImGuiIO& io = ImGui::GetIO(); io.IniFilename = nullptr;
    {
        wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::string dir = U8(fs::path(exe).parent_path().wstring());
        std::vector<std::string> cand = {dir + "\\font.ttf", "C:\\Windows\\Fonts\\segoeui.ttf", "C:\\Windows\\Fonts\\arial.ttf",
                                         "C:\\Windows\\Fonts\\tahoma.ttf", "C:\\Windows\\Fonts\\verdana.ttf", "C:\\Windows\\Fonts\\calibri.ttf"};
        bool ok = false; std::string fp; // Латиница + кириллица + общая пунктуация (тире, кавычки, многоточие, •): раньше этих символов в шрифте не было и вместо них рисовались «?»
        static const ImWchar kRanges[] = {0x0020, 0x00FF, 0x0400, 0x052F, 0x2000, 0x206F, 0x20A0, 0x20CF, 0x2190, 0x21FF, 0x2DE0, 0x2DFF, 0xA640, 0xA69F, 0};
        const ImWchar* rng = kRanges;
        for (auto& f : cand)
            if (fs::exists(P(f)) && io.Fonts->AddFontFromFileTTF(f.c_str(), 18.f * S, nullptr, rng)) { ok = true; fp = f; break; }
        if (!ok) io.Fonts->AddFontDefault(); // без кириллицы: будут «?»
        else { // крупные шрифты настоящего размера (а не растянутый мелкий): текст остаётся чётким
            gFTitle = io.Fonts->AddFontFromFileTTF(fp.c_str(), 27.f * S, nullptr, rng);
            gFSub = io.Fonts->AddFontFromFileTTF(fp.c_str(), 23.4f * S, nullptr, rng);
        }
    }
    ImGui_ImplWin32_Init(gHwnd); ImGui_ImplDX11_Init(gDev, gCtx);

    LoadConfig();
    { auto fx = [](std::string& s) { size_t p = s.find(".mclauncher"); if (p != std::string::npos) s.replace(p, 11, ".ptlauncher"); }; fx(cfg.javaPath); fx(cfg.gameDir); fx(cfg.bg); fx(cfg.shotsDir); } // пути после переименования папки
    snprintf(bJava, sizeof(bJava), "%s", cfg.javaPath.c_str()); snprintf(bJvm, sizeof(bJvm), "%s", cfg.jvmArgs.c_str()); gLogOn = cfg.logsOn; OpenSessionLog();
    if (!cfg.gameDir.empty()) { // своя папка игры из настроек
        std::error_code e2; fs::create_directories(P(cfg.gameDir), e2);
        if (!e2 && fs::is_directory(P(cfg.gameDir), e2)) gRoot = P(cfg.gameDir); else cfg.gameDir.clear();
    }
    MigrateInstances(); LoadLoaderCache();
    std::thread(FetchVersions).detach();
    std::thread(FetchLoaderLists).detach();
    ScanShots();
    if (!cfg.bg.empty() && !cfg.slideshow) LoadBgAsync(cfg.bg); else NextShot();

    bool done = false; auto last = std::chrono::steady_clock::now();
    while (!done) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); if (msg.message == WM_QUIT) done = true; }
        if (done) break;
        if (IsIconic(gHwnd) || !IsWindowVisible(gHwnd)) { Sleep(60); continue; }
        auto now = std::chrono::steady_clock::now(); float dt = std::chrono::duration<float>(now - last).count(); last = now; gTime += dt;

        { std::lock_guard<std::mutex> l(gMu); if (gNewAcc) {
            bool rep = false; for (auto& a : cfg.accs) if (a.type == gNewAcc->type && a.uuid == gNewAcc->uuid) { a = *gNewAcc; rep = true; }
            if (!rep) { cfg.accs.push_back(*gNewAcc); cfg.sel = (int)cfg.accs.size() - 1; }
            gNewAcc.reset(); SaveConfig(); } }
        { long long ss = gStatSec.exchange(0); int sl2 = gStatLaunch.exchange(0); if (ss || sl2) { cfg.playSec += ss; cfg.launches += sl2; SaveConfig(); } } // статистика из потока запуска
        SkinTick();
        UpdateBackground(dt);

        { int sg = std::clamp(cfg.theme, 0, 4) | 16 | (cfg.style << 5); if (sg != gThemeSig) { gThemeSig = sg; ApplyTheme(); } }
        ImGui_ImplDX11_NewFrame(); ImGui_ImplWin32_NewFrame(); ImGui::NewFrame();
        RECT rc; GetClientRect(gHwnd, &rc);
        DrawUI((float)(rc.right - rc.left), (float)(rc.bottom - rc.top));
        ImGui::Render();
        const float clear[4] = {0.05f, 0.06f, 0.09f, 1.f};
        if (gRTV) { gCtx->OMSetRenderTargets(1, &gRTV, nullptr); gCtx->ClearRenderTargetView(gRTV, clear); ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData()); }
        gSwap->Present(1, 0); // VSync — не жжём CPU/GPU
    }
    SaveConfig();
    ImGui_ImplDX11_Shutdown(); ImGui_ImplWin32_Shutdown(); ImGui::DestroyContext();
    if (gRTV) gRTV->Release(); gSwap->Release(); gCtx->Release(); gDev->Release();
    DestroyWindow(gHwnd); UnregisterClassW(L"PTL", hi);
    return 0;
}
