#include "ns3/log.h"
#include "tor-marut.h"
#include <cmath>

using namespace std;

namespace ns3 {

  std::string CellDirectionArray[2] =
  {
          "INBOUND",
                "OUTBOUND"
  };


NS_LOG_COMPONENT_DEFINE ("MarutTorBktapApp");
NS_OBJECT_ENSURE_REGISTERED (MarutTorBktapApp);

MarutUdpChannel::MarutUdpChannel ()
{
  NS_LOG_FUNCTION (this);
  this->m_socket = 0;
}

MarutUdpChannel::MarutUdpChannel (Address remote, int conntype)
{
  m_remote = remote;
  m_conntype = conntype;
  this->m_socket = 0;
}

void
MarutUdpChannel::SetSocket (Ptr<Socket> socket)
{
  this->m_socket = socket;
}

uint8_t
MarutUdpChannel::GetType ()
{
  return m_conntype;
}

bool
MarutUdpChannel::SpeaksCells ()
{
  return m_conntype == RELAYEDGE;
}

void
MarutUdpChannel::Flush ()
{
  while (m_flushQueue.size () > 0)
    {
      if (SpeaksCells () && m_devQlimit <= m_devQ->GetNPackets ())
        {
          m_flushEvent = Simulator::Schedule (MilliSeconds (1), &MarutUdpChannel::Flush, this);
          return;
        }
      Ptr<Packet> data = Create<Packet> ();
      while (m_flushQueue.size () > 0 && data->GetSize () + m_flushQueue.front ()->GetSize () <= 1400)
        {
          data->AddAtEnd (m_flushQueue.front ());
          m_flushQueue.pop ();
        }
      m_socket->SendTo (data,0,m_remote);
    }
}


void
MarutUdpChannel::ScheduleFlush (bool delay)
{
  if (m_flushQueue.size () == 0)
    {
      return;
    }
  else if (m_flushQueue.size () > 1)
    {
      m_flushEvent.Cancel ();
      Flush ();
    }
  else
    {
      m_flushEvent.Cancel ();
      if (!delay)
        {
          m_flushEvent = Simulator::Schedule (MilliSeconds (1), &MarutUdpChannel::Flush, this);
        }
      else
        {
          m_flushEvent = Simulator::Schedule (rttEstimator.baseRtt, &MarutUdpChannel::Flush, this);
        }
    }
}

MarutBktapCircuit::MarutBktapCircuit (uint16_t id) : BaseCircuit (id)
{
  inboundQueue = Create<MarutSeqQueue> ();
  outboundQueue = Create<MarutSeqQueue> ();
}

CellDirection
MarutBktapCircuit::GetDirection (Ptr<MarutUdpChannel> ch)
{
  if (this->outbound == ch)
    {
      return OUTBOUND;
    }
  else
    {
      return INBOUND;
    }
}

Ptr<MarutUdpChannel>
MarutBktapCircuit::GetChannel (CellDirection direction)
{
  if (direction == OUTBOUND)
    {
      return this->outbound;
    }
  else
    {
      return this->inbound;
    }
}

Ptr<MarutSeqQueue>
MarutBktapCircuit::GetQueue (CellDirection direction)
{
  if (direction == OUTBOUND)
    {
      return this->outboundQueue;
    }
  else
    {
      return this->inboundQueue;
    }
}

MarutTorBktapApp::MarutTorBktapApp ()
{
  NS_LOG_FUNCTION (this);
}

MarutTorBktapApp::~MarutTorBktapApp ()
{
  NS_LOG_FUNCTION (this);
}

TypeId
MarutTorBktapApp::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::MarutTorBktapApp")
    .SetParent<TorBaseApp> ()
    .AddConstructor<MarutTorBktapApp> ()
    .AddAttribute ("Nagle", "Enable the Nagle Algorithm for BackTap.",
                   BooleanValue (false),
                   MakeBooleanAccessor (&MarutTorBktapApp::m_nagle),
                   MakeBooleanChecker ());
  return tid;
}

void
MarutTorBktapApp::AddCircuit (int id, Ipv4Address n_ip, int n_conntype, Ipv4Address p_ip, int p_conntype,
                         Ptr<PseudoClientSocket> clientSocket)
{
  TorBaseApp::AddCircuit (id, n_ip, n_conntype, p_ip, p_conntype);

  // ensure unique circ_id
  NS_ASSERT (circuits[id] == 0);

  Ptr<MarutBktapCircuit> circ = Create<MarutBktapCircuit> (id);
  circuits[id] = circ;
  baseCircuits[id] = circ;

  circ->inbound = AddChannel (InetSocketAddress (p_ip,9001),p_conntype);
  circ->inbound->circuits.push_back (circ);
  circ->inbound->SetSocket (clientSocket);

  circ->outbound = AddChannel (InetSocketAddress (n_ip,9001),n_conntype);
  circ->outbound->circuits.push_back (circ);

}

Ptr<MarutUdpChannel>
MarutTorBktapApp::AddChannel (Address remote, int conntype)
{
  // find existing or create new channel-object
  Ptr<MarutUdpChannel> ch = channels[remote];
  if (!ch)
    {
      ch = Create<MarutUdpChannel> (remote, conntype);
      channels[remote] = ch;
    }
  return ch;
}

void
MarutTorBktapApp::StartApplication (void)
{
  //tor proposal #183: smooth bursts & get queued data out earlier
  m_refilltime = MilliSeconds (10);
  TorBaseApp::StartApplication ();
  m_readbucket.SetRefilledCallback (MakeCallback (&MarutTorBktapApp::RefillReadCallback, this));
  m_writebucket.SetRefilledCallback (MakeCallback (&MarutTorBktapApp::RefillWriteCallback, this));

  circit = circuits.begin ();

  m_devQ = GetNode ()->GetDevice (0)->GetObject<PointToPointNetDevice> ()->GetQueue ();
  UintegerValue limit;
  m_devQ->GetAttribute ("MaxPackets", limit);
  m_devQlimit = limit.Get ();

  if (m_socket == 0)
    {
      m_socket = Socket::CreateSocket (GetNode (), UdpSocketFactory::GetTypeId ());
      m_socket->Bind (m_local);
    }

  m_socket->SetRecvCallback (MakeCallback (&MarutTorBktapApp::ReadCallback, this));
  m_socket->SetDataSentCallback (MakeCallback (&MarutTorBktapApp::SocketWriteCallback, this));

  Ipv4Mask ipmask = Ipv4Mask ("255.0.0.0");

  // iterate over all neighboring channels
  map<Address,Ptr<MarutUdpChannel> >::iterator it;
  for ( it = channels.begin (); it != channels.end (); it++ )
    {
      Ptr<MarutUdpChannel> ch = it->second;
      NS_ASSERT (ch);

      if (ch->SpeaksCells ())
        {
          ch->SetSocket (m_socket);
          ch->m_devQ = m_devQ;
          ch->m_devQlimit = m_devQlimit;
        }

      // PseudoSockets only
      if (ipmask.IsMatch (InetSocketAddress::ConvertFrom (ch->m_remote).GetIpv4 (), Ipv4Address ("127.0.0.1")) )
        {
          if (ch->GetType () == SERVEREDGE)
            {
              Ptr<Socket> socket = CreateObject<PseudoServerSocket> ();
              socket->SetRecvCallback (MakeCallback (&MarutTorBktapApp::ReadCallback, this));
              ch->SetSocket (socket);
            }

          if (ch->GetType () == PROXYEDGE)
            {
              if (!ch->m_socket)
                {
                  ch->m_socket = CreateObject<PseudoClientSocket> ();
                }
              ch->m_socket->SetRecvCallback (MakeCallback (&MarutTorBktapApp::ReadCallback, this));
            }
        }
    }
}

void
MarutTorBktapApp::StopApplication (void)
{
  m_socket->Close ();
  m_socket->SetRecvCallback (MakeNullCallback<void, Ptr<Socket> > ());
  m_socket->SetDataSentCallback (MakeNullCallback<void, Ptr<Socket>, uint32_t > ());
}

void
MarutTorBktapApp::RefillReadCallback (int64_t prev_read_bucket)
{
  vector<Ptr<Socket> > v;
  map<Address,Ptr<MarutUdpChannel> >::iterator it;
  for ( it = channels.begin (); it != channels.end (); it++ )
    {
      Ptr<Socket> socket = it->second->m_socket;
      if (std::find (v.begin (), v.end (),socket) == v.end ())
        {
          Simulator::Schedule (Seconds (0), &MarutTorBktapApp::ReadCallback, this, it->second->m_socket);
          v.push_back (socket);
        }
    }
}

void
MarutTorBktapApp::RefillWriteCallback (int64_t prev_read_bucket)
{
  if (prev_read_bucket <= 0 && writeevent.IsExpired ())
    {
      writeevent = Simulator::ScheduleNow (&MarutTorBktapApp::WriteCallback, this);
    }
}

void
MarutTorBktapApp::ReadCallback (Ptr<Socket> socket)
{
  uint32_t read_bytes = 0;
  if (socket->GetSocketType () == NS3_SOCK_STREAM)
    {
      read_bytes = ReadFromEdge (socket);
    }
  else
    {
      read_bytes = ReadFromRelay (socket);
    }

  if (writeevent.IsExpired ())
    {
      writeevent = Simulator::Schedule (NanoSeconds (read_bytes * 2),&MarutTorBktapApp::WriteCallback, this);
    }
}

uint32_t
MarutTorBktapApp::ReadFromRelay (Ptr<Socket> socket) {
  uint32_t read_bytes = 0;
  while (socket->GetRxAvailable () > 0) {
      Ptr<Packet> data;
      Address from;
      if (data = socket->RecvFrom (from)) {
          data->RemoveAllPacketTags (); //Fix for ns3 PacketTag Bug
          read_bytes += data->GetSize ();
          Ptr<MarutUdpChannel> ch = channels[from];
          NS_ASSERT (ch);
          while (data->GetSize () > 0) {
              BaseCellHeader header;
              data->PeekHeader (header);
              Ptr<MarutBktapCircuit> circ = circuits[header.circId];
              NS_ASSERT (circ);
              CellDirection direction = circ->GetDirection (ch);
              CellDirection oppdir = circ->GetOppositeDirection (direction);
              if (header.cellType == FDBK) {
                  FdbkCellHeader h;
                  data->RemoveHeader (h);
                  circ->IncrementStats (oppdir,h.GetSerializedSize (),0);
                  if (h.flags & ACK) {
                      ReceivedAck (circ,direction,h);
                  }
                  if (h.flags & FWD) {
                      ReceivedFwd (circ,direction,h);
                  }
              }
              else
                {
                  Ptr<Packet> cell = data->CreateFragment (0,CELL_PAYLOAD_SIZE + UDP_CELL_HEADER_SIZE);
                  data->RemoveAtStart (CELL_PAYLOAD_SIZE + UDP_CELL_HEADER_SIZE);
                  circ->IncrementStats (oppdir,cell->GetSize (),0);
                  ReceivedRelayCell (circ,oppdir,cell);
                }
            }
        }
    }
  return read_bytes;
}

void
MarutTorBktapApp::ReceivedRelayCell (Ptr<MarutBktapCircuit> circ, CellDirection direction, Ptr<Packet> cell)
{
 Ptr<MarutSeqQueue> queue = circ->GetQueue (direction);
 cout << "Node: " << GetNodeName() << " Received Relay Cell " << endl;
 UdpCellHeader header;
  cell->PeekHeader (header);
  bool newseq = queue->Add (cell, header.seq);
  if (newseq) {
    m_readbucket.Decrement(cell->GetSize());
  }
  CellDirection oppdir = circ->GetOppositeDirection (direction);
  SendFeedbackCell (circ, oppdir, ACK, queue->tailSeq + 1);
}


void
MarutTorBktapApp::ReceivedAck (Ptr<MarutBktapCircuit> circ, CellDirection direction, FdbkCellHeader header)
{
 Ptr<MarutSeqQueue> queue = circ->GetQueue (direction);
 cout << "Node: " << GetNodeName() << " Received Ack " << endl;
 if (header.ack == queue->headSeq)
    {
      // DupACK. Do fast retransmit.
      ++queue->dupackcnt;
      if (m_writebucket.GetSize () >= CELL_PAYLOAD_SIZE && queue->dupackcnt > 2)
        {
          FlushPendingCell (circ,direction,true);
          queue->dupackcnt = 0;
        }
    }
  else if (header.ack > queue->headSeq)
    {
      //NewAck
      queue->dupackcnt = 0;
      queue->DiscardUpTo (header.ack);
      Time rtt = queue->actRtt.EstimateRtt (header.ack);
      ScheduleRto (circ,direction,true);
      if (!queue->PackageInflight ())
        {
          Ptr<MarutUdpChannel> ch = circ->GetChannel(direction);
          ch->ScheduleFlush(false);
        }
    }
  else
    {
      cerr << GetNodeName () << " Ignore Ack" << endl;
    }
}

//UPDATE cwnd for endhost (proxy node or server node)
void
MarutTorBktapApp::WindowUpdate (Ptr<MarutSeqQueue> queue, Time baseRtt, uint16_t circ_id, CellDirection direction) {
//   if (queue->virtRtt.cntRtt > 2) {
  cout << "Node: " << GetNodeName() <<", CircuitId: "<< circ_id <<", Direction: "<<CellDirectionArray[static_cast<int>(direction)] <<", Updating Window, cwnd=" << queue->cwnd << ", Circuit Diff:"  << queue->circ_diff << ", " <<  queue->circ_diff / 10000. << endl;
  double c_diff = queue->circ_diff / 10000.;
  if (c_diff < VEGASALPHA) {
      ++queue->cwnd;
  }

  if (c_diff > VEGASBETA) {
      --queue->cwnd;
  }

  if (queue->cwnd < 1) {
      queue->cwnd = 1;
  }

  double maxexp = m_burst.GetBitRate () / 8 / CELL_PAYLOAD_SIZE * baseRtt.GetSeconds (); 
  queue->cwnd = min (queue->cwnd, (uint32_t) maxexp);

  cout << "Node: " << GetNodeName() <<", CircuitId: "<< circ_id <<", Direction: "<<CellDirectionArray[static_cast<int>(direction)] << ", Updated Window, cwnd=" << queue->cwnd << endl;
//   }
}

void
MarutTorBktapApp::CongestionAvoidance (Ptr<MarutSeqQueue> queue, uint64_t packet_diff, Time baseRtt, uint16_t circ_id, CellDirection direction) {
 //Do the Vegas-thing every RTT
  cout << "Node: " << GetNodeName() <<", CircuitId: "<< circ_id <<", Direction: "<<CellDirectionArray[static_cast<int>(direction)] << ", Updating congestion, circ_diff=" << queue->circ_diff << endl;
//  if (queue->virtRtt.cntRtt > 2) {
      Time rtt = queue->virtRtt.currentRtt;
      double diff = queue->cwnd * (rtt.GetSeconds () - baseRtt.GetSeconds ()) / baseRtt.GetSeconds ();

//      cout << "Node: " << GetNodeName() << ", diff=" << diff << endl;
			//DO NOT UPDATE QUEUE HERE. JUST CALCULATE THE DIFF FOR A CIRCUIT AND DIRECTION (QUEUE)

      queue->diff = diff * 10000;
//      cout << "Node: " << GetNodeName() << ", queue->diff=" << queue->diff << endl;
//  }

			double c_diff = packet_diff / 10000.;
//      cout << "Node: " << GetNodeName() << ", packet c_diff=" << c_diff << endl;
  
			c_diff = max (queue->diff / 10000., c_diff);

//      cout << "Node: " << GetNodeName() << ", actual c_diff=" << c_diff << endl;

  		queue->circ_diff = c_diff * 10000;	

      queue->virtRtt.ResetCurrRtt ();

      cout << "Node: " << GetNodeName() <<", CircuitId: "<< circ_id <<", Direction: "<<CellDirectionArray[static_cast<int>(direction)] << " Updated congestion, circ_diff=" << queue->circ_diff << endl;
//  }
      // Vegas falls back to Reno CA, i.e. increase per RTT
      // However, This messes up with our backlog and makes the approach too aggressive.
}

void
MarutTorBktapApp::ReceivedFwd (Ptr<MarutBktapCircuit> circ, CellDirection direction, FdbkCellHeader header) {
  cout << "Node: " << GetNodeName() << " Received Fwd Cell" << endl;
  //Received flow control feeback (FWD)
  Ptr<MarutSeqQueue> queue = circ->GetQueue (direction);
  Ptr<MarutUdpChannel> ch = circ->GetChannel (direction);

  CellDirection oppdir = circ->GetOppositeDirection (direction);
  Ptr<MarutUdpChannel> oppch = circ->GetChannel (oppdir);

  Time rtt = queue->virtRtt.EstimateRtt (header.fwd);
  ch->rttEstimator.AddSample (rtt);

  if (queue->virtHeadSeq <= header.fwd) {
      queue->virtHeadSeq = header.fwd;
  }

  if (header.fwd > queue->begRttSeq) {
      queue->begRttSeq = queue->nextTxSeq;
      CongestionAvoidance (queue, header.diff, ch->rttEstimator.baseRtt, circ->GetId (), direction);
			//Only for Edge Tor Nodes
			if (!(oppch->SpeaksCells())) {	
					WindowUpdate(queue, ch->rttEstimator.baseRtt, circ->GetId (), direction);
			}
      //queue->ssthresh = min (queue->cwnd,queue->ssthresh);
      //queue->ssthresh = max (queue->ssthresh,queue->cwnd / 2);
  }
  else if (queue->cwnd <= queue->ssthresh) {
      //TODO test different slow start schemes
  }

  Simulator::Schedule (Seconds (0), &MarutTorBktapApp::ReadCallback, this, oppch->m_socket);

  if (writeevent.IsExpired ()) {
      writeevent = Simulator::Schedule (Seconds (0), &MarutTorBktapApp::WriteCallback, this);
  }
}

uint32_t
MarutTorBktapApp::ReadFromEdge (Ptr<Socket> socket)
{
  Ptr<MarutUdpChannel> ch = LookupChannel (socket);
  NS_ASSERT (ch);
  Ptr<MarutBktapCircuit> circ = ch->circuits.front ();
  NS_ASSERT (circ);
  CellDirection direction = circ->GetDirection (ch);
  CellDirection oppdir = circ->GetOppositeDirection (direction);
  Ptr<MarutSeqQueue> queue = circ->GetQueue (oppdir);

  uint32_t max_read = (queue->cwnd - queue->VirtSize () <= 0) ? 0 : queue->cwnd - queue->VirtSize ();
  max_read *= CELL_PAYLOAD_SIZE;

  uint32_t read_bytes = 0;

  while (max_read - read_bytes >= CELL_PAYLOAD_SIZE && socket->GetRxAvailable () >= CELL_PAYLOAD_SIZE)
    {
      Ptr<Packet> data = socket->Recv (CELL_PAYLOAD_SIZE, 0);
      data->RemoveAllPacketTags (); //Fix for ns3 PacketTag Bug
      read_bytes += data->GetSize ();
      m_readbucket.Decrement (data->GetSize ());
      circ->IncrementStats (oppdir,data->GetSize (),0);
      PackageRelayCell (circ, oppdir, data);
    }

  return read_bytes;
}

void
MarutTorBktapApp::PackageRelayCell (Ptr<MarutBktapCircuit> circ, CellDirection direction, Ptr<Packet> cell)
{
  cout << "Node: " << GetNodeName() << " Relay Cell Packaged " << endl;
  UdpCellHeader header;
  header.circId = circ->GetId ();
  Ptr<MarutSeqQueue> queue = circ->GetQueue (direction);
  NS_ASSERT (queue);
  header.seq = queue->tailSeq + 1;
  cell->AddHeader (header);
  queue->virtRtt.SentSeq (header.seq);
  queue->actRtt.SentSeq (header.seq);
  queue->Add (cell, header.seq);
}

void
MarutTorBktapApp::SocketWriteCallback (Ptr<Socket> s, uint32_t i)
{
  if (writeevent.IsExpired ())
    {
      writeevent = Simulator::Schedule (Seconds (0), &MarutTorBktapApp::WriteCallback, this);
    }
}

void
MarutTorBktapApp::WriteCallback ()
{
  uint32_t bytes_written = 0;

  if (m_writebucket.GetSize () >= CELL_PAYLOAD_SIZE)
    {
      Ptr<MarutBktapCircuit> start = circit->second;
      Ptr<MarutBktapCircuit> circ;

      while (bytes_written == 0 && start != circ)
        {
          circ = GetNextCircuit ();
          bytes_written += FlushPendingCell (circ,INBOUND);
          bytes_written += FlushPendingCell (circ,OUTBOUND);
        }
    }

  if (bytes_written > 0)
    {
      // try flushing more ...
      if (writeevent.IsExpired ())
        {
          writeevent = Simulator::ScheduleNow (&MarutTorBktapApp::WriteCallback, this);
        }
    }
}



uint32_t
MarutTorBktapApp::FlushPendingCell (Ptr<MarutBktapCircuit> circ, CellDirection direction, bool retx) {
  Ptr<MarutSeqQueue> queue = circ->GetQueue (direction);
  cout << "Node: " << GetNodeName() << ", Flush Pending Cell" << endl;

  CellDirection oppdir   = circ->GetOppositeDirection (direction);
  Ptr<MarutUdpChannel> ch  = circ->GetChannel (direction);
  Ptr<MarutUdpChannel> oppch = circ->GetChannel (oppdir);
  Ptr<Packet> cell;

//Only check Window if NOT a MIDDLE NODE. 
//ToDo:Should we also add a check (!ch->SpeaksCells() && oppch->SpeaksCells())
  if (!(ch->SpeaksCells() && oppch->SpeaksCells()) && queue->Window () <= 0 && !retx) {
      cout << "Node: " << GetNodeName() << ", Edge Node with window less than 0" << endl;
      return 0;
  }

  if (retx) {
      cell = queue->GetCell (queue->headSeq);
      queue->dupackcnt = 0;
  }
  else {
      cell = queue->GetNextCell ();
  }

  if (cell) {
      UdpCellHeader header;
      cell->PeekHeader (header);
      if (!ch->SpeaksCells ()) {
          cell->RemoveHeader (header);
      }

//************************
//What is this doing here?
//Think they are ensuring its the middle node here.
//For edge nodes data going in we use PackageRelayCell 
      if (circ->GetChannel (oppdir)->SpeaksCells ()) {
          queue->virtRtt.SentSeq (header.seq);
          queue->actRtt.SentSeq (header.seq);
      }

      ch->m_flushQueue.push (cell);
      int bytes_written = cell->GetSize ();
      ch->ScheduleFlush (m_nagle && queue->PackageInflight ());

      if (ch->SpeaksCells ()) {
          ScheduleRto (circ,direction,true);
      }
      else {
          queue->DiscardUpTo (header.seq + 1);
          ++queue->virtHeadSeq;
      }

      if (queue->highestTxSeq == header.seq) {
          circ->IncrementStats (direction,0,bytes_written);
          SendFeedbackCell (circ, oppdir, FWD, queue->highestTxSeq + 1);
      }

      if (!queue->WasRetransmit()) {
        m_writebucket.Decrement(bytes_written);
      }

      return bytes_written;
    }
    cout << "Node: " << GetNodeName() << ", Node has no data to flush" << endl;
  return 0;
}

void
MarutTorBktapApp::SendFeedbackCell (Ptr<MarutBktapCircuit> circ, CellDirection direction, uint8_t flag, uint32_t ack) {
  cout << "Node: " << GetNodeName() << ", Sending feedback cell " << ", isAck:" << (flag & ACK) <<", isFwd:"<< (flag & FWD) << ", ack:" << ack << endl;
  Ptr<MarutUdpChannel> ch = circ->GetChannel (direction);
  Ptr<MarutSeqQueue> queue = circ->GetQueue (direction);
  NS_ASSERT (ch);
  if (ch->SpeaksCells ()) {
      if (flag & ACK) {
          queue->ackq.push (ack);
      }
      if (flag & FWD) {
          queue->fwdq.push (ack);
      }
      if (queue->ackq.size () > 0 && queue->fwdq.size () > 0) {
          queue->delFeedbackEvent.Cancel ();
          PushFeedbackCell (circ, direction);
      }
      else {
          queue->delFeedbackEvent = Simulator::Schedule (MilliSeconds (1), &MarutTorBktapApp::PushFeedbackCell, this, circ, direction);
      }
  }
}

void
MarutTorBktapApp::PushFeedbackCell (Ptr<MarutBktapCircuit> circ, CellDirection direction) {
  Ptr<MarutUdpChannel> ch = circ->GetChannel (direction);
  Ptr<MarutSeqQueue> queue = circ->GetQueue (direction);

  CellDirection oppdir   = circ->GetOppositeDirection (direction);
  Ptr<MarutSeqQueue> oppqueue = circ->GetQueue (oppdir);

  NS_ASSERT (ch);

  while (queue->ackq.size () > 0 || queue->fwdq.size () > 0) {
      Ptr<Packet> cell = Create<Packet> ();
      FdbkCellHeader header;
      header.circId = circ->GetId ();
      if (queue->ackq.size () > 0) {
          header.flags |= ACK;
          while (queue->ackq.size () > 0 && header.ack < queue->ackq.front ()) {
              header.ack = queue->ackq.front ();
              queue->ackq.pop ();
          }
      }
      if (queue->fwdq.size () > 0) {
          header.flags |= FWD;
          header.fwd = queue->fwdq.front ();
          queue->fwdq.pop ();
      }

			header.diff = oppqueue->circ_diff;

      cell->AddHeader (header);
      ch->m_flushQueue.push (cell);
      ch->ScheduleFlush ();
  }
}

void
MarutTorBktapApp::ScheduleRto (Ptr<MarutBktapCircuit> circ, CellDirection direction, bool force)
{
  Ptr<MarutSeqQueue> queue = circ->GetQueue (direction);
  if (force)
    {
      queue->retxEvent.Cancel ();
    }
  if (queue->Inflight () <= 0)
    {
      return;
    }
  if (queue->retxEvent.IsExpired ())
    {
      queue->retxEvent = Simulator::Schedule (queue->actRtt.Rto (), &MarutTorBktapApp::Rto, this, circ, direction);
    }
}

void
MarutTorBktapApp::Rto (Ptr<MarutBktapCircuit> circ, CellDirection direction)
{
  Ptr<MarutSeqQueue> queue = circ->GetQueue (direction);
  queue->nextTxSeq = queue->headSeq;
  FlushPendingCell (circ,direction);
}

Ptr<MarutBktapCircuit>
MarutTorBktapApp::GetCircuit (uint16_t id)
{
  return circuits[id];
}

Ptr<MarutBktapCircuit>
MarutTorBktapApp::GetNextCircuit ()
{
  ++circit;
  if (circit == circuits.end ())
    {
      circit = circuits.begin ();
    }
  return circit->second;
}

Ptr<MarutUdpChannel>
MarutTorBktapApp::LookupChannel (Ptr<Socket> socket)
{
  map<Address,Ptr<MarutUdpChannel> >::iterator it;
  for ( it = channels.begin (); it != channels.end (); it++ )
    {
      NS_ASSERT (it->second);
      if (it->second->m_socket == socket)
        {
          return it->second;
        }
    }
  return NULL;
}

void
MarutTorBktapApp::DoDispose (void)
{
  NS_LOG_FUNCTION (this);

  map<uint16_t,Ptr<MarutBktapCircuit> >::iterator i;
  for (i = circuits.begin (); i != circuits.end (); ++i)
    {
      i->second->inbound = 0;
      i->second->outbound = 0;
      i->second->inboundQueue = 0;
      i->second->outboundQueue = 0;

    }
  circuits.clear ();
  baseCircuits.clear ();
  channels.clear ();
  Application::DoDispose ();
}

} //namespace ns3
