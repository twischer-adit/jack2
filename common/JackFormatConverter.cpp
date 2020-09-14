/*
  Copyright (C) 2019 Laxmi Devi <laxmi.devi@in.bosch.com>
  Copyright (C) 2019 Timo Wischer <twischer@de.adit-jv.com>


  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published by
  the Free Software Foundation; either version 2.1 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.

*/

#include <cstring>
#include <vector>
#include <math.h>
#include <jack/jack.h>
#include <jack/format_converter.h>
#include "JackCompilerDeps.h"
#include "JackError.h"

#define SAMPLE_32BIT_SCALING          0x7FFFFFFF
#define SAMPLE_16BIT_SCALING          0x7FFF
#define NORMALIZED_FLOAT_MIN         -1.0f
#define NORMALIZED_FLOAT_MAX          1.0f
#define BUF_ALIGN_BYTES               32L
#define BUF_ALIGN_MASK               ~(BUF_ALIGN_BYTES-1L)       /* Mask to get 32-byte align address */
                                      /* Adding BUF_ALIGN_BYTES, So aligned address in not less than
                                       * base address of buffer */
#define GET_32BYTE_ALIGN_BUFFER(buf)  ((char*)((uintptr_t)buf & BUF_ALIGN_MASK) + BUF_ALIGN_BYTES)

/*ToDo : - Remove C++ wrapper jack_port_create_converter
 * After porting all clients using format convert to C-API
 */

class IJackBufferConverter {
    public:
        virtual ~IJackBufferConverter() {}
};

class  ForwardJackPortConverter: public IJackPortConverter {
    private:
        jack_port_t* const port;
        void* buffer = NULL;
    public:
        ForwardJackPortConverter(jack_port_t* pt) : port(pt) {}

        jack_default_audio_sample_t* get_buffer(const jack_nframes_t frames) {
            return reinterpret_cast<jack_default_audio_sample_t*>(
                        jack_port_get_buffer(port, frames));
        }

        virtual void* get(const jack_nframes_t frames) override {
            buffer = get_buffer(frames);
            return buffer;
        }

        virtual void set(const void* const buf, const jack_nframes_t frames) override {
            if (buf == buffer)
                return;
            std::memcpy(get_buffer(frames), buf, frames*sizeof(jack_default_audio_sample_t));
        }
};

class ShadowBufferJackPortConverter : public ForwardJackPortConverter {
    private:
        uint32_t buffer[BUFFER_SIZE_MAX + BUF_ALIGN_BYTES/(sizeof(uint32_t))];
        char* aligned_buffer;

    public:
        ShadowBufferJackPortConverter(jack_port_t* pt, size_t sample_size) :
            ForwardJackPortConverter(pt), sample_size(sample_size) {
            /* To optimize, aligned address calculation is moved to constructor */
            aligned_buffer = GET_32BYTE_ALIGN_BUFFER(buffer);
        }

        /* This function shall be used to get aligned shadow buffer */
        void* get_shadow_buffer()
        {
            return reinterpret_cast<void*>(aligned_buffer);
        }

        void* get_shadow_buffer(size_t offset)
        {
            char* ret = ((char*)get_shadow_buffer()) + (offset * sample_size);
            return ret;
         }

        virtual void* copy_from_jack(const jack_nframes_t shadow_offset,
                                    const jack_nframes_t jack_offset,
                                    const jack_nframes_t frames) {
            const jack_default_audio_sample_t* const src =
                    get_buffer(frames + jack_offset);
            if (src == NULL)
                return NULL;
            jack_default_audio_sample_t* const dst =
                    reinterpret_cast<jack_default_audio_sample_t*>(get_shadow_buffer());
            const size_t size = frames * sizeof(dst[0]);
            std::memcpy(&dst[shadow_offset], &src[jack_offset], size);
            return &dst[shadow_offset];
        }

        virtual void copy_to_jack(const char* const src,
                                  const jack_nframes_t src_offset,
                                  const jack_nframes_t jack_offset,
                                  const jack_nframes_t frames) {
            jack_default_audio_sample_t* const dst =
                    get_buffer(frames + jack_offset);
            if (dst == NULL)
                return;
            const size_t size = frames * sizeof(dst[0]);
            std::memcpy(&dst[jack_offset], &src[src_offset * sizeof(dst[0])],
                        size);
        }

        /** This function shall be used to set requested silence data to
         * shadow buffer. Sine this function can be called any inherited
         * class the the shadow buffer wil accessed based on working
         * sample size of shadow buffer.
         */

        void silence_shadow(const jack_nframes_t frames) {
            std::memset(get_shadow_buffer(), 0, frames * sample_size);
        }

        virtual void* get(const jack_nframes_t frames) override {
            return copy_from_jack(0, 0, frames);
        }

        virtual void set(const void* const src,
                         const jack_nframes_t frames) override {
            copy_to_jack(reinterpret_cast<const char*>(src), 0, 0, frames);
        }

        size_t get_samplesize() {
            return sample_size;
        }

        const size_t sample_size;
};

class IntegerJackPortConverter : public ShadowBufferJackPortConverter {
    typedef void (*ReadCopyFunction)  (jack_default_audio_sample_t *dst, char *src,
                                       unsigned long src_bytes,
                                       unsigned long src_skip_bytes);
    typedef void (*WriteCopyFunction) (char *dst, jack_default_audio_sample_t *src,
                                       unsigned long src_bytes,
                                       unsigned long dst_skip_bytes,
                                       void*);

    private:
        const ReadCopyFunction to_jack;
        const WriteCopyFunction from_jack;

    public:
        IntegerJackPortConverter(const ReadCopyFunction to_jack,
                                 const WriteCopyFunction from_jack,
                                 const size_t sample_size,
                                 jack_port_t* pt) : ShadowBufferJackPortConverter(pt, sample_size),
        to_jack(to_jack), from_jack(from_jack) {}

        virtual void* copy_from_jack(const jack_nframes_t shadow_offset,
                                     const jack_nframes_t jack_offset,
                                     const jack_nframes_t frames) override {
            char* const dst = reinterpret_cast<char*>(get_shadow_buffer());
            jack_default_audio_sample_t* const src =
                    get_buffer(frames + jack_offset);
            /* error is already thrown in jack_port_get_buffer() */
            if (src == NULL)
                return NULL;
            from_jack(&dst[shadow_offset * sample_size], &src[jack_offset],
                      frames, sample_size, NULL);
            return &dst[sample_size * shadow_offset];
        }

        virtual void copy_to_jack(const char* const src,
                                  const jack_nframes_t src_offset,
                                  const jack_nframes_t jack_offset,
                                  jack_nframes_t frames) override {
            jack_default_audio_sample_t* const dst =
                    get_buffer(frames + jack_offset);
            /* error is already thrown in jack_port_get_buffer() */
            if (dst == NULL)
                return;
            to_jack(&dst[jack_offset], const_cast<char*>(&src[src_offset * sample_size]),
                    frames, sample_size);
        }
};

static void sample_move_dS_s32(jack_default_audio_sample_t *dst, char *src,
                               unsigned long nsamples, unsigned long src_skip)
{
    const jack_default_audio_sample_t scaling = 1.0 / SAMPLE_32BIT_SCALING;

    while (nsamples--) {
        const int32_t src32 = *((int32_t *) src);
        *dst = src32 * scaling;
        dst++;
        src += src_skip;
    }
}

static void sample_move_d32_sS (char *dst, jack_default_audio_sample_t *src,
                                unsigned long nsamples, unsigned long dst_skip,
                                void*)
{
    const int32_t scaling = SAMPLE_32BIT_SCALING;

    while (nsamples--) {
        int32_t* const dst32 = (int32_t*)dst;

        if (*src <= NORMALIZED_FLOAT_MIN) {
            *dst32 = -scaling;
        } else if (*src >= NORMALIZED_FLOAT_MAX) {
            *dst32 = scaling;
        } else {
            *dst32 = lrintf(*src * scaling);
        }
        dst += dst_skip;
        src++;
    }
}

static void sample_move_dS_s16 (jack_default_audio_sample_t *dst, char *src,
                                unsigned long nsamples, unsigned long src_skip)
{
    const jack_default_audio_sample_t scaling = 1.0 / SAMPLE_16BIT_SCALING;

    while (nsamples--) {
        const int16_t src16 = *((int16_t*)src);
        *dst = src16 * scaling;
        dst++;
        src += src_skip;
    }
}

static void sample_move_d16_sS (char *dst, jack_default_audio_sample_t *src,
                                unsigned long nsamples, unsigned long dst_skip,
                                void*)
{
    while (nsamples--) {
        int16_t* const dst16 = (int16_t*) dst;
        if (*src <= NORMALIZED_FLOAT_MIN) {
            *dst16 = -SAMPLE_16BIT_SCALING;
        } else if (*src >= NORMALIZED_FLOAT_MAX) {
            *dst16 = SAMPLE_16BIT_SCALING;
        } else {
            *dst16 = lrintf(*src * SAMPLE_16BIT_SCALING);
        }
        dst += dst_skip;
        src++;
    }
}

class FramesPortConverter : public IJackPortConverter {
    protected:
        ShadowBufferJackPortConverter* const converter;
        const jack_nframes_t dst_frames;
        /* frames indicates amount of valid frames beginning at offset */
        jack_nframes_t shadow_frames;
        /* offset indicates position of first valid frame */
        jack_nframes_t jack_offset;

    public:
        FramesPortConverter(ShadowBufferJackPortConverter* const conv,
                            const jack_nframes_t dst_frames):
            converter(conv), dst_frames(dst_frames), shadow_frames(0),
            jack_offset(0) {}

        /** has to be called to provide the next dst_frames to get()
         * @param frames of the JackProcessCallback
         * @return >0 in case of further data available
         *          0 in case no data available
         *         <0 in case of error
         */
        virtual int next(const jack_nframes_t frames) = 0;
        virtual void update_client_frames() {};

        virtual void* get(const jack_nframes_t frames) {
            if (frames != dst_frames) {
                jack_error("Requested frames (%lu) differ from configuration (%lu)",
                           frames, dst_frames);
                return NULL;
            }
            return converter->get_shadow_buffer();
        }

        virtual ~FramesPortConverter() {
            delete converter;
        }
};

class FramesInPortConverter : public FramesPortConverter {
    private:

    public:
        FramesInPortConverter(ShadowBufferJackPortConverter* const conv,
                            const jack_nframes_t dst_frames,
                            const jack_nframes_t silence_prefill):
                            FramesPortConverter(conv, dst_frames) {
            shadow_frames = silence_prefill;
            converter->silence_shadow(silence_prefill);
        }

        int append_from_jack(const jack_nframes_t missing_jack_frames) {
            converter->copy_from_jack(shadow_frames, jack_offset, missing_jack_frames);
            jack_offset += missing_jack_frames;
            /* In case of client period size less than server period size Jack offset will go beyond
             * dst_frames (client period size)
             */

            /* data from shadow buffer is consumed immediately after
             * returning from this function. Therefore reset it here
             */
            shadow_frames = 0;
            return 1;
        }

        virtual int next(const jack_nframes_t frames) override {
            if (shadow_frames > dst_frames) {
                jack_error("Inport shadow buffer should not contain more frames (%lu) then requested (%lu)",
                            shadow_frames, dst_frames);
                return -EINVAL;
            }

            const jack_nframes_t jack_frames = frames - jack_offset;
            if ((shadow_frames + jack_frames) >= dst_frames) {
                /* shadow_frames cannot be greater than dst_frames due to
                 * previous condition
                 */
                /* only copy exactly required amount of samples to access shadow
                 * buffer always at offset 0
                 */
                const jack_nframes_t missing_jack_frames = dst_frames - shadow_frames;
                return append_from_jack(missing_jack_frames);
            } else {
                /* sufficient frames are not available. Therefore store pending
                 * jack frames in shadow buffer for next period.
                 * jack_frames < dst_frames otherwise above condition would be
                 * true. Therefore always shadow_frames < dst_frames.
                 * This case handles also jack_offset == frames. Therefore all
                 * frames from jack buffer were read and nothing to process
                 */
                converter->copy_from_jack(shadow_frames, jack_offset, jack_frames);
                shadow_frames += jack_frames;
                /* reset jack_offset for next call of this function in next
                 * period. Due to NULL is return user is only allowed to call
                 * this function on next period
                 */
                jack_offset = 0;
                return 0;
            }
        }

        virtual void set(const void* const src,
                         const jack_nframes_t frames) override {
                jack_error("Not allowed to call set() for input ports");
        }
};

class FramesOutPortConverter : public FramesPortConverter {
    private:
        /* offset indicates position of first valid frame */
        /* only used for output ports between last set() and next get() call */
        jack_nframes_t shadow_offset;
        /* client_frames used only by outport, to keep track of frames via set() call */
        jack_nframes_t client_frames;

    public:
        FramesOutPortConverter(ShadowBufferJackPortConverter* const conv,
                               const jack_nframes_t dst_frames):
            FramesPortConverter(conv, dst_frames), shadow_offset(0), client_frames(0) {}


        virtual int next(const jack_nframes_t frames) override {
            if (shadow_frames > ((frames > dst_frames)? frames :dst_frames)) {
                jack_error("OutPort shadow buffer should not contain more frames (%lu) then requested (%lu)",
                            shadow_frames, (frames > dst_frames)? frames:dst_frames);
                return -EINVAL;
            }

            /* check for available data in shadow buffer (shadow_frames + client_frames)
             * client_frames is updated in previous set() calls.
             * If shadow buffer contains more than server, copy data from shadow buffer
             * to jack server.else increment shadow_frames
             */
            if ((shadow_frames + client_frames) >= frames) {
                /* Copy data from shadow buffer to jack buffer */
                converter->copy_to_jack(reinterpret_cast<const char*>(
                                        converter->get_shadow_buffer(shadow_offset)), 0,
                                        0, frames);
                shadow_frames = shadow_frames + client_frames - frames;
                shadow_offset += frames;

                /* If all frames in shadow buffer are consumed reset shadow offset to zero */
                if (shadow_frames == 0)
                    shadow_offset = 0;

            } else {
                shadow_frames += client_frames;
            }

            /* If any remaining data available in shadow buffer and less/equal of jack server,
             * move to start of shadow buffer and Reset the shadow_offset to zero.
             * Not able to avoid memmove for outport shadow buffer.
             */
            if (shadow_offset && (shadow_frames <= frames)) {
                std::memmove(converter->get_shadow_buffer(), converter->get_shadow_buffer(shadow_offset),
                             shadow_frames*converter->get_samplesize());
                shadow_offset=0;
            }

            /* Reset client frames to zero . All client frames are consumed.
             * client_frames will be updated in next set()
             */
            client_frames = 0;
            return 1;
        }

        virtual void* get(const jack_nframes_t frames) override {
            if (frames != dst_frames) {
                jack_error("Requested frames (%lu) differ from configuration (%lu)",
                           frames, dst_frames);
                return NULL;
            }

            /* Return the available free space in shadow buffer
             * Available free space offset  = shadow_offset + shadow_frames + client_frames*/
            return converter->get_shadow_buffer(shadow_offset + shadow_frames + client_frames);
        }

        virtual void set(const void* const src,
                         const jack_nframes_t frames) override {
            if (frames != dst_frames) {
                jack_error("Requested frames (%lu) differ from configuration (%lu)",
                                                     frames, dst_frames);
            }
            void* buf = converter->get_shadow_buffer(shadow_offset + shadow_frames + client_frames);

            if (src == buf)
                return;

            std::memcpy(buf, src, frames * converter->get_samplesize());
            return;
        }

        virtual void update_client_frames() {
            client_frames += dst_frames;
        }
};

class JackBufferConverter : public IJackBufferConverter {
    private:
        const JackProcessCallback callback;
        void* const arg;
        std::vector<FramesPortConverter*> InPorts;
        std::vector<FramesPortConverter*> OutPorts;

        static int converter(jack_nframes_t frames, void *arg) {
            JackBufferConverter* conv =
                                reinterpret_cast<JackBufferConverter*>(((JackFormatConverterHandle_t*)arg)->handle);
            int ret = 0;
            while ((ret = conv->next(frames, conv->InPorts)) >=1) {
                /* Call client call back only if inport has data*/
                const int cb_ret = conv->callback(conv->dst_frames, conv->arg);
                if (cb_ret < 0)
                    return cb_ret;

                /* This call is required only for outport, To keep track of frames given by client
                 * via set() function call. The total frames given by set() will be used in next()
                 * function of outport.
                  */
                for (auto& port : conv->OutPorts) {
                    port->update_client_frames();
                }
            }

            /* Outport can contain frames multiple time of jack server. Data from shadow buffer
             * to jack server should be copied, Other wise we may overwrite the data.
             */
            int outport_ret = conv->next(frames, conv->OutPorts);
            if (outport_ret < 0)
                return outport_ret;

            return ret;
        }

        int next(const jack_nframes_t frames, std::vector<FramesPortConverter*> ports) {
            if (ports.empty()) {
                jack_error("Processing called without any audio ports");
                return -1;
            }
            int result = 1;
            for (auto& port : ports) {
                const int ret = port->next(frames);
                if (ret < 0)
                    return ret;
                if (ret < result)
                    result = ret;
            }
            return result;
        }

        jack_nframes_t calculate_silence_prefill(const jack_nframes_t client_per_size,
                                                 const jack_nframes_t server_per_size)
        {
            jack_nframes_t silence_prefil = 0;
            if (server_per_size > client_per_size) {
                /* if server period size is exactly divisible by client
                 * silence prefill is not required.
                 * else silence prefill is required so that for all jack server
                 * callback data can be give to jack server
                 */
                if ((server_per_size%client_per_size) == 0)
                    silence_prefil = 0;
                else
                    silence_prefil = client_per_size;
            } else if (server_per_size < client_per_size) {
                /* If if server period size is exactly divisible by client
                 * silence prefill is diff of client and server, else silence prefill
                 * is set to client, This is the least silence prefill required so
                 * for all server callback data can be given to jack server
                 */
                if ((client_per_size%server_per_size) == 0) {
                    silence_prefil = client_per_size - server_per_size;
                } else {
                    silence_prefil = client_per_size;
                }
            } else {
                silence_prefil = 0;
            }

            return silence_prefil;
        }

    public:
        const jack_nframes_t dst_frames;
        const jack_nframes_t silence_prefil;

        JackBufferConverter(jack_client_t* const client,
                        const JackProcessCallback cb, void* const arg,
                        const jack_nframes_t dst_frames) :
            callback(cb), arg(arg), dst_frames(dst_frames),
            silence_prefil(calculate_silence_prefill(dst_frames, jack_get_buffer_size(client)))
        {
            if (arg == NULL)
                throw std::exception();

            /* Assign Buffer converter handle to JackFormatConverterHandle_t*/
            ((JackFormatConverterHandle_t*)arg)->handle = this;

            if (jack_set_process_callback(client, converter, arg) != 0)
                throw std::exception();
        }

        void add(FramesPortConverter* port, const int flags) {
            if (flags & JackPortIsOutput)
                OutPorts.push_back(port);
            else
                InPorts.push_back(port);
        }
};


LIB_EXPORT JackBufferConverter_t* jack_buffer_create_convert(jack_client_t* const client,
                                                  const JackProcessCallback cb,
                                                  void* arg,
                                                  const jack_nframes_t dst_frames)
{
    try {
        return new JackBufferConverter(client, cb, arg, dst_frames);
    } catch (std::exception) {
        return NULL;
    }
}

LIB_EXPORT void jack_buffer_destroy_convert(JackBufferConverter_t* handle)
{
    delete reinterpret_cast<IJackBufferConverter*>(handle);
}

LIB_EXPORT JackPortConverter_t* jack_port_create_convert(jack_port_t* port,
                                                          const JackPortConverterFormat_t dst_type,
                                                          const bool init_output_silence,
                                                          JackBufferConverter_t* const iconv)
{
    ShadowBufferJackPortConverter* format_conv = NULL;

    if(dst_type == JACK_PORT_CONV_FMT_DEFAULT) {
        if (iconv == nullptr)
            /* allocate object without shadow buffer to save memory space */
            return new ForwardJackPortConverter(port);
        else
            format_conv = new ShadowBufferJackPortConverter(port, sizeof(jack_default_audio_sample_t));
    }
    else if(dst_type == JACK_PORT_CONV_FMT_INT32) {
        format_conv = new IntegerJackPortConverter(sample_move_dS_s32,
                                                   sample_move_d32_sS,
                                                   sizeof(int32_t), port);
    }
    else if(dst_type == JACK_PORT_CONV_FMT_INT16) {
        format_conv = new IntegerJackPortConverter(sample_move_dS_s16,
                                                   sample_move_d16_sS,
                                                   sizeof(int16_t), port);
    }
    else {
        jack_error("jack_port_create_converter called with dst_type that is not supported");
        return NULL;
    }

    if (iconv == nullptr) {
        return format_conv;
    }

    const int flags = jack_port_flags(port);
    if (flags < 0) {
        jack_error("Getting flags of port %s failed", jack_port_name(port));
        delete format_conv;
        return NULL;
    }

    JackBufferConverter* conv = reinterpret_cast<JackBufferConverter*>(iconv);
    FramesPortConverter* frames_conv = nullptr;
    if (flags & JackPortIsOutput)
        frames_conv = new FramesOutPortConverter(format_conv, conv->dst_frames);
    else
        frames_conv = new FramesInPortConverter(format_conv, conv->dst_frames, conv->silence_prefil);

    conv->add(frames_conv, flags);
    return frames_conv;
}

LIB_EXPORT void jack_port_destroy_convert(JackPortConverter_t* handle)
{
    delete reinterpret_cast<IJackPortConverter*>(handle);
}

LIB_EXPORT void* jack_port_convert_get(JackPortConverter_t* handle, const jack_nframes_t frames)
{
    if (handle == NULL) {
        jack_error("Port converter handle is NULL");
        return NULL;
    }

    IJackPortConverter* pc = reinterpret_cast<IJackPortConverter*>(handle);
    return pc->get(frames);
}

LIB_EXPORT void jack_port_convert_set(JackPortConverter_t* handle, const void* const buf, const jack_nframes_t frames)
{
    if (handle == NULL) {
        jack_error("Port converter handle is NULL");
        return;
    }

    IJackPortConverter* pc = reinterpret_cast<IJackPortConverter*>(handle);
    pc->set(buf, frames);

}

LIB_EXPORT IJackPortConverter* jack_port_create_converter(jack_port_t* port,
                                                          const std::type_info& dst_type,
                                                          const bool init_output_silence)
{
    IJackBufferConverter* iconv = nullptr;
    ShadowBufferJackPortConverter* format_conv = NULL;

    if(dst_type == (typeid(jack_default_audio_sample_t))) {
        if (iconv == nullptr)
            /* allocate object without shadow buffer to save memory space */
            return new ForwardJackPortConverter(port);
        else
            format_conv = new ShadowBufferJackPortConverter(port, sizeof(jack_default_audio_sample_t));
    }
    else if(dst_type == (typeid(int32_t))) {
        format_conv = new IntegerJackPortConverter(sample_move_dS_s32,
                                                   sample_move_d32_sS,
                                                   sizeof(int32_t), port);
    }
    else if(dst_type == (typeid(int16_t))) {
        format_conv = new IntegerJackPortConverter(sample_move_dS_s16,
                                                   sample_move_d16_sS,
                                                   sizeof(int16_t), port);
    }
    else {
        jack_error("jack_port_create_converter called with dst_type that is not supported");
        return NULL;
    }

    if (iconv == nullptr) {
        return format_conv;
    }

    const int flags = jack_port_flags(port);
    if (flags < 0) {
        jack_error("Getting flags of port %s failed", jack_port_name(port));
        delete format_conv;
        return NULL;
    }

    JackBufferConverter* conv = dynamic_cast<JackBufferConverter*>(iconv);
    FramesPortConverter* frames_conv = nullptr;
    if (flags & JackPortIsOutput)
        frames_conv = new FramesOutPortConverter(format_conv, conv->dst_frames);
    else
        frames_conv = new FramesInPortConverter(format_conv, conv->dst_frames, conv->silence_prefil);

    conv->add(frames_conv, flags);
    return frames_conv;
}
