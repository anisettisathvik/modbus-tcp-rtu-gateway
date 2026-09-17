#pragma once
#include <string>
#include <map>
#include <vector>
#include <fstream>
#include <sstream>
#include <cstdint>
#include <cstdlib>

namespace mb {

// ---------------------------------------------------------------------------
// Configuration file: plain key = value with # comments.
//
// Every key has a command-line override, since commissioning usually means
// trying one setting without editing the file.
// gateway is configured by a technician over a serial console, not by editing
// YAML. Every key also has a command-line override, because the commissioning
// case is "try one setting without touching the file".
// ---------------------------------------------------------------------------

class Config {
public:
    bool load(const std::string& path, std::string& err)
    {
        std::ifstream f(path);
        if (!f) { err = "cannot open " + path; return false; }

        std::string line;
        int lineno = 0;
        while (std::getline(f, line)) {
            lineno++;
            const auto hash = line.find('#');
            if (hash != std::string::npos) line.resize(hash);

            const auto eq = line.find('=');
            if (eq == std::string::npos) {
                if (line.find_first_not_of(" \t\r\n") != std::string::npos) {
                    err = path + ":" + std::to_string(lineno) + ": expected key = value";
                    return false;
                }
                continue;
            }
            kv_[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
        }
        return true;
    }

    bool has(const std::string& k) const { return kv_.count(k) != 0; }

    std::string str(const std::string& k, const std::string& dflt) const
    {
        const auto it = kv_.find(k);
        return it == kv_.end() ? dflt : it->second;
    }

    unsigned num(const std::string& k, unsigned dflt) const
    {
        const auto it = kv_.find(k);
        if (it == kv_.end()) return dflt;
        return static_cast<unsigned>(std::strtoul(it->second.c_str(), nullptr, 0));
    }

    double real(const std::string& k, double dflt) const
    {
        const auto it = kv_.find(k);
        return it == kv_.end() ? dflt : std::strtod(it->second.c_str(), nullptr);
    }

    // "17,34,51" -> {17,34,51}
    std::vector<std::uint8_t> units(const std::string& k,
                                    const std::vector<std::uint8_t>& dflt) const
    {
        const auto it = kv_.find(k);
        if (it == kv_.end()) return dflt;
        std::vector<std::uint8_t> v;
        std::stringstream ss(it->second);
        std::string piece;
        while (std::getline(ss, piece, ',')) {
            const auto t = trim(piece);
            if (!t.empty())
                v.push_back(static_cast<std::uint8_t>(std::strtoul(t.c_str(), nullptr, 0)));
        }
        return v.empty() ? dflt : v;
    }

    const std::map<std::string, std::string>& all() const { return kv_; }

private:
    static std::string trim(const std::string& s)
    {
        const auto b = s.find_first_not_of(" \t\r\n");
        if (b == std::string::npos) return "";
        const auto e = s.find_last_not_of(" \t\r\n");
        return s.substr(b, e - b + 1);
    }
    std::map<std::string, std::string> kv_;
};

} // namespace mb
