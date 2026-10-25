/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "runtime.hpp"

namespace aera::mobile::peridot {
struct Session {
  Fd lease;
  std::unique_ptr<QmiClient> dpm,wda,wds,nas;
  std::string modem,firmware,firmware_mount,old_firmware_path,old_resolv,own_resolv,dns1,dns2;
  std::array<std::string,2> old_dns;
  pid_t rmtfs=-1,tftp=-1;
  bool modem_owned=false,link_owned=false,firmware_mounted=false,dns_owned=false,firmware_path_changed=false;
  bool old_resolv_exists=false;
  bool established=false;
  std::thread network_worker;
  std::atomic<bool> network_stop{false};
  std::mutex network_mutex;
  std::string network_error;
  uint32_t handle=0;

  ~Session();
  void Connect(const std::string &apn,int subscription);
  void Stop();

 private:
  std::map<unsigned,std::string> SimIdentities(QmiClient &uim);
  int WaitForSim(const Endpoint &endpoint,int subscription);
  pid_t Spawn(const std::string &name,const std::vector<std::string> &arguments);
  bool Alive(pid_t pid);
  void CheckHelpers();
  void PrepareFirmware();
  Endpoint Find(const std::vector<Endpoint> &endpoints,uint32_t service,uint32_t node=UINT32_MAX);
  void Dns();
  void RestoreDns();
  void WatchNetwork();
  void Signal();
  bool LteServiceAvailable();
  void BindSubscription(int subscription);
  void StartModem();
  void StopChild(pid_t &pid);
  void ConnectData(const std::string &apn,int subscription);
};
}
