/* SPDX-License-Identifier: Apache-2.0 */
#include "../nr.hpp"
#include <cassert>
using namespace aera::mobile;
int main() {
  const Values lte{{0x14,{2,2,1}},{0x4e,{1}},{0x4f,{0}}};
  const Values idle{{0x17,{0,0x80,0,0x80}}};
  auto r=ObserveNr(&lte,&idle);
  assert(r.lte_service&&r.eutra_nr==1&&r.dcnr_restricted==0);
  assert(r.nr_signal_tlv&&!r.nr_signal_active);
  assert(r.Summary().find("advertises")!=std::string::npos);
  const Values active{{0x17,{0xB5,0xFF,0x9E,0x00}},{0x18,{0xF3,0xFF}}};
  r=ObserveNr(&lte,&active);
  assert(r.nr_signal_active&&r.rsrp_dbm==-75);
  assert(r.snr_valid&&r.snr_tenths==158&&r.rsrq_valid&&r.rsrq_db==-13);
  assert(r.Summary().find("NSA evidence")!=std::string::npos);
  const Values restricted{{0x14,{2,2,1}},{0x4e,{1}},{0x4f,{1}}};
  r=ObserveNr(&restricted,&idle);
  assert(!r.nr_signal_active&&r.Summary().find("restricted")!=std::string::npos);
  const Values sa{{0x4a,{2,2,1}},{0x4b,{1,2,1,2,0,0,1,0}}};
  r=ObserveNr(&sa,&idle);
  assert(r.nr_service==2&&r.nr_packet_domain&&!r.nr_forbidden);
  assert(r.Summary().find("available")!=std::string::npos);
  const Values empty;
  r=ObserveNr(&empty,&empty);
  assert(!r.nr_signal_active&&r.nr_service==-1&&r.eutra_nr==-1);
  r=ObserveNr(nullptr,nullptr);
  assert(!r.system_query_ok&&!r.signal_query_ok);
  // Truncated data cannot be mistaken for a positive NR observation.
  const Values broken{{0x17,{0xB5}},{0x18,{0xF3}}};
  r=ObserveNr(&empty,&broken);assert(!r.nr_signal_active);
}
