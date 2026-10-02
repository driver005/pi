export module pi.support.text_diff;

import std;
export import pi.types.diff_part;
export import pi.types.display_diff;

/**
 * Line diff (Myers) with the two renderings the edit tool reports: a unified patch and a
 * line-numbered display diff. Mirrors jsdiff's diffLines/createTwoFilesPatch behavior.
 */
export class TextDiff {
public:
    /** Common/removed/added runs; within a changed block removals come before additions. */
    std::vector<DiffPart> diffLines(const std::string& oldText, const std::string& newText) const;

    /** "--- path\n+++ path\n@@ ... @@" patch with `context` lines around each change. */
    std::string unifiedPatch(const std::string& path, const std::string& oldText,
                             const std::string& newText, int context = 4) const;

    DisplayDiff displayDiff(const std::string& oldText, const std::string& newText,
                            int contextLines = 4) const;

private:
    std::vector<std::string> tokenize(const std::string& text) const;
    std::vector<std::pair<int, int>> editScript(const std::vector<std::string>& a,
                                                const std::vector<std::string>& b) const;
    std::vector<std::string> splitForDisplay(const std::vector<std::string>& tokens) const;
    std::string stripNewline(const std::string& token) const;
    std::string padLeft(int value, std::size_t width) const;
    bool endsWithNewline(const std::string& text) const;
};

std::vector<std::string> TextDiff::tokenize(const std::string& text) const {
    std::vector<std::string> tokens;
    std::size_t start = 0;
    while (start < text.size()) {
        const std::size_t newline = text.find('\n', start);
        const std::size_t end = newline == std::string::npos ? text.size() : newline + 1;
        tokens.push_back(text.substr(start, end - start));
        start = end;
    }
    return tokens;
}

std::string TextDiff::stripNewline(const std::string& token) const {
    return !token.empty() && token.back() == '\n' ? token.substr(0, token.size() - 1) : token;
}

bool TextDiff::endsWithNewline(const std::string& text) const {
    return !text.empty() && text.back() == '\n';
}

std::string TextDiff::padLeft(int value, std::size_t width) const {
    std::string text = std::to_string(value);
    return text.size() >= width ? text : std::string(width - text.size(), ' ') + text;
}

// Returns the edit script as (type, tokenIndex) pairs: type 0 common (index into a), 1 removed
// (index into a), 2 added (index into b), in output order with removals before additions.
std::vector<std::pair<int, int>> TextDiff::editScript(const std::vector<std::string>& a,
                                                      const std::vector<std::string>& b) const {
    const int n = static_cast<int>(a.size());
    const int m = static_cast<int>(b.size());
    const int maxD = std::min(n + m, 4000);
    const int offset = maxD + 1;
    std::vector<int> v(static_cast<std::size_t>(2 * maxD + 3), 0);
    std::vector<std::vector<int>> trace;
    int found = -1;
    for (int d = 0; d <= maxD && found < 0; ++d) {
        trace.push_back(v);
        for (int k = -d; k <= d; k += 2) {
            int x = 0;
            const auto idx = [&](int diagonal) { return static_cast<std::size_t>(diagonal + offset); };
            if (k == -d || (k != d && v[idx(k - 1)] < v[idx(k + 1)])) {
                x = v[idx(k + 1)];
            } else {
                x = v[idx(k - 1)] + 1;
            }
            int y = x - k;
            while (x < n && y < m && a[static_cast<std::size_t>(x)] == b[static_cast<std::size_t>(y)]) {
                ++x;
                ++y;
            }
            v[idx(k)] = x;
            if (x >= n && y >= m) {
                found = d;
                break;
            }
        }
    }
    std::vector<std::pair<int, int>> script;
    if (found < 0) {
        for (int i = 0; i < n; ++i) {
            script.emplace_back(1, i);
        }
        for (int j = 0; j < m; ++j) {
            script.emplace_back(2, j);
        }
        return script;
    }
    int x = n;
    int y = m;
    for (int d = found; d > 0; --d) {
        const std::vector<int>& prev = trace[static_cast<std::size_t>(d)];
        const int k = x - y;
        const auto idx = [&](int diagonal) { return static_cast<std::size_t>(diagonal + offset); };
        int prevK = 0;
        if (k == -d || (k != d && prev[idx(k - 1)] < prev[idx(k + 1)])) {
            prevK = k + 1;
        } else {
            prevK = k - 1;
        }
        const int prevX = prev[idx(prevK)];
        const int prevY = prevX - prevK;
        while (x > prevX && y > prevY) {
            script.emplace_back(0, x - 1);
            --x;
            --y;
        }
        if (x == prevX) {
            script.emplace_back(2, prevY);
        } else {
            script.emplace_back(1, prevX);
        }
        x = prevX;
        y = prevY;
    }
    while (x > 0 && y > 0) {
        script.emplace_back(0, x - 1);
        --x;
        --y;
    }
    std::reverse(script.begin(), script.end());
    return script;
}

std::vector<DiffPart> TextDiff::diffLines(const std::string& oldText, const std::string& newText) const {
    const std::vector<std::string> a = tokenize(oldText);
    const std::vector<std::string> b = tokenize(newText);
    const auto script = editScript(a, b);
    std::vector<DiffPart> parts;
    std::size_t i = 0;
    while (i < script.size()) {
        if (script[i].first == 0) {
            DiffPart part;
            while (i < script.size() && script[i].first == 0) {
                part.lines.push_back(stripNewline(a[static_cast<std::size_t>(script[i].second)]));
                ++i;
            }
            parts.push_back(std::move(part));
            continue;
        }
        DiffPart removed;
        removed.removed = true;
        DiffPart added;
        added.added = true;
        while (i < script.size() && script[i].first != 0) {
            if (script[i].first == 1) {
                removed.lines.push_back(stripNewline(a[static_cast<std::size_t>(script[i].second)]));
            } else {
                added.lines.push_back(stripNewline(b[static_cast<std::size_t>(script[i].second)]));
            }
            ++i;
        }
        if (!removed.lines.empty()) {
            parts.push_back(std::move(removed));
        }
        if (!added.lines.empty()) {
            parts.push_back(std::move(added));
        }
    }
    return parts;
}

std::string TextDiff::unifiedPatch(const std::string& path, const std::string& oldText,
                                   const std::string& newText, int context) const {
    std::vector<DiffPart> diff = diffLines(oldText, newText);
    diff.push_back(DiffPart{});
    std::string out = "--- " + path + "\n+++ " + path + "\n";
    std::vector<std::string> range;
    int oldRangeStart = 0;
    int newRangeStart = 0;
    int oldLine = 1;
    int newLine = 1;
    const auto count = static_cast<int>(diff.size());
    for (int i = 0; i < count; ++i) {
        const DiffPart& current = diff[static_cast<std::size_t>(i)];
        const auto size = static_cast<int>(current.lines.size());
        if (current.added || current.removed) {
            if (oldRangeStart == 0) {
                oldRangeStart = oldLine;
                newRangeStart = newLine;
                if (i > 0) {
                    const auto& previous = diff[static_cast<std::size_t>(i - 1)].lines;
                    const int take = std::min<int>(context, static_cast<int>(previous.size()));
                    for (int j = static_cast<int>(previous.size()) - take; j < static_cast<int>(previous.size()); ++j) {
                        range.push_back(" " + previous[static_cast<std::size_t>(j)]);
                    }
                    oldRangeStart -= take;
                    newRangeStart -= take;
                }
            }
            for (const std::string& line : current.lines) {
                range.push_back((current.added ? "+" : "-") + line);
            }
            (current.added ? newLine : oldLine) += size;
            continue;
        }
        if (oldRangeStart != 0) {
            if (size <= context * 2 && i < count - 2) {
                for (const std::string& line : current.lines) {
                    range.push_back(" " + line);
                }
            } else {
                const int contextSize = std::min(size, context);
                for (int j = 0; j < contextSize; ++j) {
                    range.push_back(" " + current.lines[static_cast<std::size_t>(j)]);
                }
                int oldLines = oldLine - oldRangeStart + contextSize;
                const int newLines = newLine - newRangeStart + contextSize;
                if (i >= count - 2 && size <= context) {
                    const bool oldEof = endsWithNewline(oldText);
                    const bool newEof = endsWithNewline(newText);
                    const bool noNlBeforeAdds = size == 0 && static_cast<int>(range.size()) > oldLines;
                    if (!oldEof && noNlBeforeAdds && !oldText.empty()) {
                        range.insert(range.begin() + oldLines, "\\ No newline at end of file");
                    }
                    if ((!oldEof && !noNlBeforeAdds) || !newEof) {
                        range.push_back("\\ No newline at end of file");
                    }
                }
                const int shownOldStart = oldLines == 0 ? oldRangeStart - 1 : oldRangeStart;
                const int shownNewStart = newLines == 0 ? newRangeStart - 1 : newRangeStart;
                out += "@@ -" + std::to_string(shownOldStart) + "," + std::to_string(oldLines) + " +" +
                       std::to_string(shownNewStart) + "," + std::to_string(newLines) + " @@\n";
                for (const std::string& line : range) {
                    out += line + "\n";
                }
                range.clear();
                oldRangeStart = 0;
                newRangeStart = 0;
            }
        }
        oldLine += size;
        newLine += size;
    }
    return out;
}

DisplayDiff TextDiff::displayDiff(const std::string& oldText, const std::string& newText,
                                  int contextLines) const {
    const std::vector<DiffPart> parts = diffLines(oldText, newText);
    const auto countLines = [](const std::string& text) {
        return static_cast<int>(std::count(text.begin(), text.end(), '\n')) + 1;
    };
    const std::size_t width = std::to_string(std::max(countLines(oldText), countLines(newText))).size();
    std::vector<std::string> output;
    int oldNum = 1;
    int newNum = 1;
    bool lastWasChange = false;
    DisplayDiff result;
    const std::string blank(width, ' ');
    for (std::size_t i = 0; i < parts.size(); ++i) {
        const DiffPart& part = parts[i];
        const auto& raw = part.lines;
        if (part.added || part.removed) {
            if (!result.firstChangedLine.has_value()) {
                result.firstChangedLine = newNum;
            }
            for (const std::string& line : raw) {
                if (part.added) {
                    output.push_back("+" + padLeft(newNum++, width) + " " + line);
                } else {
                    output.push_back("-" + padLeft(oldNum++, width) + " " + line);
                }
            }
            lastWasChange = true;
            continue;
        }
        const bool nextIsChange = i + 1 < parts.size() && (parts[i + 1].added || parts[i + 1].removed);
        const auto size = static_cast<int>(raw.size());
        const auto show = [&](int from, int to) {
            for (int j = from; j < to; ++j) {
                output.push_back(" " + padLeft(oldNum++, width) + " " + raw[static_cast<std::size_t>(j)]);
                ++newNum;
            }
        };
        const auto skip = [&](int count) {
            output.push_back(" " + blank + " ...");
            oldNum += count;
            newNum += count;
        };
        if (lastWasChange && nextIsChange) {
            if (size <= contextLines * 2) {
                show(0, size);
            } else {
                show(0, contextLines);
                skip(size - contextLines * 2);
                show(size - contextLines, size);
            }
        } else if (lastWasChange) {
            const int shown = std::min(size, contextLines);
            show(0, shown);
            if (size - shown > 0) {
                skip(size - shown);
            }
        } else if (nextIsChange) {
            const int skipped = std::max(0, size - contextLines);
            if (skipped > 0) {
                skip(skipped);
            }
            show(skipped, size);
        } else {
            oldNum += size;
            newNum += size;
        }
        lastWasChange = false;
    }
    for (std::size_t i = 0; i < output.size(); ++i) {
        result.diff += (i > 0 ? "\n" : "") + output[i];
    }
    return result;
}
