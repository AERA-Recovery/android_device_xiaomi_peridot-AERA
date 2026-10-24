/* SPDX-License-Identifier: Apache-2.0 */
#include "session.hpp"

namespace aera::mobile::peridot {
void OnSignal(int);
void Worker(std::string apn,int subscription) {
  std::string failure;
  bool was_connected=false;
  unsigned retry_seconds=5;
  while(!cancel.load()) {
    Session session;
    try{session.Connect(apn,subscription);}catch(const std::exception &e){if(!cancel.load())failure=e.what();}
    was_connected|=session.established;
    if(session.established)retry_seconds=5;
    Phase("stopping","Disconnecting mobile data");
    // Keep modem storage helpers and the lease until teardown completes.
    bool cleanup_had_error=false;
    std::string last_cleanup_error;
    for(;;) {
      try {session.Stop();break;}
      catch(const std::exception &e) {
        cleanup_had_error=true;
        failure=std::string("Mobile cleanup incomplete: ")+e.what();
        if(failure!=last_cleanup_error) {
          last_cleanup_error=failure;
          Phase("error",failure+". Do not flash; reboot recovery if this persists");
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
      }
    }
    {std::lock_guard<std::mutex> lock(state_mutex);ClearConnectionLocked();}
    // Do not automatically start another MSS session after an uncertain
    // cleanup. Require a fresh user request once the modem is fully offline.
    if(cancel.load()||cleanup_had_error||!was_connected)break;
    Phase("waiting","Mobile connection lost. Retrying in "+std::to_string(retry_seconds)+" seconds");
    try{Delay(retry_seconds*1000);}catch(const std::exception &){break;}
    retry_seconds=std::min(60u,retry_seconds*2);
  }
  {std::lock_guard<std::mutex> lock(state_mutex);status["enabled"]=false;status["connected"]=false;
    ClearPin(pending_pin);pending_pin_request.clear();status["pin_request"]="";
    status["registered"]=false;status["signal_valid"]=false;status["bars"]=-1;status["technology"]="";status["address"]="";}
  if(cancel.load())failure.clear();
  Phase(failure.empty()?"off":"error",failure.empty()?"Mobile data is off":failure);
  {std::lock_guard<std::mutex> lock(completion_mutex);working.store(false);}completion.notify_all();
}
bool Disconnect() {
  cancel.store(true);
  std::unique_lock<std::mutex> lock(completion_mutex);
  return completion.wait_for(lock,std::chrono::seconds(60),[]{return !working.load();});
}
void OnSignal(int){cancel.store(true);exiting.store(true);}

int Main(int argc,char **argv) {
  // Read the resident provider status without contacting the modem.
  if(argc==2&&std::string(argv[1])=="--nr-report") {
    Json::CharReaderBuilder reader;Json::Value snapshot;std::string error;
    std::istringstream data(Read(kStatus));
    if(!Json::parseFromStream(reader,data,&snapshot,&error)||!snapshot.isObject()) {
      puts("No cellular provider status yet; enable mobile data in AERA first.");return 2;
    }
    if(!snapshot.get("connected",false).asBool()) {
      puts("Mobile data is not connected; enable it in AERA before testing 5G.");return 2;
    }
    if(snapshot.get("nr_sample_count",0).asUInt()==0) {
      puts("Waiting for first read-only NR sample; repeat in a few seconds.");return 2;
    }
    const auto tri=[&snapshot](const char *name)->const char * {
      const Json::Value &v=snapshot[name];
      return !v.isInt()?"not reported":v.asInt()==0?"no":v.asInt()==1?"yes":"unknown";
    };
    printf("AERA cellular NR5G report (read-only; sample %u)\n",
           snapshot["nr_sample_count"].asUInt());
    printf("Interpretation: %s\n",snapshot.get("nr_summary","unavailable").asString().c_str());
    printf("NAS system-info response: %s\n",snapshot.get("nr_system_query_ok",false).asBool()?"yes":"no");
    printf("NAS signal-info response: %s\n",snapshot.get("nr_signal_query_ok",false).asBool()?"yes":"no");
    printf("LTE service available: %s\n",snapshot.get("nr_lte_service",false).asBool()?"yes":"no/not reported");
    const int svc=snapshot.get("nr_service",-1).asInt();
    const int true_svc=snapshot.get("nr_true_service",-1).asInt();
    printf("NR5G service/true-service codes: %d / %d (-1=not reported; 2=available)\n",svc,true_svc);
    printf("LTE-with-NR (NSA) advertised: %s\n",tri("nr_nsa_advertised"));
    printf("DCNR restricted: %s\n",tri("nr_dcnr_restricted"));
    printf("Active NR RSRP measurement: %s\n",snapshot.get("nr_active",false).asBool()?"yes":"no/not reported");
    if(snapshot.isMember("nr_rsrp_dbm"))printf("NR RSRP: %d dBm\n",snapshot["nr_rsrp_dbm"].asInt());
    if(snapshot.isMember("nr_snr_tenths_db"))
      printf("NR SNR: %.1f dB\n",snapshot["nr_snr_tenths_db"].asInt()/10.0);
    if(snapshot.isMember("nr_rsrq_db"))printf("NR RSRQ: %d dB\n",snapshot["nr_rsrq_db"].asInt());
    puts("This measures NR radio evidence; it does not prove 5G throughput or SA/NSA mode.");
    return 0;
  }
  // Read-only prerequisite check for maintainers.
  if(argc==2&&std::string(argv[1])=="--probe") {
    char recovery[PROPERTY_VALUE_MAX]{};
    char sku[PROPERTY_VALUE_MAX]{};
    char slot[PROPERTY_VALUE_MAX]{};
    property_get("ro.aera.boot",recovery,"");
    property_get("ro.boot.hardware.sku",sku,"");
    property_get("ro.boot.slot_suffix",slot,"");
    bool ok=strcmp(recovery,"1")==0&&strcmp(sku,"peridot")==0;
    printf("AERA recovery / peridot: %s\n",ok?"yes":"no");
    const auto modem=ModemPath();
    const bool mss=!modem.empty()&&Read(modem+"/state")=="offline";
    printf("Offline integrated MSS: %s\n",mss?"yes":"no");ok&=mss;
    const bool uio=HasRmtfsUio();
    printf("RMTFS UIO interface: %s\n",uio?"yes":"no");ok&=uio;
    Fd transport(socket(42,SOCK_DGRAM|SOCK_CLOEXEC,0));
    const bool qrtr=transport.get()>=0;
    printf("QRTR socket: %s\n",qrtr?"yes":"no");ok&=qrtr;
    for(const auto *helper:{"aera-mobile-rmtfs","aera-mobile-tftp"}) {
      const bool present=access((std::string(kHelperDirectory)+"/"+helper).c_str(),X_OK)==0;
      printf("%s: %s\n",helper,present?"installed":"missing");ok&=present;
    }
    const bool valid_slot=!strcmp(slot,"_a")||!strcmp(slot,"_b");
    const std::string fw=std::string("/dev/block/bootdevice/by-name/modemfirmware")+slot;
    struct stat firmware{};
    const bool have_fw=valid_slot&&stat(fw.c_str(),&firmware)==0&&S_ISBLK(firmware.st_mode);
    printf("Active-slot modemfirmware partition: %s\n",have_fw?"present":"missing");ok&=have_fw;
    for(const auto *module:{"rmnet_ctl","ipanetm","rmnet_core"}) {
      bool found=access((std::string("/sys/module/")+module).c_str(),F_OK)==0;
      for(const auto *root:{"/tmp/vendor_dlkm/lib/modules","/vendor_dlkm/lib/modules",
            "/tmp/vendor/lib/modules","/vendor/lib/modules","/lib/modules"})
        found|=access((std::string(root)+"/"+module+".ko").c_str(),R_OK)==0;
      printf("%s module: %s\n",module,found?"available":"missing");ok&=found;
    }
    puts("AERA mobile data provider available; use --nr-report for live NR status.");
    return ok?0:1;
  }
  if(argc==4&&std::string(argv[1])=="connect") {
    Json::Value config;config["apn"]=argv[2];
    if(strlen(argv[3])!=1||argv[3][0]<'0'||argv[3][0]>'2')return 2;
    config["subscription"]=argv[3][0]-'0';
    Json::StreamWriterBuilder builder;builder["indentation"]="";
    std::string reply;
    bool ok=Request("connect "+Json::writeString(builder,config),&reply)&&reply=="OK";
    fprintf(stdout,"%s\n",reply.c_str());return ok?0:1;
  }
  if(argc>1&&std::string(argv[1])=="disconnect") {
    std::string reply;return Request("disconnect",&reply,65000)&&reply=="OK"?0:1;
  }
  if(argc!=1)return 2;
  // Peridot reports ro.bootmode=unknown in recovery; check the AERA marker.
  char mode[PROPERTY_VALUE_MAX]{},sku[PROPERTY_VALUE_MAX]{};
  property_get("ro.aera.boot",mode,"");
  property_get("ro.boot.hardware.sku",sku,"");
  if(strcmp(mode,"1")||strcmp(sku,"peridot"))return 1;
  umask(0077);
  if(mkdir(kDirectory,0700)&&errno!=EEXIST)return 1;
  struct statfs fs{};if(statfs(kDirectory,&fs)||
      (static_cast<unsigned long>(fs.f_type)!=0x01021994UL&&static_cast<unsigned long>(fs.f_type)!=0x858458f6UL))return 1;
  Fd instance(open("/tmp/aera-cellular-daemon.lock",O_RDWR|O_CREAT|O_CLOEXEC|O_NOFOLLOW,0600));
  if(instance.get()<0||flock(instance.get(),LOCK_EX|LOCK_NB))return 1;
  Fd server(socket(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC|SOCK_NONBLOCK,0));
  sockaddr_un address{};address.sun_family=AF_UNIX;strcpy(address.sun_path,kSocket);
  unlink(kSocket);
  if(server.get()<0||bind(server.get(),reinterpret_cast<sockaddr *>(&address),sizeof(address))||listen(server.get(),4))return 1;
  chmod(kSocket,0600);signal(SIGPIPE,SIG_IGN);signal(SIGTERM,OnSignal);signal(SIGINT,OnSignal);
  const auto capability=Capability();
  status["available"]=capability.empty();
  status["enabled"]=false;status["connected"]=false;status["bars"]=-1;
  Phase("off",capability.empty()?"Mobile data is off":capability);
  std::thread worker;
  while(!exiting.load()) {
    pollfd p{server.get(),POLLIN,0};if(poll(&p,1,250)<=0)continue;
    Fd client(accept4(server.get(),nullptr,nullptr,SOCK_CLOEXEC));if(client.get()<0)continue;
    ucred credentials{};socklen_t length=sizeof(credentials);
    if(getsockopt(client.get(),SOL_SOCKET,SO_PEERCRED,&credentials,&length)||credentials.uid!=0)continue;
    pollfd request_poll{client.get(),POLLIN,0};if(poll(&request_poll,1,250)<=0)continue;
    char buffer[1024];ssize_t count=recv(client.get(),buffer,sizeof(buffer),MSG_TRUNC);
    if(count<=0||count>=static_cast<ssize_t>(sizeof(buffer)))continue;
    std::string command(buffer,count),reply="ERROR";
    if(command=="disconnect")reply=Disconnect()?"OK":"Modem is still stopping. Reboot before flashing";
    else if(command=="disconnect-async"){cancel.store(true);reply="OK";}
    else if(command.rfind("verify-pin ",0)==0) {
      Json::CharReaderBuilder reader;Json::Value config;std::string error;
      std::istringstream input(command.substr(11));
      if(Json::parseFromStream(reader,input,&config,&error)&&config.isObject()&&
          config["pin"].isString()&&config["request"].isString()&&config["id"].isString()) {
        std::string pin=config["pin"].asString();
        std::lock_guard<std::mutex> lock(state_mutex);
        if(working.load()&&!cancel.load()&&status["state"]=="pin_required"&&pending_pin.empty()&&
            status["pin_request"]==config["request"]&&status["sim_id"]==config["id"]&&
            ValidPin(pin)&&status.get("pin_retries",0).asUInt()>0) {
          pending_pin.swap(pin);pending_pin_request=config["request"].asString();
          status["pin_attempt"]=status.get("pin_attempt",0).asUInt()+1;
          status["state"]="verifying_pin";status["message"]="Verifying SIM PIN";
          status.removeMember("pin_result");SaveLocked();reply="OK";
        }else reply="SIM state changed. Check the selected SIM and try again";
        ClearPin(pin);
      }
      ClearPin(command);memset(buffer,0,sizeof(buffer));
    }
    else if(command.rfind("connect ",0)==0) {
      Json::CharReaderBuilder reader;Json::Value config;std::string error;
      std::istringstream input(command.substr(8));
      if(Json::parseFromStream(reader,input,&config,&error)&&config.isObject()&&
          config["apn"].isString()&&config["subscription"].isInt()) {
        std::string apn=config["apn"].asString();int subscription=config["subscription"].asInt();
        bool valid=apn.size()<=100&&subscription>=0&&subscription<=2;
        for(unsigned char c:apn)valid&=(c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='.'||c=='-'||c=='_';
        if(valid&&working.load()) {
          // Reusing an active APN and subscription must not start another modem session.
          std::lock_guard<std::mutex> lock(state_mutex);
          const auto configured_apn=status.get("apn","").asString();
          const auto effective_apn=status.get("effective_apn","").asString();
          const int configured_sim=status.get("subscription",0).asInt();
          const int active_sim=status.get("active_subscription",0).asInt();
          const bool same_apn=apn==configured_apn||(!apn.empty()&&apn==effective_apn);
          const bool same_sim=subscription==configured_sim||
              (subscription!=0&&subscription==active_sim);
          reply=same_apn&&same_sim&&status.get("enabled",false).asBool()?"OK":
              "Mobile data is already active or starting. Disconnect before changing APN or SIM";
        }else if(valid) {
          if(worker.joinable())worker.join();
          cancel.store(false);working.store(true);
          {std::lock_guard<std::mutex> lock(state_mutex);ClearConnectionLocked();status["enabled"]=true;status["subscription"]=subscription;status["apn"]=apn;
            ClearPin(pending_pin);pending_pin_request.clear();status["pin_request"]="";
            status["sim_state"]="";status["sim_slot"]=0;status["sim_id"]="";status["pin_attempt"]=0;
            status.removeMember("pin_result");status["sims"]=Json::Value(Json::arrayValue);SaveLocked();}
          worker=std::thread(Worker,apn,subscription);reply="OK";
        }else reply="Invalid APN or SIM selection";
      }else reply="Invalid connect request";
    }
    send(client.get(),reply.data(),reply.size(),MSG_NOSIGNAL);
  }
  Disconnect();if(worker.joinable())worker.join();unlink(kSocket);return 0;
}
}
int main(int argc,char **argv){return aera::mobile::peridot::Main(argc,argv);}
