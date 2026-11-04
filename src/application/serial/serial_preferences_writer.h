#pragma once

#include "application/serial/at_command_library.h"
#include <functional>
#include <memory>

namespace testtools {

// One file, one writer, and at most one pending replacement snapshot.
class SerialPreferencesWriter final {
  public:
    using Save = std::function<bool(const std::filesystem::path&,
                                   const SerialWorkspacePreferences&,
                                   std::string&)>;
    explicit SerialPreferencesWriter(std::filesystem::path path,
                                     Save save = SerialPreferencesStore::save);
    ~SerialPreferencesWriter();
    void submit(SerialWorkspacePreferences preferences);
    bool pending() const;
    std::string error() const;
    void waitIdle();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace testtools
