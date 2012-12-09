/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering
  Copyright 2008 Colin Guthrie

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2.1 of the License,
  or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with PulseAudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/ioctl.h>

#ifdef HAVE_LINUX_SOCKIOS_H
#include <linux/sockios.h>
#endif

#include <pulse/rtclock.h>
#include <pulse/timeval.h>
#include <pulse/xmalloc.h>

#include <pulsecore/core-error.h>
#include <pulsecore/sink.h>
#include <pulsecore/module.h>
#include <pulsecore/core-util.h>
#include <pulsecore/modargs.h>
#include <pulsecore/log.h>
#include <pulsecore/socket-client.h>
#include <pulsecore/thread-mq.h>
#include <pulsecore/thread.h>
#include <pulsecore/time-smoother.h>
#include <pulsecore/poll.h>

#include "module-raop-sink-symdef.h"
#include "rtp.h"
#include "sdp.h"
#include "sap.h"
#include "raop_client.h"

PA_MODULE_AUTHOR("Colin Guthrie");
PA_MODULE_DESCRIPTION("RAOP Sink");
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(FALSE);
PA_MODULE_USAGE(
        "sink_name=<name for the sink> "
        "sink_properties=<properties for the sink> "
        "server=<address> "
        "protocol=<transport protocol> "
        "encryption=<encryption type> "
        "codec=<audio codec> "
        "channels=<number of channels> "
        "format=<sample format> "
        "rate=<sample rate> ");

struct userdata {
    pa_core *core;
    pa_module *module;
    pa_sink *sink;

    pa_thread *thread;
    pa_thread_mq thread_mq;
    pa_rtpoll *rtpoll;

    pa_memchunk raw_memchunk;
    pa_memchunk encoded_memchunk;

    void *write_data;
    size_t write_length, write_index;

    void *read_data;
    size_t read_length, read_index;

    pa_usec_t latency;

    unsigned int rate;

    pa_rtpoll_item *raop_rtpoll_item;

    pa_smoother *smoother;
    int fd;

    int64_t offset;
    int64_t encoding_overhead;
    int32_t next_encoding_overhead;
    double encoding_ratio;

    pa_raop_client *raop;

    size_t block_size;
};

static const char* const valid_modargs[] = {
    "sink_name",
    "sink_properties",
    "server",
    "protocol",
    "encryption",
    "codec",
    "channels",
    "format",
    "rate",
    NULL
};

enum {
    SINK_MESSAGE_SETUP = PA_SINK_MESSAGE_MAX,
    SINK_MESSAGE_RECORD,
    SINK_MESSAGE_DISCONNECTED
};

static pa_usec_t sink_get_latency(struct userdata *u) {
    pa_usec_t r;
    int64_t delay;
    pa_usec_t now1, now2;

    pa_assert(u);

    now1 = pa_rtclock_now();
    now2 = pa_smoother_get(u->smoother, now1);

    delay = (int64_t) pa_bytes_to_usec(u->offset
                                       - u->encoding_overhead
                                       + (u->encoded_memchunk.length / u->encoding_ratio),
                                       &u->sink->sample_spec)
                      - (int64_t) now2;

    r = delay >= 0 ? (pa_usec_t) delay : 0;

    return r;
}

static int sink_process_msg(pa_msgobject *o, int code, void *data, int64_t offset, pa_memchunk *chunk) {
    struct userdata *u = PA_SINK(o)->userdata;

    switch (code) {
        case PA_SINK_MESSAGE_SET_STATE:
            switch ((pa_sink_state_t) PA_PTR_TO_UINT(data)) {
                case PA_SINK_SUSPENDED:
                    pa_assert(PA_SINK_IS_OPENED(u->sink->thread_info.state));

                    pa_smoother_pause(u->smoother, pa_rtclock_now());

                    /* Issue a FLUSH if we are connected. */
                    if (pa_raop_client_can_stream(u->raop))
                        pa_raop_client_teardown(u->raop);

                    break;

                case PA_SINK_IDLE:
                    if (u->sink->thread_info.state == PA_SINK_RUNNING) {
                        pa_rtpoll_set_timer_disabled(u->rtpoll);
                        pa_raop_client_flush(u->raop);
                    }

                    break;

                case PA_SINK_RUNNING:
                    if (u->sink->thread_info.state == PA_SINK_SUSPENDED)
                        pa_smoother_resume(u->smoother, pa_rtclock_now(), TRUE);

                    /* The connection can be closed when idle, so check to
                     * see if we need to reestablish it. */
                    if (!pa_raop_client_can_stream(u->raop))
                        pa_raop_client_connect(u->raop);

                    break;

                case PA_SINK_UNLINKED:
                case PA_SINK_INIT:
                case PA_SINK_INVALID_STATE:
                    ;
            }

            break;

        case PA_SINK_MESSAGE_GET_LATENCY: {
            pa_usec_t r = 0;

            if (pa_raop_client_can_stream(u->raop))
                r = sink_get_latency(u);

            *((pa_usec_t*) data) = r;

            return 0;
        }

        case SINK_MESSAGE_RECORD: {
            pa_rtpoll_set_timer_relative(u->rtpoll, pa_bytes_to_usec(u->block_size, &u->sink->sample_spec));

            if (u->sink->thread_info.state == PA_SINK_SUSPENDED) {
                /* Our stream has been suspended so we just flush it... */
                pa_raop_client_flush(u->raop);
            }

            return 0;
        }

        case SINK_MESSAGE_DISCONNECTED: {
            if (u->sink->thread_info.state == PA_SINK_SUSPENDED) {
                pa_log_debug("RTSP control connection closed, but we're suspended so let's not worry about it... we'll open it again later");

                pa_rtpoll_set_timer_disabled(u->rtpoll);
                if (u->raop_rtpoll_item)
                    pa_rtpoll_item_free(u->raop_rtpoll_item);
                u->raop_rtpoll_item = NULL;
            } else {
                /* Question: is this valid here: or should we do some sort of:
                 * return pa_sink_process_msg(PA_MSGOBJECT(u->core), PA_CORE_MESSAGE_UNLOAD_MODULE, u->module, 0, NULL); ?? */
                pa_module_unload_request(u->module, TRUE);
            }

            return 0;
        }
    }

    return pa_sink_process_msg(o, code, data, offset, chunk);
}

static void sink_set_volume_cb(pa_sink *s) {
    struct userdata *u = s->userdata;
    pa_cvolume hw;
    pa_volume_t v;
    char t[PA_CVOLUME_SNPRINT_MAX];

    pa_assert(u);

    /* If we're muted we don't need to do anything. */
    if (s->muted)
        return;

    /* Calculate the max volume of all channels.
     * We'll use this as our (single) volume on the APEX device and emulate
     * any variation in channel volumes in software. */
    v = pa_cvolume_max(&s->real_volume);

    /* Create a pa_cvolume version of our single value. */
    pa_cvolume_set(&hw, s->sample_spec.channels, v);

    /* Perform any software manipulation of the volume needed. */
    pa_sw_cvolume_divide(&s->soft_volume, &s->real_volume, &hw);

    pa_log_debug("Requested volume: %s", pa_cvolume_snprint(t, sizeof(t), &s->real_volume));
    pa_log_debug("Got hardware volume: %s", pa_cvolume_snprint(t, sizeof(t), &hw));
    pa_log_debug("Calculated software volume: %s", pa_cvolume_snprint(t, sizeof(t), &s->soft_volume));

    /* Any necessary software volume manipulation is done so set
     * our hw volume (or v as a single value) on the device. */
    pa_raop_client_set_volume(u->raop, v);
}

static void sink_set_mute_cb(pa_sink *s) {
    struct userdata *u = s->userdata;

    pa_assert(u);

    if (s->muted) {
        pa_raop_client_set_volume(u->raop, PA_VOLUME_MUTED);
    } else {
        sink_set_volume_cb(s);
    }
}

static void raop_record_cb(void *userdata) {
    struct userdata *u = userdata;

    pa_assert(u);

    /* Set the initial volume. */
    sink_set_volume_cb(u->sink);

    pa_log_debug("Connection authenticated, handing fd to IO thread...");

    pa_asyncmsgq_post(u->thread_mq.inq, PA_MSGOBJECT(u->sink), SINK_MESSAGE_RECORD, NULL, 0, NULL, NULL);
}

static void raop_disconnected_cb(void *userdata) {
    struct userdata *u = userdata;

    pa_assert(u);

    pa_log_debug("Connection closed, informing IO thread...");

    pa_asyncmsgq_post(u->thread_mq.inq, PA_MSGOBJECT(u->sink), SINK_MESSAGE_DISCONNECTED, NULL, 0, NULL, NULL);
}

static void thread_func(void *userdata) {
    struct userdata *u = userdata;
    int write_type = 0;
    pa_memchunk silence;
    uint32_t silence_overhead = 0;
    double silence_ratio = 0;

    pa_assert(u);

    pa_log_debug("Thread starting up");

    pa_thread_mq_install(&u->thread_mq);

    pa_smoother_set_time_offset(u->smoother, pa_rtclock_now());

    /* Create a chunk of memory that is our encoded silence sample. */
    pa_memchunk_reset(&silence);

    for (;;) {
        int ret;

        if (PA_SINK_IS_OPENED(u->sink->thread_info.state))
            if (u->sink->thread_info.rewind_requested)
                pa_sink_process_rewind(u->sink, 0);

        if (u->raop_rtpoll_item) {
            struct pollfd *pollfd;
            pollfd = pa_rtpoll_item_get_pollfd(u->raop_rtpoll_item, NULL);

            /* Render some data and write it to the fifo. */
            if (/*PA_SINK_IS_OPENED(u->sink->thread_info.state) && */pollfd->revents) {
                pa_usec_t usec;
                int64_t n;
                void *p;

                if (!silence.memblock) {
                    pa_memchunk silence_tmp;

                    pa_memchunk_reset(&silence_tmp);
                    silence_tmp.memblock = pa_memblock_new(u->core->mempool, 4096);
                    silence_tmp.length = 4096;
                    p = pa_memblock_acquire(silence_tmp.memblock);
                      memset(p, 0, 4096);
                    pa_memblock_release(silence_tmp.memblock);
                    pa_raop_client_encode_sample(u->raop, &silence_tmp, &silence);
                    pa_assert(0 == silence_tmp.length);
                    silence_overhead = silence_tmp.length - 4096;
                    silence_ratio = silence_tmp.length / 4096;
                    pa_memblock_unref(silence_tmp.memblock);
                }

                for (;;) {
                    ssize_t l;

                    if (u->encoded_memchunk.length <= 0) {
                        if (u->encoded_memchunk.memblock)
                            pa_memblock_unref(u->encoded_memchunk.memblock);
                        if (PA_SINK_IS_OPENED(u->sink->thread_info.state)) {
                            size_t rl;

                            /* We render real data. */
                            if (u->raw_memchunk.length <= 0) {
                                if (u->raw_memchunk.memblock)
                                    pa_memblock_unref(u->raw_memchunk.memblock);
                                pa_memchunk_reset(&u->raw_memchunk);

                                /* Grab unencoded data. */
                                pa_sink_render(u->sink, u->block_size, &u->raw_memchunk);
                            }
                            pa_assert(u->raw_memchunk.length > 0);

                            /* Encode it. */
                            rl = u->raw_memchunk.length;
                            u->encoding_overhead += u->next_encoding_overhead;
                            pa_raop_client_encode_sample(u->raop, &u->raw_memchunk, &u->encoded_memchunk);
                            u->next_encoding_overhead = (u->encoded_memchunk.length - (rl - u->raw_memchunk.length));
                            u->encoding_ratio = u->encoded_memchunk.length / (rl - u->raw_memchunk.length);
                        } else {
                            /* We render some silence into our memchunk. */
                            memcpy(&u->encoded_memchunk, &silence, sizeof(pa_memchunk));
                            pa_memblock_ref(silence.memblock);

                            /* Calculate/store some values to be used with the smoother. */
                            u->next_encoding_overhead = silence_overhead;
                            u->encoding_ratio = silence_ratio;
                        }
                    }
                    pa_assert(u->encoded_memchunk.length > 0);

                    p = pa_memblock_acquire(u->encoded_memchunk.memblock);
                    l = pa_write(u->fd, (uint8_t*) p + u->encoded_memchunk.index, u->encoded_memchunk.length, &write_type);
                    pa_memblock_release(u->encoded_memchunk.memblock);

                    pa_assert(l != 0);

                    if (l < 0) {

                        if (errno == EINTR)
                            continue;
                        else if (errno == EAGAIN) {

                            /* OK, we filled all socket buffers up now. */
                            goto filled_up;

                        } else {
                            pa_log("Failed to write data to FIFO: %s", pa_cstrerror(errno));
                            goto fail;
                        }

                    } else {
                        u->offset += l;

                        u->encoded_memchunk.index += l;
                        u->encoded_memchunk.length -= l;

                        pollfd->revents = 0;

                        if (u->encoded_memchunk.length > 0) {
                            /* We've completely written the encoded data, so update our overhead. */
                            u->encoding_overhead += u->next_encoding_overhead;

                            /* OK, we wrote less that we asked for,
                             * hence we can assume that the socket
                             * buffers are full now. */
                            goto filled_up;
                        }
                    }
                }

            filled_up:
                /* At this spot we know that the socket buffers are
                 * fully filled up. This is the best time to estimate
                 * the playback position of the server. */

                n = u->offset - u->encoding_overhead;

#ifdef SIOCOUTQ
                {
                    int l;
                    if (ioctl(u->fd, SIOCOUTQ, &l) >= 0 && l > 0)
                        n -= (l / u->encoding_ratio);
                }
#endif

                usec = pa_bytes_to_usec(n, &u->sink->sample_spec);

                if (usec > u->latency)
                    usec -= u->latency;
                else
                    usec = 0;

                pa_smoother_put(u->smoother, pa_rtclock_now(), usec);
            }

            /* Hmm, nothing to do. Let's sleep... */
            pollfd->events = POLLOUT; /*PA_SINK_IS_OPENED(u->sink->thread_info.state)  ? POLLOUT : 0;*/
        }

        if ((ret = pa_rtpoll_run(u->rtpoll, TRUE)) < 0)
            goto fail;

        if (ret == 0)
            goto finish;

        if (u->raop_rtpoll_item) {
            struct pollfd* pollfd;

            pollfd = pa_rtpoll_item_get_pollfd(u->raop_rtpoll_item, NULL);

            if (pollfd->revents & ~POLLOUT) {
                if (u->sink->thread_info.state != PA_SINK_SUSPENDED) {
                    pa_log("FIFO shutdown.");
                    goto fail;
                }

                /* We expect this to happen on occasion if we are not sending data.
                 * It's perfectly natural and normal. */
                if (u->raop_rtpoll_item)
                    pa_rtpoll_item_free(u->raop_rtpoll_item);
                u->raop_rtpoll_item = NULL;
            }
        }
    }

fail:
    /* If this was no regular exit from the loop we have to continue
     * processing messages until we received PA_MESSAGE_SHUTDOWN. */
    pa_asyncmsgq_post(u->thread_mq.outq, PA_MSGOBJECT(u->core), PA_CORE_MESSAGE_UNLOAD_MODULE, u->module, 0, NULL, NULL);
    pa_asyncmsgq_wait_for(u->thread_mq.inq, PA_MESSAGE_SHUTDOWN);

finish:
    if (silence.memblock)
        pa_memblock_unref(silence.memblock);
    pa_log_debug("Thread shutting down.");
}

int pa__init(pa_module *m) {
    struct userdata *u = NULL;
    pa_sample_spec ss;
    pa_modargs *ma = NULL;
    const char *server;
    pa_sink_new_data data;
    char *t;

    pa_assert(m);

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log("Failed to parse module arguments.");
        goto fail;
    }

    ss = m->core->default_sample_spec;
    if (pa_modargs_get_sample_spec(ma, &ss) < 0) {
        pa_log("Invalid sample format specification.");
        goto fail;
    }

    if ((/*ss.format != PA_SAMPLE_U8 &&*/ ss.format != PA_SAMPLE_S16NE) ||
        (ss.channels > 2)) {
        pa_log("Sample type support is limited to mono/stereo and S16NE sample data.");
        goto fail;
    }

    if (!(server = pa_modargs_get_value(ma, "server", NULL))) {
        pa_log("No server argument given.");
        goto fail;
    }

    u = pa_xnew0(struct userdata, 1);
    u->core = m->core;
    u->module = m;
    m->userdata = u;

    pa_memchunk_reset(&u->raw_memchunk);
    pa_memchunk_reset(&u->encoded_memchunk);

    u->smoother = pa_smoother_new(
            PA_USEC_PER_SEC,
            PA_USEC_PER_SEC*2,
            TRUE,
            TRUE,
            10,
            0,
            FALSE);
    u->fd = -1;

    u->offset = 0;
    u->encoding_overhead = 0;
    u->next_encoding_overhead = 0;
    u->encoding_ratio = 1.0;

    u->rtpoll = pa_rtpoll_new();
    pa_thread_mq_init(&u->thread_mq, m->core->mainloop, u->rtpoll);
    u->raop_rtpoll_item = NULL;

    u->rate = ss.rate;

    u->block_size = pa_usec_to_bytes(PA_USEC_PER_SEC / 20, &ss);

    u->read_data = u->write_data = NULL;
    u->write_index = u->write_length = 0;
    u->read_index = u->read_length = 0;

    u->latency = 0;

    /* This may be overwriten if sink_name is speciffied in module arguments. */
    t = pa_sprintf_malloc("raop_client.%s", server);

    pa_sink_new_data_init(&data);
    data.driver = __FILE__;
    data.module = m;
    pa_sink_new_data_set_name(&data, pa_modargs_get_value(ma, "sink_name", t));
    pa_sink_new_data_set_sample_spec(&data, &ss);
    pa_proplist_sets(data.proplist, PA_PROP_DEVICE_STRING, server);
    pa_proplist_sets(data.proplist, PA_PROP_DEVICE_INTENDED_ROLES, "music");
    pa_proplist_setf(data.proplist, PA_PROP_DEVICE_DESCRIPTION, "RAOP sink '%s'", server);

    /* RAOP discover module will eventually overwrite sink_name and others (PA_UPDATE_REPLACE). */
    if (pa_modargs_get_proplist(ma, "sink_properties", data.proplist, PA_UPDATE_REPLACE) < 0) {
        pa_log("Invalid properties.");
        pa_sink_new_data_done(&data);
        pa_xfree(t);
        goto fail;
    }

    pa_xfree(t);

    u->sink = pa_sink_new(m->core, &data, PA_SINK_NETWORK | PA_SINK_LATENCY);
    pa_sink_new_data_done(&data);

    if (!u->sink) {
        pa_log("Failed to create sink object.");
        goto fail;
    }

    u->sink->parent.process_msg = sink_process_msg;
    pa_sink_set_set_volume_callback(u->sink, sink_set_volume_cb);
    pa_sink_set_set_mute_callback(u->sink, sink_set_mute_cb);

    pa_sink_set_asyncmsgq(u->sink, u->thread_mq.inq);
    pa_sink_set_rtpoll(u->sink, u->rtpoll);
    u->sink->userdata = u;

    if (!(u->raop = pa_raop_client_new(u->core, server))) {
        pa_log("Failed to connect to server.");
        goto fail;
    }

    pa_raop_client_set_record_callback(u->raop, raop_record_cb, u);
    pa_raop_client_set_disconnected_callback(u->raop, raop_disconnected_cb, u);

    pa_raop_client_connect(u->raop);

    if (!(u->thread = pa_thread_new("raop-sink", thread_func, u))) {
        pa_log("Failed to create thread.");
        goto fail;
    }

    pa_sink_put(u->sink);

    pa_modargs_free(ma);

    return 0;

fail:
    if (ma)
        pa_modargs_free(ma);

    pa__done(m);

    return -1;
}

int pa__get_n_used(pa_module *m) {
    struct userdata *u;

    pa_assert(m);
    pa_assert_se(u = m->userdata);

    return pa_sink_linked_by(u->sink);
}

void pa__done(pa_module *m) {
    struct userdata *u;
    pa_assert(m);

    if (!(u = m->userdata))
        return;

    if (u->sink)
        pa_sink_unlink(u->sink);

    if (u->thread) {
        pa_asyncmsgq_send(u->thread_mq.inq, NULL, PA_MESSAGE_SHUTDOWN, NULL, 0, NULL);
        pa_thread_free(u->thread);
    }

    pa_thread_mq_done(&u->thread_mq);

    if (u->sink)
        pa_sink_unref(u->sink);

    if (u->raop_rtpoll_item)
        pa_rtpoll_item_free(u->raop_rtpoll_item);

    if (u->rtpoll)
        pa_rtpoll_free(u->rtpoll);

    if (u->raw_memchunk.memblock)
        pa_memblock_unref(u->raw_memchunk.memblock);

    if (u->encoded_memchunk.memblock)
        pa_memblock_unref(u->encoded_memchunk.memblock);

    if (u->raop)
        pa_raop_client_free(u->raop);

    pa_xfree(u->read_data);
    pa_xfree(u->write_data);

    if (u->smoother)
        pa_smoother_free(u->smoother);

    if (u->fd >= 0)
        pa_close(u->fd);

    pa_xfree(u);
}
