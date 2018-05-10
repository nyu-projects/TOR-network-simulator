#ifndef __TOR_MARUT_H__
#define __TOR_MARUT_H__

#include "tor-base.h"
#include "cell-header.h"
#include "bktap-base.h"

#include "ns3/point-to-point-net-device.h"

/*
#define ACK 1
#define FWD 2
#define FDBK 12
#define NS3_SOCK_STREAM 0
#define VEGASALPHA 3
#define VEGASBETA 6
#define UDP_CELL_HEADER_SIZE (4 + 4 + 2 + 6 + 2 + 1)
*/

namespace ns3 {

class MarutBktapCircuit;
class MarutUdpChannel;

class MarutSeqQueue : public SimpleRefCount<MarutSeqQueue>
{
public:
  uint32_t cwnd;
  uint64_t diff;
  uint8_t  isNegative;

  uint64_t circ_diff;
  uint8_t  circ_isNegative;

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

  MarutSeqQueue ()
  {
    cwnd = 6;
    diff = 0;
    isNegative = 0;
    circ_diff = 0;
    circ_isNegative = 0;

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


class MarutUdpChannel : public SimpleRefCount<MarutUdpChannel>
{
public:
  MarutUdpChannel ();
  MarutUdpChannel (Address,int);

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
  list<Ptr<MarutBktapCircuit> > circuits;
  SimpleRttEstimator rttEstimator;
};


class MarutBktapCircuit : public BaseCircuit
{
public:
  MarutBktapCircuit (uint16_t);
  // ~MarutBktapCircuit();

  Ptr<MarutUdpChannel> inbound;
  Ptr<MarutUdpChannel> outbound;

  Ptr<MarutSeqQueue> inboundQueue;
  Ptr<MarutSeqQueue> outboundQueue;

  CellDirection GetDirection (Ptr<MarutUdpChannel>);
  Ptr<MarutSeqQueue> GetQueue (CellDirection);
  Ptr<MarutUdpChannel> GetChannel (CellDirection direction);
};


class MarutTorBktapApp : public TorBaseApp
{
public:
  static TypeId GetTypeId (void);
  MarutTorBktapApp ();
  ~MarutTorBktapApp ();

  virtual void StartApplication (void);
  virtual void StopApplication (void);
  virtual void DoDispose (void);
  void RefillReadCallback (int64_t);
  void RefillWriteCallback (int64_t);

  Ptr<MarutUdpChannel> AddChannel (Address, int);
  Ptr<MarutBktapCircuit> GetCircuit (uint16_t);
  Ptr<MarutBktapCircuit> GetNextCircuit ();
  virtual void AddCircuit (int, Ipv4Address, int, Ipv4Address, int,
                           Ptr<PseudoClientSocket> clientSocket = 0);

  Ptr<Socket> m_socket;

  map<Address,Ptr<MarutUdpChannel> > channels;
  map<uint16_t,Ptr<MarutBktapCircuit> > circuits;
  map<uint16_t,Ptr<MarutBktapCircuit> >::iterator circit;

  void ReadCallback (Ptr<Socket>);
  uint32_t ReadFromEdge (Ptr<Socket>);
  uint32_t ReadFromRelay (Ptr<Socket>);
  void PackageRelayCell (Ptr<MarutBktapCircuit>, CellDirection, Ptr<Packet>);
  void ReceivedRelayCell (Ptr<MarutBktapCircuit>, CellDirection, Ptr<Packet>);
  void ReceivedAck (Ptr<MarutBktapCircuit>, CellDirection, FdbkCellHeader);
  void ReceivedFwd (Ptr<MarutBktapCircuit>, CellDirection, FdbkCellHeader);
  void CongestionAvoidance (Ptr<MarutSeqQueue>, uint64_t, Time,uint16_t, CellDirection);
  void WindowUpdate (Ptr<MarutSeqQueue>, Time, uint16_t, CellDirection);


  Ptr<MarutUdpChannel> LookupChannel (Ptr<Socket>);

  void SocketWriteCallback (Ptr<Socket>, uint32_t);
  void WriteCallback ();
  uint32_t FlushPendingCell (Ptr<MarutBktapCircuit>, CellDirection,bool = false);
  void SendFeedbackCell (Ptr<MarutBktapCircuit>, CellDirection, uint8_t, uint32_t);
  void PushFeedbackCell (Ptr<MarutBktapCircuit>, CellDirection);
  void ScheduleRto (Ptr<MarutBktapCircuit>, CellDirection, bool = false);
  void Rto (Ptr<MarutBktapCircuit>, CellDirection);

  bool m_nagle;

  EventId writeevent;
  EventId readevent;
  Ptr<Queue> m_devQ;
  uint32_t m_devQlimit;
};


} /* end namespace ns3 */
#endif /* __TOR_MARUT_H__ */
