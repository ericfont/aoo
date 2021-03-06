#include "m_pd.h"
#include "aoo/aoo.h"

#include <string.h>
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <inttypes.h>
#include <errno.h>

#ifdef _WIN32
#include <winsock2.h>
typedef int socklen_t;
#else
#include <sys/socket.h>
#include <sys/select.h>
#include <unistd.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif

#define classname(x) class_getname(*(t_pd *)x)

#ifndef AOO_DEBUG_OSCTIME
#define AOO_DEBUG_OSCTIME 0
#endif

#define DEFBUFSIZE 20

int socket_close(int socket)
{
#ifdef _WIN32
    return closesocket(socket);
#else
    return close(socket);
#endif
}

void socket_error_print(const char *label)
{
#ifdef _WIN32
    int err = WSAGetLastError();
    char str[1024];
    str[0] = 0;
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, 0,
                   err, MAKELANGID (LANG_NEUTRAL, SUBLANG_DEFAULT), str,
                   sizeof(str), NULL);
#else
    int err = errno;
    const char *str = strerror(err);
#endif
    if (label){
        fprintf(stderr, "%s: %s (%d)\n", label, str, err);
    } else {
        fprintf(stderr, "%s (%d)\n", str, err);
    }
    fflush(stderr);
}

/*////////////////////// socket listener //////////////////*/

// use linked list for persistent memory
typedef struct _client {
    int socket;
    struct sockaddr_storage addr;
    int addrlen;
    struct _client *next;
} t_client;

typedef struct _aoo_receive t_aoo_receive;

static t_class *socket_listener_class;

typedef struct _socket_listener
{
    t_pd pd;
    t_symbol *sym;
    // dependants
    t_aoo_receive **recv;
    int numrecv; // doubles as refcount
    // socket
    int socket;
    int port;
    t_client *clients;
    // threading
    pthread_t thread;
    pthread_mutex_t mutex;
    int quit; // should be atomic, but works anyway
} t_socket_listener;

static void socket_listener_reply(t_client *x, const char *data, int32_t n)
{
    // no check or synchronization needed
    sendto(x->socket, data, n, 0, (const struct sockaddr *)&x->addr, x->addrlen);
}

static void aoo_receive_handle_message(t_aoo_receive *x, int32_t id,
                                const char * data, int32_t n, void *src, aoo_replyfn fn);

static void* socket_listener_threadfn(void *y)
{
    t_socket_listener *x = (t_socket_listener *)y;

    while (!x->quit){
        struct sockaddr_storage sa;
        socklen_t len = sizeof(sa);
        char buf[AOO_MAXPACKETSIZE];
        int nbytes = recvfrom(x->socket, buf, AOO_MAXPACKETSIZE, 0, (struct sockaddr *)&sa, &len);
        if (nbytes > 0){
            // try to find client
            t_client *client = 0;
            for (t_client *c = x->clients; c; c = c->next){
                if (len == c->addrlen &&
                    !memcmp(&sa, &c->addr, len)){
                    client = c;
                    break;
                }
            }
            if (!client){
                // add client
                client = (t_client *)getbytes(sizeof(t_client));
                client->socket = x->socket;
                memcpy(&client->addr, &sa, len);
                client->addrlen = len;
                client->next = 0;
                if (x->clients){
                    client->next = x->clients;
                    x->clients = client;
                } else {
                    x->clients = client;
                }
            }
            // forward OSC packet to matching receivers
            int32_t id = 0;
            if (aoo_parsepattern(buf, nbytes, &id) > 0){
                pthread_mutex_lock(&x->mutex);
                for (int i = 0; i < x->numrecv; ++i){
                    aoo_receive_handle_message(x->recv[i], id, buf, nbytes,
                                               client, (aoo_replyfn)socket_listener_reply);
                }
                pthread_mutex_unlock(&x->mutex);
            } else {
                // not a valid AoO OSC message
            }
        } else if (nbytes < 0){
            // ignore errors when quitting
            if (!x->quit){
                socket_error_print("recv");
            }
        }
    }

    return 0;
}

static int aoo_receive_match(t_aoo_receive *x, t_aoo_receive *other);

t_socket_listener* socket_listener_add(t_aoo_receive *r, int port)
{
    // make bind symbol for port number
    char buf[64];
    snprintf(buf, sizeof(buf), "aoo listener %d", port);
    t_symbol *s = gensym(buf);
    t_socket_listener *x = (t_socket_listener *)pd_findbyclass(s, socket_listener_class);
    if (x){
        // check receiver and add to list
        pthread_mutex_lock(&x->mutex);
    #if 1
        for (int i = 0; i < x->numrecv; ++i){
            if (aoo_receive_match(x->recv[i], r)){
                pthread_mutex_unlock(&x->mutex);
                return 0;
            }
        }
    #endif
        x->recv = (t_aoo_receive **)resizebytes(x->recv, sizeof(t_aoo_receive *) * x->numrecv,
                                                sizeof(t_aoo_receive *) * (x->numrecv + 1));
        x->recv[x->numrecv] = r;
        x->numrecv++;
        pthread_mutex_unlock(&x->mutex);
    } else {
        // make new socket listener

        // first create socket
        int sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock < 0){
            socket_error_print("socket");
            return 0;
        }
        struct sockaddr_in sa;
        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_addr.s_addr = INADDR_ANY;
        sa.sin_port = htons(port);
        if (bind(sock, (const struct sockaddr *)&sa, sizeof(sa)) < 0){
            pd_error(x, "%s: couldn't bind to port %d", classname(r), port);
            socket_close(sock);
            return 0;
        }

        // now create socket listener instance
        x = (t_socket_listener *)getbytes(sizeof(t_socket_listener));
        x->pd = socket_listener_class;
        x->sym = s;
        pd_bind(&x->pd, s);

        // add receiver
        x->recv = (t_aoo_receive **)getbytes(sizeof(t_aoo_receive *));
        x->recv[0] = r;
        x->numrecv = 1;

        x->socket = sock;
        x->port = port;
        x->clients = 0;

        // start thread
        x->quit = 0;
        pthread_mutex_init(&x->mutex, 0);
        pthread_create(&x->thread, 0, socket_listener_threadfn, x);

        verbose(0, "new socket listener on port %d", x->port);
    }
    return x;
}

void socket_listener_release(t_socket_listener *x, t_aoo_receive *r)
{
    if (x->numrecv > 1){
        // just remove receiver from list
        int n = x->numrecv;
        for (int i = 0; i < n; ++i){
            if (x->recv[i] == r){
                memmove(&x->recv[i], x->recv[i + 1], n - (i + 1));
                x->recv = (t_aoo_receive **)resizebytes(x->recv, n * sizeof(t_aoo_receive *),
                                                        (n - 1) * sizeof(t_aoo_receive *));
                x->numrecv--;
                return;
            }
        }
        bug("socket_listener_release: receiver not found!");
    } else if (x->numrecv == 1){
        // last instance
        pd_unbind(&x->pd, x->sym);
        // notify the thread that we're done
        x->quit = 1;
        // wake up blocking recv() by sending an empty packet
        int didit = 0;
        int signal = socket(AF_INET, SOCK_DGRAM, 0);
        if (signal >= 0){
            struct sockaddr_in sa;
            memset(&sa, 0, sizeof(sa));
            sa.sin_family = AF_INET;
            sa.sin_addr.s_addr = htonl(0x7f000001); // localhost
            sa.sin_port = htons(x->port);
            if (sendto(x->socket, 0, 0, 0, (struct sockaddr *)&sa, sizeof(sa)) < 0){
                socket_error_print("sendto");
                socket_close(signal);
            } else {
                didit = 1;
            }
        } else {
            socket_error_print("socket");
        }
        if (!didit){
            // force wakeup by closing the socket.
            // this is not nice and probably undefined behavior,
            // the MSDN docs explicitly forbid it!
            socket_close(x->socket);
        }
        pthread_join(x->thread, 0); // wait for thread
        pthread_mutex_destroy(&x->mutex);

        if (didit){
            socket_close(x->socket);
            socket_close(signal);
        }

        // free memory
        t_client *c = x->clients;
        while (c){
            t_client *next = c->next;
            freebytes(c, sizeof(t_client));
            c = next;
        }
        freebytes(x->recv, sizeof(t_aoo_receive*) * x->numrecv);
        verbose(0, "released socket listener on port %d", x->port);
        freebytes(x, sizeof(*x));
    } else {
        bug("socket_listener_release: negative refcount!");
    }
}

void socket_listener_setup(void)
{
    socket_listener_class = class_new(gensym("aoo socket listener"), 0, 0,
                                  sizeof(t_socket_listener), CLASS_PD, A_NULL);
}

/*///////////////////// aoo_receive~ ////////////////////*/

static t_class *aoo_receive_class;

typedef struct _aoo_receive
{
    t_object x_obj;
    t_float x_f;
    aoo_sink *x_aoo_sink;
    aoo_sink_settings x_settings;
    int32_t x_id;
    t_sample **x_vec;
    t_socket_listener * x_listener;
    pthread_mutex_t x_mutex;
    t_outlet *x_eventout;
    aoo_event *x_eventbuf;
    int x_eventbufsize;
    int x_numevents;
    t_clock *x_clock;
} t_aoo_receive;

// called from socket listener
static int aoo_receive_match(t_aoo_receive *x, t_aoo_receive *other)
{
    if (x == other){
        bug("socket_listener_add: receiver already added!");
        return 1;
    }
    if (x->x_id == other->x_id){
        pd_error(x, "%s with ID %d on port %d already exists!",
                 classname(x), x->x_id, x->x_listener->port);
        return 1;
    }
    return 0;
}

static void aoo_receive_handle_message(t_aoo_receive *x, int32_t id,
                                const char * data, int32_t n, void *src, aoo_replyfn fn)
{
    if (id == AOO_ID_WILDCARD || id == x->x_id){
        pthread_mutex_lock(&x->x_mutex);
        aoo_sink_handlemessage(x->x_aoo_sink, data, n, src, fn);
        pthread_mutex_unlock(&x->x_mutex);
    }
}

static void aoo_receive_buffersize(t_aoo_receive *x, t_floatarg f)
{
    x->x_settings.buffersize = f;
    if (x->x_settings.blocksize){
        pthread_mutex_lock(&x->x_mutex);
        aoo_sink_setup(x->x_aoo_sink, &x->x_settings);
        pthread_mutex_unlock(&x->x_mutex);
    }
}

static void aoo_receive_timefilter(t_aoo_receive *x, t_floatarg f)
{
    x->x_settings.time_filter_bandwidth = f;
    if (x->x_settings.blocksize){
        pthread_mutex_lock(&x->x_mutex);
        aoo_sink_setup(x->x_aoo_sink, &x->x_settings);
        pthread_mutex_unlock(&x->x_mutex);
    }
}

int aoo_parseresend(void *x, aoo_sink_settings *s, int argc, t_atom *argv);

static void aoo_receive_resend(t_aoo_receive *x, t_symbol *s, int argc, t_atom *argv)
{
    if (!aoo_parseresend(x, &x->x_settings, argc, argv)){
        return;
    }
    if (x->x_settings.blocksize){
        pthread_mutex_lock(&x->x_mutex);
        aoo_sink_setup(x->x_aoo_sink, &x->x_settings);
        pthread_mutex_unlock(&x->x_mutex);
    }
}

static void aoo_receive_listen(t_aoo_receive *x, t_floatarg f)
{
    int port = f;
    if (x->x_listener){
        if (x->x_listener->port == port){
            return;
        }
        // release old listener
        socket_listener_release(x->x_listener, x);
    }
    // add new listener
    if (port){
        x->x_listener = socket_listener_add(x, f);
        if (x->x_listener){
            post("listening on port %d", x->x_listener->port);
        }
    } else {
        // stop listening
        x->x_listener = 0;
    }
}

static void aoo_receive_tick(t_aoo_receive *x)
{
    for (int i = 0; i < x->x_numevents; ++i){
        if (x->x_eventbuf[i].type == AOO_SOURCE_STATE_EVENT){
            aoo_source_state_event *e = &x->x_eventbuf[i].source_state;

            t_client *client = (t_client *)e->endpoint;
            struct sockaddr_in *addr = (struct sockaddr_in *)&client->addr;

            t_atom msg[4];
            const char *host = inet_ntoa(addr->sin_addr);
            int port = ntohs(addr->sin_port);
            if (!host){
                fprintf(stderr, "inet_ntoa failed!\n");
                continue;
            }
            SETSYMBOL(&msg[0], gensym(host));
            SETFLOAT(&msg[1], port);
            SETFLOAT(&msg[2], e->id);
            SETFLOAT(&msg[3], e->state);
            outlet_anything(x->x_eventout, gensym("source"), 4, msg);
        }
    }
    x->x_numevents = 0;
}

static void aoo_receive_process(t_aoo_receive *x, const aoo_sample **data, int32_t n,
                                const aoo_event *events, int32_t nevents)
{
    assert(sizeof(t_sample) == sizeof(aoo_sample));
    // copy samples
    for (int i = 0; i < x->x_settings.nchannels; ++i){
        memcpy(x->x_vec[i], data[i], sizeof(aoo_sample) * n);
    }
    // handle events
    if (nevents > 0){
        // resize event buffer if necessary
        if (nevents > x->x_eventbufsize){
            x->x_eventbuf = (aoo_event *)resizebytes(x->x_eventbuf,
                sizeof(aoo_event) * x->x_eventbufsize, sizeof(aoo_event) * nevents);
            x->x_eventbufsize = nevents;
        }
        // copy events
        for (int i = 0; i < nevents; ++i){
            x->x_eventbuf[i] = events[i];
        }
        x->x_numevents = nevents;

        clock_delay(x->x_clock, 0);
    }
}

uint64_t aoo_pd_osctime(int n, t_float sr);

static t_int * aoo_receive_perform(t_int *w)
{
    t_aoo_receive *x = (t_aoo_receive *)(w[1]);
    int n = (int)(w[2]);

    uint64_t t = aoo_pd_osctime(n, x->x_settings.samplerate);
    if (!aoo_sink_process(x->x_aoo_sink, t)){
        // output zeros
        for (int i = 0; i < x->x_settings.nchannels; ++i){
            memset(x->x_vec[i], 0, sizeof(t_float) * n);
        }
    }

    return w + 3;
}

static void aoo_receive_dsp(t_aoo_receive *x, t_signal **sp)
{
    int n = x->x_settings.blocksize = (int)sp[0]->s_n;
    x->x_settings.samplerate = sp[0]->s_sr;

    for (int i = 0; i < x->x_settings.nchannels; ++i){
        x->x_vec[i] = sp[i]->s_vec;
    }

    pthread_mutex_lock(&x->x_mutex);
    aoo_sink_setup(x->x_aoo_sink, &x->x_settings);
    pthread_mutex_unlock(&x->x_mutex);

    dsp_add(aoo_receive_perform, 2, (t_int)x, (t_int)n);
}

static void * aoo_receive_new(t_symbol *s, int argc, t_atom *argv)
{
    t_aoo_receive *x = (t_aoo_receive *)pd_new(aoo_receive_class);

    x->x_f = 0;
    x->x_listener = 0;
    pthread_mutex_init(&x->x_mutex, 0);
    // pre-allocate event buffer
    x->x_eventbuf = getbytes(sizeof(aoo_event) * 16);
    x->x_eventbufsize = 16;
    x->x_numevents = 0;
    x->x_clock = clock_new(x, (t_method)aoo_receive_tick);
    // default settings
    memset(&x->x_settings, 0, sizeof(aoo_sink_settings));
    x->x_settings.userdata = x;
    x->x_settings.processfn = (aoo_processfn)aoo_receive_process;
    x->x_settings.resend_limit = AOO_RESEND_LIMIT;
    x->x_settings.resend_interval = AOO_RESEND_INTERVAL;
    x->x_settings.resend_maxnumframes = AOO_RESEND_MAXNUMFRAMES;
    x->x_settings.resend_packetsize = AOO_RESEND_PACKETSIZE;

    // arg #1: ID
    int id = atom_getfloatarg(0, argc, argv);
    x->x_id = id >= 0 ? id : 0;
    x->x_aoo_sink = aoo_sink_new(x->x_id);

    // arg #2: num channels
    int nchannels = atom_getfloatarg(1, argc, argv);
    if (nchannels < 1){
        nchannels = 1;
    }
    x->x_settings.nchannels = nchannels;

    // arg #3: port number
    if (argc > 2){
        aoo_receive_listen(x, atom_getfloat(argv + 2));
    }

    // arg #4: buffer size (ms)
    aoo_receive_buffersize(x, argc > 3 ? atom_getfloat(argv + 3) : DEFBUFSIZE);

    // make signal outlets
    for (int i = 0; i < nchannels; ++i){
        outlet_new(&x->x_obj, &s_signal);
    }
    x->x_vec = (t_sample **)getbytes(sizeof(t_sample *) * nchannels);

    // event outlet
    x->x_eventout = outlet_new(&x->x_obj, 0);

    return x;
}

static void aoo_receive_free(t_aoo_receive *x)
{
    if (x->x_listener){
        socket_listener_release(x->x_listener, x);
    }
    // clean up
    freebytes(x->x_vec, sizeof(t_sample *) * x->x_settings.nchannels);
    freebytes(x->x_eventbuf, sizeof(aoo_event) * x->x_eventbufsize);
    clock_free(x->x_clock);

    aoo_sink_free(x->x_aoo_sink);

    pthread_mutex_destroy(&x->x_mutex);
}

void aoo_receive_tilde_setup(void)
{
    socket_listener_setup();

    aoo_receive_class = class_new(gensym("aoo_receive~"), (t_newmethod)(void *)aoo_receive_new,
        (t_method)aoo_receive_free, sizeof(t_aoo_receive), 0, A_GIMME, A_NULL);
    class_addmethod(aoo_receive_class, (t_method)aoo_receive_dsp, gensym("dsp"), A_CANT, A_NULL);
    class_addmethod(aoo_receive_class, (t_method)aoo_receive_listen, gensym("listen"), A_FLOAT, A_NULL);
    class_addmethod(aoo_receive_class, (t_method)aoo_receive_buffersize,
                    gensym("bufsize"), A_FLOAT, A_NULL);
    class_addmethod(aoo_receive_class, (t_method)aoo_receive_timefilter,
                    gensym("timefilter"), A_FLOAT, A_NULL);
    class_addmethod(aoo_receive_class, (t_method)aoo_receive_resend,
                    gensym("resend"), A_GIMME, A_NULL);

    aoo_setup();
}
