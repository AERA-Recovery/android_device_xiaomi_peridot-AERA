/* SPDX-License-Identifier: Apache-2.0 */
#include "runtime.hpp"

namespace aera::mobile::peridot {
std::atomic<bool> cancel{false}, exiting{false}, working{false};
std::mutex state_mutex, completion_mutex;
std::condition_variable completion;
Json::Value status(Json::objectValue);
std::string pending_pin,pending_pin_request;
uint64_t pin_request_sequence=0;
void ClearPin(std::string &pin) {
  volatile char *data=pin.data();for(size_t i=0;i<pin.size();++i)data[i]=0;
  pin.clear();
}
std::string SimIdentity(const Bytes &iccid) {
  unsigned char digest[SHA256_DIGEST_LENGTH];
  SHA256(iccid.data(),iccid.size(),digest);
  const char *hex="0123456789abcdef";std::string out;
  for(unsigned char c:digest){out+=hex[c>>4];out+=hex[c&15];}
  return out;
}

std::string Read(const std::string &path,size_t maximum) {
  Fd fd(open(path.c_str(),O_RDONLY|O_CLOEXEC));
  if(fd.get()<0)return {};
  std::string result;char buffer[1024];
  while(result.size()<maximum) {
    ssize_t n=read(fd.get(),buffer,std::min(sizeof(buffer),maximum-result.size()));
    if(n<0&&errno==EINTR)continue;
    if(n<=0)break;
    result.append(buffer,n);
  }
  while(!result.empty()&&(result.back()=='\n'||result.back()=='\r'))result.pop_back();
  return result;
}
bool Write(const std::string &path,const std::string &value,int flags,mode_t mode) {
  Fd fd(open(path.c_str(),flags|O_CLOEXEC|O_NOFOLLOW,mode));
  if(fd.get()<0)return false;
  if((flags&O_CREAT)&&fchmod(fd.get(),mode))return false;
  size_t off=0;
  while(off<value.size()) {
    ssize_t n=write(fd.get(),value.data()+off,value.size()-off);
    if(n<0&&errno==EINTR)continue;
    if(n<=0)return false;
    off+=n;
  }
  return true;
}
void SaveLocked() {
  status["boot_id"]=Read("/proc/sys/kernel/random/boot_id");
  status["pid"]=getpid();status["version"]=1;
  Json::StreamWriterBuilder builder;builder["indentation"]="";
  if(Write(std::string(kStatus)+".new",Json::writeString(builder,status),O_WRONLY|O_CREAT|O_TRUNC))
    rename((std::string(kStatus)+".new").c_str(),kStatus);
}
void Phase(const std::string &state,const std::string &message) {
  std::lock_guard<std::mutex> lock(state_mutex);
  status["state"]=state;status["message"]=message;SaveLocked();
  fprintf(stderr,"mobile: %s: %s\n",state.c_str(),message.c_str());
}
// Logs stay in recovery RAM and never contain SIM identifiers, PINs or IP addresses.
void Log(const std::string &message) {
  const std::string line="aera-cellular: "+message+"\n";
  fputs(line.c_str(),stderr);fflush(stderr);
  (void)Write(std::string(kDirectory)+"/provider.log",line,O_WRONLY|O_CREAT|O_APPEND,0600);
}
void ClearNrLocked() {
  status.removeMember("nr_sample_count");
  for(const char *key:{"nr_system_query_ok","nr_signal_query_ok","nr_lte_service",
      "nr_signal_tlv","nr_active","nr_packet_domain","nr_forbidden","nr_reject"})
    status.removeMember(key);
  for(const char *key:{"nr_service","nr_true_service","nr_nsa_advertised","nr_dcnr_restricted",
      "nr_rsrp_dbm","nr_snr_tenths_db","nr_rsrq_db","nr_summary"})
    status.removeMember(key);
}
void ClearConnectionLocked() {
  ClearNrLocked();
  status["active_subscription"]=0;
  status["connected"]=false;status["registered"]=false;status["signal_valid"]=false;
  status["bars"]=-1;status["signal_dbm"]=0;
  for(const char *key:{"address","gateway","interface","operator","technology","dns1","dns2","effective_apn"})
    status[key]="";
  status.removeMember("mtu");
}
void CheckCancelled() { if(cancel.load())throw std::runtime_error("Connection cancelled"); }
void Delay(unsigned milliseconds) {
  for(unsigned elapsed=0;elapsed<milliseconds;elapsed+=50) {
    CheckCancelled();std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
}
std::vector<std::string> Entries(const std::string &directory) {
  std::vector<std::string> out;DIR *dir=opendir(directory.c_str());if(!dir)return out;
  while(auto *entry=readdir(dir))if(entry->d_name[0]!='.')out.emplace_back(entry->d_name);
  closedir(dir);return out;
}
std::string ModemPath() {
  std::string found;
  for(const auto &entry:Entries("/sys/class/remoteproc")) {
    const std::string path="/sys/class/remoteproc/"+entry;
    const std::string name=Read(path+"/name");
    if(name.find("mss")!=std::string::npos||name=="modem") {
      if(!found.empty())return {};
      found=path;
    }
  }
  return found;
}
bool HasRmtfsUio() {
  for(const auto &entry:Entries("/sys/class/uio"))
    if(Read("/sys/class/uio/"+entry+"/name")=="rmtfs")return true;
  return false;
}
std::string Capability() {
  if(ModemPath().empty())return "No supported Qualcomm modem control was found";
  if(!HasRmtfsUio())return "The modem RAM-storage interface is unavailable";
  for(const auto &helper:{"aera-mobile-rmtfs","aera-mobile-tftp"})
    if(access((std::string(kHelperDirectory)+"/"+helper).c_str(),X_OK))return "Mobile data helpers are not installed";
  Fd transport(socket(42,SOCK_DGRAM|SOCK_CLOEXEC,0));
  if(transport.get()<0)return "The kernel has no QRTR modem transport";
  return {};
}
int Run(const std::vector<std::string> &args) {
  if(args.empty())return -1;
  pid_t pid=fork();if(pid<0)return -1;
  if(pid==0) {
    std::vector<char *> argv;for(const auto &arg:args)argv.push_back(const_cast<char *>(arg.c_str()));
    argv.push_back(nullptr);execv(argv[0],argv.data());_exit(127);
  }
  int result=0;while(waitpid(pid,&result,0)<0)if(errno!=EINTR)return -1;
  return WIFEXITED(result)?WEXITSTATUS(result):-1;
}
void Ip(std::initializer_list<std::string> args,bool required) {
  std::vector<std::string> command{"/sbin/busybox","ip"};command.insert(command.end(),args);
  if(Run(command)&&required)throw std::runtime_error("Network interface configuration failed");
}
void Module(const std::string &name) {
  if(access(("/sys/module/"+name).c_str(),F_OK)==0)return;
  for(const auto &root:{"/tmp/vendor_dlkm/lib/modules","/vendor_dlkm/lib/modules",
      "/tmp/vendor/lib/modules","/vendor/lib/modules","/lib/modules"}) {
    Fd fd(open((std::string(root)+"/"+name+".ko").c_str(),O_RDONLY|O_CLOEXEC));
    if(fd.get()<0)continue;
    if(syscall(SYS_finit_module,fd.get(),"",0)==0||errno==EEXIST)return;
  }
  throw std::runtime_error("Missing or incompatible kernel module: "+name);
}
IpaData Ipa(IpaData data) {
  Fd fd(socket(AF_INET,SOCK_DGRAM|SOCK_CLOEXEC,0));
  ifreq request{};strcpy(request.ifr_name,"rmnet_ipa0");
  request.ifr_data=reinterpret_cast<char *>(data.data());
  if(fd.get()<0||ioctl(fd.get(),0x89fd,&request))
    throw std::runtime_error("The kernel IPA data ioctl failed (errno "+
                             std::to_string(errno)+")");
  return data;
}
Bytes Attribute(uint16_t type,const Bytes &value) {
  Bytes b;Put16(b,value.size()+4);Put16(b,type);b.insert(b.end(),value.begin(),value.end());
  b.resize((b.size()+3)&~size_t(3));return b;
}
void Append(Bytes &a,const Bytes &b){a.insert(a.end(),b.begin(),b.end());}
Bytes StringBytes(const std::string &s){Bytes b(s.begin(),s.end());b.push_back(0);return b;}
void CreateLink() {
  unsigned parent=if_nametoindex("rmnet_ipa0");
  if(!parent||if_nametoindex(kInterface))throw std::runtime_error("Mobile network interface is already in use");
  Fd fd(socket(AF_NETLINK,SOCK_RAW|SOCK_CLOEXEC,NETLINK_ROUTE));
  if(fd.get()<0)throw std::runtime_error("Cannot open route control");
  sockaddr_nl addr{};addr.nl_family=AF_NETLINK;
  if(bind(fd.get(),reinterpret_cast<sockaddr *>(&addr),sizeof(addr)))throw std::runtime_error("Cannot bind route control");
  Bytes data=Attribute(1,Bytes{1,0});Append(data,Attribute(2,Words({0xbc000071,0xffffffff})));
  Bytes info=Attribute(1,StringBytes("rmnet"));Append(info,Attribute(2|NLA_F_NESTED,data));
  Bytes attrs=Attribute(IFLA_LINK,Words({parent}));Append(attrs,Attribute(IFLA_IFNAME,StringBytes(kInterface)));
  Append(attrs,Attribute(IFLA_LINKINFO|NLA_F_NESTED,info));
  nlmsghdr header{};header.nlmsg_len=sizeof(header)+sizeof(ifinfomsg)+attrs.size();
  header.nlmsg_type=RTM_NEWLINK;header.nlmsg_flags=NLM_F_REQUEST|NLM_F_ACK|NLM_F_CREATE|NLM_F_EXCL;header.nlmsg_seq=1;
  Bytes packet(header.nlmsg_len);memcpy(packet.data(),&header,sizeof(header));
  memcpy(packet.data()+sizeof(header)+sizeof(ifinfomsg),attrs.data(),attrs.size());
  if(sendto(fd.get(),packet.data(),packet.size(),0,reinterpret_cast<sockaddr *>(&addr),sizeof(addr))<0)
    throw std::runtime_error("Cannot create mobile network interface");
  pollfd p{fd.get(),POLLIN,0};uint8_t buffer[4096];
  if(poll(&p,1,3000)<=0)throw std::runtime_error("Route control timed out");
  ssize_t n=recv(fd.get(),buffer,sizeof(buffer),0);
  if(n<static_cast<ssize_t>(NLMSG_LENGTH(sizeof(nlmsgerr))))throw std::runtime_error("Invalid route response");
  auto *reply=reinterpret_cast<nlmsghdr *>(buffer);
  auto *error=reinterpret_cast<nlmsgerr *>(NLMSG_DATA(reply));
  if(reply->nlmsg_type!=NLMSG_ERROR||reply->nlmsg_seq!=1)
    throw std::runtime_error("Invalid rmnet rtnetlink ACK");
  if(error->error)
    throw std::runtime_error("Kernel rejected rmnet link (errno "+
                             std::to_string(-error->error)+")");
}
std::string Address(const Values &v,uint8_t tag) {
  const auto &b=Field(v,tag);if(b.size()!=4)return {};
  in_addr address{htonl(U32(b.data()))};char text[INET_ADDRSTRLEN];
  return inet_ntop(AF_INET,&address,text,sizeof(text))?text:"";
}
int Prefix(const std::string &netmask) {
  in_addr a{};if(inet_pton(AF_INET,netmask.c_str(),&a)!=1)throw std::runtime_error("Missing IPv4 netmask");
  uint32_t mask=ntohl(a.s_addr);int prefix=0;
  while(mask&0x80000000){++prefix;mask<<=1;}
  if(mask)throw std::runtime_error("Invalid IPv4 netmask");
  return prefix;
}
bool WifiConnected() {
  if(Read("/sys/class/net/wlan0/carrier")!="1")return false;
  Fd fd(socket(AF_INET,SOCK_DGRAM|SOCK_CLOEXEC,0));
  ifreq request{};strcpy(request.ifr_name,"wlan0");
  if(fd.get()<0||ioctl(fd.get(),SIOCGIFADDR,&request)||
      !reinterpret_cast<sockaddr_in *>(&request.ifr_addr)->sin_addr.s_addr)return false;
  std::istringstream routes(Read("/proc/net/route"));std::string line;
  while(std::getline(routes,line)) {
    std::istringstream fields(line);std::string iface,destination,gateway;unsigned flags=0;
    if(fields>>iface>>destination>>gateway>>std::hex>>flags)
      if(iface=="wlan0"&&destination=="00000000"&&(flags&1))return true;
  }
  return false;
}

}
