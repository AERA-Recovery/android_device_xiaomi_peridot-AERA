/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include "aera_mobile/client.hpp"
#include "qmi.hpp"
#include "nr.hpp"
#include "sim.hpp"
#include <openssl/sha.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <linux/if_link.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/file.h>
#include <sys/un.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <cutils/properties.h>
#include <json/json.h>
#include <array>
#include <atomic>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <string>
#include <map>
#include <vector>
#include <condition_variable>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#include <csignal>

namespace aera::mobile::peridot {
inline constexpr const char *kDirectory="/tmp/aera-cellular";
inline constexpr const char *kInterface="rmnet_aera";
inline constexpr const char *kHelperDirectory="/system/bin";

extern std::atomic<bool> cancel, exiting, working;
extern std::mutex state_mutex, completion_mutex;
extern std::condition_variable completion;
extern Json::Value status;
extern std::string pending_pin, pending_pin_request;
extern uint64_t pin_request_sequence;

void ClearPin(std::string &pin);
std::string SimIdentity(const Bytes &iccid);
std::string Read(const std::string &path,size_t maximum=16384);
bool Write(const std::string &path,const std::string &value,
           int flags=O_WRONLY,mode_t mode=0600);
void SaveLocked();
void Phase(const std::string &state,const std::string &message);
void Log(const std::string &message);
void ClearNrLocked();
void ClearConnectionLocked();
void CheckCancelled();
void Delay(unsigned milliseconds);
std::vector<std::string> Entries(const std::string &directory);
std::string ModemPath();
bool HasRmtfsUio();
std::string Capability();
int Run(const std::vector<std::string> &args);
void Ip(std::initializer_list<std::string> args,bool required=true);
void Module(const std::string &name);
using IpaData=std::array<uint32_t,6>;
IpaData Ipa(IpaData data);
Bytes Attribute(uint16_t type,const Bytes &value);
void Append(Bytes &a,const Bytes &b);
Bytes StringBytes(const std::string &s);
void CreateLink();
std::string Address(const Values &v,uint8_t tag);
int Prefix(const std::string &netmask);
bool WifiConnected();
}
