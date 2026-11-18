// clipboard.h
//
// Copyright (c) 2022-2025 Kristofer Berggren
// All rights reserved.
//
// nchat is distributed under the MIT license, see LICENSE for details.

#pragma once

#include <string>

class Clipboard
{
public:
  // Configure only in processes that own AppConfig. UI-only processes use native defaults.
  static void SetCommands(const std::string& p_Copy, const std::string& p_Paste,
                          const std::string& p_HasImage, const std::string& p_PasteImage);
  static void LoadCommands(const std::string& p_Path); // Read only; never creates or saves config.
  static void SetText(const std::string& p_Text);
  static std::string GetText();

  static bool HasImage();
  static bool GetImage(const std::string& p_Path);
};
