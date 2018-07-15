#include "ns3/log.h"
#include "tor-bbr.h"
#include <cmath>

using namespace std;

namespace ns3 {

NS_LOG_COMPONENT_DEFINE ("BBRTorBktapApp");
NS_OBJECT_ENSURE_REGISTERED (BBRTorBktapApp);

BBRUdpChannel::BBRUdpChannel ()
{
  NS_LOG_FUNCTION (this);
  this->m_socket = 0;
}

BBRUdpChannel::BBRUdpChannel (Address remote, int conntype)
{
  m_remote = remote;
  m_conntype = conntype;
  this->m_socket = 0;
}

void
BBRUdpChannel::SetSocket (Ptr<Socket> socket)
{
  this->m_socket = socket;
}

uint8_t
BBRUdpChannel::GetType ()
{
  return m_conntype;
}

bool
BBRUdpChannel::SpeaksCells ()
{
  return m_conntype == RELAYEDGE;
}

void
BBRUdpChannel::Flush ()
{
  while (m_flushQueue.size () > 0)
    {
      if (SpeaksCells () && m_devQlimit <= m_devQ->GetNPackets ())
        {
          m_flushEvent = Simulator::Schedule (MilliSeconds (1), &BBRUdpChannel::Flush, this);
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
BBRUdpChannel::ScheduleFlush (bool delay)
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
          m_flushEvent = Simulator::Schedule (MilliSeconds (1), &BBRUdpChannel::Flush, this);
        }
      else
        {
          m_flushEvent = Simulator::Schedule (rttEstimator.baseRtt, &BBRUdpChannel::Flush, this);
        }
    }
}

BBRBktapCircuit::BBRBktapCircuit (uint16_t id) : BaseCircuit (id)
{
  inboundQueue = Create<BBRSeqQueue> ();
  outboundQueue = Create<BBRSeqQueue> ();
}

CellDirection
BBRBktapCircuit::GetDirection (Ptr<BBRUdpChannel> ch)
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

Ptr<BBRUdpChannel>
BBRBktapCircuit::GetChannel (CellDirection direction)
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

Ptr<BBRSeqQueue>
BBRBktapCircuit::GetQueue (CellDirection direction)
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

BBRTorBktapApp::BBRTorBktapApp ()
{
  NS_LOG_FUNCTION (this);
}

BBRTorBktapApp::~BBRTorBktapApp ()
{
  NS_LOG_FUNCTION (this);
}

TypeId
BBRTorBktapApp::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::BBRTorBktapApp")
    .SetParent<TorBaseApp> ()
    .AddConstructor<BBRTorBktapApp> ()
    .AddAttribute ("Nagle", "Enable the Nagle Algorithm for BackTap.",
                   BooleanValue (false),
                   MakeBooleanAccessor (&BBRTorBktapApp::m_nagle),
                   MakeBooleanChecker ());
  return tid;
}

void
BBRTorBktapApp::AddCircuit (int id, Ipv4Address n_ip, int n_conntype, Ipv4Address p_ip, int p_conntype,
                         Ptr<PseudoClientSocket> clientSocket)
{
  TorBaseApp::AddCircuit (id, n_ip, n_conntype, p_ip, p_conntype);

  // ensure unique circ_id
  NS_ASSERT (circuits[id] == 0);

  Ptr<BBRBktapCircuit> circ = Create<BBRBktapCircuit> (id);
  circuits[id] = circ;
  baseCircuits[id] = circ;

  circ->inbound = AddChannel (InetSocketAddress (p_ip,9001),p_conntype);
  circ->inbound->circuits.push_back (circ);
  circ->inbound->SetSocket (clientSocket);

  circ->outbound = AddChannel (InetSocketAddress (n_ip,9001),n_conntype);
  circ->outbound->circuits.push_back (circ);

}

Ptr<BBRUdpChannel>
BBRTorBktapApp::AddChannel (Address remote, int conntype)
{
  // find existing or create new channel-object
  Ptr<BBRUdpChannel> ch = channels[remote];
  if (!ch)
    {
      ch = Create<BBRUdpChannel> (remote, conntype);
      channels[remote] = ch;
    }
  return ch;
}

void
BBRTorBktapApp::StartApplication (void)
{
  //tor proposal #183: smooth bursts & get queued data out earlier
  m_refilltime = MilliSeconds (10);
  TorBaseApp::StartApplication ();
  m_readbucket.SetRefilledCallback (MakeCallback (&BBRTorBktapApp::RefillReadCallback, this));
  m_writebucket.SetRefilledCallback (MakeCallback (&BBRTorBktapApp::RefillWriteCallback, this));

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

  m_socket->SetRecvCallback (MakeCallback (&BBRTorBktapApp::ReadCallback, this));
  m_socket->SetDataSentCallback (MakeCallback (&BBRTorBktapApp::SocketWriteCallback, this));

  Ipv4Mask ipmask = Ipv4Mask ("255.0.0.0");

  // iterate over all neighboring channels
  map<Address,Ptr<BBRUdpChannel> >::iterator it;
  for ( it = channels.begin (); it != channels.end (); it++ )
    {
      Ptr<BBRUdpChannel> ch = it->second;
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
              socket->SetRecvCallback (MakeCallback (&BBRTorBktapApp::ReadCallback, this));
              ch->SetSocket (socket);
            }

          if (ch->GetType () == PROXYEDGE)
            {
              if (!ch->m_socket)
                {
                  ch->m_socket = CreateObject<PseudoClientSocket> ();
                }
              ch->m_socket->SetRecvCallback (MakeCallback (&BBRTorBktapApp::ReadCallback, this));
            }
        }
    }
}

void
BBRTorBktapApp::StopApplication (void)
{
  m_socket->Close ();
  m_socket->SetRecvCallback (MakeNullCallback<void, Ptr<Socket> > ());
  m_socket->SetDataSentCallback (MakeNullCallback<void, Ptr<Socket>, uint32_t > ());
}

void
BBRTorBktapApp::RefillReadCallback (int64_t prev_read_bucket)
{
  vector<Ptr<Socket> > v;
  map<Address,Ptr<BBRUdpChannel> >::iterator it;
  for ( it = channels.begin (); it != channels.end (); it++ )
    {
      Ptr<Socket> socket = it->second->m_socket;
      if (std::find (v.begin (), v.end (),socket) == v.end ())
        {
          Simulator::Schedule (Seconds (0), &BBRTorBktapApp::ReadCallback, this, it->second->m_socket);
          v.push_back (socket);
        }
    }
}

void
BBRTorBktapApp::RefillWriteCallback (int64_t prev_read_bucket)
{
  if (prev_read_bucket <= 0 && writeevent.IsExpired ())
    {
      writeevent = Simulator::ScheduleNow (&BBRTorBktapApp::WriteCallback, this);
    }
}

void
BBRTorBktapApp::ReadCallback (Ptr<Socket> socket)
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
      writeevent = Simulator::Schedule (NanoSeconds (read_bytes * 2),&BBRTorBktapApp::WriteCallback, this);
    }
}

uint32_t
BBRTorBktapApp::ReadFromRelay (Ptr<Socket> socket) {
  uint32_t read_bytes = 0;
  while (socket->GetRxAvailable() > 0) {
      Ptr<Packet> data;
      Address from;
      if (data = socket->RecvFrom (from)) {
          data->RemoveAllPacketTags (); //Fix for ns3 PacketTag Bug
          read_bytes += data->GetSize ();
          Ptr<BBRUdpChannel> ch = channels[from];
          NS_ASSERT (ch);
          while (data->GetSize () > 0) {
              BaseCellHeader header;
              data->PeekHeader (header);
              Ptr<BBRBktapCircuit> circ = circuits[header.circId];
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
                      //ReceivedFwd (circ,direction,h);
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
BBRTorBktapApp::ReceivedRelayCell (Ptr<BBRBktapCircuit> circ, CellDirection direction, Ptr<Packet> cell) {
  Ptr<BBRSeqQueue> queue = circ->GetQueue (direction);
  Ptr<BBRUdpChannel> ch = circ->GetChannel (direction);

  cout << "Node: " << GetNodeName() << " Received Relay Cell " << endl;
  UdpCellHeader header;
  cell->PeekHeader (header);
  bool newseq = queue->Add (cell, header.seq);
  if (newseq) {
    m_readbucket.Decrement(cell->GetSize());
  }
  CellDirection oppdir = circ->GetOppositeDirection (direction);
  //Only send feedback cell from endnode

  if (!(ch->SpeaksCells())) {
    SendFeedbackCell (circ, oppdir, ACK, queue->tailSeq + 1);
  }
}

void
BBRTorBktapApp::ReceivedAck (Ptr<BBRBktapCircuit> circ, CellDirection direction, FdbkCellHeader header) {
  Ptr<BBRSeqQueue> queue = circ->GetQueue (direction);
  Ptr<BBRUdpChannel> ch = circ->GetChannel (direction);

  CellDirection oppdir   = circ->GetOppositeDirection(direction);
  Ptr<BBRUdpChannel> oppch = circ->GetChannel(oppdir);

  if (ch->SpeaksCells() && oppch->SpeaksCells()) {
		SendFeedbackCell (circ, direction, ACK, header.ack);
  } else {
    cout << "Node: " << GetNodeName() << " Received Ack " << endl;
    if (header.ack == queue->headSeq) {
        // DupACK. Do fast retransmit.
        ++queue->dupackcnt;
        if (m_writebucket.GetSize () >= CELL_PAYLOAD_SIZE && queue->dupackcnt > 2) {
            FlushPendingCell (circ,direction,true);
            queue->dupackcnt = 0;
        }
    }
    else if (header.ack > queue->headSeq) {
        //NewAck
        queue->dupackcnt = 0;
        queue->DiscardUpTo (header.ack);
        Time rtt = queue->actRtt.EstimateRtt (header.ack);
        ScheduleRto (circ,direction,true);
	queue->actRtt.EstimateDelRate (header.ack,rtt);
        if (!queue->PackageInflight ()) {
            Ptr<BBRUdpChannel> ch = circ->GetChannel(direction);
            ch->ScheduleFlush(false);
        }
	CongestionAvoidance(queue);    
    }
    else {
        cerr << GetNodeName () << " Ignore Ack" << endl;
    }
  }
}

//UPDATE cwnd for endhost (proxy node or server node)
void
BBRTorBktapApp::WindowUpdate (Ptr<BBRSeqQueue> queue, Time baseRtt, Time currRtt, double maxDelRate) {
  cout << "Node: " << GetNodeName() <<"Inside  window upddate";
  if (queue->Inflight2() > currRtt*maxDelRate)
	--queue->cwnd;
  else
	++queue->cwnd;
  if (queue->cwnd < 1) {
      queue->cwnd = 1;
  }
  double maxexp = m_burst.GetBitRate () / 8 / CELL_PAYLOAD_SIZE * baseRtt.GetSeconds (); 
  queue->cwnd = min (queue->cwnd, (uint32_t) maxexp);
}

void
BBRTorBktapApp::CongestionAvoidance (Ptr<BBRSeqQueue> queue){	
	WindowUpdate(queue, queue->actRtt.baseRtt, queue->actRtt.currentRtt, queue->actRtt.maxDelRate);
}

void
BBRTorBktapApp::ReceivedFwd (Ptr<BBRBktapCircuit> circ, CellDirection direction, FdbkCellHeader header) {
  cout << "Node: " << GetNodeName() << " Received Fwd Cell" << endl;
  //Received flow control feeback (FWD)
  Ptr<BBRSeqQueue> queue = circ->GetQueue (direction);
  Ptr<BBRUdpChannel> ch = circ->GetChannel (direction);

  CellDirection oppdir = circ->GetOppositeDirection (direction);
  Ptr<BBRUdpChannel> oppch = circ->GetChannel (oppdir);

  Time rtt = queue->virtRtt.EstimateRtt (header.fwd);
  ch->rttEstimator.AddSample (rtt);

  if (queue->virtHeadSeq <= header.fwd) {
      queue->virtHeadSeq = header.fwd;
  }

  if (header.fwd > queue->begRttSeq) {
      queue->begRttSeq = queue->nextTxSeq;
      //CongestionAvoidance (queue, header.diff, ch->rttEstimator.baseRtt, circ->GetId (), direction);
			//Only for Edge Tor Nodes
			if (!(oppch->SpeaksCells())) {	
	//				WindowUpdate(queue, ch->rttEstimator.baseRtt, circ->GetId (), direction);
			}
      //queue->ssthresh = min (queue->cwnd,queue->ssthresh);
      //queue->ssthresh = max (queue->ssthresh,queue->cwnd / 2);
  }
  else if (queue->cwnd <= queue->ssthresh) {
      //TODO test different slow start schemes
  }

  Simulator::Schedule (Seconds (0), &BBRTorBktapApp::ReadCallback, this, oppch->m_socket);

  if (writeevent.IsExpired ()) {
      writeevent = Simulator::Schedule (Seconds (0), &BBRTorBktapApp::WriteCallback, this);
  }
}

uint32_t
BBRTorBktapApp::ReadFromEdge (Ptr<Socket> socket)
{
  Ptr<BBRUdpChannel> ch = LookupChannel (socket);
  NS_ASSERT (ch);
  Ptr<BBRBktapCircuit> circ = ch->circuits.front ();
  NS_ASSERT (circ);
  CellDirection direction = circ->GetDirection (ch);
  CellDirection oppdir = circ->GetOppositeDirection (direction);
  Ptr<BBRSeqQueue> queue = circ->GetQueue (oppdir);

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
BBRTorBktapApp::PackageRelayCell (Ptr<BBRBktapCircuit> circ, CellDirection direction, Ptr<Packet> cell)
{
  cout << "Node: " << GetNodeName() << " Relay Cell Packaged " << endl;
  UdpCellHeader header;
  header.circId = circ->GetId ();
  Ptr<BBRSeqQueue> queue = circ->GetQueue (direction);
  NS_ASSERT (queue);
  header.seq = queue->tailSeq + 1;
  cell->AddHeader (header);
  queue->virtRtt.SentSeq2 (header.seq, queue->headSeq);
  queue->actRtt.SentSeq2 (header.seq, queue->headSeq);
  queue->Add (cell, header.seq);
}

void
BBRTorBktapApp::SocketWriteCallback (Ptr<Socket> s, uint32_t i)
{
  if (writeevent.IsExpired ())
    {
      writeevent = Simulator::Schedule (Seconds (0), &BBRTorBktapApp::WriteCallback, this);
    }
}

void
BBRTorBktapApp::WriteCallback ()
{
  uint32_t bytes_written = 0;

  if (m_writebucket.GetSize () >= CELL_PAYLOAD_SIZE)
    {
      Ptr<BBRBktapCircuit> start = circit->second;
      Ptr<BBRBktapCircuit> circ;

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
          writeevent = Simulator::ScheduleNow (&BBRTorBktapApp::WriteCallback, this);
        }
    }
}



uint32_t
BBRTorBktapApp::FlushPendingCell (Ptr<BBRBktapCircuit> circ, CellDirection direction, bool retx) {
  Ptr<BBRSeqQueue> queue = circ->GetQueue (direction);
  cout << "Node: " << GetNodeName() << ", Flush Pending Cell" << endl;

  CellDirection oppdir   = circ->GetOppositeDirection (direction);
  Ptr<BBRUdpChannel> ch  = circ->GetChannel (direction);
  Ptr<BBRUdpChannel> oppch = circ->GetChannel (oppdir);
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
          queue->virtRtt.SentSeq2 (header.seq, queue->headSeq);
          queue->actRtt.SentSeq2 (header.seq, queue->headSeq);
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
          //SendFeedbackCell (circ, oppdir, FWD, queue->highestTxSeq + 1);
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
BBRTorBktapApp::SendFeedbackCell (Ptr<BBRBktapCircuit> circ, CellDirection direction, uint8_t flag, uint32_t ack) {
  cout << "Node: " << GetNodeName() << ", Sending feedback cell " << ", isAck:" << (flag & ACK) <<", isFwd:"<< (flag & FWD) << ", ack:" << ack << endl;
  Ptr<BBRUdpChannel> ch = circ->GetChannel (direction);
  Ptr<BBRSeqQueue> queue = circ->GetQueue (direction);
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
          queue->delFeedbackEvent = Simulator::Schedule (MilliSeconds (1), &BBRTorBktapApp::PushFeedbackCell, this, circ, direction);
      }
  }
}

void
BBRTorBktapApp::PushFeedbackCell (Ptr<BBRBktapCircuit> circ, CellDirection direction) {
  cout << "Node: " << GetNodeName() << ", Pushing Feedback cell " << endl;
  Ptr<BBRUdpChannel> ch = circ->GetChannel (direction);
  Ptr<BBRSeqQueue> queue = circ->GetQueue (direction);

  CellDirection oppdir   = circ->GetOppositeDirection (direction);
  Ptr<BBRSeqQueue> oppqueue = circ->GetQueue (oppdir);
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
      //if (queue->fwdq.size () > 0) {
      //    header.flags |= FWD;
      //    header.fwd = queue->fwdq.front ();
      //    queue->fwdq.pop ();
      //}
      header.diff = 0;

      cell->AddHeader (header);
      cout << "Reaching here"<<endl;
      ch->m_flushQueue.push (cell);
      ch->ScheduleFlush ();
      //cout << "Reaching here"<<endl;
  }
}

void
BBRTorBktapApp::ScheduleRto (Ptr<BBRBktapCircuit> circ, CellDirection direction, bool force)
{
  Ptr<BBRSeqQueue> queue = circ->GetQueue (direction);
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
      queue->retxEvent = Simulator::Schedule (queue->actRtt.Rto (), &BBRTorBktapApp::Rto, this, circ, direction);
    }
}

void
BBRTorBktapApp::Rto (Ptr<BBRBktapCircuit> circ, CellDirection direction)
{
  Ptr<BBRSeqQueue> queue = circ->GetQueue (direction);
  queue->nextTxSeq = queue->headSeq;
  FlushPendingCell (circ,direction);
}

Ptr<BBRBktapCircuit>
BBRTorBktapApp::GetCircuit (uint16_t id)
{
  return circuits[id];
}

Ptr<BBRBktapCircuit>
BBRTorBktapApp::GetNextCircuit ()
{
  ++circit;
  if (circit == circuits.end ())
    {
      circit = circuits.begin ();
    }
  return circit->second;
}

Ptr<BBRUdpChannel>
BBRTorBktapApp::LookupChannel (Ptr<Socket> socket)
{
  map<Address,Ptr<BBRUdpChannel> >::iterator it;
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
BBRTorBktapApp::DoDispose (void)
{
  NS_LOG_FUNCTION (this);

  map<uint16_t,Ptr<BBRBktapCircuit> >::iterator i;
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
