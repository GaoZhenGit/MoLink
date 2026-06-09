#ifndef GITIGNORE_H
#define GITIGNORE_H

#include <string>
#include <vector>
#include <fstream>
#include <algorithm>
#include <cstdio>
#include <filesystem>

namespace fs = std::filesystem;

class GitignoreMatcher {
public:
    bool load(const std::string& gitignorePath);
    bool isIgnored(const std::string& filePath, bool isDir) const;
    bool hasRules() const { return !m_rules.empty(); }

private:
    struct Rule {
        std::string pattern;
        bool negate;
        bool dirOnly;
        bool anchored;
    };
    std::vector<Rule> m_rules;

    static bool wildmatch(const std::string& text, const std::string& pattern);
};

inline std::string findGitignore(const std::string& folderPath) {
    std::ifstream test(".gitignore");
    if (test.good()) return ".gitignore";

    std::string folderGi = folderPath;
    std::replace(folderGi.begin(), folderGi.end(), '\\', '/');
    while (!folderGi.empty() && folderGi.back() == '/')
        folderGi.pop_back();
    folderGi += "/.gitignore";

    std::ifstream test2(fs::u8path(folderGi));
    if (test2.good()) return folderGi;

    return "";
}

inline bool GitignoreMatcher::load(const std::string& gitignorePath) {
    m_rules.clear();
    std::ifstream f(fs::u8path(gitignorePath));
    if (!f.good()) return false;

    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;

        Rule rule;
        rule.negate = (line[0] == '!');
        if (rule.negate) line = line.substr(1);

        while (!line.empty() && line.back() == ' ') line.pop_back();
        if (line.empty()) continue;

        rule.dirOnly = (!line.empty() && line.back() == '/');
        if (rule.dirOnly) line.pop_back();

        if (!line.empty() && line[0] == '/') {
            line = line.substr(1);
            rule.anchored = true;
        } else {
            rule.anchored = (line.find('/') != std::string::npos);
        }

        rule.pattern = line;
        m_rules.push_back(rule);
    }
    return true;
}

inline bool GitignoreMatcher::wildmatch(const std::string& text,
                                         const std::string& pattern) {
    // Handle ** (match anything including /)
    if (pattern == "**") return true;

    // Simple recursive wildcard matching
    size_t ti = 0, pi = 0;
    size_t starPi = std::string::npos;
    size_t matchTi = 0;

    while (ti < text.size()) {
        if (pi < pattern.size() && pattern[pi] == '*') {
            // Handle **/ pattern
            if (pi + 2 < pattern.size() && pattern[pi+1] == '*' && pattern[pi+2] == '/') {
                std::string rest = pattern.substr(pi + 3);
                // Match from current position (zero ** match)
                if (wildmatch(text.substr(ti), rest)) return true;
                // Match deeper
                while (ti < text.size()) {
                    if (text[ti] == '/' && wildmatch(text.substr(ti + 1), rest))
                        return true;
                    ti++;
                }
                return false;
            }
            starPi = pi;
            matchTi = ti;
            pi++;
        } else if (pi < pattern.size() &&
                   (pattern[pi] == '?' ||
                    tolower(pattern[pi]) == tolower(text[ti]))) {
            ti++; pi++;
        } else if (starPi != std::string::npos) {
            pi = starPi + 1;
            matchTi++;
            ti = matchTi;
        } else {
            return false;
        }
    }
    while (pi < pattern.size() && pattern[pi] == '*') pi++;
    return pi == pattern.size();
}

inline bool GitignoreMatcher::isIgnored(const std::string& filePath,
                                         bool isDir) const {
    bool ignored = false;

    for (const auto& rule : m_rules) {
        if (rule.dirOnly && !isDir) continue;

        std::string target = filePath;
        // Normalize to forward slash
        std::string norm = filePath;
        std::replace(norm.begin(), norm.end(), '\\', '/');

        bool matches;
        if (rule.anchored) {
            matches = wildmatch(norm, rule.pattern);
        } else {
            // Try basename first, then full path
            size_t lastSlash = norm.find_last_of('/');
            std::string basename = (lastSlash != std::string::npos)
                ? norm.substr(lastSlash + 1) : norm;
            matches = wildmatch(basename, rule.pattern);
            if (!matches)
                matches = wildmatch(norm, rule.pattern);
        }

        if (matches) ignored = !rule.negate;
    }

    return ignored;
}

#endif
