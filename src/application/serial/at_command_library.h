#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace testtools {

// 快捷指令与串口偏好持久化模型及 JSON 存储服务。
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
    bool valid(const AtCommand& command, std::size_t ignoredIndex, std::string& error) const;

    std::vector<AtCommand> commands_;
    AtCommandLineEnding lineEnding_ = AtCommandLineEnding::CrLf;
};

class AtCommandStore final {
  public:
    static bool
    load(const std::filesystem::path& path, AtCommandLibrary& library, std::string& error);
    static bool
    save(const std::filesystem::path& path, const AtCommandLibrary& library, std::string& error);
    static bool importCommands(const std::filesystem::path& path,
                               AtCommandLibrary& library,
                               std::string& error);
};

} // namespace testtools
