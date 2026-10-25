#pragma once
#include <iostream>
#include <string>
#include <string_view>

// CLI-only presentation. Shared application code already redacts credentials.
class StreamOutput {
public:
    void append(std::string_view text) {
        std::string display;
        display.reserve(text.size());
        for (const unsigned char c : text) {
            if ((c < 32 && c != '\n' && c != '\t') || c == 127) {
                constexpr char hex[] = "0123456789abcdef";
                display += "\\x";
                display += hex[c >> 4]; display += hex[c & 15];
            } else display.push_back(static_cast<char>(c));
        }
        if (!display.empty()) { std::cerr << display; wrote_ = true; std::cerr.flush(); }
    }
    void finish() {
        if (wrote_) { std::cerr << '\n'; std::cerr.flush(); wrote_ = false; }
    }
private:
    bool wrote_ = false;
};
