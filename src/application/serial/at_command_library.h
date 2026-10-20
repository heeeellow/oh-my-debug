#pragma once

#include <array>
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace testtools {

enum class AtCommandLineEnding { None, Lf, Cr, CrLf };

struct AtCommand {
    std::string name;
    std::string command;
    bool hex = false;
};

class AtCommandLibrary final {
  public:
    AtCommandLibrary();

    const std::vector<AtCommand>& commands() const noexcept;
    AtCommandLineEnding lineEnding() const noexcept;
    void setLineEnding(AtCommandLineEnding ending) noexcept;
    bool replace(std::vector<AtCommand> commands, std::string& error);
    bool add(AtCommand command, std::string& error);
    bool update(std::size_t index, AtCommand command, std::string& error);
    bool remove(std::size_t index);

  private:
    bool valid(const AtCommand& command,
               std::size_t ignoredIndex,
               std::string& error) const;

    std::vector<AtCommand> commands_;
    AtCommandLineEnding lineEnding_ = AtCommandLineEnding::CrLf;
};

class AtCommandStore final {
  public:
    static bool load(const std::filesystem::path& path,
                     AtCommandLibrary& library,
                     std::string& error);
    static bool save(const std::filesystem::path& path,
                     const AtCommandLibrary& library,
                     std::string& error);
    static bool importCommands(const std::filesystem::path& path,
                                AtCommandLibrary& library, std::string& error);
};

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
};

struct SerialWorkspacePreferences {
    std::array<SerialChannelPreferences, 2> channels{};
    bool shortcutBarVisible = true;
    int terminalLineEnding = 2;
    int terminalBackspace = 127;
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
