#include <iostream>
#include <fstream>

#include "ns3/tor-module.h"

using namespace ns3;
using namespace std;
NS_LOG_COMPONENT_DEFINE ("TorExample");

void StatsCallback(TorDumbbellHelper*, Time);
void TtfbCallback(int, double, std::string);
void TtlbCallback(int, double, std::string);

int main (int argc, char *argv[]) {
    uint32_t run = 1;
    Time simTime = Time("90s");
    //string flavor = "vanilla";
    //string flavor = "bktap";
    string flavor = "marut";

    CommandLine cmd;
    cmd.AddValue("run", "run number", run);
    cmd.AddValue("time", "simulation time", simTime);
    cmd.AddValue("flavor", "Tor flavor", flavor);
    cmd.Parse(argc, argv);

    SeedManager::SetSeed (42);
    SeedManager::SetRun (run);

    /* set global defaults */
    // GlobalValue::Bind ("ChecksumEnabled", BooleanValue (true));

    /* defaults for ns3's native Tcp implementation */
    // Config::SetDefault ("ns3::TcpSocket::SegmentSize", UintegerValue (1458));
    // Config::SetDefault ("ns3::TcpSocket::TcpNoDelay", BooleanValue (true));
    // Config::SetDefault ("ns3::DropTailQueue::MaxPackets", UintegerValue (100));

    /* TorApp defaults. Note, this also affects onion proxies. */
    // Config::SetDefault ("ns3::TorBaseApp::BandwidthRate", DataRateValue (DataRate ("12Mbps")));
    // Config::SetDefault ("ns3::TorBaseApp::BandwidthBurst", DataRateValue (DataRate ("12Mbps")));
    Config::SetDefault ("ns3::TorApp::WindowStart", IntegerValue (500));
    Config::SetDefault ("ns3::TorApp::WindowIncrement", IntegerValue (50));
    // Config::SetDefault ("ns3::TorBktapApp::Nagle", BooleanValue (true));

    NS_LOG_INFO("setup topology");

    TorDumbbellHelper th;
    if (flavor == "pctcp")
        th.SetTorAppType("ns3::TorPctcpApp");
    else if (flavor == "bktap")
        th.SetTorAppType("ns3::TorBktapApp");
    else if (flavor == "e2e")
        th.SetTorAppType("ns3::TorE2eApp");
    else if (flavor == "marut")
        th.SetTorAppType("ns3::MarutTorBktapApp");
    else if (flavor == "n23")
        th.SetTorAppType("ns3::TorN23App");
    else if (flavor == "fair")
        th.SetTorAppType("ns3::TorFairApp");

    th.DisableProxies(true); // make circuits shorter (entry = proxy), thus the simulation faster
    th.EnableNscStack(true,"cubic"); // enable linux protocol stack and set tcp flavor

    Ptr<UniformRandomVariable> m_startTime = CreateObject<UniformRandomVariable> ();
    m_startTime->SetAttribute ("Min", DoubleValue (0.1));
    m_startTime->SetAttribute ("Max", DoubleValue (30.0));
    th.SetStartTimeStream (m_startTime);

    th.ParseFile ("circuits-1c3r-20180507.dat",1,0.); // parse scenario from file
    //th.ParseFile ("circuits-10000c100r-20150804.dat",100,0.1);
    //th.ParseFile ("circuits-2c4r-20150804.dat",2,0.);
    //th.ParseFile ("circuits-20c22r-20150804.dat",20,0.5);
    th.PrintCircuits();
    th.BuildTopology(); // finally build topology, setup relays and seed circuits

    th.RegisterTtfbCallback (TtfbCallback);
    th.RegisterTtlbCallback (TtlbCallback);

    ApplicationContainer relays = th.GetTorAppsContainer();

    relays.Start (Seconds (0.0));
    relays.Stop (simTime);
    Simulator::Stop (simTime);

    Simulator::Schedule(Seconds(0), &StatsCallback, &th, simTime);

    NS_LOG_INFO("start simulation");
    Simulator::Run ();

    NS_LOG_INFO("stop simulation");
    Simulator::Destroy ();

    return 0;
}

/* example of (cumulative) i/o stats */
void StatsCallback(TorDumbbellHelper* th, Time simTime) {
    cout << Simulator::Now().GetSeconds() << " ";
    vector<int>::iterator id;
    for (id = th->circuitIds.begin(); id != th->circuitIds.end(); ++id) {
      Ptr<TorBaseApp> proxyApp = th->GetProxyApp(*id);
      Ptr<TorBaseApp> middleApp = th->GetMiddleApp(*id);
      Ptr<TorBaseApp> exitApp = th->GetExitApp(*id);
      Ptr<BaseCircuit> proxyCirc = proxyApp->baseCircuits[*id];
      Ptr<BaseCircuit> middleCirc = middleApp->baseCircuits[*id];
      Ptr<BaseCircuit> exitCirc = exitApp->baseCircuits[*id];
      cout <<"INBOUND: " << endl;
      cout <<"Exit Node: "<< exitCirc->GetBytesRead(INBOUND) << " " << exitCirc->GetBytesWritten(INBOUND) << endl;
      cout <<"Middle Node: "<< middleCirc->GetBytesRead(INBOUND) << " " << middleCirc->GetBytesWritten(INBOUND) << endl;
      cout <<"Proxy Node: "<< proxyCirc->GetBytesRead(INBOUND) << " " << proxyCirc->GetBytesWritten(INBOUND) << endl;

      cout <<"OUTBOUND: " << endl;
      cout <<"Proxy Node: "<< proxyCirc->GetBytesRead(OUTBOUND) << " " << proxyCirc->GetBytesWritten(OUTBOUND) << endl;
      cout <<"Middle Node: "<< middleCirc->GetBytesRead(OUTBOUND) << " " << middleCirc->GetBytesWritten(OUTBOUND) << endl;
      cout <<"Exit Node: "<< exitCirc->GetBytesRead(OUTBOUND) << " " << exitCirc->GetBytesWritten(OUTBOUND) << endl;
      //proxyCirc->ResetStats(); exitCirc->ResetStats();
    }
    cout << endl;

    Time resolution = MilliSeconds(10);
    if (Simulator::Now()+resolution < simTime)
        Simulator::Schedule(resolution, &StatsCallback, th, simTime);
}

void TtfbCallback(int id, double time, std::string desc) {
    cout << Simulator::Now().GetSeconds() << " " << desc << " ttfb from id " << id << ": " << time << endl;
}

void TtlbCallback(int id, double time, std::string desc) {
    cout << Simulator::Now().GetSeconds() << " " << desc << " ttlb from id " << id << ": " << time << endl;
}
