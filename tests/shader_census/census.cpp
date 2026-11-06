// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// Offline shader census (M4 item 3): translate every shader exported by survey/shader_export.py
// with runtime::shader::TranslateShader (runtime/shader/shader_glsl.h) and validate the GLSL with
// glslangValidator -V (Vulkan 1.0) -> spirv-val --target-env vulkan1.0 -> spirv-cross --msl.
// CPU only: nothing here touches a GPU.
//
//   census <export dir> <work dir> <report.tsv> <summary.txt>
//          <glslangValidator> <spirv-val> <spirv-cross> <tool deadline seconds>
//
// Per shader, the first stage that fails names its outcome: refused (the translator), glslang,
// spirv-val, spirv-cross; ok when all pass. Each tool runs without a shell, in its own process
// group, killed (SIGKILL) at the deadline (a timeout is that stage's failure). A passing
// shader's files are removed at once; a failing one's GLSL, SPIR-V and tool log stay in the
// work dir. Fatal (exit 1, no report): an unreadable export, a file whose name, size or hash
// disagrees with its bytes, counts that disagree with index.json, a tool that cannot be started.
// Exit 0 when every shader was examined, whatever the outcomes.
//
// The dump holds the microcode as stored on the disc (big-endian dwords). The hash (FNV-1a-64,
// seed 0xcbf29ce484222325 ^ ShaderType, one step per dword loaded little-endian from those
// bytes: survey/fxobj.py shader_hash) is recomputed from the raw bytes and must equal the file
// name's; the translator gets the byte-swapped (host-order) words, as its header requires.
#include "shader/shader_glsl.h"

#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

extern char** environ;

namespace fs = std::filesystem;

namespace {

struct Fatal : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

struct Shader
{
    std::string stage;  // "vs" or "ps"
    std::string hash;   // 16 lowercase hex digits
    fs::path path;
};

struct Result
{
    std::string outcome, reason, key;
    runtime::shader::ShaderTranslation t;
    size_t dwords = 0, glslLines = 0;
};

std::string ReadFile(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    if (!f)
        throw Fatal("cannot read " + p.string());
    std::ostringstream s;
    s << f.rdbuf();
    return s.str();
}

void WriteFile(const fs::path& p, const std::string& data)
{
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f || !f.write(data.data(), std::streamsize(data.size())))
        throw Fatal("cannot write " + p.string());
}

uint64_t ShaderHash(const std::string& raw, uint32_t type)
{
    uint64_t h = 0xcbf29ce484222325ull ^ type;
    for (size_t i = 0; i < raw.size(); i += 4)
    {
        const auto* b = reinterpret_cast<const unsigned char*>(raw.data() + i);
        const uint32_t le = uint32_t(b[0]) | uint32_t(b[1]) << 8 | uint32_t(b[2]) << 16 | uint32_t(b[3]) << 24;
        h = (h ^ le) * 0x100000001b3ull;
    }
    return h;
}

std::vector<uint32_t> HostWords(const std::string& raw)
{
    std::vector<uint32_t> w(raw.size() / 4);
    for (size_t i = 0; i < w.size(); i++)
    {
        const auto* b = reinterpret_cast<const unsigned char*>(raw.data() + 4 * i);
        w[i] = uint32_t(b[0]) << 24 | uint32_t(b[1]) << 16 | uint32_t(b[2]) << 8 | uint32_t(b[3]);
    }
    return w;
}

std::vector<Shader> ListShaders(const fs::path& dir)
{
    static const std::regex name("(vs|ps)_([0-9a-f]{16})\\.bin");
    std::vector<Shader> out;
    for (const auto& e : fs::directory_iterator(dir))
    {
        const std::string n = e.path().filename().string();
        std::smatch m;
        if (std::regex_match(n, m, name))
            out.push_back({m[1], m[2], e.path()});
        else if (n != "index.json")
            throw Fatal("unexpected file in the export: " + n);
    }
    std::sort(out.begin(), out.end(), [](const Shader& a, const Shader& b) {
        return std::tie(a.stage, a.hash) < std::tie(b.stage, b.hash);
    });
    return out;
}

// index.json is written by json.dump(sort_keys=True): "counts": {"ps": N, "vs": M}.
void CheckCounts(const fs::path& dir, const std::vector<Shader>& shaders)
{
    static const std::regex counts("\"counts\":\\s*\\{\\s*\"ps\":\\s*(\\d+),\\s*\"vs\":\\s*(\\d+)\\s*\\}");
    const std::string index = ReadFile(dir / "index.json");
    std::smatch m;
    if (!std::regex_search(index, m, counts))
        throw Fatal("index.json has no counts");
    const size_t ps = std::count_if(shaders.begin(), shaders.end(), [](const Shader& s) { return s.stage == "ps"; });
    const size_t vs = shaders.size() - ps;
    if (std::stoul(m[1]) != ps || std::stoul(m[2]) != vs)
        throw Fatal("index.json counts ps " + std::string(m[1]) + " vs " + std::string(m[2]) + ", the export holds ps " +
                    std::to_string(ps) + " vs " + std::to_string(vs));
}

// Runs argv[0] with stdout and stderr to `log`, stdin from /dev/null, in its own process group.
// Returns "" on exit status 0, otherwise a short failure ("exit 1", "signal 11", "timeout 30 s").
std::string RunTool(const std::vector<std::string>& args, const fs::path& log, int deadlineSeconds)
{
    std::vector<char*> argv;
    for (const auto& a : args)
        argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    posix_spawn_file_actions_t fa;
    posix_spawnattr_t attr;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 1, log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    posix_spawn_file_actions_adddup2(&fa, 1, 2);
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attr, 0);
    pid_t pid = 0;
    const int rc = posix_spawn(&pid, argv[0], &fa, &attr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
    if (rc != 0)
        throw Fatal("cannot start " + args[0] + ": " + std::strerror(rc));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(deadlineSeconds);
    int status = 0;
    for (;;)
    {
        const pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid)
            break;
        if (r < 0 && errno != EINTR)
            throw Fatal("waitpid on " + args[0] + ": " + std::strerror(errno));
        if (std::chrono::steady_clock::now() >= deadline)
        {
            kill(-pid, SIGKILL);
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
            return "timeout " + std::to_string(deadlineSeconds) + " s";
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (WIFEXITED(status))
        return WEXITSTATUS(status) == 0 ? "" : "exit " + std::to_string(WEXITSTATUS(status));
    return "signal " + std::to_string(WIFSIGNALED(status) ? WTERMSIG(status) : 0);
}

std::string Trim(std::string s)
{
    s.erase(0, s.find_first_not_of(" \t\r\n"));
    s.erase(s.find_last_not_of(" \t\r\n") + 1);
    return s;
}

// The first log line that names an error (else the first non-empty line), with the work
// directory's paths removed, and tabs replaced so it fits a TSV cell.
std::string ToolReason(const fs::path& log, const fs::path& work)
{
    static const std::regex errorLine("error|exception", std::regex::icase);
    std::istringstream in(ReadFile(log));
    std::string line, first;
    while (std::getline(in, line))
    {
        line = Trim(line);
        // glslangValidator prints the input path alone on the first line.
        if (line.empty() || (line.rfind(work.string() + "/", 0) == 0 && line.find(' ') == std::string::npos))
            continue;
        if (first.empty())
            first = line;
        if (std::regex_search(line, errorLine))
        {
            first = line;
            break;
        }
    }
    for (size_t at; (at = first.find(work.string() + "/")) != std::string::npos;)
        first.erase(at, work.string().size() + 1);
    std::replace(first.begin(), first.end(), '\t', ' ');
    return first.empty() ? "(no output)" : first;
}

// Reason with locations and register numbers removed, so equal causes count together; opcode,
// format and filter numbers are kept.
std::string ReasonKey(const std::string& reason)
{
    static const std::vector<std::pair<std::regex, std::string>> rules = {
        {std::regex(" \\([A-Za-z_ ]+ at (slot|control-flow instruction) \\d+\\)"), ""},
        {std::regex(" \\(slot \\d+\\)"), ""},
        {std::regex("\\b(slot|instruction) \\d+"), "$1 N"},
        {std::regex("\\br\\d+\\b"), "rN"},
        {std::regex("(vs|ps)_[0-9a-f]{16}\\.(vert|frag|spv|metal)"), "<shader>"},
        {std::regex(":\\d+:"), ":N:"},
        {std::regex("\\bline \\d+"), "line N"},
        {std::regex("%\\d+"), "%N"},
    };
    std::string k = reason;
    for (const auto& [re, with] : rules)
        k = std::regex_replace(k, re, with);
    return k;
}

size_t CountLines(const std::string& s)
{
    return size_t(std::count(s.begin(), s.end(), '\n'));
}

struct Tools
{
    std::string glslang, spirvVal, spirvCross;
    int deadline;
};

Result Examine(const Shader& s, const fs::path& work, const Tools& tools)
{
    Result r;
    const std::string raw = ReadFile(s.path);
    const uint32_t type = s.stage == "ps" ? 1 : 0;
    if (raw.empty() || raw.size() % 12)
        throw Fatal(s.path.string() + ": " + std::to_string(raw.size()) + " bytes is not whole 3-dword slots");
    char hex[17];
    std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(ShaderHash(raw, type)));
    if (s.hash != hex)
        throw Fatal(s.path.string() + ": microcode hashes to " + hex);
    const std::vector<uint32_t> words = HostWords(raw);
    r.dwords = words.size();
    std::string error;
    if (!runtime::shader::TranslateShader(words.data(), words.size(), type == 1, r.t, error))
    {
        r.outcome = "refused";
        r.reason = error;
        std::replace(r.reason.begin(), r.reason.end(), '\t', ' ');
        r.key = ReasonKey(r.reason);
        return r;
    }
    r.glslLines = CountLines(r.t.glsl);
    const std::string base = s.stage + "_" + s.hash;
    const fs::path glsl = work / (base + (type ? ".frag" : ".vert"));
    const fs::path spv = work / (base + ".spv"), metal = work / (base + ".metal");
    WriteFile(glsl, r.t.glsl);
    struct Step
    {
        const char* outcome;
        std::vector<std::string> args;
    };
    const Step steps[] = {
        {"glslang", {tools.glslang, "-V", "-o", spv.string(), glsl.string()}},
        {"spirv-val", {tools.spirvVal, "--target-env", "vulkan1.0", spv.string()}},
        {"spirv-cross", {tools.spirvCross, spv.string(), "--msl", "--output", metal.string()}},
    };
    std::vector<fs::path> logs;
    for (const Step& step : steps)
    {
        const fs::path log = work / (base + "." + step.outcome + ".log");
        logs.push_back(log);
        const std::string failure = RunTool(step.args, log, tools.deadline);
        if (!failure.empty())
        {
            r.outcome = step.outcome;
            r.reason = failure + ": " + ToolReason(log, work);
            r.key = ReasonKey(r.reason);
            return r;  // the GLSL, SPIR-V and logs stay for diagnosis
        }
    }
    r.outcome = "ok";
    for (const fs::path& p : logs)
        fs::remove(p);
    for (const fs::path& p : {glsl, spv, metal})
        fs::remove(p);
    return r;
}

std::string Join(const std::vector<uint32_t>& v)
{
    std::string s;
    for (uint32_t x : v)
        s += (s.empty() ? "" : ",") + std::to_string(x);
    return s.empty() ? "-" : s;
}

int Run(int argc, char** argv)
{
    if (argc != 9)
    {
        std::fprintf(stderr, "usage: census <export dir> <work dir> <report.tsv> <summary.txt> "
                             "<glslangValidator> <spirv-val> <spirv-cross> <tool deadline seconds>\n");
        return 2;
    }
    const fs::path exportDir = argv[1], work = argv[2];
    const Tools tools{argv[5], argv[6], argv[7], std::stoi(argv[8])};
    if (tools.deadline <= 0)
        throw Fatal("tool deadline must be positive");
    if (!fs::is_directory(work) || !fs::is_empty(work))
        throw Fatal("work dir " + work.string() + " must be an existing empty directory");
    const std::vector<Shader> shaders = ListShaders(exportDir);
    if (shaders.empty())
        throw Fatal("no shader in " + exportDir.string());
    CheckCounts(exportDir, shaders);

    const std::vector<std::string> outcomes = {"ok", "refused", "glslang", "spirv-val", "spirv-cross"};
    std::map<std::string, std::map<std::string, size_t>> byStage;           // stage -> outcome -> n
    std::map<std::pair<std::string, std::string>, std::map<std::string, size_t>> byReason;  // (outcome, key) -> stage -> n
    std::ostringstream tsv;
    tsv << "stage\thash\tdwords\toutcome\treason_key\treason\tinterpolators\tvertex_fetch_slots\t"
           "texture_slots\tbool_constants\tloop_constants\tglsl_lines\n";
    for (const Shader& s : shaders)
    {
        const Result r = Examine(s, work, tools);
        byStage[s.stage][r.outcome]++;
        if (r.outcome != "ok")
            byReason[{r.outcome, r.key}][s.stage]++;
        const bool translated = r.outcome != "refused";
        tsv << s.stage << '\t' << s.hash << '\t' << r.dwords << '\t' << r.outcome << '\t'
            << (r.key.empty() ? "-" : r.key) << '\t' << (r.reason.empty() ? "-" : r.reason) << '\t';
        if (translated)
            tsv << r.t.interpolatorCount << '\t' << Join(r.t.vertexFetchSlots) << '\t' << Join(r.t.textureSlots) << '\t'
                << Join(r.t.boolConstants) << '\t' << Join(r.t.loopConstants) << '\t' << r.glslLines << '\n';
        else
            tsv << "-\t-\t-\t-\t-\t-\n";
    }

    std::ostringstream sum;
    sum << "shader census: " << shaders.size() << " distinct shaders (default GLSL variant only)\n\n";
    char line[256];
    std::snprintf(line, sizeof line, "%-6s %6s %6s %6s %8s %10s %12s\n", "stage", "total", "ok", "refused", "glslang",
                  "spirv-val", "spirv-cross");
    sum << line;
    for (const char* stage : {"vs", "ps"})
    {
        auto& o = byStage[stage];
        size_t total = 0;
        for (const auto& name : outcomes)
            total += o[name];
        std::snprintf(line, sizeof line, "%-6s %6zu %6zu %6zu %8zu %10zu %12zu\n", stage, total, o["ok"], o["refused"],
                      o["glslang"], o["spirv-val"], o["spirv-cross"]);
        sum << line;
    }
    std::vector<std::pair<size_t, std::pair<std::string, std::string>>> reasons;
    for (auto& [k, st] : byReason)
        reasons.push_back({st["vs"] + st["ps"], k});
    std::sort(reasons.begin(), reasons.end(), [](const auto& a, const auto& b) {
        return a.first != b.first ? a.first > b.first : a.second < b.second;
    });
    sum << "\nfailures by reason (first failure per shader; count, vs, ps, outcome, reason):\n";
    for (const auto& [n, k] : reasons)
    {
        auto& st = byReason[k];
        std::snprintf(line, sizeof line, "%5zu %4zu %4zu  %-11s ", n, st["vs"], st["ps"], k.first.c_str());
        sum << line << k.second << '\n';
    }
    if (reasons.empty())
        sum << "  (none)\n";
    WriteFile(argv[3], tsv.str());
    WriteFile(argv[4], sum.str());
    std::fputs(sum.str().c_str(), stdout);
    return 0;
}

}  // namespace

int main(int argc, char** argv)
{
    try
    {
        return Run(argc, argv);
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "FATAL: %s\n", e.what());
        return 1;
    }
}
