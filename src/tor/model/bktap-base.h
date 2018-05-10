#ifndef __BKTAP_BASE_H__
#define __BKTAP_BASE_H__

#include "tor-base.h"
#include "cell-header.h"

#include "ns3/point-to-point-net-device.h"

#define ACK 1
#define FWD 2
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
  uint64_t diff;
//  uint8_t  isNegative;

  FdbkCellHeader ()
  {
    circId = flags = ack = fwd = 0;
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
    os << " ack=" << ack << " fwd=" << fwd;
    if ((flags & ACK) != 0)
      {
        os << " ACK";
      }
    if ((flags & FWD) != 0)
      {
        os << " FWD";
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

  SimpleRttEstimator ()
  {
    rttMultiplier = 1;
    estimatedRtt = Time (0);
    devRtt = Time (0);
    baseRtt = Time (Seconds (42));
    ResetCurrRtt ();
    cntRtt = 0;
  }

  void
  SentSeq (uint32_t seq)
  {
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
  EstimateRtt (uint32_t ack)
  {
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
  AddSample (Time rtt)
  {
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
  ResetCurrRtt ()
  {
    currentRtt = Time (Seconds (10000));
    cntRtt = 0;
  }

  Time
  Rto ()
  {
    Time rto = estimatedRtt + 4 * devRtt;
    rto = rto * rttMultiplier;
    if (rto.GetMilliSeconds () < 1000)
      {
        return Time (MilliSeconds (1000));
      }
    return rto;
  }
};



/*
class SeqQueue : public SimpleRefCount<SeqQueue>
{
public:
  uint32_t cwnd;
  uint64_t diff;
//  uint8_t  isNegative;

  uint64_t circ_diff;
//  uint8_t  circ_isNegative;

  uint32_t ssthresh;
  uint32_t nextTxSeq;
  uint32_t highestTxSeq;
  uint32_t tailSeq;
  uint32_t headSeq;
  uint32_t virtHeadSeq;
  uint32_t begRttSeq;
  uint32_t dupackcnt;
  map< uint32_t, Ptr<Packet> > cellMap;

  bool wasRetransmit;

  queue<uint32_t> ackq;
  queue<uint32_t> fwdq;
  EventId delFeedbackEvent;

  SimpleRttEstimator virtRtt;
  SimpleRttEstimator actRtt;
  EventId retxEvent;

  SeqQueue ()
  {
    cwnd = 2;
    diff = 0;
//    isNegative = 0;
    circ_diff = 0;
//    circ_isNegative = 0;

    nextTxSeq = 1;
    highestTxSeq = 0;
    tailSeq = 0;
    headSeq = 0;
    virtHeadSeq = 0;
    begRttSeq = 1;
    ssthresh = pow (2,10);
    dupackcnt = 0;
  }

  // IMPORTANT: return value is now true if the cell is new, else false
  // previous behavior was: true if tailSeq increases
  bool
  Add ( Ptr<Packet> cell, uint32_t seq ) {
    if (tailSeq < seq && cellMap.find(seq) == cellMap.end()) {
        cellMap[seq] = cell;
        while (cellMap.find (tailSeq + 1) != cellMap.end ()) {
            ++tailSeq;
        }

        if (headSeq == 0)
          {
            headSeq = virtHeadSeq = cellMap.begin ()->first;
          }

        return true;
      }
    return false;
  }

  Ptr<Packet>
  GetCell (uint32_t seq)
  {
    Ptr<Packet> cell;
    if (cellMap.find (seq) != cellMap.end ())
      {
        cell = cellMap[seq];
      }
    wasRetransmit = true; //implicitely assume that it is a retransmit
    return cell;
  }

  Ptr<Packet>
  GetNextCell ()
  {
    Ptr<Packet> cell;
    if (cellMap.find (nextTxSeq) != cellMap.end ())
      {
        cell = cellMap[nextTxSeq];
        ++nextTxSeq;
      }

    if (highestTxSeq < nextTxSeq - 1)
      {
        highestTxSeq = nextTxSeq - 1;
        wasRetransmit = false;
      }
    else
    {
      wasRetransmit = true;
    }

    return cell;
  }

  bool WasRetransmit()
  {
    return wasRetransmit;
  }


  void
  DiscardUpTo (uint32_t seq)
  {
    while (cellMap.find (seq - 1) != cellMap.end ())
      {
        cellMap.erase (seq - 1);
        ++headSeq;
        --seq;
      }

    if (headSeq > nextTxSeq)
      {
        nextTxSeq = headSeq;
      }
  }

  uint32_t
  VirtSize ()
  {
    int t_diff = tailSeq - virtHeadSeq;
    return t_diff < 0 ? 0 : t_diff;
  }

  uint32_t
  Size ()
  {
    int t_diff = tailSeq - headSeq;
    return t_diff < 0 ? 0 : t_diff;
  }

  uint32_t
  Window ()
  {
    return cwnd - Inflight ();
  }

  uint32_t
  Inflight ()
  {
    return nextTxSeq - virtHeadSeq - 1;
  }

  bool
  PackageInflight ()
  {
    return headSeq != highestTxSeq;
  }

};
*/
} /* end namespace ns3 */
#endif /* __BKTAP_BASE_H__ */
