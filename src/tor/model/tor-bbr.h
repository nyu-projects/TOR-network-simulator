#ifndef __TOR_BBR_H__
#define __TOR_BBR_H__

#include "tor-base.h"
#include "cell-header.h"
#include "bktap-base.h"

#include "ns3/point-to-point-net-device.h"

#define STARTUP_STATE 1
#define DRAIN_STATE 2
#define PROBE_BW_STATE 3
#define PROBE_RTT_STATE 4

namespace ns3 {

class BBRBktapCircuit;
class BBRUdpChannel;

class BBRSeqQueue : public SimpleRefCount<BBRSeqQueue>
{
public:
  uint32_t cwnd;
  uint8_t  isNegative;
  uint8_t  circ_isNegative;
  
  uint8_t bbr_state;  
  double pacing_gain;
 
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

  BBRSeqQueue ()
  {
    cwnd = 6;
    isNegative = 0;
    circ_isNegative = 0;
    // N: initialize state as Startup
    bbr_state = 1;
    pacing_gain = 2/ log (2);    

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

  uint32_t
  InflightAct ()
  {
    return nextTxSeq - headSeq - 1;
  }

  bool
  PackageInflight ()
  {
    return headSeq != highestTxSeq;
  }

};


class BBRUdpChannel : public SimpleRefCount<BBRUdpChannel>
{
public:
  BBRUdpChannel ();
  BBRUdpChannel (Address,int);

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
  list<Ptr<BBRBktapCircuit> > circuits;
  SimpleRttEstimator rttEstimator;
};


class BBRBktapCircuit : public BaseCircuit
{
public:
  BBRBktapCircuit (uint16_t);

  Ptr<BBRUdpChannel> inbound;
  Ptr<BBRUdpChannel> outbound;

  Ptr<BBRSeqQueue> inboundQueue;
  Ptr<BBRSeqQueue> outboundQueue;

  CellDirection GetDirection (Ptr<BBRUdpChannel>);
  Ptr<BBRSeqQueue> GetQueue (CellDirection);
  Ptr<BBRUdpChannel> GetChannel (CellDirection direction);
};


class BBRTorBktapApp : public TorBaseApp
{
public:
  static TypeId GetTypeId (void);
  BBRTorBktapApp ();
  ~BBRTorBktapApp ();

  virtual void StartApplication (void);
  virtual void StopApplication (void);
  virtual void DoDispose (void);
  void RefillReadCallback (int64_t);
  void RefillWriteCallback (int64_t);

  Ptr<BBRUdpChannel> AddChannel (Address, int);
  Ptr<BBRBktapCircuit> GetCircuit (uint16_t);
  Ptr<BBRBktapCircuit> GetNextCircuit ();
  virtual void AddCircuit (int, Ipv4Address, int, Ipv4Address, int,
                           Ptr<PseudoClientSocket> clientSocket = 0);

  Ptr<Socket> m_socket;

  map<Address,Ptr<BBRUdpChannel> > channels;
  map<uint16_t,Ptr<BBRBktapCircuit> > circuits;
  map<uint16_t,Ptr<BBRBktapCircuit> >::iterator circit;

  void ReadCallback (Ptr<Socket>);
  uint32_t ReadFromEdge (Ptr<Socket>);
  uint32_t ReadFromRelay (Ptr<Socket>);
  void PackageRelayCell (Ptr<BBRBktapCircuit>, CellDirection, Ptr<Packet>);
  void ReceivedRelayCell (Ptr<BBRBktapCircuit>, CellDirection, Ptr<Packet>);
  void ReceivedAck (Ptr<BBRBktapCircuit>, CellDirection, FdbkCellHeader);
  void ReceivedFwd (Ptr<BBRBktapCircuit>, CellDirection, FdbkCellHeader);
  void CongestionAvoidance (Ptr<BBRSeqQueue>);
  void WindowUpdate (Ptr<BBRSeqQueue>, Time, Time, double);


  Ptr<BBRUdpChannel> LookupChannel (Ptr<Socket>);

  void SocketWriteCallback (Ptr<Socket>, uint32_t);
  void WriteCallback ();
  uint32_t FlushPendingCell (Ptr<BBRBktapCircuit>, CellDirection,bool = false);
  void SendFeedbackCell (Ptr<BBRBktapCircuit>, CellDirection, uint8_t, uint32_t);
  void PushFeedbackCell (Ptr<BBRBktapCircuit>, CellDirection);
  void ScheduleRto (Ptr<BBRBktapCircuit>, CellDirection, bool = false);
  void Rto (Ptr<BBRBktapCircuit>, CellDirection);

  bool m_nagle;

  EventId writeevent;
  EventId readevent;
  Ptr<Queue> m_devQ;
  uint32_t m_devQlimit;
};


} /* end namespace ns3 */
#endif /* __TOR_BBR_H__ */
