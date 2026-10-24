/* SPDX-License-Identifier: Apache-2.0 */
#include "session.hpp"

namespace aera::mobile::peridot {
std::map<unsigned,std::string> Session::SimIdentities(QmiClient &uim) {
  std::map<unsigned,std::string> out;
  for(const auto &entry:ParseSimIdentifiers(Field(uim.Call(0x47,{},5,&cancel),0x10)))
    out[entry.first]=SimIdentity(entry.second);
  return out;
}
int Session::WaitForSim(const Endpoint &endpoint,int subscription) {
  QmiClient uim(endpoint);
  std::map<unsigned,std::string> identities;
  try {
    identities=SimIdentities(uim);
  }catch(const std::exception &){CheckCancelled();}
  unsigned initializing=0,polls=0;
  bool activation_attempted=false;
  const std::string request=std::to_string(getpid())+"-"+std::to_string(++pin_request_sequence);
  while(true) {
    CheckCancelled();CheckHelpers();
    if(++polls%10==0) {
      try{identities=SimIdentities(uim);}catch(const std::exception &){CheckCancelled();}
    }
    const auto cards=ParseSimCards(Field(uim.Call(0x2f,{},5,&cancel),0x10));
    const int resolved=ResolveSimSubscription(cards,subscription);
    const uint16_t index=resolved==2?cards.secondary:cards.primary;
    const SimCard *selected=nullptr;
    Json::Value sims(Json::arrayValue);
    for(const auto &card:cards.cards) {
      Json::Value sim;sim["slot"]=card.slot;sim["id"]=identities[card.slot];
      sim["state"]=SimState(card);sim["retries"]=card.retries;sims.append(sim);
      if(card.slot==unsigned((index>>8)+1)&&card.application==unsigned(index&255))selected=&card;
    }
    if(!selected) {
      // UIM may advertise both indexes as 0xffff for several seconds after
      // MSS starts, before the modem assigns the existing SIM application.
      // Give it a chance to settle instead of issuing a premature change.
      if(!activation_attempted&&polls>=12) {
        const auto *candidate=ProvisioningCandidate(cards,resolved);
        if(candidate) {
          // One request only, even if its response times out. Query the
          // modem afterwards; never repeatedly activate or submit a PIN
          activation_attempted=true;
          Phase("initializing","Activating SIM application");
          try { uim.Call(0x38,ProvisioningRequest(*candidate,resolved),10,&cancel); }
          catch(const QmiError &) { throw; }
          catch(const std::exception &) { CheckCancelled(); }
          Delay(500);continue;
        }
      }
      {
        std::lock_guard<std::mutex> lock(state_mutex);status["sims"]=sims;
        status["sim_state"]="initializing";status["state"]="initializing";
        status["message"]="Preparing SIM";SaveLocked();
      }
      if(++initializing>120)throw std::runtime_error("The selected data SIM has no provisioning application");
      Delay(500);continue;
    }
    if(selected->slot>2)throw std::runtime_error("Unsupported SIM slot");
    const auto state=SimState(*selected);
    const std::string identity=identities[selected->slot];
    {
      std::lock_guard<std::mutex> lock(state_mutex);
      status["sims"]=sims;status["sim_slot"]=selected->slot;status["sim_id"]=identity;
      status["pin_retries"]=selected->retries;status["pin_request"]=request;
      status["sim_state"]=state;
      if(state!="ready") {
        status["state"]=pending_pin.empty()?state:"verifying_pin";status["message"]=state=="pin_required"?"SIM PIN required":
          state=="puk_required"?"SIM requires PUK. Unlock it in Android":
          state=="blocked"?"SIM is blocked":state=="absent"?"Selected SIM is not present":"Preparing SIM";
      }
      SaveLocked();
    }
    if(state=="ready")return resolved;
    if(state=="absent")throw std::runtime_error("Selected SIM is not present");
    if(state=="initializing"&&++initializing>120)throw std::runtime_error("SIM initialization timed out");
    std::string pin;
    {
      std::lock_guard<std::mutex> lock(state_mutex);
      if(pending_pin_request==request)pin.swap(pending_pin);
      pending_pin_request.clear();
    }
    if(!pin.empty()) {
      if(state!="pin_required") {ClearPin(pin);continue;}
      Bytes payload,info{static_cast<uint8_t>(selected->universal?3:1),static_cast<uint8_t>(pin.size())};
      info.insert(info.end(),pin.begin(),pin.end());ClearPin(pin);
      Tlv(payload,1,{static_cast<uint8_t>(selected->slot==1?6:7),0});Tlv(payload,2,info);
      std::fill(info.begin(),info.end(),0);
      Phase("verifying_pin","Verifying SIM PIN");
      std::string outcome="success";
      try {
        if(!identity.empty()&&SimIdentities(uim)[selected->slot]!=identity)
          throw std::runtime_error("SIM changed before PIN verification");
        uim.Call(0x26,payload,10,&cancel);
      }
      catch(const QmiError &){outcome="failed";}
      catch(const std::exception &){outcome="unknown";}
      std::fill(payload.begin(),payload.end(),0);
      {
        std::lock_guard<std::mutex> lock(state_mutex);
        status["pin_result"]["request"]=request;status["pin_result"]["id"]=identity;
        status["pin_result"]["outcome"]=outcome;SaveLocked();
      }
      // Even on a transport timeout, only query status; never resend a PIN
      CheckCancelled();
    }
    Delay(500);
  }
}
Session::~Session(){try{Stop();}catch(const std::exception &e){fprintf(stderr,"mobile cleanup: %s\n",e.what());}}

pid_t Session::Spawn(const std::string &name,const std::vector<std::string> &arguments) {
  pid_t pid=fork();if(pid<0)throw std::runtime_error("Cannot start modem helper");
  if(pid==0) {
    setsid();
    int log=open((std::string(kDirectory)+"/"+name+".log").c_str(),O_WRONLY|O_CREAT|O_APPEND|O_CLOEXEC,0600);
    int input=open("/dev/null",O_RDONLY);
    if(log<0||input<0)_exit(126);
    dup2(log,STDOUT_FILENO);dup2(log,STDERR_FILENO);dup2(input,STDIN_FILENO);
    setenv("AERA_CELLULAR_FIRMWARE",firmware.c_str(),1);
    std::string executable=std::string(kHelperDirectory)+"/"+name;
    std::vector<char *> argv{const_cast<char *>(executable.c_str())};
    for(const auto &arg:arguments)argv.push_back(const_cast<char *>(arg.c_str()));
    argv.push_back(nullptr);execv(executable.c_str(),argv.data());_exit(127);
  }
  return pid;
}
bool Session::Alive(pid_t pid) {
  int result=0;return pid>0&&waitpid(pid,&result,WNOHANG)==0;
}
void Session::CheckHelpers() {
  if(!Alive(rmtfs)||!Alive(tftp))throw std::runtime_error("A modem storage helper stopped unexpectedly");
}
void Session::PrepareFirmware() {
  // modem${slot} mounted at /firmware for WPSS/audio
  // modemfirmware${slot} (MSS: /image/modem.mdt). Don't replace the first
  char suffix[PROPERTY_VALUE_MAX]{};
  property_get("ro.boot.slot_suffix",suffix,"");
  if(strcmp(suffix,"_a")&&strcmp(suffix,"_b"))
    throw std::runtime_error("Invalid Peridot modem firmware boot slot");
  firmware_mount=std::string(kDirectory)+"/firmware";
  if(mkdir(firmware_mount.c_str(),0700)&&errno!=EEXIST)
    throw std::runtime_error("Cannot create isolated modem firmware mountpoint");
  // Do not stack a mount over firmware left by an interrupted session.
  std::ifstream mounts("/proc/mounts");
  std::string line;
  while(std::getline(mounts,line)) {
    std::istringstream entry(line);
    std::string source,target;
    if(entry>>source>>target && target==firmware_mount)
      throw std::runtime_error("Modem firmware is already mounted; reboot recovery before retrying");
  }
  const std::string block=std::string("/dev/block/bootdevice/by-name/modemfirmware")+suffix;
  struct stat source{};
  if(stat(block.c_str(),&source)||!S_ISBLK(source.st_mode))
    throw std::runtime_error("Peridot modemfirmware partition not found");
  if(mount(block.c_str(),firmware_mount.c_str(),"vfat",
           MS_RDONLY|MS_NODEV|MS_NOSUID|MS_NOEXEC,""))
    throw std::runtime_error("Cannot mount Peridot modem firmware read-only");
  firmware_mounted=true;
  firmware=firmware_mount+"/image";
  if(access((firmware+"/modem.mdt").c_str(),R_OK))
    throw std::runtime_error("Peridot modem firmware lacks image/modem.mdt");
  // Only after the dedicated MSS mount is validated, temporarily select
  // its firmware directory. Restore the original global lookup on cleanup
  old_firmware_path=Read("/sys/module/firmware_class/parameters/path");
  if(!Write("/sys/module/firmware_class/parameters/path",firmware))
    throw std::runtime_error("Cannot configure the modem firmware lookup path");
  firmware_path_changed=true;
}
Endpoint Session::Find(const std::vector<Endpoint> &endpoints,uint32_t service,uint32_t node) {
  for(const auto &e:endpoints)if(e.service==service&&(e.instance>>8)==0&&(node==UINT32_MAX||e.node==node))return e;
  throw std::runtime_error("Required modem service "+std::to_string(service)+" is unavailable");
}
void Session::Dns() {
  if(WifiConnected()) { RestoreDns();return; }
  // AERA's Wi-Fi-off path can retain a default route. Delete only routes
  // belonging to a WLAN whose carrier is down, never another active link.
  std::istringstream routes(Read("/proc/net/route"));std::string line;
  while(std::getline(routes,line)) {
    std::istringstream fields(line);std::string iface,destination,gateway;
    fields>>iface>>destination>>gateway;
    if(iface=="wlan0"&&destination=="00000000"&&Read("/sys/class/net/wlan0/carrier")!="1") {
      uint32_t raw=0;std::istringstream(gateway)>>std::hex>>raw;
      in_addr a{raw};char text[INET_ADDRSTRLEN];
      if(inet_ntop(AF_INET,&a,text,sizeof(text)))Ip({"route","del","default","via",text,"dev",iface},false);
    }
  }
  if(dns_owned)return;
  struct stat resolver{};
  const int existing=lstat("/etc/resolv.conf",&resolver);
  if((existing&&errno!=ENOENT)||(!existing&&!S_ISREG(resolver.st_mode)))
    throw std::runtime_error("Unsupported recovery resolver file");
  old_resolv_exists=existing==0;
  struct statfs fs{};
  // Only change resolver files in recovery RAM.
  if(statfs(old_resolv_exists?"/etc/resolv.conf":"/etc",&fs)||
      (static_cast<unsigned long>(fs.f_type)!=0x01021994UL&&static_cast<unsigned long>(fs.f_type)!=0x858458f6UL))
    throw std::runtime_error("Resolver configuration is not in volatile recovery memory");
  old_resolv=Read("/etc/resolv.conf");
  own_resolv="nameserver "+dns1+"\nnameserver "+dns2;
  char value[PROPERTY_VALUE_MAX];
  property_get("net.dns1",value,"");old_dns[0]=value;
  property_get("net.dns2",value,"");old_dns[1]=value;
  if(!Write("/etc/resolv.conf",own_resolv+"\n",O_WRONLY|O_CREAT|O_TRUNC,0644))
    throw std::runtime_error("Cannot configure mobile DNS");
  property_set("net.dns1",dns1.c_str());property_set("net.dns2",dns2.c_str());dns_owned=true;
}
void Session::RestoreDns() {
  if(!dns_owned)return;
  if(Read("/etc/resolv.conf")==own_resolv) {
    if(old_resolv_exists)Write("/etc/resolv.conf",old_resolv+"\n",O_WRONLY|O_TRUNC);
    // Keep the inode stable for read-only resolver mounts in plugin jails.
    else Write("/etc/resolv.conf","",O_WRONLY|O_TRUNC);
  }
  char value[PROPERTY_VALUE_MAX];
  property_get("net.dns1",value,"");if(value==dns1)property_set("net.dns1",old_dns[0].c_str());
  property_get("net.dns2",value,"");if(value==dns2)property_set("net.dns2",old_dns[1].c_str());
  dns_owned=false;
}
void Session::WatchNetwork() {
  // Route/DNS handover must not wait for NAS/WDS requests to finish
  network_worker=std::thread([this] {
    try {
      while(!network_stop.load()&&!cancel.load()) {
        Dns();
        for(int i=0;i<5&&!network_stop.load()&&!cancel.load();++i)
          std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
    }catch(const std::exception &error) {
      std::lock_guard<std::mutex> lock(network_mutex);network_error=error.what();
    }
  });
}
void Session::Signal() {
  bool registered=false;std::string rat;int dbm=0;
  Values serving, nr_system_info, nr_signal_info;
  bool nr_system_ok=false,nr_signal_ok=false;
  try { serving=nas->Call(0x24,{},3,&cancel); }
  catch(const QmiError &error) {
    if(error.code!=74)throw;
    CheckCancelled();
  }
  const auto &system=Field(serving,1);
  if(system.size()>=5&&system[4]<=system.size()-5) {
    registered=system[0]==1&&system[2]==1;
    for(size_t i=5;i<5u+system[4];++i) {
      switch(system[i]) {
        case 4:rat="2G";break;case 5:case 9:if(rat!="4G"&&rat!="5G")rat="3G";break;
        case 8:if(rat!="5G")rat="4G";break;case 12:rat="5G";break;default:break;
      }
    }
  }
  // Get System Info is read-only. NR service advertised/available is NOT
  // proof that a 5G secondary cell is carrying a data connection.
  try {nr_system_info=nas->Call(0x4d,{},3,&cancel);nr_system_ok=true;}
  catch(const std::exception &){CheckCancelled();}
  try {
    nr_signal_info=nas->Call(0x4f,{},3,&cancel);nr_signal_ok=true;
    const auto &info=nr_signal_info;
    if(rat=="5G"&&Field(info,0x17).size()>=2)dbm=static_cast<int16_t>(U16(Field(info,0x17).data()));
    else if(rat=="4G"&&Field(info,0x14).size()>=4)dbm=static_cast<int16_t>(U16(&Field(info,0x14)[2]));
    else if(rat=="3G"&&Field(info,0x13).size()>=1)dbm=static_cast<int8_t>(Field(info,0x13)[0]);
    else if(rat=="2G"&&Field(info,0x12).size()>=1)dbm=static_cast<int8_t>(Field(info,0x12)[0]);
  }catch(const std::exception &){CheckCancelled();}
  if(!registered&&handle&&wds) {
    // Legacy NAS may report detached even while LTE data is connected.
    if(!nr_system_ok) {
      try {nr_system_info=nas->Call(0x4d,{},3,&cancel);nr_system_ok=true;}
      catch(const std::exception &){CheckCancelled();}
    }
    const auto &lte=Field(nr_system_info,0x14);
    if(lte.size()>=2&&lte[0]==2&&lte[1]==2) {
      registered=true;
      if(rat.empty())rat="4G";
    }
  }
  const auto nr=ObserveNr(nr_system_ok?&nr_system_info:nullptr,
                          nr_signal_ok?&nr_signal_info:nullptr);
  // Only show 5G while NR signal measurements are valid and WDS is connected.
  // This does not establish which radio carried individual packets.
  if(nr.nr_signal_active&&handle&&wds) {
    registered=true;rat="5G";dbm=nr.rsrp_dbm;
  } else if(rat=="5G") {
    // Do not mislabel 5G because an NR service flag or legacy RAT was set.
    rat=nr.lte_service?"4G":"";
    const auto &lte_signal=Field(nr_signal_info,0x14);
    dbm=rat=="4G"&&lte_signal.size()>=4?
        static_cast<int16_t>(U16(lte_signal.data()+2)):0;
  }
  if(rat=="4G"&&dbm==0) {
    const auto &lte_signal=Field(nr_signal_info,0x14);
    if(lte_signal.size()>=4)dbm=static_cast<int16_t>(U16(lte_signal.data()+2));
  }
  const bool valid=registered&&dbm>=-160&&dbm<=-20;
  int bars=-1;
  if(valid) {
    const bool rsrp=rat=="4G"||rat=="5G";
    bars=rsrp?(dbm>=-95?4:dbm>=-105?3:dbm>=-115?2:dbm>=-125?1:0):
               (dbm>=-75?4:dbm>=-85?3:dbm>=-95?2:dbm>=-105?1:0);
  }
  const std::string nr_summary=nr.Summary();
  bool changed=false;
  {
    std::lock_guard<std::mutex> lock(state_mutex);
    changed=status.get("nr_summary","").asString()!=nr_summary;
    status["nr_sample_count"]=status.get("nr_sample_count",0).asUInt()+1;
    status["nr_summary"]=nr_summary;
    status["nr_system_query_ok"]=nr.system_query_ok;
    status["nr_signal_query_ok"]=nr.signal_query_ok;
    status["nr_lte_service"]=nr.lte_service;
    status["nr_service"]=nr.nr_service;status["nr_true_service"]=nr.nr_true_service;
    status["nr_nsa_advertised"]=nr.eutra_nr;
    status["nr_dcnr_restricted"]=nr.dcnr_restricted;
    status["nr_signal_tlv"]=nr.nr_signal_tlv;status["nr_active"]=nr.nr_signal_active;
    status["nr_packet_domain"]=nr.nr_packet_domain;
    status["nr_forbidden"]=nr.nr_forbidden;status["nr_reject"]=nr.nr_reject;
    if(nr.nr_signal_active)status["nr_rsrp_dbm"]=nr.rsrp_dbm;
    else status.removeMember("nr_rsrp_dbm");
    if(nr.snr_valid)status["nr_snr_tenths_db"]=nr.snr_tenths;
    else status.removeMember("nr_snr_tenths_db");
    if(nr.rsrq_valid)status["nr_rsrq_db"]=nr.rsrq_db;
    else status.removeMember("nr_rsrq_db");
    status["registered"]=registered;status["technology"]=registered?rat:"";
    status["signal_valid"]=valid;status["signal_dbm"]=valid?dbm:0;status["bars"]=bars;
    const auto &operator_info=Field(serving,0x12);
    if(operator_info.size()>=5&&operator_info[4]<=operator_info.size()-5)
      status["operator"]=std::string(operator_info.begin()+5,operator_info.begin()+5+operator_info[4]);
    SaveLocked();
  }
  if(changed)Log("NR status: "+nr_summary);
}
// Never log raw modem identity or cell information.
bool Session::LteServiceAvailable() {
  const auto info=nas->Call(0x4d,{},5,&cancel);
  const auto &lte=Field(info,0x14);
  const auto &v2=Field(info,0x19);
  // If the LTE v2 TLV is present, require its complete non-identifying
  // fixed header before checking domain, forbidden and rejection flags.
  if(!v2.empty()&&v2.size()<19)
    throw std::runtime_error("Truncated LTE system information");
  const bool service=lte.size()>=2&&lte[0]==2&&lte[1]==2;
  bool domain_ok=true,capability_ok=true,forbidden=false,rejected=false;
  if(v2.size()>=2&&v2[0])domain_ok=v2[1]==2||v2[1]==3;
  if(v2.size()>=4&&v2[2])capability_ok=v2[3]==2||v2[3]==3;
  if(v2.size()>=8&&v2[6])forbidden=v2[7]!=0;
  // Do not log PLMN, TAC, cell IDs or raw NAS fields.
  if(v2.size()>=19&&v2[16])rejected=true;
  Log("LTE service check: available="+std::to_string(service)+
              " v2="+(v2.empty()?std::string("absent"):std::string("present"))+
              " packet-domain="+std::to_string(domain_ok)+
              " packet-capability="+std::to_string(capability_ok)+
              " forbidden="+std::to_string(forbidden)+
              " rejection-indicator="+std::to_string(rejected));
  bool emergency_only=false;
  try {
    const auto preference=nas->Call(0x34,{},5,&cancel);
    const auto &emergency=Field(preference,0x10);
    emergency_only=emergency.size()==1&&emergency[0]!=0;
    Log(std::string("Network preference: emergency-only=")+
                (emergency.empty()?"unknown":emergency_only?"yes":"no")+
                "");
  }catch(const std::exception &) {
    CheckCancelled();
    Log("Network selection preference unavailable");
  }
  return service&&domain_ok&&capability_ok&&!forbidden&&!rejected&&!emergency_only;
}
void Session::BindSubscription(int subscription) {
  if(subscription!=1&&subscription!=2)throw std::runtime_error("Invalid data subscription");
  Bytes binding;Tlv(binding,1,Words({static_cast<uint32_t>(subscription)}));
  wds->Call(0xaf,binding,6,&cancel);
  const auto bound=wds->Call(0xb0,{},6,&cancel);
  const auto &selected=Field(bound,0x10);
  if(selected.size()!=4||U32(selected.data())!=static_cast<uint32_t>(subscription))
    throw std::runtime_error("The modem did not select the requested data SIM");
  Bytes nas_binding;Tlv(nas_binding,1,{static_cast<uint8_t>(subscription-1)});
  nas->Call(0x45,nas_binding,6,&cancel);
}
void Session::StartModem() {
  Log("Acquiring AERA storage-operation lease");
  lease.reset(open(kLease,O_RDWR|O_CREAT|O_CLOEXEC|O_NOFOLLOW,0600));
  if(lease.get()<0||flock(lease.get(),LOCK_SH|LOCK_NB))throw std::runtime_error("Finish the current storage operation first");
  const auto reason=Capability();if(!reason.empty())throw std::runtime_error(reason);
  modem=ModemPath();
  if(Read(modem+"/state")!="offline")throw std::runtime_error("The modem is already running outside AERA mobile data; reboot recovery first");
  Log("Loading the three RMNET modules (no network routes changed)");
  Module("rmnet_ctl");Module("ipanetm");Module("rmnet_core");
  Log("Mounting the active-slot modemfirmware partition read-only");
  PrepareFirmware();CheckCancelled();
  Log("Validated image/modem.mdt; starting RMTFS and TFTP helpers");
  rmtfs=Spawn("aera-mobile-rmtfs",{"-r","-P","-o","/dev/block/bootdevice/by-name"});
  tftp=Spawn("aera-mobile-tftp",{"-d"});
  Delay(700);CheckHelpers();
  // Storage/RFS services must be registered before remoteproc boots firmware.
  bool ready=false;
  for(int attempt=0;attempt<10&&!ready;++attempt) {
    CheckHelpers();
    bool rmt=false,tf=false;
    try {
      for(const auto &e:Discover(&cancel,true)) {
        rmt|=e.service==14;tf|=e.service==4096;
      }
    } catch(const std::exception &e) {
      CheckCancelled();
      if(attempt==9)Log(std::string("QRTR storage discovery: ")+e.what());
    }
    ready=rmt&&tf;
    if(!ready)Delay(200);
  }
  if(!ready)throw std::runtime_error("Modem storage services are not ready");
  Log("QRTR RMTFS (14) and TFTP (4096) endpoints published");
  // Own the modem before the start request: even a partial start needs a
  // controlled stop before any helper or read-only firmware mount is freed.
  modem_owned=true;
  Log("Requesting integrated MSS remoteproc startup");
  if(!Write(modem+"/state","start"))throw std::runtime_error("Cannot start the modem");
}
// Verify LTE service before using a WDS session when legacy NAS reports detached.
void Session::Connect(const std::string &apn,int subscription) {
  Phase("starting","Preparing the mobile modem");
  ConnectData(apn,subscription);
}
void Session::StopChild(pid_t &pid) {
  if(pid<=0)return;
  kill(pid,SIGTERM);
  for(int i=0;i<40;++i) {
    int result=0;if(waitpid(pid,&result,WNOHANG)!=0){pid=-1;return;}
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  kill(pid,SIGKILL);while(waitpid(pid,nullptr,0)<0&&errno==EINTR){}pid=-1;
}
void Session::Stop() {
  network_stop.store(true);
  if(network_worker.joinable())network_worker.join();
  if(wds&&handle) {
    try{Bytes payload;Tlv(payload,1,Words({handle}));wds->Call(0x21,payload,5);}catch(const std::exception &e){fprintf(stderr,"Stop data: %s\n",e.what());}
    handle=0;
  }
  RestoreDns();
  if(link_owned){Ip({"link","del",kInterface},false);link_owned=false;}
  wds.reset();wda.reset();dpm.reset();nas.reset();
  if(modem_owned) {
    if(Read(modem+"/state")!="offline")Write(modem+"/state","stop");
    for(int i=0;i<200&&Read(modem+"/state")!="offline";++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    if(Read(modem+"/state")!="offline")throw std::runtime_error("Modem shutdown failed. Reboot recovery before flashing");
    modem_owned=false;
  }
  StopChild(tftp);StopChild(rmtfs);
  if(firmware_path_changed) {
    // Never unmount a directory still selected by the global firmware
    // loader: WPSS/ADSP may need that lookup path later in recovery.
    if(Read("/sys/module/firmware_class/parameters/path")==firmware &&
       !Write("/sys/module/firmware_class/parameters/path",
              old_firmware_path.empty()?"\n":old_firmware_path))
      throw std::runtime_error("Cannot restore the global firmware lookup path");
    firmware_path_changed=false;
  }
  if(firmware_mounted){if(umount(firmware_mount.c_str()))throw std::runtime_error("Cannot release modem firmware mount");firmware_mounted=false;}
  lease.reset();
}
void Session::ConnectData(const std::string &apn,int subscription) {
  Log("Starting cellular data session");
  StartModem();
  bool running=false;
  for(int i=0;i<80;++i) {
    CheckCancelled();CheckHelpers();
    const auto state=Read(modem+"/state");
    if(state=="running") {running=true;break;}
    if(state=="crashed")throw std::runtime_error("MSS crashed during startup");
    Delay(250);
  }
  if(!running)throw std::runtime_error("Modem startup timed out");
  Log("MSS running; discovering QMI DMS/NAS/WDS/UIM/WDA/DPM");
  std::vector<Endpoint> endpoints;Endpoint nase;
  bool found=false;
  for(int attempt=0;attempt<24&&!found;++attempt) {
    CheckCancelled();CheckHelpers();
    try {
      endpoints=Discover(&cancel);
      nase=Find(endpoints,3);
      for(uint32_t service:{1u,2u,11u,26u,47u})Find(endpoints,service,nase.node);
      found=true;
    }catch(const std::exception &) {
      CheckCancelled();if(attempt==23)throw;
      Delay(500);
    }
  }
  if(!found)throw std::runtime_error("Modem QMI data services unavailable");
  QmiClient dms(Find(endpoints,2,nase.node));
  Bytes online;Tlv(online,1,{0});dms.Call(0x2e,online,12,&cancel);
  Log("DMS online request succeeded");
  nas=std::make_unique<QmiClient>(nase);
  wds=std::make_unique<QmiClient>(Find(endpoints,1,nase.node));
  const auto uime=Find(endpoints,11,nase.node);
  Phase("initializing","Preparing the selected SIM");
  const int selected_subscription=WaitForSim(uime,subscription);
  int selected_physical_slot=0;
  {
    std::lock_guard<std::mutex> lock(state_mutex);
    selected_physical_slot=status.get("sim_slot",0).asInt();
    status["active_subscription"]=selected_subscription;
    SaveLocked();
  }
  Log("UIM ready: logical subscription "+std::to_string(selected_subscription)+
              ", physical SIM slot "+std::to_string(selected_physical_slot));
  Log("Binding WDS and NAS to selected subscription");
  BindSubscription(selected_subscription);
  Log("Waiting for packet-service registration");
  Phase("registering","Checking carrier LTE service");
  bool attached=false,registration_denied=false;
  const int max_attempts=12;
  for(int attempt=0;attempt<max_attempts&&!attached;++attempt) {
    CheckCancelled();CheckHelpers();
    std::string nas_summary="serving-system query unavailable";
    try {
      auto serving=nas->Call(0x24,{},4,&cancel);
      const auto &system=Field(serving,1);
      attached=system.size()>=3&&system[0]==1&&system[2]==1;
      registration_denied=system.size()>=1&&system[0]==3;
      nas_summary="registration="+std::to_string(system.empty()?255:system[0])+
                  " packet="+std::to_string(system.size()>2?system[2]:255);
    }catch(const QmiError &error) {
      if(error.code!=74)throw;
      CheckCancelled();
      nas_summary="QMI NAS temporarily not ready (error 74)";
    }
    if(registration_denied)throw std::runtime_error("NAS explicitly reports registration denied");
    if(!attached) {
      if(attempt==0||attempt%10==9) {
        Log("Awaiting NAS registration (attempt "+std::to_string(attempt+1)+
                    "/"+std::to_string(max_attempts)+"): "+nas_summary);
      }
      Delay(750);
    }
  }
  if(attached)Log("NAS reports registered packet service");
  else {
    Log("Legacy NAS reports detached; checking current LTE service");
    if(!LteServiceAvailable())
      throw std::runtime_error("Carrier LTE service is unavailable or restricted");
    Log("LTE service confirmed; configuring IPA data channel");
  }
  if(!if_nametoindex("rmnet_ipa0"))
    throw std::runtime_error("rmnet_ipa0 is absent after RMNET module loading");
  Log("Querying IPA endpoint");
  const auto endpoint=Ipa({3,0,0,0,0,0});
  const auto pipes=Ipa({0x10,0,0,0,0,0});
  Log("IPA endpoint ready");
  dpm=std::make_unique<QmiClient>(Find(endpoints,47,nase.node));
  Bytes hardware{1};Append(hardware,Words({4,endpoint[1],pipes[1],pipes[2]}));
  Bytes dpm_request;Tlv(dpm_request,0x11,hardware);
  Log("Opening DPM hardware data port");
  dpm->Call(0x20,dpm_request,12,&cancel);
  Log("DPM open-data-port succeeded");
  wda=std::make_unique<QmiClient>(Find(endpoints,26,nase.node));
  Bytes format;Tlv(format,0x10,{0});
  for(const auto &v:std::vector<std::pair<uint8_t,uint32_t>>{
        {0x11,2},{0x12,9},{0x13,9},{0x15,32},{0x16,16384},{0x1b,32},{0x1c,16384}})
    Tlv(format,v.first,Words({v.second}));
  Tlv(format,0x17,Words({4,endpoint[1]}));
  Log("Negotiating WDA QMAP data format");
  const auto negotiated=wda->Call(0x20,format,12,&cancel);
  for(uint8_t tag:{0x11,0x12,0x13}) {
    const auto &value=Field(negotiated,tag);
    if(value.size()!=4||U32(value.data())!=(tag==0x11?2u:9u))
      throw std::runtime_error("WDA returned an incompatible data format");
  }
  Log("WDA data-format negotiation succeeded");
  Bytes binding;Tlv(binding,0x10,Words({4,endpoint[1]}));Tlv(binding,0x11,{1});
  Log("Binding WDS to IPA data endpoint");
  wds->Call(0xa2,binding,12,&cancel);
  Bytes family;Tlv(family,1,{4});wds->Call(0x4d,family,12,&cancel);
  Log("WDS endpoint binding and IPv4 selection succeeded");
  std::string effective_apn=apn;
  if(effective_apn.empty()) {
    // Permit a carrier-configured attach APN, but never use an arbitrary
    // unvalidated modem string in a QMI request.
    try {
      const auto attach=wds->Call(0x85,{},5,&cancel);
      const auto &value=Field(attach,0x10);
      if(!value.empty()&&value.size()<=100) {
        const std::string candidate(value.begin(),value.end());
        if(candidate.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-_")==std::string::npos)
          effective_apn=candidate;
      }
    }catch(const std::exception &){CheckCancelled();}
    if(effective_apn.empty())
      throw std::runtime_error("Enter the carrier APN in AERA mobile-data settings");
  }
  {
    std::lock_guard<std::mutex> lock(state_mutex);
    status["effective_apn"]=effective_apn;
    SaveLocked();
  }
  Phase("connecting","Establishing the cellular data session");
  Bytes start;Tlv(start,0x14,Bytes(effective_apn.begin(),effective_apn.end()));Tlv(start,0x19,{4});
  Log("Starting IPv4 data session");
  const auto reply=wds->Call(0x20,start,45,&cancel);
  const auto &session_handle=Field(reply,1);
  if(session_handle.size()!=4)throw std::runtime_error("WDS returned no data handle");
  handle=U32(session_handle.data());
  Log("WDS returned a live data-session handle");
  const auto link_state=wds->Call(0x22,{},4,&cancel);
  if(Field(link_state,1).empty()||Field(link_state,1)[0]!=2)
    throw std::runtime_error("WDS data session is not connected");
  Log("WDS packet-service state is connected");
  Bytes settings_request;Tlv(settings_request,0x10,Words({0x2310}));
  const auto settings=wds->Call(0x2d,settings_request,12,&cancel);
  const auto address=Address(settings,0x1e);
  const auto gateway=Address(settings,0x20);
  const auto netmask=Address(settings,0x21);
  dns1=Address(settings,0x15);dns2=Address(settings,0x16);
  if(address.empty()||gateway.empty()||netmask.empty()||dns1.empty()||dns1=="0.0.0.0")
    throw std::runtime_error("WDS returned incomplete IPv4 parameters");
  int mtu=1500;const auto &mtu_field=Field(settings,0x29);
  if(mtu_field.size()==4)mtu=U32(mtu_field.data());
  if(mtu<576||mtu>9000)throw std::runtime_error("Invalid cellular MTU");
  if(dns2.empty()||dns2=="0.0.0.0")dns2=dns1;
  Log("Received IPv4 configuration");
  Log("Creating RMNET interface");
  CreateLink();link_owned=true;
  Ip({"link","set","rmnet_ipa0","up"});
  Ip({"link","set",kInterface,"mtu",std::to_string(mtu),"up"});
  Ip({"addr","add",address+"/"+std::to_string(Prefix(netmask)),"dev",kInterface});
  Log("RMNET interface and IPv4 address configured");
  Log("Completing kernel IPA data-channel configuration");
  Ipa({6,0x1e,0,0,0,0});Ipa({7,0x3e,16384,32,0,0});
  IpaData channel{};channel[0]=5;channel[1]=1;
  memcpy(&channel[2],kInterface,strlen(kInterface)+1);Ipa(channel);
  // Prefer Wi-Fi while retaining the cellular route for fallback.
  Ip({"route","add","default","via",gateway,"dev",kInterface,"metric","30000"});
  Dns();
  {std::lock_guard<std::mutex> lock(state_mutex);
    status["connected"]=true;status["interface"]=kInterface;
    status["address"]=address;status["gateway"]=gateway;
    status["dns1"]=dns1;status["dns2"]=dns2;status["mtu"]=mtu;
    status["technology"]="4G";SaveLocked();}
  established=true;
  Phase("connected","Mobile data connected");
  Log("Cellular connected; Wi-Fi remains preferred");
  WatchNetwork();
  while(!cancel.load()) {
    {std::lock_guard<std::mutex> lock(network_mutex);
      if(!network_error.empty())throw std::runtime_error(network_error);}
    CheckHelpers();
    if(Read(modem+"/state")!="running")throw std::runtime_error("The modem stopped");
    const auto current=wds->Call(0x22,{},4,&cancel);
    if(Field(current,1).empty()||Field(current,1)[0]!=2)
      throw std::runtime_error("Mobile data bearer disconnected");
    Signal();
    Delay(2500);
  }
  return;

}

}
