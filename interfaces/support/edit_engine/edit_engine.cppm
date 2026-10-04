export module pi.support.edit_engine;

import std;
export import pi.types.applied_edits;
export import pi.types.fuzzy_match;
export import pi.types.matched_edit;
export import pi.types.result;
export import pi.types.text_edit;

/**
 * Text editing core of the edit tool (port of core/tools/edit-diff.ts). All edits are matched
 * against the same original content, then applied back to front. When any edit needs fuzzy
 * matching the work happens in normalized space and unchanged line blocks keep their original
 * bytes. Normalization is an approximation of NFKC: smart quotes, dashes, unicode spaces,
 * full-width ASCII, ellipsis and fi/fl ligatures, plus trailing-whitespace stripping.
 */
export class EditEngine {
public:
    /** "\r\n" when the first line break is CRLF, otherwise "\n". */
    std::string detectLineEnding(const std::string& content) const {
        const std::size_t crlf = content.find("\r\n");
        const std::size_t lf = content.find('\n');
        if (lf == std::string::npos || crlf == std::string::npos) {
            return "\n";
        }
        return crlf < lf ? "\r\n" : "\n";
    }

    std::string normalizeToLF(const std::string& text) const {
        return replaceAll(replaceAll(text, "\r\n", "\n"), "\r", "\n");
    }

    std::string restoreLineEndings(const std::string& text, const std::string& ending) const {
        return ending == "\r\n" ? replaceAll(text, "\n", "\r\n") : text;
    }

    std::string normalizeForFuzzyMatch(const std::string& text) const {
        std::string mapped;
        mapped.reserve(text.size());
        for (std::size_t i = 0; i < text.size();) {
            const auto c = static_cast<unsigned char>(text[i]);
            std::size_t length = 1;
            unsigned int cp = c;
            if (c >= 0xF0 && i + 3 < text.size()) {
                length = 4;
                cp = ((c & 0x07u) << 18) | ((static_cast<unsigned char>(text[i + 1]) & 0x3Fu) << 12) |
                     ((static_cast<unsigned char>(text[i + 2]) & 0x3Fu) << 6) |
                     (static_cast<unsigned char>(text[i + 3]) & 0x3Fu);
            } else if (c >= 0xE0 && i + 2 < text.size()) {
                length = 3;
                cp = ((c & 0x0Fu) << 12) | ((static_cast<unsigned char>(text[i + 1]) & 0x3Fu) << 6) |
                     (static_cast<unsigned char>(text[i + 2]) & 0x3Fu);
            } else if (c >= 0xC0 && i + 1 < text.size()) {
                length = 2;
                cp = ((c & 0x1Fu) << 6) | (static_cast<unsigned char>(text[i + 1]) & 0x3Fu);
            }
            if (length == 1 && c >= 0x80) {
                mapped.push_back(text[i]);
            } else {
                mapCodePoint(cp, mapped);
            }
            i += length;
        }
        std::string out;
        out.reserve(mapped.size());
        std::size_t lineStart = 0;
        while (lineStart <= mapped.size()) {
            std::size_t lineEnd = mapped.find('\n', lineStart);
            const bool last = lineEnd == std::string::npos;
            if (last) {
                lineEnd = mapped.size();
            }
            std::size_t trimmed = lineEnd;
            while (trimmed > lineStart && std::string_view(" \t\r\f\v").find(mapped[trimmed - 1]) != std::string_view::npos) {
                --trimmed;
            }
            out.append(mapped, lineStart, trimmed - lineStart);
            if (last) {
                break;
            }
            out.push_back('\n');
            lineStart = lineEnd + 1;
        }
        return out;
    }

    /** Splits a leading UTF-8 BOM: (bom, rest). */
    std::pair<std::string, std::string> splitBom(const std::string& text) const {
        if (text.rfind("\xEF\xBB\xBF", 0) == 0) {
            return {"\xEF\xBB\xBF", text.substr(3)};
        }
        return {"", text};
    }

    FuzzyMatch fuzzyFindText(const std::string& content, const std::string& oldText) const {
        FuzzyMatch match;
        const std::size_t exact = content.find(oldText);
        if (exact != std::string::npos) {
            match.found = true;
            match.index = exact;
            match.matchLength = oldText.size();
            match.contentForReplacement = content;
            return match;
        }
        const std::string fuzzyContent = normalizeForFuzzyMatch(content);
        const std::string fuzzyOld = normalizeForFuzzyMatch(oldText);
        const std::size_t fuzzy = fuzzyContent.find(fuzzyOld);
        match.contentForReplacement = content;
        if (fuzzy == std::string::npos) {
            return match;
        }
        match.found = true;
        match.index = fuzzy;
        match.matchLength = fuzzyOld.size();
        match.usedFuzzyMatch = true;
        match.contentForReplacement = fuzzyContent;
        return match;
    }

    Result<AppliedEdits> applyEdits(const std::string& normalizedContent, const std::vector<TextEdit>& edits, const std::string& path) const {
        std::vector<TextEdit> normalized;
        for (const TextEdit& edit : edits) {
            normalized.push_back({normalizeToLF(edit.oldText), normalizeToLF(edit.newText)});
        }
        for (std::size_t i = 0; i < normalized.size(); ++i) {
            if (normalized[i].oldText.empty()) {
                return std::unexpected(Error{
                    "edit_empty", normalized.size() == 1
                                      ? "oldText must not be empty in " + path + "."
                                      : "edits[" + std::to_string(i) + "].oldText must not be empty in " + path + "."});
            }
        }
        bool usedFuzzy = false;
        for (const TextEdit& edit : normalized) {
            usedFuzzy = usedFuzzy || fuzzyFindText(normalizedContent, edit.oldText).usedFuzzyMatch;
        }
        const std::string base = usedFuzzy ? normalizeForFuzzyMatch(normalizedContent) : normalizedContent;
        std::vector<MatchedEdit> matched;
        for (std::size_t i = 0; i < normalized.size(); ++i) {
            const FuzzyMatch match = fuzzyFindText(base, normalized[i].oldText);
            if (!match.found) {
                return std::unexpected(Error{"edit_not_found", notFoundMessage(path, i, normalized.size())});
            }
            const std::size_t occurrences = countOccurrences(base, normalized[i].oldText);
            if (occurrences > 1) {
                return std::unexpected(
                    Error{"edit_duplicate", duplicateMessage(path, i, normalized.size(), occurrences)});
            }
            matched.push_back({i, match.index, match.matchLength, normalized[i].newText});
        }
        std::sort(matched.begin(), matched.end(),
                  [](const MatchedEdit& a, const MatchedEdit& b) { return a.matchIndex < b.matchIndex; });
        for (std::size_t i = 1; i < matched.size(); ++i) {
            if (matched[i - 1].matchIndex + matched[i - 1].matchLength > matched[i].matchIndex) {
                return std::unexpected(Error{
                    "edit_overlap", "edits[" + std::to_string(matched[i - 1].editIndex) + "] and edits[" +
                                        std::to_string(matched[i].editIndex) + "] overlap in " + path +
                                        ". Merge them into one edit or target disjoint regions."});
            }
        }
        std::string newContent;
        if (usedFuzzy) {
            auto preserved = applyPreservingUnchangedLines(normalizedContent, base, matched);
            if (!preserved.has_value()) {
                return std::unexpected(preserved.error());
            }
            newContent = std::move(*preserved);
        } else {
            newContent = applyReplacements(base, matched, 0);
        }
        if (newContent == normalizedContent) {
            return std::unexpected(Error{
                "edit_no_change",
                normalized.size() == 1
                    ? "No changes made to " + path +
                          ". The replacement produced identical content. This might indicate an issue with "
                          "special characters or the text not existing as expected."
                    : "No changes made to " + path + ". The replacements produced identical content."});
        }
        return AppliedEdits{normalizedContent, std::move(newContent)};
    }

private:
    std::string replaceAll(std::string text, const std::string& from, const std::string& to) const {
        std::size_t pos = 0;
        while ((pos = text.find(from, pos)) != std::string::npos) {
            text.replace(pos, from.size(), to);
            pos += to.size();
        }
        return text;
    }

    std::vector<std::string> splitLinesWithEndings(const std::string& content) const {
        std::vector<std::string> lines;
        std::size_t start = 0;
        while (start < content.size()) {
            const std::size_t newline = content.find('\n', start);
            const std::size_t end = newline == std::string::npos ? content.size() : newline + 1;
            lines.push_back(content.substr(start, end - start));
            start = end;
        }
        return lines;
    }

    std::vector<std::pair<std::size_t, std::size_t>> lineSpans(const std::string& content) const {
        std::vector<std::pair<std::size_t, std::size_t>> spans;
        std::size_t offset = 0;
        for (const std::string& line : splitLinesWithEndings(content)) {
            spans.emplace_back(offset, offset + line.size());
            offset += line.size();
        }
        return spans;
    }

    Result<std::pair<std::size_t, std::size_t>> replacementLineRange(const std::vector<std::pair<std::size_t, std::size_t>>& lines, const MatchedEdit& edit) const {
        const std::size_t replacementStart = edit.matchIndex;
        const std::size_t replacementEnd = edit.matchIndex + edit.matchLength;
        std::size_t startLine = lines.size();
        for (std::size_t i = 0; i < lines.size(); ++i) {
            if (replacementStart >= lines[i].first && replacementStart < lines[i].second) {
                startLine = i;
                break;
            }
        }
        if (startLine == lines.size()) {
            return std::unexpected(Error{"edit_range", "Replacement range is outside the base content."});
        }
        std::size_t endLine = startLine;
        while (endLine < lines.size() && lines[endLine].second < replacementEnd) {
            ++endLine;
        }
        if (endLine >= lines.size()) {
            return std::unexpected(Error{"edit_range", "Replacement range is outside the base content."});
        }
        return std::make_pair(startLine, endLine + 1);
    }

    std::string applyReplacements(const std::string& content, const std::vector<MatchedEdit>& edits, std::size_t offset) const {
        std::string result = content;
        for (auto it = edits.rbegin(); it != edits.rend(); ++it) {
            const std::size_t index = it->matchIndex - offset;
            result = result.substr(0, index) + it->newText + result.substr(index + it->matchLength);
        }
        return result;
    }

    Result<std::string> applyPreservingUnchangedLines(const std::string& original, const std::string& base, std::vector<MatchedEdit> edits) const {
        const std::vector<std::string> originalLines = splitLinesWithEndings(original);
        const auto baseLines = lineSpans(base);
        if (originalLines.size() != baseLines.size()) {
            return std::unexpected(Error{
                "edit_lines",
                "Cannot preserve unchanged lines because the base content has a different line count."});
        }
        std::sort(edits.begin(), edits.end(),
                  [](const MatchedEdit& a, const MatchedEdit& b) { return a.matchIndex < b.matchIndex; });
        std::vector<std::pair<std::size_t, std::size_t>> groupRanges;
        std::vector<std::vector<MatchedEdit>> groupEdits;
        for (const MatchedEdit& edit : edits) {
            auto range = replacementLineRange(baseLines, edit);
            if (!range.has_value()) {
                return std::unexpected(range.error());
            }
            if (!groupRanges.empty() && range->first < groupRanges.back().second) {
                groupRanges.back().second = std::max(groupRanges.back().second, range->second);
                groupEdits.back().push_back(edit);
            } else {
                groupRanges.push_back(*range);
                groupEdits.push_back({edit});
            }
        }
        std::string result;
        std::size_t originalIndex = 0;
        for (std::size_t g = 0; g < groupRanges.size(); ++g) {
            for (std::size_t i = originalIndex; i < groupRanges[g].first; ++i) {
                result += originalLines[i];
            }
            const std::size_t startOffset = baseLines[groupRanges[g].first].first;
            const std::size_t endOffset = baseLines[groupRanges[g].second - 1].second;
            result += applyReplacements(base.substr(startOffset, endOffset - startOffset), groupEdits[g], startOffset);
            originalIndex = groupRanges[g].second;
        }
        for (std::size_t i = originalIndex; i < originalLines.size(); ++i) {
            result += originalLines[i];
        }
        return result;
    }

    std::size_t countOccurrences(const std::string& content, const std::string& oldText) const {
        const std::string fuzzyContent = normalizeForFuzzyMatch(content);
        const std::string fuzzyOld = normalizeForFuzzyMatch(oldText);
        if (fuzzyOld.empty()) {
            return 0;
        }
        std::size_t count = 0;
        for (std::size_t pos = fuzzyContent.find(fuzzyOld); pos != std::string::npos;
             pos = fuzzyContent.find(fuzzyOld, pos + fuzzyOld.size())) {
            ++count;
        }
        return count;
    }

    unsigned int mapCodePoint(unsigned int cp, std::string& out) const {
        if (cp >= 0x2018 && cp <= 0x201B) {
            out.push_back('\'');
        } else if (cp >= 0x201C && cp <= 0x201F) {
            out.push_back('"');
        } else if ((cp >= 0x2010 && cp <= 0x2015) || cp == 0x2212) {
            out.push_back('-');
        } else if (cp == 0x00A0 || (cp >= 0x2002 && cp <= 0x200A) || cp == 0x202F || cp == 0x205F || cp == 0x3000) {
            out.push_back(' ');
        } else if (cp == 0x2026) {
            out.append("...");
        } else if (cp == 0xFB01) {
            out.append("fi");
        } else if (cp == 0xFB02) {
            out.append("fl");
        } else if (cp >= 0xFF01 && cp <= 0xFF5E) {
            out.push_back(static_cast<char>(cp - 0xFF01 + 0x21));
        } else {
            appendUtf8(cp, out);
        }
        return cp;
    }

    void appendUtf8(unsigned int cp, std::string& out) const {
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    std::string notFoundMessage(const std::string& path, std::size_t index, std::size_t total) const {
        if (total == 1) {
            return "Could not find the exact text in " + path +
                   ". The old text must match exactly including all whitespace and newlines.";
        }
        return "Could not find edits[" + std::to_string(index) + "] in " + path +
               ". The oldText must match exactly including all whitespace and newlines.";
    }

    std::string duplicateMessage(const std::string& path, std::size_t index, std::size_t total, std::size_t occurrences) const {
        if (total == 1) {
            return "Found " + std::to_string(occurrences) + " occurrences of the text in " + path +
                   ". The text must be unique. Please provide more context to make it unique.";
        }
        return "Found " + std::to_string(occurrences) + " occurrences of edits[" + std::to_string(index) +
               "] in " + path + ". Each oldText must be unique. Please provide more context to make it unique.";
    }
};

// Appends the normalized form of one code point to `out`; returns the code point.
