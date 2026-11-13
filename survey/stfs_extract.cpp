// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// survey/stfs_extract.py in C++, so the player's setup needs no Python (no Homebrew or developer
// tools on the player's Mac). Unpacks a read-only STFS package (Disc 2's PIRS content packages) into a directory with every
// table and data block checked against the package's SHA-1 hash tree, and writes its header.json. Both outputs must be
// byte-identical to the Python tool's, mtimes included (tests/test_stfs_extract.py CxxParity compares them).
//   stfs_extract <package> <out dir> <out header.json>     exit 0 done, 1 refused (a message names why), 2 usage
// Neither output may exist. Layout, hash tree, timestamps and every refusal as survey/stfs_extract.py's header, which cites
// the sources (Xenia canary 67f16c4e315f via survey/stfsinfo.py; ShiftGlue 72b7dd6f stfs_container_device.cpp:539-788 and
// stfs_xbox.h:32-54, 83-195; Velocity 1690428a StfsPackage.cpp HashBlock/Rehash/CalcualateTopLevel, facts only).
// SHA-1: TinySHA1 (XenonRecomp thirdparty, on setup_tools.sh's include path; its notice is kept in that header).
#include <TinySHA1.hpp>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <map>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {
constexpr uint64_t kBlock = 0x1000;
constexpr uint32_t kPerLevel[3] = {170, 28900, 4913000};
constexpr uint32_t kTopLimits[3] = {0xAA, 0x70E4, 0x4AF768};
constexpr uint32_t kEndOfChain = 0xFFFFFF;
constexpr size_t kHeaderMin = 0x1712;
constexpr size_t kVolume = 0x379, kExec = 0x354;

struct Error : std::runtime_error { using std::runtime_error::runtime_error; };

std::string Format(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
std::string Format(const char* fmt, ...)
{
    char buf[512];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, args);
    va_end(args);
    return buf;
}

uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint64_t Be64(const uint8_t* p) { return uint64_t(Be32(p)) << 32 | Be32(p + 4); }
uint32_t U24le(const uint8_t* p) { return p[0] | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16; }

// --- JSON exactly as Python's json.dump(indent=1, sort_keys=True) with ensure_ascii, then "\n".
struct Json
{
    enum Kind { kInt, kBool, kString, kArray, kObject } kind = kInt;
    int64_t i = 0;
    bool b = false;
    std::u32string s;  // code points
    std::vector<Json> items;
    std::map<std::string, Json> fields;  // ASCII keys: std::map order is Python's sorted() order
    static Json Int(int64_t v) { Json j; j.kind = kInt; j.i = v; return j; }
    static Json Bool(bool v) { Json j; j.kind = kBool; j.b = v; return j; }
    static Json Str(std::u32string v) { Json j; j.kind = kString; j.s = std::move(v); return j; }
    static Json Ascii(const std::string& v) { return Str(std::u32string(v.begin(), v.end())); }
    static Json Array() { Json j; j.kind = kArray; return j; }
    static Json Object() { Json j; j.kind = kObject; return j; }
};

void EscapeUnit(std::string& out, uint32_t u) { out += Format("\\u%04x", u); }

void EmitString(std::string& out, const std::u32string& s)
{
    out += '"';
    for (char32_t c : s)
    {
        switch (c)
        {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        default:
            if (c >= 0x20 && c <= 0x7E) out += char(c);
            else if (c >= 0x10000)
            {
                uint32_t v = c - 0x10000;
                EscapeUnit(out, 0xD800 | (v >> 10));
                EscapeUnit(out, 0xDC00 | (v & 0x3FF));
            }
            else EscapeUnit(out, c);
        }
    }
    out += '"';
}

void Emit(std::string& out, const Json& j, int depth)
{
    std::string pad(depth + 1, ' ');
    switch (j.kind)
    {
    case Json::kInt: out += std::to_string(j.i); break;
    case Json::kBool: out += j.b ? "true" : "false"; break;
    case Json::kString: EmitString(out, j.s); break;
    case Json::kArray:
        if (j.items.empty()) { out += "[]"; break; }
        out += "[";
        for (size_t n = 0; n < j.items.size(); ++n)
        {
            out += n ? ",\n" : "\n";
            out += pad;
            Emit(out, j.items[n], depth + 1);
        }
        out += "\n" + std::string(depth, ' ') + "]";
        break;
    case Json::kObject:
        if (j.fields.empty()) { out += "{}"; break; }
        out += "{";
        bool first = true;
        for (const auto& [key, value] : j.fields)
        {
            out += first ? "\n" : ",\n";
            first = false;
            out += pad;
            EmitString(out, std::u32string(key.begin(), key.end()));
            out += ": ";
            Emit(out, value, depth + 1);
        }
        out += "\n" + std::string(depth, ' ') + "}";
        break;
    }
}

// survey/stfsinfo.py utf16(): UTF-16BE with Python's errors="replace" (a lone surrogate, one code unit, becomes U+FFFD),
// cut at the first NUL.
std::u32string Utf16(const uint8_t* p, size_t bytes)
{
    std::u32string out;
    for (size_t n = 0; n + 1 < bytes; n += 2)
    {
        uint32_t u = uint32_t(p[n]) << 8 | p[n + 1];
        if (u >= 0xD800 && u <= 0xDBFF && n + 3 < bytes)
        {
            uint32_t low = uint32_t(p[n + 2]) << 8 | p[n + 3];
            if (low >= 0xDC00 && low <= 0xDFFF)
            {
                out += char32_t(0x10000 + ((u - 0xD800) << 10) + (low - 0xDC00));
                n += 2;
                continue;
            }
        }
        out += (u >= 0xD800 && u <= 0xDFFF) ? char32_t(0xFFFD) : char32_t(u);
    }
    size_t nul = out.find(char32_t(0));
    return nul == std::u32string::npos ? out : out.substr(0, nul);
}

const char* ContentTypeName(uint32_t t)  // survey/stfsinfo.py CONTENT_TYPES
{
    static const std::map<uint32_t, const char*> names = {
        {0x00000001, "SavedGame"}, {0x00000002, "MarketplaceContent"}, {0x00000003, "Publisher"},
        {0x00001000, "Xbox360Title"}, {0x00002000, "IptvPauseBuffer"}, {0x00003000, "XNACommunity"},
        {0x00004000, "InstalledGame"}, {0x00005000, "XboxTitle"}, {0x00006000, "SocialTitle"},
        {0x00007000, "GamesOnDemand"}, {0x00008000, "SUStoragePack"}, {0x00009000, "AvatarItem"},
        {0x00010000, "Profile"}, {0x00020000, "GamerPicture"}, {0x00030000, "Theme"},
        {0x00040000, "CacheFile"}, {0x00050000, "StorageDownload"}, {0x00060000, "XboxSavedGame"},
        {0x00070000, "XboxDownload"}, {0x00080000, "GameDemo"}, {0x00090000, "Video"},
        {0x000A0000, "GameTitle"}, {0x000B0000, "Installer"}, {0x000C0000, "GameTrailer"},
        {0x000D0000, "ArcadeTitle"}, {0x000E0000, "XNA"}, {0x000F0000, "LicenseStore"},
        {0x00100000, "Movie"}, {0x00200000, "TV"}, {0x00300000, "MusicVideo"}, {0x00400000, "GameVideo"},
        {0x00500000, "PodcastVideo"}, {0x00600000, "ViralVideo"}, {0x02000000, "CommunityGame"},
    };
    auto it = names.find(t);
    return it == names.end() ? "unknown" : it->second;
}

struct Entry
{
    std::string name, path;
    bool directory;
    uint32_t parent;  // 0xFFFF: the root
    uint32_t length, allocated, start, created, modified;
};

struct Package
{
    int fd = -1;
    std::vector<uint8_t> h;
    uint64_t base = 0;
    uint32_t total = 0;
    int top = 0;
    uint8_t topHash[20];
    uint32_t step[2];
    std::map<std::pair<int, uint32_t>, std::vector<uint8_t>> verified;

    std::vector<uint8_t> Read(uint64_t offset, size_t size)
    {
        std::vector<uint8_t> data(size);
        size_t got = 0;
        while (got < size)
        {
            ssize_t r = pread(fd, data.data() + got, size - got, off_t(offset + got));
            if (r < 0 && errno == EINTR) continue;
            if (r < 0) throw Error(Format("read at 0x%llX: %s", (unsigned long long)offset, std::strerror(errno)));
            if (r == 0)
                throw Error(Format("read of 0x%zX bytes at 0x%llX past end of package", size, (unsigned long long)offset));
            got += size_t(r);
        }
        return data;
    }

    // survey/stfsinfo.py Stfs.block_to_offset / hash_block_number with one table per level (read-only).
    uint64_t BlockToOffset(uint32_t index) const
    {
        uint64_t block = index;
        for (uint32_t levelBase : kPerLevel)
        {
            block += (uint64_t(index) + levelBase) / levelBase;
            if (index < levelBase) break;
        }
        return base + (block << 12);
    }

    uint32_t HashBlockNumber(uint32_t index, int level) const
    {
        if (level == 2) return step[1];
        if (index < kPerLevel[level]) return level == 0 ? 0 : step[level - 1];
        uint32_t block = (index / kPerLevel[level]) * step[level];
        if (level == 0)
        {
            block += (index / kPerLevel[1]) + 1;
            if (index < kPerLevel[1]) return block;
        }
        return block + 1;
    }

    static bool Sha1Matches(const std::vector<uint8_t>& data, const uint8_t* want)
    {
        sha1::SHA1 s;
        s.processBytes(data.data(), data.size());
        uint8_t digest[20];
        s.finalize(digest);
        return std::memcmp(digest, want, 20) == 0;
    }

    const std::vector<uint8_t>& Table(int level, uint32_t number)
    {
        auto key = std::make_pair(level, number);
        auto it = verified.find(key);
        if (it != verified.end()) return it->second;
        const uint8_t* want;
        std::vector<uint8_t> parentCopy;
        if (level == top)
        {
            if (number != 0) throw Error(Format("level-%d table %u past the top table", level, number));
            want = topHash;
        }
        else
        {
            parentCopy = Table(level + 1, number / kPerLevel[0]);
            want = parentCopy.data() + (number % kPerLevel[0]) * 0x18;
        }
        uint32_t physical = HashBlockNumber(uint32_t(uint64_t(number) * kPerLevel[level]), level);
        std::vector<uint8_t> data = Read(base + (uint64_t(physical) << 12), kBlock);
        if (!Sha1Matches(data, want))
            throw Error(Format("level-%d hash table %u (physical block %u) does not match its hash", level, number, physical));
        return verified.emplace(key, std::move(data)).first->second;
    }

    const uint8_t* HashEntry(uint32_t index)
    {
        if (index >= total) throw Error(Format("data block %u is past the volume (%u blocks)", index, total));
        return Table(0, index / kPerLevel[0]).data() + (index % kPerLevel[0]) * 0x18;
    }

    std::vector<uint8_t> DataBlock(uint32_t index)
    {
        uint8_t want[20];
        std::memcpy(want, HashEntry(index), 20);
        std::vector<uint8_t> data = Read(BlockToOffset(index), kBlock);
        if (!Sha1Matches(data, want)) throw Error(Format("data block %u does not match its level-0 hash", index));
        return data;
    }

    uint32_t NextBlock(uint32_t index) { return Be32(HashEntry(index) + 0x14) & 0xFFFFFF; }
};

bool SafeName(const std::string& name)
{
    if (name.empty() || name == "." || name == ".." || name.size() > 40) return false;
    for (unsigned char c : name)
        if (c < 0x20 || c > 0x7E || c == '/' || c == '\\' || c == ':') return false;
    return true;
}

// stfs_extract.py fat_time(): -1 unless a real date and time.
time_t FatTime(uint32_t value)
{
    uint32_t date = value >> 16, time = value & 0xFFFF;
    int month = (date & 0x01E0) >> 5, day = date & 0x001F;
    int hour = (time & 0xF800) >> 11, minute = (time & 0x07E0) >> 5, second = (time & 0x001F) << 1;
    if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 || second > 59) return -1;
    struct tm tm = {};
    tm.tm_year = int((date & 0xFE00) >> 9) + 80;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = minute;
    tm.tm_sec = second;
    return timegm(&tm);
}

bool Exists(const std::string& path)
{
    struct stat st;
    return lstat(path.c_str(), &st) == 0;
}

void MakeDirs(const std::string& path)  // mkdir -p
{
    for (size_t at = path.find('/', 1); ; at = path.find('/', at + 1))
    {
        std::string part = at == std::string::npos ? path : path.substr(0, at);
        if (mkdir(part.c_str(), 0755) != 0 && errno != EEXIST) throw Error(Format("mkdir %s: %s", part.c_str(), std::strerror(errno)));
        if (at == std::string::npos) return;
    }
}

void WriteAll(int fd, const uint8_t* p, size_t n, const std::string& path)
{
    while (n)
    {
        ssize_t w = write(fd, p, n);
        if (w < 0 && errno == EINTR) continue;
        if (w < 0) throw Error(Format("write %s: %s", path.c_str(), std::strerror(errno)));
        p += w;
        n -= size_t(w);
    }
}

int CreateExclusive(const std::string& path)
{
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) throw Error(Format("create %s: %s", path.c_str(), std::strerror(errno)));
    return fd;
}

struct Summary { size_t files = 0, directories = 0; uint64_t bytes = 0; };

Summary Extract(const std::string& packagePath, const std::string& outDir, const std::string& outJson)
{
    for (const std::string& out : {outDir, outJson})
        if (Exists(out)) throw Error(out + " exists (never overwritten)");
    Package pkg;
    pkg.fd = open(packagePath.c_str(), O_RDONLY);
    if (pkg.fd < 0) throw Error(Format("open: %s", std::strerror(errno)));
    struct stat st;
    if (fstat(pkg.fd, &st) != 0) throw Error(Format("stat: %s", std::strerror(errno)));
    if (uint64_t(st.st_size) < kHeaderMin) throw Error(Format("file shorter than an XContent header (0x%zX bytes)", kHeaderMin));
    pkg.h = pkg.Read(0, kHeaderMin);
    const uint8_t* h = pkg.h.data();
    std::string magic(reinterpret_cast<const char*>(h), 4);
    if (magic != "CON " && magic != "LIVE" && magic != "PIRS") throw Error("not an XContent package");
    if (Be32(h + 0x3A9) != 0) throw Error(Format("volume type %u, not STFS", Be32(h + 0x3A9)));
    const uint8_t* v = h + kVolume;
    if (v[0] != 0x24) throw Error(Format("STFS volume descriptor length 0x%X, want 0x24", v[0]));
    if (!(v[2] & 1)) throw Error("not a read-only STFS package (two tables per level are not supported)");
    uint32_t headerSize = Be32(h + 0x340);
    pkg.base = (uint64_t(headerSize) + kBlock - 1) & ~(kBlock - 1);
    pkg.total = Be32(v + 0x1C);
    pkg.top = -1;
    for (int level = 0; level < 3; ++level)
        if (pkg.total <= kTopLimits[level]) { pkg.top = level; break; }
    if (pkg.top < 0) throw Error(Format("total block count %u exceeds a level-2 hash tree", pkg.total));
    std::memcpy(pkg.topHash, v + 8, 20);
    pkg.step[0] = kPerLevel[0] + 1;
    pkg.step[1] = kPerLevel[1] + (kPerLevel[0] + 1);
    uint32_t tableCount = v[3] | uint32_t(v[4]) << 8;
    uint32_t tableBlock = U24le(v + 5);

    // stfsinfo.py Stfs.entries(), every block verified.
    std::vector<Entry> entries;
    for (uint32_t n = 0; n < tableCount; ++n)
    {
        std::vector<uint8_t> block = pkg.DataBlock(tableBlock);
        for (size_t m = 0; m < kBlock / 0x40; ++m)
        {
            const uint8_t* e = block.data() + m * 0x40;
            if (e[0] == 0) break;
            Entry entry;
            entry.name.assign(reinterpret_cast<const char*>(e), e[40] & 0x3F);
            entry.directory = (e[40] & 0x80) != 0;
            entry.parent = uint32_t(e[50]) << 8 | e[51];
            entry.length = Be32(e + 52);
            entry.allocated = U24le(e + 44);
            entry.start = U24le(e + 47);
            entry.created = Be32(e + 56);
            entry.modified = Be32(e + 60);
            entries.push_back(entry);
        }
        if (n + 1 == tableCount) break;
        tableBlock = pkg.NextBlock(tableBlock);
        if (tableBlock == kEndOfChain) throw Error(Format("file table chain ends after %u of %u blocks", n + 1, tableCount));
    }
    for (const Entry& e : entries)
        if (e.parent != 0xFFFF && !(e.parent < entries.size() && entries[e.parent].directory))
            throw Error("entry " + e.name + " names a parent that is not a directory");
    // stfs_extract.py plan(): names, then paths (cycles) and case-insensitive duplicates.
    for (const Entry& e : entries)
        if (!SafeName(e.name)) throw Error("unsafe entry name");
    std::map<std::string, std::string> seen;
    for (Entry& e : entries)
    {
        std::vector<std::string> parts{e.name};
        for (const Entry* p = &e; p->parent != 0xFFFF;)
        {
            p = &entries[p->parent];
            parts.push_back(p->name);
            if (parts.size() > entries.size()) throw Error("directory entries form a cycle at " + p->name);
        }
        for (auto it = parts.rbegin(); it != parts.rend(); ++it) e.path += (e.path.empty() ? "" : "/") + *it;
        std::string folded = e.path;
        std::transform(folded.begin(), folded.end(), folded.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        if (!seen.emplace(folded, e.path).second) throw Error("duplicate path " + e.path + " (case-insensitive)");
    }

    if (mkdir(outDir.c_str(), 0755) != 0) throw Error(Format("mkdir %s: %s", outDir.c_str(), std::strerror(errno)));
    for (const Entry& e : entries)
        if (e.directory) MakeDirs(outDir + "/" + e.path);
    Summary sum;
    for (const Entry& e : entries)
    {
        if (e.directory) { ++sum.directories; continue; }
        uint32_t blocks = uint32_t((uint64_t(e.length) + kBlock - 1) / kBlock);
        if (blocks != e.allocated)
            throw Error(Format("%s: %u bytes need %u blocks, the entry allocates %u", e.path.c_str(), e.length, blocks, e.allocated));
        std::string target = outDir + "/" + e.path;
        size_t slash = target.rfind('/');
        MakeDirs(target.substr(0, slash));
        int out = CreateExclusive(target);
        uint32_t index = e.start;
        uint64_t left = e.length;
        try
        {
            for (uint32_t n = 0; n < blocks; ++n)
            {
                if (index == kEndOfChain) throw Error(Format("%s: chain ends after %u of %u blocks", e.path.c_str(), n, blocks));
                std::vector<uint8_t> data = pkg.DataBlock(index);
                WriteAll(out, data.data(), size_t(std::min<uint64_t>(kBlock, left)), target);
                left -= std::min<uint64_t>(kBlock, left);
                if (n + 1 < blocks) index = pkg.NextBlock(index);
            }
        }
        catch (...)
        {
            close(out);
            throw;
        }
        if (close(out) != 0) throw Error(Format("close %s: %s", target.c_str(), std::strerror(errno)));
        ++sum.files;
        sum.bytes += e.length;
    }
    // Deepest first, stable (Python's sorted() by -depth).
    std::vector<const Entry*> order;
    for (const Entry& e : entries) order.push_back(&e);
    std::stable_sort(order.begin(), order.end(), [](const Entry* a, const Entry* b) {
        return std::count(a->path.begin(), a->path.end(), '/') > std::count(b->path.begin(), b->path.end(), '/');
    });
    for (const Entry* e : order)
    {
        time_t when = FatTime(e->modified);
        if (when == -1) continue;
        struct timeval times[2] = {{when, 0}, {when, 0}};
        std::string path = outDir + "/" + e->path;
        if (utimes(path.c_str(), times) != 0) throw Error(Format("utimes %s: %s", path.c_str(), std::strerror(errno)));
    }

    // stfsinfo.py header() + describe(): the --json document.
    Json info = Json::Object();
    auto& f = info.fields;
    f["magic"] = Json::Ascii(magic == "CON " ? "CON" : magic);
    f["header_size"] = Json::Int(headerSize);
    f["content_type"] = Json::Int(Be32(h + 0x344));
    f["content_type_name"] = Json::Ascii(ContentTypeName(Be32(h + 0x344)));
    f["metadata_version"] = Json::Int(Be32(h + 0x348));
    f["content_size"] = Json::Int(int64_t(Be64(h + 0x34C)));
    f["media_id"] = Json::Int(Be32(h + kExec));
    f["title_id"] = Json::Int(Be32(h + kExec + 0xC));
    f["disc_number"] = Json::Int(h[kExec + 0x12]);
    f["disc_count"] = Json::Int(h[kExec + 0x13]);
    f["data_file_count"] = Json::Int(Be32(h + 0x39D));
    f["data_file_size"] = Json::Int(int64_t(Be64(h + 0x3A1)));
    f["volume_type"] = Json::Ascii("STFS");
    f["display_name"] = Json::Str(Utf16(h + 0x411, 256));
    f["description"] = Json::Str(Utf16(h + 0xD11, 256));
    f["publisher"] = Json::Str(Utf16(h + 0x1611, 128));
    f["title_name"] = Json::Str(Utf16(h + 0x1691, 128));
    Json stfs = Json::Object();
    stfs.fields["descriptor_length"] = Json::Int(v[0]);
    stfs.fields["version"] = Json::Int(v[1]);
    stfs.fields["read_only_format"] = Json::Bool(v[2] & 1);
    stfs.fields["root_active_index"] = Json::Bool(v[2] & 2);
    stfs.fields["file_table_block_count"] = Json::Int(tableCount);
    stfs.fields["file_table_block_number"] = Json::Int(U24le(v + 5));
    std::string hex;
    for (int n = 0; n < 20; ++n) hex += Format("%02x", v[8 + n]);
    stfs.fields["top_hash_table_hash"] = Json::Ascii(hex);
    stfs.fields["total_block_count"] = Json::Int(Be32(v + 0x1C));
    stfs.fields["free_block_count"] = Json::Int(Be32(v + 0x20));
    f["stfs"] = stfs;
    Json files = Json::Array(), dirs = Json::Array();
    for (const Entry& e : entries)
    {
        Json row = Json::Object();
        row.fields["path"] = Json::Ascii(e.path);
        row.fields["created_fat"] = Json::Int(e.created);
        row.fields["modified_fat"] = Json::Int(e.modified);
        if (!e.directory) row.fields["length"] = Json::Int(e.length);
        (e.directory ? dirs : files).items.push_back(row);
    }
    f["file_count"] = Json::Int(int64_t(sum.files));
    f["directory_count"] = Json::Int(int64_t(sum.directories));
    f["files_total_bytes"] = Json::Int(int64_t(sum.bytes));
    f["files"] = files;
    f["directories"] = dirs;
    std::string text;
    Emit(text, info, 0);
    text += "\n";
    int out = CreateExclusive(outJson);
    WriteAll(out, reinterpret_cast<const uint8_t*>(text.data()), text.size(), outJson);
    if (close(out) != 0) throw Error(Format("close %s: %s", outJson.c_str(), std::strerror(errno)));
    close(pkg.fd);
    return sum;
}
}  // namespace

int main(int argc, char** argv)
{
    if (argc != 4)
    {
        std::fprintf(stderr, "usage: stfs_extract <package> <out dir> <out header.json>\n");
        return 2;
    }
    try
    {
        Summary s = Extract(argv[1], argv[2], argv[3]);
        std::printf("stfs_extract: %s: %zu files (%llu bytes) in %zu directories, every block verified\n", argv[1], s.files,
                    (unsigned long long)s.bytes, s.directories);
        return 0;
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "stfs_extract: %s: %s\n", argv[1], e.what());
        return 1;
    }
}
