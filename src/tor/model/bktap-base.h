#ifndef __BKTAP_BASE_H__
#define __BKTAP_BASE_H__

#include "tor-base.h"
#include "cell-header.h"

#include "ns3/point-to-point-net-device.h"

#define ACK 1
#define FWD 2
#define MRT 3
#define FDBK 12
#define NS3_SOCK_STREAM 0
#define VEGASALPHA 3
#define VEGASBETA 6
#define UDP_CELL_HEADER_SIZE (4 + 4 + 2 + 6 + 2 + 1)


namespace ns3 {

class BaseCellHeader : public Header
{
public:
  uint16_t circId;
  uint8_t cellType;
  uint8_t flags;

  BaseCellHeader ()
  {
    circId = cellType = flags = 0;
  }

  TypeId
  GetTypeId () const
  {
    static TypeId tid = TypeId ("ns3::BaseCellHeader")
      .SetParent<Header> ()
      .AddConstructor<BaseCellHeader> ()
    ;
    return tid;
  }

  TypeId
  GetInstanceTypeId () const
  {
    return GetTypeId ();
  }

  void
  Print (ostream &os) const
  {
    os << "id=" << circId;
  }

  uint32_t
  GetSerializedSize () const
  {
    return (2 + 1 + 1);
  }

  void
  Serialize (Buffer::Iterator start) const
  {
    Buffer::Iterator i = start;
    i.WriteU16 (circId);
    i.WriteU8 (cellType);
    i.WriteU8 (flags);
  }

  uint32_t
  Deserialize (Buffer::Iterator start)
  {
    Buffer::Iterator i = start;
    circId = i.ReadU16 ();
    cellType = i.ReadU8 ();
    flags = i.ReadU8 ();
    return GetSerializedSize ();
  }
};


class UdpCellHeader : public Header
{
public:
  uint16_t circId;
  uint8_t cellType;
  uint8_t flags;
  uint32_t seq;
  uint16_t streamId;
  uint8_t digest[6];
  uint16_t length;
  uint8_t cmd;

  UdpCellHeader ()
  {
    circId = cellType = flags = seq = streamId = length = cmd = 0;
  }

  TypeId
  GetTypeId () const
  {
    static TypeId tid = TypeId ("ns3::UdpCellHeader")
      .SetParent<Header> ()
      .AddConstructor<UdpCellHeader> ()
    ;
    return tid;
  }

  TypeId
  GetInstanceTypeId () const
  {
    return GetTypeId ();
  }

  void
  Print (ostream &os) const
  {
    os << "id=" << circId;
    os << " seq=" << seq;
    if (cmd == RELAY_SENDME)
      {
        os << " SENDME";
      }
  }

  uint32_t
  GetSerializedSize () const
  {
    return (4 + 4 + 2 + 6 + 2 + 1);
  }

  void
  Serialize (Buffer::Iterator start) const
  {
    Buffer::Iterator i = start;
    i.WriteU16 (circId);
    i.WriteU8 (cellType);
    i.WriteU8 (flags);
    i.WriteU32 (seq);
    i.WriteU16 (streamId);
    i.Write (digest, 6);
    i.WriteU16 (length);
    i.WriteU8 (cmd);
  }

  uint32_t
  Deserialize (Buffer::Iterator start)
  {
    Buffer::Iterator i = start;
    circId = i.ReadU16 ();
    cellType = i.ReadU8 ();
    flags = i.ReadU8 ();
    seq = i.ReadU32 ();
    streamId = i.ReadU16 ();
    i.Read (digest, 6);
    length = i.ReadU16 ();
    cmd = i.ReadU8 ();
    return GetSerializedSize ();
  }
};

class FdbkCellHeader : public Header
{
public:
  uint16_t circId;
  uint8_t cellType;
  uint8_t flags;
  uint32_t ack;
  uint32_t fwd;
  uint32_t mrt;
  uint64_t diff;
//  uint8_t  isNegative;

  FdbkCellHeader ()
  {
    circId = flags = ack = fwd = mrt = 0;
//    diff = isNegative = 0;
    cellType = FDBK;
  }

  TypeId
  GetTypeId () const
  {
    static TypeId tid = TypeId ("ns3::FdbkCellHeader")
      .SetParent<Header> ()
      .AddConstructor<FdbkCellHeader> ()
    ;
    return tid;
  }

  TypeId
  GetInstanceTypeId () const
  {
    return GetTypeId ();
  }

  void
  Print (ostream &os) const
  {
    os << "id=" << circId;
    os << " ack=" << ack << " fwd=" << fwd << " mrt=" << mrt;
    if ((flags & ACK) != 0) {
        os << " ACK";
    }
    if ((flags & FWD) != 0) {
        os << " FWD";
    }
    if ((flags & MRT) != 0) {
        os << " MRT";
    }
    os <<" diff= "<< diff;
//    os <<" isNegative= "<<isNegative;
  }

  uint32_t
  GetSerializedSize () const
  {
    //return  (2 + 1 + 1 + 4 + 4 + 8 + 1);
    return  (2 + 1 + 1 + 4 + 4 + 8);
  }

  void
  Serialize (Buffer::Iterator start) const
  {
    Buffer::Iterator i = start;
    i.WriteU16 (circId);
    i.WriteU8 (cellType);
    i.WriteU8 (flags);
    i.WriteU32 (ack);
    i.WriteU32 (fwd);
    i.WriteU32 (mrt);
    i.WriteU64 (diff);
//    i.WriteU8 (isNegative);
  }

  uint32_t
  Deserialize (Buffer::Iterator start)
  {
    Buffer::Iterator i = start;
    circId = i.ReadU16 ();
    cellType = i.ReadU8 ();
    flags = i.ReadU8 ();
    ack = i.ReadU32 ();
    fwd = i.ReadU32 ();
    mrt = i.ReadU32 ();
    diff = i.ReadU64 ();
//    isNegative = i.ReadU8 ();
    return GetSerializedSize ();
  }
};


class SimpleRttEstimator
{
public:
  map< uint32_t,Time > rttHistory;
  set<uint32_t> retx;
  Time estimatedRtt;
  Time devRtt;
  Time currentRtt;
  Time baseRtt;
  uint32_t cntRtt;
  uint32_t rttMultiplier;

  SimpleRttEstimator () {
    rttMultiplier = 1;
    estimatedRtt = Time (0);
    devRtt = Time (0);
    baseRtt = Time (Seconds (42));
    ResetCurrRtt ();
    cntRtt = 0;
  }

  void
  SentSeq (uint32_t seq){
    if (rttHistory.size () == 0 || rttHistory.rbegin ()->first + 1 == seq)
      {
        // next seq, log it.
        rttHistory[seq] = Simulator::Now ();
      }
    else
      {
        //remember es retx
        retx.insert (seq);
      }
  }

  Time
  EstimateRtt (uint32_t ack) {
    Time rtt = Time (0);
    if (rttHistory.find (ack - 1) != rttHistory.end ())
      {
        if (retx.find (ack - 1) == retx.end ())
          {
            rtt = Simulator::Now () - rttHistory[ack - 1];
            AddSample (rtt);
            rttMultiplier = 1;
          }
      }
    retx.erase (ack - 1);
    rttHistory.erase (ack - 1);
    return rtt;
  }

  void
  AddSample (Time rtt) {
    if (rtt > 0)
      {
        double alpha = 0.125;
        double beta = 0.25;
        if (estimatedRtt > 0)
          {
            estimatedRtt = (1 - alpha) * estimatedRtt + alpha * rtt;
            devRtt = (1 - beta) * devRtt + beta* Abs (rtt - estimatedRtt);
          }
        else
          {
            estimatedRtt = rtt;
          }

        baseRtt = min (baseRtt,rtt);
        currentRtt = min (rtt,currentRtt);
        ++cntRtt;
      }
  }

  void
  ResetCurrRtt () {
    currentRtt = Time (Seconds (10000));
    cntRtt = 0;
  }

  Time
  Rto () {
    Time rto = estimatedRtt + 4 * devRtt;
    rto = rto * rttMultiplier;
    if (rto.GetMilliSeconds () < 1000) {
        return Time (MilliSeconds (1000));
    }
    return rto;
  }
};
} /* end namespace ns3 */
#endif /* __BKTAP_BASE_H__ */
