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

  Ptr<SeqQueue> inboundQueue;
  Ptr<SeqQueue> outboundQueue;

  CellDirection GetDirection (Ptr<MarutUdpChannel>);
  Ptr<SeqQueue> GetQueue (CellDirection);
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
  void CongestionAvoidance (Ptr<SeqQueue>, uint64_t, Time);
  void WindowUpdate (Ptr<SeqQueue>, Time);
	

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
