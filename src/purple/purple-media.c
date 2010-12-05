/**
 * @file purple-media.c
 *
 * pidgin-sipe
 *
 * Copyright (C) 2010 SIPE Project <http://sipe.sourceforge.net/>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "glib.h"
#include "glib/gstdio.h"
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include "sipe-common.h"

#include "mediamanager.h"
#include "media-gst.h"
#include "request.h"
#include "agent.h"

#include "sipe-backend.h"
#include "sipe-core.h"

#include "purple-private.h"

struct sipe_backend_media {
	PurpleMedia *m;
	GSList *streams;
};

struct sipe_backend_stream {
	gchar *sessionid;
	gchar *participant;
	gboolean candidates_prepared;
	gboolean local_on_hold;
	gboolean remote_on_hold;
};

static void
backend_stream_free(struct sipe_backend_stream *stream)
{
	if (stream) {
		g_free(stream->sessionid);
		g_free(stream->participant);
		g_free(stream);
	}
}

static PurpleMediaSessionType sipe_media_to_purple(SipeMediaType type);
static PurpleMediaCandidateType sipe_candidate_type_to_purple(SipeCandidateType type);
static SipeCandidateType purple_candidate_type_to_sipe(PurpleMediaCandidateType type);
static PurpleMediaNetworkProtocol sipe_network_protocol_to_purple(SipeNetworkProtocol proto);
static SipeNetworkProtocol purple_network_protocol_to_sipe(PurpleMediaNetworkProtocol proto);

struct stream_info_context {
	struct sipe_media_call *call;
	struct sipe_backend_media *backend_media;
};

static void
on_candidates_prepared_cb(SIPE_UNUSED_PARAMETER PurpleMedia *media,
			  gchar *sessionid,
			  SIPE_UNUSED_PARAMETER gchar *participant,
			  struct stream_info_context *ctx)
{
	struct sipe_backend_stream *stream;
	stream = sipe_backend_media_get_stream_by_id(ctx->backend_media, sessionid);

	stream->candidates_prepared = TRUE;

	if (ctx->call->candidates_prepared_cb &&
	    sipe_backend_candidates_prepared(ctx->backend_media)) {
		ctx->call->candidates_prepared_cb(ctx->call, stream);
	}
}

static void
on_state_changed_cb(SIPE_UNUSED_PARAMETER PurpleMedia *media,
		    PurpleMediaState state,
		    gchar *sessionid,
		    gchar *participant,
		    struct sipe_media_call *call)
{
	// TODO: this should be aware of legacy backend_media
	SIPE_DEBUG_INFO("sipe_media_state_changed_cb: %d %s %s\n", state, sessionid, participant);
	if (state == PURPLE_MEDIA_STATE_CONNECTED && call->media_connected_cb)
		call->media_connected_cb(call);
}

static void
capture_pipeline(PurpleMedia *media, gchar *label) {
	PurpleMediaManager *manager = purple_media_get_manager(media);
	GstElement *pipeline = purple_media_manager_get_pipeline(manager);
	GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS(GST_BIN(pipeline), GST_DEBUG_GRAPH_SHOW_ALL, label);
}

static void
on_error_cb(PurpleMedia *media, gchar *message, struct stream_info_context *ctx)
{
	capture_pipeline(media, "ERROR");

	if (ctx->call->error_cb)
		ctx->call->error_cb(ctx->call, ctx->backend_media, message);
}

static void
on_stream_info_cb(SIPE_UNUSED_PARAMETER PurpleMedia *media,
		  PurpleMediaInfoType type,
		  gchar *sessionid,
		  gchar *participant,
		  gboolean local,
		  struct stream_info_context *ctx)
{
	if (type == PURPLE_MEDIA_INFO_ACCEPT && ctx->call->call_accept_cb
	    && !sessionid && !participant)
		ctx->call->call_accept_cb(ctx->call, local);
	else if (type == PURPLE_MEDIA_INFO_HOLD || type == PURPLE_MEDIA_INFO_UNHOLD) {

		gboolean state = (type == PURPLE_MEDIA_INFO_HOLD);

		if (sessionid) {
			// Hold specific stream
			struct sipe_backend_stream *stream;
			stream = sipe_backend_media_get_stream_by_id(ctx->backend_media,
								     sessionid);

			if (local)
				stream->local_on_hold = state;
			else
				stream->remote_on_hold = state;
		} else {
			// Hold all streams
			GSList *i = sipe_backend_media_get_streams(ctx->backend_media);
			for (; i; i = i->next) {
				struct sipe_backend_stream *stream = i->data;

				if (local)
					stream->local_on_hold = state;
				else
					stream->remote_on_hold = state;
			}
		}

		if (ctx->call->call_hold_cb)
			ctx->call->call_hold_cb(ctx->call, local, state);
	} else if (type == PURPLE_MEDIA_INFO_HANGUP || type == PURPLE_MEDIA_INFO_REJECT) {
		if (!sessionid && !participant) {
			if (type == PURPLE_MEDIA_INFO_HANGUP && ctx->call->call_hangup_cb)
				ctx->call->call_hangup_cb(ctx->call, ctx->backend_media, local);
			else if (type == PURPLE_MEDIA_INFO_REJECT && ctx->call->call_reject_cb)
				ctx->call->call_reject_cb(ctx->call, local);
		} else if (sessionid && participant) {
			struct sipe_backend_stream *stream;
			stream = sipe_backend_media_get_stream_by_id(ctx->backend_media,
								     sessionid);

			if (stream) {
				ctx->backend_media->streams = g_slist_remove(ctx->backend_media->streams, stream);
				backend_stream_free(stream);
			}
		}
	}
}

struct sipe_backend_media *
sipe_backend_media_new(struct sipe_core_public *sipe_public,
		       struct sipe_media_call *call,
		       const gchar *participant,
		       gboolean initiator)
{
	struct sipe_backend_media *media = g_new0(struct sipe_backend_media, 1);
	struct sipe_backend_private *purple_private = sipe_public->backend_private;
	PurpleMediaManager *manager = purple_media_manager_get();
	struct stream_info_context *ctx = g_new0(struct stream_info_context, 1);
	GstElement *pipeline;

	media->m = purple_media_manager_create_media(manager,
						     purple_private->account,
						     "fsrtpconference",
						     participant, initiator);

	ctx->call = call;
	ctx->backend_media = media;

	/* Passing the same ctx structure to all signal handlers, only the first
	 * of them has destroy_data closure and is responsible for freeing the
	 * context. */
	g_signal_connect_data(G_OBJECT(media->m), "candidates-prepared",
			      G_CALLBACK(on_candidates_prepared_cb), ctx,
			      (GClosureNotify) g_free, 0);
	g_signal_connect(G_OBJECT(media->m), "stream-info",
			 G_CALLBACK(on_stream_info_cb), ctx);
	g_signal_connect(G_OBJECT(media->m), "error",
			 G_CALLBACK(on_error_cb), ctx);
	g_signal_connect(G_OBJECT(media->m), "state-changed",
			 G_CALLBACK(on_state_changed_cb), call);

	/* On error, the pipeline is no longer in PLAYING state and libpurple
	 * will not switch it back to PLAYING, preventing any more calls until
	 * application restart. We switch the state ourselves here to negate
	 * effect of any error in previous call (if any). */
	pipeline = purple_media_manager_get_pipeline(manager);
	gst_element_set_state(pipeline, GST_STATE_PLAYING);

	return media;
}

void
sipe_backend_media_free(struct sipe_backend_media *media)
{
	if (media) {
		GSList *stream = media->streams;

		for (; stream; stream = g_slist_delete_link(stream, stream))
			backend_stream_free(stream->data);

		g_free(media);
	}
}

void
sipe_backend_media_set_cname(struct sipe_backend_media *media, gchar *cname)
{
	if (media) {
		GParameter *params = g_new0(GParameter, 4);
		params[0].name = "sdes-cname";
		g_value_init(&params[0].value, G_TYPE_STRING);
		g_value_set_string(&params[0].value, cname);
		params[1].name = "sdes-name";
		g_value_init(&params[1].value, G_TYPE_STRING);
		params[2].name = "sdes-tool";
		g_value_init(&params[2].value, G_TYPE_STRING);
		params[3].name = NULL;
		purple_media_set_params(media->m, params);

		g_value_unset(&params[0].value);
		g_free(params);
	}
}

#define FS_CODECS_CONF \
	"# Automatically created by SIPE plugin\n" \
	"[video/H263]\n" \
	"farsight-send-profile=videoscale ! ffmpegcolorspace ! fsvideoanyrate ! ffenc_h263 rtp-payload-size=30 ! rtph263pay\n";

static void
ensure_codecs_conf()
{
	gchar *filename;
	filename = g_build_filename(purple_user_dir(), "fs-codec.conf", NULL);

	if (!g_file_test(filename, G_FILE_TEST_EXISTS)) {
		int fd = g_open(filename, O_CREAT | O_WRONLY, S_IRUSR | S_IWUSR);
		gchar *fs_codecs_conf = FS_CODECS_CONF;
		if (!fd || write(fd, fs_codecs_conf, strlen(fs_codecs_conf)) == -1)
			SIPE_DEBUG_ERROR_NOFORMAT("Can not create fs-codec.conf!");
		close(fd);
	}

	g_free(filename);
}

struct sipe_backend_media_relays *
sipe_backend_media_relays_convert(GSList *media_relays, gchar *username, gchar *password)
{
	GValueArray *relay_info = g_value_array_new(0);

	for (; media_relays; media_relays = media_relays->next) {
		struct sipe_media_relay *relay = media_relays->data;
		GValue value;
		GstStructure *gst_relay_info;

		/* Skip relays where IP could not be resolved. */
		if (!relay->hostname)
			continue;

		gst_relay_info = gst_structure_new("relay-info",
				"ip", G_TYPE_STRING, relay->hostname,
				"port", G_TYPE_UINT, relay->udp_port,
				"username", G_TYPE_STRING, username,
				"password", G_TYPE_STRING, password,
				NULL);

		if (!gst_relay_info) {
			g_value_array_free(relay_info);
			return NULL;
		}

		memset(&value, 0, sizeof(GValue));
		g_value_init(&value, GST_TYPE_STRUCTURE);
		gst_value_set_structure(&value, gst_relay_info);

		relay_info = g_value_array_append(relay_info, &value);
		gst_structure_free(gst_relay_info);
	}

	return (struct sipe_backend_media_relays *)relay_info;
}

void
sipe_backend_media_relays_free(struct sipe_backend_media_relays *media_relays)
{
	g_value_array_free((GValueArray *)media_relays);
}

struct sipe_backend_stream *
sipe_backend_media_add_stream(struct sipe_backend_media *media,
			      const gchar *id,
			      const gchar *participant,
			      SipeMediaType type,
			      SipeIceVersion ice_version,
			      gboolean initiator,
			      struct sipe_backend_media_relays *media_relays)
{
	struct sipe_backend_stream *stream = NULL;
	PurpleMediaSessionType prpl_type = sipe_media_to_purple(type);
	GParameter *params = NULL;
	guint params_cnt = 0;
	gchar *transmitter;

	if (ice_version != SIPE_ICE_NO_ICE) {
		transmitter = "nice";
		params_cnt = 2;

		params = g_new0(GParameter, params_cnt);

		params[0].name = "compatibility-mode";
		g_value_init(&params[0].value, G_TYPE_UINT);
		g_value_set_uint(&params[0].value,
				 ice_version == SIPE_ICE_DRAFT_6 ?
				 NICE_COMPATIBILITY_OC2007 :
				 NICE_COMPATIBILITY_OC2007R2);

		if (media_relays) {
			params[1].name = "relay-info";
			g_value_init(&params[1].value, G_TYPE_VALUE_ARRAY);
			g_value_set_boxed(&params[1].value, media_relays);
		} else
			--params_cnt;
	} else {
		// TODO: session naming here, Communicator needs audio/video
		transmitter = "rawudp";
		//sessionid = "sipe-voice-rawudp";
	}

	ensure_codecs_conf();

	if (purple_media_add_stream(media->m, id, participant, prpl_type,
				    initiator, transmitter, params_cnt, params)) {
		stream = g_new0(struct sipe_backend_stream, 1);
		stream->sessionid = g_strdup(id);
		stream->participant = g_strdup(participant);
		stream->candidates_prepared = FALSE;

		media->streams = g_slist_append(media->streams, stream);
	}

	if (media_relays)
		g_value_unset(&params[1].value);

	g_free(params);

	return stream;
}

void
sipe_backend_media_remove_stream(struct sipe_backend_media *media,
				 struct sipe_backend_stream *stream)
{
	g_return_if_fail(media && stream);

	purple_media_end(media->m, stream->sessionid, NULL);
	media->streams = g_slist_remove(media->streams, stream);
	backend_stream_free(stream);
}

GSList *sipe_backend_media_get_streams(struct sipe_backend_media *media)
{
	return media->streams;
}

struct sipe_backend_stream *
sipe_backend_media_get_stream_by_id(struct sipe_backend_media *media,
				    const gchar *id)
{
	GSList *i;
	for (i = media->streams; i; i = i->next) {
		struct sipe_backend_stream *stream = i->data;
		if (sipe_strequal(stream->sessionid, id))
			return stream;
	}
	return NULL;
}

void
sipe_backend_media_add_remote_candidates(struct sipe_backend_media *media,
					 struct sipe_backend_stream *stream,
					 GList *candidates)
{
	purple_media_add_remote_candidates(media->m, stream->sessionid,
					   stream->participant, candidates);
}

gboolean sipe_backend_media_is_initiator(struct sipe_backend_media *media,
					 struct sipe_backend_stream *stream)
{
	return purple_media_is_initiator(media->m,
					 stream ? stream->sessionid : NULL,
					 stream ? stream->participant : NULL);
}

gboolean sipe_backend_media_accepted(struct sipe_backend_media *media)
{
	return purple_media_accepted(media->m, NULL, NULL);
}

gboolean
sipe_backend_candidates_prepared(struct sipe_backend_media *media)
{
	GSList *streams = media->streams;
	for (; streams; streams = streams->next) {
		struct sipe_backend_stream *s = streams->data;
		if (!s->candidates_prepared)
			return FALSE;
	}
	return TRUE;
}

GList *
sipe_backend_media_get_active_local_candidates(struct sipe_backend_media *media,
					       struct sipe_backend_stream *stream)
{
	return purple_media_get_active_local_candidates(media->m,
							stream->sessionid,
							stream->participant);
}

GList *
sipe_backend_media_get_active_remote_candidates(struct sipe_backend_media *media,
						struct sipe_backend_stream *stream)
{
	return purple_media_get_active_remote_candidates(media->m,
							 stream->sessionid,
							 stream->participant);
}

gchar *
sipe_backend_stream_get_id(struct sipe_backend_stream *stream)
{
	return stream->sessionid;
}

void sipe_backend_stream_hold(struct sipe_backend_media *media,
			      struct sipe_backend_stream *stream,
			      gboolean local)
{
	purple_media_stream_info(media->m, PURPLE_MEDIA_INFO_HOLD,
				 stream->sessionid, stream->participant,
				 local);
}

void sipe_backend_stream_unhold(struct sipe_backend_media *media,
				struct sipe_backend_stream *stream,
				gboolean local)
{
	purple_media_stream_info(media->m, PURPLE_MEDIA_INFO_UNHOLD,
				 stream->sessionid, stream->participant,
				 local);
}

gboolean sipe_backend_stream_is_held(struct sipe_backend_stream *stream)
{
	g_return_val_if_fail(stream, FALSE);

	return stream->local_on_hold || stream->remote_on_hold;
}

struct sipe_backend_codec *
sipe_backend_codec_new(int id, const char *name, SipeMediaType type, guint clock_rate)
{
	return (struct sipe_backend_codec *)purple_media_codec_new(id, name,
						    sipe_media_to_purple(type),
						    clock_rate);
}

void
sipe_backend_codec_free(struct sipe_backend_codec *codec)
{
	if (codec)
		g_object_unref(codec);
}

int
sipe_backend_codec_get_id(struct sipe_backend_codec *codec)
{
	return purple_media_codec_get_id((PurpleMediaCodec *)codec);
}

gchar *
sipe_backend_codec_get_name(struct sipe_backend_codec *codec)
{
	/* Not explicitly documented, but return value must be g_free()'d */
	return purple_media_codec_get_encoding_name((PurpleMediaCodec *)codec);
}

guint
sipe_backend_codec_get_clock_rate(struct sipe_backend_codec *codec)
{
	return purple_media_codec_get_clock_rate((PurpleMediaCodec *)codec);
}

void
sipe_backend_codec_add_optional_parameter(struct sipe_backend_codec *codec,
										  const gchar *name, const gchar *value)
{
	purple_media_codec_add_optional_parameter((PurpleMediaCodec *)codec, name, value);
}

GList *
sipe_backend_codec_get_optional_parameters(struct sipe_backend_codec *codec)
{
	return purple_media_codec_get_optional_parameters((PurpleMediaCodec *)codec);
}

gboolean
sipe_backend_set_remote_codecs(struct sipe_backend_media *media,
			       struct sipe_backend_stream *stream,
			       GList *codecs)
{
	return purple_media_set_remote_codecs(media->m,
					      stream->sessionid,
					      stream->participant,
					      codecs);
}

GList*
sipe_backend_get_local_codecs(struct sipe_backend_media *media,
			      struct sipe_backend_stream *stream)
{
	GList *codecs = purple_media_get_codecs(media->m,
						stream->sessionid);
	GList *i = codecs;

	/*
	 * Do not announce Theora. Its optional parameters are too long,
	 * Communicator rejects such SDP message and does not support the codec
	 * anyway.
	 */
	while (i) {
		PurpleMediaCodec *codec = i->data;
		gchar *encoding_name = purple_media_codec_get_encoding_name(codec);

		if (sipe_strequal(encoding_name,"THEORA")) {
			GList *tmp;
			g_object_unref(codec);
			tmp = i->next;
			codecs = g_list_delete_link(codecs, i);
			i = tmp;
		} else
			i = i->next;

		g_free(encoding_name);
	}

	return codecs;
}

struct sipe_backend_candidate *
sipe_backend_candidate_new(const gchar *foundation,
			   SipeComponentType component,
			   SipeCandidateType type, SipeNetworkProtocol proto,
			   const gchar *ip, guint port,
			   const gchar *username,
			   const gchar *password)
{
	PurpleMediaCandidate *c = purple_media_candidate_new(
		/* Libnice and Farsight rely on non-NULL foundation to
		 * distinguish between candidates of a component. When NULL
		 * foundation is passed (ie. ICE draft 6 does not use foudation),
		 * use username instead. If no foundation is provided, Farsight
		 * may signal an active candidate different from the one actually
		 * in use. See Farsight's agent_new_selected_pair() in
		 * fs-nice-stream-transmitter.h where first candidate in the
		 * remote list is always selected when no foundation. */
		foundation ? foundation : username,
		component,
		sipe_candidate_type_to_purple(type),
		sipe_network_protocol_to_purple(proto),
		ip,
		port);
	g_object_set(c, "username", username, "password", password, NULL);
	return (struct sipe_backend_candidate *)c;
}

void
sipe_backend_candidate_free(struct sipe_backend_candidate *candidate)
{
	if (candidate)
		g_object_unref(candidate);
}

gchar *
sipe_backend_candidate_get_username(struct sipe_backend_candidate *candidate)
{
	/* Not explicitly documented, but return value must be g_free()'d */
	return purple_media_candidate_get_username((PurpleMediaCandidate*)candidate);
}

gchar *
sipe_backend_candidate_get_password(struct sipe_backend_candidate *candidate)
{
	/* Not explicitly documented, but return value must be g_free()'d */
	return purple_media_candidate_get_password((PurpleMediaCandidate*)candidate);
}

gchar *
sipe_backend_candidate_get_foundation(struct sipe_backend_candidate *candidate)
{
	/* Not explicitly documented, but return value must be g_free()'d */
	return purple_media_candidate_get_foundation((PurpleMediaCandidate*)candidate);
}

gchar *
sipe_backend_candidate_get_ip(struct sipe_backend_candidate *candidate)
{
	/* Not explicitly documented, but return value must be g_free()'d */
	return purple_media_candidate_get_ip((PurpleMediaCandidate*)candidate);
}

guint
sipe_backend_candidate_get_port(struct sipe_backend_candidate *candidate)
{
	return purple_media_candidate_get_port((PurpleMediaCandidate*)candidate);
}

gchar *
sipe_backend_candidate_get_base_ip(struct sipe_backend_candidate *candidate)
{
	/* Not explicitly documented, but return value must be g_free()'d */
	return purple_media_candidate_get_base_ip((PurpleMediaCandidate*)candidate);
}

guint
sipe_backend_candidate_get_base_port(struct sipe_backend_candidate *candidate)
{
	return purple_media_candidate_get_base_port((PurpleMediaCandidate*)candidate);
}

guint32
sipe_backend_candidate_get_priority(struct sipe_backend_candidate *candidate)
{
	return purple_media_candidate_get_priority((PurpleMediaCandidate*)candidate);
}

void
sipe_backend_candidate_set_priority(struct sipe_backend_candidate *candidate, guint32 priority)
{
	g_object_set(candidate, "priority", priority, NULL);
}

SipeComponentType
sipe_backend_candidate_get_component_type(struct sipe_backend_candidate *candidate)
{
	return purple_media_candidate_get_component_id((PurpleMediaCandidate*)candidate);
}

SipeCandidateType
sipe_backend_candidate_get_type(struct sipe_backend_candidate *candidate)
{
	PurpleMediaCandidateType type =
		purple_media_candidate_get_candidate_type((PurpleMediaCandidate*)candidate);
	return purple_candidate_type_to_sipe(type);
}

SipeNetworkProtocol
sipe_backend_candidate_get_protocol(struct sipe_backend_candidate *candidate)
{
	PurpleMediaNetworkProtocol proto =
		purple_media_candidate_get_protocol((PurpleMediaCandidate*)candidate);
	return purple_network_protocol_to_sipe(proto);
}

static void
remove_lone_candidate_cb(SIPE_UNUSED_PARAMETER gpointer key,
			 gpointer value,
			 gpointer user_data)
{
	GList  *entry = value;
	GList **candidates = user_data;

	g_object_unref(entry->data);
	*candidates = g_list_delete_link(*candidates, entry);
}

static GList *
ensure_candidate_pairs(GList *candidates)
{
	GHashTable *lone_cand_links;
	GList	   *i;

	lone_cand_links = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

	for (i = candidates; i; i = i->next) {
		PurpleMediaCandidate *c = i->data;
		gchar *foundation = purple_media_candidate_get_foundation(c);

		if (g_hash_table_lookup(lone_cand_links, foundation)) {
			g_hash_table_remove(lone_cand_links, foundation);
			g_free(foundation);
		} else {
			g_hash_table_insert(lone_cand_links, foundation, i);
		}
	}

	g_hash_table_foreach(lone_cand_links, remove_lone_candidate_cb, &candidates);
	g_hash_table_destroy(lone_cand_links);

	return candidates;
}

GList *
sipe_backend_get_local_candidates(struct sipe_backend_media *media,
				  struct sipe_backend_stream *stream)
{
	GList *candidates = purple_media_get_local_candidates(media->m,
							      stream->sessionid,
							      stream->participant);
	/*
	 * Sometimes purple will not return complete list of candidates, even
	 * after "candidates-prepared" signal is emitted. This is a feature of
	 * libnice, namely affecting candidates discovered via UPnP. Nice does
	 * not wait until discovery is finished and can signal end of candidate
	 * gathering before all responses from UPnP enabled gateways are received.
	 *
	 * Remove any incomplete RTP+RTCP candidate pairs from the list.
	 */
	candidates = ensure_candidate_pairs(candidates);
	return candidates;
}

void
sipe_backend_media_accept(struct sipe_backend_media *media, gboolean local)
{
	if (media)
		purple_media_stream_info(media->m, PURPLE_MEDIA_INFO_ACCEPT,
					 NULL, NULL, local);
}

void
sipe_backend_media_hangup(struct sipe_backend_media *media, gboolean local)
{
	if (media)
		purple_media_stream_info(media->m, PURPLE_MEDIA_INFO_HANGUP,
					 NULL, NULL, local);
}

void
sipe_backend_media_reject(struct sipe_backend_media *media, gboolean local)
{
	if (media)
		purple_media_stream_info(media->m, PURPLE_MEDIA_INFO_REJECT,
					 NULL, NULL, local);
}

static PurpleMediaSessionType sipe_media_to_purple(SipeMediaType type)
{
	switch (type) {
		case SIPE_MEDIA_AUDIO: return PURPLE_MEDIA_AUDIO;
		case SIPE_MEDIA_VIDEO: return PURPLE_MEDIA_VIDEO;
		default:               return PURPLE_MEDIA_NONE;
	}
}

/*SipeMediaType purple_media_to_sipe(PurpleMediaSessionType type)
{
	switch (type) {
		case PURPLE_MEDIA_AUDIO: return SIPE_MEDIA_AUDIO;
		case PURPLE_MEDIA_VIDEO: return SIPE_MEDIA_VIDEO;
		default:				 return SIPE_MEDIA_AUDIO;
	}
}*/

static PurpleMediaCandidateType
sipe_candidate_type_to_purple(SipeCandidateType type)
{
	switch (type) {
		case SIPE_CANDIDATE_TYPE_HOST:	return PURPLE_MEDIA_CANDIDATE_TYPE_HOST;
		case SIPE_CANDIDATE_TYPE_RELAY:	return PURPLE_MEDIA_CANDIDATE_TYPE_RELAY;
		case SIPE_CANDIDATE_TYPE_SRFLX:	return PURPLE_MEDIA_CANDIDATE_TYPE_SRFLX;
		case SIPE_CANDIDATE_TYPE_PRFLX: return PURPLE_MEDIA_CANDIDATE_TYPE_PRFLX;
		default:			return PURPLE_MEDIA_CANDIDATE_TYPE_HOST;
	}
}

static SipeCandidateType
purple_candidate_type_to_sipe(PurpleMediaCandidateType type)
{
	switch (type) {
		case PURPLE_MEDIA_CANDIDATE_TYPE_HOST:	return SIPE_CANDIDATE_TYPE_HOST;
		case PURPLE_MEDIA_CANDIDATE_TYPE_RELAY:	return SIPE_CANDIDATE_TYPE_RELAY;
		case PURPLE_MEDIA_CANDIDATE_TYPE_SRFLX:	return SIPE_CANDIDATE_TYPE_SRFLX;
		case PURPLE_MEDIA_CANDIDATE_TYPE_PRFLX: return SIPE_CANDIDATE_TYPE_PRFLX;
		default:				return SIPE_CANDIDATE_TYPE_HOST;
	}
}

static PurpleMediaNetworkProtocol
sipe_network_protocol_to_purple(SipeNetworkProtocol proto)
{
	switch (proto) {
		case SIPE_NETWORK_PROTOCOL_TCP:	return PURPLE_MEDIA_NETWORK_PROTOCOL_TCP;
		case SIPE_NETWORK_PROTOCOL_UDP:	return PURPLE_MEDIA_NETWORK_PROTOCOL_UDP;
		default:						return PURPLE_MEDIA_NETWORK_PROTOCOL_TCP;
	}
}

static SipeNetworkProtocol
purple_network_protocol_to_sipe(PurpleMediaNetworkProtocol proto)
{
	switch (proto) {
		case PURPLE_MEDIA_NETWORK_PROTOCOL_TCP: return SIPE_NETWORK_PROTOCOL_TCP;
		case PURPLE_MEDIA_NETWORK_PROTOCOL_UDP: return SIPE_NETWORK_PROTOCOL_UDP;
		default:								return SIPE_NETWORK_PROTOCOL_UDP;
	}
}

/*
  Local Variables:
  mode: c
  c-file-style: "bsd"
  indent-tabs-mode: t
  tab-width: 8
  End:
*/
