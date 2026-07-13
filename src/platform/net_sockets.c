// net_sockets.c -- portable UDP networking: BSD sockets + WinSock2.
// Replaces win32/net_wins.c and linux/net_udp.c. IPX is gone for good.

#include "../common/qcommon.h"

#ifdef _WIN32

#include <winsock2.h>
#include <ws2tcpip.h>

typedef SOCKET	qsocket_t;
#define Q_INVALID_SOCKET	INVALID_SOCKET
#define Q_SOCKET_ERROR		SOCKET_ERROR
#define Q_CLOSESOCKET		closesocket
#define Q_SOCKERRNO()		WSAGetLastError()
#define Q_EWOULDBLOCK		WSAEWOULDBLOCK
#define Q_ECONNREFUSED		WSAECONNREFUSED

#else

#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/param.h>
#include <sys/ioctl.h>
#include <errno.h>

typedef int		qsocket_t;
#define Q_INVALID_SOCKET	(-1)
#define Q_SOCKET_ERROR		(-1)
#define Q_CLOSESOCKET		close
#define Q_SOCKERRNO()		errno
#define Q_EWOULDBLOCK		EWOULDBLOCK
#define Q_ECONNREFUSED		ECONNREFUSED

#endif

netadr_t	net_local_adr;

#define	MAX_LOOPBACK	4

typedef struct
{
	byte	data[MAX_MSGLEN];
	int		datalen;
} loopmsg_t;

typedef struct
{
	loopmsg_t	msgs[MAX_LOOPBACK];
	int			get, send;
} loopback_t;

static loopback_t	loopbacks[2];
static qsocket_t	ip_sockets[2] = { Q_INVALID_SOCKET, Q_INVALID_SOCKET };

static qsocket_t NET_Socket (char *net_interface, int port);
static char *NET_ErrorString (void);

//=============================================================================

static void NetadrToSockadr (netadr_t *a, struct sockaddr_in *s)
{
	memset (s, 0, sizeof(*s));

	if (a->type == NA_BROADCAST)
	{
		s->sin_family = AF_INET;
		s->sin_port = a->port;
		s->sin_addr.s_addr = INADDR_BROADCAST;
	}
	else if (a->type == NA_IP)
	{
		s->sin_family = AF_INET;
		memcpy (&s->sin_addr, a->ip, 4);
		s->sin_port = a->port;
	}
}

static void SockadrToNetadr (struct sockaddr_in *s, netadr_t *a)
{
	memcpy (a->ip, &s->sin_addr, 4);
	a->port = s->sin_port;
	a->type = NA_IP;
}

qboolean NET_CompareAdr (netadr_t a, netadr_t b)
{
	if (a.ip[0] == b.ip[0] && a.ip[1] == b.ip[1] && a.ip[2] == b.ip[2] && a.ip[3] == b.ip[3] && a.port == b.port)
		return true;
	return false;
}

/*
===================
NET_CompareBaseAdr

Compares without the port
===================
*/
qboolean NET_CompareBaseAdr (netadr_t a, netadr_t b)
{
	if (a.type != b.type)
		return false;

	if (a.type == NA_LOOPBACK)
		return true;

	if (a.type == NA_IP)
	{
		if (a.ip[0] == b.ip[0] && a.ip[1] == b.ip[1] && a.ip[2] == b.ip[2] && a.ip[3] == b.ip[3])
			return true;
		return false;
	}

	return false;
}

char *NET_AdrToString (netadr_t a)
{
	static	char	s[64];

	Com_sprintf (s, sizeof(s), "%i.%i.%i.%i:%i", a.ip[0], a.ip[1], a.ip[2], a.ip[3], ntohs(a.port));

	return s;
}

char *NET_BaseAdrToString (netadr_t a)
{
	static	char	s[64];

	Com_sprintf (s, sizeof(s), "%i.%i.%i.%i", a.ip[0], a.ip[1], a.ip[2], a.ip[3]);

	return s;
}

/*
=============
NET_StringToSockaddr

localhost
idnewt
idnewt:28000
192.246.40.70
192.246.40.70:28000
=============
*/
static qboolean NET_StringToSockaddr (char *s, struct sockaddr_in *sadr)
{
	struct addrinfo	hints, *res;
	char	*colon;
	char	copy[128];

	memset (sadr, 0, sizeof(*sadr));
	sadr->sin_family = AF_INET;
	sadr->sin_port = 0;

	strncpy (copy, s, sizeof(copy)-1);
	copy[sizeof(copy)-1] = 0;

	// strip off a trailing :port if present
	for (colon = copy ; *colon ; colon++)
		if (*colon == ':')
		{
			*colon = 0;
			sadr->sin_port = htons((short)atoi(colon+1));
		}

	memset (&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_DGRAM;

	if (getaddrinfo (copy, NULL, &hints, &res) != 0 || !res)
		return false;

	sadr->sin_addr = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
	freeaddrinfo (res);

	return true;
}

qboolean NET_StringToAdr (char *s, netadr_t *a)
{
	struct sockaddr_in sadr;

	if (!strcmp (s, "localhost"))
	{
		memset (a, 0, sizeof(*a));
		a->type = NA_LOOPBACK;
		return true;
	}

	if (!NET_StringToSockaddr (s, &sadr))
		return false;

	SockadrToNetadr (&sadr, a);

	return true;
}

qboolean NET_IsLocalAddress (netadr_t adr)
{
	return NET_CompareAdr (adr, net_local_adr);
}

/*
=============================================================================

LOOPBACK BUFFERS FOR LOCAL PLAYER

=============================================================================
*/

static qboolean NET_GetLoopPacket (netsrc_t sock, netadr_t *net_from, sizebuf_t *net_message)
{
	int			i;
	loopback_t	*loop;

	loop = &loopbacks[sock];

	if (loop->send - loop->get > MAX_LOOPBACK)
		loop->get = loop->send - MAX_LOOPBACK;

	if (loop->get >= loop->send)
		return false;

	i = loop->get & (MAX_LOOPBACK-1);
	loop->get++;

	memcpy (net_message->data, loop->msgs[i].data, loop->msgs[i].datalen);
	net_message->cursize = loop->msgs[i].datalen;
	*net_from = net_local_adr;
	return true;
}

static void NET_SendLoopPacket (netsrc_t sock, int length, void *data, netadr_t to)
{
	int			i;
	loopback_t	*loop;

	loop = &loopbacks[sock^1];

	i = loop->send & (MAX_LOOPBACK-1);
	loop->send++;

	memcpy (loop->msgs[i].data, data, length);
	loop->msgs[i].datalen = length;
}

//=============================================================================

qboolean NET_GetPacket (netsrc_t sock, netadr_t *net_from, sizebuf_t *net_message)
{
	int		ret;
	struct sockaddr_in	from;
	socklen_t	fromlen;
	qsocket_t	net_socket;
	int		err;

	if (NET_GetLoopPacket (sock, net_from, net_message))
		return true;

	net_socket = ip_sockets[sock];
	if (net_socket == Q_INVALID_SOCKET)
		return false;

	fromlen = sizeof(from);
	ret = recvfrom (net_socket, (char *)net_message->data, net_message->maxsize,
		0, (struct sockaddr *)&from, &fromlen);
	if (ret == Q_SOCKET_ERROR)
	{
		err = Q_SOCKERRNO ();

		if (err == Q_EWOULDBLOCK || err == Q_ECONNREFUSED)
			return false;
		Com_Printf ("NET_GetPacket: %s\n", NET_ErrorString());
		return false;
	}

	SockadrToNetadr (&from, net_from);

	if (ret == net_message->maxsize)
	{
		Com_Printf ("Oversize packet from %s\n", NET_AdrToString (*net_from));
		return false;
	}

	net_message->cursize = ret;
	return true;
}

//=============================================================================

void NET_SendPacket (netsrc_t sock, int length, void *data, netadr_t to)
{
	int		ret;
	struct sockaddr_in	addr;
	qsocket_t	net_socket;

	if ( to.type == NA_LOOPBACK )
	{
		NET_SendLoopPacket (sock, length, data, to);
		return;
	}

	if (to.type == NA_BROADCAST || to.type == NA_IP)
	{
		net_socket = ip_sockets[sock];
		if (net_socket == Q_INVALID_SOCKET)
			return;
	}
	else
	{
		Com_Error (ERR_FATAL, "NET_SendPacket: bad address type");
		return;
	}

	NetadrToSockadr (&to, &addr);

	ret = sendto (net_socket, data, length, 0, (struct sockaddr *)&addr, sizeof(addr) );
	if (ret == Q_SOCKET_ERROR)
	{
		Com_Printf ("NET_SendPacket ERROR: %s\n", NET_ErrorString());
	}
}

//=============================================================================

/*
====================
NET_OpenIP
====================
*/
static void NET_OpenIP (void)
{
	cvar_t	*port, *ip;

	port = Cvar_Get ("port", va("%i", PORT_SERVER), CVAR_NOSET);
	ip = Cvar_Get ("ip", "localhost", CVAR_NOSET);

	if (ip_sockets[NS_SERVER] == Q_INVALID_SOCKET)
		ip_sockets[NS_SERVER] = NET_Socket (ip->string, port->value);
	if (ip_sockets[NS_CLIENT] == Q_INVALID_SOCKET)
		ip_sockets[NS_CLIENT] = NET_Socket (ip->string, PORT_ANY);
}

/*
====================
NET_Config

A single player game will only use the loopback code
====================
*/
void NET_Config (qboolean multiplayer)
{
	int		i;

	if (!multiplayer)
	{	// shut down any existing sockets
		for (i=0 ; i<2 ; i++)
		{
			if (ip_sockets[i] != Q_INVALID_SOCKET)
			{
				Q_CLOSESOCKET (ip_sockets[i]);
				ip_sockets[i] = Q_INVALID_SOCKET;
			}
		}
	}
	else
	{	// open sockets
		NET_OpenIP ();
	}
}

//===================================================================

/*
====================
NET_Init
====================
*/
void NET_Init (void)
{
#ifdef _WIN32
	WSADATA	winsockdata;

	if (WSAStartup (MAKEWORD(2,2), &winsockdata))
		Com_Error (ERR_FATAL, "Winsock initialization failed.");

	Com_Printf ("Winsock Initialized\n");
#endif
}

/*
====================
NET_Socket
====================
*/
static qsocket_t NET_Socket (char *net_interface, int port)
{
	qsocket_t	newsocket;
	struct sockaddr_in	address;
#ifdef _WIN32
	u_long	_true = 1;
#else
	int		_true = 1;
#endif
	int		i = 1;

	if ((newsocket = socket (PF_INET, SOCK_DGRAM, IPPROTO_UDP)) == Q_INVALID_SOCKET)
	{
		Com_Printf ("ERROR: UDP_OpenSocket: socket: %s\n", NET_ErrorString());
		return Q_INVALID_SOCKET;
	}

	// make it non-blocking
#ifdef _WIN32
	if (ioctlsocket (newsocket, FIONBIO, &_true) == Q_SOCKET_ERROR)
#else
	if (ioctl (newsocket, FIONBIO, &_true) == Q_SOCKET_ERROR)
#endif
	{
		Com_Printf ("ERROR: UDP_OpenSocket: ioctl FIONBIO: %s\n", NET_ErrorString());
		Q_CLOSESOCKET (newsocket);
		return Q_INVALID_SOCKET;
	}

	// make it broadcast capable
	if (setsockopt(newsocket, SOL_SOCKET, SO_BROADCAST, (char *)&i, sizeof(i)) == Q_SOCKET_ERROR)
	{
		Com_Printf ("ERROR: UDP_OpenSocket: setsockopt SO_BROADCAST: %s\n", NET_ErrorString());
		Q_CLOSESOCKET (newsocket);
		return Q_INVALID_SOCKET;
	}

	memset (&address, 0, sizeof(address));

	if (!net_interface || !net_interface[0] || !Q_stricmp(net_interface, "localhost"))
		address.sin_addr.s_addr = INADDR_ANY;
	else
		NET_StringToSockaddr (net_interface, &address);

	if (port == PORT_ANY)
		address.sin_port = 0;
	else
		address.sin_port = htons((short)port);

	address.sin_family = AF_INET;

	if (bind (newsocket, (struct sockaddr *)&address, sizeof(address)) == Q_SOCKET_ERROR)
	{
		Com_Printf ("ERROR: UDP_OpenSocket: bind: %s\n", NET_ErrorString());
		Q_CLOSESOCKET (newsocket);
		return Q_INVALID_SOCKET;
	}

	return newsocket;
}

/*
====================
NET_Shutdown
====================
*/
void NET_Shutdown (void)
{
	NET_Config (false);	// close sockets

#ifdef _WIN32
	WSACleanup ();
#endif
}

/*
====================
NET_ErrorString
====================
*/
static char *NET_ErrorString (void)
{
#ifdef _WIN32
	static char	msg[256];
	int		code;

	code = WSAGetLastError ();
	if (!FormatMessageA (FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
			NULL, code, 0, msg, sizeof(msg), NULL))
		Com_sprintf (msg, sizeof(msg), "winsock error %d", code);
	return msg;
#else
	return strerror (errno);
#endif
}

// sleeps msec or until net socket is ready
void NET_Sleep (int msec)
{
	struct timeval	timeout;
	fd_set	fdset;
	extern qboolean stdin_active;

	if (ip_sockets[NS_SERVER] == Q_INVALID_SOCKET || (dedicated && !dedicated->value))
		return; // we're not a server, just run full speed

	FD_ZERO (&fdset);
#ifndef _WIN32
	if (stdin_active)
		FD_SET (0, &fdset); // stdin is processed too
#endif
	FD_SET (ip_sockets[NS_SERVER], &fdset); // network socket
	timeout.tv_sec = msec/1000;
	timeout.tv_usec = (msec%1000)*1000;
	select ((int)(ip_sockets[NS_SERVER]+1), &fdset, NULL, NULL, &timeout);
}
