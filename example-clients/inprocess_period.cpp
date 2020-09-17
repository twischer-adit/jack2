/** @file inprocess_period.cpp
 *
 * @brief This demonstrates the basic concepts for writing a client
 * that runs within the JACK server process for period size alignment
 * between server and client.
 *
 * For the sake of example, a port_converter_pair_t is allocated in
 * jack_initialize(), passed to inprocess() as an argument, then freed
 * in jack_finish().
 */

#include <stdlib.h>
#include <stdio.h>
#include <memory.h>
#include <jack/jack.h>
#include <jack/format_converter.h>

/**
 * For the sake of example, an instance of this struct is allocated in
 * jack_initialize(), passed to inprocess() as an argument, then freed
 * in jack_finish().
 */
typedef struct {
	/* Client private structure/class should have first variable as JackFormatConverterHandle_t
	 * This space will be used by jack format converter
	 */
	JackFormatConverterHandle_t* handle;
	JackBufferConverter_t* buffer_converter;
	JackPortConverter_t* input_port_converter;
	JackPortConverter_t* output_port_converter;
} port_converter_pair_t;

/**
 * Called in the realtime thread on every process cycle.  The entry
 * point name was passed to jack_set_process_callback() from
 * jack_initialize().  Although this is an internal client, its
 * process() interface is identical to @ref simple_client.c.
 *
 * @return 0 if successful; otherwise jack_finish() will be called and
 * the client terminated immediately.
 */
int
inprocess (jack_nframes_t nframes, void *arg)
{
	port_converter_pair_t *pp = (port_converter_pair_t *)arg;
	int32_t *in_buffer;
	int32_t *out_buffer;

	in_buffer = (int32_t *) jack_port_convert_get(pp->input_port_converter, nframes);
	out_buffer = (int32_t *) jack_port_convert_get(pp->output_port_converter, nframes);

	/* This memcpy needs to be replaced with the actual processing */
	memcpy (out_buffer, in_buffer, sizeof(int32_t) * nframes);

	jack_port_convert_set(pp->output_port_converter, out_buffer, nframes);

	return 0;
}

/**
 * This required entry point is called after the client is loaded by
 * jack_internal_client_load().
 *
 * @param client pointer to JACK client structure.
 * @param load_init character string passed to the load operation.
 *
 * @return 0 if successful; otherwise jack_finish() will be called and
 * the client terminated immediately.
 */
extern "C" int
jack_initialize (jack_client_t *client, const char *load_init)
{
	jack_port_t *input_port;
	jack_port_t *output_port;

	port_converter_pair_t *pp = (port_converter_pair_t *) malloc (sizeof (port_converter_pair_t));
	const char **ports;

	if (pp == NULL)
		return 1;		/* heap exhausted */

	const uint32_t period_time = 16; /* ms */
	pp->buffer_converter = jack_buffer_create_convert(client, inprocess, pp,
                                                 (period_time * jack_get_sample_rate(client))/1000);

	/* create a pair of ports */
	input_port = jack_port_register (client, "input",
					     JACK_DEFAULT_AUDIO_TYPE,
					     JackPortIsInput, 0);
	output_port = jack_port_register (client, "output",
					      JACK_DEFAULT_AUDIO_TYPE,
					      JackPortIsOutput, 0);

	pp->input_port_converter = jack_port_create_convert(input_port,
							      JACK_PORT_CONV_FMT_INT32,
							      false,
							      pp->buffer_converter);
	pp->output_port_converter = jack_port_create_convert(output_port,
							      JACK_PORT_CONV_FMT_INT32,
							       false,
							       pp->buffer_converter);

	/* join the process() cycle */
	jack_activate (client);

	ports = jack_get_ports (client, NULL, NULL,
				JackPortIsPhysical|JackPortIsOutput);
	if (ports == NULL) {
		fprintf(stderr, "no physical capture ports\n");
		return 1;		/* terminate client */
	}

	if (jack_connect (client, ports[0], jack_port_name (input_port))) {
		fprintf (stderr, "cannot connect input ports\n");
	}

	jack_free (ports);

	ports = jack_get_ports (client, NULL, NULL,
				JackPortIsPhysical|JackPortIsInput);
	if (ports == NULL) {
		fprintf(stderr, "no physical playback ports\n");
		return 1;		/* terminate client */
	}

	if (jack_connect (client, jack_port_name (output_port), ports[0])) {
		fprintf (stderr, "cannot connect output ports\n");
	}

	jack_free (ports);

	return 0;			/* success */
}

/**
 * This required entry point is called immediately before the client
 * is unloaded, which could happen due to a call to
 * jack_internal_client_unload(), or a nonzero return from either
 * jack_initialize() or inprocess().
 *
 * @param arg the same parameter provided to inprocess().
 */
extern "C" void
jack_finish (void *arg)
{
	port_converter_pair_t* pp = (port_converter_pair_t*)(arg);
	if (pp == NULL)
		return;

	jack_port_destroy_convert(pp->input_port_converter);
	jack_port_destroy_convert(pp->output_port_converter);
	jack_buffer_destroy_convert(pp->buffer_converter);
	free (pp);
}
