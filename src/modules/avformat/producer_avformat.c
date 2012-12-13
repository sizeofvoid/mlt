/*
 * producer_avformat.c -- avformat producer
 * Copyright (C) 2003-2012 Ushodaya Enterprises Limited
 * Author: Charles Yates <charles.yates@pandora.be>
 * Author: Dan Dennedy <dan@dennedy.org>
 * Much code borrowed from ffmpeg.c: Copyright (c) 2000-2003 Fabrice Bellard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

// MLT Header files
#include <framework/mlt_producer.h>
#include <framework/mlt_frame.h>
#include <framework/mlt_profile.h>
#include <framework/mlt_log.h>
#include <framework/mlt_deque.h>
#include <framework/mlt_factory.h>
#include <framework/mlt_cache.h>

// ffmpeg Header files
#include <libavformat/avformat.h>
#ifdef SWSCALE
#  include <libswscale/swscale.h>
#endif

#if LIBAVUTIL_VERSION_INT >= ((50<<16)+(38<<8)+0)
#  include <libavutil/samplefmt.h>
#else
#  define AV_SAMPLE_FMT_S16 SAMPLE_FMT_S16
#  define AV_SAMPLE_FMT_S32 SAMPLE_FMT_S32
#  define AV_SAMPLE_FMT_FLT SAMPLE_FMT_FLT
#  if (LIBAVCODEC_VERSION_INT >= ((51<<16)+(71<<8)+0))
const char *avcodec_get_sample_fmt_name(int sample_fmt);
#  endif
#endif

#ifdef VDPAU
#  include <libavcodec/vdpau.h>
#endif
#if (LIBAVUTIL_VERSION_INT > ((50<<16)+(7<<8)+0))
#  include <libavutil/pixdesc.h>
#endif
#if (LIBAVUTIL_VERSION_INT >= ((51<<16)+(8<<8)+0))
#  include <libavutil/dict.h>
#endif

// System header files
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <limits.h>

#if LIBAVUTIL_VERSION_INT < (50<<16)
#define PIX_FMT_RGB32 PIX_FMT_RGBA32
#define PIX_FMT_YUYV422 PIX_FMT_YUV422
#endif

#if LIBAVCODEC_VERSION_MAJOR >= 53
#include <libavutil/opt.h>
#define CODEC_TYPE_VIDEO      AVMEDIA_TYPE_VIDEO
#define CODEC_TYPE_AUDIO      AVMEDIA_TYPE_AUDIO
#define PKT_FLAG_KEY AV_PKT_FLAG_KEY
#else
#include <libavcodec/opt.h>
#endif

#define POSITION_INITIAL (-2)
#define POSITION_INVALID (-1)

#define MAX_AUDIO_STREAMS (32)
#define MAX_VDPAU_SURFACES (10)

struct producer_avformat_s
{
	mlt_producer parent;
	AVFormatContext *dummy_context;
	AVFormatContext *audio_format;
	AVFormatContext *video_format;
	AVCodecContext *audio_codec[ MAX_AUDIO_STREAMS ];
	AVCodecContext *video_codec;
	AVFrame *av_frame;
	AVPacket pkt;
	ReSampleContext *audio_resample[ MAX_AUDIO_STREAMS ];
	mlt_position audio_expected;
	mlt_position video_expected;
	int audio_index;
	int video_index;
	int64_t first_pts;
	int64_t last_position;
	int seekable;
	int64_t current_position;
	mlt_position nonseek_position;
	int top_field_first;
	uint8_t *audio_buffer[ MAX_AUDIO_STREAMS ];
	size_t audio_buffer_size[ MAX_AUDIO_STREAMS ];
	uint8_t *decode_buffer[ MAX_AUDIO_STREAMS ];
	int audio_used[ MAX_AUDIO_STREAMS ];
	int audio_streams;
	int audio_max_stream;
	int total_channels;
	int max_channel;
	int max_frequency;
	unsigned int invalid_pts_counter;
	unsigned int invalid_dts_counter;
	double resample_factor;
	mlt_cache image_cache;
	int colorspace;
	int full_luma;
	pthread_mutex_t video_mutex;
	pthread_mutex_t audio_mutex;
	mlt_deque apackets;
	mlt_deque vpackets;
	pthread_mutex_t packets_mutex;
	pthread_mutex_t open_mutex;
#ifdef VDPAU
	struct
	{
		// from FFmpeg
		struct vdpau_render_state render_states[MAX_VDPAU_SURFACES];
		
		// internal
		mlt_deque deque;
		int b_age;
		int ip_age[2];
		int is_decoded;
		uint8_t *buffer;

		VdpDevice device;
		VdpDecoder decoder;
	} *vdpau;
#endif
};
typedef struct producer_avformat_s *producer_avformat;

// Forward references.
static int list_components( char* file );
static int producer_open( producer_avformat self, mlt_profile profile, const char *URL, int take_lock );
static int producer_get_frame( mlt_producer producer, mlt_frame_ptr frame, int index );
static void producer_avformat_close( producer_avformat );
static void producer_close( mlt_producer parent );
static void producer_set_up_video( producer_avformat self, mlt_frame frame );
static void producer_set_up_audio( producer_avformat self, mlt_frame frame );
static void apply_properties( void *obj, mlt_properties properties, int flags );
static int video_codec_init( producer_avformat self, int index, mlt_properties properties );
static void get_audio_streams_info( producer_avformat self );

#ifdef VDPAU
#include "vdpau.c"
#endif

/** Constructor for libavformat.
*/

mlt_producer producer_avformat_init( mlt_profile profile, const char *service, char *file )
{
	if ( list_components( file ) )
		return NULL;

	mlt_producer producer = NULL;

	// Check that we have a non-NULL argument
	if ( file )
	{
		// Construct the producer
		producer_avformat self = calloc( 1, sizeof( struct producer_avformat_s ) );
		producer = calloc( 1, sizeof( struct mlt_producer_s ) );

		// Initialise it
		if ( mlt_producer_init( producer, self ) == 0 )
		{
			self->parent = producer;

			// Get the properties
			mlt_properties properties = MLT_PRODUCER_PROPERTIES( producer );

			// Set the resource property (required for all producers)
			mlt_properties_set( properties, "resource", file );

			// Register transport implementation with the producer
			producer->close = (mlt_destructor) producer_close;

			// Register our get_frame implementation
			producer->get_frame = producer_get_frame;

			// init mutexes
			pthread_mutex_init( &self->audio_mutex, NULL );
			pthread_mutex_init( &self->video_mutex, NULL );
			pthread_mutex_init( &self->packets_mutex, NULL );
			pthread_mutex_init( &self->open_mutex, NULL );

			// init queues
			self->apackets = mlt_deque_init();
			self->vpackets = mlt_deque_init();

			if ( strcmp( service, "avformat-novalidate" ) )
			{
				// Open the file
				if ( producer_open( self, profile, file, 1 ) != 0 )
				{
					// Clean up
					mlt_producer_close( producer );
					producer = NULL;
					producer_avformat_close( self );
				}
				else if ( self->seekable )
				{
					// Close the file to release resources for large playlists - reopen later as needed
#if LIBAVFORMAT_VERSION_INT >= ((53<<16)+(17<<8)+0)
					if ( self->audio_format )
						avformat_close_input( &self->audio_format );
					if ( self->video_format )
						avformat_close_input( &self->video_format );
#else
					if ( self->audio_format )
						av_close_input_file( self->audio_format );
					if ( self->video_format )
						av_close_input_file( self->video_format );
#endif
					self->audio_format = NULL;
					self->video_format = NULL;
				}
			}
			if ( producer )
			{
				// Default the user-selectable indices from the auto-detected indices
				mlt_properties_set_int( properties, "audio_index",  self->audio_index );
				mlt_properties_set_int( properties, "video_index",  self->video_index );
#ifdef VDPAU
				mlt_service_cache_set_size( MLT_PRODUCER_SERVICE(producer), "producer_avformat", 5 );
#endif
				mlt_service_cache_put( MLT_PRODUCER_SERVICE(producer), "producer_avformat", self, 0, (mlt_destructor) producer_avformat_close );
			}
		}
	}
	return producer;
}

int list_components( char* file )
{
	int skip = 0;

	// Report information about available demuxers and codecs as YAML Tiny
	if ( file && strstr( file, "f-list" ) )
	{
		fprintf( stderr, "---\nformats:\n" );
		AVInputFormat *format = NULL;
		while ( ( format = av_iformat_next( format ) ) )
			fprintf( stderr, "  - %s\n", format->name );
		fprintf( stderr, "...\n" );
		skip = 1;
	}
	if ( file && strstr( file, "acodec-list" ) )
	{
		fprintf( stderr, "---\naudio_codecs:\n" );
		AVCodec *codec = NULL;
		while ( ( codec = av_codec_next( codec ) ) )
			if ( codec->decode && codec->type == CODEC_TYPE_AUDIO )
				fprintf( stderr, "  - %s\n", codec->name );
		fprintf( stderr, "...\n" );
		skip = 1;
	}
	if ( file && strstr( file, "vcodec-list" ) )
	{
		fprintf( stderr, "---\nvideo_codecs:\n" );
		AVCodec *codec = NULL;
		while ( ( codec = av_codec_next( codec ) ) )
			if ( codec->decode && codec->type == CODEC_TYPE_VIDEO )
				fprintf( stderr, "  - %s\n", codec->name );
		fprintf( stderr, "...\n" );
		skip = 1;
	}

	return skip;
}

/** Find the default streams.
*/

static mlt_properties find_default_streams( producer_avformat self )
{
	int i;
	char key[200];
#if LIBAVUTIL_VERSION_INT >= ((51<<16)+(8<<8)+0)
	AVDictionaryEntry *tag = NULL;
#else
	AVMetadataTag *tag = NULL;
#endif
	AVFormatContext *context = self->video_format;
	mlt_properties meta_media = MLT_PRODUCER_PROPERTIES( self->parent );

	// Default to the first audio and video streams found
	self->audio_index = -1;
	self->video_index = -1;

	mlt_properties_set_int( meta_media, "meta.media.nb_streams", context->nb_streams );

	// Allow for multiple audio and video streams in the file and select first of each (if available)
	for( i = 0; i < context->nb_streams; i++ )
	{
		// Get the codec context
		AVStream *stream = context->streams[ i ];
		if ( ! stream ) continue;
		AVCodecContext *codec_context = stream->codec;
		if ( ! codec_context ) continue;
		AVCodec *codec = avcodec_find_decoder( codec_context->codec_id );
		if ( ! codec ) continue;

		snprintf( key, sizeof(key), "meta.media.%d.stream.type", i );

		// Determine the type and obtain the first index of each type
		switch( codec_context->codec_type )
		{
			case CODEC_TYPE_VIDEO:
				// Use first video stream
				if ( self->video_index < 0 )
					self->video_index = i;
				mlt_properties_set( meta_media, key, "video" );
				snprintf( key, sizeof(key), "meta.media.%d.stream.frame_rate", i );
#if LIBAVFORMAT_VERSION_INT >= ((52<<16)+(42<<8)+0)
				double ffmpeg_fps = av_q2d( context->streams[ i ]->avg_frame_rate );
				if ( isnan( ffmpeg_fps ) || ffmpeg_fps == 0 )
					ffmpeg_fps = av_q2d( context->streams[ i ]->r_frame_rate );
				mlt_properties_set_double( meta_media, key, ffmpeg_fps );
#else
				mlt_properties_set_double( meta_media, key, av_q2d( context->streams[ i ]->r_frame_rate ) );
#endif

#if LIBAVFORMAT_VERSION_INT >= ((52<<16)+(21<<8)+0)
				snprintf( key, sizeof(key), "meta.media.%d.stream.sample_aspect_ratio", i );
				mlt_properties_set_double( meta_media, key, av_q2d( context->streams[ i ]->sample_aspect_ratio ) );
#endif
				snprintf( key, sizeof(key), "meta.media.%d.codec.width", i );
				mlt_properties_set_int( meta_media, key, codec_context->width );
				snprintf( key, sizeof(key), "meta.media.%d.codec.height", i );
				mlt_properties_set_int( meta_media, key, codec_context->height );
				snprintf( key, sizeof(key), "meta.media.%d.codec.frame_rate", i );
				mlt_properties_set_double( meta_media, key, (double) codec_context->time_base.den /
										   ( codec_context->time_base.num == 0 ? 1 : codec_context->time_base.num ) );
				snprintf( key, sizeof(key), "meta.media.%d.codec.pix_fmt", i );
#if LIBAVUTIL_VERSION_INT >= ((51<<16)+(3<<8)+0)
				mlt_properties_set( meta_media, key, av_get_pix_fmt_name( codec_context->pix_fmt ) );
#else
				mlt_properties_set( meta_media, key, avcodec_get_pix_fmt_name( codec_context->pix_fmt ) );
#endif
				snprintf( key, sizeof(key), "meta.media.%d.codec.sample_aspect_ratio", i );
				mlt_properties_set_double( meta_media, key, av_q2d( codec_context->sample_aspect_ratio ) );
#if LIBAVCODEC_VERSION_INT > ((52<<16)+(28<<8)+0)
				snprintf( key, sizeof(key), "meta.media.%d.codec.colorspace", i );
				switch ( codec_context->colorspace )
				{
				case AVCOL_SPC_SMPTE240M:
					mlt_properties_set_int( meta_media, key, 240 );
					break;
				case AVCOL_SPC_BT470BG:
				case AVCOL_SPC_SMPTE170M:
					mlt_properties_set_int( meta_media, key, 601 );
					break;
				case AVCOL_SPC_BT709:
					mlt_properties_set_int( meta_media, key, 709 );
					break;
				default:
					// This is a heuristic Charles Poynton suggests in "Digital Video and HDTV"
					mlt_properties_set_int( meta_media, key, codec_context->width * codec_context->height > 750000 ? 709 : 601 );
					break;
				}
#endif
				break;
			case CODEC_TYPE_AUDIO:
				if ( !codec_context->channels )
					break;
				// Use first audio stream
				if ( self->audio_index < 0 )
					self->audio_index = i;
				mlt_properties_set( meta_media, key, "audio" );
#if LIBAVUTIL_VERSION_INT >= ((50<<16)+(38<<8)+0)
				snprintf( key, sizeof(key), "meta.media.%d.codec.sample_fmt", i );
				mlt_properties_set( meta_media, key, av_get_sample_fmt_name( codec_context->sample_fmt ) );
#elif (LIBAVCODEC_VERSION_INT >= ((51<<16)+(71<<8)+0))
				snprintf( key, sizeof(key), "meta.media.%d.codec.sample_fmt", i );
				mlt_properties_set( meta_media, key, avcodec_get_sample_fmt_name( codec_context->sample_fmt ) );
#endif
				snprintf( key, sizeof(key), "meta.media.%d.codec.sample_rate", i );
				mlt_properties_set_int( meta_media, key, codec_context->sample_rate );
				snprintf( key, sizeof(key), "meta.media.%d.codec.channels", i );
				mlt_properties_set_int( meta_media, key, codec_context->channels );
				break;
			default:
				break;
		}
// 		snprintf( key, sizeof(key), "meta.media.%d.stream.time_base", i );
// 		mlt_properties_set_double( meta_media, key, av_q2d( context->streams[ i ]->time_base ) );
		snprintf( key, sizeof(key), "meta.media.%d.codec.name", i );
		mlt_properties_set( meta_media, key, codec->name );
#if (LIBAVCODEC_VERSION_INT >= ((51<<16)+(55<<8)+0))
		snprintf( key, sizeof(key), "meta.media.%d.codec.long_name", i );
		mlt_properties_set( meta_media, key, codec->long_name );
#endif
		snprintf( key, sizeof(key), "meta.media.%d.codec.bit_rate", i );
		mlt_properties_set_int( meta_media, key, codec_context->bit_rate );
// 		snprintf( key, sizeof(key), "meta.media.%d.codec.time_base", i );
// 		mlt_properties_set_double( meta_media, key, av_q2d( codec_context->time_base ) );
//		snprintf( key, sizeof(key), "meta.media.%d.codec.profile", i );
//		mlt_properties_set_int( meta_media, key, codec_context->profile );
//		snprintf( key, sizeof(key), "meta.media.%d.codec.level", i );
//		mlt_properties_set_int( meta_media, key, codec_context->level );

		// Read Metadata
#if LIBAVFORMAT_VERSION_INT >= ((52<<16)+(31<<8)+0)
#if LIBAVUTIL_VERSION_INT >= ((51<<16)+(8<<8)+0)
		while ( ( tag = av_dict_get( stream->metadata, "", tag, AV_DICT_IGNORE_SUFFIX ) ) )
#else
		while ( ( tag = av_metadata_get( stream->metadata, "", tag, AV_METADATA_IGNORE_SUFFIX ) ) )
#endif
		{
			if ( tag->value && strcmp( tag->value, "" ) && strcmp( tag->value, "und" ) )
			{
				snprintf( key, sizeof(key), "meta.attr.%d.stream.%s.markup", i, tag->key );
				mlt_properties_set( meta_media, key, tag->value );
			}
		}
#endif
	}
#if LIBAVFORMAT_VERSION_INT >= ((52<<16)+(31<<8)+0)
#if LIBAVUTIL_VERSION_INT >= ((51<<16)+(8<<8)+0)
	while ( ( tag = av_dict_get( context->metadata, "", tag, AV_DICT_IGNORE_SUFFIX ) ) )
#else
	while ( ( tag = av_metadata_get( context->metadata, "", tag, AV_METADATA_IGNORE_SUFFIX ) ) )
#endif
	{
		if ( tag->value && strcmp( tag->value, "" ) && strcmp( tag->value, "und" ) )
		{
			snprintf( key, sizeof(key), "meta.attr.%s.markup", tag->key );
			mlt_properties_set( meta_media, key, tag->value );
		}
	}
#else
	if ( context->title && strcmp( context->title, "" ) )
		mlt_properties_set(properties, "meta.attr.title.markup", context->title );
	if ( context->author && strcmp( context->author, "" ) )
		mlt_properties_set(properties, "meta.attr.author.markup", context->author );
	if ( context->copyright && strcmp( context->copyright, "" ) )
		mlt_properties_set(properties, "meta.attr.copyright.markup", context->copyright );
	if ( context->comment )
		mlt_properties_set(properties, "meta.attr.comment.markup", context->comment );
	if ( context->album )
		mlt_properties_set(properties, "meta.attr.album.markup", context->album );
	if ( context->year )
		mlt_properties_set_int(properties, "meta.attr.year.markup", context->year );
	if ( context->track )
		mlt_properties_set_int(properties, "meta.attr.track.markup", context->track );
#endif

	return meta_media;
}

static inline int dv_is_pal( AVPacket *pkt )
{
	return pkt->data[3] & 0x80;
}

static int dv_is_wide( AVPacket *pkt )
{
	int i = 80 /* block size */ *3 /* VAUX starts at block 3 */ +3 /* skip block header */;

	for ( ; i < pkt->size; i += 5 /* packet size */ )
	{
		if ( pkt->data[ i ] == 0x61 )
		{
			uint8_t x = pkt->data[ i + 2 ] & 0x7;
			return ( x == 2 ) || ( x == 7 );
		}
	}
	return 0;
}

static double get_aspect_ratio( mlt_properties properties, AVStream *stream, AVCodecContext *codec_context, AVPacket *pkt )
{
	double aspect_ratio = 1.0;

	if ( codec_context->codec_id == CODEC_ID_DVVIDEO )
	{
		if ( pkt )
		{
			if ( dv_is_pal( pkt ) )
			{
				if ( dv_is_wide( pkt ) )
				{
					mlt_properties_set_int( properties, "meta.media.sample_aspect_num", 64 );
					mlt_properties_set_int( properties, "meta.media.sample_aspect_den", 45 );
				}
				else
				{
					mlt_properties_set_int( properties, "meta.media.sample_aspect_num", 16 );
					mlt_properties_set_int( properties, "meta.media.sample_aspect_den", 15 );
				}
			}
			else
			{
				if ( dv_is_wide( pkt ) )
				{
					mlt_properties_set_int( properties, "meta.media.sample_aspect_num", 32 );
					mlt_properties_set_int( properties, "meta.media.sample_aspect_den", 27 );
				}
				else
				{
					mlt_properties_set_int( properties, "meta.media.sample_aspect_num", 8 );
					mlt_properties_set_int( properties, "meta.media.sample_aspect_den", 9 );
				}
			}
		}
		else
		{
			AVRational ar =
#if LIBAVFORMAT_VERSION_INT >= ((52<<16)+(21<<8)+0)
				stream->sample_aspect_ratio;
#else
				codec_context->sample_aspect_ratio;
#endif
			// Override FFmpeg's notion of DV aspect ratios, which are
			// based upon a width of 704. Since we do not have a normaliser
			// that crops (nor is cropping 720 wide ITU-R 601 video always desirable)
			// we just coerce the values to facilitate a passive behaviour through
			// the rescale normaliser when using equivalent producers and consumers.
			// = display_aspect / (width * height)
			if ( ar.num == 10 && ar.den == 11 )
			{
				// 4:3 NTSC
				mlt_properties_set_int( properties, "meta.media.sample_aspect_num", 8 );
				mlt_properties_set_int( properties, "meta.media.sample_aspect_den", 9 );
			}
			else if ( ar.num == 59 && ar.den == 54 )
			{
				// 4:3 PAL
				mlt_properties_set_int( properties, "meta.media.sample_aspect_num", 16 );
				mlt_properties_set_int( properties, "meta.media.sample_aspect_den", 15 );
			}
			else if ( ar.num == 40 && ar.den == 33 )
			{
				// 16:9 NTSC
				mlt_properties_set_int( properties, "meta.media.sample_aspect_num", 32 );
				mlt_properties_set_int( properties, "meta.media.sample_aspect_den", 27 );
			}
			else if ( ar.num == 118 && ar.den == 81 )
			{
				// 16:9 PAL
				mlt_properties_set_int( properties, "meta.media.sample_aspect_num", 64 );
				mlt_properties_set_int( properties, "meta.media.sample_aspect_den", 45 );
			}
		}
	}
	else
	{
		AVRational codec_sar = codec_context->sample_aspect_ratio;
		AVRational stream_sar =
#if LIBAVFORMAT_VERSION_INT >= ((52<<16)+(21<<8)+0)
			stream->sample_aspect_ratio;
#else
			{ 0, 1 };
#endif
		if ( codec_sar.num > 0 )
		{
			mlt_properties_set_int( properties, "meta.media.sample_aspect_num", codec_sar.num );
			mlt_properties_set_int( properties, "meta.media.sample_aspect_den", codec_sar.den );
		}
		else if ( stream_sar.num > 0 )
		{
			mlt_properties_set_int( properties, "meta.media.sample_aspect_num", stream_sar.num );
			mlt_properties_set_int( properties, "meta.media.sample_aspect_den", stream_sar.den );
		}
		else
		{
			mlt_properties_set_int( properties, "meta.media.sample_aspect_num", 1 );
			mlt_properties_set_int( properties, "meta.media.sample_aspect_den", 1 );
		}
	}
	AVRational ar = { mlt_properties_get_double( properties, "meta.media.sample_aspect_num" ), mlt_properties_get_double( properties, "meta.media.sample_aspect_den" ) };
	aspect_ratio = av_q2d( ar );
	mlt_properties_set_double( properties, "aspect_ratio", aspect_ratio );

	return aspect_ratio;
}

#if LIBAVFORMAT_VERSION_INT > ((53<<16)+(6<<8)+0)
static char* parse_url( mlt_profile profile, const char* URL, AVInputFormat **format, AVDictionary **params )
#else
static char* parse_url( mlt_profile profile, const char* URL, AVInputFormat **format, AVFormatParameters *params )
#endif
{
	if ( !URL ) return NULL;

	char *result = NULL;
	char *protocol = strdup( URL );
	char *url = strchr( protocol, ':' );

	// Only if there is not a protocol specification that avformat can handle
#if LIBAVFORMAT_VERSION_MAJOR >= 53
	if ( url && avio_check( URL, 0 ) < 0 )
#else
	if ( url && !url_exist( URL ) )
#endif
	{
		// Truncate protocol string
		url[0] = 0;
		mlt_log_debug( NULL, "%s: protocol=%s resource=%s\n", __FUNCTION__, protocol, url + 1 );

		// Lookup the format
		*format = av_find_input_format( protocol );

		// Eat the format designator
		result = ++url;

		if ( *format )
		{
#if LIBAVFORMAT_VERSION_INT > ((53<<16)+(6<<8)+0)
			// support for legacy width and height parameters
			char *width = NULL;
			char *height = NULL;
#else
			// These are required by video4linux2 (defaults)
			params->width = profile->width;
			params->height = profile->height;
			if ( !strstr( URL, "&frame_rate" ) )
				params->time_base = (AVRational){ profile->frame_rate_den, profile->frame_rate_num };
			params->channels = 2;
			params->sample_rate = 48000;
#endif

			// Parse out params
			url = strchr( url, '?' );
			while ( url )
			{
				url[0] = 0;
				char *name = strdup( ++url );
				char *value = strchr( name, '=' );
				if ( !value )
					// Also accept : as delimiter for backwards compatibility.
					value = strchr( name, ':' );
				if ( value )
				{
					value[0] = 0;
					value++;
					char *t = strchr( value, '&' );
					if ( t )
						t[0] = 0;
#if LIBAVFORMAT_VERSION_INT > ((53<<16)+(6<<8)+0)
					// translate old parameters to new av_dict names
					if ( !strcmp( name, "frame_rate" ) )
						av_dict_set( params, "framerate", value, 0 );
					else if ( !strcmp( name, "pix_fmt" ) )
						av_dict_set( params, "pixel_format", value, 0 );
					else if ( !strcmp( name, "width" ) )
						width = strdup( value );
					else if ( !strcmp( name, "height" ) )
						height = strdup( value );
					else
						// generic demux/device option support
						av_dict_set( params, name, value, 0 );
#else
					if ( !strcmp( name, "frame_rate" ) )
						params->time_base.den = atoi( value );
					else if ( !strcmp( name, "frame_rate_base" ) )
						params->time_base.num = atoi( value );
					else if ( !strcmp( name, "sample_rate" ) )
						params->sample_rate = atoi( value );
					else if ( !strcmp( name, "channel" ) )
						params->channel = atoi( value );
					else if ( !strcmp( name, "channels" ) )
						params->channels = atoi( value );
#if (LIBAVUTIL_VERSION_INT > ((50<<16)+(7<<8)+0))
					else if ( !strcmp( name, "pix_fmt" ) )
						params->pix_fmt = av_get_pix_fmt( value );
#endif
					else if ( !strcmp( name, "width" ) )
						params->width = atoi( value );
					else if ( !strcmp( name, "height" ) )
						params->height = atoi( value );
					else if ( !strcmp( name, "standard" ) )
						params->standard = strdup( value );
#endif
				}
				free( name );
				url = strchr( url, '&' );
			}
#if LIBAVFORMAT_VERSION_INT > ((53<<16)+(6<<8)+0)
			// continued support for legacy width and height parameters
			if ( width && height )
			{
				char *s = malloc( strlen( width ) + strlen( height ) + 2 );
				strcpy( s, width );
				strcat( s, "x");
				strcat( s, height );
				av_dict_set( params, "video_size", s, 0 );
				free( s );
			}
			if ( width ) free( width );
			if ( height ) free ( height );
#endif
		}
		result = strdup( result );
	}
	else
	{
		result = strdup( URL );
	}
	free( protocol );
	return result;
}

static int get_basic_info( producer_avformat self, mlt_profile profile, const char *filename )
{
	int error = 0;

	// Get the properties
	mlt_properties properties = MLT_PRODUCER_PROPERTIES( self->parent );

	AVFormatContext *format = self->video_format;

	// We will treat everything with the producer fps.
	// TODO: make this more flexible.
	double fps = mlt_profile_fps( profile );

	// Get the duration
	if ( !mlt_properties_get_int( properties, "_length_computed" ) )
	{
		// The _length_computed flag prevents overwriting explicity set length/out/eof properties
		// when producer_open is called after initial call when restoring or reseting the producer.
		if ( format->duration != AV_NOPTS_VALUE )
		{
			// This isn't going to be accurate for all formats
			mlt_position frames = ( mlt_position )( ( ( double )format->duration / ( double )AV_TIME_BASE ) * fps );
			mlt_properties_set_position( properties, "out", frames - 1 );
			mlt_properties_set_position( properties, "length", frames );
			mlt_properties_set_int( properties, "_length_computed", 1 );
		}
		else
		{
			// Set live sources to run forever
			mlt_properties_set_position( properties, "length", INT_MAX );
			mlt_properties_set_position( properties, "out", INT_MAX - 1 );
			mlt_properties_set( properties, "eof", "loop" );
			mlt_properties_set_int( properties, "_length_computed", 1 );
		}
	}

	// Check if we're seekable
	// avdevices are typically AVFMT_NOFILE and not seekable
	self->seekable = !format->iformat || !( format->iformat->flags & AVFMT_NOFILE );
	if ( format->pb )
	{
		// protocols can indicate if they support seeking
#if LIBAVFORMAT_VERSION_MAJOR >= 53
		self->seekable = format->pb->seekable;
#else
		URLContext *uc = url_fileno( format->pb );
		if ( uc )
			self->seekable = !uc->is_streamed;
#endif
	}
	if ( self->seekable )
	{
		// Do a more rigourous test of seekable on a disposable context
		self->seekable = av_seek_frame( format, -1, format->start_time, AVSEEK_FLAG_BACKWARD ) >= 0;
		mlt_properties_set_int( properties, "seekable", self->seekable );
		self->dummy_context = format;
#if LIBAVFORMAT_VERSION_INT > ((53<<16)+(6<<8)+0)
		self->video_format = NULL;
		avformat_open_input( &self->video_format, filename, NULL, NULL );
		avformat_find_stream_info( self->video_format, NULL );
#else
		av_open_input_file( &self->video_format, filename, NULL, 0, NULL );
		av_find_stream_info( self->video_format );
#endif
		format = self->video_format;
	}

	// Fetch the width, height and aspect ratio
	if ( self->video_index != -1 )
	{
		AVCodecContext *codec_context = format->streams[ self->video_index ]->codec;
		mlt_properties_set_int( properties, "width", codec_context->width );
		mlt_properties_set_int( properties, "height", codec_context->height );

		if ( codec_context->codec_id == CODEC_ID_DVVIDEO )
		{
			// Fetch the first frame of DV so we can read it directly
			AVPacket pkt;
			int ret = 0;
			while ( ret >= 0 )
			{
				ret = av_read_frame( format, &pkt );
				if ( ret >= 0 && pkt.stream_index == self->video_index && pkt.size > 0 )
				{
					get_aspect_ratio( properties, format->streams[ self->video_index ], codec_context, &pkt );
					av_free_packet(&pkt);
					break;
				}
				if ( ret >= 0 )
					av_free_packet(&pkt);
			}
		}
		else
		{
			get_aspect_ratio( properties, format->streams[ self->video_index ], codec_context, NULL );
		}

#ifdef SWSCALE
		// Verify that we can convert this to YUV 4:2:2
		// TODO: we can now also return RGB and RGBA and quite possibly more in the future.
		struct SwsContext *context = sws_getContext( codec_context->width, codec_context->height, codec_context->pix_fmt,
			codec_context->width, codec_context->height, PIX_FMT_YUYV422, SWS_BILINEAR, NULL, NULL, NULL);
		if ( context )
			sws_freeContext( context );
		else
			error = 1;
#endif
	}
	return error;
}

/** Open the file.
*/

static int producer_open( producer_avformat self, mlt_profile profile, const char *URL, int take_lock )
{
	// Return an error code (0 == no error)
	int error = 0;
	mlt_properties properties = MLT_PRODUCER_PROPERTIES( self->parent );

	// Lock the service
	if ( take_lock )
	{
		pthread_mutex_lock( &self->audio_mutex );
		pthread_mutex_lock( &self->video_mutex );
	}
	mlt_events_block( properties, self->parent );

	// Parse URL
	AVInputFormat *format = NULL;
#if LIBAVFORMAT_VERSION_INT > ((53<<16)+(6<<8)+0)
	AVDictionary *params = NULL;
#else
	AVFormatParameters params;
	memset( &params, 0, sizeof(params) );
#endif
	char *filename = parse_url( profile, URL, &format, &params );

	// Now attempt to open the file or device with filename
#if LIBAVFORMAT_VERSION_INT > ((53<<16)+(6<<8)+0)
	error = avformat_open_input( &self->video_format, filename, format, &params ) < 0;
	if ( error )
		// If the URL is a network stream URL, then we probably need to open with full URL
		error = avformat_open_input( &self->video_format, URL, format, &params ) < 0;
#else
	error = av_open_input_file( &self->video_format, filename, format, 0, &params ) < 0;
	if ( error )
		// If the URL is a network stream URL, then we probably need to open with full URL
		error = av_open_input_file( &self->video_format, URL, format, 0, &params ) < 0;
#endif

	// Set MLT properties onto video AVFormatContext
	if ( !error && self->video_format )
	{
		apply_properties( self->video_format, properties, AV_OPT_FLAG_DECODING_PARAM );
#if LIBAVFORMAT_VERSION_INT >= ((52<<16)+(110<<8)+0)
		if ( self->video_format->iformat && self->video_format->iformat->priv_class && self->video_format->priv_data )
			apply_properties( self->video_format->priv_data, properties, AV_OPT_FLAG_DECODING_PARAM );
#endif
	}

#if LIBAVFORMAT_VERSION_INT > ((53<<16)+(6<<8)+0)
	av_dict_free( &params );
#else
	// Cleanup AVFormatParameters
	if ( params.standard )
		free( (void*) params.standard );
#endif

	// If successful, then try to get additional info
	if ( !error && self->video_format )
	{
		// Get the stream info
#if LIBAVFORMAT_VERSION_INT > ((53<<16)+(6<<8)+0)
		error = avformat_find_stream_info( self->video_format, NULL ) < 0;
#else
		error = av_find_stream_info( self->video_format ) < 0;
#endif

		// Continue if no error
		if ( !error && self->video_format )
		{
			// Find default audio and video streams
			find_default_streams( self );
			error = get_basic_info( self, profile, filename );

			// Initialize position info
			self->first_pts = AV_NOPTS_VALUE;
			self->last_position = POSITION_INITIAL;

			if ( !self->audio_format )
			{
				// We're going to cheat here - for seekable A/V files, we will have separate contexts
				// to support independent seeking of audio from video.
				// TODO: Is this really necessary?
				if ( self->audio_index != -1 && self->video_index != -1 )
				{
					if ( self->seekable )
					{
						// And open again for our audio context
#if LIBAVFORMAT_VERSION_INT > ((53<<16)+(6<<8)+0)
						avformat_open_input( &self->audio_format, filename, NULL, NULL );
						apply_properties( self->audio_format, properties, AV_OPT_FLAG_DECODING_PARAM );
						if ( self->audio_format->iformat && self->audio_format->iformat->priv_class && self->audio_format->priv_data )
							apply_properties( self->audio_format->priv_data, properties, AV_OPT_FLAG_DECODING_PARAM );
						avformat_find_stream_info( self->audio_format, NULL );
#else
						av_open_input_file( &self->audio_format, filename, NULL, 0, NULL );
						apply_properties( self->audio_format, properties, AV_OPT_FLAG_DECODING_PARAM );
#if LIBAVFORMAT_VERSION_INT >= ((52<<16)+(110<<8)+0)
                        if ( self->audio_format->iformat && self->audio_format->iformat->priv_class && self->audio_format->priv_data )
                            apply_properties( self->audio_format->priv_data, properties, AV_OPT_FLAG_DECODING_PARAM );
#endif
						av_find_stream_info( self->audio_format );
#endif
					}
					else
					{
						self->audio_format = self->video_format;
					}
				}
				else if ( self->audio_index != -1 )
				{
					// We only have an audio context
					self->audio_format = self->video_format;
					self->video_format = NULL;
				}
				else if ( self->video_index == -1 )
				{
					// Something has gone wrong
					error = -1;
				}
				if ( self->audio_format && !self->audio_streams )
					get_audio_streams_info( self );
			}
		}
	}
	if ( filename )
		free( filename );

	if ( self->dummy_context )
	{
		pthread_mutex_lock( &self->open_mutex );
#if LIBAVFORMAT_VERSION_INT >= ((53<<16)+(17<<8)+0)
		avformat_close_input( &self->dummy_context );
#else
		av_close_input_file( self->dummy_context );
#endif
		self->dummy_context = NULL;
		pthread_mutex_unlock( &self->open_mutex );
	}

	// Unlock the service
	if ( take_lock )
	{
		pthread_mutex_unlock( &self->audio_mutex );
		pthread_mutex_unlock( &self->video_mutex );
	}
	mlt_events_unblock( properties, self->parent );

	return error;
}

static void reopen_video( producer_avformat self, mlt_producer producer )
{
	mlt_properties properties = MLT_PRODUCER_PROPERTIES( producer );
	mlt_service_lock( MLT_PRODUCER_SERVICE( producer ) );
	pthread_mutex_lock( &self->audio_mutex );
	pthread_mutex_lock( &self->open_mutex );

	if ( self->video_codec )
		avcodec_close( self->video_codec );
	self->video_codec = NULL;
#if LIBAVFORMAT_VERSION_INT >= ((53<<16)+(17<<8)+0)
	if ( self->dummy_context )
		avformat_close_input( &self->dummy_context );
	if ( self->video_format )
		avformat_close_input( &self->video_format );
#else
	if ( self->dummy_context )
		av_close_input_file( self->dummy_context );
	if ( self->video_format )
		av_close_input_file( self->video_format );
#endif
	self->dummy_context = NULL;
	self->video_format = NULL;
	pthread_mutex_unlock( &self->open_mutex );

	int audio_index = self->audio_index;
	int video_index = self->video_index;

	producer_open( self, mlt_service_profile( MLT_PRODUCER_SERVICE(producer) ),
		mlt_properties_get( properties, "resource" ), 0 );

	self->audio_index = audio_index;
	if ( self->video_format && video_index > -1 )
	{
		self->video_index = video_index;
		video_codec_init( self, video_index, properties );
	}

	pthread_mutex_unlock( &self->audio_mutex );
	mlt_service_unlock( MLT_PRODUCER_SERVICE( producer ) );
}

static int64_t best_pts( producer_avformat self, int64_t pts, int64_t dts )
{
	self->invalid_pts_counter += pts == AV_NOPTS_VALUE;
	self->invalid_dts_counter += dts == AV_NOPTS_VALUE;
	if ( ( self->invalid_pts_counter <= self->invalid_dts_counter
		   || dts == AV_NOPTS_VALUE ) && pts != AV_NOPTS_VALUE )
		return pts;
	else
		return dts;
}

static int seek_video( producer_avformat self, mlt_position position,
	int64_t req_position, int preseek )
{
	mlt_producer producer = self->parent;
	int paused = 0;

	if ( self->seekable && ( position != self->video_expected || self->last_position < 0 ) )
	{
		mlt_properties properties = MLT_PRODUCER_PROPERTIES( producer );

		// Fetch the video format context
		AVFormatContext *context = self->video_format;

		// Get the video stream
		AVStream *stream = context->streams[ self->video_index ];

		// Get codec context
		AVCodecContext *codec_context = stream->codec;

		// We may want to use the source fps if available
		double source_fps = mlt_properties_get_double( properties, "meta.media.frame_rate_num" ) /
			mlt_properties_get_double( properties, "meta.media.frame_rate_den" );

		// find initial PTS
		if ( self->last_position == POSITION_INITIAL )
		{
			int ret = 0;
			int toscan = 500;
			AVPacket pkt;

			while ( ret >= 0 && toscan-- > 0 )
			{
				ret = av_read_frame( context, &pkt );
				if ( ret >= 0 && pkt.stream_index == self->video_index && ( pkt.flags & PKT_FLAG_KEY ) )
				{
					mlt_log_debug( MLT_PRODUCER_SERVICE(producer),
						"first_pts %"PRId64" dts %"PRId64" pts_dts_delta %d\n",
						pkt.pts, pkt.dts, (int)(pkt.pts - pkt.dts) );
					self->first_pts = best_pts( self, pkt.pts, pkt.dts );
					if ( self->first_pts != AV_NOPTS_VALUE )
						toscan = 0;
				}
				av_free_packet( &pkt );
			}
			av_seek_frame( context, -1, 0, AVSEEK_FLAG_BACKWARD );
		}

		if ( self->av_frame && position + 1 == self->video_expected )
		{
			// We're paused - use last image
			paused = 1;
		}
		else if ( self->seekable && ( position < self->video_expected || position - self->video_expected >= 12 || self->last_position < 0 ) )
		{
			// Calculate the timestamp for the requested frame
			int64_t timestamp = req_position / ( av_q2d( stream->time_base ) * source_fps );
			if ( req_position <= 0 )
				timestamp = 0;
			else if ( self->first_pts != AV_NOPTS_VALUE )
				timestamp += self->first_pts;
			else if ( context->start_time != AV_NOPTS_VALUE )
				timestamp += context->start_time;
			if ( preseek && av_q2d( stream->time_base ) != 0 )
				timestamp -= 2 / av_q2d( stream->time_base );
			if ( timestamp < 0 )
				timestamp = 0;
			mlt_log_debug( MLT_PRODUCER_SERVICE(producer), "seeking timestamp %"PRId64" position %d expected %d last_pos %"PRId64"\n",
				timestamp, position, self->video_expected, self->last_position );

			// Seek to the timestamp
			// NOTE: reopen_video is disabled at this time because it is causing trouble with A/V sync.
			if ( 1 || req_position > 0 || self->last_position <= 0 )
			{
				codec_context->skip_loop_filter = AVDISCARD_NONREF;
				av_seek_frame( context, self->video_index, timestamp, AVSEEK_FLAG_BACKWARD );

				// flush any pictures still in decode buffer
				avcodec_flush_buffers( codec_context );
			}
			else
			{
				// Re-open video stream when rewinding to beginning from somewhere else.
				// This is rather ugly, and I prefer not to do it this way, but ffmpeg is
				// not reliably seeking to the first frame across formats.
				reopen_video( self, producer );
			}

			// Remove the cached info relating to the previous position
			self->current_position = POSITION_INVALID;
			self->last_position = POSITION_INVALID;
			av_freep( &self->av_frame );
		}
	}
	return paused;
}

/** Convert a frame position to a time code.
*/

static double producer_time_of_frame( mlt_producer producer, mlt_position position )
{
	return ( double )position / mlt_producer_get_fps( producer );
}

// Collect information about all audio streams

static void get_audio_streams_info( producer_avformat self )
{
	// Fetch the audio format context
	AVFormatContext *context = self->audio_format;
	int i;

	for ( i = 0;
		  i < context->nb_streams;
		  i++ )
	{
		if ( context->streams[i]->codec->codec_type == CODEC_TYPE_AUDIO )
		{
			AVCodecContext *codec_context = context->streams[i]->codec;
			AVCodec *codec = avcodec_find_decoder( codec_context->codec_id );

			// If we don't have a codec and we can't initialise it, we can't do much more...
			pthread_mutex_lock( &self->open_mutex );
#if LIBAVCODEC_VERSION_INT >= ((53<<16)+(8<<8)+0)
			if ( codec && avcodec_open2( codec_context, codec, NULL ) >= 0 )
#else
			if ( codec && avcodec_open( codec_context, codec ) >= 0 )
#endif
			{
				self->audio_streams++;
				self->audio_max_stream = i;
				self->total_channels += codec_context->channels;
				if ( codec_context->channels > self->max_channel )
					self->max_channel = codec_context->channels;
				if ( codec_context->sample_rate > self->max_frequency )
					self->max_frequency = codec_context->sample_rate;
				avcodec_close( codec_context );
			}
			pthread_mutex_unlock( &self->open_mutex );
		}
	}
	mlt_log_verbose( NULL, "[producer avformat] audio: total_streams %d max_stream %d total_channels %d max_channels %d\n",
		self->audio_streams, self->audio_max_stream, self->total_channels, self->max_channel );
	
	// Other audio-specific initializations
	self->resample_factor = 1.0;
}

static void set_luma_transfer( struct SwsContext *context, int colorspace, int use_full_range )
{
#if defined(SWSCALE) && (LIBSWSCALE_VERSION_INT >= ((0<<16)+(7<<8)+2))
	int *coefficients;
	const int *new_coefficients;
	int full_range;
	int brightness, contrast, saturation;

	if ( sws_getColorspaceDetails( context, &coefficients, &full_range, &coefficients, &full_range,
			&brightness, &contrast, &saturation ) != -1 )
	{
		// Don't change these from defaults unless explicitly told to.
		if ( use_full_range >= 0 )
			full_range = use_full_range;
		switch ( colorspace )
		{
		case 170:
		case 470:
		case 601:
		case 624:
			new_coefficients = sws_getCoefficients( SWS_CS_ITU601 );
			break;
		case 240:
			new_coefficients = sws_getCoefficients( SWS_CS_SMPTE240M );
			break;
		case 709:
			new_coefficients = sws_getCoefficients( SWS_CS_ITU709 );
			break;
		default:
			new_coefficients = coefficients;
			break;
		}
		sws_setColorspaceDetails( context, new_coefficients, full_range, new_coefficients, full_range,
			brightness, contrast, saturation );
	}
#endif
}

static mlt_image_format pick_pix_format( enum PixelFormat pix_fmt )
{
	switch ( pix_fmt )
	{
	case PIX_FMT_ARGB:
	case PIX_FMT_RGBA:
	case PIX_FMT_ABGR:
	case PIX_FMT_BGRA:
		return mlt_image_rgb24a;
	case PIX_FMT_YUV420P:
	case PIX_FMT_YUVJ420P:
	case PIX_FMT_YUVA420P:
		return mlt_image_yuv420p;
	case PIX_FMT_RGB24:
	case PIX_FMT_BGR24:
	case PIX_FMT_GRAY8:
	case PIX_FMT_MONOWHITE:
	case PIX_FMT_MONOBLACK:
	case PIX_FMT_RGB8:
	case PIX_FMT_BGR8:
		return mlt_image_rgb24;
	default:
		return mlt_image_yuv422;
	}
}

static mlt_audio_format pick_audio_format( int sample_fmt )
{
	switch ( sample_fmt )
	{
	// interleaved
	case AV_SAMPLE_FMT_S16:
		return mlt_audio_s16;
	case AV_SAMPLE_FMT_S32:
		return mlt_audio_s32le;
	case AV_SAMPLE_FMT_FLT:
		return mlt_audio_f32le;
	// planar - this producer converts planar to interleaved
#if LIBAVUTIL_VERSION_INT >= ((51<<16)+(17<<8)+0)
	case AV_SAMPLE_FMT_S16P:
		return mlt_audio_s16;
	case AV_SAMPLE_FMT_S32P:
		return mlt_audio_s32le;
	case AV_SAMPLE_FMT_FLTP:
		return mlt_audio_f32le;
#endif
	default:
		return mlt_audio_none;
	}
}

static void convert_image( producer_avformat self, AVFrame *frame, uint8_t *buffer, int pix_fmt,
	mlt_image_format *format, int width, int height, uint8_t **alpha )
{
#ifdef SWSCALE
	int flags = SWS_BICUBIC | SWS_ACCURATE_RND;

#ifdef USE_MMX
	flags |= SWS_CPU_CAPS_MMX;
#endif
#ifdef USE_SSE
	flags |= SWS_CPU_CAPS_MMX2;
#endif

	// extract alpha from planar formats
	if ( ( pix_fmt == PIX_FMT_YUVA420P
#if defined(FFUDIV) && LIBAVUTIL_VERSION_INT >= ((51<<16)+(35<<8)+101)
			|| pix_fmt == PIX_FMT_YUVA444P
#endif
			) &&
		*format != mlt_image_rgb24a && *format != mlt_image_opengl &&
		frame->data[3] && frame->linesize[3] )
	{
		int i;
		uint8_t *src, *dst;

		dst = *alpha = mlt_pool_alloc( width * height );
		src = frame->data[3];

		for ( i = 0; i < height; dst += width, src += frame->linesize[3], i++ )
			memcpy( dst, src, FFMIN( width, frame->linesize[3] ) );
	}

	if ( *format == mlt_image_yuv420p )
	{
		struct SwsContext *context = sws_getContext( width, height, pix_fmt,
			width, height, PIX_FMT_YUV420P, flags, NULL, NULL, NULL);
		AVPicture output;
		output.data[0] = buffer;
		output.data[1] = buffer + width * height;
		output.data[2] = buffer + ( 5 * width * height ) / 4;
		output.linesize[0] = width;
		output.linesize[1] = width >> 1;
		output.linesize[2] = width >> 1;
		set_luma_transfer( context, self->colorspace, -1 );
		sws_scale( context, (const uint8_t* const*) frame->data, frame->linesize, 0, height,
			output.data, output.linesize);
		sws_freeContext( context );
	}
	else if ( *format == mlt_image_rgb24 )
	{
		struct SwsContext *context = sws_getContext( width, height, pix_fmt,
			width, height, PIX_FMT_RGB24, flags | SWS_FULL_CHR_H_INT, NULL, NULL, NULL);
		AVPicture output;
		avpicture_fill( &output, buffer, PIX_FMT_RGB24, width, height );
		set_luma_transfer( context, self->colorspace, self->full_luma );
		sws_scale( context, (const uint8_t* const*) frame->data, frame->linesize, 0, height,
			output.data, output.linesize);
		sws_freeContext( context );
	}
	else if ( *format == mlt_image_rgb24a || *format == mlt_image_opengl )
	{
		struct SwsContext *context = sws_getContext( width, height, pix_fmt,
			width, height, PIX_FMT_RGBA, flags | SWS_FULL_CHR_H_INT, NULL, NULL, NULL);
		AVPicture output;
		avpicture_fill( &output, buffer, PIX_FMT_RGBA, width, height );
		set_luma_transfer( context, self->colorspace, self->full_luma );
		sws_scale( context, (const uint8_t* const*) frame->data, frame->linesize, 0, height,
			output.data, output.linesize);
		sws_freeContext( context );
	}
	else
	{
		struct SwsContext *context = sws_getContext( width, height, pix_fmt,
			width, height, PIX_FMT_YUYV422, flags | SWS_FULL_CHR_H_INP, NULL, NULL, NULL);
		AVPicture output;
		avpicture_fill( &output, buffer, PIX_FMT_YUYV422, width, height );
		set_luma_transfer( context, self->colorspace, -1 );
		sws_scale( context, (const uint8_t* const*) frame->data, frame->linesize, 0, height,
			output.data, output.linesize);
		sws_freeContext( context );
	}
#else
	if ( *format == mlt_image_yuv420p )
	{
		AVPicture pict;
		pict.data[0] = buffer;
		pict.data[1] = buffer + width * height;
		pict.data[2] = buffer + ( 5 * width * height ) / 4;
		pict.linesize[0] = width;
		pict.linesize[1] = width >> 1;
		pict.linesize[2] = width >> 1;
		img_convert( &pict, PIX_FMT_YUV420P, (AVPicture *)frame, pix_fmt, width, height );
	}
	else if ( *format == mlt_image_rgb24 )
	{
		AVPicture output;
		avpicture_fill( &output, buffer, PIX_FMT_RGB24, width, height );
		img_convert( &output, PIX_FMT_RGB24, (AVPicture *)frame, pix_fmt, width, height );
	}
	else if ( *format == mlt_image_rgb24a || *format == mlt_image_opengl )
	{
		AVPicture output;
		avpicture_fill( &output, buffer, PIX_FMT_RGB32, width, height );
		img_convert( &output, PIX_FMT_RGB32, (AVPicture *)frame, pix_fmt, width, height );
	}
	else
	{
		AVPicture output;
		avpicture_fill( &output, buffer, PIX_FMT_YUYV422, width, height );
		img_convert( &output, PIX_FMT_YUYV422, (AVPicture *)frame, pix_fmt, width, height );
	}
#endif
}

/** Allocate the image buffer and set it on the frame.
*/

static int allocate_buffer( mlt_frame frame, AVCodecContext *codec_context, uint8_t **buffer, mlt_image_format *format, int *width, int *height )
{
	int size = 0;

	if ( codec_context->width == 0 || codec_context->height == 0 )
		return size;
	*width = codec_context->width;
	*height = codec_context->height;
	size = mlt_image_format_size( *format, *width, *height, NULL );
	*buffer = mlt_pool_alloc( size );
	if ( *buffer )
		mlt_frame_set_image( frame, *buffer, size, mlt_pool_release );
	else
		size = 0;

	return size;
}

/** Get an image from a frame.
*/

static int producer_get_image( mlt_frame frame, uint8_t **buffer, mlt_image_format *format, int *width, int *height, int writable )
{
	// Get the producer
	producer_avformat self = mlt_frame_pop_service( frame );
	mlt_producer producer = self->parent;

	// Get the properties from the frame
	mlt_properties frame_properties = MLT_FRAME_PROPERTIES( frame );

	// Obtain the frame number of this frame
	mlt_position position = mlt_frame_original_position( frame );

	// Get the producer properties
	mlt_properties properties = MLT_PRODUCER_PROPERTIES( producer );

	pthread_mutex_lock( &self->video_mutex );

	// Fetch the video format context
	AVFormatContext *context = self->video_format;

	// Get the video stream
	AVStream *stream = context->streams[ self->video_index ];

	// Get codec context
	AVCodecContext *codec_context = stream->codec;

	uint8_t *alpha = NULL;
	int got_picture = 0;
	int image_size = 0;

	// Get the image cache
	if ( ! self->image_cache )
	{
		// if cache size supplied by environment variable
		int cache_supplied = getenv( "MLT_AVFORMAT_CACHE" ) != NULL;
		int cache_size = cache_supplied? atoi( getenv( "MLT_AVFORMAT_CACHE" ) ) : 0;

		// cache size supplied via property
		if ( mlt_properties_get( properties, "cache" ) )
		{
			cache_supplied = 1;
			cache_size = mlt_properties_get_int( properties, "cache" );
		}
		if ( mlt_properties_get_int( properties, "noimagecache" ) )
			cache_size = 0;
		// create cache if not disabled
		if ( !cache_supplied || cache_size > 0 )
			self->image_cache = mlt_cache_init();
		// set cache size if supplied
		if ( self->image_cache && cache_supplied )
			mlt_cache_set_size( self->image_cache, cache_size );
	}
	if ( self->image_cache )
	{
		mlt_frame original = mlt_cache_get_frame( self->image_cache, position );
		if ( original )
		{
			mlt_properties orig_props = MLT_FRAME_PROPERTIES( original );
			int size = 0;

			*buffer = mlt_properties_get_data( orig_props, "alpha", &size );
			if (*buffer)
				mlt_frame_set_alpha( frame, *buffer, size, NULL );
			*buffer = mlt_properties_get_data( orig_props, "image", &size );
			mlt_frame_set_image( frame, *buffer, size, NULL );
			mlt_properties_set_data( frame_properties, "avformat.image_cache", original, 0, (mlt_destructor) mlt_frame_close, NULL );
			*format = mlt_properties_get_int( orig_props, "format" );

			// Set the resolution
			*width = codec_context->width;
			*height = codec_context->height;

			// Workaround 1088 encodings missing cropping info.
			if ( *height == 1088 && mlt_profile_dar( mlt_service_profile( MLT_PRODUCER_SERVICE( producer ) ) ) == 16.0/9.0 )
				*height = 1080;

			got_picture = 1;
			goto exit_get_image;
		}
	}
	// Cache miss

	// We may want to use the source fps if available
	double source_fps = mlt_properties_get_double( properties, "meta.media.frame_rate_num" ) /
		mlt_properties_get_double( properties, "meta.media.frame_rate_den" );

	// This is the physical frame position in the source
	int64_t req_position = ( int64_t )( position / mlt_producer_get_fps( producer ) * source_fps + 0.5 );

	// Determines if we have to decode all frames in a sequence
	// Temporary hack to improve intra frame only
	int must_decode = !( codec_context->codec && codec_context->codec->name ) || (
				  strcmp( codec_context->codec->name, "dnxhd" ) &&
				  strcmp( codec_context->codec->name, "dvvideo" ) &&
				  strcmp( codec_context->codec->name, "huffyuv" ) &&
				  strcmp( codec_context->codec->name, "mjpeg" ) &&
				  strcmp( codec_context->codec->name, "rawvideo" ) );

	double delay = mlt_properties_get_double( properties, "video_delay" );

	// Seek if necessary
	const char *interp = mlt_properties_get( frame_properties, "rescale.interp" );
	int preseek = must_decode
#if defined(FFUDIV) && LIBAVFORMAT_VERSION_INT >= ((53<<16)+(24<<8)+2)
		&& ( interp && strcmp( interp, "nearest" ) )
#endif
		&& codec_context->has_b_frames;
	int paused = seek_video( self, position, req_position, preseek );

	// Seek might have reopened the file
	context = self->video_format;
	stream = context->streams[ self->video_index ];
	codec_context = stream->codec;
	if ( *format == mlt_image_none ||
			codec_context->pix_fmt == PIX_FMT_ARGB ||
			codec_context->pix_fmt == PIX_FMT_RGBA ||
			codec_context->pix_fmt == PIX_FMT_ABGR ||
			codec_context->pix_fmt == PIX_FMT_BGRA )
		*format = pick_pix_format( codec_context->pix_fmt );

	// Duplicate the last image if necessary
	if ( self->av_frame && self->av_frame->linesize[0]
		 && ( paused || self->current_position >= req_position ) )
	{
		// Duplicate it
		if ( ( image_size = allocate_buffer( frame, codec_context, buffer, format, width, height ) ) )
		{
			// Workaround 1088 encodings missing cropping info.
			if ( *height == 1088 && mlt_profile_dar( mlt_service_profile( MLT_PRODUCER_SERVICE( producer ) ) ) == 16.0/9.0 )
				*height = 1080;
#ifdef VDPAU
			if ( self->vdpau && self->vdpau->buffer )
			{
				AVPicture picture;
				picture.data[0] = self->vdpau->buffer;
				picture.data[2] = self->vdpau->buffer + codec_context->width * codec_context->height;
				picture.data[1] = self->vdpau->buffer + codec_context->width * codec_context->height * 5 / 4;
				picture.linesize[0] = codec_context->width;
				picture.linesize[1] = codec_context->width / 2;
				picture.linesize[2] = codec_context->width / 2;
				convert_image( self, (AVFrame*) &picture, *buffer,
					PIX_FMT_YUV420P, format, *width, *height, &alpha );
			}
			else
#endif
			convert_image( self, self->av_frame, *buffer, codec_context->pix_fmt,
				format, *width, *height, &alpha );
			got_picture = 1;
		}
	}
	else
	{
		int ret = 0;
		int64_t int_position = 0;
		int decode_errors = 0;

		// Construct an AVFrame for YUV422 conversion
		if ( !self->av_frame )
			self->av_frame = avcodec_alloc_frame( );

		while( ret >= 0 && !got_picture )
		{
			// Read a packet
			if ( self->pkt.stream_index == self->video_index )
				av_free_packet( &self->pkt );
			av_init_packet( &self->pkt );
			pthread_mutex_lock( &self->packets_mutex );
			if ( mlt_deque_count( self->vpackets ) )
			{
				AVPacket *tmp = (AVPacket*) mlt_deque_pop_front( self->vpackets );
				self->pkt = *tmp;
				free( tmp );
			}
			else
			{
				ret = av_read_frame( context, &self->pkt );
				if ( ret >= 0 && !self->seekable && self->pkt.stream_index == self->audio_index )
				{
					if ( !av_dup_packet( &self->pkt ) )
					{
						AVPacket *tmp = malloc( sizeof(AVPacket) );
						*tmp = self->pkt;
						mlt_deque_push_back( self->apackets, tmp );
					}
				}
			}
			pthread_mutex_unlock( &self->packets_mutex );

			// We only deal with video from the selected video_index
			if ( ret >= 0 && self->pkt.stream_index == self->video_index && self->pkt.size > 0 )
			{
				int64_t pts = best_pts( self, self->pkt.pts, self->pkt.dts );
				if ( pts != AV_NOPTS_VALUE )
				{
					if ( !self->seekable && self->first_pts == AV_NOPTS_VALUE )
						self->first_pts = pts;
					if ( self->first_pts != AV_NOPTS_VALUE )
						pts -= self->first_pts;
					else if ( context->start_time != AV_NOPTS_VALUE )
						pts -= context->start_time;
					int_position = ( int64_t )( ( av_q2d( stream->time_base ) * pts + delay ) * source_fps + 0.5 );
					if ( int_position == self->last_position )
						int_position = self->last_position + 1;
				}
				mlt_log_debug( MLT_PRODUCER_SERVICE(producer),
					"V pkt.pts %"PRId64" pkt.dts %"PRId64" req_pos %"PRId64" cur_pos %"PRId64" pkt_pos %"PRId64"\n",
					self->pkt.pts, self->pkt.dts, req_position, self->current_position, int_position );

				// Make a dumb assumption on streams that contain wild timestamps
				if ( abs( req_position - int_position ) > 999 )
				{
					int_position = req_position;
					mlt_log_warning( MLT_PRODUCER_SERVICE(producer), " WILD TIMESTAMP!\n" );
				}
				self->last_position = int_position;

				// Decode the image
				if ( must_decode || int_position >= req_position )
				{
#ifdef VDPAU
					if ( self->vdpau )
					{
						if ( self->vdpau->decoder == VDP_INVALID_HANDLE )
						{
							vdpau_decoder_init( self );
						}
						self->vdpau->is_decoded = 0;
					}
#endif
					codec_context->reordered_opaque = int_position;
					if ( int_position >= req_position )
						codec_context->skip_loop_filter = AVDISCARD_NONE;
#if (LIBAVCODEC_VERSION_INT >= ((52<<16)+(26<<8)+0))
					ret = avcodec_decode_video2( codec_context, self->av_frame, &got_picture, &self->pkt );
#else
					ret = avcodec_decode_video( codec_context, self->av_frame, &got_picture, self->pkt.data, self->pkt.size );
#endif
					// Note: decode may fail at the beginning of MPEGfile (B-frames referencing before first I-frame), so allow a few errors.
					if ( ret < 0 )
					{
						if ( ++decode_errors <= 10 )
							ret = 0;
					}
					else
					{
						decode_errors = 0;
					}
				}

				if ( got_picture )
				{
					// Get position of reordered frame
					int_position = self->av_frame->reordered_opaque;
#if (LIBAVCODEC_VERSION_INT >= ((52<<16)+(106<<8)+0))
					pts = best_pts( self, self->av_frame->pkt_pts, self->av_frame->pkt_dts );
					if ( pts != AV_NOPTS_VALUE )
					{
						if ( self->first_pts != AV_NOPTS_VALUE )
							pts -= self->first_pts;
						else if ( context->start_time != AV_NOPTS_VALUE )
							pts -= context->start_time;
						int_position = ( int64_t )( ( av_q2d( stream->time_base ) * pts + delay ) * source_fps + 0.5 );
					}
#endif

					if ( int_position < req_position )
						got_picture = 0;
					else if ( int_position >= req_position )
						codec_context->skip_loop_filter = AVDISCARD_NONE;
				}
				mlt_log_debug( MLT_PRODUCER_SERVICE(producer), " got_pic %d key %d\n", got_picture, self->pkt.flags & PKT_FLAG_KEY );
			}

			// Now handle the picture if we have one
			if ( got_picture )
			{
				if ( ( image_size = allocate_buffer( frame, codec_context, buffer, format, width, height ) ) )
				{
					// Workaround 1088 encodings missing cropping info.
					if ( *height == 1088 && mlt_profile_dar( mlt_service_profile( MLT_PRODUCER_SERVICE( producer ) ) ) == 16.0/9.0 )
						*height = 1080;
#ifdef VDPAU
					if ( self->vdpau )
					{
						if ( self->vdpau->is_decoded )
						{
							struct vdpau_render_state *render = (struct vdpau_render_state*) self->av_frame->data[0];
							void *planes[3];
							uint32_t pitches[3];
							VdpYCbCrFormat dest_format = VDP_YCBCR_FORMAT_YV12;
							
							if ( !self->vdpau->buffer )
								self->vdpau->buffer = mlt_pool_alloc( codec_context->width * codec_context->height * 3 / 2 );
							self->av_frame->data[0] = planes[0] = self->vdpau->buffer;
							self->av_frame->data[2] = planes[1] = self->vdpau->buffer + codec_context->width * codec_context->height;
							self->av_frame->data[1] = planes[2] = self->vdpau->buffer + codec_context->width * codec_context->height * 5 / 4;
							self->av_frame->linesize[0] = pitches[0] = codec_context->width;
							self->av_frame->linesize[1] = pitches[1] = codec_context->width / 2;
							self->av_frame->linesize[2] = pitches[2] = codec_context->width / 2;

							VdpStatus status = vdp_surface_get_bits( render->surface, dest_format, planes, pitches );
							if ( status == VDP_STATUS_OK )
							{
								convert_image( self, self->av_frame, *buffer, PIX_FMT_YUV420P,
									format, *width, *height, &alpha );
							}
							else
							{
								mlt_log_error( MLT_PRODUCER_SERVICE(producer), "VDPAU Error: %s\n", vdp_get_error_string( status ) );
								image_size = self->vdpau->is_decoded = 0;
							}
						}
						else
						{
							mlt_log_error( MLT_PRODUCER_SERVICE(producer), "VDPAU error in VdpDecoderRender\n" );
							image_size = got_picture = 0;
						}
					}
					else
#endif
					convert_image( self, self->av_frame, *buffer, codec_context->pix_fmt,
						format, *width, *height, &alpha );
					self->top_field_first |= self->av_frame->top_field_first;
					self->current_position = int_position;
				}
				else
				{
					got_picture = 0;
				}
			}

			// Free packet data if not video and not live audio packet
			if ( self->pkt.stream_index != self->video_index &&
				 !( !self->seekable && self->pkt.stream_index == self->audio_index ) )
				av_free_packet( &self->pkt );
		}
	}

	// set alpha
	if ( alpha )
		mlt_frame_set_alpha( frame, alpha, (*width) * (*height), mlt_pool_release );

	if ( image_size > 0 && self->image_cache )
	{
		mlt_properties_set_int( frame_properties, "format", *format );
		mlt_cache_put_frame( self->image_cache, frame );
	}

	// Try to duplicate last image if there was a decoding failure
	// TODO: with multithread decoding a partial frame decoding resulting
	// in failure also resets av_frame making test below fail.
	if ( !image_size && self->av_frame && self->av_frame->linesize[0] )
	{
		// Duplicate it
		if ( ( image_size = allocate_buffer( frame, codec_context, buffer, format, width, height ) ) )
		{
			// Workaround 1088 encodings missing cropping info.
			if ( *height == 1088 && mlt_profile_dar( mlt_service_profile( MLT_PRODUCER_SERVICE( producer ) ) ) == 16.0/9.0 )
				*height = 1080;
#ifdef VDPAU
			if ( self->vdpau && self->vdpau->buffer )
			{
				AVPicture picture;
				picture.data[0] = self->vdpau->buffer;
				picture.data[2] = self->vdpau->buffer + codec_context->width * codec_context->height;
				picture.data[1] = self->vdpau->buffer + codec_context->width * codec_context->height * 5 / 4;
				picture.linesize[0] = codec_context->width;
				picture.linesize[1] = codec_context->width / 2;
				picture.linesize[2] = codec_context->width / 2;
				convert_image( self, (AVFrame*) &picture, *buffer,
					PIX_FMT_YUV420P, format, *width, *height, &alpha );
			}
			else
#endif
			convert_image( self, self->av_frame, *buffer, codec_context->pix_fmt,
				format, *width, *height, &alpha );
			got_picture = 1;
		}
	}

	// Regardless of speed, we expect to get the next frame (cos we ain't too bright)
	self->video_expected = position + 1;

exit_get_image:

	pthread_mutex_unlock( &self->video_mutex );

	// Set the progressive flag
	if ( mlt_properties_get( properties, "force_progressive" ) )
		mlt_properties_set_int( frame_properties, "progressive", !!mlt_properties_get_int( properties, "force_progressive" ) );
	else if ( self->av_frame )
		mlt_properties_set_int( frame_properties, "progressive", !self->av_frame->interlaced_frame );

	// Set the field order property for this frame
	if ( mlt_properties_get( properties, "force_tff" ) )
		mlt_properties_set_int( frame_properties, "top_field_first", !!mlt_properties_get_int( properties, "force_tff" ) );
	else
		mlt_properties_set_int( frame_properties, "top_field_first", self->top_field_first );

	// Set immutable properties of the selected track's (or overridden) source attributes.
	mlt_service_lock( MLT_PRODUCER_SERVICE( producer ) );
	mlt_properties_set_int( properties, "meta.media.top_field_first", self->top_field_first );
	mlt_properties_set_int( properties, "meta.media.progressive", mlt_properties_get_int( frame_properties, "progressive" ) );
	mlt_service_unlock( MLT_PRODUCER_SERVICE( producer ) );

	// If we already have RGB, then the full range processing either happened already
	// or does not apply (RGB source).
	if ( *format == mlt_image_rgb24 || *format == mlt_image_rgb24a || *format == mlt_image_opengl )
		mlt_properties_set( frame_properties, "force_full_luma", NULL );

	return !got_picture;
}

/** Process properties as AVOptions and apply to AV context obj
*/

static void apply_properties( void *obj, mlt_properties properties, int flags )
{
	int i;
	int count = mlt_properties_count( properties );
	for ( i = 0; i < count; i++ )
	{
		const char *opt_name = mlt_properties_get_name( properties, i );
#if LIBAVUTIL_VERSION_INT >= ((51<<16)+(10<<8)+0)
		const AVOption *opt = av_opt_find( obj, opt_name, NULL, flags, flags );
#else
		const AVOption *opt = av_find_opt( obj, opt_name, NULL, flags, flags );
#endif
		if ( opt_name && mlt_properties_get( properties, opt_name ) )
		{
			if ( opt )
#if LIBAVUTIL_VERSION_INT >= ((51<<16)+(12<<8)+0)
				av_opt_set( obj, opt_name, mlt_properties_get( properties, opt_name), 0 );
#elif LIBAVCODEC_VERSION_INT >= ((52<<16)+(7<<8)+0)
				av_set_string3( obj, opt_name, mlt_properties_get( properties, opt_name), 0, NULL );
#elif LIBAVCODEC_VERSION_INT >= ((51<<16)+(59<<8)+0)
				av_set_string2( obj, opt_name, mlt_properties_get( properties, opt_name), 0 );
#else
				av_set_string( obj, opt_name, mlt_properties_get( properties, opt_name) );
#endif
		}
	}
}

/** Initialize the video codec context.
 */

static int video_codec_init( producer_avformat self, int index, mlt_properties properties )
{
	// Initialise the codec if necessary
	if ( !self->video_codec )
	{
		// Get the video stream
		AVStream *stream = self->video_format->streams[ index ];

		// Get codec context
		AVCodecContext *codec_context = stream->codec;

		// Find the codec
		AVCodec *codec = avcodec_find_decoder( codec_context->codec_id );
#ifdef VDPAU
		if ( codec_context->codec_id == CODEC_ID_H264 )
		{
			if ( ( codec = avcodec_find_decoder_by_name( "h264_vdpau" ) ) )
			{
				if ( vdpau_init( self ) )
				{
					self->video_codec = codec_context;
					if ( !vdpau_decoder_init( self ) )
						vdpau_fini( self );
				}
			}
			if ( !self->vdpau )
				codec = avcodec_find_decoder( codec_context->codec_id );
		}
#endif

		// Initialise multi-threading
		int thread_count = mlt_properties_get_int( properties, "threads" );
		if ( thread_count == 0 && getenv( "MLT_AVFORMAT_THREADS" ) )
			thread_count = atoi( getenv( "MLT_AVFORMAT_THREADS" ) );
		if ( thread_count > 1 )
			codec_context->thread_count = thread_count;

		// If we don't have a codec and we can't initialise it, we can't do much more...
		pthread_mutex_lock( &self->open_mutex );
#if LIBAVCODEC_VERSION_INT >= ((53<<16)+(8<<8)+0)
		if ( codec && avcodec_open2( codec_context, codec, NULL ) >= 0 )
#else
		if ( codec && avcodec_open( codec_context, codec ) >= 0 )
#endif
		{
			// Now store the codec with its destructor
			self->video_codec = codec_context;
		}
		else
		{
			// Remember that we can't use this later
			self->video_index = -1;
			pthread_mutex_unlock( &self->open_mutex );
			return 0;
		}
		pthread_mutex_unlock( &self->open_mutex );

		// Process properties as AVOptions
		apply_properties( codec_context, properties, AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_DECODING_PARAM );
#if LIBAVCODEC_VERSION_INT >= ((52<<16)+(122<<8)+0)
		if ( codec->priv_class && codec_context->priv_data )
			apply_properties( codec_context->priv_data, properties, AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_DECODING_PARAM );
#endif

		// Reset some image properties
		if ( self->video_codec )
		{
			mlt_properties_set_int( properties, "width", self->video_codec->width );
			mlt_properties_set_int( properties, "height", self->video_codec->height );
		}
		// For DV, we'll just use the saved aspect ratio
		if ( codec_context->codec_id != CODEC_ID_DVVIDEO )
			get_aspect_ratio( properties, stream, self->video_codec, NULL );

		// Determine the fps first from the codec
		double source_fps = (double) self->video_codec->time_base.den /
								   ( self->video_codec->time_base.num == 0 ? 1 : self->video_codec->time_base.num );
		
		{
			// If the muxer reports a frame rate different than the codec
#if LIBAVFORMAT_VERSION_INT >= ((52<<16)+(42<<8)+0)
			double muxer_fps = av_q2d( stream->avg_frame_rate );
			if ( isnan( muxer_fps ) || muxer_fps == 0 )
				muxer_fps = av_q2d( stream->r_frame_rate );
#else
			double muxer_fps = av_q2d( stream->r_frame_rate );
#endif
			// Choose the lesser - the wrong tends to be off by some multiple of 10
			source_fps = FFMIN( source_fps, muxer_fps );
			if ( source_fps >= 1.0 && ( source_fps < muxer_fps || isnan( muxer_fps ) ) )
			{
				mlt_properties_set_int( properties, "meta.media.frame_rate_num", self->video_codec->time_base.den );
				mlt_properties_set_int( properties, "meta.media.frame_rate_den", self->video_codec->time_base.num == 0 ? 1 : self->video_codec->time_base.num );
			}
			else if ( muxer_fps > 0 )
			{
				AVRational frame_rate = stream->r_frame_rate;
				// With my samples when r_frame_rate != 1000 but avg_frame_rate is valid,
				// avg_frame_rate gives some approximate value that does not well match the media.
				// Also, on my sample where r_frame_rate = 1000, using avg_frame_rate directly
				// results in some very choppy output, but some value slightly different works
				// great.
#if LIBAVFORMAT_VERSION_INT >= ((52<<16)+(42<<8)+0)
				if ( av_q2d( stream->r_frame_rate ) >= 1000 && av_q2d( stream->avg_frame_rate ) > 0 )
					frame_rate = av_d2q( av_q2d( stream->avg_frame_rate ), 1024 );
#endif
				mlt_properties_set_int( properties, "meta.media.frame_rate_num", frame_rate.num );
				mlt_properties_set_int( properties, "meta.media.frame_rate_den", frame_rate.den );
			}
			else
			{
				source_fps = mlt_producer_get_fps( self->parent );
				AVRational frame_rate = av_d2q( source_fps, 255 );
				mlt_properties_set_int( properties, "meta.media.frame_rate_num", frame_rate.num );
				mlt_properties_set_int( properties, "meta.media.frame_rate_den", frame_rate.den );
			}
		}
		if ( mlt_properties_get( properties, "force_fps" ) )
		{
			double source_fps = mlt_properties_get_double( properties, "force_fps" );
			AVRational fps = av_d2q( source_fps, 1024 );
			stream->time_base.num *= mlt_properties_get_int( properties, "meta.media.frame_rate_num" ) * fps.den;
			stream->time_base.den *= mlt_properties_get_int( properties, "meta.media.frame_rate_den" ) * fps.num;
			mlt_properties_set_int( properties, "meta.media.frame_rate_num", fps.num );
			mlt_properties_set_int( properties, "meta.media.frame_rate_den", fps.den );
		}

		// Set the YUV colorspace from override or detect
		self->colorspace = mlt_properties_get_int( properties, "force_colorspace" );
#if LIBAVCODEC_VERSION_INT > ((52<<16)+(28<<8)+0)		
		if ( ! self->colorspace )
		{
			switch ( self->video_codec->colorspace )
			{
			case AVCOL_SPC_SMPTE240M:
				self->colorspace = 240;
				break;
			case AVCOL_SPC_BT470BG:
			case AVCOL_SPC_SMPTE170M:
				self->colorspace = 601;
				break;
			case AVCOL_SPC_BT709:
				self->colorspace = 709;
				break;
			default:
				// This is a heuristic Charles Poynton suggests in "Digital Video and HDTV"
				self->colorspace = self->video_codec->width * self->video_codec->height > 750000 ? 709 : 601;
				break;
			}
		}
#endif
		// Let apps get chosen colorspace
		mlt_properties_set_int( properties, "meta.media.colorspace", self->colorspace );

		self->full_luma = -1;
#if LIBAVCODEC_VERSION_INT >= ((52<<16)+(72<<8)+2)
		mlt_log_debug( MLT_PRODUCER_SERVICE(self->parent), "color_range %d\n", codec_context->color_range );
		if ( codec_context->color_range == AVCOL_RANGE_JPEG )
			self->full_luma = 1;
#endif
		if ( mlt_properties_get( properties, "set.force_full_luma" ) )
			self->full_luma = mlt_properties_get_int( properties, "set.force_full_luma" );
	}
	return self->video_codec && self->video_index > -1;
}

/** Set up video handling.
*/

static void producer_set_up_video( producer_avformat self, mlt_frame frame )
{
	// Get the producer
	mlt_producer producer = self->parent;

	// Get the properties
	mlt_properties properties = MLT_PRODUCER_PROPERTIES( producer );

	// Fetch the video format context
	AVFormatContext *context = self->video_format;

	// Get the video_index
	int index = mlt_properties_get_int( properties, "video_index" );

	// Reopen the file if necessary
	if ( !context && index > -1 )
	{
		producer_open( self, mlt_service_profile( MLT_PRODUCER_SERVICE(producer) ),
			mlt_properties_get( properties, "resource" ), 1 );
		context = self->video_format;
	}

	// Exception handling for video_index
	if ( context && index >= (int) context->nb_streams )
	{
		// Get the last video stream
		for ( index = context->nb_streams - 1;
			  index >= 0 && context->streams[ index ]->codec->codec_type != CODEC_TYPE_VIDEO;
			  index-- );
		mlt_properties_set_int( properties, "video_index", index );
	}
	if ( context && index > -1 && context->streams[ index ]->codec->codec_type != CODEC_TYPE_VIDEO )
	{
		// Invalidate the video stream
		index = -1;
		mlt_properties_set_int( properties, "video_index", index );
	}

	// Update the video properties if the index changed
	if ( index != self->video_index )
	{
		// Reset the video properties if the index changed
		self->video_index = index;
		pthread_mutex_lock( &self->open_mutex );
		if ( self->video_codec )
			avcodec_close( self->video_codec );
		self->video_codec = NULL;
		pthread_mutex_unlock( &self->open_mutex );
	}

	// Get the frame properties
	mlt_properties frame_properties = MLT_FRAME_PROPERTIES( frame );

	// Get the codec
	if ( context && index > -1 && video_codec_init( self, index, properties ) )
	{
		// Set the frame properties
		double force_aspect_ratio = mlt_properties_get_double( properties, "force_aspect_ratio" );
		double aspect_ratio = ( force_aspect_ratio > 0.0 ) ?
			force_aspect_ratio : mlt_properties_get_double( properties, "aspect_ratio" );

		// Set the width and height
		mlt_properties_set_int( frame_properties, "width", self->video_codec->width );
		mlt_properties_set_int( frame_properties, "height", self->video_codec->height );
		mlt_properties_set_int( properties, "meta.media.width", self->video_codec->width );
		mlt_properties_set_int( properties, "meta.media.height", self->video_codec->height );
		mlt_properties_set_double( frame_properties, "aspect_ratio", aspect_ratio );
		mlt_properties_set_int( frame_properties, "colorspace", self->colorspace );

		// Workaround 1088 encodings missing cropping info.
		if ( self->video_codec->height == 1088 && mlt_profile_dar( mlt_service_profile( MLT_PRODUCER_SERVICE( producer ) ) ) == 16.0/9.0 )
		{
			mlt_properties_set_int( properties, "meta.media.height", 1080 );
		}

		// Add our image operation
		mlt_frame_push_service( frame, self );
		mlt_frame_push_get_image( frame, producer_get_image );
	}
	else
	{
		// If something failed, use test card image
		mlt_properties_set_int( frame_properties, "test_image", 1 );
	}
}

static int seek_audio( producer_avformat self, mlt_position position, double timecode )
{
	int paused = 0;

	// Seek if necessary
	if ( self->seekable && position != self->audio_expected )
	{
		if ( position + 1 == self->audio_expected )
		{
			// We're paused - silence required
			paused = 1;
		}
		else if ( position < self->audio_expected || position - self->audio_expected >= 12 )
		{
			AVFormatContext *context = self->audio_format;
			int64_t timestamp = ( int64_t )( timecode * AV_TIME_BASE + 0.5 );
			if ( context->start_time != AV_NOPTS_VALUE )
				timestamp += context->start_time;
			if ( timestamp < 0 )
				timestamp = 0;

			// Set to the real timecode
			if ( av_seek_frame( context, -1, timestamp, AVSEEK_FLAG_BACKWARD ) != 0 )
				paused = 1;

			// Clear the usage in the audio buffer
			int i = MAX_AUDIO_STREAMS + 1;
			while ( --i )
				self->audio_used[i - 1] = 0;
		}
	}
	return paused;
}

static int sample_bytes( AVCodecContext *context )
{
#if LIBAVUTIL_VERSION_INT >= ((51<<16)+(8<<8)+0)
	return av_get_bytes_per_sample( context->sample_fmt );
#elif LIBAVCODEC_VERSION_MAJOR >= 53
	return av_get_bits_per_sample_fmt( context->sample_fmt ) / 8;
#else
	return av_get_bits_per_sample_format( context->sample_fmt ) / 8;
#endif
}

static void planar_to_interleaved( uint8_t *dest, uint8_t *src, int samples, int channels, int bytes_per_sample )
{
	int s, c;
	for ( s = 0; s < samples; s++ )
	{
		for ( c = 0; c < channels; c++ )
		{
			memcpy( dest, src + ( c * samples + s ) * bytes_per_sample, bytes_per_sample );
			dest += bytes_per_sample;
		}
	}
}

static int decode_audio( producer_avformat self, int *ignore, AVPacket pkt, int channels, int samples, double timecode, double fps )
{
	// Fetch the audio_format
	AVFormatContext *context = self->audio_format;

	// Get the current stream index
	int index = pkt.stream_index;

	// Get codec context
	AVCodecContext *codec_context = self->audio_codec[ index ];

	// Obtain the resample context if it exists (not always needed)
	ReSampleContext *resample = self->audio_resample[ index ];

	// Obtain the audio buffers
	uint8_t *audio_buffer = self->audio_buffer[ index ];
	uint8_t *decode_buffer = self->decode_buffer[ index ];

	int audio_used = self->audio_used[ index ];
	uint8_t *ptr = pkt.data;
	int len = pkt.size;
	int ret = 0;

	while ( ptr && ret >= 0 && len > 0 )
	{
		int sizeof_sample = resample? sizeof( int16_t ) : sample_bytes( codec_context );
		int data_size = self->audio_buffer_size[ index ];

		// Decode the audio
#if (LIBAVCODEC_VERSION_INT >= ((52<<16)+(26<<8)+0))
		ret = avcodec_decode_audio3( codec_context, (int16_t*) decode_buffer, &data_size, &pkt );
#elif (LIBAVCODEC_VERSION_INT >= ((51<<16)+(29<<8)+0))
		ret = avcodec_decode_audio2( codec_context, decode_buffer, &data_size, ptr, len );
#else
		ret = avcodec_decode_audio( codec_context, decode_buffer, &data_size, ptr, len );
#endif
		if ( ret < 0 )
		{
			mlt_log_warning( MLT_PRODUCER_SERVICE(self->parent), "audio decoding error %d\n", ret );
			break;
		}

		pkt.size = len -= ret;
		pkt.data = ptr += ret;

		// If decoded successfully
		if ( data_size > 0 )
		{
			// Figure out how many samples will be needed after resampling
			int convert_samples = data_size / codec_context->channels / sample_bytes( codec_context );
			int samples_needed = self->resample_factor * convert_samples;

			// Resize audio buffer to prevent overflow
			if ( ( audio_used + samples_needed ) * channels * sizeof_sample > self->audio_buffer_size[ index ] )
			{
				self->audio_buffer_size[ index ] = ( audio_used + samples_needed * 2 ) * channels * sizeof_sample;
				audio_buffer = self->audio_buffer[ index ] = mlt_pool_realloc( audio_buffer, self->audio_buffer_size[ index ] );
			}
			if ( resample )
			{
				// Copy to audio buffer while resampling
				uint8_t *source = decode_buffer;
				uint8_t *dest = &audio_buffer[ audio_used * channels * sizeof_sample ];
				audio_used += audio_resample( resample, (short*) dest, (short*) source, convert_samples );
			}
			else
			{
				uint8_t *source = decode_buffer;
				uint8_t *dest = &audio_buffer[ audio_used * codec_context->channels * sizeof_sample ];
				switch ( codec_context->sample_fmt )
				{
#if LIBAVUTIL_VERSION_INT >= ((51<<16)+(17<<8)+0)
				case AV_SAMPLE_FMT_S16P:
				case AV_SAMPLE_FMT_S32P:
				case AV_SAMPLE_FMT_FLTP:
					planar_to_interleaved( dest, source, convert_samples, codec_context->channels, sizeof_sample );
					break;
#endif
				default:
					// Straight copy to audio buffer
					memcpy( dest, decode_buffer, data_size );
				}
				audio_used += convert_samples;
			}

			// Handle ignore
			while ( *ignore && audio_used )
			{
				*ignore -= 1;
				audio_used -= audio_used > samples ? samples : audio_used;
				memmove( audio_buffer, &audio_buffer[ samples * (resample? channels : codec_context->channels) * sizeof_sample ],
						 audio_used * sizeof_sample );
			}
		}
	}

	// If we're behind, ignore this packet
	// Skip this on non-seekable, audio-only inputs.
	if ( pkt.pts >= 0 && ( self->seekable || self->video_format ) && *ignore == 0 && audio_used > samples / 2 )
	{
		int64_t pts = pkt.pts;
		if ( self->first_pts != 0 )
			pts -= self->first_pts;
		else if ( context->start_time != AV_NOPTS_VALUE )
			pts -= context->start_time;
		double timebase = av_q2d( context->streams[ index ]->time_base );
		int64_t int_position = ( int64_t )( timebase * pts * fps + 0.5 );
		int64_t req_position = ( int64_t )( timecode * fps + 0.5 );

		mlt_log_debug( MLT_PRODUCER_SERVICE(self->parent),
			"A pkt.pts %"PRId64" pkt.dts %"PRId64" req_pos %"PRId64" cur_pos %"PRId64" pkt_pos %"PRId64"\n",
			pkt.pts, pkt.dts, req_position, self->current_position, int_position );

		if ( int_position > 0 )
		{
			if ( int_position < req_position )
				// We are behind, so skip some
				*ignore = req_position - int_position;
			else if ( self->audio_index != INT_MAX && int_position > req_position + 2 )
				// We are ahead, so seek backwards some more
				seek_audio( self, req_position, timecode - 1.0 );
		}
	}

	self->audio_used[ index ] = audio_used;

	return ret;
}

/** Get the audio from a frame.
*/
static int producer_get_audio( mlt_frame frame, void **buffer, mlt_audio_format *format, int *frequency, int *channels, int *samples )
{
	// Get the producer
	producer_avformat self = mlt_frame_pop_audio( frame );

	pthread_mutex_lock( &self->audio_mutex );
	
	// Obtain the frame number of this frame
	mlt_position position = mlt_frame_original_position( frame );

	// Calculate the real time code
	double real_timecode = producer_time_of_frame( self->parent, position );

	// Get the producer fps
	double fps = mlt_producer_get_fps( self->parent );

	// Number of frames to ignore (for ffwd)
	int ignore[ MAX_AUDIO_STREAMS ] = { 0 };

	// Flag for paused (silence)
	int paused = seek_audio( self, position, real_timecode );

	// Initialize ignore for all streams from the seek return value
	int i = MAX_AUDIO_STREAMS;
	while ( i-- )
		ignore[i] = ignore[0];

	// Fetch the audio_format
	AVFormatContext *context = self->audio_format;

	int sizeof_sample = sizeof( int16_t );
	
	// Determine the tracks to use
	int index = self->audio_index;
	int index_max = self->audio_index + 1;
	if ( self->audio_index == INT_MAX )
	{
		index = 0;
		index_max = FFMIN( MAX_AUDIO_STREAMS, context->nb_streams );
		*channels = self->total_channels;
		*samples = *samples * FFMAX( self->max_frequency, *frequency ) / *frequency;
		*frequency = FFMAX( self->max_frequency, *frequency );
	}

	// Initialize the resamplers and buffers
	for ( ; index < index_max && index < MAX_AUDIO_STREAMS; index++ )
	{
		// Get codec context
		AVCodecContext *codec_context = self->audio_codec[ index ];

		if ( codec_context && !self->audio_buffer[ index ] )
		{
#if LIBAVCODEC_VERSION_INT < ((54<<16)+(26<<8)+0)
			// Check for resample and create if necessary
			if ( codec_context->channels <= 2 )
			{
				// Determine by how much resampling will increase number of samples
				double resample_factor = self->audio_index == INT_MAX ? 1 : (double) *channels / codec_context->channels;
				resample_factor *= (double) *frequency / codec_context->sample_rate;
				if ( resample_factor > self->resample_factor )
					self->resample_factor = resample_factor;
				
				// Create the resampler
#if (LIBAVCODEC_VERSION_INT >= ((52<<16)+(15<<8)+0))
				self->audio_resample[ index ] = av_audio_resample_init(
					self->audio_index == INT_MAX ? codec_context->channels : *channels,
					codec_context->channels, *frequency, codec_context->sample_rate,
					AV_SAMPLE_FMT_S16, codec_context->sample_fmt, 16, 10, 0, 0.8 );
#else
				self->audio_resample[ index ] = audio_resample_init(
					self->audio_index == INT_MAX ? codec_context->channels : *channels,
					codec_context->channels, *frequency, codec_context->sample_rate );
#endif
			}
			else
#endif
			{
				codec_context->request_channels = self->audio_index == INT_MAX ? codec_context->channels : *channels;
				sizeof_sample = sample_bytes( codec_context );
			}

			// Check for audio buffer and create if necessary
			self->audio_buffer_size[ index ] = AVCODEC_MAX_AUDIO_FRAME_SIZE * sizeof_sample;
			self->audio_buffer[ index ] = mlt_pool_alloc( self->audio_buffer_size[ index ] );

			// Check for decoder buffer and create if necessary
			self->decode_buffer[ index ] = av_malloc( self->audio_buffer_size[ index ] );
		}
	}

	// Get the audio if required
	if ( !paused && *frequency > 0 )
	{
		int ret	= 0;
		int got_audio = 0;
		AVPacket pkt;

		av_init_packet( &pkt );
		
		// If not resampling, give consumer more than requested.
		// It requested number samples based on requested frame rate.
		// Do not clean this up with a samples *= ...!
		if ( self->audio_index != INT_MAX && ! self->audio_resample[ self->audio_index ] )
			*samples = *samples * self->audio_codec[ self->audio_index ]->sample_rate / *frequency;

		while ( ret >= 0 && !got_audio )
		{
			// Check if the buffer already contains the samples required
			if ( self->audio_index != INT_MAX &&
				 self->audio_used[ self->audio_index ] >= *samples &&
				 ignore[ self->audio_index ] == 0 )
			{
				got_audio = 1;
				break;
			}
			else if ( self->audio_index == INT_MAX )
			{
				// Check if there is enough audio for all streams
				got_audio = 1;
				for ( index = 0; got_audio && index < index_max; index++ )
					if ( ( self->audio_codec[ index ] && self->audio_used[ index ] < *samples ) || ignore[ index ] )
						got_audio = 0;
				if ( got_audio )
					break;
			}

			// Read a packet
			pthread_mutex_lock( &self->packets_mutex );
			if ( mlt_deque_count( self->apackets ) )
			{
				AVPacket *tmp = (AVPacket*) mlt_deque_pop_front( self->apackets );
				pkt = *tmp;
				free( tmp );
			}
			else
			{
				ret = av_read_frame( context, &pkt );
				if ( ret >= 0 && !self->seekable && pkt.stream_index == self->video_index )
				{
					if ( !av_dup_packet( &pkt ) )
					{
						AVPacket *tmp = malloc( sizeof(AVPacket) );
						*tmp = pkt;
						mlt_deque_push_back( self->vpackets, tmp );
					}
				}
			}
			pthread_mutex_unlock( &self->packets_mutex );

			// We only deal with audio from the selected audio index
			index = pkt.stream_index;
			if ( index < MAX_AUDIO_STREAMS && ret >= 0 && pkt.data && pkt.size > 0 && ( index == self->audio_index ||
				 ( self->audio_index == INT_MAX && context->streams[ index ]->codec->codec_type == CODEC_TYPE_AUDIO ) ) )
			{
				int channels2 = ( self->audio_index == INT_MAX || !self->audio_resample[index] ) ?
					self->audio_codec[index]->channels : *channels;
				ret = decode_audio( self, &ignore[index], pkt, channels2, *samples, real_timecode, fps );
			}

			if ( self->seekable || index != self->video_index )
				av_free_packet( &pkt );

		}

		// Set some additional return values
		*format = mlt_audio_s16;
		if ( self->audio_index != INT_MAX && !self->audio_resample[ self->audio_index ] )
		{
			index = self->audio_index;
			*channels = self->audio_codec[ index ]->channels;
			*frequency = self->audio_codec[ index ]->sample_rate;
			*format = pick_audio_format( self->audio_codec[ index ]->sample_fmt );
			sizeof_sample = sample_bytes( self->audio_codec[ index ] );
		}
		else if ( self->audio_index == INT_MAX )
		{
			for ( index = 0; index < index_max; index++ )
				if ( self->audio_codec[ index ] && !self->audio_resample[ index ] )
				{
					// XXX: This only works if all audio tracks have the same sample format.
					*format = pick_audio_format( self->audio_codec[ index ]->sample_fmt );
					sizeof_sample = sample_bytes( self->audio_codec[ index ] );
					break;
				}
		}

		// Allocate and set the frame's audio buffer
		int size = mlt_audio_format_size( *format, *samples, *channels );
		*buffer = mlt_pool_alloc( size );
		mlt_frame_set_audio( frame, *buffer, *format, size, mlt_pool_release );

		// Interleave tracks if audio_index=all
		if ( self->audio_index == INT_MAX )
		{
			uint8_t *dest = *buffer;
			int i;
			for ( i = 0; i < *samples; i++ )
			{
				for ( index = 0; index < index_max; index++ )
				if ( self->audio_codec[ index ] )
				{
					int current_channels = self->audio_codec[ index ]->channels;
					uint8_t *src = self->audio_buffer[ index ] + i * current_channels * sizeof_sample;
					memcpy( dest, src, current_channels * sizeof_sample );
					dest += current_channels * sizeof_sample;
				}
			}
			for ( index = 0; index < index_max; index++ )
			if ( self->audio_codec[ index ] && self->audio_used[ index ] >= *samples )
			{
				int current_channels = self->audio_codec[ index ]->channels;
				uint8_t *src = self->audio_buffer[ index ] + *samples * current_channels * sizeof_sample;
				self->audio_used[index] -= *samples;
				memmove( self->audio_buffer[ index ], src, self->audio_used[ index ] * current_channels * sizeof_sample );
			}
		}
		// Copy a single track to the output buffer
		else
		{
			index = self->audio_index;

			// Now handle the audio if we have enough
			if ( self->audio_used[ index ] > 0 )
			{
				uint8_t *src = self->audio_buffer[ index ];
				// copy samples from audio_buffer
				size = self->audio_used[ index ] < *samples ? self->audio_used[ index ] : *samples;
				memcpy( *buffer, src, size * *channels * sizeof_sample );
				// supply the remaining requested samples as silence
				if ( *samples > self->audio_used[ index ] )
					memset( *buffer + size * *channels * sizeof_sample, 0, ( *samples - self->audio_used[ index ] ) * *channels * sizeof_sample );
				// reposition the samples within audio_buffer
				self->audio_used[ index ] -= size;
				memmove( src, src + size * *channels * sizeof_sample, self->audio_used[ index ] * *channels * sizeof_sample );
			}
			else
			{
				// Otherwise fill with silence
				memset( *buffer, 0, *samples * *channels * sizeof_sample );
			}
		}
	}
	else
	{
		// Get silence and don't touch the context
		mlt_frame_get_audio( frame, buffer, format, frequency, channels, samples );
	}
	
	// Regardless of speed (other than paused), we expect to get the next frame
	if ( !paused )
		self->audio_expected = position + 1;

	pthread_mutex_unlock( &self->audio_mutex );

	return 0;
}

/** Initialize the audio codec context.
*/

static int audio_codec_init( producer_avformat self, int index, mlt_properties properties )
{
	// Initialise the codec if necessary
	if ( !self->audio_codec[ index ] )
	{
		// Get codec context
		AVCodecContext *codec_context = self->audio_format->streams[index]->codec;

		// Find the codec
		AVCodec *codec = avcodec_find_decoder( codec_context->codec_id );

		// If we don't have a codec and we can't initialise it, we can't do much more...
		pthread_mutex_lock( &self->open_mutex );
#if LIBAVCODEC_VERSION_INT >= ((53<<16)+(8<<8)+0)
		if ( codec && avcodec_open2( codec_context, codec, NULL ) >= 0 )
#else
		if ( codec && avcodec_open( codec_context, codec ) >= 0 )
#endif
		{
			// Now store the codec with its destructor
			if ( self->audio_codec[ index ] )
				avcodec_close( self->audio_codec[ index ] );
			self->audio_codec[ index ] = codec_context;
		}
		else
		{
			// Remember that we can't use self later
			self->audio_index = -1;
		}
		pthread_mutex_unlock( &self->open_mutex );

		// Process properties as AVOptions
		apply_properties( codec_context, properties, AV_OPT_FLAG_AUDIO_PARAM | AV_OPT_FLAG_DECODING_PARAM );
#if LIBAVCODEC_VERSION_INT >= ((52<<16)+(122<<8)+0)
		if ( codec && codec->priv_class && codec_context->priv_data )
			apply_properties( codec_context->priv_data, properties, AV_OPT_FLAG_AUDIO_PARAM | AV_OPT_FLAG_DECODING_PARAM );
#endif
	}
	return self->audio_codec[ index ] && self->audio_index > -1;
}

/** Set up audio handling.
*/

static void producer_set_up_audio( producer_avformat self, mlt_frame frame )
{
	// Get the producer
	mlt_producer producer = self->parent;

	// Get the properties
	mlt_properties properties = MLT_PRODUCER_PROPERTIES( producer );

	// Fetch the audio format context
	AVFormatContext *context = self->audio_format;

	mlt_properties frame_properties = MLT_FRAME_PROPERTIES( frame );

	// Get the audio_index
	int index = mlt_properties_get_int( properties, "audio_index" );

	// Handle all audio tracks
	if ( self->audio_index > -1 &&
	     mlt_properties_get( properties, "audio_index" ) &&
	     !strcmp( mlt_properties_get( properties, "audio_index" ), "all" ) )
		index = INT_MAX;

	// Reopen the file if necessary
	if ( !context && self->audio_index > -1 && index > -1 )
	{
		producer_open( self, mlt_service_profile( MLT_PRODUCER_SERVICE(producer) ),
			mlt_properties_get( properties, "resource" ), 1 );
		context = self->audio_format;
	}

	// Exception handling for audio_index
	if ( context && index >= (int) context->nb_streams && index < INT_MAX )
	{
		for ( index = context->nb_streams - 1;
			  index >= 0 && context->streams[ index ]->codec->codec_type != CODEC_TYPE_AUDIO;
			  index-- );
		mlt_properties_set_int( properties, "audio_index", index );
	}
	if ( context && index > -1 && index < INT_MAX &&
		 context->streams[ index ]->codec->codec_type != CODEC_TYPE_AUDIO )
	{
		index = self->audio_index;
		mlt_properties_set_int( properties, "audio_index", index );
	}

	// Update the audio properties if the index changed
	if ( context && index > -1 && index != self->audio_index )
	{
		pthread_mutex_lock( &self->open_mutex );
		if ( self->audio_codec[ self->audio_index ] )
			avcodec_close( self->audio_codec[ self->audio_index ] );
		self->audio_codec[ self->audio_index ] = NULL;
		pthread_mutex_unlock( &self->open_mutex );
	}
	if ( self->audio_index != -1 )
		self->audio_index = index;
	else
		index = -1;

	// Get the codec(s)
	if ( context && index == INT_MAX )
	{
		mlt_properties_set_int( frame_properties, "audio_frequency", self->max_frequency );
		mlt_properties_set_int( frame_properties, "audio_channels", self->total_channels );
		for ( index = 0; index < context->nb_streams; index++ )
		{
			if ( context->streams[ index ]->codec->codec_type == CODEC_TYPE_AUDIO )
				audio_codec_init( self, index, properties );
		}
	}
	else if ( context && index > -1 && audio_codec_init( self, index, properties ) )
	{
		// Set the frame properties
		if ( index < MAX_AUDIO_STREAMS )
		{
			mlt_properties_set_int( frame_properties, "frequency", self->audio_codec[ index ]->sample_rate );
			mlt_properties_set_int( frame_properties, "channels", self->audio_codec[ index ]->channels );
		}
	}
	if ( context && index > -1 )
	{
		// Add our audio operation
		mlt_frame_push_audio( frame, self );
		mlt_frame_push_audio( frame, producer_get_audio );
	}
}

/** Our get frame implementation.
*/

static int producer_get_frame( mlt_producer producer, mlt_frame_ptr frame, int index )
{
	// Access the private data
	mlt_service service = MLT_PRODUCER_SERVICE( producer );
	mlt_cache_item cache_item = mlt_service_cache_get( service, "producer_avformat" );
	producer_avformat self = mlt_cache_item_data( cache_item, NULL );

	// If cache miss
	if ( !self )
	{
		self = calloc( 1, sizeof( struct producer_avformat_s ) );
		producer->child = self;
		self->parent = producer;
		mlt_service_cache_put( service, "producer_avformat", self, 0, (mlt_destructor) producer_avformat_close );
		cache_item = mlt_service_cache_get( service, "producer_avformat" );
	}

	// Create an empty frame
	*frame = mlt_frame_init( service);
	
	if ( *frame )
	{
		mlt_properties_set_data( MLT_FRAME_PROPERTIES(*frame), "avformat_cache", cache_item, 0, (mlt_destructor) mlt_cache_item_close, NULL );
	}
	else
	{
		mlt_cache_item_close( cache_item );
		return 1;
	}

	// Update timecode on the frame we're creating
	mlt_frame_set_position( *frame, mlt_producer_position( producer ) );

	// Set up the video
	producer_set_up_video( self, *frame );

	// Set up the audio
	producer_set_up_audio( self, *frame );

	// Set the position of this producer
	mlt_position position = self->seekable ? mlt_producer_frame( producer ) : self->nonseek_position++;
	mlt_properties_set_position( MLT_FRAME_PROPERTIES( *frame ), "original_position", position );

	// Calculate the next timecode
	mlt_producer_prepare_next( producer );

	return 0;
}

static void producer_avformat_close( producer_avformat self )
{
	mlt_log_debug( NULL, "producer_avformat_close\n" );

	// Cleanup av contexts
	av_free_packet( &self->pkt );
	av_free( self->av_frame );
	pthread_mutex_lock( &self->open_mutex );
	int i;
	for ( i = 0; i < MAX_AUDIO_STREAMS; i++ )
	{
		if ( self->audio_resample[i] )
			audio_resample_close( self->audio_resample[i] );
		mlt_pool_release( self->audio_buffer[i] );
		av_free( self->decode_buffer[i] );
		if ( self->audio_codec[i] )
			avcodec_close( self->audio_codec[i] );
		self->audio_codec[i] = NULL;
	}
	if ( self->video_codec )
		avcodec_close( self->video_codec );
	self->video_codec = NULL;
	// Close the file
#if LIBAVFORMAT_VERSION_INT >= ((53<<16)+(17<<8)+0)
	if ( self->dummy_context )
		avformat_close_input( &self->dummy_context );
	if ( self->seekable && self->audio_format )
		avformat_close_input( &self->audio_format );
	if ( self->video_format )
		avformat_close_input( &self->video_format );
#else
	if ( self->dummy_context )
		av_close_input_file( self->dummy_context );
	if ( self->seekable && self->audio_format )
		av_close_input_file( self->audio_format );
	if ( self->video_format )
		av_close_input_file( self->video_format );
#endif
	pthread_mutex_unlock( &self->open_mutex );
#ifdef VDPAU
	vdpau_producer_close( self );
#endif
	if ( self->image_cache )
		mlt_cache_close( self->image_cache );

	// Cleanup the mutexes
	pthread_mutex_destroy( &self->audio_mutex );
	pthread_mutex_destroy( &self->video_mutex );
	pthread_mutex_destroy( &self->packets_mutex );
	pthread_mutex_destroy( &self->open_mutex );

	// Cleanup the packet queues
	AVPacket *pkt;
	if ( self->apackets )
	{
		while ( ( pkt = mlt_deque_pop_back( self->apackets ) ) )
		{
			av_free_packet( pkt );
			free( pkt );
		}
		mlt_deque_close( self->apackets );
		self->apackets = NULL;
	}
	if ( self->vpackets )
	{
		while ( ( pkt = mlt_deque_pop_back( self->vpackets ) ) )
		{
			av_free_packet( pkt );
			free( pkt );
		}
		mlt_deque_close( self->vpackets );
		self->vpackets = NULL;
	}

	free( self );
}

static void producer_close( mlt_producer parent )
{
	// Remove this instance from the cache
	mlt_service_cache_purge( MLT_PRODUCER_SERVICE(parent) );

	// Close the parent
	parent->close = NULL;
	mlt_producer_close( parent );

	// Free the memory
	free( parent );
}
