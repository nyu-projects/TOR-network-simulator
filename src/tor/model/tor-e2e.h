#ifndef __TOR_E2E_H__
#define __TOR_E2E_H__

#include "tor-base.h"
#include "cell-header.h"

#include "ns3/point-to-point-net-device.h"

#define ACK 1
#define FWD 2
#define FDBK 12
#define NS3_SOCK_STREAM 0
#define VEGASALPHA 3
#define VEGASBETA 6
#undef UDP_CELL_HEADER_SIZE 
#define UDP_CELL_HEADER_SIZE (4 + 4 + 2 + 6 + 2 + 1 +1) //%%

namespace ns3 {

class E2eCircuit;
class E2eUdpChannel;


class E2eBaseCellHeader : public Header
{
public:
  uint16_t circId;
  uint8_t cellType;
  uint8_t flags;

  E2eBaseCellHeader ()
  {
    circId = cellType = flags = 0;
  }

  TypeId
  GetTypeId () const
  {
    static TypeId tid = TypeId ("ns3::E2eBaseCellHeader")
      .SetParent<Header> ()
      .AddConstructor<E2eBaseCellHeader> ()
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



class E2eUdpCellHeader : public Header
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
  uint8_t ECN; //Changes by Neha

  E2eUdpCellHeader ()
  {
    circId = cellType = flags = seq = streamId = length = cmd = ECN = 0; //%%
  }

  TypeId
  GetTypeId () const
  {
    static TypeId tid = TypeId ("ns3::E2eUdpCellHeader")
      .SetParent<Header> ()
      .AddConstructor<E2eUdpCellHeader> ()
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
    return (4 + 4 + 2 + 6 + 2 + 1 + 1);  //%%
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
    i.WriteU8 (ECN); //%%
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
    ECN = i.ReadU8 (); //%%
    return GetSerializedSize ();
  }
};



class E2eFdbkCellHeader : public Header
{
public:
  uint16_t circId;
  uint8_t cellType;
  uint8_t flags;
  uint32_t ack;
  uint32_t fwd;
  //Changes by Neha
  uint8_t CE;
  E2eFdbkCellHeader ()
  {
    circId = flags = ack = fwd = 0;
    cellType = FDBK;
    CE = 0; //Change by Neha
  }

  TypeId
  GetTypeId () const
  {
    static TypeId tid = TypeId ("ns3::E2eFdbkCellHeader")
      .SetParent<Header> ()
      .AddConstructor<E2eFdbkCellHeader> ()
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
  }

  uint32_t
  GetSerializedSize () const
  {
    return  (2 + 1 + 1 + 4 + 4 +1); //Changes by Neha
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
    i.WriteU8 (CE); //%%
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
    CE = i.ReadU8 (); // Changes by Neha
    return GetSerializedSize ();
  }
};



class E2eSimpleRttEstimator
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

  E2eSimpleRttEstimator ()
  {
    rttMultiplier = 1;
    estimatedRtt = Time (0);
    devRtt = Time (0);
    baseRtt = Time (Seconds (42));
    ResetCurrRtt ();
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
        //remember as retx
        retx.insert (seq);
      }
  }

  Time EstimateRtt (uint32_t ack) {
    Time rtt = Time (0);
    if (rttHistory.find (ack - 1) != rttHistory.end ()) {
        if (retx.find (ack - 1) == retx.end ()) {
            rtt = Simulator::Now () - rttHistory[ack - 1];
            AddSample (rtt);
            rttMultiplier = 1;
        }
    }
    retx.erase (ack - 1);
    rttHistory.erase (ack - 1);
//cout << "Estimated rttt for ack "<<ack<<" = "<<rtt<<endl;    
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




class E2eSeqQueue : public SimpleRefCount<E2eSeqQueue> {
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

  E2eSimpleRttEstimator virtRtt;
  E2eSimpleRttEstimator actRtt;
  EventId retxEvent;

  E2eSeqQueue () {
    cwnd = 6;
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

  Ptr<Packet> GetNextCell () {
    Ptr<Packet> cell;
    if (cellMap.find (nextTxSeq) != cellMap.end ()) {
        cell = cellMap[nextTxSeq];
        ++nextTxSeq;
    }
    if (highestTxSeq < nextTxSeq - 1) {
        highestTxSeq = nextTxSeq - 1;
        wasRetransmit = false;
    }
    else {
      wasRetransmit = true;
    }
    return cell;
  }

  bool WasRetransmit() {
    return wasRetransmit;
  }

  void
  DiscardUpTo (uint32_t seq) {
    while (cellMap.find (seq - 1) != cellMap.end ()) {
        cellMap.erase (seq - 1);
        ++headSeq;
        --seq;
    }
    if (headSeq > nextTxSeq) {
        nextTxSeq = headSeq;
    }
  }

  uint32_t
  VirtSize () {
    int diff = tailSeq - virtHeadSeq;
    return diff < 0 ? 0 : diff;
  }

  uint32_t
  Size () {
    int diff = tailSeq - headSeq;
    return diff < 0 ? 0 : diff;
  }

  uint32_t
  Window () {
    return cwnd - Inflight ();
  }

  uint32_t
  Inflight () {
    return nextTxSeq - virtHeadSeq - 1;
  }

  bool
  PackageInflight () {
    return headSeq != highestTxSeq;
  }

  bool
  IsCongested (){
    return Size() > 6; //Change here
  }
};


class E2eUdpChannel : public SimpleRefCount<E2eUdpChannel>
{
public:
  E2eUdpChannel ();
  E2eUdpChannel (Address,int);

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
  list<Ptr<E2eCircuit> > circuits;
  E2eSimpleRttEstimator rttEstimator;
};


class E2eCircuit : public BaseCircuit
{
public:
  E2eCircuit (uint16_t);
  // ~E2eCircuit();

  Ptr<E2eUdpChannel> inbound;
  Ptr<E2eUdpChannel> outbound;

  Ptr<E2eSeqQueue> inboundQueue;
  Ptr<E2eSeqQueue> outboundQueue;

  CellDirection GetDirection (Ptr<E2eUdpChannel>);
  Ptr<E2eSeqQueue> GetQueue (CellDirection);
  Ptr<E2eUdpChannel> GetChannel (CellDirection direction);
};


class TorE2eApp : public TorBaseApp
{
public:
  static TypeId GetTypeId (void);
  TorE2eApp ();
  ~TorE2eApp ();

  virtual void StartApplication (void);
  virtual void StopApplication (void);
  virtual void DoDispose (void);
  void RefillReadCallback (int64_t);
  void RefillWriteCallback (int64_t);

  Ptr<E2eUdpChannel> AddChannel (Address, int);
  Ptr<E2eCircuit> GetCircuit (uint16_t);
  Ptr<E2eCircuit> GetNextCircuit ();
  virtual void AddCircuit (int, Ipv4Address, int, Ipv4Address, int,
                           Ptr<PseudoClientSocket> clientSocket = 0);

  Ptr<Socket> m_socket;

  map<Address,Ptr<E2eUdpChannel> > channels;
  map<uint16_t,Ptr<E2eCircuit> > circuits;
  map<uint16_t,Ptr<E2eCircuit> >::iterator circit;

  void ReadCallback (Ptr<Socket>);
  uint32_t ReadFromEdge (Ptr<Socket>);
  uint32_t ReadFromRelay (Ptr<Socket>);
  void PackageRelayCell (Ptr<E2eCircuit>, CellDirection, Ptr<Packet>);
  void ReceivedRelayCell (Ptr<E2eCircuit>, CellDirection, Ptr<Packet>);
  void ReceivedAck (Ptr<E2eCircuit>, CellDirection, E2eFdbkCellHeader);
  void ReceivedFwd (Ptr<E2eCircuit>, CellDirection, E2eFdbkCellHeader);
  //void CongestionAvoidance (Ptr<E2eSeqQueue>, Time);
  void CongestionAvoidance (Ptr<E2eSeqQueue>, uint8_t); //changes here
  Ptr<E2eUdpChannel> LookupChannel (Ptr<Socket>);

  void SocketWriteCallback (Ptr<Socket>, uint32_t);
  void WriteCallback ();
  uint32_t FlushPendingCell (Ptr<E2eCircuit>, CellDirection,bool = false);
  //void SendFeedbackCell (Ptr<E2eCircuit>, CellDirection, uint8_t, uint32_t);
  void SendFeedbackCell (Ptr<E2eCircuit>, CellDirection, uint8_t, uint32_t, bool=false);
  //void PushFeedbackCell (Ptr<E2eCircuit>, CellDirection);
  void PushFeedbackCell (Ptr<E2eCircuit>, CellDirection, bool=false);
  void ScheduleRto (Ptr<E2eCircuit>, CellDirection, bool = false);
  void Rto (Ptr<E2eCircuit>, CellDirection);

  bool m_nagle;

  EventId writeevent;
  EventId readevent;
  Ptr<Queue> m_devQ;
  uint32_t m_devQlimit;
};


} /* end namespace ns3 */
#endif /* __TOR_E2E_H__ */
