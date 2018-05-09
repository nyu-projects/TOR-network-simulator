
#include "ns3/log.h"
#include "tor-e2e.h"

using namespace std;

namespace ns3 {

NS_LOG_COMPONENT_DEFINE ("TorE2eApp");
NS_OBJECT_ENSURE_REGISTERED (TorE2eApp);



E2eUdpChannel::E2eUdpChannel ()
{
  NS_LOG_FUNCTION (this);
  this->m_socket = 0;
}

E2eUdpChannel::E2eUdpChannel (Address remote, int conntype)
{
  m_remote = remote;
  m_conntype = conntype;
  this->m_socket = 0;
}

void
E2eUdpChannel::SetSocket (Ptr<Socket> socket)
{
  this->m_socket = socket;
}

uint8_t
E2eUdpChannel::GetType ()
{
  return m_conntype;
}

bool
E2eUdpChannel::SpeaksCells ()
{
  return m_conntype == RELAYEDGE;
}

void E2eUdpChannel::Flush () {
  while (m_flushQueue.size () > 0) {
      if (SpeaksCells () && m_devQlimit <= m_devQ->GetNPackets ()) {
          m_flushEvent = Simulator::Schedule (MilliSeconds (1), &E2eUdpChannel::Flush, this);
          return;
      }
      Ptr<Packet> data = Create<Packet> ();
      while (m_flushQueue.size () > 0 && data->GetSize () + m_flushQueue.front ()->GetSize () <= 1400) {
          data->AddAtEnd (m_flushQueue.front ());
          m_flushQueue.pop ();
      }
      m_socket->SendTo (data,0,m_remote);
  }
}


void
E2eUdpChannel::ScheduleFlush (bool delay)
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
          m_flushEvent = Simulator::Schedule (MilliSeconds (1), &E2eUdpChannel::Flush, this);
        }
      else
        {
          m_flushEvent = Simulator::Schedule (rttEstimator.baseRtt, &E2eUdpChannel::Flush, this);
        }
    }
}



E2eCircuit::E2eCircuit (uint16_t id) : BaseCircuit (id)
{
  inboundQueue = Create<E2eSeqQueue> ();
  outboundQueue = Create<E2eSeqQueue> ();
}

CellDirection
E2eCircuit::GetDirection (Ptr<E2eUdpChannel> ch)
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

Ptr<E2eUdpChannel>
E2eCircuit::GetChannel (CellDirection direction)
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

Ptr<E2eSeqQueue>
E2eCircuit::GetQueue (CellDirection direction)
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

TorE2eApp::TorE2eApp ()
{
  NS_LOG_FUNCTION (this);
}

TorE2eApp::~TorE2eApp ()
{
  NS_LOG_FUNCTION (this);
}

TypeId
TorE2eApp::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::TorE2eApp")
    .SetParent<TorBaseApp> ()
    .AddConstructor<TorE2eApp> ()
    .AddAttribute ("Nagle", "Enable the Nagle Algorithm for BackTap.",
                   BooleanValue (false),
                   MakeBooleanAccessor (&TorE2eApp::m_nagle),
                   MakeBooleanChecker ());
  return tid;
}

void
TorE2eApp::AddCircuit (int id, Ipv4Address n_ip, int n_conntype, Ipv4Address p_ip, int p_conntype,
                         Ptr<PseudoClientSocket> clientSocket) {
  TorBaseApp::AddCircuit (id, n_ip, n_conntype, p_ip, p_conntype);

  // ensure unique circ_id
  NS_ASSERT (circuits[id] == 0);

  Ptr<E2eCircuit> circ = Create<E2eCircuit> (id);
  circuits[id] = circ;
  baseCircuits[id] = circ;

  circ->inbound = AddChannel (InetSocketAddress (p_ip,9001),p_conntype);
  circ->inbound->circuits.push_back (circ);
  circ->inbound->SetSocket (clientSocket);

  circ->outbound = AddChannel (InetSocketAddress (n_ip,9001),n_conntype);
  circ->outbound->circuits.push_back (circ);
}

Ptr<E2eUdpChannel>
TorE2eApp::AddChannel (Address remote, int conntype)
{
  // find existing or create new channel-object
  Ptr<E2eUdpChannel> ch = channels[remote];
  if (!ch)
    {
      ch = Create<E2eUdpChannel> (remote, conntype);
      channels[remote] = ch;
    }
  return ch;
}

void TorE2eApp::StartApplication (void) {
  //tor proposal #183: smooth bursts & get queued data out earlier
  m_refilltime = MilliSeconds (10);
  TorBaseApp::StartApplication ();
  m_readbucket.SetRefilledCallback (MakeCallback (&TorE2eApp::RefillReadCallback, this));
  m_writebucket.SetRefilledCallback (MakeCallback (&TorE2eApp::RefillWriteCallback, this));

  circit = circuits.begin ();

  m_devQ = GetNode ()->GetDevice (0)->GetObject<PointToPointNetDevice> ()->GetQueue ();
  UintegerValue limit;
  m_devQ->GetAttribute ("MaxPackets", limit);
  m_devQlimit = limit.Get ();

  if (m_socket == 0) {
      m_socket = Socket::CreateSocket (GetNode (), UdpSocketFactory::GetTypeId ());
      m_socket->Bind (m_local);
  }

  m_socket->SetRecvCallback (MakeCallback (&TorE2eApp::ReadCallback, this));
  m_socket->SetDataSentCallback (MakeCallback (&TorE2eApp::SocketWriteCallback, this));

  Ipv4Mask ipmask = Ipv4Mask ("255.0.0.0");

  // iterate over all neighboring channels
  map<Address,Ptr<E2eUdpChannel> >::iterator it;
  for ( it = channels.begin (); it != channels.end (); it++ ) {
      Ptr<E2eUdpChannel> ch = it->second;
      NS_ASSERT (ch);

      if (ch->SpeaksCells ()) {
          ch->SetSocket (m_socket);
          ch->m_devQ = m_devQ;
          ch->m_devQlimit = m_devQlimit;
      }

      // PseudoSockets only
      if (ipmask.IsMatch (InetSocketAddress::ConvertFrom (ch->m_remote).GetIpv4 (), Ipv4Address ("127.0.0.1")) ) {
          if (ch->GetType () == SERVEREDGE) {
              Ptr<Socket> socket = CreateObject<PseudoServerSocket> ();
              socket->SetRecvCallback (MakeCallback (&TorE2eApp::ReadCallback, this));
              ch->SetSocket (socket);
          }

          if (ch->GetType () == PROXYEDGE) {
              if (!ch->m_socket) {
                  ch->m_socket = CreateObject<PseudoClientSocket> ();
              }
              ch->m_socket->SetRecvCallback (MakeCallback (&TorE2eApp::ReadCallback, this));
          }
      }
  }
}

void
TorE2eApp::StopApplication (void)
{
  m_socket->Close ();
  m_socket->SetRecvCallback (MakeNullCallback<void, Ptr<Socket> > ());
  m_socket->SetDataSentCallback (MakeNullCallback<void, Ptr<Socket>, uint32_t > ());
}

void
TorE2eApp::RefillReadCallback (int64_t prev_read_bucket)
{
  vector<Ptr<Socket> > v;
  map<Address,Ptr<E2eUdpChannel> >::iterator it;
  for ( it = channels.begin (); it != channels.end (); it++ )
    {
      Ptr<Socket> socket = it->second->m_socket;
      if (std::find (v.begin (), v.end (),socket) == v.end ())
        {
          Simulator::Schedule (Seconds (0), &TorE2eApp::ReadCallback, this, it->second->m_socket);
          v.push_back (socket);
        }
    }
}

void
TorE2eApp::RefillWriteCallback (int64_t prev_read_bucket)
{
  if (prev_read_bucket <= 0 && writeevent.IsExpired ())
    {
      writeevent = Simulator::ScheduleNow (&TorE2eApp::WriteCallback, this);
    }
}

void
TorE2eApp::ReadCallback (Ptr<Socket> socket)
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
      writeevent = Simulator::Schedule (NanoSeconds (read_bytes * 2),&TorE2eApp::WriteCallback, this);
    }
}

uint32_t
TorE2eApp::ReadFromRelay (Ptr<Socket> socket) {
  uint32_t read_bytes = 0;
  while (socket->GetRxAvailable () > 0) {
      Ptr<Packet> data;
      Address from;
      if (data = socket->RecvFrom (from)) {
          data->RemoveAllPacketTags (); //Fix for ns3 PacketTag Bug
          read_bytes += data->GetSize ();
          Ptr<E2eUdpChannel> ch = channels[from];
          NS_ASSERT (ch);
          while (data->GetSize () > 0) {
              E2eBaseCellHeader header;
              data->PeekHeader (header);
              Ptr<E2eCircuit> circ = circuits[header.circId];
              NS_ASSERT (circ);
              CellDirection direction = circ->GetDirection (ch);
              CellDirection oppdir = circ->GetOppositeDirection (direction);
              if (header.cellType == FDBK) {
                //cout << GetNodeName () << " Received a FDBK cell " << endl;
                  E2eFdbkCellHeader h;
                  data->RemoveHeader (h);
                  //cout << GetNodeName () << " h.flags&ACK = " <<(h.flags & ACK)<< "h.flags&FWD "<< (h.flags & FWD) << endl;
                  circ->IncrementStats (oppdir,h.GetSerializedSize (),0);
                  if (h.flags & ACK) {
                    //cout << GetNodeName () << " Calling ReceivedAck, circ->GetBytesRead(oppdir)= "<< circ->GetBytesRead(oppdir) << endl;  
                    ReceivedAck (circ,direction,h);
                  }
                  if (h.flags & FWD) {
                      //cout << GetNodeName () << " Calling ReceivedFwd, circ->GetBytesRead(oppdir) = "<< circ->GetBytesRead(oppdir) << endl;
                      ReceivedFwd (circ,direction,h);
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
TorE2eApp::ReceivedRelayCell (Ptr<E2eCircuit> circ, CellDirection direction, Ptr<Packet> cell)
{
  Ptr<E2eSeqQueue> queue = circ->GetQueue (direction);
  E2eUdpCellHeader header;
  cell->PeekHeader (header);
  if (queue->IsCongested()){
    cell->RemoveHeader(header);
    header.ECN = 1;
    cell->AddHeader(header);
  }
  bool newseq = queue->Add (cell, header.seq);
  if (newseq) {
    m_readbucket.Decrement(cell->GetSize());
  }
  CellDirection oppdir = circ->GetOppositeDirection (direction);
  SendFeedbackCell (circ, oppdir, ACK, queue->tailSeq + 1, false);
}


void
TorE2eApp::ReceivedAck (Ptr<E2eCircuit> circ, CellDirection direction, E2eFdbkCellHeader header) {
  Ptr<E2eSeqQueue> queue = circ->GetQueue (direction);
  //cout << GetNodeName () << " Received Ack Cell, header.ack = " << header.ack << " , queue-> headSeq = " << queue->headSeq << endl;
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
      if (!queue->PackageInflight ()) {
          Ptr<E2eUdpChannel> ch = circ->GetChannel(direction);
          ch->ScheduleFlush(false);
      }
  }
  else {
      cerr << GetNodeName () << " Ignore Ack" << endl;
  }
}


void
TorE2eApp::CongestionAvoidance (Ptr<E2eSeqQueue> queue, uint8_t CE) {
  //Do the Vegas-thing every RTT
  if (queue->virtRtt.cntRtt > 2) {
      Time rtt = queue->virtRtt.currentRtt;
      //double diff = queue->cwnd * (rtt.GetSeconds () - baseRtt.GetSeconds ()) / baseRtt.GetSeconds ();
      // uint32_t target = queue->cwnd * baseRtt.GetMilliSeconds() / rtt.GetMilliSeconds();

      //if (diff < VEGASALPHA) {
       //   ++queue->cwnd;
      //}

      //if (diff > VEGASBETA) {
        //  --queue->cwnd;
      //}

      //if (queue->cwnd < 1) {
        //  queue->cwnd = 1;
      //}

      //double alpha = queue->;
      //DataRateValue d;
      //this -> GetAttribute("BandwidthRate",d); 
      //cout<< "d="<<d<<endl;
	    if (CE > 0){
         --queue->cwnd;
      } else {
         ++queue->cwnd;
      }

      //double maxexp = m_burst.GetBitRate () / 8 / CELL_PAYLOAD_SIZE * baseRtt.GetSeconds ();
      //queue->cwnd = min (queue->cwnd, (uint32_t) maxexp);

      queue->virtRtt.ResetCurrRtt ();
  }
  else {
      // Vegas falls back to Reno CA, i.e. increase per RTT
      // However, This messes up with our backlog and makes the approach too aggressive.
  }
}

void
TorE2eApp::ReceivedFwd (Ptr<E2eCircuit> circ, CellDirection direction, E2eFdbkCellHeader header)
{
  //cout << GetNodeName () << " Received Feedback Cell" << endl;
  //Received flow control feedback (FWD)
  Ptr<E2eSeqQueue> queue = circ->GetQueue (direction);
  Ptr<E2eUdpChannel> ch = circ->GetChannel (direction);

  CellDirection oppdir = circ->GetOppositeDirection (direction);
  Ptr<E2eUdpChannel> oppch = circ->GetChannel (oppdir);
	//Changes
  if (oppch->SpeaksCells ()) {
    //cout << GetNodeName () << " Relaying Feedback Cell" << endl;
  	SendFeedbackCell (circ, oppdir, FWD, header.fwd, header.CE==1);
  }
  else {
    //cout << GetNodeName () << " Updating queue congestion window" << endl;
    //cout << GetNodeName () << " cwnd:" << queue->cwnd << endl;
	  //cout << GetNodeName () << " nextTxSeq:" << queue->nextTxSeq << endl;
		//cout << GetNodeName () << " highestTxSeq:" << queue->highestTxSeq << endl;
		//cout << GetNodeName () << " tailSeq:" << queue->tailSeq << endl;
		//cout << GetNodeName () << " headSeq:" << queue->headSeq << endl;
		//cout << GetNodeName () << " virtHeadSeq:" << queue->virtHeadSeq << endl;
		//cout << GetNodeName () << " begRttSeq:" << queue->begRttSeq << endl;
		//cout << GetNodeName () << " ssthresh:" << queue->ssthresh << endl;
		//cout << GetNodeName () << " dupackcnt:" << queue->dupackcnt << endl;

  	Time rtt = queue->virtRtt.EstimateRtt (header.fwd);
  	ch->rttEstimator.AddSample (rtt);

  	if (queue->virtHeadSeq <= header.fwd){
      queue->virtHeadSeq = header.fwd;
    }

  	if (header.fwd > queue->begRttSeq){
      queue->begRttSeq = queue->nextTxSeq;
      CongestionAvoidance (queue,header.CE);
      queue->ssthresh = min (queue->cwnd,queue->ssthresh);
      queue->ssthresh = max (queue->ssthresh,queue->cwnd / 2);
    }
  	else if (queue->cwnd <= queue->ssthresh){
      //TODO test different slow start schemes
    }
		//cout << GetNodeName () << " After Update" << endl;
    //cout << GetNodeName () << " cwnd:" << queue->cwnd << endl;
    //cout << GetNodeName () << " nextTxSeq:" << queue->nextTxSeq << endl;
    //cout << GetNodeName () << " highestTxSeq:" << queue->highestTxSeq << endl;
    //cout << GetNodeName () << " tailSeq:" << queue->tailSeq << endl;
    //cout << GetNodeName () << " headSeq:" << queue->headSeq << endl;
    //cout << GetNodeName () << " virtHeadSeq:" << queue->virtHeadSeq << endl;
    //cout << GetNodeName () << " begRttSeq:" << queue->begRttSeq << endl;
    //cout << GetNodeName () << " ssthresh:" << queue->ssthresh << endl;
    //cout << GetNodeName () << " dupackcnt:" << queue->dupackcnt << endl;
	  //cout << "%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%" << endl;
	}

//	ch = circ->GetChannel (oppdir);
  Simulator::Schedule (Seconds (0), &TorE2eApp::ReadCallback, this, oppch->m_socket);

  if (writeevent.IsExpired ()) {
    writeevent = Simulator::Schedule (Seconds (0), &TorE2eApp::WriteCallback, this);
  }
}

uint32_t
TorE2eApp::ReadFromEdge (Ptr<Socket> socket) {
  Ptr<E2eUdpChannel> ch = LookupChannel (socket);
  NS_ASSERT (ch);
  Ptr<E2eCircuit> circ = ch->circuits.front ();
  NS_ASSERT (circ);
  CellDirection direction = circ->GetDirection (ch);
  CellDirection oppdir = circ->GetOppositeDirection (direction);
  Ptr<E2eSeqQueue> queue = circ->GetQueue (oppdir);

  uint32_t max_read = (queue->cwnd - queue->VirtSize () <= 0) ? 0 : queue->cwnd - queue->VirtSize ();
  max_read *= CELL_PAYLOAD_SIZE;

  uint32_t read_bytes = 0;

  while (max_read - read_bytes >= CELL_PAYLOAD_SIZE && socket->GetRxAvailable () >= CELL_PAYLOAD_SIZE) {
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
TorE2eApp::PackageRelayCell (Ptr<E2eCircuit> circ, CellDirection direction, Ptr<Packet> cell)
{
  E2eUdpCellHeader header;
  header.circId = circ->GetId ();
  Ptr<E2eSeqQueue> queue = circ->GetQueue (direction);
  NS_ASSERT (queue);
  header.seq = queue->tailSeq + 1;
  cell->AddHeader (header);
  queue->virtRtt.SentSeq (header.seq);
  queue->actRtt.SentSeq (header.seq);
  queue->Add (cell, header.seq);
}

void
TorE2eApp::SocketWriteCallback (Ptr<Socket> s, uint32_t i)
{
  if (writeevent.IsExpired ())
    {
      writeevent = Simulator::Schedule (Seconds (0), &TorE2eApp::WriteCallback, this);
    }
}

void
TorE2eApp::WriteCallback ()
{
  uint32_t bytes_written = 0;

  if (m_writebucket.GetSize () >= CELL_PAYLOAD_SIZE)
    {
      Ptr<E2eCircuit> start = circit->second;
      Ptr<E2eCircuit> circ;

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
          writeevent = Simulator::ScheduleNow (&TorE2eApp::WriteCallback, this);
        }
    }
}


uint32_t TorE2eApp::FlushPendingCell (Ptr<E2eCircuit> circ, CellDirection direction, bool retx) {
  Ptr<E2eSeqQueue> queue = circ->GetQueue (direction);
  CellDirection oppdir = circ->GetOppositeDirection (direction);
  Ptr<E2eUdpChannel> ch = circ->GetChannel (direction);
  Ptr<E2eUdpChannel> oppch = circ->GetChannel (oppdir);
  Ptr<Packet> cell;

  //Only check window if not a middle node
  if (!(ch->SpeaksCells() && oppch->SpeaksCells()) && queue->Window () <= 0 && !retx) {
    //cout << GetNodeName () << " Check window, queue->Window () =" << queue ->Window () << " nextTxSeq = "<< queue->nextTxSeq <<" virtHeadSeq = "<< queue->virtHeadSeq << endl;  
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
      E2eUdpCellHeader header;
      cell->PeekHeader (header);
      if (!ch->SpeaksCells ()) {
          cell->RemoveHeader (header);
      }

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
//********Changes********
//Only send Feedback cells if you are an edge node and sending inside tor
//What feedback cell seq number should we send?
          if (!ch->SpeaksCells ()) {
            SendFeedbackCell (circ, oppdir, FWD, queue->highestTxSeq + 1, header.ECN==1);
          }
      }
      if (!queue->WasRetransmit()) {
        m_writebucket.Decrement(bytes_written);
      }
      return bytes_written;
  }
  return 0;
}

void
TorE2eApp::SendFeedbackCell (Ptr<E2eCircuit> circ, CellDirection direction, uint8_t flag, uint32_t ack, bool isECN)
{
  Ptr<E2eUdpChannel> ch = circ->GetChannel (direction);
  Ptr<E2eSeqQueue> queue = circ->GetQueue (direction);
  NS_ASSERT (ch);
  if (ch->SpeaksCells ())
    {
      if (flag & ACK)
        {
          queue->ackq.push (ack);
        }
      if (flag & FWD)
        {
          //cout << GetNodeName () << " Sending Feedback Cell" << endl;
          queue->fwdq.push (ack);
        }
      if (queue->ackq.size () > 0 && queue->fwdq.size () > 0)
        {
          queue->delFeedbackEvent.Cancel ();
          PushFeedbackCell (circ, direction,isECN);
        }
      else
        {
          queue->delFeedbackEvent = Simulator::Schedule (MilliSeconds (1), &TorE2eApp::PushFeedbackCell, this, circ, direction, isECN);
        }
    }
}

void
TorE2eApp::PushFeedbackCell (Ptr<E2eCircuit> circ, CellDirection direction, bool isECN) {
  Ptr<E2eUdpChannel> ch = circ->GetChannel (direction);
  Ptr<E2eSeqQueue> queue = circ->GetQueue (direction);
  NS_ASSERT (ch);

  while (queue->ackq.size () > 0 || queue->fwdq.size () > 0) {
      Ptr<Packet> cell = Create<Packet> ();
      E2eFdbkCellHeader header;
      header.circId = circ->GetId ();
      if (queue->ackq.size () > 0)
        {
          header.flags |= ACK;
          while (queue->ackq.size () > 0 && header.ack < queue->ackq.front ())
            {
              header.ack = queue->ackq.front ();
              queue->ackq.pop ();
            }
        }
      if (queue->fwdq.size () > 0) {
          header.flags |= FWD;
          header.fwd = queue->fwdq.front ();
    	    if (isECN) {
	          header.CE = 1;
    	    }
          queue->fwdq.pop ();
      }
      cell->AddHeader (header);
      ch->m_flushQueue.push (cell);
      ch->ScheduleFlush ();
  }
}

void
TorE2eApp::ScheduleRto (Ptr<E2eCircuit> circ, CellDirection direction, bool force) {
  Ptr<E2eSeqQueue> queue = circ->GetQueue (direction);
  if (force) {
      queue->retxEvent.Cancel ();
  }
  if (queue->Inflight () <= 0) {
      return;
  }
  if (queue->retxEvent.IsExpired ()) {
      queue->retxEvent = Simulator::Schedule (queue->actRtt.Rto (), &TorE2eApp::Rto, this, circ, direction);
  }
}

void
TorE2eApp::Rto (Ptr<E2eCircuit> circ, CellDirection direction) {
  Ptr<E2eSeqQueue> queue = circ->GetQueue (direction);
  queue->nextTxSeq = queue->headSeq;
  FlushPendingCell (circ,direction);
}

Ptr<E2eCircuit>
TorE2eApp::GetCircuit (uint16_t id)
{
  return circuits[id];
}

Ptr<E2eCircuit>
TorE2eApp::GetNextCircuit ()
{
  ++circit;
  if (circit == circuits.end ())
    {
      circit = circuits.begin ();
    }
  return circit->second;
}

Ptr<E2eUdpChannel>
TorE2eApp::LookupChannel (Ptr<Socket> socket)
{
  map<Address,Ptr<E2eUdpChannel> >::iterator it;
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
TorE2eApp::DoDispose (void)
{
  NS_LOG_FUNCTION (this);

  map<uint16_t,Ptr<E2eCircuit> >::iterator i;
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
