#include "NativeFeatureIncludes.h"
#if _VENET_SUPPORT_UDPProxyServer==1 && _VENET_SUPPORT_UDPForwarder==1

#include "UDPProxyServer.h"
#include "BitStream.h"
#include "UDPProxyCommon.h"
#include "VEPeerInterface.h"
#include "MessageIdentifiers.h"

using namespace VENet;

STATIC_FACTORY_DEFINITIONS(UDPProxyServer,UDPProxyServer);

UDPProxyServer::UDPProxyServer()
{
    resultHandler=0;
    socketFamily=AF_INET;
}
UDPProxyServer::~UDPProxyServer()
{

}
void UDPProxyServer::SetSocketFamily(unsigned short _socketFamily)
{
    socketFamily=_socketFamily;
}
void UDPProxyServer::SetResultHandler(UDPProxyServerResultHandler *rh)
{
    resultHandler=rh;
}
bool UDPProxyServer::LoginToCoordinator(VENet::VEString password, SystemAddress coordinatorAddress)
{
    unsigned int insertionIndex;
    bool objectExists;
    insertionIndex=loggingInCoordinators.GetIndexFromKey(coordinatorAddress,&objectExists);
    if (objectExists==true)
        return false;
    loggedInCoordinators.GetIndexFromKey(coordinatorAddress,&objectExists);
    if (objectExists==true)
        return false;
    VENet::BitStream outgoingBs;
    outgoingBs.Write((MessageID)ID_UDP_PROXY_GENERAL);
    outgoingBs.Write((MessageID)ID_UDP_PROXY_LOGIN_REQUEST_FROM_SERVER_TO_COORDINATOR);
    outgoingBs.Write(password);
    vePeerInterface->Send(&outgoingBs, MEDIUM_PRIORITY, RELIABLE_ORDERED, 0, coordinatorAddress, false);
    loggingInCoordinators.InsertAtIndex(coordinatorAddress, insertionIndex, _FILE_AND_LINE_ );
    return true;
}
void UDPProxyServer::SetServerPublicIP(VEString ip)
{
    serverPublicIp = ip;
}
void UDPProxyServer::Update(void)
{
}
PluginReceiveResult UDPProxyServer::OnReceive(Packet *packet)
{
    if (packet->data[0]==ID_UDP_PROXY_GENERAL && packet->length>1)
    {
        bool objectExists;

        switch (packet->data[1])
        {
        case ID_UDP_PROXY_FORWARDING_REQUEST_FROM_COORDINATOR_TO_SERVER:
            if (loggedInCoordinators.GetIndexFromKey(packet->systemAddress, &objectExists)!=(unsigned int)-1)
            {
                OnForwardingRequestFromCoordinatorToServer(packet);
                return RR_STOP_PROCESSING_AND_DEALLOCATE;
            }
            break;
        case ID_UDP_PROXY_NO_PASSWORD_SET_FROM_COORDINATOR_TO_SERVER:
        case ID_UDP_PROXY_WRONG_PASSWORD_FROM_COORDINATOR_TO_SERVER:
        case ID_UDP_PROXY_ALREADY_LOGGED_IN_FROM_COORDINATOR_TO_SERVER:
        case ID_UDP_PROXY_LOGIN_SUCCESS_FROM_COORDINATOR_TO_SERVER:
        {
            unsigned int removalIndex = loggingInCoordinators.GetIndexFromKey(packet->systemAddress, &objectExists);
            if (objectExists)
            {
                loggingInCoordinators.RemoveAtIndex(removalIndex);

                VENet::BitStream incomingBs(packet->data, packet->length, false);
                incomingBs.IgnoreBytes(2);
                VENet::VEString password;
                incomingBs.Read(password);
                switch (packet->data[1])
                {
                case ID_UDP_PROXY_NO_PASSWORD_SET_FROM_COORDINATOR_TO_SERVER:
                    if (resultHandler)
                        resultHandler->OnNoPasswordSet(password, this);
                    break;
                case ID_UDP_PROXY_WRONG_PASSWORD_FROM_COORDINATOR_TO_SERVER:
                    if (resultHandler)
                        resultHandler->OnWrongPassword(password, this);
                    break;
                case ID_UDP_PROXY_ALREADY_LOGGED_IN_FROM_COORDINATOR_TO_SERVER:
                    if (resultHandler)
                        resultHandler->OnAlreadyLoggedIn(password, this);
                    break;
                case ID_UDP_PROXY_LOGIN_SUCCESS_FROM_COORDINATOR_TO_SERVER:
                    // RakAssert(loggedInCoordinators.GetIndexOf(packet->systemAddress)==(unsigned int)-1);
                    loggedInCoordinators.Insert(packet->systemAddress, packet->systemAddress, true, _FILE_AND_LINE_);
                    if (resultHandler)
                        resultHandler->OnLoginSuccess(password, this);
                    break;
                }
            }


            return RR_STOP_PROCESSING_AND_DEALLOCATE;
        }
        }
    }
    return RR_CONTINUE_PROCESSING;
}
void UDPProxyServer::OnClosedConnection(const SystemAddress &systemAddress, VENetGUID VENetGuid, PI2_LostConnectionReason lostConnectionReason )
{
    (void) lostConnectionReason;
    (void) VENetGuid;

    loggingInCoordinators.RemoveIfExists(systemAddress);
    loggedInCoordinators.RemoveIfExists(systemAddress);
}
void UDPProxyServer::OnRakPeerStartup(void)
{
    udpForwarder.Startup();
}
void UDPProxyServer::OnRakPeerShutdown(void)
{
    udpForwarder.Shutdown();
    loggingInCoordinators.Clear(true,_FILE_AND_LINE_);
    loggedInCoordinators.Clear(true,_FILE_AND_LINE_);
}
void UDPProxyServer::OnAttach(void)
{
    if (vePeerInterface->IsActive())
        OnRakPeerStartup();
}
void UDPProxyServer::OnDetach(void)
{
    OnRakPeerShutdown();
}
void UDPProxyServer::OnForwardingRequestFromCoordinatorToServer(Packet *packet)
{
    SystemAddress sourceAddress, targetAddress;
    VENet::BitStream incomingBs(packet->data, packet->length, false);
    incomingBs.IgnoreBytes(2);
    incomingBs.Read(sourceAddress);
    incomingBs.Read(targetAddress);
    VENet::TimeMS timeoutOnNoDataMS;
    incomingBs.Read(timeoutOnNoDataMS);
    VEAssert(timeoutOnNoDataMS > 0 && timeoutOnNoDataMS <= UDP_FORWARDER_MAXIMUM_TIMEOUT);

    unsigned short forwardingPort=0;
    UDPForwarderResult success = udpForwarder.StartForwarding(sourceAddress, targetAddress, timeoutOnNoDataMS, 0, socketFamily, &forwardingPort, 0);
    VENet::BitStream outgoingBs;
    outgoingBs.Write((MessageID)ID_UDP_PROXY_GENERAL);
    outgoingBs.Write((MessageID)ID_UDP_PROXY_FORWARDING_REPLY_FROM_SERVER_TO_COORDINATOR);
    outgoingBs.Write(sourceAddress);
    outgoingBs.Write(targetAddress);
    outgoingBs.Write(serverPublicIp);
    outgoingBs.Write((unsigned char) success);
    outgoingBs.Write(forwardingPort);
    vePeerInterface->Send(&outgoingBs, MEDIUM_PRIORITY, RELIABLE_ORDERED, 0, packet->systemAddress, false);
}

#endif