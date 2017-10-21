/*=Plus=header=begin======================================================
Program: Plus
Copyright (c) Laboratory for Percutaneous Surgery. All rights reserved.
See License.txt for details.
=========================================================Plus=header=end*/

// Local includes
#include "PlusConfigure.h"
#include "PlusTrackedFrame.h"
#include "vtkPlusChannel.h"
#include "vtkPlusCommand.h"
#include "vtkPlusCommandProcessor.h"
#include "vtkPlusDataCollector.h"
#include "vtkPlusIgtlMessageCommon.h"
#include "vtkPlusIgtlMessageFactory.h"
#include "vtkPlusOpenIGTLinkServer.h"
#include "vtkPlusRecursiveCriticalSection.h"
#include "vtkPlusTrackedFrameList.h"
#include "vtkPlusTransformRepository.h"

// VTK includes
#include <vtkImageData.h>
#include <vtkObjectFactory.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkPolyDataReader.h>

// OpenIGTLink includes
#include <igtlCommandMessage.h>
#include <igtlImageMessage.h>
#include <igtlImageMetaMessage.h>
#include <igtlMessageHeader.h>
#include <igtlPlusClientInfoMessage.h>
#include <igtlStatusMessage.h>
#include <igtlStringMessage.h>
#include <igtlPolyDataMessage.h>
#include <igtlTrackingDataMessage.h>

// OpenIGTLinkIO includes
#include <igtlioPolyDataConverter.h>

#if defined(WIN32)
  #include "vtkPlusOpenIGTLinkServerWin32.cxx"
#elif defined(__APPLE__)
  #include "vtkPlusOpenIGTLinkServerMacOSX.cxx"
#elif defined(__linux__)
  #include "vtkPlusOpenIGTLinkServerLinux.cxx"
#endif

static const double DELAY_ON_SENDING_ERROR_SEC = 0.02;
static const double DELAY_ON_NO_NEW_FRAMES_SEC = 0.005;
static const int NUMBER_OF_RECENT_COMMAND_IDS_STORED = 10;
static const int IGTL_EMPTY_DATA_SIZE = -1;

const float vtkPlusOpenIGTLinkServer::CLIENT_SOCKET_TIMEOUT_SEC = 0.5;

//----------------------------------------------------------------------------
// If a frame cannot be retrieved from the device buffers (because it was overwritten by new frames)
// then we skip a SAMPLING_SKIPPING_MARGIN_SEC long period to allow the application to catch up.
// This time should be long enough to comfortably retrieve a frame from the buffer.
static const double SAMPLING_SKIPPING_MARGIN_SEC = 0.1;

vtkStandardNewMacro(vtkPlusOpenIGTLinkServer);

int vtkPlusOpenIGTLinkServer::ClientIdCounter = 1;

//----------------------------------------------------------------------------
vtkPlusOpenIGTLinkServer::vtkPlusOpenIGTLinkServer()
  : ServerSocket(igtl::ServerSocket::New())
  , TransformRepository(NULL)
  , DataCollector(NULL)
  , Threader(vtkSmartPointer<vtkMultiThreader>::New())
  , IGTLProtocolVersion(OpenIGTLink_PROTOCOL_VERSION)
  , ListeningPort(-1)
  , NumberOfRetryAttempts(10)
  , DelayBetweenRetryAttemptsSec(0.05)
  , MaxNumberOfIgtlMessagesToSend(100)
  , ConnectionActive(std::make_pair(false, false))
  , DataSenderActive(std::make_pair(false, false))
  , ConnectionReceiverThreadId(-1)
  , DataSenderThreadId(-1)
  , IgtlMessageFactory(vtkSmartPointer<vtkPlusIgtlMessageFactory>::New())
  , IgtlClientsMutex(vtkSmartPointer<vtkPlusRecursiveCriticalSection>::New())
  , LastSentTrackedFrameTimestamp(0)
  , MaxTimeSpentWithProcessingMs(50)
  , LastProcessingTimePerFrameMs(-1)
  , SendValidTransformsOnly(true)
  , DefaultClientSendTimeoutSec(CLIENT_SOCKET_TIMEOUT_SEC)
  , DefaultClientReceiveTimeoutSec(CLIENT_SOCKET_TIMEOUT_SEC)
  , IgtlMessageCrcCheckEnabled(0)
  , PlusCommandProcessor(vtkSmartPointer<vtkPlusCommandProcessor>::New())
  , MessageResponseQueueMutex(vtkSmartPointer<vtkPlusRecursiveCriticalSection>::New())
  , BroadcastChannel(NULL)
  , LogWarningOnNoDataAvailable(true)
  , KeepAliveIntervalSec(CLIENT_SOCKET_TIMEOUT_SEC / 2.0)
  , GracePeriodLogLevel(vtkPlusLogger::LOG_LEVEL_DEBUG)
  , MissingInputGracePeriodSec(0.0)
  , BroadcastStartTime(0.0)
  , MaxNumberOfStrays(0)
  , StrayReferenceFrame("Tracker")
{

}

//----------------------------------------------------------------------------
vtkPlusOpenIGTLinkServer::~vtkPlusOpenIGTLinkServer()
{
  this->Stop();
  this->SetTransformRepository(NULL);
  this->SetDataCollector(NULL);
  this->SetConfigFilename(NULL);
}

//----------------------------------------------------------------------------
PlusStatus vtkPlusOpenIGTLinkServer::QueueMessageResponseForClient(int clientId, igtl::MessageBase::Pointer message)
{
  bool found(false);
  {
    PlusLockGuard<vtkPlusRecursiveCriticalSection> igtlClientsMutexGuardedLock(this->IgtlClientsMutex);
    for (std::list<ClientData>::iterator clientIterator = this->IgtlClients.begin(); clientIterator != this->IgtlClients.end(); ++clientIterator)
    {
      if (clientIterator->ClientId == clientId)
      {
        found = true;
        break;
      }
    }
  }

  if (!found)
  {
    LOG_ERROR("Requested clientId " << clientId << " not found in list.");
    return PLUS_FAIL;
  }

  PlusLockGuard<vtkPlusRecursiveCriticalSection> mutexGuardedLock(this->MessageResponseQueueMutex);
  this->MessageResponseQueue[clientId].push_back(message);

  return PLUS_SUCCESS;
}

//----------------------------------------------------------------------------
void vtkPlusOpenIGTLinkServer::PrintSelf(ostream& os, vtkIndent indent)
{
  this->Superclass::PrintSelf(os, indent);
}

//----------------------------------------------------------------------------
PlusStatus vtkPlusOpenIGTLinkServer::StartOpenIGTLinkService()
{
  if (this->DataCollector == NULL)
  {
    LOG_WARNING("Tried to start OpenIGTLink server without a vtkPlusDataCollector");
    return PLUS_FAIL;
  }

  if (this->ConnectionReceiverThreadId < 0)
  {
    this->ConnectionActive.first = true;
    this->ConnectionReceiverThreadId = this->Threader->SpawnThread((vtkThreadFunctionType)&ConnectionReceiverThread, this);
  }

  if (this->DataSenderThreadId < 0)
  {
    this->DataSenderActive.first = true;
    this->DataSenderThreadId = this->Threader->SpawnThread((vtkThreadFunctionType)&DataSenderThread, this);
  }

  std::ostringstream ss;
  ss << "Data sent by default: ";
  this->DefaultClientInfo.PrintSelf(ss, vtkIndent(0));
  LOG_DEBUG(ss.str());

  this->PlusCommandProcessor->SetPlusServer(this);

  this->BroadcastStartTime = vtkPlusAccurateTimer::GetSystemTime();

  return PLUS_SUCCESS;
}

//----------------------------------------------------------------------------
PlusStatus vtkPlusOpenIGTLinkServer::StopOpenIGTLinkService()
{
  // Stop connection receiver thread
  if (this->ConnectionReceiverThreadId >= 0)
  {
    this->ConnectionActive.first = false;
    while (this->ConnectionActive.second)
    {
      // Wait until the thread stops
      vtkPlusAccurateTimer::DelayWithEventProcessing(0.2);
    }
    this->ConnectionReceiverThreadId = -1;
    LOG_DEBUG("ConnectionReceiverThread stopped");
  }

  // Disconnect clients (stop receiving thread, close socket)
  std::vector< int > clientIds;
  {
    // Get all the client ids and release the lock
    PlusLockGuard<vtkPlusRecursiveCriticalSection> igtlClientsMutexGuardedLock(this->IgtlClientsMutex);
    for (std::list<ClientData>::iterator clientIterator = this->IgtlClients.begin(); clientIterator != this->IgtlClients.end(); ++clientIterator)
    {
      clientIds.push_back(clientIterator->ClientId);
    }
  }
  for (std::vector< int >::iterator it = clientIds.begin(); it != clientIds.end(); ++it)
  {
    DisconnectClient(*it);
  }

  LOG_INFO("Plus OpenIGTLink server stopped.");

  return PLUS_SUCCESS;
}

//----------------------------------------------------------------------------
void* vtkPlusOpenIGTLinkServer::ConnectionReceiverThread(vtkMultiThreader::ThreadInfo* data)
{
  vtkPlusOpenIGTLinkServer* self = (vtkPlusOpenIGTLinkServer*)(data->UserData);

  int r = self->ServerSocket->CreateServer(self->ListeningPort);
  if (r < 0)
  {
    LOG_ERROR("Cannot create a server socket.");
    return NULL;
  }

  PrintServerInfo(self);

  self->ConnectionActive.second = true;

  // Wait for connections until we want to stop the thread
  while (self->ConnectionActive.first)
  {
    igtl::ClientSocket::Pointer newClientSocket = self->ServerSocket->WaitForConnection(CLIENT_SOCKET_TIMEOUT_SEC * 1000);
    if (newClientSocket.IsNotNull())
    {
      // Lock before we change the clients list
      PlusLockGuard<vtkPlusRecursiveCriticalSection> igtlClientsMutexGuardedLock(self->IgtlClientsMutex);
      ClientData newClient;
      self->IgtlClients.push_back(newClient);

      ClientData* client = &(self->IgtlClients.back());   // get a reference to the client data that is stored in the list
      client->ClientId = self->ClientIdCounter;
      self->ClientIdCounter++;
      client->ClientSocket = newClientSocket;
      client->ClientSocket->SetReceiveTimeout(self->DefaultClientReceiveTimeoutSec * 1000);
      client->ClientSocket->SetSendTimeout(self->DefaultClientSendTimeoutSec * 1000);
      client->ClientInfo = self->DefaultClientInfo;
      client->Server = self;

      int port = 0;
      std::string address = "unknown";
#if (OPENIGTLINK_VERSION_MAJOR > 1) || ( OPENIGTLINK_VERSION_MAJOR == 1 && OPENIGTLINK_VERSION_MINOR > 9 ) || ( OPENIGTLINK_VERSION_MAJOR == 1 && OPENIGTLINK_VERSION_MINOR == 9 && OPENIGTLINK_VERSION_PATCH > 4 )
      newClientSocket->GetSocketAddressAndPort(address, port);
#endif
      LOG_INFO("Received new client connection (client " << client->ClientId << " at " << address << ":" << port << "). Number of connected clients: " << self->GetNumberOfConnectedClients());

      client->DataReceiverActive.first = true;
      client->DataReceiverThreadId = self->Threader->SpawnThread((vtkThreadFunctionType)&DataReceiverThread, client);
    }
  }

  // Close server socket
  if (self->ServerSocket.IsNotNull())
  {
    self->ServerSocket->CloseSocket();
  }

  // Close thread
  self->ConnectionReceiverThreadId = -1;
  self->ConnectionActive.second = false;
  return NULL;
}

//----------------------------------------------------------------------------
void* vtkPlusOpenIGTLinkServer::DataSenderThread(vtkMultiThreader::ThreadInfo* data)
{
  vtkPlusOpenIGTLinkServer* self = (vtkPlusOpenIGTLinkServer*)(data->UserData);
  self->DataSenderActive.second = true;

  vtkPlusDevice* aDevice(NULL);
  vtkPlusChannel* aChannel(NULL);

  DeviceCollection aCollection;
  if (self->DataCollector->GetDevices(aCollection) != PLUS_SUCCESS || aCollection.size() == 0)
  {
    LOG_ERROR("Unable to retrieve devices. Check configuration and connection.");
    return NULL;
  }

  // Find the requested channel ID in all the devices
  for (DeviceCollectionIterator it = aCollection.begin(); it != aCollection.end(); ++it)
  {
    aDevice = *it;
    if (aDevice->GetOutputChannelByName(aChannel, self->GetOutputChannelId()) == PLUS_SUCCESS)
    {
      break;
    }
  }

  if (aChannel == NULL)
  {
    // The requested channel ID is not found
    if (!self->GetOutputChannelId().empty())
    {
      // the user explicitly requested a specific channel, but none was found by that name
      // this is an error
      LOG_ERROR("Unable to start data sending. OutputChannelId not found: " << self->GetOutputChannelId());
      return NULL;
    }
    // the user did not specify any channel, so just use the first channel that can be found in any device
    for (DeviceCollectionIterator it = aCollection.begin(); it != aCollection.end(); ++it)
    {
      aDevice = *it;
      if (aDevice->OutputChannelCount() > 0)
      {
        aChannel = *(aDevice->GetOutputChannelsStart());
        break;
      }
    }
  }

  // If we didn't find any channel then return
  if (aChannel == NULL)
  {
    LOG_WARNING("There are no channels to broadcast. Only command processing is available.");
  }

  self->BroadcastChannel = aChannel;
  if (self->BroadcastChannel)
  {
    self->BroadcastChannel->GetMostRecentTimestamp(self->LastSentTrackedFrameTimestamp);
  }

  double elapsedTimeSinceLastPacketSentSec = 0;
  while (self->ConnectionActive.first && self->DataSenderActive.first)
  {
    bool clientsConnected = false;
    {
      PlusLockGuard<vtkPlusRecursiveCriticalSection> igtlClientsMutexGuardedLock(self->IgtlClientsMutex);
      if (!self->IgtlClients.empty())
      {
        clientsConnected = true;
      }
    }
    if (!clientsConnected)
    {
      // No client connected, wait for a while
      vtkPlusAccurateTimer::Delay(0.2);
      self->LastSentTrackedFrameTimestamp = 0; // next time start sending from the most recent timestamp
      continue;
    }

    if (self->HasGracePeriodExpired())
    {
      self->GracePeriodLogLevel = vtkPlusLogger::LOG_LEVEL_WARNING;
    }

    SendMessageResponses(*self);

    // Send remote command execution replies to clients before sending any images/transforms/etc...
    SendCommandResponses(*self);

    // Send image/tracking/string data
    SendLatestFramesToClients(*self, elapsedTimeSinceLastPacketSentSec);
  }
  // Close thread
  self->DataSenderThreadId = -1;
  self->DataSenderActive.second = false;
  return NULL;
}

//----------------------------------------------------------------------------
PlusStatus vtkPlusOpenIGTLinkServer::SendLatestFramesToClients(vtkPlusOpenIGTLinkServer& self, double& elapsedTimeSinceLastPacketSentSec)
{
  vtkSmartPointer<vtkPlusTrackedFrameList> trackedFrameList = vtkSmartPointer<vtkPlusTrackedFrameList>::New();
  double startTimeSec = vtkPlusAccurateTimer::GetSystemTime();

  // Acquire tracked frames since last acquisition (minimum 1 frame)
  if (self.LastProcessingTimePerFrameMs < 1)
  {
    // if processing was less than 1ms/frame then assume it was 1ms (1000FPS processing speed) to avoid division by zero
    self.LastProcessingTimePerFrameMs = 1;
  }
  int numberOfFramesToGet = std::max(self.MaxTimeSpentWithProcessingMs / self.LastProcessingTimePerFrameMs, 1);
  // Maximize the number of frames to send
  numberOfFramesToGet = std::min(numberOfFramesToGet, self.MaxNumberOfIgtlMessagesToSend);

  if (self.BroadcastChannel != NULL)
  {
    if ((self.BroadcastChannel->HasVideoSource() && !self.BroadcastChannel->GetVideoDataAvailable())
        || (self.BroadcastChannel->ToolCount() > 0 && !self.BroadcastChannel->GetTrackingDataAvailable())
        || (self.BroadcastChannel->FieldCount() > 0 && !self.BroadcastChannel->GetFieldDataAvailable()))
    {
      if (self.LogWarningOnNoDataAvailable)
      {
        LOG_DYNAMIC("No data is broadcasted, as no data is available yet.", self.GracePeriodLogLevel);
      }
    }
    else
    {
      double oldestDataTimestamp = 0;
      if (self.BroadcastChannel->GetOldestTimestamp(oldestDataTimestamp) == PLUS_SUCCESS)
      {
        if (self.LastSentTrackedFrameTimestamp < oldestDataTimestamp)
        {
          LOG_INFO("OpenIGTLink broadcasting started. No data was available between " << self.LastSentTrackedFrameTimestamp << "-" << oldestDataTimestamp << "sec, therefore no data were broadcasted during this time period.");
          self.LastSentTrackedFrameTimestamp = oldestDataTimestamp + SAMPLING_SKIPPING_MARGIN_SEC;
        }
        if (self.BroadcastChannel->GetTrackedFrameList(self.LastSentTrackedFrameTimestamp, trackedFrameList, numberOfFramesToGet) != PLUS_SUCCESS)
        {
          LOG_ERROR("Failed to get tracked frame list from data collector (last recorded timestamp: " << std::fixed << self.LastSentTrackedFrameTimestamp);
          vtkPlusAccurateTimer::Delay(DELAY_ON_SENDING_ERROR_SEC);
        }
      }
    }
  }

  // There is no new frame in the buffer
  if (trackedFrameList->GetNumberOfTrackedFrames() == 0)
  {
    vtkPlusAccurateTimer::Delay(DELAY_ON_NO_NEW_FRAMES_SEC);
    elapsedTimeSinceLastPacketSentSec += vtkPlusAccurateTimer::GetSystemTime() - startTimeSec;

    // Send keep alive packet to clients
    if (elapsedTimeSinceLastPacketSentSec > self.KeepAliveIntervalSec)
    {
      self.KeepAlive();
      elapsedTimeSinceLastPacketSentSec = 0;
      return PLUS_SUCCESS;
    }

    return PLUS_FAIL;
  }

  for (unsigned int i = 0; i < trackedFrameList->GetNumberOfTrackedFrames(); ++i)
  {
    // Send tracked frame
    self.SendTrackedFrame(*trackedFrameList->GetTrackedFrame(i));
    elapsedTimeSinceLastPacketSentSec = 0;
  }

  // Compute time spent with processing one frame in this round
  double computationTimeMs = (vtkPlusAccurateTimer::GetSystemTime() - startTimeSec) * 1000.0;

  // Update last processing time if new tracked frames have been acquired
  if (trackedFrameList->GetNumberOfTrackedFrames() > 0)
  {
    self.LastProcessingTimePerFrameMs = computationTimeMs / trackedFrameList->GetNumberOfTrackedFrames();
  }
  return PLUS_SUCCESS;
}

//----------------------------------------------------------------------------
PlusStatus vtkPlusOpenIGTLinkServer::SendMessageResponses(vtkPlusOpenIGTLinkServer& self)
{
  PlusLockGuard<vtkPlusRecursiveCriticalSection> mutexGuardedLock(self.MessageResponseQueueMutex);
  if (!self.MessageResponseQueue.empty())
  {
    for (ClientIdToMessageListMap::iterator it = self.MessageResponseQueue.begin(); it != self.MessageResponseQueue.end(); ++it)
    {
      PlusLockGuard<vtkPlusRecursiveCriticalSection> igtlClientsMutexGuardedLock(self.IgtlClientsMutex);
      igtl::ClientSocket::Pointer clientSocket = NULL;

      for (std::list<ClientData>::iterator clientIterator = self.IgtlClients.begin(); clientIterator != self.IgtlClients.end(); ++clientIterator)
      {
        if (clientIterator->ClientId == it->first)
        {
          clientSocket = clientIterator->ClientSocket;
          break;
        }
      }
      if (clientSocket.IsNull())
      {
        LOG_WARNING("Message reply cannot be sent to client " << it->first << ", probably client has been disconnected.");
        continue;
      }

      for (std::vector<igtl::MessageBase::Pointer>::iterator messageIt = it->second.begin(); messageIt != it->second.end(); ++messageIt)
      {
        clientSocket->Send((*messageIt)->GetBufferPointer(), (*messageIt)->GetBufferSize());
      }
    }
    self.MessageResponseQueue.clear();
  }

  return PLUS_SUCCESS;
}

//----------------------------------------------------------------------------
PlusStatus vtkPlusOpenIGTLinkServer::SendCommandResponses(vtkPlusOpenIGTLinkServer& self)
{
  PlusCommandResponseList replies;
  self.PlusCommandProcessor->PopCommandResponses(replies);
  if (!replies.empty())
  {
    for (PlusCommandResponseList::iterator responseIt = replies.begin(); responseIt != replies.end(); responseIt++)
    {
      igtl::MessageBase::Pointer igtlResponseMessage = self.CreateIgtlMessageFromCommandResponse(*responseIt);
      if (igtlResponseMessage.IsNull())
      {
        LOG_ERROR("Failed to create OpenIGTLink message from command response");
        continue;
      }
      igtlResponseMessage->Pack();

      // Only send the response to the client that requested the command
      LOG_DEBUG("Send command reply to client " << (*responseIt)->GetClientId() << ": " << igtlResponseMessage->GetDeviceName());
      PlusLockGuard<vtkPlusRecursiveCriticalSection> igtlClientsMutexGuardedLock(self.IgtlClientsMutex);
      igtl::ClientSocket::Pointer clientSocket = NULL;
      for (std::list<ClientData>::iterator clientIterator = self.IgtlClients.begin(); clientIterator != self.IgtlClients.end(); ++clientIterator)
      {
        if (clientIterator->ClientId == (*responseIt)->GetClientId())
        {
          clientSocket = clientIterator->ClientSocket;
          break;
        }
      }

      if (clientSocket.IsNull())
      {
        LOG_WARNING("Message reply cannot be sent to client " << (*responseIt)->GetClientId() << ", probably client has been disconnected");
        continue;
      }
      clientSocket->Send(igtlResponseMessage->GetBufferPointer(), igtlResponseMessage->GetBufferSize());
    }
  }

  return PLUS_SUCCESS;
}

//----------------------------------------------------------------------------
void* vtkPlusOpenIGTLinkServer::DataReceiverThread(vtkMultiThreader::ThreadInfo* data)
{
  ClientData* client = (ClientData*)(data->UserData);
  client->DataReceiverActive.second = true;
  vtkPlusOpenIGTLinkServer* self = client->Server;

  /*! Store the IDs of recent commands to be able to detect duplicate command IDs */
  std::deque<uint32_t> previousCommandIds;

  // Make copy of frequently used data to avoid locking of client data
  igtl::ClientSocket::Pointer clientSocket = client->ClientSocket;
  int clientId = client->ClientId;

  igtl::MessageHeader::Pointer headerMsg = self->IgtlMessageFactory->CreateHeaderMessage(IGTL_HEADER_VERSION_1);

  while (client->DataReceiverActive.first)
  {
    headerMsg->InitBuffer();

    // Receive generic header from the socket
    int bytesReceived = clientSocket->Receive(headerMsg->GetPackPointer(), headerMsg->GetPackSize());
    if (bytesReceived == IGTL_EMPTY_DATA_SIZE || bytesReceived != headerMsg->GetPackSize())
    {
      vtkPlusAccurateTimer::Delay(0.1);
      continue;
    }

    headerMsg->Unpack(self->IgtlMessageCrcCheckEnabled);

    {
      PlusLockGuard<vtkPlusRecursiveCriticalSection> igtlClientsMutexGuardedLock(self->IgtlClientsMutex);
      client->ClientInfo.ClientHeaderVersion = std::min<int>(self->GetIGTLProtocolVersion(), headerMsg->GetHeaderVersion());
    }

    igtl::MessageBase::Pointer bodyMessage = self->IgtlMessageFactory->CreateReceiveMessage(headerMsg);
    if (bodyMessage.IsNull())
    {
      LOG_ERROR("Unable to receive message from client: " << client->ClientId);
      continue;
    }

    if (typeid(*bodyMessage) == typeid(igtl::PlusClientInfoMessage))
    {
      igtl::PlusClientInfoMessage::Pointer clientInfoMsg = dynamic_cast<igtl::PlusClientInfoMessage*>(bodyMessage.GetPointer());
      clientInfoMsg->SetMessageHeader(headerMsg);
      clientInfoMsg->AllocateBuffer();

      clientSocket->Receive(clientInfoMsg->GetPackBodyPointer(), clientInfoMsg->GetPackBodySize());

      int c = clientInfoMsg->Unpack(self->IgtlMessageCrcCheckEnabled);
      if (c & igtl::MessageHeader::UNPACK_BODY)
      {
        // Message received from client, need to lock to modify client info
        PlusLockGuard<vtkPlusRecursiveCriticalSection> igtlClientsMutexGuardedLock(self->IgtlClientsMutex);
        client->ClientInfo = clientInfoMsg->GetClientInfo();
        LOG_DEBUG("Client info message received from client " << clientId);
      }
    }
    else if (typeid(*bodyMessage) == typeid(igtl::GetStatusMessage))
    {
      // Just ping server, we can skip message and respond
      clientSocket->Skip(headerMsg->GetBodySizeToRead(), 0);

      igtl::StatusMessage::Pointer replyMsg = dynamic_cast<igtl::StatusMessage*>(bodyMessage.GetPointer());
      replyMsg->SetCode(igtl::StatusMessage::STATUS_OK);
      replyMsg->Pack();
      clientSocket->Send(replyMsg->GetPackPointer(), replyMsg->GetPackBodySize());
    }
    else if (typeid(*bodyMessage) == typeid(igtl::StringMessage)
             && vtkPlusCommand::IsCommandDeviceName(headerMsg->GetDeviceName()))
    {
      igtl::StringMessage::Pointer stringMsg = dynamic_cast<igtl::StringMessage*>(bodyMessage.GetPointer());
      stringMsg->SetMessageHeader(headerMsg);
      stringMsg->AllocateBuffer();
      clientSocket->Receive(stringMsg->GetPackBodyPointer(), stringMsg->GetPackBodySize());

      // We are receiving old style commands, handle it
      int c = stringMsg->Unpack(self->IgtlMessageCrcCheckEnabled);
      if (c & igtl::MessageHeader::UNPACK_BODY)
      {
        std::string deviceName(headerMsg->GetDeviceName());
        if (deviceName.empty())
        {
          self->PlusCommandProcessor->QueueStringResponse(PLUS_FAIL, std::string(vtkPlusCommand::DEVICE_NAME_REPLY), std::string("Unable to read DeviceName."));
          continue;
        }

        uint32_t uid(0);
        try
        {
#if (_MSC_VER == 1500)
          std::istringstream ss(vtkPlusCommand::GetUidFromCommandDeviceName(deviceName));
          ss >> uid;
#else
          uid = std::stoi(vtkPlusCommand::GetUidFromCommandDeviceName(deviceName));
#endif
        }
        catch (std::invalid_argument e)
        {
          LOG_ERROR("Unable to extract command UID from device name string.");
          // Removing support for malformed command strings, reply with error
          self->PlusCommandProcessor->QueueStringResponse(PLUS_FAIL, std::string(vtkPlusCommand::DEVICE_NAME_REPLY), std::string("Malformed DeviceName. Expected CMD_cmdId (ex: CMD_001)"));
          continue;
        }

        deviceName = vtkPlusCommand::GetPrefixFromCommandDeviceName(deviceName);

        if (std::find(previousCommandIds.begin(), previousCommandIds.end(), uid) != previousCommandIds.end())
        {
          // Command already exists
          LOG_WARNING("Already received a command with id = " << uid << " from client " << clientId << ". This repeated command will be ignored.");
          continue;
        }
        // New command, remember its ID
        previousCommandIds.push_back(uid);
        if (previousCommandIds.size() > NUMBER_OF_RECENT_COMMAND_IDS_STORED)
        {
          previousCommandIds.pop_front();
        }

        LOG_DEBUG("Received command from client " << clientId << ", device " << deviceName << " with UID " << uid << ": " << stringMsg->GetString());

        vtkSmartPointer<vtkXMLDataElement> cmdElement = vtkSmartPointer<vtkXMLDataElement>::Take(vtkXMLUtilities::ReadElementFromString(stringMsg->GetString()));
        std::string commandName = std::string(cmdElement->GetAttribute("Name") == NULL ? "" : cmdElement->GetAttribute("Name"));

        self->PlusCommandProcessor->QueueCommand(false, clientId, commandName, stringMsg->GetString(), deviceName, uid);
      }

    }
    else if (typeid(*bodyMessage) == typeid(igtl::CommandMessage))
    {
      igtl::CommandMessage::Pointer commandMsg = dynamic_cast<igtl::CommandMessage*>(bodyMessage.GetPointer());
      commandMsg->SetMessageHeader(headerMsg);
      commandMsg->AllocateBuffer();
      clientSocket->Receive(commandMsg->GetBufferBodyPointer(), commandMsg->GetBufferBodySize());

      int c = commandMsg->Unpack(self->IgtlMessageCrcCheckEnabled);
      if (c & igtl::MessageHeader::UNPACK_BODY)
      {
        std::string deviceName(headerMsg->GetDeviceName());

        uint32_t uid;
        uid = commandMsg->GetCommandId();

        if (std::find(previousCommandIds.begin(), previousCommandIds.end(), uid) != previousCommandIds.end())
        {
          // Command already exists
          LOG_WARNING("Already received a command with id = " << uid << " from client " << clientId << ". This repeated command will be ignored.");
          continue;
        }
        // New command, remember its ID
        previousCommandIds.push_back(uid);
        if (previousCommandIds.size() > NUMBER_OF_RECENT_COMMAND_IDS_STORED)
        {
          previousCommandIds.pop_front();
        }

        LOG_DEBUG("Received header version " << commandMsg->GetHeaderVersion() << " command " << commandMsg->GetCommandName()
                  << " from client " << clientId << ", device " << deviceName << " with UID " << uid << ": " << commandMsg->GetCommandContent());

        self->PlusCommandProcessor->QueueCommand(true, clientId, commandMsg->GetCommandName(), commandMsg->GetCommandContent(), deviceName, uid);
      }
      else
      {
        LOG_ERROR("STRING message unpacking failed for client " << clientId);
      }
    }
    else if (typeid(*bodyMessage) == typeid(igtl::StartTrackingDataMessage))
    {
      std::string deviceName("");

      igtl::StartTrackingDataMessage::Pointer startTracking = dynamic_cast<igtl::StartTrackingDataMessage*>(bodyMessage.GetPointer());
      startTracking->SetMessageHeader(headerMsg);
      startTracking->AllocateBuffer();

      clientSocket->Receive(startTracking->GetBufferBodyPointer(), startTracking->GetBufferBodySize());

      int c = startTracking->Unpack(self->IgtlMessageCrcCheckEnabled);
      if (c & igtl::MessageHeader::UNPACK_BODY)
      {
        client->ClientInfo.Resolution = startTracking->GetResolution();
        client->ClientInfo.TDATARequested = true;
      }
      else
      {
        LOG_ERROR("Client " << clientId << " STT_TDATA failed: could not retrieve startTracking message");
        return NULL;
      }

      igtl::MessageBase::Pointer msg = self->IgtlMessageFactory->CreateSendMessage("RTS_TDATA", IGTL_HEADER_VERSION_1);
      igtl::RTSTrackingDataMessage* rtsMsg = dynamic_cast<igtl::RTSTrackingDataMessage*>(msg.GetPointer());
      rtsMsg->SetStatus(0);
      rtsMsg->Pack();
      self->QueueMessageResponseForClient(client->ClientId, msg);
    }
    else if (typeid(*bodyMessage) == typeid(igtl::StopTrackingDataMessage))
    {
      igtl::StopTrackingDataMessage::Pointer stopTracking = dynamic_cast<igtl::StopTrackingDataMessage*>(bodyMessage.GetPointer());
      stopTracking->SetMessageHeader(headerMsg);
      stopTracking->AllocateBuffer();

      clientSocket->Receive(stopTracking->GetBufferBodyPointer(), stopTracking->GetBufferBodySize());

      client->ClientInfo.TDATARequested = false;
      igtl::MessageBase::Pointer msg = self->IgtlMessageFactory->CreateSendMessage("RTS_TDATA", IGTL_HEADER_VERSION_1);
      igtl::RTSTrackingDataMessage* rtsMsg = dynamic_cast<igtl::RTSTrackingDataMessage*>(msg.GetPointer());
      rtsMsg->SetStatus(0);
      rtsMsg->Pack();
      self->QueueMessageResponseForClient(client->ClientId, msg);
    }
    else if (typeid(*bodyMessage) == typeid(igtl::GetPolyDataMessage))
    {
      igtl::GetPolyDataMessage::Pointer polyDataMessage = dynamic_cast<igtl::GetPolyDataMessage*>(bodyMessage.GetPointer());
      polyDataMessage->SetMessageHeader(headerMsg);
      polyDataMessage->AllocateBuffer();

      clientSocket->Receive(polyDataMessage->GetBufferBodyPointer(), polyDataMessage->GetBufferBodySize());

      std::string fileName;
      // Check metadata for requisite parameters, if absent, check deviceName
      if (polyDataMessage->GetHeaderVersion() > IGTL_HEADER_VERSION_1)
      {
        if (!polyDataMessage->GetMetaDataElement("filename", fileName))
        {
          fileName = polyDataMessage->GetDeviceName();
          if (fileName.empty())
          {
            LOG_ERROR("GetPolyData message sent with no filename in either metadata or deviceName field.");
            continue;
          }
        }
      }
      else
      {
        fileName = polyDataMessage->GetDeviceName();
        if (fileName.empty())
        {
          LOG_ERROR("GetPolyData message sent with no filename in either metadata or deviceName field.");
          continue;
        }
      }

      vtkSmartPointer<vtkPolyDataReader> reader = vtkSmartPointer<vtkPolyDataReader>::New();
      reader->SetFileName(fileName.c_str());
      reader->Update();

      auto polyData = reader->GetOutput();
      if (polyData != nullptr)
      {
        igtl::MessageBase::Pointer msg = self->IgtlMessageFactory->CreateSendMessage("POLYDATA", polyDataMessage->GetHeaderVersion());
        igtl::PolyDataMessage* polyMsg = dynamic_cast<igtl::PolyDataMessage*>(msg.GetPointer());

        igtlio::PolyDataConverter::MessageContent content;
        content.deviceName = "PlusServer";
        content.polydata = polyData;
        igtlio::PolyDataConverter::VTKToIGTL(content, (igtl::PolyDataMessage::Pointer*)&msg);
        if (!msg->SetMetaDataElement("fileName", IANA_TYPE_US_ASCII, fileName))
        {
          LOG_ERROR("Filename too long to be sent back to client. Aborting.");
          continue;
        }
        self->QueueMessageResponseForClient(client->ClientId, msg);
        continue;
      }

      igtl::MessageBase::Pointer msg = self->IgtlMessageFactory->CreateSendMessage("RTS_POLYDATA", polyDataMessage->GetHeaderVersion());
      igtl::RTSPolyDataMessage* rtsPolyMsg = dynamic_cast<igtl::RTSPolyDataMessage*>(msg.GetPointer());
      rtsPolyMsg->SetStatus(false);
      self->QueueMessageResponseForClient(client->ClientId, rtsPolyMsg);
    }
    else if (typeid(*bodyMessage) == typeid(igtl::StatusMessage))
    {
      // status message is used as a keep-alive, don't do anything
      clientSocket->Skip(headerMsg->GetBodySizeToRead(), 0);
    }
    else
    {
      // if the device type is unknown, skip reading.
      LOG_WARNING("Unknown OpenIGTLink message is received from client " << clientId << ". Device type: " << headerMsg->GetMessageType()
                  << ". Device name: " << headerMsg->GetDeviceName() << ".");
      clientSocket->Skip(headerMsg->GetBodySizeToRead(), 0);
      continue;
    }
  } // ConnectionActive

  // Close thread
  client->DataReceiverThreadId = -1;
  client->DataReceiverActive.second = false;
  return NULL;
}

//----------------------------------------------------------------------------
PlusStatus vtkPlusOpenIGTLinkServer::SendTrackedFrame(PlusTrackedFrame& trackedFrame)
{
  int numberOfErrors = 0;

  // Update transform repository with the tracked frame
  if (this->TransformRepository != NULL)
  {
    if (this->TransformRepository->SetTransforms(trackedFrame) != PLUS_SUCCESS)
    {
      LOG_ERROR("Failed to set current transforms to transform repository");
      numberOfErrors++;
    }
  }

  // Convert relative timestamp to UTC
  double timestampSystem = trackedFrame.GetTimestamp(); // save original timestamp, we'll restore it later
  double timestampUniversal = vtkPlusAccurateTimer::GetUniversalTimeFromSystemTime(timestampSystem);
  trackedFrame.SetTimestamp(timestampUniversal);

  std::vector<int> disconnectedClientIds;
  {
    // Lock before we send message to the clients
    PlusLockGuard<vtkPlusRecursiveCriticalSection> igtlClientsMutexGuardedLock(this->IgtlClientsMutex);
    for (std::list<ClientData>::iterator clientIterator = this->IgtlClients.begin(); clientIterator != this->IgtlClients.end(); ++clientIterator)
    {
      igtl::ClientSocket::Pointer clientSocket = (*clientIterator).ClientSocket;

      // Create IGT messages
      std::vector<igtl::MessageBase::Pointer> igtlMessages;
      std::vector<igtl::MessageBase::Pointer>::iterator igtlMessageIterator;

      if (this->IgtlMessageFactory->PackMessages(clientIterator->ClientInfo, igtlMessages, trackedFrame, this->SendValidTransformsOnly, this->TransformRepository) != PLUS_SUCCESS)
      {
        LOG_WARNING("Failed to pack all IGT messages");
      }

      // Send all messages to a client
      for (igtlMessageIterator = igtlMessages.begin(); igtlMessageIterator != igtlMessages.end(); ++igtlMessageIterator)
      {
        igtl::MessageBase::Pointer igtlMessage = (*igtlMessageIterator);
        if (igtlMessage.IsNull())
        {
          continue;
        }

        int retValue = 0;
        RETRY_UNTIL_TRUE((retValue = clientSocket->Send(igtlMessage->GetBufferPointer(), igtlMessage->GetBufferSize())) != 0, this->NumberOfRetryAttempts, this->DelayBetweenRetryAttemptsSec);
        if (retValue == 0)
        {
          disconnectedClientIds.push_back(clientIterator->ClientId);
          igtl::TimeStamp::Pointer ts = igtl::TimeStamp::New();
          igtlMessage->GetTimeStamp(ts);
          LOG_INFO("Client disconnected - could not send " << igtlMessage->GetMessageType() << " message to client (device name: " << igtlMessage->GetDeviceName()
                   << "  Timestamp: " << std::fixed << ts->GetTimeStamp() << ").");
          break;
        }

        // Update the TDATA timestamp, even if TDATA isn't sent (cheaper than checking for existing TDATA message type)
        clientIterator->ClientInfo.LastTDATASentTimeStamp = trackedFrame.GetTimestamp();
      }
    }
  }

  // Clean up disconnected clients
  for (std::vector< int >::iterator it = disconnectedClientIds.begin(); it != disconnectedClientIds.end(); ++it)
  {
    DisconnectClient(*it);
  }

  // restore original timestamp
  trackedFrame.SetTimestamp(timestampSystem);

  return (numberOfErrors == 0 ? PLUS_SUCCESS : PLUS_FAIL);
}

//----------------------------------------------------------------------------
void vtkPlusOpenIGTLinkServer::DisconnectClient(int clientId)
{
  // Stop the client's data receiver thread
  {
    // Request thread stop
    PlusLockGuard<vtkPlusRecursiveCriticalSection> igtlClientsMutexGuardedLock(this->IgtlClientsMutex);
    for (std::list<ClientData>::iterator clientIterator = this->IgtlClients.begin(); clientIterator != this->IgtlClients.end(); ++clientIterator)
    {
      if (clientIterator->ClientId != clientId)
      {
        continue;
      }
      clientIterator->DataReceiverActive.first = false;
      break;
    }
  }

  // Wait for the thread to stop
  bool clientDataReceiverThreadStillActive = false;
  do
  {
    clientDataReceiverThreadStillActive = false;
    {
      // check if any of the receiver threads are still active
      PlusLockGuard<vtkPlusRecursiveCriticalSection> igtlClientsMutexGuardedLock(this->IgtlClientsMutex);
      for (std::list<ClientData>::iterator clientIterator = this->IgtlClients.begin(); clientIterator != this->IgtlClients.end(); ++clientIterator)
      {
        if (clientIterator->ClientId != clientId)
        {
          continue;
        }
        if (clientIterator->DataReceiverThreadId > 0)
        {
          if (clientIterator->DataReceiverActive.second)
          {
            // thread still running
            clientDataReceiverThreadStillActive = true;
          }
          else
          {
            // thread stopped
            clientIterator->DataReceiverThreadId = -1;
          }
          break;
        }
      }
    }
    if (clientDataReceiverThreadStillActive)
    {
      // give some time for the threads to finish
      vtkPlusAccurateTimer::DelayWithEventProcessing(0.2);
    }
  }
  while (clientDataReceiverThreadStillActive);

  // Close socket and remove client from the list
  int port = 0;
  std::string address = "unknown";
  {
    PlusLockGuard<vtkPlusRecursiveCriticalSection> igtlClientsMutexGuardedLock(this->IgtlClientsMutex);
    for (std::list<ClientData>::iterator clientIterator = this->IgtlClients.begin(); clientIterator != this->IgtlClients.end(); ++clientIterator)
    {
      if (clientIterator->ClientId != clientId)
      {
        continue;
      }
      if (clientIterator->ClientSocket.IsNotNull())
      {
#if (OPENIGTLINK_VERSION_MAJOR > 1) || ( OPENIGTLINK_VERSION_MAJOR == 1 && OPENIGTLINK_VERSION_MINOR > 9 ) || ( OPENIGTLINK_VERSION_MAJOR == 1 && OPENIGTLINK_VERSION_MINOR == 9 && OPENIGTLINK_VERSION_PATCH > 4 )
        clientIterator->ClientSocket->GetSocketAddressAndPort(address, port);
#endif
        clientIterator->ClientSocket->CloseSocket();
      }
      this->IgtlClients.erase(clientIterator);
      break;
    }
  }
  LOG_INFO("Client disconnected (" <<  address << ":" << port << "). Number of connected clients: " << GetNumberOfConnectedClients());
}

//----------------------------------------------------------------------------
void vtkPlusOpenIGTLinkServer::KeepAlive()
{
  LOG_TRACE("Keep alive packet sent to clients...");

  std::vector< int > disconnectedClientIds;

  {
    // Lock before we send message to the clients
    PlusLockGuard<vtkPlusRecursiveCriticalSection> igtlClientsMutexGuardedLock(this->IgtlClientsMutex);

    for (std::list<ClientData>::iterator clientIterator = this->IgtlClients.begin(); clientIterator != this->IgtlClients.end(); ++clientIterator)
    {
      igtl::StatusMessage::Pointer replyMsg = igtl::StatusMessage::New();
      replyMsg->SetCode(igtl::StatusMessage::STATUS_OK);
      replyMsg->Pack();

      int retValue = 0;
      RETRY_UNTIL_TRUE(
        (retValue = clientIterator->ClientSocket->Send(replyMsg->GetPackPointer(), replyMsg->GetPackSize())) != 0,
        this->NumberOfRetryAttempts, this->DelayBetweenRetryAttemptsSec);
      if (retValue == 0)
      {
        disconnectedClientIds.push_back(clientIterator->ClientId);
        igtl::TimeStamp::Pointer ts = igtl::TimeStamp::New();
        replyMsg->GetTimeStamp(ts);

        LOG_DEBUG("Client disconnected - could not send " << replyMsg->GetMessageType() << " message to client (device name: " << replyMsg->GetDeviceName()
                  << "  Timestamp: " << std::fixed <<  ts->GetTimeStamp() << ").");
      }
    } // clientIterator
  } // unlock client list

  // Clean up disconnected clients
  for (std::vector< int >::iterator it = disconnectedClientIds.begin(); it != disconnectedClientIds.end(); ++it)
  {
    DisconnectClient(*it);
  }
}

//------------------------------------------------------------------------------
unsigned int vtkPlusOpenIGTLinkServer::GetNumberOfConnectedClients() const
{
  // Lock before we send message to the clients
  PlusLockGuard<vtkPlusRecursiveCriticalSection> igtlClientsMutexGuardedLock(this->IgtlClientsMutex);
  return this->IgtlClients.size();
}

//------------------------------------------------------------------------------
PlusStatus vtkPlusOpenIGTLinkServer::GetClientInfo(unsigned int clientId, PlusIgtlClientInfo& outClientInfo) const
{
  PlusLockGuard<vtkPlusRecursiveCriticalSection> igtlClientsMutexGuardedLock(this->IgtlClientsMutex);
  for (std::list<ClientData>::const_iterator it = this->IgtlClients.begin(); it != this->IgtlClients.end(); ++it)
  {
    if (it->ClientId == clientId)
    {
      outClientInfo = it->ClientInfo;
      return PLUS_SUCCESS;
    }
  }

  return PLUS_FAIL;
}

//------------------------------------------------------------------------------
PlusStatus vtkPlusOpenIGTLinkServer::ReadConfiguration(vtkXMLDataElement* serverElement, const std::string& aFilename)
{
  LOG_TRACE("vtkPlusOpenIGTLinkServer::ReadConfiguration");

  if (aFilename.empty())
  {
    LOG_ERROR("Unable to configure PlusServer without an acceptable config file submitted.");
    return PLUS_FAIL;
  }
  this->SetConfigFilename(aFilename);

  XML_READ_SCALAR_ATTRIBUTE_REQUIRED(int, ListeningPort, serverElement);
  XML_READ_STRING_ATTRIBUTE_REQUIRED(OutputChannelId, serverElement);
  XML_READ_SCALAR_ATTRIBUTE_OPTIONAL(double, MissingInputGracePeriodSec, serverElement);
  XML_READ_SCALAR_ATTRIBUTE_OPTIONAL(double, MaxTimeSpentWithProcessingMs, serverElement);
  XML_READ_SCALAR_ATTRIBUTE_OPTIONAL(int, MaxNumberOfIgtlMessagesToSend, serverElement);
  XML_READ_SCALAR_ATTRIBUTE_OPTIONAL(int, NumberOfRetryAttempts, serverElement);
  XML_READ_SCALAR_ATTRIBUTE_OPTIONAL(double, DelayBetweenRetryAttemptsSec, serverElement);
  XML_READ_SCALAR_ATTRIBUTE_OPTIONAL(double, KeepAliveIntervalSec, serverElement);
  XML_READ_SCALAR_ATTRIBUTE_OPTIONAL(unsigned long, MaxNumberOfStrays, serverElement);
  XML_READ_STRING_ATTRIBUTE_OPTIONAL(StrayReferenceFrame, serverElement);
  XML_READ_BOOL_ATTRIBUTE_OPTIONAL(SendValidTransformsOnly, serverElement);
  XML_READ_BOOL_ATTRIBUTE_OPTIONAL(IgtlMessageCrcCheckEnabled, serverElement);
  XML_READ_BOOL_ATTRIBUTE_OPTIONAL(LogWarningOnNoDataAvailable, serverElement);

  this->DefaultClientInfo.IgtlMessageTypes.clear();
  this->DefaultClientInfo.TransformNames.clear();
  this->DefaultClientInfo.ImageStreams.clear();
  this->DefaultClientInfo.StringNames.clear();
  this->DefaultClientInfo.Resolution = 0;
  this->DefaultClientInfo.TDATARequested = false;

  vtkXMLDataElement* defaultClientInfo = serverElement->FindNestedElementWithName("DefaultClientInfo");
  if (defaultClientInfo != NULL)
  {
	// add transform names for desired number of stray markers
    if (this->MaxNumberOfStrays > 0)
    {
      vtkXMLDataElement* transformNames = defaultClientInfo->FindNestedElementWithName("TransformNames");
      if (transformNames != NULL)
      {
        std::string markerId;
        for (int i = 0; i < this->MaxNumberOfStrays; i++)
        {
          i < 9 ? markerId = "Stray0" : markerId = "Stray";
          markerId += std::to_string(i + 1);
          markerId += "To";
		  markerId += this->StrayReferenceFrame;
          vtkXMLDataElement* transformName = vtkXMLDataElement::New();
          transformName->SetName("Transform");
          transformName->SetAttribute("Name", markerId.c_str());
          transformNames->AddNestedElement(transformName);
        }
      }
    }
    if (this->DefaultClientInfo.SetClientInfoFromXmlData(defaultClientInfo) != PLUS_SUCCESS)
    {
      return PLUS_FAIL;
    }
  }

  XML_READ_SCALAR_ATTRIBUTE_OPTIONAL(float, DefaultClientSendTimeoutSec, serverElement);
  XML_READ_SCALAR_ATTRIBUTE_OPTIONAL(float, DefaultClientReceiveTimeoutSec, serverElement);

  // TODO : how come default client info isn't mandatory? send nothing?

  return PLUS_SUCCESS;
}

//------------------------------------------------------------------------------
int vtkPlusOpenIGTLinkServer::ProcessPendingCommands()
{
  return this->PlusCommandProcessor->ExecuteCommands();
}

//------------------------------------------------------------------------------
bool vtkPlusOpenIGTLinkServer::HasGracePeriodExpired()
{
  return (vtkPlusAccurateTimer::GetSystemTime() - this->BroadcastStartTime) > this->MissingInputGracePeriodSec;
}

//------------------------------------------------------------------------------
PlusStatus vtkPlusOpenIGTLinkServer::Start(vtkPlusDataCollector* dataCollector, vtkPlusTransformRepository* transformRepository, vtkXMLDataElement* serverElement, const std::string& configFilePath)
{
  if (serverElement == NULL)
  {
    LOG_ERROR("NULL configuration sent to vtkPlusOpenIGTLinkServer::Start. Unable to start PlusServer.");
    return PLUS_FAIL;
  }

  this->SetDataCollector(dataCollector);
  if (this->ReadConfiguration(serverElement, configFilePath.c_str()) != PLUS_SUCCESS)
  {
    LOG_ERROR("Failed to read PlusOpenIGTLinkServer configuration");
    return PLUS_FAIL;
  }

  this->SetTransformRepository(transformRepository);
  if (this->StartOpenIGTLinkService() != PLUS_SUCCESS)
  {
    LOG_ERROR("Failed to start Plus OpenIGTLink server");
    return PLUS_FAIL;
  }

  return PLUS_SUCCESS;
}

//------------------------------------------------------------------------------
PlusStatus vtkPlusOpenIGTLinkServer::Stop()
{
  PlusStatus status = PLUS_SUCCESS;

  if (StopOpenIGTLinkService() != PLUS_SUCCESS)
  {
    status = PLUS_FAIL;
  }

  SetDataCollector(NULL);

  SetTransformRepository(NULL);

  return status;
}

//------------------------------------------------------------------------------
igtl::MessageBase::Pointer vtkPlusOpenIGTLinkServer::CreateIgtlMessageFromCommandResponse(vtkPlusCommandResponse* response)
{
  vtkPlusCommandStringResponse* stringResponse = vtkPlusCommandStringResponse::SafeDownCast(response);
  if (stringResponse)
  {
    igtl::StringMessage::Pointer igtlMessage = dynamic_cast<igtl::StringMessage*>(this->IgtlMessageFactory->CreateSendMessage("STRING", IGTL_HEADER_VERSION_1).GetPointer());
    igtlMessage->SetDeviceName(stringResponse->GetDeviceName().c_str());
    igtlMessage->SetString(stringResponse->GetMessage());
    LOG_DEBUG("String response: " << stringResponse->GetMessage());
    return igtlMessage.GetPointer();
  }

  vtkPlusCommandImageResponse* imageResponse = vtkPlusCommandImageResponse::SafeDownCast(response);
  if (imageResponse)
  {
    std::string imageName = imageResponse->GetImageName();
    if (imageName.empty())
    {
      imageName = "PlusServerImage";
    }

    vtkSmartPointer<vtkMatrix4x4> imageToReferenceTransform = vtkSmartPointer<vtkMatrix4x4>::New();
    if (imageResponse->GetImageToReferenceTransform() != NULL)
    {
      imageToReferenceTransform = imageResponse->GetImageToReferenceTransform();
    }

    vtkImageData* imageData = imageResponse->GetImageData();
    if (imageData == NULL)
    {
      LOG_ERROR("Invalid image data in command response");
      return NULL;
    }

    igtl::ImageMessage::Pointer igtlMessage = dynamic_cast<igtl::ImageMessage*>(this->IgtlMessageFactory->CreateSendMessage("IMAGE", IGTL_HEADER_VERSION_1).GetPointer());
    igtlMessage->SetDeviceName(imageName.c_str());

    if (vtkPlusIgtlMessageCommon::PackImageMessage(igtlMessage, imageData,
        imageToReferenceTransform, vtkPlusAccurateTimer::GetSystemTime()) != PLUS_SUCCESS)
    {
      LOG_ERROR("Failed to create image mesage from command response");
      return NULL;
    }
    return igtlMessage.GetPointer();
  }

  vtkPlusCommandImageMetaDataResponse* imageMetaDataResponse = vtkPlusCommandImageMetaDataResponse::SafeDownCast(response);
  if (imageMetaDataResponse)
  {
    std::string imageMetaDataName = "PlusServerImageMetaData";
    PlusCommon::ImageMetaDataList imageMetaDataList;
    imageMetaDataResponse->GetImageMetaDataItems(imageMetaDataList);
    igtl::ImageMetaMessage::Pointer igtlMessage = dynamic_cast<igtl::ImageMetaMessage*>(this->IgtlMessageFactory->CreateSendMessage("IMGMETA", IGTL_HEADER_VERSION_1).GetPointer());
    igtlMessage->SetDeviceName(imageMetaDataName.c_str());
    if (vtkPlusIgtlMessageCommon::PackImageMetaMessage(igtlMessage, imageMetaDataList) != PLUS_SUCCESS)
    {
      LOG_ERROR("Failed to create image mesage from command response");
      return NULL;
    }
    return igtlMessage.GetPointer();
  }

  vtkPlusCommandCommandResponse* commandResponse = vtkPlusCommandCommandResponse::SafeDownCast(response);
  if (commandResponse)
  {
    if (!commandResponse->GetRespondWithCommandMessage())
    {
      // Incoming command was a v1/v2 style command, reply as such
      igtl::StringMessage::Pointer igtlMessage = dynamic_cast<igtl::StringMessage*>(this->IgtlMessageFactory->CreateSendMessage("STRING", IGTL_HEADER_VERSION_1).GetPointer());
      igtlMessage->SetDeviceName(vtkPlusCommand::GenerateReplyDeviceName(commandResponse->GetOriginalId()));

      std::ostringstream replyStr;
      replyStr << "<CommandReply";
      replyStr << " Status=\"" << (commandResponse->GetStatus() == PLUS_SUCCESS ? "SUCCESS" : "FAIL") << "\"";
      replyStr << " Message=\"";
      // Write to XML, encoding special characters, such as " ' \ < > &
      vtkXMLUtilities::EncodeString(commandResponse->GetResultString().c_str(), VTK_ENCODING_NONE, replyStr, VTK_ENCODING_NONE, 1 /* encode special characters */);
      replyStr << "\"";
      replyStr << " />";

      igtlMessage->SetString(replyStr.str().c_str());
      LOG_DEBUG("Command response: " << replyStr.str());
      return igtlMessage.GetPointer();
    }
    else
    {
      // Incoming command was a modern style command, reply using our latest
      igtl::RTSCommandMessage::Pointer igtlMessage = dynamic_cast<igtl::RTSCommandMessage*>(this->IgtlMessageFactory->CreateSendMessage("RTS_COMMAND", IGTL_HEADER_VERSION_2).GetPointer());
      //TODO : should this device name be the name of the server?
      igtlMessage->SetDeviceName(commandResponse->GetDeviceName().c_str());
      igtlMessage->SetCommandName(commandResponse->GetCommandName());
      igtlMessage->SetCommandId(commandResponse->GetOriginalId());

      std::ostringstream replyStr;
      replyStr << "<Command><Result>" << (commandResponse->GetStatus() ? "true" : "false") << "</Result>";
      if (commandResponse->GetStatus() == PLUS_FAIL)
      {
        replyStr << "<Error>" << commandResponse->GetErrorString() << "</Error>";
      }
      replyStr << "<Message>" << commandResponse->GetResultString() << "</Message></Command>";

      for (std::map<std::string, std::string>::const_iterator it = commandResponse->GetParameters().begin(); it != commandResponse->GetParameters().end(); ++it)
      {
        igtlMessage->SetMetaDataElement(it->first, IANA_TYPE_US_ASCII, it->second);
      }

      LOG_DEBUG("Command response: " << replyStr.str());
      igtlMessage->SetCommandContent(replyStr.str());

      return igtlMessage.GetPointer();
    }
  }

  LOG_ERROR("vtkPlusOpenIGTLinkServer::CreateIgtlMessageFromCommandResponse failed: invalid command response");
  return NULL;
}
