/*
 * Datapipe - Create a listen socket to pipe connections to another
 * machine/port. 'localport' accepts connections on the machine running    
 * datapipe, which will connect to 'remoteport' on 'remotehost'.
 * It will fork itself into the background on non-Windows machines.
 *
 * This implementation of the traditional "datapipe" does not depend on
 * forking to handle multiple simultaneous clients, and instead is able
 * to do all processing from within a single process, making it ideal
 * for low-memory environments.  The elimination of the fork also
 * allows it to be used in environments without fork, such as Win32.
 *
 * This implementation also differs from most others in that it allows
 * the specific IP address of the interface to listen on to be specified.
 * This is useful for machines that have multiple IP addresses.  The
 * specified listening address will also be used for making the outgoing
 * connections on.
 *
 * Note that select() is not used to perform writability testing on the
 * outgoing sockets, so conceivably other connections might have delayed
 * responses if any of the connected clients or the connection to the
 * target machine is slow enough to allow its outgoing buffer to fill
 * to capacity.
 *
 * Compile with:
 *     cc -O -o datapipe datapipe.c
 * On Solaris/SunOS, compile with:
 *     gcc -Wall datapipe.c -lsocket -lnsl -o datapipe
 * On Windows compile with:
 *     bcc32 /w datapipe.c                (Borland C++)
 *     cl /W3 datapipe.c wsock32.lib      (Microsoft Visual C++)
 *
 * Run as:
 *   datapipe localhost localport remoteport remotehost
 *
 *
 * written by Jeff Lawson <jlawson@bovine.net>
 * inspired by code originally by Todd Vierling, 1995.
 */


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#if defined(__WIN32__) || defined(WIN32) || defined(_WIN32)
  #define WIN32_LEAN_AND_MEAN
  #include <winsock.h>
  #define bzero(p, l) memset(p, 0, l)
  #define bcopy(s, t, l) memmove(t, s, l)
#else
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <sys/wait.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <netdb.h>
  #include <strings.h>
  #define recv(x,y,z,a) read(x,y,z)
  #define send(x,y,z,a) write(x,y,z)
  #define closesocket(s) close(s)
  typedef int SOCKET;
#endif


struct client_t
{
  int inuse;
  SOCKET csock, osock;
  time_t activity;
};

#define MAXCLIENTS 20
#define IDLETIMEOUT 300


const char ident[] = "$Id: datapipe.c,v 1.6 1998/12/02 01:54:04 jlawson Exp $";

int main(int argc, char *argv[])
{ 
  SOCKET lsock;
  char buf[4096];
  struct sockaddr_in laddr, oaddr;
  int i;
  struct client_t clients[MAXCLIENTS];


#if defined(__WIN32__) || defined(WIN32) || defined(_WIN32)
  /* Winsock needs additional startup activities */
  WSADATA wsadata;
  WSAStartup(MAKEWORD(1,1), &wsadata);
#endif


  /* check number of command line arguments */
  if (argc != 5) {
    fprintf(stderr,"Usage: %s localhost localport remotehost remoteport\n",argv[0]);
    return 30;
  }


  /* reset all of the client structures */
  for (i = 0; i < MAXCLIENTS; i++)
    clients[i].inuse = 0;


  /* determine the listener address and port */
  bzero(&laddr, sizeof(struct sockaddr_in));
  laddr.sin_family = AF_INET;
  laddr.sin_port = htons((unsigned short) atol(argv[2]));
  laddr.sin_addr.s_addr = inet_addr(argv[1]);
  if (!laddr.sin_port) {
    fprintf(stderr, "invalid listener port\n");
    return 20;
  }
  if ((long) laddr.sin_addr.s_addr == -1 || !laddr.sin_addr.s_addr) {
    struct hostent *n;
    if ((n = gethostbyname(argv[1])) == NULL) {
      perror("gethostbyname");
      return 20;
    }    
    bcopy(n->h_addr, (char *) &laddr.sin_addr, n->h_length);
  }


  /* determine the outgoing address and port */
  bzero(&oaddr, sizeof(struct sockaddr_in));
  oaddr.sin_family = AF_INET;
  oaddr.sin_port = htons((unsigned short) atol(argv[4]));
  if (!oaddr.sin_port) {
    fprintf(stderr, "invalid target port\n");
    return 25;
  }
  oaddr.sin_addr.s_addr = inet_addr(argv[3]);
  if ((long) oaddr.sin_addr.s_addr == -1 || !oaddr.sin_addr.s_addr) {
    struct hostent *n;
    if ((n = gethostbyname(argv[3])) == NULL) {
      perror("gethostbyname");
      return 25;
    }    
    bcopy(n->h_addr, (char *) &oaddr.sin_addr, n->h_length);
  }


  /* create the listener socket */
  if ((lsock = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
    perror("socket");
    return 20;
  }
  if (bind(lsock, (struct sockaddr *)&laddr, sizeof(laddr))) {
    perror("bind");
    return 20;
  }
  if (listen(lsock, 5)) {
    perror("listen");
    return 20;
  }


  /* change the port in the listener struct to zero, since we will
   * use it for binding to outgoing local sockets in the future. */
  laddr.sin_port = htons(0);


  /* fork off into the background. */
#if !defined(__WIN32__) && !defined(WIN32) && !defined(_WIN32)
  if ((i = fork()) == -1) {
    perror("fork");
    return 20;
  }
  if (i > 0)
    return 0;
  setsid();
#endif

  
  /* main polling loop. */
  while (1)
  {
    fd_set fdsr;
    int maxsock;
    struct timeval tv = {1,0};
    time_t now = time(NULL);

    /* build the list of sockets to check. */
    FD_ZERO(&fdsr);
    FD_SET(lsock, &fdsr);
    maxsock = (int) lsock;
    for (i = 0; i < MAXCLIENTS; i++)
      if (clients[i].inuse) {
        FD_SET(clients[i].csock, &fdsr);
        if ((int) clients[i].csock > maxsock)
          maxsock = (int) clients[i].csock;
        FD_SET(clients[i].osock, &fdsr);
        if ((int) clients[i].osock > maxsock)
          maxsock = (int) clients[i].osock;
      }      
    if (select(maxsock + 1, &fdsr, NULL, NULL, &tv) < 0) {
      return 30;
    }


    /* check if there are new connections to accept. */
    if (FD_ISSET(lsock, &fdsr))
    {
      SOCKET csock = accept(lsock, NULL, 0);
     
      for (i = 0; i < MAXCLIENTS; i++)
        if (!clients[i].inuse) break;
      if (i < MAXCLIENTS)
      {
        /* connect a socket to the outgoing host/port */
        SOCKET osock;
        if ((osock = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
          perror("socket");
          closesocket(csock);
        }
        else if (bind(osock, (struct sockaddr *)&laddr, sizeof(laddr))) {
          perror("bind");
          closesocket(csock);
          closesocket(osock);
        }
        else if (connect(osock, (struct sockaddr *)&oaddr, sizeof(oaddr))) {
          perror("connect");
          closesocket(csock);
          closesocket(osock);
        }
        else {
          clients[i].osock = osock;
          clients[i].csock = csock;
          clients[i].activity = now;
          clients[i].inuse = 1;
        }
      } else {
        fprintf(stderr, "too many clients\n");
        closesocket(csock);
      }        
    }


    /* service any client connections that have waiting data. */
    for (i = 0; i < MAXCLIENTS; i++)
    {
      int nbyt, closeneeded = 0;
      if (!clients[i].inuse) {
        continue;
      } else if (FD_ISSET(clients[i].csock, &fdsr)) {
        if ((nbyt = recv(clients[i].csock, buf, sizeof(buf), 0)) <= 0 ||
          send(clients[i].osock, buf, nbyt, 0) <= 0) closeneeded = 1;
        else clients[i].activity = now;
      } else if (FD_ISSET(clients[i].osock, &fdsr)) {
        if ((nbyt = recv(clients[i].osock, buf, sizeof(buf), 0)) <= 0 ||
          send(clients[i].csock, buf, nbyt, 0) <= 0) closeneeded = 1;
        else clients[i].activity = now;
      } else if (now - clients[i].activity > IDLETIMEOUT) {
        closeneeded = 1;
      }
      if (closeneeded) {
        closesocket(clients[i].csock);
        closesocket(clients[i].osock);
        clients[i].inuse = 0;
      }      
    }
    
  }
  return 0;
}




