#include "m_pd.h"
#include "aoo/aoo.h"

#include <string.h>
#include <assert.h>

#ifdef _WIN32
# include <malloc.h> // MSVC or mingw on windows
# ifdef _MSC_VER
#  define alloca _alloca
# endif
#elif defined(__linux__) || defined(__APPLE__)
# include <alloca.h> // linux, mac, mingw, cygwin
#else
# include <stdlib.h> // BSDs for example
#endif

static t_class *aoo_pack_class;

typedef struct _aoo_pack
{
    t_object x_obj;
    t_float x_f;
    aoo_source *x_aoo_source;
    aoo_format x_format;
    t_float **x_vec;
    t_clock *x_clock;
    t_outlet *x_out;
    t_atom x_sink_id_arg;
    int32_t x_sink_id;
    int32_t x_sink_chn;
} t_aoo_pack;

static void aoo_pack_tick(t_aoo_pack *x)
{
    if (!aoo_source_send(x->x_aoo_source)){
        bug("aoo_pack_tick");
    }
}

static void aoo_pack_reply(t_aoo_pack *x, const char *data, int32_t n)
{
    t_atom *a = (t_atom *)alloca(n * sizeof(t_atom));
    for (int i = 0; i < n; ++i){
        SETFLOAT(&a[i], (unsigned char)data[i]);
    }
    outlet_list(x->x_out, &s_list, n, a);
}

static void aoo_pack_list(t_aoo_pack *x, t_symbol *s, int argc, t_atom *argv)
{
    char *msg = (char *)alloca(argc);
    for (int i = 0; i < argc; ++i){
        msg[i] = (int)(argv[i].a_type == A_FLOAT ? argv[i].a_w.w_float : 0.f);
    }
    aoo_source_handlemessage(x->x_aoo_source, msg, argc, x, (aoo_replyfn)aoo_pack_reply);
}

static void aoo_pack_channel(t_aoo_pack *x, t_floatarg f)
{
    if (f >= 0){
        aoo_source_setsinkchannel(x->x_aoo_source, x, x->x_sink_id, f);
        x->x_sink_chn = f;
    }
}

static void aoo_pack_set(t_aoo_pack *x, t_symbol *s, int argc, t_atom *argv)
{
    if (argc){
        // remove old sink
        aoo_source_removeall(x->x_aoo_source);
        // add new sink
        if (argv->a_type == A_SYMBOL){
            if (*argv->a_w.w_symbol->s_name == '*'){
                aoo_source_addsink(x->x_aoo_source, x, AOO_ID_WILDCARD, (aoo_replyfn)aoo_pack_reply);
            } else {
                return;
            }
            x->x_sink_id = AOO_ID_WILDCARD;
        } else {
            int32_t id = atom_getfloat(argv);
            aoo_source_addsink(x->x_aoo_source, x, id, (aoo_replyfn)aoo_pack_reply);
            x->x_sink_id = id;
        }
        aoo_pack_channel(x, atom_getfloatarg(1, argc, argv));
    }
}

static void aoo_pack_clear(t_aoo_pack *x)
{
    aoo_source_removeall(x->x_aoo_source);
}

static t_int * aoo_pack_perform(t_int *w)
{
    t_aoo_pack *x = (t_aoo_pack *)(w[1]);
    int n = (int)(w[2]);

    assert(sizeof(t_sample) == sizeof(aoo_sample));

    if (aoo_source_process(x->x_aoo_source, (const aoo_sample **)x->x_vec, n)){
        clock_set(x->x_clock, 0);
    }
    return w + 3;
}

static void aoo_pack_dsp(t_aoo_pack *x, t_signal **sp)
{
    x->x_format.blocksize = sp[0]->s_n;
    x->x_format.samplerate = sp[0]->s_sr;
    aoo_source_setformat(x->x_aoo_source, &x->x_format);

    for (int i = 0; i < x->x_format.nchannels; ++i){
        x->x_vec[i] = sp[i]->s_vec;
    }

    dsp_add(aoo_pack_perform, 2, (t_int)x, (t_int)sp[0]->s_n);

    clock_unset(x->x_clock);
}

static void aoo_pack_loadbang(t_aoo_pack *x, t_floatarg f)
{
    // LB_LOAD
    if (f == 0){
        if (x->x_sink_id_arg.a_type != A_NULL){
            // set sink ID
            aoo_pack_set(x, 0, 1, &x->x_sink_id_arg);
            aoo_pack_channel(x, x->x_sink_chn);
        }
    }
}

static void * aoo_pack_new(t_symbol *s, int argc, t_atom *argv)
{
    t_aoo_pack *x = (t_aoo_pack *)pd_new(aoo_pack_class);

    x->x_clock = clock_new(x, (t_method)aoo_pack_tick);

    // arg #1: ID
    int src = atom_getfloatarg(0, argc, argv);
    x->x_aoo_source = aoo_source_new(src >= 0 ? src : 0);

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
    // make message outlet
    x->x_out = outlet_new(&x->x_obj, 0);

    return x;
}

static void aoo_pack_free(t_aoo_pack *x)
{
    // clean up
    freebytes(x->x_vec, sizeof(t_sample *) * x->x_format.nchannels);
    clock_free(x->x_clock);
    aoo_source_free(x->x_aoo_source);
}

void aoo_pack_tilde_setup(void)
{
    aoo_pack_class = class_new(gensym("aoo_pack~"), (t_newmethod)(void *)aoo_pack_new,
        (t_method)aoo_pack_free, sizeof(t_aoo_pack), 0, A_GIMME, A_NULL);
    CLASS_MAINSIGNALIN(aoo_pack_class, t_aoo_pack, x_f);
    class_addmethod(aoo_pack_class, (t_method)aoo_pack_dsp, gensym("dsp"), A_CANT, A_NULL);
    class_addmethod(aoo_pack_class, (t_method)aoo_pack_loadbang, gensym("loadbang"), A_FLOAT, A_NULL);
    class_addlist(aoo_pack_class, (t_method)aoo_pack_list);
    class_addmethod(aoo_pack_class, (t_method)aoo_pack_set, gensym("set"), A_GIMME, A_NULL);
    class_addmethod(aoo_pack_class, (t_method)aoo_pack_channel, gensym("channel"), A_FLOAT, A_NULL);
    class_addmethod(aoo_pack_class, (t_method)aoo_pack_clear, gensym("clear"), A_NULL);
}