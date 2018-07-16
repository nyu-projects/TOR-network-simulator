#ifndef __TOR_BKTAP_H__
#define __TOR_BKTAP_H__

#include "tor-base.h"
#include "cell-header.h"
#include "bktap-base.h"

#include "ns3/point-to-point-net-device.h"

namespace ns3 {

class BktapCircuit;
class UdpChannel;

class SeqQueue : public SimpleRefCount<SeqQueue>
{
public:
  uint32_t cwnd;
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
  Add ( Ptr<Packet> cell, uint32_t seq )
  {
    if (tailSeq < seq && cellMap.find(seq) == cellMap.end())
      {
        cellMap[seq] = cell;
        while (cellMap.find (tailSeq + 1) != cellMap.end ())
          {
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
    int diff = tailSeq - virtHeadSeq;
    return diff < 0 ? 0 : diff;
  }

  uint32_t
  Size ()
  {
    int diff = tailSeq - headSeq;
    return diff < 0 ? 0 : diff;
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


class UdpChannel : public SimpleRefCount<UdpChannel>
{
public:
  UdpChannel ();
  UdpChannel (Address,int);

  void SetSocket (Ptr<Socket>);
  uint8_t GetType ();
  bool SpeaksCells ();

  void ScheduleFlush (bool=false);
  void Flush ();
  EventId m_flushEvent;
  queue<Ptr<Packet> > m_flushQueue;
  Ptr<Queue> m_devQ;
  uint32_t m_devQlimit;

  Ptr<Socket> m_socket;
  Address m_remote;
  uint8_t m_conntype;
  list<Ptr<BktapCircuit> > circuits;
  SimpleRttEstimator rttEstimator;
};


class BktapCircuit : public BaseCircuit
{
public:
  BktapCircuit (uint16_t);
  // ~BktapCircuit();

  Ptr<UdpChannel> inbound;
  Ptr<UdpChannel> outbound;

  Ptr<SeqQueue> inboundQueue;
  Ptr<SeqQueue> outboundQueue;

  CellDirection GetDirection (Ptr<UdpChannel>);
  Ptr<SeqQueue> GetQueue (CellDirection);
  Ptr<UdpChannel> GetChannel (CellDirection direction);
};


class TorBktapApp : public TorBaseApp
{
public:
  static TypeId GetTypeId (void);
  TorBktapApp ();
  ~TorBktapApp ();

  virtual void StartApplication (void);
  virtual void StopApplication (void);
  virtual void DoDispose (void);
  void RefillReadCallback (int64_t);
  void RefillWriteCallback (int64_t);

  Ptr<UdpChannel> AddChannel (Address, int);
  Ptr<BktapCircuit> GetCircuit (uint16_t);
  Ptr<BktapCircuit> GetNextCircuit ();
  virtual void AddCircuit (int, Ipv4Address, int, Ipv4Address, int,
                           Ptr<PseudoClientSocket> clientSocket = 0);

  Ptr<Socket> m_socket;

  map<Address,Ptr<UdpChannel> > channels;
  map<uint16_t,Ptr<BktapCircuit> > circuits;
  map<uint16_t,Ptr<BktapCircuit> >::iterator circit;

  void ReadCallback (Ptr<Socket>);
  uint32_t ReadFromEdge (Ptr<Socket>);
  uint32_t ReadFromRelay (Ptr<Socket>);
  void PackageRelayCell (Ptr<BktapCircuit>, CellDirection, Ptr<Packet>);
  void ReceivedRelayCell (Ptr<BktapCircuit>, CellDirection, Ptr<Packet>);
  void ReceivedAck (Ptr<BktapCircuit>, CellDirection, FdbkCellHeader);
  void ReceivedFwd (Ptr<BktapCircuit>, CellDirection, FdbkCellHeader);
  void CongestionAvoidance (Ptr<SeqQueue>, Time);
  Ptr<UdpChannel> LookupChannel (Ptr<Socket>);

  void SocketWriteCallback (Ptr<Socket>, uint32_t);
  void WriteCallback ();
  uint32_t FlushPendingCell (Ptr<BktapCircuit>, CellDirection,bool = false);
  void SendFeedbackCell (Ptr<BktapCircuit>, CellDirection, uint8_t, uint32_t);
  void PushFeedbackCell (Ptr<BktapCircuit>, CellDirection);
  void ScheduleRto (Ptr<BktapCircuit>, CellDirection, bool = false);
  void Rto (Ptr<BktapCircuit>, CellDirection);

  bool m_nagle;

  EventId writeevent;
  EventId readevent;
  Ptr<Queue> m_devQ;
  uint32_t m_devQlimit;
};


} /* end namespace ns3 */
#endif /* __TOR_BKTAP_H__ */
