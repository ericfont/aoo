#include "m_pd.h"
#include "aoo/aoo.h"

#include <string.h>
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <errno.h>

#ifdef _WIN32
#include <winsock2.h>
typedef int socklen_t;
#else
#include <sys/socket.h>
#include <sys/select.h>
#include <unistd.h>
#include <netdb.h>
#endif

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

#define DEFBUFSIZE 10

static t_class *aoo_send_class;

typedef struct _aoo_send
{
    t_object x_obj;
    t_float x_f;
    aoo_source *x_aoo_source;
    aoo_format x_format;
    t_float **x_vec;
    t_atom x_sink_id_arg;
    int32_t x_sink_id;
    int32_t x_sink_chn;
    // socket
    int x_socket;
    struct sockaddr_in x_addr;
    // threading
    pthread_t x_thread;
    pthread_cond_t x_cond;
    pthread_mutex_t x_mutex;
} t_aoo_send;

static void aoo_send_channel(t_aoo_send *x, t_floatarg f)
{
    if (f >= 0){
        pthread_mutex_lock(&x->x_mutex);
        aoo_source_setsinkchannel(x->x_aoo_source, x, x->x_sink_id, f);
        x->x_sink_chn = f;
        pthread_mutex_unlock(&x->x_mutex);
    }
}

static void aoo_send_packetsize(t_aoo_send *x, t_floatarg f)
{
    pthread_mutex_lock(&x->x_mutex);
    aoo_source_setpacketsize(x->x_aoo_source, f);
    pthread_mutex_unlock(&x->x_mutex);
}

static void aoo_send_reply(t_aoo_send *x, const char *data, int32_t n)
{
    // called while holding the lock (socket might close or address might change!)
    if (x->x_socket >= 0 && x->x_addr.sin_family == AF_INET){
        if (sendto(x->x_socket, data, n, 0,
                   (const struct sockaddr *)&x->x_addr, sizeof(x->x_addr)) < 0){
            socket_error_print("sendto");
        }
    }
}

void *aoo_send_threadfn(void *y)
{
    t_aoo_send *x = (t_aoo_send *)y;

    pthread_mutex_lock(&x->x_mutex);
    while (x->x_socket >= 0){
        // send all available outgoing packets
        while (aoo_source_send(x->x_aoo_source)) ;
        // check for pending incoming packets
        while (1){
            // non-blocking receive via select()
            struct timeval tv;
            tv.tv_sec = 0;
            tv.tv_usec = 0;
            fd_set rdset;
            FD_ZERO(&rdset);
            FD_SET(x->x_socket, &rdset);
            if (select(x->x_socket + 1, &rdset, 0, 0, &tv) > 0){
                if (FD_ISSET(x->x_socket, &rdset)){
                    // receive packet
                    char buf[AOO_MAXPACKETSIZE];
                    int nbytes = recv(x->x_socket, buf, AOO_MAXPACKETSIZE, 0);
                    if (nbytes > 0){
                        aoo_source_handlemessage(x->x_aoo_source, buf, nbytes,
                                                 x, (aoo_replyfn)aoo_send_reply);
                        continue; // check for more
                    }
                }
            }
            break;
        }
        // wait for more
        pthread_cond_wait(&x->x_cond, &x->x_mutex);
    }
    pthread_mutex_unlock(&x->x_mutex);

    return 0;
}

static void aoo_send_set(t_aoo_send *x, t_symbol *s, int argc, t_atom *argv)
{
    if (argc){
        pthread_mutex_lock(&x->x_mutex);
        // remove old sink
        aoo_source_removeall(x->x_aoo_source);
        // add new sink
        if (argv->a_type == A_SYMBOL){
            if (*argv->a_w.w_symbol->s_name == '*'){
                aoo_source_addsink(x->x_aoo_source, x, AOO_ID_WILDCARD, (aoo_replyfn)aoo_send_reply);
            } else {
                return;
            }
            x->x_sink_id = AOO_ID_WILDCARD;
        } else {
            int32_t id = atom_getfloat(argv);
            aoo_source_addsink(x->x_aoo_source, x, id, (aoo_replyfn)aoo_send_reply);
            x->x_sink_id = id;
        }
        pthread_mutex_unlock(&x->x_mutex);

        aoo_send_channel(x, atom_getfloatarg(1, argc, argv));
    }
}

static void aoo_send_clear(t_aoo_send *x)
{
    pthread_mutex_lock(&x->x_mutex);
    aoo_source_removeall(x->x_aoo_source);
    pthread_mutex_unlock(&x->x_mutex);
}

uint64_t aoo_pd_osctime(int n, t_float sr);

static t_int * aoo_send_perform(t_int *w)
{
    t_aoo_send *x = (t_aoo_send *)(w[1]);
    int n = (int)(w[2]);

    assert(sizeof(t_sample) == sizeof(aoo_sample));

    if (x->x_addr.sin_family == AF_INET){
        uint64_t t = aoo_pd_osctime(n, x->x_format.samplerate);
        if (aoo_source_process(x->x_aoo_source, (const aoo_sample **)x->x_vec, n, t)){
            pthread_cond_signal(&x->x_cond);
        }
    }
    return w + 3;
}

static void aoo_send_dsp(t_aoo_send *x, t_signal **sp)
{
    pthread_mutex_lock(&x->x_mutex);
    x->x_format.blocksize = sp[0]->s_n;
    x->x_format.samplerate = sp[0]->s_sr;
    aoo_source_setformat(x->x_aoo_source, &x->x_format);
    pthread_mutex_unlock(&x->x_mutex);

    for (int i = 0; i < x->x_format.nchannels; ++i){
        x->x_vec[i] = sp[i]->s_vec;
    }

    dsp_add(aoo_send_perform, 2, (t_int)x, (t_int)sp[0]->s_n);
}

static void aoo_send_loadbang(t_aoo_send *x, t_floatarg f)
{
    // LB_LOAD
    if (f == 0){
        if (x->x_sink_id_arg.a_type != A_NULL){
            // set sink ID
            aoo_send_set(x, 0, 1, &x->x_sink_id_arg);
            aoo_send_channel(x, x->x_sink_chn);
        }
    }
}

void aoo_send_disconnect(t_aoo_send *x)
{
    pthread_mutex_lock(&x->x_mutex);
    memset(&x->x_addr, 0, sizeof(x->x_addr));
    pthread_mutex_unlock(&x->x_mutex);
}

void aoo_send_connect(t_aoo_send *x, t_symbol *s, int argc, t_atom *argv)
{
    t_symbol *hostname = atom_getsymbolarg(0, argc, argv);
    int port = atom_getfloatarg(1, argc, argv);

    if (x->x_socket < 0){
        pd_error(x, "can't connect - no socket!");
    }

    if (port <= 0){
        post("bad port number %d", port);
        return;
    }

    struct hostent *he = gethostbyname(hostname->s_name);
    if (he){
        pthread_mutex_lock(&x->x_mutex);
        memcpy(&x->x_addr.sin_addr, he->h_addr_list[0], he->h_length);
        x->x_addr.sin_family = AF_INET;
        x->x_addr.sin_port = htons(port);
        pthread_mutex_unlock(&x->x_mutex);

        post("connected to %s on port %d", he->h_name, port);
    } else {
        aoo_send_disconnect(x);

        error("couldn't resolve hostname '%s'", hostname->s_name);
    }
}

static void * aoo_send_new(t_symbol *s, int argc, t_atom *argv)
{
    t_aoo_send *x = (t_aoo_send *)pd_new(aoo_send_class);

    memset(&x->x_addr, 0, sizeof(x->x_addr));
    x->x_socket = socket(AF_INET, SOCK_DGRAM, 0);
    if (x->x_socket >= 0){
        int val = 1;
        if (setsockopt(x->x_socket, SOL_SOCKET, SO_BROADCAST, (void *)&val, sizeof(val))){
            error("couldn't set SO_BROADCAST");
        }
    } else {
        socket_error_print("socket");
    }
    pthread_mutex_init(&x->x_mutex, 0);
    pthread_cond_init(&x->x_cond, 0);

    // arg #1: ID
    int src = atom_getfloatarg(0, argc, argv);
    x->x_aoo_source = aoo_source_new(src >= 0 ? src : 0);
    aoo_source_setbuffersize(x->x_aoo_source, DEFBUFSIZE);

    // arg #2: num channels
    int nchannels = atom_getfloatarg(1, argc, argv);
    memset(&x->x_format, 0, sizeof(x->x_format));
    x->x_format.nchannels = nchannels > 1 ? nchannels : 1;
    // LATER make this settable
    x->x_format.mime_type = AOO_MIME_PCM;
    x->x_format.bitdepth = AOO_FLOAT32;

    // arg #3: sink ID
    x->x_sink_id = -1;
    if (argc > 2){
        x->x_sink_id_arg.a_type = argv[2].a_type;
        x->x_sink_id_arg.a_w = argv[2].a_w;
    } else {
        x->x_sink_id_arg.a_type = A_NULL;
    }

    // arg #4: sink channel
    x->x_sink_chn = atom_getfloatarg(3, argc, argv);

    // make additional inlets
    if (nchannels > 1){
        while (--nchannels){
            inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);
        }
    }
    x->x_vec = (t_sample **)getbytes(sizeof(t_sample *) * x->x_format.nchannels);

    pthread_create(&x->x_thread, 0, aoo_send_threadfn, x);

    return x;
}

static void aoo_send_free(t_aoo_send *x)
{
    pthread_mutex_lock(&x->x_mutex);
    socket_close(x->x_socket);
    x->x_socket = -1;
    pthread_mutex_unlock(&x->x_mutex);

    // notify thread and join
    pthread_cond_signal(&x->x_cond);
    pthread_join(x->x_thread, 0);

    pthread_mutex_destroy(&x->x_mutex);
    pthread_condattr_destroy(&x->x_cond);

    aoo_source_free(x->x_aoo_source);

    freebytes(x->x_vec, sizeof(t_sample *) * x->x_format.nchannels);
}

void aoo_send_tilde_setup(void)
{
    aoo_send_class = class_new(gensym("aoo_send~"), (t_newmethod)(void *)aoo_send_new,
        (t_method)aoo_send_free, sizeof(t_aoo_send), 0, A_GIMME, A_NULL);
    CLASS_MAINSIGNALIN(aoo_send_class, t_aoo_send, x_f);
    class_addmethod(aoo_send_class, (t_method)aoo_send_dsp, gensym("dsp"), A_CANT, A_NULL);
    class_addmethod(aoo_send_class, (t_method)aoo_send_loadbang, gensym("loadbang"), A_FLOAT, A_NULL);
    class_addmethod(aoo_send_class, (t_method)aoo_send_connect, gensym("connect"), A_GIMME, A_NULL);
    class_addmethod(aoo_send_class, (t_method)aoo_send_disconnect, gensym("disconnect"), A_NULL);
    class_addmethod(aoo_send_class, (t_method)aoo_send_set, gensym("set"), A_GIMME, A_NULL);
    class_addmethod(aoo_send_class, (t_method)aoo_send_channel, gensym("channel"), A_FLOAT, A_NULL);
    class_addmethod(aoo_send_class, (t_method)aoo_send_packetsize, gensym("packetsize"), A_FLOAT, A_NULL);
    class_addmethod(aoo_send_class, (t_method)aoo_send_clear, gensym("clear"), A_NULL);
}