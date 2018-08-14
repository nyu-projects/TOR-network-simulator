#include "ns3/log.h"
#include "tor-timely.h"
#include <cmath>

using namespace std;
namespace ns3 {
//  std::string CellDirectionArray[2] =
//  {
//          "INBOUND",
//          "OUTBOUND"
//  };


NS_LOG_COMPONENT_DEFINE ("TorTimelyApp");
NS_OBJECT_ENSURE_REGISTERED (TorTimelyApp);

TimelyUdpChannel::TimelyUdpChannel ()
{
  NS_LOG_FUNCTION (this);
  this->m_socket = 0;
}

TimelyUdpChannel::TimelyUdpChannel (Address remote, int conntype)
{
  m_remote = remote;
  m_conntype = conntype;
  this->m_socket = 0;
}

void
TimelyUdpChannel::SetSocket (Ptr<Socket> socket)
{
  this->m_socket = socket;
}

uint8_t
TimelyUdpChannel::GetType ()
{
  return m_conntype;
}

bool
TimelyUdpChannel::SpeaksCells ()
{
  return m_conntype == RELAYEDGE;
}

void
TimelyUdpChannel::Flush ()
{
  while (m_flushQueue.size () > 0)
    {
      if (SpeaksCells () && m_devQlimit <= m_devQ->GetNPackets ())
        {
          m_flushEvent = Simulator::Schedule (MilliSeconds (1), &TimelyUdpChannel::Flush, this);
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
TimelyUdpChannel::ScheduleFlush (bool delay)
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
          m_flushEvent = Simulator::Schedule (MilliSeconds (1), &TimelyUdpChannel::Flush, this);
        }
      else
        {
          m_flushEvent = Simulator::Schedule (rttEstimator.baseRtt, &TimelyUdpChannel::Flush, this);
        }
    }
}

TimelyCircuit::TimelyCircuit (uint16_t id) : BaseCircuit (id)
{
  inboundQueue = Create<TimelySeqQueue> ();
  outboundQueue = Create<TimelySeqQueue> ();
}

CellDirection
TimelyCircuit::GetDirection (Ptr<TimelyUdpChannel> ch)
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

Ptr<TimelyUdpChannel>
TimelyCircuit::GetChannel (CellDirection direction)
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

Ptr<TimelySeqQueue>
TimelyCircuit::GetQueue (CellDirection direction)
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

TorTimelyApp::TorTimelyApp ()
{
  NS_LOG_FUNCTION (this);
}

TorTimelyApp::~TorTimelyApp ()
{
  NS_LOG_FUNCTION (this);
}

TypeId
TorTimelyApp::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::TorTimelyApp")
    .SetParent<TorBaseApp> ()
    .AddConstructor<TorTimelyApp> ()
    .AddAttribute ("Nagle", "Enable the Nagle Algorithm for BackTap.",
                   BooleanValue (false),
                   MakeBooleanAccessor (&TorTimelyApp::m_nagle),
                   MakeBooleanChecker ());
  return tid;
}

void
TorTimelyApp::AddCircuit (int id, Ipv4Address n_ip, int n_conntype, Ipv4Address p_ip, int p_conntype,
                         Ptr<PseudoClientSocket> clientSocket)
{
  TorBaseApp::AddCircuit (id, n_ip, n_conntype, p_ip, p_conntype);

  // ensure unique circ_id
  NS_ASSERT (circuits[id] == 0);

  Ptr<TimelyCircuit> circ = Create<TimelyCircuit> (id);
  circuits[id] = circ;
  baseCircuits[id] = circ;

  circ->inbound = AddChannel (InetSocketAddress (p_ip,9001),p_conntype);
  circ->inbound->circuits.push_back (circ);
  circ->inbound->SetSocket (clientSocket);

  circ->outbound = AddChannel (InetSocketAddress (n_ip,9001),n_conntype);
  circ->outbound->circuits.push_back (circ);

}

Ptr<TimelyUdpChannel>
TorTimelyApp::AddChannel (Address remote, int conntype)
{
  // find existing or create new channel-object
  Ptr<TimelyUdpChannel> ch = channels[remote];
  if (!ch)
    {
      ch = Create<TimelyUdpChannel> (remote, conntype);
      channels[remote] = ch;
    }
  return ch;
}

void
TorTimelyApp::StartApplication (void)
{
  //tor proposal #183: smooth bursts & get queued data out earlier
  m_refilltime = MilliSeconds (10);
  TorBaseApp::StartApplication ();
  m_readbucket.SetRefilledCallback (MakeCallback (&TorTimelyApp::RefillReadCallback, this));
  m_writebucket.SetRefilledCallback (MakeCallback (&TorTimelyApp::RefillWriteCallback, this));

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

  m_socket->SetRecvCallback (MakeCallback (&TorTimelyApp::ReadCallback, this));
  m_socket->SetDataSentCallback (MakeCallback (&TorTimelyApp::SocketWriteCallback, this));

  Ipv4Mask ipmask = Ipv4Mask ("255.0.0.0");

  // iterate over all neighboring channels
  map<Address,Ptr<TimelyUdpChannel> >::iterator it;
  for ( it = channels.begin (); it != channels.end (); it++ )
    {
      Ptr<TimelyUdpChannel> ch = it->second;
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
              socket->SetRecvCallback (MakeCallback (&TorTimelyApp::ReadCallback, this));
              ch->SetSocket (socket);
            }

          if (ch->GetType () == PROXYEDGE)
            {
              if (!ch->m_socket)
                {
                  ch->m_socket = CreateObject<PseudoClientSocket> ();
                }
              ch->m_socket->SetRecvCallback (MakeCallback (&TorTimelyApp::ReadCallback, this));
            }
        }
    }
}

void
TorTimelyApp::StopApplication (void)
{
  m_socket->Close ();
  m_socket->SetRecvCallback (MakeNullCallback<void, Ptr<Socket> > ());
  m_socket->SetDataSentCallback (MakeNullCallback<void, Ptr<Socket>, uint32_t > ());
}

void
TorTimelyApp::RefillReadCallback (int64_t prev_read_bucket)
{
  vector<Ptr<Socket> > v;
  map<Address,Ptr<TimelyUdpChannel> >::iterator it;
  for ( it = channels.begin (); it != channels.end (); it++ )
    {
      Ptr<Socket> socket = it->second->m_socket;
      if (std::find (v.begin (), v.end (),socket) == v.end ())
        {
          Simulator::Schedule (Seconds (0), &TorTimelyApp::ReadCallback, this, it->second->m_socket);
          v.push_back (socket);
        }
    }
}

void
TorTimelyApp::RefillWriteCallback (int64_t prev_read_bucket)
{
  if (prev_read_bucket <= 0 && writeevent.IsExpired ())
    {
      writeevent = Simulator::ScheduleNow (&TorTimelyApp::WriteCallback, this);
    }
}

void
TorTimelyApp::ReadCallback (Ptr<Socket> socket)
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
      writeevent = Simulator::Schedule (NanoSeconds (read_bytes * 2),&TorTimelyApp::WriteCallback, this);
    }
}

uint32_t
TorTimelyApp::ReadFromRelay (Ptr<Socket> socket) {
  uint32_t read_bytes = 0;
  while (socket->GetRxAvailable() > 0) {
      Ptr<Packet> data;
      Address from;
      if (data = socket->RecvFrom (from)) {
          data->RemoveAllPacketTags (); //Fix for ns3 PacketTag Bug
          read_bytes += data->GetSize ();
          Ptr<TimelyUdpChannel> ch = channels[from];
          NS_ASSERT (ch);
          while (data->GetSize () > 0) {
              BaseCellHeader header;
              data->PeekHeader (header);
              Ptr<TimelyCircuit> circ = circuits[header.circId];
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
              }
              else {
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
TorTimelyApp::ReceivedRelayCell (Ptr<TimelyCircuit> circ, CellDirection direction, Ptr<Packet> cell) {
  Ptr<TimelySeqQueue> queue = circ->GetQueue (direction);
  Ptr<TimelyUdpChannel> ch = circ->GetChannel (direction);

  cout << "Node: " << GetNodeName() << " Received Relay Cell " << endl;
  UdpCellHeader header;
  cell->PeekHeader (header);
  bool newseq = queue->Add (cell, header.seq);
  if (newseq) {
    m_readbucket.Decrement(cell->GetSize());
  }

  CellDirection oppdir = circ->GetOppositeDirection (direction);
  //Only send ack cell from endnode
  if (!(ch->SpeaksCells())) {
    cout << "Node: " << GetNodeName() << " Sending ACK Cell with header: " << queue->tailSeq + 1 <<endl;
    SendFeedbackCell (circ, oppdir, ACK, queue->tailSeq + 1);
  }
}

void
TorTimelyApp::ReceivedAck (Ptr<TimelyCircuit> circ, CellDirection direction, FdbkCellHeader header) {
  Ptr<TimelySeqQueue> queue = circ->GetQueue (direction);
  Ptr<TimelyUdpChannel> ch = circ->GetChannel (direction);

  CellDirection oppdir   = circ->GetOppositeDirection(direction);
  Ptr<TimelyUdpChannel> oppch = circ->GetChannel(oppdir);

  if (ch->SpeaksCells() && oppch->SpeaksCells()) {
    cout << "Node: " << GetNodeName() <<" Received Ack at relay node with header: "<< header.ack <<endl;
		SendFeedbackCell (circ, direction, ACK, header.ack);
  } else {
    cout << "Node: " << GetNodeName() << " Received Ack at end node " << header.ack << endl;
    if (header.ack == queue->headSeq) {
        // DupACK. Do fast retransmit.
        cout << "Node: " << GetNodeName() << " Duplicate Ack " << endl;
        ++queue->dupackcnt;
        if (m_writebucket.GetSize () >= CELL_PAYLOAD_SIZE && queue->dupackcnt > 2) {
            FlushPendingCell (circ,direction,true);
            queue->dupackcnt = 0;
        }
    }
    else if (header.ack > queue->headSeq) {
        //NewAck
        cout << "Node: " << GetNodeName() << " Valid Ack " << endl;
        queue->dupackcnt = 0;
        queue->DiscardUpTo (header.ack);
        Time rtt = queue->actRtt.EstimateRtt (header.ack);
        ScheduleRto (circ,direction,true);
        CongestionAvoidance(queue, rtt);
        if (!queue->PackageInflight ()) {
            Ptr<TimelyUdpChannel> ch = circ->GetChannel(direction);
            ch->ScheduleFlush(false);
        }
    }
    else {
        cerr << GetNodeName () << " Ignore Ack" << endl;
    }
  }
}

void TorTimelyApp::CongestionAvoidance (Ptr<TimelySeqQueue> queue, Time new_rtt) {
  queue->actRtt.AddSample(new_rtt);
  queue->cwnd = queue->cwnd * (1 - beta * queue->actRtt.norm_gradient);
}

uint32_t
TorTimelyApp::ReadFromEdge (Ptr<Socket> socket)
{
  Ptr<TimelyUdpChannel> ch = LookupChannel (socket);
  NS_ASSERT (ch);
  Ptr<TimelyCircuit> circ = ch->circuits.front ();
  NS_ASSERT (circ);
  CellDirection direction = circ->GetDirection (ch);
  CellDirection oppdir = circ->GetOppositeDirection (direction);
  Ptr<TimelySeqQueue> queue = circ->GetQueue (oppdir);

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
TorTimelyApp::PackageRelayCell (Ptr<TimelyCircuit> circ, CellDirection direction, Ptr<Packet> cell)
{
  cout << "Node: " << GetNodeName() << " Relay Cell Packaged " << endl;
  UdpCellHeader header;
  header.circId = circ->GetId ();
  Ptr<TimelySeqQueue> queue = circ->GetQueue (direction);
  NS_ASSERT (queue);
  header.seq = queue->tailSeq + 1;
  cell->AddHeader (header);

  queue->actRtt.SentSeq (header.seq);
  queue->Add (cell, header.seq);
}

void
TorTimelyApp::SocketWriteCallback (Ptr<Socket> s, uint32_t i)
{
  if (writeevent.IsExpired ())
    {
      writeevent = Simulator::Schedule (Seconds (0), &TorTimelyApp::WriteCallback, this);
    }
}

void
TorTimelyApp::WriteCallback ()
{
  uint32_t bytes_written = 0;

  if (m_writebucket.GetSize () >= CELL_PAYLOAD_SIZE)
    {
      Ptr<TimelyCircuit> start = circit->second;
      Ptr<TimelyCircuit> circ;

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
          writeevent = Simulator::ScheduleNow (&TorTimelyApp::WriteCallback, this);
        }
    }
}



uint32_t
TorTimelyApp::FlushPendingCell (Ptr<TimelyCircuit> circ, CellDirection direction, bool retx) {
  Ptr<TimelySeqQueue> queue = circ->GetQueue (direction);
//  cout << "Node: " << GetNodeName() << ", Flush Pending Cell" << endl;

  CellDirection oppdir   = circ->GetOppositeDirection (direction);
  Ptr<TimelyUdpChannel> ch  = circ->GetChannel (direction);
  Ptr<TimelyUdpChannel> oppch = circ->GetChannel (oppdir);
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
      }

      if (!queue->WasRetransmit()) {
        m_writebucket.Decrement(bytes_written);
      }

      return bytes_written;
    }
    //cout << "Node: " << GetNodeName() << ", Node has no data to flush" << endl;
  return 0;
}

void
TorTimelyApp::SendFeedbackCell (Ptr<TimelyCircuit> circ, CellDirection direction, uint8_t flag, uint32_t ack) {
  cout << "Node: " << GetNodeName() << ", Sending feedback cell " << ", isAck:" << (flag & ACK) << ", ack:" << ack << endl;
  Ptr<TimelyUdpChannel> ch = circ->GetChannel (direction);
  Ptr<TimelySeqQueue> queue = circ->GetQueue (direction);
  NS_ASSERT (ch);
  if (ch->SpeaksCells ()) {
      if (flag & ACK) {
          queue->ackq.push (ack);
      }
      if (queue->ackq.size () > 0) {
          queue->delFeedbackEvent.Cancel ();
          PushFeedbackCell (circ, direction);
      }
      else {
          queue->delFeedbackEvent = Simulator::Schedule (MilliSeconds (1), &TorTimelyApp::PushFeedbackCell, this, circ, direction);
      }
  }
}

void
TorTimelyApp::PushFeedbackCell (Ptr<TimelyCircuit> circ, CellDirection direction) {
  Ptr<TimelyUdpChannel> ch = circ->GetChannel (direction);
  Ptr<TimelySeqQueue> queue = circ->GetQueue (direction);

  CellDirection oppdir   = circ->GetOppositeDirection (direction);
  Ptr<TimelySeqQueue> oppqueue = circ->GetQueue (oppdir);

  NS_ASSERT (ch);

  while (queue->ackq.size () > 0) {
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

      cell->AddHeader (header);
      ch->m_flushQueue.push (cell);
      ch->ScheduleFlush ();
  }
}

void
TorTimelyApp::ScheduleRto (Ptr<TimelyCircuit> circ, CellDirection direction, bool force)
{
  Ptr<TimelySeqQueue> queue = circ->GetQueue (direction);
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
      queue->retxEvent = Simulator::Schedule (queue->actRtt.Rto (), &TorTimelyApp::Rto, this, circ, direction);
    }
}

void
TorTimelyApp::Rto (Ptr<TimelyCircuit> circ, CellDirection direction)
{
  Ptr<TimelySeqQueue> queue = circ->GetQueue (direction);
  queue->nextTxSeq = queue->headSeq;
  FlushPendingCell (circ,direction);
}

Ptr<TimelyCircuit>
TorTimelyApp::GetCircuit (uint16_t id)
{
  return circuits[id];
}

Ptr<TimelyCircuit>
TorTimelyApp::GetNextCircuit ()
{
  ++circit;
  if (circit == circuits.end ())
    {
      circit = circuits.begin ();
    }
  return circit->second;
}

Ptr<TimelyUdpChannel>
TorTimelyApp::LookupChannel (Ptr<Socket> socket)
{
  map<Address,Ptr<TimelyUdpChannel> >::iterator it;
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
TorTimelyApp::DoDispose (void)
{
  NS_LOG_FUNCTION (this);

  map<uint16_t,Ptr<TimelyCircuit> >::iterator i;
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
