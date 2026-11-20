#pragma once
#include "protocol.h"
#include <td/telegram/td_api.h>

namespace TelegramPhoto
{
// Keep the largest Telegram rendition and its image identity before download.
inline FileInfo File(const td::td_api::photo& photo)
{
  FileInfo out;
  if (photo.sizes_.empty()) return out;
  const auto& file = photo.sizes_.back()->photo_;
  out.fileId = file->remote_->id_;
  out.fileType = "image/jpeg";
  out.filePath = file->local_->path_.empty() ? "[Photo]" : file->local_->path_;
  out.fileStatus = file->local_->path_.empty() ? FileStatusNotDownloaded : FileStatusDownloaded;
  return out;
}
}
