// Copyright (c) FufuLauncher Dev Team. AGPL-3.0.
#pragma once
#include <Windows.h>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>

namespace Config
{
    // Preserve existing bytes, comments, custom keys and encoding. Add whole
    // sections only when absent, then replace the file after a complete write.
    inline bool EnsureShortcutSettings(const char* file)
    {
        std::ifstream input(file, std::ios::binary);
        if (!input) return false;
        std::string original((std::istreambuf_iterator<char>(input)), {});
        if (input.bad()) return false;
        input.close();
        const bool wide = original.compare(0, 2, "\xFF\xFE") == 0;
        if (original.compare(0, 2, "\xFE\xFF") == 0) return false;
        std::string text = original;
        if (wide)
        {
            if ((original.size() - 2) % sizeof(wchar_t)) return false;
            std::wstring decoded((original.size() - 2) / sizeof(wchar_t), L'\0');
            std::memcpy(decoded.data(), original.data() + 2, original.size() - 2);
            const int length = WideCharToMultiByte(CP_UTF8, 0, decoded.data(),
                static_cast<int>(decoded.size()), nullptr, 0, nullptr, nullptr);
            text.assign(length, '\0');
            WideCharToMultiByte(CP_UTF8, 0, decoded.data(), static_cast<int>(decoded.size()),
                text.data(), length, nullptr, nullptr);
        }
        else if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) text.erase(0, 3);

        const struct { const char* name; const char* body; } sections[] = {
            {"QuickClaim", reinterpret_cast<const char*>(u8"[QuickClaim]\r\nName = 快捷领取每日委托\r\nType = bool\r\nValue = 0\r\n")},
            {"AutoForge", reinterpret_cast<const char*>(u8"[AutoForge]\r\nName = 快捷打开锻造界面\r\nType = bool\r\nValue = 0\r\n")},
            {"ClaimAllKey", reinterpret_cast<const char*>(u8"[ClaimAllKey]\r\nName = 每日委托领取快捷键\r\nDescription = 默认 F8；两阶段请求间隔至少 600 毫秒，按住不会重复触发\r\nType = key\r\nValue = 119\r\n")},
            {"AutoForgeKey", reinterpret_cast<const char*>(u8"[AutoForgeKey]\r\nName = 打开锻造界面快捷键\r\nType = key\r\nValue = 118\r\n")}
        };
        std::string addition;
        for (const auto& section : sections)
        {
            std::istringstream lines(text);
            std::string line;
            bool found = false;
            while (std::getline(lines, line))
            {
                const auto first = line.find_first_not_of(" \t\r");
                const auto last = line.find_last_not_of(" \t\r");
                if (first == std::string::npos || line[first] != '[' || line[last] != ']') continue;
                auto name = line.substr(first + 1, last - first - 1);
                const auto start = name.find_first_not_of(" \t");
                if (start == std::string::npos) continue;
                name = name.substr(start, name.find_last_not_of(" \t") - start + 1);
                if (_stricmp(name.c_str(), section.name) == 0) { found = true; break; }
            }
            if (!found) { addition += "\r\n"; addition += section.body; }
        }
        if (addition.empty()) return true;
        if (wide)
        {
            const int length = MultiByteToWideChar(CP_UTF8, 0, addition.data(),
                static_cast<int>(addition.size()), nullptr, 0);
            std::wstring encoded(length, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, addition.data(), static_cast<int>(addition.size()),
                encoded.data(), length);
            original.append(reinterpret_cast<const char*>(encoded.data()), encoded.size() * sizeof(wchar_t));
        }
        else original += addition;

        const auto temporary = std::string(file) + ".shortcuts.tmp";
        HANDLE output = CreateFileA(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
            CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (output == INVALID_HANDLE_VALUE) return false;
        DWORD written = 0;
        bool success = original.size() <= MAXDWORD && WriteFile(output, original.data(),
            static_cast<DWORD>(original.size()), &written, nullptr) && written == original.size();
        if (success) success = FlushFileBuffers(output) != FALSE;
        CloseHandle(output);
        if (success) success = MoveFileExA(temporary.c_str(), file,
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
        if (!success) DeleteFileA(temporary.c_str());
        return success;
    }
}
