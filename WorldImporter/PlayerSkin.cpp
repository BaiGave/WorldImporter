#include "PlayerSkin.h"
#include "config.h"
#include "fileutils.h"
#include "texture.h"
#include "include/stb_image.h"
#include "include/stb_image_write.h"

#include <Windows.h>

#include <cctype>
#include <climits>
#include <condition_variable>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#endif

namespace {

struct Image {
    int w = 0;
    int h = 0;
    std::vector<unsigned char> px; // tightly packed RGBA, top-left origin
};

std::string TagString(const NbtTagPtr& tag) {
    if (!tag || tag->type != TagType::STRING || tag->payload.empty()) return "";
    return std::string(tag->payload.begin(), tag->payload.end());
}

// Local lookup. getChildByName logs an error when the parent is not a compound.
NbtTagPtr Child(const NbtTagPtr& tag, const char* name) {
    if (!tag || tag->type != TagType::COMPOUND) return nullptr;
    for (const auto& c : tag->children) {
        if (c && c->name == name) return c;
    }
    return nullptr;
}

std::string ExeDir() {
    wchar_t buffer[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (n == 0) return "";
    std::string s = wstring_to_string(std::wstring(buffer, n));
    size_t pos = s.find_last_of("\\/");
    return pos == std::string::npos ? s : s.substr(0, pos);
}

bool EnsureDir(const std::string& path) {
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    return !ec || std::filesystem::is_directory(path);
}

bool ReadFileBytes(const std::string& path, std::vector<unsigned char>& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    in.seekg(0, std::ios::end);
    std::streamoff n = in.tellg();
    if (n < 0) return false;
    in.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(n));
    if (n > 0 && !in.read(reinterpret_cast<char*>(out.data()), n)) return false;
    return true;
}

bool WriteFileBytes(const std::string& path, const std::vector<unsigned char>& bytes) {
    std::filesystem::path p(path);
    if (p.has_parent_path() && !EnsureDir(p.parent_path().string())) return false;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    if (!bytes.empty())
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
}

int B64Val(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

std::string Base64Decode(const std::string& in) {
    std::string out;
    out.reserve(in.size() * 3 / 4);
    int val = 0;
    int valb = -8;
    for (unsigned char c : in) {
        if (c == '=' || std::isspace(c)) continue;
        int d = B64Val(c);
        if (d < 0) return "";
        val = (val << 6) + d;
        valb += 6;
        if (valb >= 0) {
            out.push_back(static_cast<char>((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return out;
}

// Mojang skin JSON: {"textures":{"SKIN":{"url":".../texture/<hash>"}}}
std::string ExtractSkinHash(const std::string& json) {
    size_t skin = json.find("\"SKIN\"");
    std::string region = skin == std::string::npos ? json : json.substr(skin);
    size_t urlKey = region.find("\"url\"");
    if (urlKey == std::string::npos) return "";
    size_t colon = region.find(':', urlKey);
    if (colon == std::string::npos) return "";
    size_t q1 = region.find('"', colon);
    if (q1 == std::string::npos) return "";
    size_t q2 = region.find('"', q1 + 1);
    if (q2 == std::string::npos) return "";
    std::string url = region.substr(q1 + 1, q2 - q1 - 1);
    size_t pos = url.rfind("/texture/");
    if (pos == std::string::npos) return "";
    std::string hash = url.substr(pos + 9);
    size_t cut = hash.find_first_of("?#");
    if (cut != std::string::npos) hash.resize(cut);
    while (!hash.empty() && (hash.back() == '/' || std::isspace(static_cast<unsigned char>(hash.back()))))
        hash.pop_back();
    for (char& c : hash) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    if (hash.empty() || hash.size() > 64) return "";
    for (char c : hash) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return "";
    }
    return hash;
}

std::string PropertyValue(const NbtTagPtr& comp) {
    std::string v = TagString(Child(comp, "value"));
    if (!v.empty()) return v;
    return TagString(Child(comp, "Value"));
}

// 1.20.5+ profile.properties[{name,value}] and <=1.20.4 SkullOwner.Properties.textures[{Value}].
std::string FindTexturesValue(const NbtTagPtr& profile) {
    NbtTagPtr props = Child(profile, "properties");
    if (!props) props = Child(profile, "Properties");
    if (!props) return "";

    auto fromList = [](const NbtTagPtr& list, bool requireTexturesName) -> std::string {
        if (!list || list->type != TagType::LIST) return "";
        for (const auto& el : list->children) {
            if (!el || el->type != TagType::COMPOUND) continue;
            if (requireTexturesName) {
                std::string name = TagString(Child(el, "name"));
                if (name != "textures") continue;
            }
            std::string v = PropertyValue(el);
            if (!v.empty()) return v;
        }
        return "";
    };

    if (props->type == TagType::LIST) return fromList(props, true);
    if (props->type != TagType::COMPOUND) return "";

    NbtTagPtr textures = Child(props, "textures");
    if (textures && textures->type == TagType::LIST) return fromList(textures, false);
    if (textures && textures->type == TagType::COMPOUND) return PropertyValue(textures);
    return "";
}

// "namespace:path" relative to assets/<ns>/textures/. Default namespace is minecraft.
bool SplitResourceLocation(const std::string& loc, std::string& ns, std::string& path) {
    std::string s = loc;
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    if (s.empty()) return false;
    size_t colon = s.find(':');
    if (colon == std::string::npos) {
        ns = "minecraft";
        path = s;
    } else {
        ns = s.substr(0, colon);
        path = s.substr(colon + 1);
    }
    if (ns.empty() || path.empty()) return false;
    if (path.rfind("textures/", 0) == 0) path = path.substr(9);
    if (path.size() >= 4 && path.compare(path.size() - 4, 4, ".png") == 0) path.resize(path.size() - 4);
    return !path.empty();
}

std::string UseResourceTexture(const std::string& loc) {
    std::string ns, path;
    if (!SplitResourceLocation(loc, ns, path)) return "";
    std::string dir = "textures";
    if (!SaveTextureToFile(ns, path, dir)) return "";
    std::string saved = "textures/" + ns + "/" + path + ".png";
    RegisterTexture(ns, path, saved);
    return saved;
}

// NativeImage.copyRect(srcX, srcY, offsetX, offsetY, w, h, flipX, flipY):
// destination origin is (srcX+offsetX, srcY+offsetY). In-place, same loop order as vanilla.
void CopyRect(Image& img, int sx, int sy, int ox, int oy, int rw, int rh, bool flipX, bool flipY) {
    const int dx0 = sx + ox;
    const int dy0 = sy + oy;
    for (int y = 0; y < rh; ++y) {
        for (int x = 0; x < rw; ++x) {
            int sxp = sx + x;
            int syp = sy + y;
            int dx = dx0 + (flipX ? rw - 1 - x : x);
            int dy = dy0 + (flipY ? rh - 1 - y : y);
            if (sxp < 0 || syp < 0 || sxp >= img.w || syp >= img.h) continue;
            if (dx < 0 || dy < 0 || dx >= img.w || dy >= img.h) continue;
            const unsigned char* s = &img.px[(static_cast<size_t>(syp) * img.w + sxp) * 4];
            unsigned char* d = &img.px[(static_cast<size_t>(dy) * img.w + dx) * 4];
            unsigned char tmp[4] = {s[0], s[1], s[2], s[3]};
            d[0] = tmp[0];
            d[1] = tmp[1];
            d[2] = tmp[2];
            d[3] = tmp[3];
        }
    }
}

void SetNoAlpha(Image& img, int x0, int y0, int x1, int y1) {
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            if (x < 0 || y < 0 || x >= img.w || y >= img.h) continue;
            img.px[(static_cast<size_t>(y) * img.w + x) * 4 + 3] = 255;
        }
    }
}

// Legacy skins whose hat rect is fully opaque (Notch) have that rect cleared,
// otherwise the hat is a solid copy of the head.
void DoNotchTransparencyHack(Image& img, int x0, int y0, int x1, int y1) {
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            if (x < 0 || y < 0 || x >= img.w || y >= img.h) return;
            if (img.px[(static_cast<size_t>(y) * img.w + x) * 4 + 3] < 128) return;
        }
    }
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            img.px[(static_cast<size_t>(y) * img.w + x) * 4 + 3] = 0;
        }
    }
}

// SkinTextureDownloader.processLegacySkin (26.3 / HttpTexture 1.21.1).
Image ProcessLegacySkin(Image src) {
    const bool legacy = src.h == 32;
    Image img;
    if (legacy) {
        img.w = 64;
        img.h = 64;
        img.px.assign(64 * 64 * 4, 0);
        std::memcpy(img.px.data(), src.px.data(), 64 * 32 * 4);
        const int rects[][6] = {
            {4, 16, 16, 32, 4, 4},
            {8, 16, 16, 32, 4, 4},
            {0, 20, 24, 32, 4, 12},
            {4, 20, 16, 32, 4, 12},
            {8, 20, 8, 32, 4, 12},
            {12, 20, 16, 32, 4, 12},
            {44, 16, -8, 32, 4, 4},
            {48, 16, -8, 32, 4, 4},
            {40, 20, 0, 32, 4, 12},
            {44, 20, -8, 32, 4, 12},
            {48, 20, -16, 32, 4, 12},
            {52, 20, -8, 32, 4, 12},
        };
        for (const auto& r : rects)
            CopyRect(img, r[0], r[1], r[2], r[3], r[4], r[5], true, false);
    } else {
        img = std::move(src);
    }
    SetNoAlpha(img, 0, 0, 32, 16);
    if (legacy) DoNotchTransparencyHack(img, 32, 0, 64, 32);
    SetNoAlpha(img, 0, 16, 64, 32);
    SetNoAlpha(img, 16, 48, 48, 64);
    return img;
}

bool DecodeSkinPng(const std::vector<unsigned char>& bytes, Image& img, std::string& reason) {
    if (bytes.size() > static_cast<size_t>(INT32_MAX)) {
        reason = "png too large";
        return false;
    }
    int w = 0, h = 0, comp = 0;
    unsigned char* data = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h, &comp, 4);
    if (!data) {
        reason = "decode failed";
        return false;
    }
    if (!((w == 64 && h == 64) || (w == 64 && h == 32))) {
        reason = "size " + std::to_string(w) + "x" + std::to_string(h);
        stbi_image_free(data);
        return false;
    }
    img.w = w;
    img.h = h;
    img.px.assign(data, data + static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
    stbi_image_free(data);
    return true;
}

std::string CacheFile(const std::string& hash) {
    return JoinPath(ExeDir(), "skin_cache", hash + ".png");
}

#ifndef _WIN32
std::string ShellQuote(const std::string& s) {
    std::string o = "'";
    for (char c : s) {
        if (c == '\'') o += "'\\''";
        else o.push_back(c);
    }
    o.push_back('\'');
    return o;
}
#endif

bool DownloadRaw(const std::string& hash, std::vector<unsigned char>& out, std::string& reason) {
    const std::string url = "https://textures.minecraft.net/texture/" + hash;
#ifdef _WIN32
    HINTERNET session = WinHttpOpen(L"WorldImporter", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        reason = "winhttp open failed";
        return false;
    }
    WinHttpSetTimeouts(session, 10000, 10000, 10000, 10000);
    HINTERNET connect = WinHttpConnect(session, L"textures.minecraft.net", INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!connect) {
        WinHttpCloseHandle(session);
        reason = "winhttp connect failed";
        return false;
    }
    std::wstring wpath = L"/texture/";
    wpath.append(hash.begin(), hash.end());
    HINTERNET request = WinHttpOpenRequest(connect, L"GET", wpath.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!request) {
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        reason = "winhttp request failed";
        return false;
    }
    DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
    bool ok = false;
    if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request, nullptr)) {
        reason = "winhttp transfer failed";
    } else {
        DWORD status = 0;
        DWORD sz = sizeof(status);
        if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX)) {
            reason = "winhttp status failed";
        } else if (status / 100 != 2) {
            reason = "http " + std::to_string(status);
        } else {
            out.clear();
            for (;;) {
                DWORD avail = 0;
                if (!WinHttpQueryDataAvailable(request, &avail)) {
                    reason = "read failed";
                    out.clear();
                    break;
                }
                if (avail == 0) {
                    ok = !out.empty();
                    if (!ok) reason = "empty download";
                    break;
                }
                size_t off = out.size();
                out.resize(off + avail);
                DWORD read = 0;
                if (!WinHttpReadData(request, out.data() + off, avail, &read)) {
                    reason = "read failed";
                    out.clear();
                    break;
                }
                out.resize(off + read);
            }
        }
    }
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    (void)url;
    return ok;
#else
    std::string dir = JoinPath(ExeDir(), "skin_cache");
    if (!EnsureDir(dir)) {
        reason = "mkdir failed";
        return false;
    }
    std::string tmp = JoinPath(dir, hash + ".part");
    std::string cmd = "curl -sfL --max-time 10 -o " + ShellQuote(tmp) + " " + ShellQuote(url);
    int rc = std::system(cmd.c_str());
    if (rc != 0) {
        std::error_code ec;
        std::filesystem::remove(tmp, ec);
        reason = "curl failed";
        return false;
    }
    bool read = ReadFileBytes(tmp, out);
    std::error_code ec;
    std::filesystem::remove(tmp, ec);
    if (!read || out.empty()) {
        reason = "empty download";
        return false;
    }
    return true;
#endif
}

std::string WriteExportTexture(const std::string& hash, const Image& img, std::string& reason) {
    std::string dir = JoinPath({ExeDir(), "textures", "minecraft", "entity", "player_skins"});
    if (!EnsureDir(dir)) {
        reason = "mkdir failed";
        return "";
    }
    std::string full = JoinPath(dir, hash + ".png");
    if (img.w != 64 || img.h != 64 ||
        !stbi_write_png(full.c_str(), img.w, img.h, 4, img.px.data(), img.w * 4)) {
        reason = "write failed";
        return "";
    }
    std::string rel = "textures/minecraft/entity/player_skins/" + hash + ".png";
    RegisterTexture("minecraft", "entity/player_skins/" + hash, rel);
    return rel;
}

// One in-flight download per hash. Waiters block until the owner finishes.
struct Slot {
    bool done = false;
    std::string path;
};

std::mutex gMu;
std::condition_variable gCv;
std::unordered_map<std::string, std::shared_ptr<Slot>> gCache;
std::mutex gResourceMu;

std::string ProduceHash(const std::string& hash, std::string& status) {
    std::string reason;
    std::vector<unsigned char> raw;
    Image img;
    bool fromCache = false;
    std::string cache = CacheFile(hash);
    if (std::filesystem::is_regular_file(cache) && ReadFileBytes(cache, raw) && DecodeSkinPng(raw, img, reason)) {
        fromCache = true;
    } else {
        raw.clear();
        img = Image();
        reason.clear();
        std::error_code ec;
        std::filesystem::remove(cache, ec);
        if (!config.downloadPlayerSkins) {
            status = "failed (downloads disabled)";
            return "";
        }
        if (!DownloadRaw(hash, raw, reason) || !DecodeSkinPng(raw, img, reason)) {
            status = "failed (" + (reason.empty() ? std::string("download failed") : reason) + ")";
            return "";
        }
        if (!WriteFileBytes(cache, raw)) {
            // The export texture can still be written; the next run will download again.
        }
    }
    Image processed = ProcessLegacySkin(std::move(img));
    std::string path = WriteExportTexture(hash, processed, reason);
    if (path.empty()) {
        status = "failed (" + (reason.empty() ? std::string("write failed") : reason) + ")";
        return "";
    }
    status = fromCache ? "cached" : "downloaded";
    return path;
}

void LogSkin(const std::string& hash, const std::string& status) {
    std::cout << "[PlayerSkin] " << hash << " " << status << std::endl;
    std::cout.flush();
}

std::string ResolveHash(const std::string& hash) {
    std::shared_ptr<Slot> slot;
    {
        std::unique_lock<std::mutex> lk(gMu);
        auto it = gCache.find(hash);
        if (it != gCache.end()) {
            slot = it->second;
            gCv.wait(lk, [&] { return slot->done; });
            return slot->path;
        }
        slot = std::make_shared<Slot>();
        gCache.emplace(hash, slot);
    }

    std::string path;
    std::string status;
    try {
        path = ProduceHash(hash, status);
    } catch (const std::exception& e) {
        path.clear();
        status = std::string("failed (") + e.what() + ")";
    } catch (...) {
        path.clear();
        status = "failed (exception)";
    }
    {
        std::lock_guard<std::mutex> lk(gMu);
        slot->path = path;
        slot->done = true;
        LogSkin(hash, status);
    }
    gCv.notify_all();
    return path;
}

}  // namespace

std::string ResolvePlayerSkinTexture(const NbtTagPtr& blockEntityNbt) {
    if (!blockEntityNbt) return "";
    NbtTagPtr profile = Child(blockEntityNbt, "profile");
    if (!profile) profile = Child(blockEntityNbt, "SkullOwner");
    if (!profile) return "";
    // Name-only profile / very old SkullOwner string: no texture payload.
    if (profile->type == TagType::STRING) return "";
    if (profile->type != TagType::COMPOUND) return "";

    // 1.21.9+ PlayerSkin.Patch "texture" overrides the downloaded skin.
    std::string overrideLoc = TagString(Child(profile, "texture"));
    if (!overrideLoc.empty()) {
        std::lock_guard<std::mutex> lk(gResourceMu);
        std::string saved = UseResourceTexture(overrideLoc);
        if (!saved.empty()) return saved;
    }

    std::string b64 = FindTexturesValue(profile);
    if (b64.empty()) return "";
    std::string json = Base64Decode(b64);
    if (json.empty()) return "";
    std::string hash = ExtractSkinHash(json);
    if (hash.empty()) return "";
    return ResolveHash(hash);
}
