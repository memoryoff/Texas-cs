#include "SocketLayer.h"
#include "VEAssert.h"
#include "VENetTypes.h"
#include "VEPeer.h"
#include "GetTime.h"
#include "LinuxStrings.h"
#include "SocketDefines.h"

using namespace VENet;

#if defined(__native_client__)
using namespace pp;
#endif

#if USE_SLIDING_WINDOW_CONGESTION_CONTROL!=1
#include "CCVENetUDT.h"
#else
#include "CCVENetSlidingWindow.h"
#endif

SocketLayerOverride *SocketLayer::slo=0;

#ifdef _WIN32
#else
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#if !defined(ANDROID)
#include <ifaddrs.h>
#endif
#include <netinet/in.h>
#include <net/if.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>

#endif

#if   defined(_WIN32)
#include "WSAStartupSingleton.h"
#include <ws2tcpip.h>
#elif defined SN_TARGET_PSP2
#else
#include <unistd.h>
#endif

#include "VESleep.h"
#include <stdio.h>
#include "Itoa.h"

#ifdef _MSC_VER
#pragma warning( push )
#endif

namespace VENet
{
extern void ProcessNetworkPacket( const SystemAddress systemAddress, const char *data, const int length, VEPeer *rakPeer, VENet::TimeUS timeRead );
extern void ProcessNetworkPacket( const SystemAddress systemAddress, const char *data, const int length, VEPeer *rakPeer, VENetSmartPtr<VENetSocket> VENetSocket, VENet::TimeUS timeRead );
}

#ifdef _DEBUG
#include <stdio.h>
#endif

#ifdef __native_client__

namespace VENet
{
struct SocketImpl
{
    SocketImpl();
    ~SocketImpl();

    PP_Resource s;

    bool sendInProgress;

    char nextSendBuffer[MAXIMUM_MTU_SIZE];

    int nextSendSize;

    PP_NetAddress_Private nextSendAddr;
};

SocketImpl::SocketImpl()
{
    s = 0;
    sendInProgress = false;
    nextSendSize = 0;
}

SocketImpl::~SocketImpl()
{
    if (s != 0)
        ((PPB_UDPSocket_Private_0_3*) pp::Module::Get()->GetBrowserInterface(PPB_UDPSOCKET_PRIVATE_INTERFACE_0_3))->Close(s);
}

void CloseSocket(SOCKET s)
{
    VENet::OP_DELETE(s, _FILE_AND_LINE_);
}

}

#endif


#if VENET_SUPPORT_IPV6==1
void PrepareAddrInfoHints(addrinfo *hints)
{
    memset(hints, 0, sizeof (addrinfo));
    hints->ai_socktype = SOCK_DGRAM;
    hints->ai_flags = AI_PASSIVE;
}
#endif


bool SocketLayer::IsPortInUse_Old(unsigned short port, const char *hostAddress)
{
#ifdef __native_client__
    return false;
#else
    SOCKET listenSocket;
    sockaddr_in listenerSocketAddress;
    memset(&listenerSocketAddress,0,sizeof(sockaddr_in));
    listenerSocketAddress.sin_port = htons( port );



    listenSocket = socket__( AF_INET, SOCK_DGRAM, 0 );

    if ( listenSocket == (SOCKET) -1 )
        return true;
    listenerSocketAddress.sin_family = AF_INET;

    if ( hostAddress && hostAddress[0] )
    {
        listenerSocketAddress.sin_addr.s_addr = inet_addr__( hostAddress );
    }
    else
        listenerSocketAddress.sin_addr.s_addr = INADDR_ANY;

    int ret = bind__( listenSocket, ( struct sockaddr * ) & listenerSocketAddress, sizeof( listenerSocketAddress ) );
    closesocket__(listenSocket);

    return ret <= -1;

#endif
}
bool SocketLayer::IsSocketFamilySupported(const char *hostAddress, unsigned short socketFamily)
{
    (void) hostAddress;
    (void) socketFamily;

#if VENET_SUPPORT_IPV6!=1
    return socketFamily==AF_INET;
#else
    struct addrinfo hints;
#if VENET_SUPPORT_IPV6==1
    PrepareAddrInfoHints(&hints);
#endif
    hints.ai_family = socketFamily;
    struct addrinfo *servinfo=0;
    int error;
    if (hostAddress &&
            (_stricmp(hostAddress,"UNASSIGNED_SYSTEM_ADDRESS")==0 || hostAddress[0]==0))
    {
        getaddrinfo(0, "0", &hints, &servinfo);
    }
    else
    {
        getaddrinfo(hostAddress, "0", &hints, &servinfo);
    }


    (void) error;
    if (servinfo)
    {
        freeaddrinfo(servinfo);
        return true;
    }
    else
    {
#if (defined(__GNUC__) || defined(__GCCXML__)) && !defined(_WIN32)
        printf("IsSocketFamilySupported failed. hostAddress=%s. %s\n", hostAddress, gai_strerror(error));
#endif
    }
    return false;
#endif
}
bool SocketLayer::IsPortInUse(unsigned short port, const char *hostAddress, unsigned short socketFamily)
{
#if VENET_SUPPORT_IPV6!=1
    (void) socketFamily;
    return IsPortInUse_Old(port, hostAddress);
#else
    SOCKET listenSocket;
    struct addrinfo hints;
    struct addrinfo *servinfo=0, *aip;
    PrepareAddrInfoHints(&hints);
    hints.ai_family = socketFamily;
    char portStr[32];
    Itoa(port,portStr,10);

    if (hostAddress &&
            (_stricmp(hostAddress,"UNASSIGNED_SYSTEM_ADDRESS")==0 || hostAddress[0]==0))
    {
        getaddrinfo(0, portStr, &hints, &servinfo);
    }
    else
    {
        getaddrinfo(hostAddress, portStr, &hints, &servinfo);
    }

    for (aip = servinfo; aip != NULL; aip = aip->ai_next)
    {
        listenSocket = socket__(aip->ai_family, aip->ai_socktype, aip->ai_protocol);
        if (listenSocket != -1)
        {
            int ret = bind__( listenSocket, aip->ai_addr, (int) aip->ai_addrlen );
            closesocket__(listenSocket);
            if (ret>=0)
            {
                freeaddrinfo(servinfo);
                return false;
            }
        }

        if (hostAddress==0 || hostAddress[0]==0)
            break;
    }

    freeaddrinfo(servinfo);
    return true;
#endif
}
void SocketLayer::SetDoNotFragment( SOCKET listenSocket, int opt, int IPPROTO )
{
#if defined(IP_DONTFRAGMENT )

#if defined(_WIN32) &&  defined(_DEBUG)
    VEAssert(IP_DONTFRAGMENT==14);
#endif

    if ( setsockopt__( listenSocket, IPPROTO, IP_DONTFRAGMENT, ( char * ) & opt, sizeof ( opt ) ) == -1 )
    {
#if defined(_WIN32) && defined(_DEBUG)
        DWORD dwIOError = GetLastError();
        LPVOID messageBuffer;
        FormatMessage( FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                       NULL, dwIOError, MAKELANGID( LANG_NEUTRAL, SUBLANG_DEFAULT ),
                       ( LPTSTR ) & messageBuffer, 0, NULL );
        VENET_DEBUG_PRINTF( "Warning: setsockopt__(IP_DONTFRAGMENT) failed:Error code - %d\n%s", dwIOError, messageBuffer );
        LocalFree( messageBuffer );
#endif
    }
#endif
}

void SocketLayer::SetNonBlocking( SOCKET listenSocket)
{
#ifdef __native_client__
#elif defined(_WIN32)
    unsigned long nonBlocking = 1;
    ioctlsocket__( listenSocket, FIONBIO, &nonBlocking );



#else
    int flags = fcntl(listenSocket, F_GETFL, 0);
    fcntl(listenSocket, F_SETFL, flags | O_NONBLOCK);
#endif
}

void SocketLayer::SetSocketOptions( SOCKET listenSocket, bool blockingSocket)
{
#ifdef __native_client__
    (void) listenSocket;
#else
    int sock_opt = 1;

    sock_opt=1024*256;
    setsockopt__(listenSocket, SOL_SOCKET, SO_RCVBUF, ( char * ) & sock_opt, sizeof ( sock_opt ) );

    sock_opt=0;
    setsockopt__(listenSocket, SOL_SOCKET, SO_LINGER, ( char * ) & sock_opt, sizeof ( sock_opt ) );

    sock_opt=1024*16;
    setsockopt__(listenSocket, SOL_SOCKET, SO_SNDBUF, ( char * ) & sock_opt, sizeof ( sock_opt ) );


    if (blockingSocket==false)
    {
#ifdef _WIN32
        unsigned long nonblocking = 1;
        ioctlsocket__( listenSocket, FIONBIO, &nonblocking );



#else
    fcntl( listenSocket, F_SETFL, O_NONBLOCK );
#endif
    }

    sock_opt=1;
    if ( setsockopt__( listenSocket, SOL_SOCKET, SO_BROADCAST, ( char * ) & sock_opt, sizeof( sock_opt ) ) == -1 )
    {
#if defined(_WIN32) && defined(_DEBUG)

        DWORD dwIOError = GetLastError();
        LPVOID messageBuffer;
        FormatMessage( FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                       NULL, dwIOError, MAKELANGID( LANG_NEUTRAL, SUBLANG_DEFAULT ),  // Default language
                       ( LPTSTR ) & messageBuffer, 0, NULL );
        VENET_DEBUG_PRINTF( "setsockopt__(SO_BROADCAST) failed:Error code - %d\n%s", dwIOError, messageBuffer );
        LocalFree( messageBuffer );

#endif

    }

#endif
}
SOCKET SocketLayer::CreateBoundSocket_PS3Lobby( unsigned short port, bool blockingSocket, const char *forceHostAddress, unsigned short socketFamily )
{
    (void) port;
    (void) blockingSocket;
    (void) forceHostAddress;
    (void) socketFamily;
    return INVALID_SOCKET;
}
SOCKET SocketLayer::CreateBoundSocket_PSP2( unsigned short port, bool blockingSocket, const char *forceHostAddress, unsigned short socketFamily )
{
    (void) port;
    (void) blockingSocket;
    (void) forceHostAddress;
    (void) socketFamily;
    return 0;
}
#ifdef __native_client__
struct SocketAndBuffer
{
    VEPeer *peer;
    SOCKET chromeSocket;
    char buffer[MAXIMUM_MTU_SIZE];
    int32_t dataSize;
    SystemAddress recvFromAddress;
    VENet::TimeUS timeRead;
    SocketAndBuffer *next;
} *sabHead=0, *sabLast=0;
void onRecvFrom(void* pData, int32_t dataSize);
void ChromeRecvFrom(SocketAndBuffer *sab)
{
    PP_CompletionCallback cc = PP_MakeCompletionCallback(onRecvFrom, sab);
    ((PPB_UDPSocket_Private_0_3*) Module::Get()->GetBrowserInterface(PPB_UDPSOCKET_PRIVATE_INTERFACE_0_3))->RecvFrom(sab->chromeSocket->s, sab->buffer, MAXIMUM_MTU_SIZE, cc);
}

void onSendTo(void* pData, int32_t dataSize)
{
    if (dataSize <= 0)
        VENET_DEBUG_PRINTF("onSendTo: send failed with error %d\n", dataSize);

    if (dataSize == PP_ERROR_ABORTED)
        return;

    SocketImpl *s = (SocketImpl *)pData;
    if (s->nextSendSize > 0)
    {
        int bufSize = s->nextSendSize;
        s->nextSendSize = 0;
        PP_CompletionCallback cc = PP_MakeCompletionCallback(onSendTo, s);
        ((PPB_UDPSocket_Private_0_3*) Module::Get()->GetBrowserInterface(PPB_UDPSOCKET_PRIVATE_INTERFACE_0_3))->SendTo(s->s, s->nextSendBuffer, bufSize, &s->nextSendAddr, cc);
    }
    else
    {
        s->sendInProgress = false;
    }
}
void onRecvFrom(void* pData, int32_t dataSize)
{
    SocketAndBuffer *sab = (SocketAndBuffer *) pData;

    if (dataSize <=0 || !pData )
    {
        if (dataSize==PP_ERROR_ABORTED)
        {
            VENET_DEBUG_PRINTF("onRecvFrom error PP_ERROR_ABORTED, killing recvfrom loop\n");
            VENet::OP_DELETE(sab, _FILE_AND_LINE_);
        }
        else
        {
            VENET_DEBUG_PRINTF("onRecvFrom error %d\n", dataSize);

            PP_CompletionCallback cc = PP_MakeCompletionCallback(onRecvFrom, sab);
            ((PPB_UDPSocket_Private_0_3*) Module::Get()->GetBrowserInterface(PPB_UDPSOCKET_PRIVATE_INTERFACE_0_3))->RecvFrom(sab->chromeSocket->s, sab->buffer, MAXIMUM_MTU_SIZE, cc);
        }

        return;
    }

    sab->dataSize=dataSize;
    sab->timeRead=VENet::GetTimeUS();
    PP_NetAddress_Private addr;
    bool ok=false;
    if (((PPB_UDPSocket_Private_0_3*) Module::Get()->GetBrowserInterface(PPB_UDPSOCKET_PRIVATE_INTERFACE_0_3))->GetRecvFromAddress(sab->chromeSocket->s, &addr) == PP_TRUE)
    {
        PP_NetAddressFamily_Private family = NetAddressPrivate::GetFamily(addr);
        if (family == PP_NETADDRESSFAMILY_IPV4)
        {
            ok = NetAddressPrivate::GetAddress(addr, &sab->recvFromAddress.address.addr4.sin_addr, sizeof(in_addr));
        }
#if VENET_SUPPORT_IPV6==1
        else
        {
            ok = NetAddressPrivate::GetAddress(addr, &sab->recvFromAddress.address.addr6.sin6_addr, sizeof(in6_addr));
        }
#endif
    }

    if (ok)
    {
        sab->recvFromAddress.SetPort(pp::NetAddressPrivate::GetPort(addr));

        if (sab->peer)
        {
            sab->peer->ProcessChromePacket(sab->chromeSocket, sab->buffer, sab->dataSize, sab->recvFromAddress, sab->timeRead);
        }
        else
        {
            if (sabHead==0)
                sabHead=sab;
            else
                sabLast->next=sab;
            sabLast=sab;
        }
    }

    ChromeRecvFrom(sab);
}
struct ChromeSocketContainer
{
    VEPeer *peer;
    SOCKET s;
};
void onSocketBound(void* pData, int32_t dataSize)
{
    VENET_DEBUG_PRINTF("onSocketBound ==> %d\n", dataSize);

    if (dataSize < 0)
    {
        fprintf(stderr,"onSocketBound exiting, dataSize = %d\n", dataSize);
        return;
    }

    ChromeSocketContainer *csc = (ChromeSocketContainer *)pData;
    SocketAndBuffer *sab = VENet::OP_NEW<SocketAndBuffer>(_FILE_AND_LINE_);
    sab->peer=csc->peer;
    sab->chromeSocket=csc->s;
    sab->next=0;
    ChromeRecvFrom(sab);
    VENet::OP_DELETE(csc, _FILE_AND_LINE_);
}
SOCKET CreateChromeSocket(VEPeer *peer, unsigned short port, const char *forceHostAddress, _PP_Instance_ chromeInstance, bool is_ipv6)
{
    if (Module::Get()->GetBrowserInterface(PPB_UDPSOCKET_PRIVATE_INTERFACE_0_3))
    {
        SOCKET s = VENet::OP_NEW<SocketImpl>(_FILE_AND_LINE_);
        s->s = ((PPB_UDPSocket_Private_0_3*) Module::Get()->GetBrowserInterface(PPB_UDPSOCKET_PRIVATE_INTERFACE_0_3))->Create(chromeInstance);
        VENET_DEBUG_PRINTF("CreateChromeSocket(%d,%s,0x%08x,%d) ==> 0x%08x\n", port, forceHostAddress?forceHostAddress:"(null)",chromeInstance,is_ipv6, s->s);
        PP_NetAddress_Private client_addr;
        uint8_t ipv6[16], ipv4[4];
        if (forceHostAddress)
        {
            unsigned int ipIdx=0;
            char * pch;
            pch = strtok ((char*) forceHostAddress,".");
            if (is_ipv6)
            {
                while (pch != NULL && ipIdx<16)
                {
                    ipv6[ipIdx++]=atoi(pch);
                    pch = strtok (NULL, ".");
                }
                NetAddressPrivate::CreateFromIPv6Address(ipv6,0,port,&client_addr);
            }
            else
            {
                while (pch != NULL && ipIdx<4)
                {
                    ipv4[ipIdx++]=atoi(pch);
                    pch = strtok (NULL, ".");
                }
                NetAddressPrivate::CreateFromIPv4Address(ipv4,port,&client_addr);
            }
        }
        else
        {
            NetAddressPrivate::GetAnyAddress(is_ipv6, &client_addr);
            NetAddressPrivate::ReplacePort(client_addr, port, &client_addr);
        }
        ChromeSocketContainer *csc = VENet::OP_NEW<ChromeSocketContainer>(_FILE_AND_LINE_);
        csc->peer=peer;
        csc->s=s;
        VENET_DEBUG_PRINTF("attempting to bind to %s\n", NetAddressPrivate::Describe(client_addr, true).c_str());
        PP_CompletionCallback cc = PP_MakeCompletionCallback(onSocketBound, csc);
        ((PPB_UDPSocket_Private_0_3*) Module::Get()->GetBrowserInterface(PPB_UDPSOCKET_PRIVATE_INTERFACE_0_3))->Bind(s->s, &client_addr, cc);
        return s;
    }
    return INVALID_SOCKET;
}
#endif

SOCKET SocketLayer::CreateBoundSocket_Old( VEPeer *peer, unsigned short port, bool blockingSocket, const char *forceHostAddress, unsigned int sleepOn10048, unsigned int extraSocketOptions, _PP_Instance_ chromeInstance )
{
#ifdef __native_client__
    return CreateChromeSocket(peer,port,forceHostAddress,chromeInstance,false);
#else
    (void) peer;
    (void) chromeInstance;

    int ret;
    SOCKET listenSocket;
    sockaddr_in listenerSocketAddress;
    memset(&listenerSocketAddress,0,sizeof(sockaddr_in));
    listenerSocketAddress.sin_port = htons( port );

    listenSocket = socket__( AF_INET, SOCK_DGRAM, extraSocketOptions );

    if ( listenSocket == (SOCKET) -1 )
    {
#if defined(_WIN32) &&  defined(_DEBUG)
        DWORD dwIOError = GetLastError();
        LPVOID messageBuffer;
        FormatMessage( FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                       NULL, dwIOError, MAKELANGID( LANG_NEUTRAL, SUBLANG_DEFAULT ),  // Default language
                       ( LPTSTR ) & messageBuffer, 0, NULL );
        VENET_DEBUG_PRINTF( "socket__(...) failed:Error code - %d\n%s", dwIOError, messageBuffer );
        char str[512];
        strcpy(str,(const char*) messageBuffer);
        LocalFree( messageBuffer );
#endif

        return (SOCKET) -1;
    }

    SetSocketOptions(listenSocket, blockingSocket);

    listenerSocketAddress.sin_family = AF_INET;

    if (forceHostAddress && forceHostAddress[0])
    {
        listenerSocketAddress.sin_addr.s_addr = inet_addr__( forceHostAddress );
    }
    else
    {
        listenerSocketAddress.sin_addr.s_addr = INADDR_ANY;
    }

    ret = bind__( listenSocket, ( struct sockaddr * ) & listenerSocketAddress, sizeof( listenerSocketAddress ) );

    if ( ret <= -1 )
    {
#if defined(_WIN32)
        DWORD dwIOError = GetLastError();
        if (dwIOError==10048)
        {
            if (sleepOn10048==0)
                return (SOCKET) -1;
            VESleep(100);

            closesocket__(listenSocket);
            listenerSocketAddress.sin_port = htons( port );
            listenSocket = socket__( AF_INET, SOCK_DGRAM, 0 );
            if ( listenSocket == (SOCKET) -1 )
                return false;
            SetSocketOptions(listenSocket, blockingSocket);

            listenerSocketAddress.sin_family = AF_INET;
            if (forceHostAddress && forceHostAddress[0])
            {
                listenerSocketAddress.sin_addr.s_addr = inet_addr__( forceHostAddress );
            }
            else
                listenerSocketAddress.sin_addr.s_addr = INADDR_ANY;

            ret = bind__( listenSocket, ( struct sockaddr * ) & listenerSocketAddress, sizeof( listenerSocketAddress ) );

            if ( ret >= 0 )
                return listenSocket;
        }
        dwIOError = GetLastError();
        LPVOID messageBuffer;
        FormatMessage( FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                       NULL, dwIOError, MAKELANGID( LANG_NEUTRAL, SUBLANG_DEFAULT ),  // Default language
                       ( LPTSTR ) & messageBuffer, 0, NULL );
        VENET_DEBUG_PRINTF( "bind__(...) failed:Error code - %d\n%s", (unsigned int) dwIOError, (char*) messageBuffer );
        LocalFree( messageBuffer );
#elif (defined(__GNUC__) || defined(__GCCXML__)  ) && !defined(_WIN32)
    switch (ret)
    {
    case EBADF:
        VENET_DEBUG_PRINTF("bind__(): sockfd is not a valid descriptor.\n");
        break;

    case ENOTSOCK:
        VENET_DEBUG_PRINTF("bind__(): Argument is a descriptor for a file, not a socket.\n");
        break;

    case EINVAL:
        VENET_DEBUG_PRINTF("bind__(): The addrlen is wrong, or the socket was not in the AF_UNIX family.\n");
        break;
    case EROFS:
        VENET_DEBUG_PRINTF("bind__(): The socket inode would reside on a read-only file system.\n");
        break;
    case EFAULT:
        VENET_DEBUG_PRINTF("bind__(): my_addr points outside the user's accessible address space.\n");
        break;
    case ENAMETOOLONG:
        VENET_DEBUG_PRINTF("bind__(): my_addr is too long.\n");
        break;
    case ENOENT:
        VENET_DEBUG_PRINTF("bind__(): The file does not exist.\n");
        break;
    case ENOMEM:
        VENET_DEBUG_PRINTF("bind__(): Insufficient kernel memory was available.\n");
        break;
    case ENOTDIR:
        VENET_DEBUG_PRINTF("bind__(): A component of the path prefix is not a directory.\n");
        break;
    case EACCES:
        VENET_DEBUG_PRINTF("bind__(): Search permission is denied on a component of the path prefix.\n");
        break;

    case ELOOP:
        VENET_DEBUG_PRINTF("bind__(): Too many symbolic links were encountered in resolving my_addr.\n");
        break;

    default:
        VENET_DEBUG_PRINTF("Unknown bind__() error %i.\n", ret);
        break;
    }
#endif

        return (SOCKET) -1;
    }

    return listenSocket;


#endif
}
SOCKET SocketLayer::CreateBoundSocket( VEPeer *peer, unsigned short port, bool blockingSocket, const char *forceHostAddress, unsigned int sleepOn10048, unsigned int extraSocketOptions, unsigned short socketFamily, _PP_Instance_ chromeInstance )
{
    (void) peer;
    (void) blockingSocket;
    (void) extraSocketOptions;
    (void) socketFamily;
    (void) chromeInstance;

#if VENET_SUPPORT_IPV6!=1
    return CreateBoundSocket_Old(peer,port,blockingSocket,forceHostAddress,sleepOn10048,extraSocketOptions, chromeInstance);
#else

#ifdef __native_client__
    return CreateChromeSocket(peer,port,forceHostAddress,chromeInstance,true);
#else
    (void) blockingSocket;
#endif


#ifdef _WIN32
    if (sleepOn10048==0)
        VESleep(100);
#endif

    int ret=0;
    SOCKET listenSocket;
    struct addrinfo hints;
    struct addrinfo *servinfo=0, *aip;
    PrepareAddrInfoHints(&hints);
    hints.ai_family=socketFamily;
    char portStr[32];
    Itoa(port,portStr,10);

    if (forceHostAddress &&
            (_stricmp(forceHostAddress,"UNASSIGNED_SYSTEM_ADDRESS")==0 || forceHostAddress[0]==0))
    {
        getaddrinfo(0, portStr, &hints, &servinfo);
    }
    else
    {
        getaddrinfo(forceHostAddress, portStr, &hints, &servinfo);
    }

    for (aip = servinfo; aip != NULL; aip = aip->ai_next)
    {
        listenSocket = socket__(aip->ai_family, aip->ai_socktype, aip->ai_protocol);
        if (listenSocket != -1)
        {
            ret = bind__( listenSocket, aip->ai_addr, (int) aip->ai_addrlen );
            if (ret>=0)
            {
                sockaddr_in6 addr6;
                memcpy(&addr6, aip->ai_addr, sizeof(addr6));

                freeaddrinfo(servinfo);

                SetSocketOptions(listenSocket, blockingSocket);
                return listenSocket;
            }
        }
    }

#if defined(_WIN32)
    DWORD dwIOError = GetLastError();
    LPVOID messageBuffer;
    FormatMessage( FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   NULL, dwIOError, MAKELANGID( LANG_NEUTRAL, SUBLANG_DEFAULT ),  // Default language
                   ( LPTSTR ) & messageBuffer, 0, NULL );

    VENET_DEBUG_PRINTF( "bind__(...) failed:Error code - %d\n%s", (unsigned int) dwIOError, (char*) messageBuffer );
    LocalFree( messageBuffer );
#elif defined(__GNUC__)  || defined(__GCCXML__) && !defined(_WIN32)
    switch (ret)
    {
    case EBADF:
        VENET_DEBUG_PRINTF("bind__(): sockfd is not a valid descriptor.\n");
        break;

    case ENOTSOCK:
        VENET_DEBUG_PRINTF("bind__(): Argument is a descriptor for a file, not a socket.\n");
        break;

    case EINVAL:
        VENET_DEBUG_PRINTF("bind__(): The addrlen is wrong, or the socket was not in the AF_UNIX family.\n");
        break;
    case EROFS:
        VENET_DEBUG_PRINTF("bind__(): The socket inode would reside on a read-only file system.\n");
        break;
    case EFAULT:
        VENET_DEBUG_PRINTF("bind__(): my_addr points outside the user's accessible address space.\n");
        break;
    case ENAMETOOLONG:
        VENET_DEBUG_PRINTF("bind__(): my_addr is too long.\n");
        break;
    case ENOENT:
        VENET_DEBUG_PRINTF("bind__(): The file does not exist.\n");
        break;
    case ENOMEM:
        VENET_DEBUG_PRINTF("bind__(): Insufficient kernel memory was available.\n");
        break;
    case ENOTDIR:
        VENET_DEBUG_PRINTF("bind__(): A component of the path prefix is not a directory.\n");
        break;
    case EACCES:
        VENET_DEBUG_PRINTF("bind__(): Search permission is denied on a component of the path prefix.\n");
        break;

    case ELOOP:
        VENET_DEBUG_PRINTF("bind__(): Too many symbolic links were encountered in resolving my_addr.\n");
        break;

    default:
        VENET_DEBUG_PRINTF("Unknown bind__() error %i.\n", ret);
        break;
    }
#endif


#endif

    return INVALID_SOCKET;
}
const char* SocketLayer::DomainNameToIP_Old( const char *domainName )
{
    static struct in_addr addr;
    memset(&addr,0,sizeof(in_addr));
    struct hostent * phe = gethostbyname( domainName );

    if ( phe == 0 || phe->h_addr_list[ 0 ] == 0 )
    {
        return 0;
    }

    if (phe->h_addr_list[ 0 ]==0)
        return 0;

    memcpy( &addr, phe->h_addr_list[ 0 ], sizeof( struct in_addr ) );
    return inet_ntoa( addr );


    return "";
}
const char* SocketLayer::DomainNameToIP( const char *domainName )
{
#if VENET_SUPPORT_IPV6!=1
    return DomainNameToIP_Old(domainName);
#else
    struct addrinfo hints, *res, *p;
    int status;
    static char ipstr[INET6_ADDRSTRLEN];
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC; // AF_INET or AF_INET6 to force version
    hints.ai_socktype = SOCK_DGRAM;

    if ((status = getaddrinfo(domainName, NULL, &hints, &res)) != 0)
    {
        return 0;
    }

    p=res;
    void *addr;

    if (p->ai_family == AF_INET)
    {
        struct sockaddr_in *ipv4 = (struct sockaddr_in *)p->ai_addr;
        addr = &(ipv4->sin_addr);
        strcpy(ipstr, inet_ntoa( ipv4->sin_addr ));
    }
    else
    {
        struct sockaddr_in6 *ipv6 = (struct sockaddr_in6 *)p->ai_addr;
        addr = &(ipv6->sin6_addr);
        getnameinfo((struct sockaddr *)ipv6, sizeof(struct sockaddr_in6), ipstr, 1, NULL, 0, NI_NUMERICHOST);
    }
    freeaddrinfo(res);
    return ipstr;

    return "";

#endif
}


void SocketLayer::Write( const SOCKET writeSocket, const char* data, const int length )
{
#ifdef __native_client__
#else

#ifdef _DEBUG
    VEAssert( writeSocket != (SOCKET) -1 );
#endif

    send__( writeSocket, data, length, 0 );
#endif
}
void SocketLayer::RecvFromBlocking_Old( const SOCKET s, VEPeer *rakPeer, unsigned short remotePortVENetWasStartedOn_PS3, unsigned int extraSocketOptions, char *dataOut, int *bytesReadOut, SystemAddress *systemAddressOut, VENet::TimeUS *timeRead )
{
    (void) rakPeer;
    (void) remotePortVENetWasStartedOn_PS3;
    (void) extraSocketOptions;

#ifndef __native_client__

    sockaddr* sockAddrPtr;
    socklen_t sockLen;
    socklen_t* socketlenPtr=(socklen_t*) &sockLen;
    sockaddr_in sa;
    memset(&sa,0,sizeof(sockaddr_in));
    int dataOutSize;
    const int flag=0;
    {
        sockLen=sizeof(sa);
        sa.sin_family = AF_INET;
        sa.sin_port=0;
        sockAddrPtr=(sockaddr*) &sa;
    }
    dataOutSize=MAXIMUM_MTU_SIZE;

    *bytesReadOut = recvfrom__( s, dataOut, dataOutSize, flag, sockAddrPtr, socketlenPtr );

    if (*bytesReadOut<=0)
        return;
    *timeRead=VENet::GetTimeUS();
    {
        systemAddressOut->SetPortNetworkOrder( sa.sin_port );
        systemAddressOut->address.addr4.sin_addr.s_addr=sa.sin_addr.s_addr;
    }

#endif
}

void SocketLayer::RecvFromBlocking( const SOCKET s, VEPeer *rakPeer, unsigned short remotePortVENetWasStartedOn_PS3, unsigned int extraSocketOptions, char *dataOut, int *bytesReadOut, SystemAddress *systemAddressOut, VENet::TimeUS *timeRead )
{
#ifdef __native_client__
    if (sabHead)
    {
        *bytesReadOut = sabHead->dataSize;
        memcpy(dataOut, sabHead->buffer, sabHead->dataSize);
        *systemAddressOut=sabHead->recvFromAddress;
        *timeRead = sabHead->timeRead;
        SocketAndBuffer *sabNext=sabHead->next;
        VENet::OP_DELETE(sabHead, _FILE_AND_LINE_);
        sabHead=sabNext;
    }
    else
    {
        *bytesReadOut=0;
    }
    return;
#endif

#if VENET_SUPPORT_IPV6!=1
    RecvFromBlocking_Old(s,rakPeer,remotePortVENetWasStartedOn_PS3,extraSocketOptions,dataOut,bytesReadOut,systemAddressOut,timeRead);
#else
    (void) rakPeer;
    sockaddr_storage their_addr;
    sockaddr* sockAddrPtr;
    socklen_t sockLen;
    socklen_t* socketlenPtr=(socklen_t*) &sockLen;
    memset(&their_addr,0,sizeof(their_addr));
    int dataOutSize;
    const int flag=0;

    (void) remotePortVENetWasStartedOn_PS3;
    (void) extraSocketOptions;
    {
        sockLen=sizeof(their_addr);
        sockAddrPtr=(sockaddr*) &their_addr;
    }
    dataOutSize=MAXIMUM_MTU_SIZE;
    *bytesReadOut = recvfrom__( s, dataOut, dataOutSize, flag, sockAddrPtr, socketlenPtr );

    if (*bytesReadOut<=0)
        return;
    *timeRead=VENet::GetTimeUS();
    {
        if (their_addr.ss_family==AF_INET)
        {
            memcpy(&systemAddressOut->address.addr4,(sockaddr_in *)&their_addr,sizeof(sockaddr_in));
            systemAddressOut->debugPort=ntohs(systemAddressOut->address.addr4.sin_port);
        }
        else
        {
            memcpy(&systemAddressOut->address.addr6,(sockaddr_in6 *)&their_addr,sizeof(sockaddr_in6));
            systemAddressOut->debugPort=ntohs(systemAddressOut->address.addr6.sin6_port);
        }
    }

#endif
}

int SocketLayer::SendTo_PS3Lobby( SOCKET s, const char *data, int length, const SystemAddress &systemAddress, unsigned short remotePortVENetWasStartedOn_PS3 )
{
    (void) s;
    (void) data;
    (void) length;
    (void) remotePortVENetWasStartedOn_PS3;
    (void) systemAddress;

    int len=0;
    return len;
}
int SocketLayer::SendTo_PSP2( SOCKET s, const char *data, int length, const SystemAddress &systemAddress, unsigned short remotePortVENetWasStartedOn_PS3 )
{
    (void) s;
    (void) data;
    (void) length;
    (void) remotePortVENetWasStartedOn_PS3;
    (void) systemAddress;

    int len=0;

    return len;
}
int SocketLayer::SendTo_360( SOCKET s, const char *data, int length, const char *voiceData, int voiceLength, const SystemAddress &systemAddress, unsigned int extraSocketOptions )
{
    (void) s;
    (void) data;
    (void) length;
    (void) voiceData;
    (void) voiceLength;
    (void) extraSocketOptions;
    (void) systemAddress;

    int len=0;
    return len;
}
int SocketLayer::SendTo_PC( SOCKET s, const char *data, int length, const SystemAddress &systemAddress, const char *file, const long line )
{
#ifdef __native_client__
    PP_NetAddress_Private client_addr;
#if VENET_SUPPORT_IPV6==1
    NetAddressPrivate::CreateFromIPv6Address(systemAddress.address.addr6.sin6_addr.u.Byte,0,systemAddress.GetPort(),&client_addr);
#else
    NetAddressPrivate::CreateFromIPv4Address((const uint8_t*) &systemAddress.address.addr4.sin_addr,systemAddress.GetPort(),&client_addr);
#endif
    if (!s->sendInProgress)
    {
        s->sendInProgress = true;
        PP_CompletionCallback cc = PP_MakeCompletionCallback(onSendTo, s);
        return ((PPB_UDPSocket_Private_0_3*) Module::Get()->GetBrowserInterface(PPB_UDPSOCKET_PRIVATE_INTERFACE_0_3))->SendTo(s->s, data, length, &client_addr, cc);
    }
    else if (s->nextSendSize == 0)
    {
        VEAssert(length <= MAXIMUM_MTU_SIZE);
        memcpy(s->nextSendBuffer, data, length);
        s->nextSendSize = length;
        s->nextSendAddr = client_addr;
        return 0;
    }
    else
    {
        return -1;
    }
#else
    int len=0;
    do
    {
        (void) file;
        (void) line;
#endif
    if (systemAddress.address.addr4.sin_family==AF_INET)
    {
        len = sendto__( s, data, length, 0, ( const sockaddr* ) & systemAddress.address.addr4, sizeof( sockaddr_in ) );
    }
    else
    {
#if VENET_SUPPORT_IPV6==1
        len = sendto__( s, data, length, 0, ( const sockaddr* ) & systemAddress.address.addr6, sizeof( sockaddr_in6 ) );
#endif
    }
    if (len<0)
    {
        VENET_DEBUG_PRINTF("sendto failed with code %i for char %i and length %i.\n", len, data[0], length);
    }
}
while ( len == 0 );
return len;

}

#ifdef _MSC_VER
#pragma warning( disable : 4702 )
#endif
int SocketLayer::SendTo( SOCKET s, const char *data, int length, SystemAddress &systemAddress, unsigned short remotePortVENetWasStartedOn_PS3, unsigned int extraSocketOptions, const char *file, const long line )
{
    (void) extraSocketOptions;

    int len=0;
    VEAssert(length<=MAXIMUM_MTU_SIZE-UDP_HEADER_SIZE);
#if   !defined(__S3E__)
    VEAssert(systemAddress.address.addr4.sin_family!=AF_MAX);
#endif

    if (slo)
    {
        len = slo->VENetSendTo(s,data,length,systemAddress);
        if ( len != -1 )
            return 0;
        return 1;
    }

    if ( s == INVALID_SOCKET )
    {
        return -1;
    }


    if (remotePortVENetWasStartedOn_PS3!=0)
    {
    }
    else
    {
        len = SendTo_PC(s,data,length,systemAddress,file,line);
    }

    if ( len != -1 )
        return 0;

#if defined(_WIN32) && !defined(_WIN32_WCE)

    DWORD dwIOError = WSAGetLastError();

    if ( dwIOError == WSAECONNRESET )
    {
#if defined(_DEBUG)
        VENET_DEBUG_PRINTF( "A previous send operation resulted in an ICMP Port Unreachable message.\n" );
#endif

    }
    else if ( dwIOError != WSAEWOULDBLOCK && dwIOError != WSAEADDRNOTAVAIL)
    {
#if defined(_WIN32) &&  defined(_DEBUG)
        LPVOID messageBuffer;
        FormatMessage( FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                       NULL, dwIOError, MAKELANGID( LANG_NEUTRAL, SUBLANG_DEFAULT ),  // Default language
                       ( LPTSTR ) & messageBuffer, 0, NULL );
        VENET_DEBUG_PRINTF( "sendto failed:Error code - %d\n%s", dwIOError, messageBuffer );

        LocalFree( messageBuffer );
#endif

    }

    return dwIOError;
#endif

    return 1;
}
int SocketLayer::SendToTTL( SOCKET s, const char *data, int length, SystemAddress &systemAddress, int ttl )
{
    if (slo)
        return slo->VENetSendTo(s,data,length,systemAddress);

#if   !defined(__native_client__)
    int oldTTL;
    socklen_t opLen=sizeof(oldTTL);
    if (getsockopt__(s, systemAddress.GetIPPROTO(), IP_TTL, ( char * ) & oldTTL, &opLen ) == -1)
    {
#if defined(_WIN32) && defined(_DEBUG)
        DWORD dwIOError = GetLastError();
        LPVOID messageBuffer;
        FormatMessage( FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                       NULL, dwIOError, MAKELANGID( LANG_NEUTRAL, SUBLANG_DEFAULT ),  // Default language
                       ( LPTSTR ) & messageBuffer, 0, NULL );
        VENET_DEBUG_PRINTF( "getsockopt__(IPPROTO_IP,IP_TTL) failed:Error code - %d\n%s", dwIOError, messageBuffer );
        LocalFree( messageBuffer );
#endif
    }

    int newTTL=ttl;
    if (setsockopt__(s, systemAddress.GetIPPROTO(), IP_TTL, ( char * ) & newTTL, sizeof ( newTTL ) ) == -1)
    {

#if defined(_WIN32) && defined(_DEBUG)
        DWORD dwIOError = GetLastError();
        LPVOID messageBuffer;
        FormatMessage( FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                       NULL, dwIOError, MAKELANGID( LANG_NEUTRAL, SUBLANG_DEFAULT ),  // Default language
                       ( LPTSTR ) & messageBuffer, 0, NULL );
        VENET_DEBUG_PRINTF( "setsockopt__(IPPROTO_IP,IP_TTL) failed:Error code - %d\n%s", dwIOError, messageBuffer );
        LocalFree( messageBuffer );
#endif
    }
    int res = SendTo(s,data,length,systemAddress,0,0, __FILE__, __LINE__ );

    setsockopt__(s, systemAddress.GetIPPROTO(), IP_TTL, ( char * ) & oldTTL, opLen );

    return res;
#else
    return 0;
#endif
}


VENet::VEString SocketLayer::GetSubNetForSocketAndIp(SOCKET inSock, VENet::VEString inIpString)
{
    VENet::VEString netMaskString;
    VENet::VEString ipString;
#if   defined(_WIN32)
    INTERFACE_INFO InterfaceList[20];
    unsigned long nBytesReturned;
    if (WSAIoctl(inSock, SIO_GET_INTERFACE_LIST, 0, 0, &InterfaceList,
                 sizeof(InterfaceList), &nBytesReturned, 0, 0) == SOCKET_ERROR)
    {
        return "";
    }

    int nNumInterfaces = nBytesReturned / sizeof(INTERFACE_INFO);

    for (int i = 0; i < nNumInterfaces; ++i)
    {
        sockaddr_in *pAddress;
        pAddress = (sockaddr_in *) & (InterfaceList[i].iiAddress);
        ipString=inet_ntoa(pAddress->sin_addr);

        if (inIpString==ipString)
        {
            pAddress = (sockaddr_in *) & (InterfaceList[i].iiNetmask);
            netMaskString=inet_ntoa(pAddress->sin_addr);
            return netMaskString;
        }
    }
    return "";
#else

    int fd,fd2;
    fd2 = socket__(AF_INET, SOCK_DGRAM, 0);

    if (fd2 < 0)
    {
        return "";
    }

    struct ifconf ifc;
    char          buf[1999];
    ifc.ifc_len = sizeof(buf);
    ifc.ifc_buf = buf;
    if (ioctl(fd2, SIOCGIFCONF, &ifc) < 0)
    {
        return "";
    }

    struct ifreq *ifr;
    ifr         = ifc.ifc_req;
    int intNum = ifc.ifc_len / sizeof(struct ifreq);
    for (int i = 0; i < intNum; i++)
    {
        ipString=inet_ntoa(((struct sockaddr_in *)&ifr[i].ifr_addr)->sin_addr);

        if (inIpString==ipString)
        {
            struct ifreq ifr2;
            fd = socket__(AF_INET, SOCK_DGRAM, 0);
            if (fd < 0)
            {
                return "";
            }
            ifr2.ifr_addr.sa_family = AF_INET;

            strncpy(ifr2.ifr_name, ifr[i].ifr_name, IFNAMSIZ-1);

            ioctl(fd, SIOCGIFNETMASK, &ifr2);

            close(fd);
            close(fd2);
            netMaskString=inet_ntoa(((struct sockaddr_in *)&ifr2.ifr_addr)->sin_addr);

            return netMaskString;
        }
    }

    close(fd2);
    return "";

#endif

}

void GetMyIP_Win32( SystemAddress addresses[MAXIMUM_NUMBER_OF_INTERNAL_IDS] )
{
    int idx=0;
    idx=0;
    char ac[ 80 ];
    if ( gethostname( ac, sizeof( ac ) ) == -1 )
    {
#if defined(_WIN32)
        DWORD dwIOError = GetLastError();
        LPVOID messageBuffer;
        FormatMessage( FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                       NULL, dwIOError, MAKELANGID( LANG_NEUTRAL, SUBLANG_DEFAULT ),  // Default language
                       ( LPTSTR ) & messageBuffer, 0, NULL );
        VENET_DEBUG_PRINTF( "gethostname failed:Error code - %d\n%s", dwIOError, messageBuffer );
        LocalFree( messageBuffer );
#endif
        return ;
    }


#if VENET_SUPPORT_IPV6==1
    struct addrinfo hints;
    struct addrinfo *servinfo=0, *aip;
    PrepareAddrInfoHints(&hints);
    getaddrinfo(ac, "", &hints, &servinfo);

    for (idx=0, aip = servinfo; aip != NULL && idx < MAXIMUM_NUMBER_OF_INTERNAL_IDS; aip = aip->ai_next, idx++)
    {
        if (aip->ai_family == AF_INET)
        {
            struct sockaddr_in *ipv4 = (struct sockaddr_in *)aip->ai_addr;
            memcpy(&addresses[idx].address.addr4,ipv4,sizeof(sockaddr_in));
        }
        else
        {
            struct sockaddr_in6 *ipv6 = (struct sockaddr_in6 *)aip->ai_addr;
            memcpy(&addresses[idx].address.addr4,ipv6,sizeof(sockaddr_in6));
        }

    }

    freeaddrinfo(servinfo);
#else
    struct hostent *phe = gethostbyname( ac );

    if ( phe == 0 )
    {
#ifdef _WIN32
        DWORD dwIOError = GetLastError();
        LPVOID messageBuffer;
        FormatMessage( FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                       NULL, dwIOError, MAKELANGID( LANG_NEUTRAL, SUBLANG_DEFAULT ),  // Default language
                       ( LPTSTR ) & messageBuffer, 0, NULL );
        VENET_DEBUG_PRINTF( "gethostbyname failed:Error code - %d\n%s", dwIOError, messageBuffer );

        LocalFree( messageBuffer );
#endif
        return ;
    }
    for ( idx = 0; idx < MAXIMUM_NUMBER_OF_INTERNAL_IDS; ++idx )
    {
        if (phe->h_addr_list[ idx ] == 0)
            break;

        memcpy(&addresses[idx].address.addr4.sin_addr,phe->h_addr_list[ idx ],sizeof(struct in_addr));

    }
#endif

    while (idx < MAXIMUM_NUMBER_OF_INTERNAL_IDS)
    {
        addresses[idx]=UNASSIGNED_SYSTEM_ADDRESS;
        idx++;
    }
}




void SocketLayer::GetMyIP( SystemAddress addresses[MAXIMUM_NUMBER_OF_INTERNAL_IDS] )
{
#if   defined(_WIN32)
    GetMyIP_Win32(addresses);
#else
    GetMyIP_Win32(addresses);
#endif
}


unsigned short SocketLayer::GetLocalPort(SOCKET s)
{
    SystemAddress sa;
    GetSystemAddress(s,&sa);
    return sa.GetPort();
}
void SocketLayer::GetSystemAddress_Old ( SOCKET s, SystemAddress *systemAddressOut )
{
#if defined(__native_client__)
    *systemAddressOut = UNASSIGNED_SYSTEM_ADDRESS;
#else
    sockaddr_in sa;
    memset(&sa,0,sizeof(sockaddr_in));
    socklen_t len = sizeof(sa);
    if (getsockname__(s, (sockaddr*)&sa, &len)!=0)
    {
#if defined(_WIN32) &&  defined(_DEBUG)
        DWORD dwIOError = GetLastError();
        LPVOID messageBuffer;
        FormatMessage( FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                       NULL, dwIOError, MAKELANGID( LANG_NEUTRAL, SUBLANG_DEFAULT ),  // Default language
                       ( LPTSTR ) & messageBuffer, 0, NULL );
        VENET_DEBUG_PRINTF( "getsockname failed:Error code - %d\n%s", dwIOError, messageBuffer );

        LocalFree( messageBuffer );
#endif
        *systemAddressOut = UNASSIGNED_SYSTEM_ADDRESS;
        return;
    }

    systemAddressOut->SetPortNetworkOrder(sa.sin_port);
    systemAddressOut->address.addr4.sin_addr.s_addr=sa.sin_addr.s_addr;
#endif
}
void SocketLayer::GetSystemAddress ( SOCKET s, SystemAddress *systemAddressOut )
{
#if VENET_SUPPORT_IPV6!=1
    GetSystemAddress_Old(s,systemAddressOut);
#else
    socklen_t slen;
    sockaddr_storage ss;
    slen = sizeof(ss);

    if (getsockname__(s, (struct sockaddr *)&ss, &slen)!=0)
    {
#if defined(_WIN32) &&  defined(_DEBUG)
        DWORD dwIOError = GetLastError();
        LPVOID messageBuffer;
        FormatMessage( FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                       NULL, dwIOError, MAKELANGID( LANG_NEUTRAL, SUBLANG_DEFAULT ),  // Default language
                       ( LPTSTR ) & messageBuffer, 0, NULL );
        VENET_DEBUG_PRINTF( "getsockname failed:Error code - %d\n%s", dwIOError, messageBuffer );

        LocalFree( messageBuffer );
#endif
        systemAddressOut->FromString(0);
        return;
    }

    if (ss.ss_family==AF_INET)
    {
        memcpy(&systemAddressOut->address.addr4,(sockaddr_in *)&ss,sizeof(sockaddr_in));
        systemAddressOut->debugPort=ntohs(systemAddressOut->address.addr4.sin_port);

        uint32_t zero = 0;
        if (memcmp(&systemAddressOut->address.addr4.sin_addr.s_addr, &zero, sizeof(zero))==0)
            systemAddressOut->SetToLoopback(4);
    }
    else
    {
        memcpy(&systemAddressOut->address.addr6,(sockaddr_in6 *)&ss,sizeof(sockaddr_in6));
        systemAddressOut->debugPort=ntohs(systemAddressOut->address.addr6.sin6_port);

        char zero[16];
        memset(zero,0,sizeof(zero));
        if (memcmp(&systemAddressOut->address.addr4.sin_addr.s_addr, &zero, sizeof(zero))==0)
            systemAddressOut->SetToLoopback(6);

    }
#endif
}

void SocketLayer::SetSocketLayerOverride(SocketLayerOverride *_slo)
{
    slo=_slo;
}

bool SocketLayer::GetFirstBindableIP(char firstBindable[128], int ipProto)
{
    SystemAddress ipList[ MAXIMUM_NUMBER_OF_INTERNAL_IDS ];
    SocketLayer::GetMyIP( ipList );


    if (ipProto==AF_UNSPEC)

    {
        ipList[0].ToString(false,firstBindable);
        return true;
    }

    unsigned int l;
    for (l=0; l < MAXIMUM_NUMBER_OF_INTERNAL_IDS; l++)
    {
        if (ipList[l]==UNASSIGNED_SYSTEM_ADDRESS)
            break;
        if (ipList[l].GetIPVersion()==4 && ipProto==AF_INET)
            break;
        if (ipList[l].GetIPVersion()==6 && ipProto==AF_INET6)
            break;
    }

    if (ipList[l]==UNASSIGNED_SYSTEM_ADDRESS || l==MAXIMUM_NUMBER_OF_INTERNAL_IDS)
        return false;
    ipList[l].ToString(false,firstBindable);
    return true;

}


#ifdef _MSC_VER
#pragma warning( pop )
#endif
