/*
    PowerDNS Versatile Database Driven Nameserver
    Copyright (C) 2001 - 2011  PowerDNS.COM BV

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License version 2 as 
    published by the Free Software Foundation

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*/
#include "dnssecinfra.hh"
#include "namespaces.hh"
#include <boost/foreach.hpp>
#include "dnsseckeeper.hh"
#include "lock.hh"

/* this is where the RRSIGs begin, keys are retrieved,
   but the actual signing happens in fillOutRRSIG */
int getRRSIGsForRRSET(DNSSECKeeper& dk, const std::string& signer, const std::string signQName, uint16_t signQType, uint32_t signTTL, 
		     vector<shared_ptr<DNSRecordContent> >& toSign, vector<RRSIGRecordContent>& rrcs, bool ksk)
{
  if(toSign.empty())
    return -1;
  RRSIGRecordContent rrc;
  rrc.d_type=signQType;

  rrc.d_labels=countLabels(signQName); 
  rrc.d_originalttl=signTTL; 
  rrc.d_siginception=getCurrentInception();;
  rrc.d_sigexpire = rrc.d_siginception + 14*86400; // XXX should come from zone metadata
  rrc.d_signer = signer;
  rrc.d_tag = 0;
  
  // we sign the RRSET in toSign + the rrc w/o hash
  
  DNSSECKeeper::keyset_t keys = dk.getKeys(rrc.d_signer);
  vector<DNSSECPrivateKey> KSKs, ZSKs;
  vector<DNSSECPrivateKey>* signingKeys;
  
  // if ksk==1, only get KSKs
  // if ksk==0, get ZSKs, unless there is no ZSK, then get KSK
  BOOST_FOREACH(DNSSECKeeper::keyset_t::value_type& keymeta, keys) {
    rrc.d_algorithm = keymeta.first.d_algorithm;
    if(!keymeta.second.active) 
      continue;
      
    if(keymeta.second.keyOrZone)
      KSKs.push_back(keymeta.first);
    else if(!ksk)
      ZSKs.push_back(keymeta.first);
  }
  if(ksk)
    signingKeys = &KSKs;
  else {
    if(ZSKs.empty())
      signingKeys = &KSKs;
    else
      signingKeys =&ZSKs;
  }
  
  BOOST_FOREACH(DNSSECPrivateKey& dpk, *signingKeys) {
    fillOutRRSIG(dpk, signQName, rrc, toSign);
    rrcs.push_back(rrc);
  }
  return 0;
}

// this is the entrypoint from DNSPacket
void addSignature(DNSSECKeeper& dk, const std::string& signer, const std::string signQName, const std::string& wildcardname, uint16_t signQType, 
  uint32_t signTTL, DNSPacketWriter::Place signPlace, 
  vector<shared_ptr<DNSRecordContent> >& toSign, vector<DNSResourceRecord>& outsigned)
{
  // cerr<<"Asked to sign '"<<signQName<<"'|"<<DNSRecordContent::NumberToType(signQType)<<", "<<toSign.size()<<" records\n";
  if(toSign.empty())
    return;
  vector<RRSIGRecordContent> rrcs;
  if(dk.isPresigned(signer)) {
    dk.getPreRRSIGs(signer, signQName, QType(signQType), signPlace, outsigned); // does it all
  }
  else {
    if(getRRSIGsForRRSET(dk, signer, wildcardname.empty() ? signQName : wildcardname, signQType, signTTL, toSign, rrcs, signQType == QType::DNSKEY) < 0)  {
      // cerr<<"Error signing a record!"<<endl;
      return;
    } 
  
    DNSResourceRecord rr;
    rr.qname=signQName;
    rr.qtype=QType::RRSIG;
    rr.ttl=signTTL;
    rr.auth=false;
    rr.d_place = (DNSResourceRecord::Place) signPlace;
    BOOST_FOREACH(RRSIGRecordContent& rrc, rrcs) {
      rr.content = rrc.getZoneRepresentation();
      outsigned.push_back(rr);
    }
  }
  toSign.clear();
}

static pthread_mutex_t g_signatures_lock = PTHREAD_MUTEX_INITIALIZER;
static map<pair<string, string>, string> g_signatures;

void fillOutRRSIG(DNSSECPrivateKey& dpk, const std::string& signQName, RRSIGRecordContent& rrc, vector<shared_ptr<DNSRecordContent> >& toSign) 
{
  DNSKEYRecordContent drc= dpk.getDNSKEY(); 
  RSAContext& rc = dpk.d_key;
  rrc.d_tag = drc.getTag();
  rrc.d_algorithm = drc.d_algorithm;
  string realhash=getHashForRRSET(signQName, rrc, toSign); // this is what we sign

  unsigned char signature[mpi_size(&rc.getContext().N)];
  pair<string, string> lookup(rc.getPubKeyHash(), realhash);
  
  {
    Lock l(&g_signatures_lock);
    if(g_signatures.count(lookup)) {
      // cerr<<"Hit!"<<endl;
      rrc.d_signature=g_signatures[lookup];
      return;
    }
    else
      ; // cerr<<"Miss!"<<endl;
  }
  
  int ret=rsa_pkcs1_sign(&rc.getContext(), RSA_PRIVATE, 
    rrc.d_algorithm < 8 ? SIG_RSA_SHA1 : SIG_RSA_SHA256, 
    rrc.d_algorithm < 8 ? 20 : 32,
    (unsigned char*) realhash.c_str(), signature);
  
  if(ret!=0) {
    cerr<<"signing returned: "<<ret<<endl;
    exit(1);
  }
  
  rrc.d_signature.assign((char*)signature, sizeof(signature));

  Lock l(&g_signatures_lock);
  g_signatures[lookup] = rrc.d_signature;
}

static bool rrsigncomp(const DNSResourceRecord& a, const DNSResourceRecord& b)
{
  return a.d_place < b.d_place;
}

void addRRSigs(DNSSECKeeper& dk, const std::string& signer, DNSPacket& p)
{
  vector<DNSResourceRecord>& rrs=p.getRRS();
  
  stable_sort(rrs.begin(), rrs.end(), rrsigncomp);
  
  string signQName, wildcardQName;
  uint16_t signQType=0;
  uint32_t signTTL=0;
  
  DNSPacketWriter::Place signPlace=DNSPacketWriter::ANSWER;
  vector<shared_ptr<DNSRecordContent> > toSign;

  vector<DNSResourceRecord> signedRecords;

  for(vector<DNSResourceRecord>::const_iterator pos = rrs.begin(); pos != rrs.end(); ++pos) {
    signedRecords.push_back(*pos);
    if(pos != rrs.begin() && (signQType != pos->qtype.getCode()  || signQName != pos->qname)) {
      addSignature(dk, signer, signQName, wildcardQName, signQType, signTTL, signPlace, toSign, signedRecords);
    }
    signQName= pos->qname;
    wildcardQName = pos->wildcardname;
    signQType = pos ->qtype.getCode();
    signTTL = pos->ttl;
    signPlace = (DNSPacketWriter::Place) pos->d_place;
    if(pos->auth || pos->qtype.getCode() == QType::DS) {
      string content = pos->content;
      if(pos->qtype.getCode()==QType::MX || pos->qtype.getCode() == QType::SRV) {  
        content = lexical_cast<string>(pos->priority) + " " + pos->content;
      }
      if(!pos->content.empty() && pos->qtype.getCode()==QType::TXT && pos->content[0]!='"') {
        content="\""+pos->content+"\"";
      }
      if(pos->content.empty())  // empty contents confuse the MOADNS setup
        content=".";
      
      shared_ptr<DNSRecordContent> drc(DNSRecordContent::mastermake(pos->qtype.getCode(), 1, content)); 
      toSign.push_back(drc);
    }
  }
  addSignature(dk, signer, signQName, wildcardQName, signQType, signTTL, signPlace, toSign, signedRecords);
  
  rrs.swap(signedRecords);
}