#include <iostream>
#include <map>
#include <set>
#include <vector>>
#include <string>
#include <cstdlib>
#include <cstdio>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/socket.h>
#include <sys/socketvar.h>
#include <net/if_dl.h>
#include <net/if.h>
#include <net/route.h>
#include <netinet/in.h>
#include <netinet/in_pcb.h>
#include <netinet/tcp.h>
#include <netinet/tcp_var.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <sys/event.h>
#include <sys/time.h>

#include "lua.hpp"

#define CLEAN_TIME 5

#if defined(__NetBSD__)
#define PTR intptr_t
#elif defined(__APPLE__)
#define PTR void*
#endif

using namespace std;

struct File;
static map<int,File*> openfiles;
static set<File*> files2open;

struct ReadFailed {};
struct IfaddrsFailed {};
struct BadIfName {};

static int myqueue = -1;
static bool caught_sighup = false;
static lua_State* lua = 0;

struct File {
    File(const char* filename_,int myqueue_,
	 lua_State *lua_,int funcref_,
	 map<int,File*>& openfiles,set<File*>& files2open) : myqueue(myqueue_),filename(filename_) {
	if(openfile() == false)
	    files2open.insert(this);
	else
	    openfiles[fd] = this;
	lua = lua_;
	funcref = funcref_;
    }

    bool openfile(void) {
	struct kevent evt;
	if((fd = open(filename.c_str(),O_RDONLY,0)) == -1)
	    return false;

	lseek(fd,0,SEEK_END);

	EV_SET(&evt,fd,EVFILT_READ,EV_ADD,0,0,(PTR)File::readfd);
	kevent(myqueue,&evt,1,0,0,0); // bugbug -- error	
	EV_SET(&evt,fd,EVFILT_VNODE,EV_ADD,NOTE_DELETE | NOTE_RENAME,
	       0,(PTR)File::reopen);
	kevent(myqueue,&evt,1,0,0,0); // bugbug -- error
	return true;
    }

    static void readfd(struct kevent* evt) {
	File* me = openfiles.find(evt->ident)->second;
	(*me)(evt->data);
    }

    static void reopen(struct kevent* evt) {
	File* me = openfiles.find(evt->ident)->second;
	files2open.insert(me);
    }

    void operator()(int64_t bytestoread);
    bool operator()(void);
    
    ~File() {
	close(fd);
    }
    int myqueue;
    string filename;
    int fd;
    int funcref;
    lua_State *lua;
};

void File::operator()(int64_t bytestoread) {
    char* readbuf = new char[bytestoread];
    int64_t bytesread;
    bool linehandled = false;

    if((bytesread = read(fd,readbuf,bytestoread)) == -1)
	throw ReadFailed();

    // bugbug -- handle multiple lines in one read
    for(unsigned int i = 0; i < bytesread; i++)
	if(*(readbuf + i) == '\n') {
	    *(readbuf + i) = 0;
	    lua_rawgeti(lua,LUA_REGISTRYINDEX, funcref);
	    lua_pushstring(lua,readbuf);
	    lua_call(lua,1,0);
	    linehandled = true;

	    if((i + 1) != bytesread)
		lseek(fd,-(bytesread - (i + 1)),SEEK_CUR);

	    break;
	}
    
    if(linehandled == false)
	lseek(fd,-bytesread,SEEK_CUR);

    delete [] readbuf;
}

bool File::operator()(void) {
    cerr << "Opening " << filename << endl;
    if(fd != -1) {
	close(fd);
	fd = -1;
    }

    return openfile();
}

struct Proto {
    Proto(const char* laddr_,const char* faddr_,
          u_int16_t lport_,u_int16_t fport_) : laddr(laddr_),faddr(faddr_),lport(lport_),fport(fport_),proto(0) { }
   virtual ~Proto() { }
   string laddr,faddr;
   u_int16_t lport,fport;
   const char* proto;
};

struct Udp : Proto {
    Udp(const char* laddr_,const char* faddr_,
        u_int16_t lport_,u_int16_t fport_) : Proto(laddr_,faddr_,lport_,fport_) { proto = "UDP"; }
};

static const char* StateLookup[] = {
   "CLOSED","LISTEN","UNKNOWN2","UNKNOWN3",
   "ESTABLISHED","CLOSE_WAIT","UNKNOWN6","UNKNOWN7"
};

struct Tcp : Proto {
    Tcp(const char* laddr_,const char* faddr_,
        u_int16_t lport_,u_int16_t fport_,u_int8_t state_) : Proto(laddr_,faddr_,lport_,fport_),state(state_) { proto = "TCP"; }
    u_int8_t state;
};

struct tcpcmp {
    bool operator()(const Tcp* a,const TCP* b) {

    }
};

set<Tcp*> tcpdata;
set<Udp*> udpdata;

static void
MacPcbLookup(void* tcpbuf,size_t tcpsz,void* udpbuf,size_t udpsz) {
    struct xinpgen* xig = (struct xinpgen*)tcpbuf;
    struct xtcpcb* cp = (struct xtcpcb*)((char*)tcpbuf + xig->xig_len);
    tcpsz -= xig->xig_len;
    struct xtcpcb* ep = cp + (tcpsz / sizeof(struct xtcpcb));
    vector<Proto*> pvec;
    
    for( ; cp != ep; cp++)
        pvec.push_back(new Tcp(inet_ntoa(cp->xt_inp.inp_laddr),
                               inet_ntoa(cp->xt_inp.inp_faddr),
                               htons(cp->xt_inp.inp_lport),
                               htons(cp->xt_inp.inp_fport),
                               cp->xt_tp.t_state));
    
    xig = (struct xinpgen*)udpbuf;
    struct xinpcb* cp2 = (struct xinpcb*)((char*)udpbuf + xig->xig_len);
    udpsz -= xig->xig_len;
    struct xinpcb* ep2 = cp2 + (udpsz / sizeof(struct xinpcb));
    for( ; cp2 != ep2; cp2++)
        pvec.push_back(new Udp(inet_ntoa(cp->xt_inp.inp_laddr),
                               inet_ntoa(cp->xt_inp.inp_faddr),
                               htons(cp->xt_inp.inp_lport),
                               htons(cp->xt_inp.inp_fport)));
    
    for(vector<Proto*>::iterator i = pvec.begin(); i != pvec.end(); ++i)
	delete *i;
}

static int
getinterface_address(lua_State* l) {
    struct ifaddrs* ifa_p;
    int numelts = lua_gettop(l);
    const char* ifname,*addr = 0;

    if(numelts == 0 || lua_isstring(l,1) == 0)
	throw BadIfName();

    ifname = lua_tostring(l,numelts);

    if(getifaddrs(&ifa_p) == -1) {
	lua_pop(l,1);
	throw IfaddrsFailed();
    }

    for(struct ifaddrs* cp = ifa_p; cp != 0; cp = cp->ifa_next) {
	if(strcmp(cp->ifa_name,ifname) == 0 &&
	   ((struct sockaddr_in*)cp->ifa_addr)->sin_family == AF_INET) {
	    addr = inet_ntoa(((struct sockaddr_in*)cp->ifa_addr)->sin_addr);
	    break;
	}
    }

    lua_pop(l,1);
    if(addr != 0)
	lua_pushstring(l,addr);
    else  // interface name not found
	lua_pushnil(l);

    freeifaddrs(ifa_p);
    return(1);
}

#define ROUNDUP(a) \
    ((a) > 0 ? (1 + (((a) - 1) | (sizeof(long) - 1))) : sizeof(long))

static int
getnicaddressbydest(lua_State* l) {
    struct data {
	struct rt_msghdr hdr;
	char rest[512];
    } msg = {0};
    char* cp = msg.rest;
    static int seq = 1;
    static pid_t pid = getpid();
    struct sockaddr_in so_dst = {0};
    int rtsock;
    in_addr_t addr;

    if(lua_isstring(l,lua_gettop(l)) != 1 ||
       (addr = inet_addr(lua_tostring(lua,lua_gettop(l)))) == INADDR_NONE ||
       (rtsock  = socket(PF_ROUTE,SOCK_RAW,PF_UNSPEC)) == -1) {
	lua_pop(l,1);
	lua_pushnil(l);
	return 1;
    }

    lua_pop(l,1);
    msg.hdr.rtm_type = RTM_GET;
    msg.hdr.rtm_addrs = (RTA_DST | RTA_IFA);
    msg.hdr.rtm_pid = pid;
    msg.hdr.rtm_version = RTM_VERSION;
    msg.hdr.rtm_seq = seq;

    so_dst.sin_addr.s_addr = addr;
    so_dst.sin_len = sizeof(struct sockaddr_in);
    so_dst.sin_family = PF_INET;
    memcpy(cp,&so_dst,sizeof(struct sockaddr_in));
    msg.hdr.rtm_msglen = sizeof(struct rt_msghdr) + ROUNDUP(so_dst.sin_len);

    // bugbug -- handle errors for write and read
    write(rtsock,&msg,msg.hdr.rtm_msglen);
    memset(&msg,0,msg.hdr.rtm_msglen); // probably unnecessary
    do {
	read(rtsock,&msg,sizeof(msg));
	if(msg.hdr.rtm_seq == seq && msg.hdr.rtm_pid == pid)
	    break;
    } while(1);
    seq++;

    int offset = sizeof(struct sockaddr_in) * 2 + sizeof(struct sockaddr_dl);
    offset += (msg.hdr.rtm_addrs & RTA_NETMASK) ? sizeof(in_addr) : 0;
    in_addr addr_ = ((struct sockaddr_in*)(msg.rest + offset))->sin_addr;
    lua_pushstring(l,inet_ntoa(addr_));
    close(rtsock);
    return(1);
}

static void
cleanup(void) {
    alarm(0);
    if(lua != 0)
	lua_close(lua);

    if(myqueue != -1)
	close(myqueue);
    myqueue = -1;

    caught_sighup = false;

    for(map<int,File*>::iterator i = openfiles.begin();
    	i != openfiles.end(); ++i)
	delete (i->second);

    for(set<File*>::iterator i = files2open.begin(); i != files2open.end(); ++i)
	delete (*i);

    openfiles.clear();
    files2open.clear();
}

static void
handle_sighup(struct evt* evt) {
    caught_sighup = true;
}

static void
handle_alarm(struct evt* evt) {
    lua_getglobal(lua,"alarm_handler");

    if(lua_isfunction(lua,1) == 1) {
        char* tcpbuf = 0;
        char* udpbuf = 0;
        size_t tcpsz = 1024;
        size_t udpsz = 1024;
        bool done = false;

        lua_newtable(lua);

        while(!done) {
            tcpbuf = (char*)malloc(tcpsz);
            udpbuf = (char*)malloc(udpsz);

            if(tcpbuf == 0 || udpbuf == 0) {
                done = true;
                goto again;
            }

            errno = 0;
            sysctlbyname("net.inet.tcp.pcblist",tcpbuf,&tcpsz,0,0);
            if(errno == ENOMEM) {
                tcpsz <<= 1;
                goto again;
            }
            errno = 0;
            sysctlbyname("net.inet.udp.pcblist",udpbuf,&udpsz,0,0);
            if(errno == ENOMEM) {
                udpsz <<= 1;
                goto again;
            }
            MacPcbLookup(tcpbuf,tcpsz,udpbuf,udpsz);
            done = true;

       again:
            free(tcpbuf);
            free(udpbuf);
        }
	lua_call(lua,1,0);
    }

    lua_gc(lua,LUA_GCCOLLECT,0);
    alarm(CLEAN_TIME);

 invalid:
    for(set<File*>::iterator i = files2open.begin(); i != files2open.end(); ++i)
	if((**i)() == true) {
	    files2open.erase(*i);
	    goto invalid;
}
}

static void
usage(const char* prog) {
    cerr << prog << " file_to_evaluate" << endl;
}

int main(int argc,char *argv[]) {
    struct kevent evt;

    if(argc != 2)
	return(usage(argv[0]),EXIT_FAILURE);

    atexit(cleanup);
    signal(SIGHUP,SIG_IGN);
    signal(SIGALRM,SIG_IGN);

    for( ; ; ) {
	if((lua = lua_open()) == 0)
	    return(cerr << "Failed opening lua" << endl,EXIT_FAILURE);

	luaL_openlibs(lua);
	lua_register(lua,"getinterface_address",getinterface_address);
	lua_register(lua,"getnicaddressbydest",getnicaddressbydest);

	if(luaL_dofile(lua,argv[1]) != 0)
	    return(cerr << "Failed evaluating: " << argv[1] << endl,
		   EXIT_FAILURE);

	if((myqueue = kqueue()) == -1)
	    return(cerr << "Failed creating kmyqueue: " << strerror(errno)
		   << endl,EXIT_FAILURE);

	EV_SET(&evt,SIGHUP,EVFILT_SIGNAL,EV_ADD,0,0,(PTR)handle_sighup);
	kevent(myqueue,&evt,1,0,0,0); // bugbug -- error
	EV_SET(&evt,SIGALRM,EVFILT_SIGNAL,EV_ADD,0,0,(PTR)handle_alarm);
	kevent(myqueue,&evt,1,0,0,0); // bugbug -- error
	
	lua_getglobal(lua,"files2watch");
	if(lua_istable(lua,0) != 1)
	    return(cerr << "must define files2watch in " << argv[1] << endl,
		   EXIT_FAILURE);

	unsigned int numelts = lua_objlen(lua,1);
	for(unsigned int i = 1; i <= numelts; i++) {
	    lua_pushinteger(lua,i);
	    lua_gettable(lua,1);

	    if(lua_istable(lua,2) != 1)
		return(cerr << "files2watch must be a table of tables" << endl,
		       EXIT_FAILURE);

	    if(lua_objlen(lua,2) != 2)
		return(cerr << "files2watch must contain 2-elt tables" << endl,
		       EXIT_FAILURE);
	    lua_pushinteger(lua,1);
	    lua_gettable(lua,2);
	    lua_pushinteger(lua,2);
	    lua_gettable(lua,2);

	    if(lua_isstring(lua,3) != 1 ||
	       lua_isfunction(lua,4) != 1)
		return(cerr << "files2watch tables must be a string, function pair" << endl, EXIT_FAILURE);

	    new File(lua_tostring(lua,3),myqueue,
		     lua,luaL_ref(lua,LUA_REGISTRYINDEX),openfiles,files2open);
	    lua_pop(lua,2);

	}
	alarm(CLEAN_TIME);
	lua_pop(lua,1);
	
	do {
	    kevent(myqueue,0,0,&evt,1,0); // bugbug -- error
	    ((void (*)(struct kevent*))evt.udata)(&evt);
	} while(caught_sighup == false);

	cleanup();
    }
}
