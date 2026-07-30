/* -*-  Mode: C++; c-file-style: "gnu"; indent-tabs-mode:nil; -*- */
/* *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation;
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * Authors: Andrea Lacava <thecave003@gmail.com>
 *          Michele Polese <michele.polese@gmail.com>
 */

#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/mobility-module.h"
#include "ns3/applications-module.h"
#include "ns3/point-to-point-helper.h"
#include <ns3/lte-ue-net-device.h>
#include "ns3/mmwave-helper.h"
#include "ns3/epc-helper.h"
#include "ns3/mmwave-point-to-point-epc-helper.h"
#include "ns3/lte-helper.h"
#include "ns3/buildings-module.h"

#include <filesystem>



#include <cmath>
#include <fstream>
#include <map>
#include <string>
#include <vector>




using namespace ns3;
using namespace mmwave;

/**
 * Scenario Six
 *
 */
struct SignalMetrics
{
  double rsrpDbm = NAN;
  double sinrDb = NAN;
  double rsrqDb = NAN;
  uint64_t imsi = 0;
  uint16_t rnti = 0;
  uint16_t cellId = 0;
  uint8_t componentCarrierId = 0;
  bool servingCell = false;
  bool hasRsrp = false;
  bool hasSinr = false;
  bool hasRsrq = false;
};


std::map<std::string, SignalMetrics> g_signalMetricsMap;

std::ofstream g_csvFile;
std::ofstream g_positionCsvFile;
std::ofstream g_appMetricsCsvFile;

NS_LOG_COMPONENT_DEFINE ("ScenarioSix");



static double
ToDb (double linear)
{
  return linear > 0.0 ? 10.0 * std::log10 (linear) : NAN;
}

static double
WattsToDbm (double watts)
{
  return watts > 0.0 ? 10.0 * std::log10 (watts) + 30.0 : NAN;
}

static double
AverageSpectrumValue (const SpectrumValue &values)
{
  double sum = 0.0;
  uint32_t count = 0;
  for (auto it = values.ConstValuesBegin (); it != values.ConstValuesEnd (); ++it)
    {
      sum += *it;
      ++count;
    }
  return count > 0 ? sum / count : NAN;
}

static std::string
BuildSignalKey (const std::string &source, uint64_t imsi, uint16_t rnti, uint16_t cellId,
                uint8_t componentCarrierId)
{
  return source + "|" + std::to_string (imsi) + "|" + std::to_string (rnti) + "|" +
         std::to_string (cellId) + "|" + std::to_string (componentCarrierId);
}

static void
CreateScenarioBuilding (std::vector<Box> &buildingBoxes,
                        double xCenter,
                        double yCenter,
                        double xSize,
                        double ySize,
                        double height)
{
  Box box (xCenter - xSize / 2.0,
           xCenter + xSize / 2.0,
           yCenter - ySize / 2.0,
           yCenter + ySize / 2.0,
           0.0,
           height);
  Ptr<Building> building = CreateObject<Building> ();
  building->SetBoundaries (box);
  buildingBoxes.push_back (box);
}

static void
CreateWindowedBuilding (std::vector<Box> &buildingBoxes,
                        double xCenter,
                        double yCenter,
                        double xSize,
                        double ySize,
                        double height)
{
  const double wall = 2.0;
  const double opening = 4.0;
  const double horizontalLength = (xSize - opening) / 2.0;
  const double horizontalOffset = (opening + horizontalLength) / 2.0;
  const double verticalAvailable = ySize - 2.0 * wall;
  const double verticalLength = (verticalAvailable - opening) / 2.0;
  const double verticalOffset = (opening + verticalLength) / 2.0;

  for (double ySide : {-1.0, 1.0})
    {
      for (double xSide : {-1.0, 1.0})
        {
          CreateScenarioBuilding (buildingBoxes,
                                  xCenter + xSide * horizontalOffset,
                                  yCenter + ySide * (ySize - wall) / 2.0,
                                  horizontalLength, wall, height - 0.5);
        }
    }
  for (double xSide : {-1.0, 1.0})
    {
      for (double ySide : {-1.0, 1.0})
        {
          CreateScenarioBuilding (buildingBoxes,
                                  xCenter + xSide * (xSize - wall) / 2.0,
                                  yCenter + ySide * verticalOffset,
                                  wall, verticalLength, height - 0.5);
        }
    }

  // Thin roof slab: the facade openings remain unobstructed below the roof.
  Box roof (xCenter - xSize / 2.0, xCenter + xSize / 2.0,
            yCenter - ySize / 2.0, yCenter + ySize / 2.0,
            height - 0.5, height);
  Ptr<Building> roofBuilding = CreateObject<Building> ();
  roofBuilding->SetBoundaries (roof);
  buildingBoxes.push_back (roof);
}

void
LteRsrpSinrCallback (std::string context, uint16_t cellId, uint16_t rnti, double rsrp,
                     double sinr, uint8_t componentCarrierId)
{
  (void) context;
  SignalMetrics &metrics =
      g_signalMetricsMap[BuildSignalKey ("LTE-PHY", 0, rnti, cellId, componentCarrierId)];
  metrics.rnti = rnti;
  metrics.cellId = cellId;
  metrics.componentCarrierId = componentCarrierId;
  metrics.rsrpDbm = WattsToDbm (rsrp);
  metrics.sinrDb = ToDb (sinr);
  metrics.hasRsrp = true;
  metrics.hasSinr = true;
  metrics.servingCell = true;
}

void
LteRsrpRsrqCallback (std::string context, uint16_t rnti, uint16_t cellId, double rsrp,
                     double rsrq, bool servingCell, uint8_t componentCarrierId)
{
  (void) context;
  SignalMetrics &metrics =
      g_signalMetricsMap[BuildSignalKey ("LTE-MEAS", 0, rnti, cellId, componentCarrierId)];
  metrics.rnti = rnti;
  metrics.cellId = cellId;
  metrics.componentCarrierId = componentCarrierId;
  metrics.rsrpDbm = rsrp;
  metrics.rsrqDb = rsrq;
  metrics.hasRsrp = true;
  metrics.hasRsrq = true;
  metrics.servingCell = servingCell;
}

void
MmWaveSinrCallback (std::string context, uint64_t imsi, uint16_t cellId, long double sinr)
{
  (void) context;
  SignalMetrics &metrics =
      g_signalMetricsMap[BuildSignalKey ("MMWAVE-RRC", imsi, 0, cellId, 0)];
  metrics.imsi = imsi;
  metrics.cellId = cellId;
  metrics.sinrDb = ToDb (static_cast<double> (sinr));
  metrics.hasSinr = true;
  metrics.servingCell = true;
}

static void
UpdateMmWavePhyMetrics (uint64_t imsi, uint16_t cellId, SpectrumValue &sinr, SpectrumValue &power)
{
  SignalMetrics &metrics =
      g_signalMetricsMap[BuildSignalKey ("MMWAVE-PHY", imsi, 0, cellId, 0)];
  metrics.imsi = imsi;
  metrics.cellId = cellId;
  metrics.rsrpDbm = WattsToDbm (AverageSpectrumValue (power));
  metrics.sinrDb = ToDb (AverageSpectrumValue (sinr));
  metrics.hasRsrp = true;
  metrics.hasSinr = true;
  metrics.servingCell = true;
}

void
MmWavePhyRsrpSinrCallback (std::string context, uint64_t imsi, uint16_t cellId,
                           SpectrumValue &sinr, SpectrumValue &power)
{
  (void) context;
  UpdateMmWavePhyMetrics (imsi, cellId, sinr, power);
}

void
MmWavePhyRsrpSinrCallbackWithoutContext (uint64_t imsi, uint16_t cellId,
                                         SpectrumValue &sinr, SpectrumValue &power)
{
  UpdateMmWavePhyMetrics (imsi, cellId, sinr, power);
}


void
RecordSignalMetricsSnapshot (Time step)
{
  const double now = Simulator::Now ().GetSeconds ();
  for (auto const& entry : g_signalMetricsMap)
    {
      const SignalMetrics &metrics = entry.second;
      g_csvFile << now << ","
                << entry.first.substr (0, entry.first.find ('|')) << ","
                << metrics.imsi << ","
                << metrics.rnti << ","
                << metrics.cellId << ","
                << static_cast<uint32_t> (metrics.componentCarrierId) << ",";
      if (metrics.hasRsrp && std::isfinite (metrics.rsrpDbm))
        {
          g_csvFile << metrics.rsrpDbm;
        }
      g_csvFile << ",";
      if (metrics.hasSinr && std::isfinite (metrics.sinrDb))
        {
          g_csvFile << metrics.sinrDb;
        }
      g_csvFile << ",";
      if (metrics.hasRsrq && std::isfinite (metrics.rsrqDb))
        {
          g_csvFile << metrics.rsrqDb;
        }
      g_csvFile << "," << (metrics.servingCell ? 1 : 0) << "\n";
    }

  Simulator::Schedule (step, &RecordSignalMetricsSnapshot, step);
}

void
RecordUePositionsSnapshot (NodeContainer ueNodes, NetDeviceContainer mcUeDevs, Time step)
{
  const double now = Simulator::Now ().GetSeconds ();
  for (uint32_t i = 0; i < ueNodes.GetN (); ++i)
    {
      Ptr<Node> node = ueNodes.Get (i);
      Ptr<MobilityModel> mobility = node->GetObject<MobilityModel> ();
      if (!mobility)
        {
          continue;
        }

      uint64_t imsi = 0;
      if (i < mcUeDevs.GetN ())
        {
          Ptr<McUeNetDevice> ueDevice = mcUeDevs.Get (i)->GetObject<McUeNetDevice> ();
          if (ueDevice)
            {
              imsi = ueDevice->GetImsi ();
            }
        }

      Vector position = mobility->GetPosition ();
      Vector velocity = mobility->GetVelocity ();
      g_positionCsvFile << now << ","
                        << node->GetId () << ","
                        << imsi << ","
                        << position.x << ","
                        << position.y << ","
                        << position.z << ","
                        << velocity.x << ","
                        << velocity.y << ","
                        << velocity.z << "\n";
    }

  Simulator::Schedule (step, &RecordUePositionsSnapshot, ueNodes, mcUeDevs, step);
}

struct AppTrafficState
{
  uint64_t lastTotalRxBytes = 0;
  Time lastPacketDelay = Seconds (0);
  double intervalJitterSumMs = 0.0;
  uint64_t intervalJitterSamples = 0;
  uint64_t intervalRxPackets = 0;
  bool hasLastPacketDelay = false;
};

std::map<uint64_t, AppTrafficState> g_appTrafficState;

void
DlPacketRxCallback (uint64_t imsi, Ptr<const Packet> packet, const Address &from)
{
  (void) from;
  Ptr<Packet> copy = packet->Copy ();
  SeqTsHeader header;
  if (copy->PeekHeader (header) == 0)
    {
      return;
    }
  AppTrafficState &state = g_appTrafficState[imsi];
  Time delay = Simulator::Now () - header.GetTs ();
  if (state.hasLastPacketDelay)
    {
      state.intervalJitterSumMs += std::abs ((delay - state.lastPacketDelay).GetSeconds ()) * 1000.0;
      ++state.intervalJitterSamples;
    }
  state.lastPacketDelay = delay;
  state.hasLastPacketDelay = true;
  ++state.intervalRxPackets;
}

void
RecordAppMetricsSnapshot (NodeContainer ueNodes,
                          NetDeviceContainer mcUeDevs,
                          ApplicationContainer dlSinkApps,
                          Time step)
{
  const double now = Simulator::Now ().GetSeconds ();
  const double intervalSeconds = step.GetSeconds ();
  for (uint32_t i = 0; i < dlSinkApps.GetN (); ++i)
    {
      Ptr<PacketSink> sink = DynamicCast<PacketSink> (dlSinkApps.Get (i));
      if (!sink)
        {
          continue;
        }

      uint64_t imsi = 0;
      uint32_t nodeId = 0;
      if (i < ueNodes.GetN ())
        {
          nodeId = ueNodes.Get (i)->GetId ();
        }
      if (i < mcUeDevs.GetN ())
        {
          Ptr<McUeNetDevice> ueDevice = mcUeDevs.Get (i)->GetObject<McUeNetDevice> ();
          if (ueDevice)
            {
              imsi = ueDevice->GetImsi ();
            }
        }

      uint64_t totalRxBytes = sink->GetTotalRx ();
      AppTrafficState &state = g_appTrafficState[imsi];
      uint64_t intervalRxBytes = totalRxBytes >= state.lastTotalRxBytes
                                     ? totalRxBytes - state.lastTotalRxBytes
                                     : 0;
      double throughputKbps = intervalSeconds > 0.0
                                  ? static_cast<double> (intervalRxBytes) * 8.0 / intervalSeconds / 1000.0
                                  : 0.0;
      double jitterMs = state.intervalJitterSamples > 0
                              ? state.intervalJitterSumMs / state.intervalJitterSamples
                              : 0.0;

      g_appMetricsCsvFile << now << ","
                          << nodeId << ","
                          << imsi << ","
                          << totalRxBytes << ","
                          << intervalRxBytes << ","
                          << throughputKbps << ","
                          << jitterMs << "\n";

      state.lastTotalRxBytes = totalRxBytes;
      state.intervalJitterSumMs = 0.0;
      state.intervalJitterSamples = 0;
      state.intervalRxPackets = 0;
    }

  Simulator::Schedule (step, &RecordAppMetricsSnapshot, ueNodes, mcUeDevs, dlSinkApps, step);
}




void
PrintGnuplottableUeListToFile (std::string filename)
{
  std::ofstream outFile;
  outFile.open (filename.c_str (), std::ios_base::out | std::ios_base::trunc);
  if (!outFile.is_open ())
    {
      NS_LOG_ERROR ("Can't open file " << filename);
      return;
    }
  for (NodeList::Iterator it = NodeList::Begin (); it != NodeList::End (); ++it)
    {
      Ptr<Node> node = *it;
      int nDevs = node->GetNDevices ();
      for (int j = 0; j < nDevs; j++)
        {
          Ptr<LteUeNetDevice> uedev = node->GetDevice (j)->GetObject<LteUeNetDevice> ();
          Ptr<MmWaveUeNetDevice> mmuedev = node->GetDevice (j)->GetObject<MmWaveUeNetDevice> ();
          Ptr<McUeNetDevice> mcuedev = node->GetDevice (j)->GetObject<McUeNetDevice> ();
          if (uedev)
            {
              Vector pos = node->GetObject<MobilityModel> ()->GetPosition ();
              outFile << "set label \"" << uedev->GetImsi () << "\" at " << pos.x << "," << pos.y
                      << " left font \"Helvetica,8\" textcolor rgb \"black\" front point pt 1 ps "
                         "0.3 lc rgb \"black\" offset 0,0"
                      << std::endl;
            }
          else if (mmuedev)
            {
              Vector pos = node->GetObject<MobilityModel> ()->GetPosition ();
              outFile << "set label \"" << mmuedev->GetImsi () << "\" at " << pos.x << "," << pos.y
                      << " left font \"Helvetica,8\" textcolor rgb \"black\" front point pt 1 ps "
                         "0.3 lc rgb \"black\" offset 0,0"
                      << std::endl;
            }
          else if (mcuedev)
            {
              Vector pos = node->GetObject<MobilityModel> ()->GetPosition ();
              outFile << "set label \"" << mcuedev->GetImsi () << "\" at " << pos.x << "," << pos.y
                      << " left font \"Helvetica,8\" textcolor rgb \"black\" front point pt 1 ps "
                         "0.3 lc rgb \"black\" offset 0,0"
                      << std::endl;
            }
        }
    }
}

void
PrintGnuplottableEnbListToFile (std::string filename)
{
  std::ofstream outFile;
  outFile.open (filename.c_str (), std::ios_base::out | std::ios_base::trunc);
  if (!outFile.is_open ())
    {
      NS_LOG_ERROR ("Can't open file " << filename);
      return;
    }
  for (NodeList::Iterator it = NodeList::Begin (); it != NodeList::End (); ++it)
    {
      Ptr<Node> node = *it;
      int nDevs = node->GetNDevices ();
      for (int j = 0; j < nDevs; j++)
        {
          Ptr<LteEnbNetDevice> enbdev = node->GetDevice (j)->GetObject<LteEnbNetDevice> ();
          Ptr<MmWaveEnbNetDevice> mmdev = node->GetDevice (j)->GetObject<MmWaveEnbNetDevice> ();
          if (enbdev)
            {
              Vector pos = node->GetObject<MobilityModel> ()->GetPosition ();
              outFile << "set label \"" << enbdev->GetCellId () << "\" at " << pos.x << "," << pos.y
                      << " left font \"Helvetica,8\" textcolor rgb \"blue\" front  point pt 4 ps "
                         "0.3 lc rgb \"blue\" offset 0,0"
                      << std::endl;
            }
          else if (mmdev)
            {
              Vector pos = node->GetObject<MobilityModel> ()->GetPosition ();
              outFile << "set label \"" << mmdev->GetCellId () << "\" at " << pos.x << "," << pos.y
                      << " left font \"Helvetica,8\" textcolor rgb \"red\" front  point pt 4 ps "
                         "0.3 lc rgb \"red\" offset 0,0"
                      << std::endl;
            }
        }
    }
}

void
PrintPosition (Ptr<Node> node)
{
  Ptr<MobilityModel> model = node->GetObject<MobilityModel> ();
  NS_LOG_UNCOND ("Position +****************************** " << model->GetPosition () << " at time "
                                                             << Simulator::Now ().GetSeconds ());
}

static ns3::GlobalValue g_bufferSize ("bufferSize", "RLC tx buffer size (MB)",
                                      ns3::UintegerValue (10),
                                      ns3::MakeUintegerChecker<uint32_t> ());

static ns3::GlobalValue g_enableTraces ("enableTraces", "If true, generate ns-3 traces",
                                        ns3::BooleanValue (false), ns3::MakeBooleanChecker ());

static ns3::GlobalValue g_e2lteEnabled ("e2lteEnabled", "If true, send LTE E2 reports",
                                        ns3::BooleanValue (false), ns3::MakeBooleanChecker ());

static ns3::GlobalValue g_e2nrEnabled ("e2nrEnabled", "If true, send NR E2 reports",
                                       ns3::BooleanValue (false), ns3::MakeBooleanChecker ());

static ns3::GlobalValue g_e2du ("e2du", "If true, send DU reports", ns3::BooleanValue (false),
                                ns3::MakeBooleanChecker ());

static ns3::GlobalValue g_e2cuUp ("e2cuUp", "If true, send CU-UP reports", ns3::BooleanValue (false),
                                  ns3::MakeBooleanChecker ());

static ns3::GlobalValue g_e2cuCp ("e2cuCp", "If true, send CU-CP reports", ns3::BooleanValue (false),
                                  ns3::MakeBooleanChecker ());

static ns3::GlobalValue g_reducedPmValues ("reducedPmValues", "If true, use a subset of the the pm containers",
                                        ns3::BooleanValue (false), ns3::MakeBooleanChecker ());

static ns3::GlobalValue
    g_hoSinrDifference ("hoSinrDifference",
                        "The value for which an handover between MmWave eNB is triggered",
                        ns3::DoubleValue (3), ns3::MakeDoubleChecker<double> ());

static ns3::GlobalValue
    g_indicationPeriodicity ("indicationPeriodicity",
                             "E2 Indication Periodicity reports (value in seconds)",
                             ns3::DoubleValue (0.1), ns3::MakeDoubleChecker<double> (0.01, 2.0));

static ns3::GlobalValue g_simTime ("simTime", "Simulation time in seconds", ns3::DoubleValue (2),
                                   ns3::MakeDoubleChecker<double> (0.1, 100.0));

//inferior limit for connection between UE and gNB
static ns3::GlobalValue g_outageThreshold ("outageThreshold",
                                           "SNR threshold for outage events [dB]", // use -1000.0 with NoAuto
                                           ns3::DoubleValue (-5.0),
                                           ns3::MakeDoubleChecker<double> ());

static ns3::GlobalValue g_numberOfRaPreambles (
    "numberOfRaPreambles",
    "how many random access preambles are available for the contention based RACH process",
    ns3::UintegerValue (40), // Indicated for TS use case, 52 is default
    ns3::MakeUintegerChecker<uint8_t> ());

static ns3::GlobalValue
    g_handoverMode ("handoverMode",
                    "HO euristic to be used,"
                    "can be only \"NoAuto\", \"FixedTtt\", \"DynamicTtt\",   \"Threshold\"",
                    ns3::StringValue ("DynamicTtt"), ns3::MakeStringChecker ());

static ns3::GlobalValue g_e2TermIp ("e2TermIp", "The IP address of the RIC E2 termination",
                                    ns3::StringValue ("10.0.2.10"), ns3::MakeStringChecker ());

static ns3::GlobalValue
    g_enableE2FileLogging ("enableE2FileLogging",
                           "If true, generate offline file logging instead of connecting to RIC",
                           ns3::BooleanValue (true), ns3::MakeBooleanChecker ());

static ns3::GlobalValue g_controlFileName ("controlFileName",
                                           "The path to the control file (can be absolute)",
                                           ns3::StringValue (""),
                                           ns3::MakeStringChecker ());

static ns3::GlobalValue q_useSemaphores ("useSemaphores", "If true, enables the use of semaphores for external environment control",
                                        ns3::BooleanValue (false), ns3::MakeBooleanChecker ());

static ns3::GlobalValue g_outputDir ("outputDir",
                                     "Directory used to store scenario output files",
                                     ns3::StringValue ("outputs/scenario-six"),
                                     ns3::MakeStringChecker ());

static ns3::GlobalValue g_signalSamplePeriod ("signalSamplePeriod",
                                              "Synchronous signal dataset sampling period in seconds",
                                              ns3::DoubleValue (0.1),
                                              ns3::MakeDoubleChecker<double> (0.001, 10.0));

int
main (int argc, char *argv[])
{
  namespace fs = std::filesystem;

  LogComponentEnableAll (LOG_PREFIX_ALL);
  // LogComponentEnable ("RicControlMessage", LOG_LEVEL_ALL);
  // LogComponentEnable ("Asn1Types", LOG_LEVEL_LOGIC);
  // LogComponentEnable ("E2Termination", LOG_LEVEL_LOGIC);

  // LogComponentEnable ("LteEnbNetDevice", LOG_LEVEL_ALL);
  // LogComponentEnable ("MmWaveEnbNetDevice", LOG_LEVEL_DEBUG);

  // The maximum X coordinate of the scenario
  double maxXAxis = 4000;
  // The maximum Y coordinate of the scenario
  double maxYAxis = 4000;

  uint32_t nMmWaveEnbNodes = 3;
  uint32_t nLteEnbNodes = 1;
  uint32_t ues = 10;

  // Distance between the mmWave BSs and the two co-located LTE and mmWave BSs in meters
  double isd = 615.0; // longest triangle side, used only to scale UE mobility
  double minSpeed= 2.0;
  double maxSpeed= 10.0;
  uint32_t rngRun = 1;
  // add variable control by terminal
  // Command line arguments
  CommandLine cmd;

  cmd.AddValue("nmmWave", "Number of mmWave gNBs; scenario-zero uses 3 for the equilateral layout", nMmWaveEnbNodes);
  cmd.AddValue("nUes", "Number of UEs", ues);
  cmd.AddValue ("minSpeed", "Minimum UE speed (m/s)", minSpeed);
  cmd.AddValue ("maxSpeed", "Maximum UE speed (m/s)", maxSpeed);
  cmd.AddValue ("rngRun", "Random Number Generator Run", rngRun);

  cmd.Parse (argc, argv);

  ns3::RngSeedManager::SetSeed(1);
  ns3::RngSeedManager::SetRun (rngRun);

  if (nMmWaveEnbNodes != 3)
    {
      NS_FATAL_ERROR ("scenario-six requires exactly 3 mmWave gNBs");
    }


  uint32_t nUeNodes = ues * nMmWaveEnbNodes;
  bool harqEnabled = true;

  UintegerValue uintegerValue;
  BooleanValue booleanValue;
  StringValue stringValue;
  DoubleValue doubleValue;

  GlobalValue::GetValueByName ("hoSinrDifference", doubleValue);
  double hoSinrDifference = doubleValue.Get ();
  GlobalValue::GetValueByName ("bufferSize", uintegerValue);
  uint32_t bufferSize = uintegerValue.Get ();
  GlobalValue::GetValueByName ("enableTraces", booleanValue);
  bool enableTraces = booleanValue.Get ();
  GlobalValue::GetValueByName ("outageThreshold", doubleValue);
  double outageThreshold = doubleValue.Get ();
  GlobalValue::GetValueByName ("handoverMode", stringValue);
  std::string handoverMode = stringValue.Get ();
  GlobalValue::GetValueByName ("e2TermIp", stringValue);
  std::string e2TermIp = stringValue.Get ();
  GlobalValue::GetValueByName ("enableE2FileLogging", booleanValue);
  bool enableE2FileLogging = booleanValue.Get ();
  GlobalValue::GetValueByName ("numberOfRaPreambles", uintegerValue);
  uint8_t numberOfRaPreambles = uintegerValue.Get ();

  NS_LOG_UNCOND ("bufferSize " << bufferSize << " OutageThreshold " << outageThreshold
                               << " HandoverMode " << handoverMode << " e2TermIp " << e2TermIp
                               << " enableE2FileLogging " << enableE2FileLogging);

  GlobalValue::GetValueByName ("e2lteEnabled", booleanValue);
  bool e2lteEnabled = booleanValue.Get ();

  GlobalValue::GetValueByName ("e2nrEnabled", booleanValue);
  bool e2nrEnabled = booleanValue.Get ();

  GlobalValue::GetValueByName ("e2du", booleanValue);
  bool e2du = booleanValue.Get ();

  GlobalValue::GetValueByName ("e2cuUp", booleanValue);
  bool e2cuUp = booleanValue.Get ();

  GlobalValue::GetValueByName ("e2cuCp", booleanValue);
  bool e2cuCp = booleanValue.Get ();

  GlobalValue::GetValueByName ("reducedPmValues", booleanValue);
  bool reducedPmValues = booleanValue.Get ();

  GlobalValue::GetValueByName ("indicationPeriodicity", doubleValue);
  double indicationPeriodicity = doubleValue.Get ();

  GlobalValue::GetValueByName ("controlFileName", stringValue);
  std::string controlFilename = stringValue.Get ();

  GlobalValue::GetValueByName ("useSemaphores", booleanValue);
  bool useSemaphores = booleanValue.Get ();

  GlobalValue::GetValueByName ("outputDir", stringValue);
  std::string outputDir = stringValue.Get ();

  GlobalValue::GetValueByName ("signalSamplePeriod", doubleValue);
  double signalSamplePeriod = doubleValue.Get ();


  NS_LOG_UNCOND ("e2lteEnabled " << e2lteEnabled << " e2nrEnabled " << e2nrEnabled << " e2du "
                                 << e2du << " e2cuCp " << e2cuCp << " e2cuUp " << e2cuUp
                                 << " controlFilename " << controlFilename
                                 << " useSemaphores " << useSemaphores
                                 << " indicationPeriodicity " << indicationPeriodicity);

  if (!outputDir.empty ())
    {
      fs::path outputPath (outputDir);
      std::error_code ec;
      fs::create_directories (outputPath, ec);
      if (ec)
        {
          NS_FATAL_ERROR ("Could not create output directory " << outputDir << ": " << ec.message ());
        }

      fs::current_path (outputPath, ec);
      if (ec)
        {
          NS_FATAL_ERROR ("Could not switch to output directory " << outputDir << ": " << ec.message ());
        }

      NS_LOG_UNCOND ("Scenario outputs will be written to " << fs::current_path ().string ());
    }

  Config::SetDefault ("ns3::LteEnbNetDevice::UseSemaphores", BooleanValue (useSemaphores));
  Config::SetDefault ("ns3::LteEnbNetDevice::ControlFileName", StringValue (controlFilename));
  Config::SetDefault ("ns3::LteEnbNetDevice::E2Periodicity", DoubleValue (indicationPeriodicity));
  Config::SetDefault ("ns3::MmWaveEnbNetDevice::E2Periodicity",
                      DoubleValue (indicationPeriodicity));

  Config::SetDefault ("ns3::MmWaveHelper::E2ModeLte", BooleanValue (e2lteEnabled));
  Config::SetDefault ("ns3::MmWaveHelper::E2ModeNr", BooleanValue (e2nrEnabled));
  Config::SetDefault ("ns3::MmWaveHelper::E2Periodicity", DoubleValue (indicationPeriodicity));

  // The DU PM reports should come from both NR gNB as well as LTE eNB,
  // since in the RLC/MAC/PHY entities are present in BOTH NR gNB as well as LTE eNB.
  // DU reports from LTE eNB are not implemented in this release
  Config::SetDefault ("ns3::MmWaveEnbNetDevice::EnableDuReport", BooleanValue (e2du));

  // The CU-UP PM reports should only come from LTE eNB, since in the NS3 “EN-DC
  // simulation (Option 3A)”, the PDCP is only in the LTE eNB and NOT in the NR gNB
  Config::SetDefault ("ns3::MmWaveEnbNetDevice::EnableCuUpReport", BooleanValue (e2cuUp));
  Config::SetDefault ("ns3::LteEnbNetDevice::EnableCuUpReport", BooleanValue (e2cuUp));

  Config::SetDefault ("ns3::MmWaveEnbNetDevice::EnableCuCpReport", BooleanValue (e2cuCp));
  Config::SetDefault ("ns3::LteEnbNetDevice::EnableCuCpReport", BooleanValue (e2cuCp));

  Config::SetDefault ("ns3::MmWaveEnbNetDevice::ReducedPmValues", BooleanValue (reducedPmValues));
  Config::SetDefault ("ns3::LteEnbNetDevice::ReducedPmValues", BooleanValue (reducedPmValues));

  Config::SetDefault ("ns3::LteEnbNetDevice::EnableE2FileLogging",
                      BooleanValue (enableE2FileLogging));
  Config::SetDefault ("ns3::MmWaveEnbNetDevice::EnableE2FileLogging",
                      BooleanValue (enableE2FileLogging));

  Config::SetDefault ("ns3::MmWaveEnbMac::NumberOfRaPreambles",
                      UintegerValue (numberOfRaPreambles));

  Config::SetDefault ("ns3::MmWaveHelper::HarqEnabled", BooleanValue (harqEnabled));
  Config::SetDefault ("ns3::MmWaveHelper::UseIdealRrc", BooleanValue (true));
  Config::SetDefault ("ns3::MmWaveHelper::E2TermIp", StringValue (e2TermIp));

  Config::SetDefault ("ns3::MmWaveFlexTtiMacScheduler::HarqEnabled", BooleanValue (harqEnabled));
  Config::SetDefault ("ns3::MmWavePhyMacCommon::NumHarqProcess", UintegerValue (100));
  //Config::SetDefault ("ns3::MmWaveBearerStatsCalculator::EpochDuration", TimeValue (MilliSeconds (10.0)));

  // set to false to use the 3GPP radiation pattern (proper configuration of the bearing and downtilt angles is needed)
  Config::SetDefault ("ns3::ThreeGppChannelModel::UpdatePeriod", TimeValue (MilliSeconds (100.0)));
  Config::SetDefault ("ns3::ThreeGppChannelConditionModel::UpdatePeriod",
                      TimeValue (MilliSeconds (100)));

  Config::SetDefault ("ns3::LteRlcAm::ReportBufferStatusTimer", TimeValue (MilliSeconds (10.0)));
  Config::SetDefault ("ns3::LteRlcUmLowLat::ReportBufferStatusTimer",
                      TimeValue (MilliSeconds (10.0)));
  Config::SetDefault ("ns3::LteRlcUm::MaxTxBufferSize", UintegerValue (bufferSize * 1024 * 1024));
  Config::SetDefault ("ns3::LteRlcUmLowLat::MaxTxBufferSize",
                      UintegerValue (bufferSize * 1024 * 1024));
  Config::SetDefault ("ns3::LteRlcAm::MaxTxBufferSize", UintegerValue (bufferSize * 1024 * 1024));

  Config::SetDefault ("ns3::LteEnbRrc::OutageThreshold", DoubleValue (outageThreshold));
  Config::SetDefault ("ns3::LteEnbRrc::SecondaryCellHandoverMode", StringValue (handoverMode));
  Config::SetDefault ("ns3::LteEnbRrc::HoSinrDifference", DoubleValue (hoSinrDifference));

  // Carrier bandwidth in Hz
  double bandwidth = 20e6;
  // Center frequency in Hz
  double centerFrequency = 3.5e9;

  // Number of antennas in each UE
  int numAntennasMcUe = 1;
  // Number of antennas in each mmWave BS
  int numAntennasMmWave = 1;

  NS_LOG_INFO ("Bandwidth " << bandwidth << " centerFrequency " << double (centerFrequency)
                            << " isd " << isd << " numAntennasMcUe " << numAntennasMcUe
                            << " numAntennasMmWave " << numAntennasMmWave);

  Config::SetDefault ("ns3::MmWavePhyMacCommon::Bandwidth", DoubleValue (bandwidth));
  Config::SetDefault ("ns3::MmWavePhyMacCommon::CenterFreq", DoubleValue (centerFrequency));

  Ptr<MmWaveHelper> mmwaveHelper = CreateObject<MmWaveHelper> ();
  mmwaveHelper->SetPathlossModelType ("ns3::ThreeGppUmiStreetCanyonPropagationLossModel");
  mmwaveHelper->SetChannelConditionModelType ("ns3::ThreeGppUmiStreetCanyonChannelConditionModel");

  // Set the number of antennas in the devices
  mmwaveHelper->SetUePhasedArrayModelAttribute("NumColumns", UintegerValue(std::sqrt(numAntennasMcUe)));
  mmwaveHelper->SetUePhasedArrayModelAttribute("NumRows", UintegerValue(std::sqrt(numAntennasMcUe)));
  mmwaveHelper->SetEnbPhasedArrayModelAttribute("NumColumns",UintegerValue(std::sqrt(numAntennasMmWave)));
  mmwaveHelper->SetEnbPhasedArrayModelAttribute("NumRows", UintegerValue(std::sqrt(numAntennasMmWave)));

  Ptr<MmWavePointToPointEpcHelper> epcHelper = CreateObject<MmWavePointToPointEpcHelper> ();
  mmwaveHelper->SetEpcHelper (epcHelper);


  NS_LOG_INFO (" Bandwidth " << bandwidth << " centerFrequency " << double (centerFrequency)
                             << " isd " << isd << " numAntennasMcUe " << numAntennasMcUe
                             << " numAntennasMmWave " << numAntennasMmWave << " nMmWaveEnbNodes "
                             << unsigned (nMmWaveEnbNodes));

  // Get SGW/PGW and create a single RemoteHost
  Ptr<Node> pgw = epcHelper->GetPgwNode ();
  NodeContainer remoteHostContainer;
  remoteHostContainer.Create (1);
  Ptr<Node> remoteHost = remoteHostContainer.Get (0);
  InternetStackHelper internet;
  internet.Install (remoteHostContainer);

  // Create the Internet by connecting remoteHost to pgw. Setup routing too
  PointToPointHelper p2ph;
  p2ph.SetDeviceAttribute ("DataRate", DataRateValue (DataRate ("100Gb/s")));
  p2ph.SetDeviceAttribute ("Mtu", UintegerValue (2500));
  p2ph.SetChannelAttribute ("Delay", TimeValue (Seconds (0.010)));
  NetDeviceContainer internetDevices = p2ph.Install (pgw, remoteHost);
  Ipv4AddressHelper ipv4h;
  ipv4h.SetBase ("1.0.0.0", "255.0.0.0");
  Ipv4InterfaceContainer internetIpIfaces = ipv4h.Assign (internetDevices);
  // interface 0 is localhost, 1 is the p2p device
  Ipv4Address remoteHostAddr = internetIpIfaces.GetAddress (1);
  Ipv4StaticRoutingHelper ipv4RoutingHelper;
  Ptr<Ipv4StaticRouting> remoteHostStaticRouting =
      ipv4RoutingHelper.GetStaticRouting (remoteHost->GetObject<Ipv4> ());
  remoteHostStaticRouting->AddNetworkRouteTo (Ipv4Address ("7.0.0.0"), Ipv4Mask ("255.0.0.0"), 1);

  // create LTE, mmWave eNB nodes and UE node
  NodeContainer ueNodes;
  NodeContainer mmWaveEnbNodes;
  NodeContainer lteEnbNodes;
  NodeContainer allEnbNodes;
  mmWaveEnbNodes.Create (nMmWaveEnbNodes);
  lteEnbNodes.Create (nLteEnbNodes);
  ueNodes.Create (nUeNodes);
  allEnbNodes.Add (lteEnbNodes);
  allEnbNodes.Add (mmWaveEnbNodes);

  // Fixed scalene triangle: gNB1-gNB2=615 m, gNB1-gNB3=585 m,
  // and gNB2-gNB3=500 m. LTE is co-located with gNB1.
  const double d12 = 615.0;
  const double d13 = 585.0;
  const double d23 = 500.0;
  const double gnb3RelativeX =
      (d13 * d13 + d12 * d12 - d23 * d23) / (2.0 * d12);
  const double gnb3RelativeY =
      std::sqrt (d13 * d13 - gnb3RelativeX * gnb3RelativeX);

  Vector centerPosition = Vector (maxXAxis / 2.0, maxYAxis / 2.0, 20.0);
  std::vector<Vector> mmWavePositions;
  mmWavePositions.push_back (centerPosition);
  mmWavePositions.push_back (Vector (centerPosition.x + d12, centerPosition.y, 25.0));
  mmWavePositions.push_back (Vector (centerPosition.x + gnb3RelativeX,
                                     centerPosition.y + gnb3RelativeY,
                                     25.0));

  Ptr<ListPositionAllocator> enbPositionAlloc = CreateObject<ListPositionAllocator> ();
  enbPositionAlloc->Add (centerPosition); // LTE anchor co-located with gNB1
  for (const auto &position : mmWavePositions)
    {
      enbPositionAlloc->Add (position);
    }

  MobilityHelper enbmobility;
  enbmobility.SetMobilityModel ("ns3::ConstantPositionMobilityModel");
  enbmobility.SetPositionAllocator (enbPositionAlloc);
  enbmobility.Install (allEnbNodes);

  // Windowed, roofed supporting building below cell 2 (gNB1 + LTE).
  std::vector<Box> buildingBoxes;
  CreateWindowedBuilding (buildingBoxes, mmWavePositions[0].x, mmWavePositions[0].y,
                          83.0, 118.0, 10.0);

  // H-shaped building centered at gNB2 (cell 3).
  const double hBarOffset = 25.0;
  CreateScenarioBuilding (buildingBoxes, mmWavePositions[1].x - hBarOffset,
                          mmWavePositions[1].y, 15.0, 80.0, 10.0);
  CreateScenarioBuilding (buildingBoxes, mmWavePositions[1].x + hBarOffset,
                          mmWavePositions[1].y, 15.0, 80.0, 10.0);
  CreateScenarioBuilding (buildingBoxes, mmWavePositions[1].x,
                          mmWavePositions[1].y + 15.0, 35.0, 20.0, 10.0);
  CreateScenarioBuilding (buildingBoxes, mmWavePositions[1].x,
                          mmWavePositions[1].y - 15.0, 35.0, 20.0, 10.0);

  // The former gNB2 building is moved below gNB3.
  CreateScenarioBuilding (buildingBoxes, mmWavePositions[2].x, mmWavePositions[2].y,
                          35.0, 85.0, 15.0);

  // Move the 400 x 200 m low-rise campus from cell 3 to cell 4 while
  // preserving its scale and relative arrangement.
  const double campusDx = mmWavePositions[2].x - mmWavePositions[1].x;
  const double campusDy = mmWavePositions[2].y - mmWavePositions[1].y;
  CreateWindowedBuilding (buildingBoxes, 2470.0 + campusDx, 1970.0 + campusDy, 25.0, 110.0, 4.0);
  CreateWindowedBuilding (buildingBoxes, 2520.0 + campusDx, 2030.0 + campusDy, 25.0, 110.0, 4.0);

  // Upper-left H-shaped low-rise complex.
  CreateWindowedBuilding (buildingBoxes, 2250.0 + campusDx, 2040.0 + campusDy, 25.0, 90.0, 7.0);
  CreateWindowedBuilding (buildingBoxes, 2320.0 + campusDx, 2040.0 + campusDy, 25.0, 90.0, 7.0);
  CreateWindowedBuilding (buildingBoxes, 2285.0 + campusDx, 2040.0 + campusDy, 45.0, 20.0, 7.0);

  // Lower-left U-shaped one-storey complex.
  CreateWindowedBuilding (buildingBoxes, 2250.0 + campusDx, 1940.0 + campusDy, 20.0, 60.0, 4.0);
  CreateWindowedBuilding (buildingBoxes, 2320.0 + campusDx, 1940.0 + campusDy, 20.0, 60.0, 4.0);
  CreateWindowedBuilding (buildingBoxes, 2285.0 + campusDx, 1900.0 + campusDy, 70.0, 20.0, 4.0);

  // Small two-storey complex immediately to the left/upper-left of gNB2.
  CreateWindowedBuilding (buildingBoxes, 2570.0 + campusDx, 2070.0 + campusDy, 24.0, 55.0, 7.0);
  CreateWindowedBuilding (buildingBoxes, 2605.0 + campusDx, 2080.0 + campusDy, 24.0, 40.0, 7.0);

  const double blockerFraction = 300.0 / d23;
  const double blockerX = mmWavePositions[1].x +
                          blockerFraction * (mmWavePositions[2].x - mmWavePositions[1].x);
  const double blockerY = mmWavePositions[1].y +
                          blockerFraction * (mmWavePositions[2].y - mmWavePositions[1].y);
  CreateScenarioBuilding (buildingBoxes, blockerX, blockerY, 127.0, 63.0, 10.0);

  std::ofstream buildingCsv ("buildings.csv", std::ios::out | std::ios::trunc);
  buildingCsv << "building_id,center_x_m,center_y_m,width_x_m,depth_y_m,height_m\n";
  for (uint32_t i = 0; i < buildingBoxes.size (); ++i)
    {
      const Box &box = buildingBoxes[i];
      buildingCsv << (i + 1) << ","
                  << (box.xMin + box.xMax) / 2.0 << ","
                  << (box.yMin + box.yMax) / 2.0 << ","
                  << (box.xMax - box.xMin) << ","
                  << (box.yMax - box.yMin) << ","
                  << box.zMax << "\n";
    }
  buildingCsv.close ();

  MobilityHelper uemobility;
  Ptr<ListPositionAllocator> uePositionAlloc = CreateObject<ListPositionAllocator> ();
  const uint32_t aroundEnd = nUeNodes * 2 / 5;
  const uint32_t betweenEnd = nUeNodes * 3 / 5;
  const uint32_t campusEnd = nUeNodes * 4 / 5;
  const uint32_t aroundPerGnb = (aroundEnd + nMmWaveEnbNodes - 1) / nMmWaveEnbNodes;

  std::vector<Vector> campusOffsets = {
      Vector (-75.0, 65.0, 0.0),
      Vector (-125.0, -10.0, 0.0),
      Vector (-175.0, 70.0, 0.0),
      Vector (-225.0, -55.0, 0.0),
      Vector (-290.0, 35.0, 0.0),
      Vector (-105.0, -85.0, 0.0)};

  std::vector<Vector> indoorUePositions = {
      Vector (mmWavePositions[1].x - hBarOffset, mmWavePositions[1].y, 1.5),
      Vector (mmWavePositions[1].x + hBarOffset, mmWavePositions[1].y, 1.5),
      Vector (mmWavePositions[1].x, mmWavePositions[1].y + 15.0, 1.5),
      Vector (mmWavePositions[1].x, mmWavePositions[1].y - 15.0, 1.5),
      Vector (mmWavePositions[2].x, mmWavePositions[2].y, 1.5),
      Vector (blockerX, blockerY, 1.5)};

  for (uint32_t u = 0; u < nUeNodes; ++u)
    {
      if (u < aroundEnd)
        {
          const uint32_t gnbIndex = u % nMmWaveEnbNodes;
          const uint32_t localIndex = u / nMmWaveEnbNodes;
          const Vector &gnbPosition = mmWavePositions[gnbIndex];
          const double angle =
              2.0 * M_PI * static_cast<double> (localIndex) /
                  static_cast<double> (aroundPerGnb) +
              static_cast<double> (gnbIndex) * M_PI / 6.0;
          const double radius = 90.0 + 25.0 * static_cast<double> (localIndex % 3);
          uePositionAlloc->Add (Vector (gnbPosition.x + radius * std::cos (angle),
                                        gnbPosition.y + radius * std::sin (angle),
                                        1.5));
        }
      else if (u < betweenEnd)
        {
          const uint32_t index = u - aroundEnd;
          const uint32_t edge = index % nMmWaveEnbNodes;
          const uint32_t lane = index / nMmWaveEnbNodes;
          const Vector &a = mmWavePositions[edge];
          const Vector &b = mmWavePositions[(edge + 1) % nMmWaveEnbNodes];
          const double dx = b.x - a.x;
          const double dy = b.y - a.y;
          const double length = std::sqrt (dx * dx + dy * dy);
          const double t = (lane % 2 == 0) ? 0.38 : 0.62;
          const double lateral = (lane % 2 == 0) ? 35.0 : -35.0;
          uePositionAlloc->Add (Vector (a.x + t * dx - lateral * dy / length,
                                        a.y + t * dy + lateral * dx / length,
                                        1.5));
        }
      else if (u < campusEnd)
        {
          const uint32_t index = u - betweenEnd;
          const Vector &offset = campusOffsets[index % campusOffsets.size ()];
          const double extraOffset = 12.0 * static_cast<double> (index / campusOffsets.size ());
          uePositionAlloc->Add (Vector (mmWavePositions[2].x + offset.x - extraOffset,
                                        mmWavePositions[2].y + offset.y,
                                        1.5));
        }
      else
        {
          const uint32_t index = u - campusEnd;
          uePositionAlloc->Add (indoorUePositions[index % indoorUePositions.size ()]);
        }
    }

  Ptr<UniformRandomVariable> speed = CreateObject<UniformRandomVariable> ();
  speed->SetAttribute ("Min", DoubleValue (minSpeed));
  speed->SetAttribute ("Max", DoubleValue (maxSpeed));

  double mobilityMargin = 420.0;
  double mobilityRadius = d12 + mobilityMargin;
  uemobility.SetMobilityModel ("ns3::RandomWalk2dMobilityModel", "Speed",
                               PointerValue (speed), "Bounds",
                               RectangleValue (Rectangle (centerPosition.x - mobilityRadius,
                                                          centerPosition.x + mobilityRadius,
                                                          centerPosition.y - mobilityRadius,
                                                          centerPosition.y + mobilityRadius)));
  uemobility.SetPositionAllocator (uePositionAlloc);
  uemobility.Install (ueNodes);

  BuildingsHelper::Install (allEnbNodes);
  BuildingsHelper::Install (ueNodes);

  // Install mmWave, lte, mc Devices to the nodes
  NetDeviceContainer lteEnbDevs = mmwaveHelper->InstallLteEnbDevice (lteEnbNodes);
  NetDeviceContainer mmWaveEnbDevs = mmwaveHelper->InstallEnbDevice (mmWaveEnbNodes);
  NetDeviceContainer mcUeDevs = mmwaveHelper->InstallMcUeDevice (ueNodes);

  // Install the IP stack on the UEs
  internet.Install (ueNodes);
  Ipv4InterfaceContainer ueIpIface;
  ueIpIface = epcHelper->AssignUeIpv4Address (NetDeviceContainer (mcUeDevs));
  // Assign IP address to UEs, and install applications
  for (uint32_t u = 0; u < ueNodes.GetN (); ++u)
    {
      Ptr<Node> ueNode = ueNodes.Get (u);
      // Set the default gateway for the UE
      Ptr<Ipv4StaticRouting> ueStaticRouting =
          ipv4RoutingHelper.GetStaticRouting (ueNode->GetObject<Ipv4> ());
      ueStaticRouting->SetDefaultRoute (epcHelper->GetUeDefaultGatewayAddress (), 1);
    }

  // Add X2 interfaces
  mmwaveHelper->AddX2Interface (lteEnbNodes, mmWaveEnbNodes);

  // Manual attachment
  mmwaveHelper->AttachToClosestEnb (mcUeDevs, mmWaveEnbDevs, lteEnbDevs);

  // Install and start applications
  // On the remoteHost there is UDP OnOff Application

  uint16_t portUdp = 60000;
  Address sinkLocalAddressUdp (InetSocketAddress (Ipv4Address::GetAny (), portUdp));
  PacketSinkHelper sinkHelperUdp ("ns3::UdpSocketFactory", sinkLocalAddressUdp);
  AddressValue serverAddressUdp (InetSocketAddress (remoteHostAddr, portUdp));

  ApplicationContainer sinkApp;
  sinkApp.Add (sinkHelperUdp.Install (remoteHost));

  ApplicationContainer clientApp;
  ApplicationContainer dlSinkApps;

  for (uint32_t u = 0; u < ueNodes.GetN (); ++u)
    {
      // Full traffic
      PacketSinkHelper dlPacketSinkHelper ("ns3::UdpSocketFactory",
                                           InetSocketAddress (Ipv4Address::GetAny (), 1234));
      ApplicationContainer ueSinkApp = dlPacketSinkHelper.Install (ueNodes.Get (u));
      sinkApp.Add (ueSinkApp);
      dlSinkApps.Add (ueSinkApp);
      Ptr<PacketSink> dlSink = DynamicCast<PacketSink> (ueSinkApp.Get (0));
      Ptr<McUeNetDevice> ueDevice = mcUeDevs.Get (u)->GetObject<McUeNetDevice> ();
      if (dlSink && ueDevice)
        {
          dlSink->TraceConnectWithoutContext ("Rx", MakeBoundCallback (&DlPacketRxCallback, ueDevice->GetImsi ()));
        }
      UdpClientHelper dlClient (ueIpIface.GetAddress (u), 1234);
      dlClient.SetAttribute ("Interval", TimeValue (MicroSeconds (500)));
      dlClient.SetAttribute ("MaxPackets", UintegerValue (UINT32_MAX));
      dlClient.SetAttribute ("PacketSize", UintegerValue (1280));
      clientApp.Add (dlClient.Install (remoteHost));
    }

  // Start applications
  GlobalValue::GetValueByName ("simTime", doubleValue);
  double simTime = doubleValue.Get ();
  sinkApp.Start (Seconds (0));

  clientApp.Start (MilliSeconds (100));
  clientApp.Stop (Seconds (simTime - 0.1));

  // int numPrints = 5;
  // for (int i = 0; i < numPrints; i++)
  //   {
  //     for (uint32_t j = 0; j < ueNodes.GetN (); j++)
  //       {
  //         Simulator::Schedule (Seconds (i * simTime / numPrints), &PrintPosition, ueNodes.Get (j));
  //       }
  //   }

  if (enableTraces)
    {
      mmwaveHelper->EnableTraces ();
    }

  // trick to enable PHY traces for the LTE stack
  Ptr<LteHelper> lteHelper = CreateObject<LteHelper> ();
  lteHelper->Initialize ();
  lteHelper->EnablePhyTraces ();
  lteHelper->EnableMacTraces ();

  // Since nodes are randomly allocated during each run we always need to print their positions
  PrintGnuplottableUeListToFile ("ues.txt");
  PrintGnuplottableEnbListToFile ("enbs.txt");
  g_csvFile.open ("dataset_lstm.csv", std::ios::out | std::ios::trunc);
  g_csvFile << "time_s,source,imsi,rnti,cell_id,component_carrier_id,rsrp_dbm,sinr_db,rsrq_db,is_serving_cell\n";
  g_positionCsvFile.open ("ue_positions.csv", std::ios::out | std::ios::trunc);
  g_positionCsvFile << "time_s,node_id,imsi,x_m,y_m,z_m,vx_mps,vy_mps,vz_mps\n";
  g_appMetricsCsvFile.open ("app_metrics.csv", std::ios::out | std::ios::trunc);
  g_appMetricsCsvFile << "time_s,node_id,imsi,rx_bytes_total,interval_rx_bytes,throughput_kbps,jitter_ms\n";

  Config::ConnectFailSafe (
      "/NodeList/*/DeviceList/*/LteUePhy/ReportCurrentCellRsrpSinr",
      MakeCallback (&LteRsrpSinrCallback));
  Config::ConnectFailSafe (
      "/NodeList/*/DeviceList/*/ComponentCarrierMapUe/*/ns3::LteUePhy/ReportCurrentCellRsrpSinr",
      MakeCallback (&LteRsrpSinrCallback));
  Config::ConnectFailSafe (
      "/NodeList/*/DeviceList/*/LteUePhy/ReportUeMeasurements",
      MakeCallback (&LteRsrpRsrqCallback));
  Config::ConnectFailSafe (
      "/NodeList/*/DeviceList/*/ComponentCarrierMapUe/*/ns3::LteUePhy/ReportUeMeasurements",
      MakeCallback (&LteRsrpRsrqCallback));
  Config::ConnectFailSafe (
      "/NodeList/*/DeviceList/*/LteEnbRrc/NotifyMmWaveSinr",
      MakeCallback (&MmWaveSinrCallback));

  for (uint32_t i = 0; i < mcUeDevs.GetN (); ++i)
    {
      Ptr<McUeNetDevice> ueDevice = mcUeDevs.Get (i)->GetObject<McUeNetDevice> ();
      if (ueDevice && ueDevice->GetMmWavePhy ())
        {
          ueDevice->GetMmWavePhy ()->TraceConnectWithoutContext (
              "ReportCurrentCellRsrpSinr",
              MakeCallback (&MmWavePhyRsrpSinrCallbackWithoutContext));
        }
    }

  Time intervaloAmostragem = Seconds (signalSamplePeriod);
  Simulator::Schedule (intervaloAmostragem,
                       &RecordSignalMetricsSnapshot,
                       intervaloAmostragem);
  Simulator::Schedule (intervaloAmostragem,
                       &RecordUePositionsSnapshot,
                       ueNodes,
                       mcUeDevs,
                       intervaloAmostragem);
  Simulator::Schedule (intervaloAmostragem,
                       &RecordAppMetricsSnapshot,
                       ueNodes,
                       mcUeDevs,
                       dlSinkApps,
                       intervaloAmostragem);
  bool run = true;
  if (run)
    {
      NS_LOG_UNCOND ("Simulation time is " << simTime << " seconds ");
      Simulator::Stop (Seconds (simTime));
      NS_LOG_INFO ("Run Simulation.");
      Simulator::Run ();
    }

  g_csvFile.close();
  g_positionCsvFile.close();
  g_appMetricsCsvFile.close();

  NS_LOG_INFO (lteHelper);
  Simulator::Destroy ();
  NS_LOG_INFO ("Done.");
  return 0;
}
