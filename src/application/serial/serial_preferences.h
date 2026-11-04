#pragma once

#include <array>
#include <filesystem>
#include <string>

namespace testtools {

struct SerialChannelPreferences {
    bool useAgent = false;
    std::string endpoint;
    int baudRate = 115200;
    int dataBits = 8;
    int parity = 0;
    int stopBits = 0;
    int flowControl = 0;
    int lineEnding = 0;
    bool receiveHex = false;
    bool timestamp = false;
    bool autoScroll = true;
    bool sendHex = false;
    bool highlightKeywords = true;
    bool manualEndpoint = false;
};

struct SerialWorkspacePreferences {
    std::array<SerialChannelPreferences, 2> channels{};
    bool shortcutBarVisible = true;
    int shortcutSidebarWidth = 240;
    int terminalLineEnding = 2;
    int terminalBackspace = 127;
    bool terminalSmartKeys = true;
    bool terminalCopyOnSelect = false;
    bool terminalRightPaste = false;
    int terminalTheme = 0;
    int terminalFontSize = 15;
    int terminalLineSpacing = 3;
    int terminalCursorShape = 0;
    std::string terminalFont = "Cascadia Mono";
    SerialWorkspacePreferences();
};

class SerialPreferencesStore final {
  public:
    static bool load(const std::filesystem::path& path,
                     SerialWorkspacePreferences& preferences,
                     std::string& error);
    static bool save(const std::filesystem::path& path,
                     const SerialWorkspacePreferences& preferences,
                     std::string& error);
};

} // namespace testtools
