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

#ifndef __jack_format_converter_h__
#define __jack_format_converter_h__

#include <stdbool.h>
#include <typeinfo>

#ifdef __cplusplus
extern "C"
{
#endif

#include <jack/weakmacros.h>
/*ToDo : - After moving all client using format convert to C-API
 *       - Move IJackPortConverter to JackFormatConverter.cpp
 *       - Remove include <typeinfo>
 *       - Remove C++ wrapper jack_port_create_converter().
 *       - This is required to have comaptability with clients using
 *         c++ wrappers
 */

/**
 * @defgroup PortConverter Converting sample format & period size of ports
 * @{
 */

/* Structure to hold the jack format converter private handle */
typedef struct {
  void* handle;    /* Pointer to hold jack format converter handle */
}JackFormatConverterHandle_t;

typedef enum {
    JACK_PORT_CONV_FMT_DEFAULT=0,
    JACK_PORT_CONV_FMT_INT16,
    JACK_PORT_CONV_FMT_INT32,
}JackPortConverterFormat_t;

/* Opaque objects accessed only via jack port converter API's*/
typedef void JackBufferConverter_t;
typedef void JackPortConverter_t;

/**
 * This function can be used to align the period size between the JACK daemon
 * and a JACK client. For example when the client wants to use a period size of
 * 16ms but the daemon 8ms, this API has to be used instead of the
 * jack_set_process_callback() API. Each audio port of this client has to be
 * accessed via the jack_port_convert_get()/set() API.
 * An example for the usage of this API can be found in
 * example-clients/inprocess_period.c
 * When comparing this example with
 * example-clients/inprocess_s32.cpp
 * it exactly highlights the modifications required for period size alignment.
 *
 * @param client instance of the actual JACK client
 * @param cb callback usually provided to jack_set_process_callback() API
 * @param arg pointer usually provided to jack_set_process_callback() API
 * @param dst_frames the required period size of the client in frames. In case
 * of 16ms period time with a sample rate of 48kHz the value of 768 frames
 * has to be provided. Therefore the defined callback function would be called
 * for each 768 frames. The given frames can be greater or less the period size
 * used by the JACK daemon.
 *
 * @return The returned object has to be provided to each audio port of this
 * client created with jack_port_create_convert(). The returned object has to
 * be freed with jack_buffer_destroy_converter() when the client will be destroyed.
 */
JackBufferConverter_t* jack_buffer_create_convert(jack_client_t* const client,
                                            const JackProcessCallback cb,
                                            void* arg,
                                            const jack_nframes_t dst_frames
                                            ) JACK_OPTIONAL_WEAK_EXPORT;

/**
 * This function is used to destroy the object created in jack_buffer_create_converter()
 * @param handle object returned by jack_buffer_create_converter()
 */
void jack_buffer_destroy_convert(JackBufferConverter_t* handle) JACK_OPTIONAL_WEAK_EXPORT;

/**
 * This function returns a pointer to the instance of the object
 * JackPortConverter_t based on the dst_type. Applications can use the
 * jack_port_convert_get()/set() method of this object to get and set
 * the pointers to the memory area associated with the specified port.
 * Currently Jack only supports float, int32_t and int16_t.
 * An example for the usage of this API can be found in
 * example-clients/inprocess_s32.cpp
 * Since the introduction of the jack_buffer_create_converter() this function
 * accepts a JackBufferConverter_t object as an additional parameter.
 *
 * @param port for which the converter should be created
 * @param dst_type sample format required by client.
 * @param init_output_silence if true, jack will always initialize the output
 * port with silence. This will introduce additional overhead. Therefore it
 * should be avoided if possible.
 * @param conv is a pointer to a BufferConverter object. This can be used to
 * align different period sizes. For example the client should run with a period
 * time of 16ms but the JACK daemon should use a period time of 8ms. In this
 * case all audio ports of this client have not to be accessed by the
 * jack_port_get_buffer() API. The audio ports have to be accessed via this
 * jack_port_convert_get()/set() API. In addition the BufferConverter object
 * has to be provided to each port of this client.
 *
 * @return ptr to JackPortConverter_t on success, otherwise NULL if dst_type is
 * not supported. This object has to be freed with jack_port_destroy_convert()
 * when the client will be destroyed.
 */
JackPortConverter_t* jack_port_create_convert(jack_port_t* port,
                                               const JackPortConverterFormat_t dst_type,
                                               const bool init_output_silence,
                                               JackBufferConverter_t* const conv
                                               ) JACK_OPTIONAL_WEAK_EXPORT;

/**
 * This function is used to destroy the object created in jack_port_create_convert()
 * @param handle object returned by jack_port_create_convert()
 */
void jack_port_destroy_convert(JackPortConverter_t* handle) JACK_OPTIONAL_WEAK_EXPORT;

/**
 * This function returns a pointer to the memory region containing the audio data of
 * the port. The sample format and the size of this region is depending on the arguments
 * given when creating the object with jack_port_create_converter().
 *
 * @param handle handle returned by jack_port_create_converter() api
 * @param frames has always to be the same value as given as argument in
 * the JackProcessCallback function.
 *
 * @return pointer to a valid memory region containing audio data. The
 * size in frames of the return buffer is defined by the frames
 * argument of this method.
 */
void* jack_port_convert_get(JackPortConverter_t* handle,
                              const jack_nframes_t frames
                              ) JACK_OPTIONAL_WEAK_EXPORT;


/**
 * This function used to overwrites the audio data with the content of
 * the given buffer. This method is only allowed to be called for output ports.
 *
 * @param handle handle returned by jack_port_create_converter() api
 * @param buf containing the audio data which should be written to this
 * audio output port. The size of this buffer is defined by the frames
 * argument of this function.
 * @param frames has always to be the same value as given as argument in
 * the JackProcessCallback function. Therefore buffers with a different
 * size are not supported.
 */
void jack_port_convert_set(JackPortConverter_t* handle,
                             const void* const buf,
                             const jack_nframes_t frames
                             ) JACK_OPTIONAL_WEAK_EXPORT;

class IJackPortConverter {
    public:
        virtual ~IJackPortConverter() {}

        /**
         * Returns a pointer to the memory region containing the audio data of
         * the port. The sample format and the size of this region is depending
         * on the arguments given when creating the object with
         * jack_port_create_converter().
         *
         * @param frames has always to be the same value as given as argument in
         * the JackProcessCallback function.
         *
         * @return pointer to a valid memory region containing audio data. The
         * size in frames of the return buffer is defined by the frames
         * argument of this method.
         */
        virtual void* get(const jack_nframes_t frames) = 0;

        /**
         * Overwrites the audio data with the content of the given buffer. This
         * method is only allowed to be called for output ports.
         *
         * @param buf containing the audio data which should be written to this
         * audio output port. The size of this buffer is defined by the frames
         * argument of this function.
         * @param frames has always to be the same value as given as argument in
         * the JackProcessCallback function. Therefore buffers with a different
         * size are not supported.
         */
        virtual void set(const void* const buf, const jack_nframes_t frames) = 0;
};

/**
 * This returns a pointer to the instance of the object IJackPortConverter based
 * on the dst_type. Applications can use the get() and set()
 * of this object to get and set the pointers to the memory area associated with the specified port.
 * Currently Jack only supports Float, int32_t and int16_t.
 *
 * @param port jack_port_t pointer.
 * @param dst_type type required by client.
 * @param init_output_silence if true, jack will initialize the output port with silence
 *
 * @return ptr to IJackPortConverter on success, otherwise NULL if dst_type is not supported.
 */

IJackPortConverter* jack_port_create_converter(jack_port_t* port, const std::type_info& dst_type, const bool init_output_silence=true) JACK_OPTIONAL_WEAK_EXPORT;

/*@}*/

#ifdef __cplusplus
}
#endif

#endif // __jack_format_converter_h__
