#pragma once
#include "core/device/file_transfer.h"
namespace testtools {
FileTransferResult changeLocalFile(const FileChange&, const std::atomic<bool>&);
std::string deviceFileChangeScript(const FileChange&);
}
