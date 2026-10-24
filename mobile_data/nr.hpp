/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
// Qualcomm NAS service flags and NR signal measurements.
#include "qmi.hpp"

namespace aera::mobile {
struct NrObservation {
  bool system_query_ok=false, signal_query_ok=false;
  bool lte_service=false;
  int nr_service=-1;          // 0 none, 1 limited, 2 available; -1 not reported
  int nr_true_service=-1;
  int eutra_nr=-1;            // LTE anchor advertises NR (NSA capability)
  int dcnr_restricted=-1;     // 1 means NR dual connectivity restricted
  bool nr_signal_tlv=false, nr_signal_active=false;
  int rsrp_dbm=0, snr_tenths=0, rsrq_db=0;
  bool snr_valid=false, rsrq_valid=false;
  bool nr_packet_domain=false, nr_forbidden=false, nr_reject=false;

  std::string Summary() const {
    if(nr_signal_active)
      return lte_service?"NR radio measured with LTE service (NSA evidence)":
                         "NR radio measured (NSA/SA not determined)";
    if(nr_service==2&&nr_true_service==2)
      return "NR service reported available; active NR radio not demonstrated";
    if(dcnr_restricted==1)
      return "LTE-to-NR dual connectivity restricted";
    if(eutra_nr==1)
      return "LTE advertises NR capability; NR radio not active in this sample";
    if(system_query_ok&&signal_query_ok)
      return "No active NR evidence in this sample (not proof 5G is unsupported)";
    return "Insufficient NAS responses to assess 5G";
  }
};

inline NrObservation ObserveNr(const Values *system, const Values *signal) {
  NrObservation out;
  if(system) {
    out.system_query_ok=true;
    const auto &lte=Field(*system,0x14);
    out.lte_service=lte.size()>=2&&lte[0]==2&&lte[1]==2;
    const auto &nr=Field(*system,0x4a);
    if(nr.size()>=2) {out.nr_service=nr[0];out.nr_true_service=nr[1];}
    const auto &nsa=Field(*system,0x4e);
    if(nsa.size()==1&&nsa[0]<=1)out.eutra_nr=nsa[0];
    const auto &restricted=Field(*system,0x4f);
    if(restricted.size()==1&&restricted[0]<=1)out.dcnr_restricted=restricted[0];
    const auto &nrinfo=Field(*system,0x4b);
    // Only inspect the fixed non-identifying prefix. Later bytes include
    // network/cell identities and must never be logged or saved.
    if(nrinfo.size()>=8) {
      if(nrinfo[0])out.nr_packet_domain=nrinfo[1]==2||nrinfo[1]==3;
      if(nrinfo[6])out.nr_forbidden=nrinfo[7]!=0;
    }
    if(nrinfo.size()>=19&&nrinfo[16])out.nr_reject=true;
  }
  if(signal) {
    out.signal_query_ok=true;
    const auto &nr=Field(*signal,0x17);
    out.nr_signal_tlv=nr.size()>=4;
    if(out.nr_signal_tlv) {
      const int rsrp=static_cast<int16_t>(U16(nr.data()));
      const int snr=static_cast<int16_t>(U16(nr.data()+2));
      // 0x8000 (-32768) is QMI's inactive NR measurement sentinel.
      out.nr_signal_active=rsrp>=-160&&rsrp<=-20;
      if(out.nr_signal_active) {
        out.rsrp_dbm=rsrp;
        out.snr_valid=snr!=-32768&&snr>=-300&&snr<=500;
        if(out.snr_valid)out.snr_tenths=snr;
        const auto &rsrq=Field(*signal,0x18);
        if(rsrq.size()==2) {
          const int value=static_cast<int16_t>(U16(rsrq.data()));
          out.rsrq_valid=value>=-50&&value<=10;
          if(out.rsrq_valid)out.rsrq_db=value;
        }
      }
    }
  }
  return out;
}
} // namespace aera::mobile
