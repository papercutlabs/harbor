// ui.h
//
// Copyright (c) 2019-2025 Kristofer Berggren
// All rights reserved.
//
// nchat is distributed under the MIT license, see LICENSE for details.

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

class Protocol;
class ServiceMessage;
class UiController;
class Harbor;
class UiModel;

class Ui
{
public:
  Ui();
  virtual ~Ui();

  void Init();
  void Cleanup();

  void Run();
  void AddProtocol(std::shared_ptr<Protocol> p_Protocol);
  std::unordered_map<std::string, std::shared_ptr<Protocol>> GetProtocols();
  void MessageHandler(std::shared_ptr<ServiceMessage> p_ServiceMessage);

  static void RunKeyDump();
  // Harbor shared-session window only (defined in src/harbor/harbor.cpp).
  void SetSession(int p_Slot, std::function<void()> p_OnAuthSuspended, const std::string& p_Generation);
  void SetInitialChat(const std::string& p_Profile, const std::string& p_Conversation, const std::string& p_Message = {});

private:
  std::shared_ptr<Harbor> m_Harbor;
  std::shared_ptr<UiModel> m_Model;
  std::shared_ptr<UiController> m_Controller;
  std::string m_TerminalTitle;
};
