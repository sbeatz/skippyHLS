/* skippyHLS
 *
 * Copyright (C) 2010 Marc-Andre Lureau <marcandre.lureau@gmail.com>
 * Copyright (C) 2010 Andoni Morales Alastruey <ylatuya@gmail.com>
 * Copyright (C) 2011, Hewlett-Packard Development Company, L.P.
 *  Author: Youness Alaoui <youness.alaoui@collabora.co.uk>, Collabora Ltd.
 *  Author: Sebastian Dröge <sebastian.droege@collabora.co.uk>, Collabora Ltd.
 * Copyright (C) 2014 Sebastian Dröge <sebastian@centricular.com>
 * Copyright (C) 2015, SoundCloud Ltd. (http://soundcloud.com)
 *  Author: Stephan Hesse <stephan@soundcloud.com>, SoundCloud Ltd.
 *
 * skippy_hlsdemux.c:
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include <string.h>
#include <math.h>

#include <skippyHLS/skippy_hlsdemux.h>

#define RETRY_TIME_BASE (500*GST_MSECOND)
#define RETRY_THRESHOLD 6 // when we switch from constant to exponential backoff retrial
#define RETRY_MAX_TIME_UNTIL (60*GST_SECOND)

// Must be doubles above zero
#define BUFFER_WATERMARK_HIGH_RATIO 0.5
#define BUFFER_WATERMARK_LOW_RATIO 0.5

#define DEFAULT_BUFFER_DURATION (30*GST_SECOND)

static GstStaticPadTemplate srctemplate = GST_STATIC_PAD_TEMPLATE ("src_%u",
    GST_PAD_SRC,
    GST_PAD_SOMETIMES,
    GST_STATIC_CAPS_ANY);

static GstStaticPadTemplate sinktemplate = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("application/x-hls"));

GST_DEBUG_CATEGORY_STATIC (skippy_hls_demux_debug);
#define GST_CAT_DEFAULT skippy_hls_demux_debug

typedef enum
{
  STAT_TIME_OF_FIRST_PLAYLIST,
  STAT_TIME_TO_PLAYLIST,
  STAT_TIME_TO_DOWNLOAD_FRAGMENT,
} SkippyHLSDemuxStats;

/* GObject */
static void skippy_hls_demux_dispose (GObject * obj);
static void skippy_hls_demux_finalize (GObject * obj);

/* GstElement */
static GstStateChangeReturn
skippy_hls_demux_change_state (GstElement * element, GstStateChange transition);

/* SkippyHLSDemux */
static GstFlowReturn skippy_hls_demux_sink_data (GstPad * pad, GstObject * parent, GstBuffer * buf);
static gboolean skippy_hls_demux_sink_event (GstPad * pad, GstObject * parent, GstEvent * event);
static gboolean skippy_hls_demux_src_event (GstPad * pad, GstObject * parent, GstEvent * event);
static gboolean skippy_hls_demux_src_query (GstPad * pad, GstObject * parent, GstQuery * query);
static gboolean skippy_hls_demux_handle_seek (SkippyHLSDemux *demux, GstEvent * event);
static void skippy_hls_demux_stream_loop (SkippyHLSDemux * demux);
static void skippy_hls_demux_stop (SkippyHLSDemux * demux);
static void skippy_hls_demux_pause (SkippyHLSDemux * demux);
static SkippyFragment *skippy_hls_demux_get_next_fragment (SkippyHLSDemux * demux, SkippyUriDownloaderFetchReturn* fetch_ret, GError ** err);
static void skippy_hls_demux_reset (SkippyHLSDemux * demux);
static void skippy_hls_demux_link_pads (SkippyHLSDemux * demux);
static gboolean skippy_hls_demux_refresh_playlist (SkippyHLSDemux * demux);

#define skippy_hls_demux_parent_class parent_class
G_DEFINE_TYPE (SkippyHLSDemux, skippy_hls_demux, GST_TYPE_BIN);

// Set up our class
static void
skippy_hls_demux_class_init (SkippyHLSDemuxClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *element_class;

  gobject_class = (GObjectClass *) klass;
  element_class = (GstElementClass *) klass;

  gobject_class->dispose = skippy_hls_demux_dispose;
  gobject_class->finalize = skippy_hls_demux_finalize;

  element_class->change_state = GST_DEBUG_FUNCPTR (skippy_hls_demux_change_state);

  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&srctemplate));

  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&sinktemplate));

  gst_element_class_set_static_metadata (element_class,
      "HLS Client/Demuxer",
      "Codec/Demuxer/Adaptive",
      "HTTP Live Streaming Demuxer",
      "Stephan Hesse <stephan@soundcloud.com>\n"
      "Marc-Andre Lureau <marcandre.lureau@gmail.com>\n"
      "Andoni Morales Alastruey <ylatuya@gmail.com>");

  GST_DEBUG_CATEGORY_INIT (skippy_hls_demux_debug, "skippyhlsdemux", 0,
      "Skippy HLS client");
}

// Creating the element with all the internal components whose lifetime will span across states
// See: _dispose and _finalize functions where we free all of this
static void
skippy_hls_demux_init (SkippyHLSDemux * demux)
{
  // Pads
  demux->srcpad = NULL;
  demux->sinkpad = gst_pad_new_from_static_template (&sinktemplate, "sink");

  // Configure sink pad
  gst_pad_set_chain_function (demux->sinkpad, GST_DEBUG_FUNCPTR (skippy_hls_demux_sink_data));
  gst_pad_set_event_function (demux->sinkpad, GST_DEBUG_FUNCPTR (skippy_hls_demux_sink_event));
  gst_element_add_pad (GST_ELEMENT (demux), demux->sinkpad);

  // Member objects
  demux->client = skippy_m3u8_client_new ();
  demux->playlist = NULL; // Storage for initial playlist

  // Internal elements
  demux->queue = gst_element_factory_make ("queue2", NULL);
  demux->queue_sinkpad = gst_element_get_static_pad (demux->queue, "sink");
  demux->downloader = skippy_uri_downloader_new ();
  demux->playlist_downloader = skippy_uri_downloader_new ();

  // Add bin elements
  gst_bin_add (GST_BIN (demux), demux->queue);
  gst_bin_add (GST_BIN (demux), GST_ELEMENT(demux->downloader));
  gst_bin_add (GST_BIN (demux), GST_ELEMENT(demux->playlist_downloader));

  // Thread
  g_rec_mutex_init (&demux->stream_lock);
  demux->stream_task = gst_task_new ((GstTaskFunction) skippy_hls_demux_stream_loop, demux, NULL);
  gst_task_set_lock (demux->stream_task, &demux->stream_lock);
}

// Dispose: Remove everything we allocated in _init
// NOTE: Elements and pads that have been added to the element/bin are owned by the element itself (unreffed by the parent handler)
static void
skippy_hls_demux_dispose (GObject * obj)
{
  GST_DEBUG ("Disposing ...");

  SkippyHLSDemux *demux = SKIPPY_HLS_DEMUX (obj);

  G_OBJECT_CLASS (parent_class)->dispose (obj);

  // Remove M3U8 client
  if (demux->client) {
    skippy_m3u8_client_free (demux->client);
    demux->client = NULL;
  }

  // Release ref to queue sinkpad
  if (demux->queue_sinkpad) {
    g_object_unref (demux->queue_sinkpad);
    demux->queue_sinkpad = NULL;
  }

  // Clean up thread
  if (demux->stream_task) {
    gst_object_unref (demux->stream_task);
    demux->stream_task = NULL;
  }

  GST_DEBUG ("Done cleaning up.");
}

static void
skippy_hls_demux_finalize (GObject * obj)
{
  GST_DEBUG ("Finalizing ...");
  SkippyHLSDemux *demux = SKIPPY_HLS_DEMUX (obj);
  g_rec_mutex_clear (&demux->stream_lock);
  G_OBJECT_CLASS (parent_class)->finalize (obj);
  GST_DEBUG ("Finalized.");
}

// This performs the transition from NULL -> READY (re-)allocating all the resources of this element,
// cleaning up from eventual previous state cycles
// and reconfigures the internal elements.
//
// MT-safe
static void
skippy_hls_demux_reset (SkippyHLSDemux * demux)
{
  GST_DEBUG ("Re-setting element");

  GST_OBJECT_LOCK (demux);
  // Reset all our state fields
  demux->position = 0;
  demux->download_failed_count = 0;
  demux->continuing = FALSE;

  // Get rid of eventual playlist data
  if (demux->playlist) {
    gst_buffer_unref (demux->playlist);
    demux->playlist = NULL;
  }

  // We might already have a source pad from a previous PLAYING state, clean up if necessary
  if (demux->srcpad) {
    GST_OBJECT_UNLOCK (demux);
    gst_element_remove_pad (GST_ELEMENT_CAST (demux), demux->srcpad);
    GST_OBJECT_LOCK (demux);
    demux->srcpad = NULL;
  }

  // Configure our queue
  if (demux->queue) {
    GST_OBJECT_UNLOCK (demux);
    // Configure internal queue: get rid of all size limitations, don't emit buffering messages
    g_object_set (demux->queue,
      "max-size-buffers", 0,
      "max-size-bytes", 0,
      "max-size-time", 0,
      "use-buffering", FALSE,
      NULL);
    GST_OBJECT_LOCK (demux);
  }

  GST_OBJECT_UNLOCK (demux);
}

// Called for state change from PAUSED -> READY and during seek handling
// Schedules the streaming thread to paused state, cancels all ongoing downloads,
// Locks&unlocks the streaming thread in order for this function to block until the
// thread is actually in paused state (task function exited)
//
// MT-safe
static void
skippy_hls_demux_pause (SkippyHLSDemux * demux)
{
  GST_DEBUG ("Pausing task ...");
  // Pause the task
  gst_task_pause (demux->stream_task);
  // Signal the thread in case it's waiting
  GST_OBJECT_LOCK (demux);
  demux->continuing = TRUE;
  GST_TASK_SIGNAL (demux->stream_task);
  GST_OBJECT_UNLOCK (demux);
  GST_DEBUG ("Checking for ongoing downloads to cancel ...");
  // Now cancel all downloads to make the stream function exit quickly in case there are some
  skippy_uri_downloader_cancel (demux->downloader);
  skippy_uri_downloader_cancel (demux->playlist_downloader);
  // Block until we're done cancelling
  g_rec_mutex_lock (&demux->stream_lock);
  g_rec_mutex_unlock (&demux->stream_lock);
  GST_DEBUG ("Paused streaming task");
}

static void
skippy_hls_demux_restart (SkippyHLSDemux * demux)
{
  // If we are already paused, this is just about restarting
  if (gst_task_get_state (demux->stream_task) == GST_TASK_PAUSED) {
    gst_task_start (demux->stream_task);
    return;
  }

  // Other case is if we want to interrupt a currently ongoing waiting for retrial
  // in which case we are running but want to interrupt waiting, resetting the count and
  // restart immediatly
  GST_OBJECT_LOCK (demux);
  if (demux->download_failed_count >= RETRY_THRESHOLD) {
    GST_OBJECT_UNLOCK (demux);
    skippy_hls_demux_pause (demux);
    // At this point the stream loop is paused
    demux->download_failed_count = 0;
    gst_task_start (demux->stream_task);
    return;
  }
  GST_OBJECT_UNLOCK (demux);

}

// Called for state change from READY -> NULL
// This will stop & join the task (given it's not stopped yet) in a blocking way
// Which means the function will block until the task is stopped i.e streaming thread is joined.
// This function assumes the task is already in paused state (i.e task loop not running)
// Therefore calling this function should not imply any waiting for the task function to exit
// but is simply to allow eventual further deallocation of the task resources (unref'ing the task).
// However we can also just restart the task once it has been stopped.
//
// MT-safe
static void
skippy_hls_demux_stop (SkippyHLSDemux *demux)
{
  // Let's join the streaming thread
  GST_DEBUG ("Stopping task ...");
  g_return_if_fail (gst_task_get_state (demux->stream_task) == GST_TASK_PAUSED);
  if (gst_task_get_state (demux->stream_task) != GST_TASK_STOPPED) {
    gst_task_join (demux->stream_task);
  }
  GST_DEBUG ("Stopped streaming task");
}

// The state change handler: simply calls the pause, stop and reset functions above
// and afterwards the parent handler
//
// MT-safe
static GstStateChangeReturn
skippy_hls_demux_change_state (GstElement * element, GstStateChange transition)
{
  GstStateChangeReturn ret;
  SkippyHLSDemux *demux = SKIPPY_HLS_DEMUX (element);

  GST_DEBUG ("Performing transition: %s -> %s", gst_element_state_get_name (GST_STATE_TRANSITION_CURRENT(transition)),
    gst_element_state_get_name (GST_STATE_TRANSITION_NEXT(transition)));

  switch (transition) {
    // Boot up
    case GST_STATE_CHANGE_NULL_TO_READY:
      // When we go from NULL to READY there is no concurrent function running with reset
      // Therefore no need to lock the object here
      skippy_hls_demux_reset (demux);
      break;
    // Start streaming thread
    case GST_STATE_CHANGE_READY_TO_PAUSED:
      // This is initially starting the task
      gst_task_start (demux->stream_task);
      break;
    case GST_STATE_CHANGE_PAUSED_TO_PLAYING:
      skippy_hls_demux_restart (demux);
      break;
    case GST_STATE_CHANGE_PLAYING_TO_PAUSED:
      break;
    // Interrupt streaming thread
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      // Can be called while streaming thread is running
      skippy_hls_demux_pause (demux);
      break;
    // Shut down
    case GST_STATE_CHANGE_READY_TO_NULL:
      // Will only be called after streaming thread was paused
      skippy_hls_demux_stop (demux);
      break;
    default:
      break;
  }

  return GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);
}

// Posts HLS statistics messages on the element bus
//
// MT-safe
static void
skippy_hls_demux_post_stat_msg (SkippyHLSDemux * demux, SkippyHLSDemuxStats metric, guint64 time_val, gsize size)
{
  GstStructure * structure = NULL;

  // Create message data
  switch (metric) {
  case STAT_TIME_TO_DOWNLOAD_FRAGMENT:
    GST_DEBUG ("Statistic: STAT_TIME_TO_DOWNLOAD_FRAGMENT");
    structure = gst_structure_new (SKIPPY_HLS_DEMUX_STATISTIC_MSG_NAME,
      "fragment-download-time", G_TYPE_UINT64, time_val,
      "fragment-size", G_TYPE_UINT64, (guint64) size,
      NULL);
    break;
  case STAT_TIME_TO_PLAYLIST:
    GST_DEBUG ("Statistic: STAT_TIME_TO_PLAYLIST");
    structure = gst_structure_new (SKIPPY_HLS_DEMUX_STATISTIC_MSG_NAME,
      "time-to-playlist", GST_TYPE_CLOCK_TIME, time_val,
      NULL);
    break;
  case STAT_TIME_OF_FIRST_PLAYLIST:
    GST_DEBUG ("Statistic: STAT_TIME_OF_FIRST_PLAYLIST");
    structure = gst_structure_new (SKIPPY_HLS_DEMUX_STATISTIC_MSG_NAME,
      "manifest-download-start", GST_TYPE_CLOCK_TIME, GST_CLOCK_TIME_NONE,
      "manifest-download-stop", GST_TYPE_CLOCK_TIME, time_val,
      NULL);
    break;
  default:
    GST_ERROR ("Can't post unknown stats type");
    return;
  }

  // Post the message on the bus
  gst_element_post_message (GST_ELEMENT_CAST (demux),
    gst_message_ref (gst_message_new_element (GST_OBJECT_CAST (demux), structure))
  );
}

// Queries current source URI from upstream element
// Returns NULL when query was not successful
// Caller owns returned pointer
//
// MT-safe
static gchar*
skippy_hls_demux_query_location (SkippyHLSDemux * demux)
{
  GstQuery* query = gst_query_new_uri ();
  gboolean ret = gst_pad_peer_query (demux->sinkpad, query);
  gboolean permanent;
  gchar *uri;

  if (ret) {
    gst_query_parse_uri_redirection (query, &uri);
    gst_query_parse_uri_redirection_permanent (query, &permanent);

    /* Only use the redirect target for permanent redirects */
    if (!permanent || uri == NULL) {
      g_free (uri);
      gst_query_parse_uri (query, &uri);
    }
    return uri;
  }
  gst_query_unref (query);
  return NULL;
}

// Queries current playback position from downstream element
// Returns MAXINT64 when query was not successful (which might happen in some pipeline states)
//
// MT-safe
static gint64
skippy_hls_demux_query_position (SkippyHLSDemux * demux)
{
  GstFormat format;
  gint64 pos;
  GstQuery* query;
  gboolean query_ret;

  // Query current playback position from downstream
  query = gst_query_new_position (GST_FORMAT_TIME);
  query_ret = gst_pad_peer_query (demux->srcpad, query);
  if (query_ret) {
    gst_query_parse_position (query, &format, &pos);
    if (format != GST_FORMAT_TIME) {
      GST_ERROR ("Position query result is not in TIME format");
      query_ret = FALSE;
    }
  }
  gst_query_unref (query);

  if (!query_ret) {
    // If we didn't get a proper position we could be anywhere in the stream
    // and should assume it's MAX int in order to keep re-buffering going
    pos = GST_CLOCK_TIME_NONE;
  }
  GST_TRACE ("Position query result: %" GST_TIME_FORMAT, GST_TIME_ARGS ((GstClockTime) pos));
  return pos;
}

// Sets the duration field of our object according to the M3U8 parser output
// This function is called only from the first playlist handler. So only in the source thread.
//
// MT-safe
static void
skippy_hls_demux_update_duration (SkippyHLSDemux * demux)
{
  GstClockTime duration;

  // Only post duration message if non-live
  if (skippy_m3u8_client_is_live (demux->client)) {
    return;
  }
  duration = skippy_m3u8_client_get_total_duration (demux->client);

  GST_DEBUG_OBJECT (demux, "Playlist duration: %" GST_TIME_FORMAT, GST_TIME_ARGS (duration));

  // Post message
  if (duration == GST_CLOCK_TIME_NONE) {
    GST_ERROR ("Duration has invalid value, not posting message to pipeline");
    return;
  }
  gst_element_post_message (GST_ELEMENT (demux), gst_message_new_duration_changed (GST_OBJECT (demux)));
}

// This is called by the URL source (sinkpad) event handler on EOS to handle the initial playlist data
//
// MT-safe
static void
skippy_hls_demux_handle_first_playlist (SkippyHLSDemux* demux)
{
  gchar* uri = NULL;
  guint64 timestamp = (guint64) gst_util_get_timestamp ();

  // Query the playlist URI
  uri = skippy_hls_demux_query_location (demux);
  if (!uri) {
    GST_ELEMENT_ERROR (demux, RESOURCE, NOT_FOUND, ("Failed querying the playlist URI"), (NULL));
    goto error;
  }
  GST_INFO_OBJECT (demux, "M3U8 location: %s", uri);

  // Parse main playlist - lock the object for this
  GST_OBJECT_LOCK (demux);
  if (demux->playlist == NULL || !skippy_m3u8_client_load_playlist (demux->client, uri, demux->playlist)) {
    GST_OBJECT_UNLOCK (demux);
    GST_ELEMENT_ERROR (demux, STREAM, DECODE, ("Invalid M3U8 playlist (buffer=%p)", demux->playlist), (NULL));
    goto error;
  }
  GST_OBJECT_UNLOCK (demux);

  // Sending stats message about first playlist fetch
  skippy_hls_demux_post_stat_msg (demux, STAT_TIME_OF_FIRST_PLAYLIST, timestamp, 0);

  // Updates duration field and posts message to bus
  skippy_hls_demux_update_duration (demux);

  GST_DEBUG_OBJECT (demux, "Finished setting up playlist");

  // Make sure URI downloaders are ready asap
  skippy_uri_downloader_prepare (demux->downloader, uri);
  skippy_uri_downloader_prepare (demux->playlist_downloader, uri);

  skippy_hls_demux_link_pads (demux);

error:
  g_free (uri);
  return;
}

// Called when we want to link the downloader to the queue pads
//
// MT-safe
static void
skippy_hls_demux_link_pads (SkippyHLSDemux * demux)
{
  GstPad *queue_srcpad, *downloader_srcpad, *srcpad;
  GstPadTemplate* templ;

  GST_DEBUG ("Linking pads...");

  // Link downloader -> queue
  downloader_srcpad = gst_element_get_static_pad (GST_ELEMENT(demux->downloader), "src");
  if (!downloader_srcpad) {
    GST_WARNING ("No src pad on downloader found yet");
    return;
  }

  // Link the internal source with our queue sink
  gst_pad_link (downloader_srcpad, demux->queue_sinkpad);
  gst_object_unref (downloader_srcpad);
  GST_TRACE ("Linked downloader to queue");
  // Link queue src with external src pad (ghost pad)
  templ = gst_static_pad_template_get (&srctemplate);
  queue_srcpad = gst_element_get_static_pad (demux->queue, "src");
  // Set our srcpad reference (NOTE: gst_ghost_pad_new_from_template locks the element eventually don't call inside locked block)
  srcpad = gst_ghost_pad_new_from_template ("src_0", queue_srcpad, templ);
  GST_OBJECT_LOCK (demux);
  demux->srcpad = srcpad;
  GST_OBJECT_UNLOCK (demux);
  // Cleanup
  gst_object_unref (templ);
  gst_object_unref (queue_srcpad);
  // Configure external source pad
  gst_pad_set_active (demux->srcpad, TRUE);
  // Set event & query handlers for downstream pads
  gst_pad_set_event_function (demux->srcpad,
      GST_DEBUG_FUNCPTR (skippy_hls_demux_src_event));
  gst_pad_set_query_function (demux->srcpad,
      GST_DEBUG_FUNCPTR (skippy_hls_demux_src_query));
  // Add pad to element
  gst_element_add_pad (GST_ELEMENT (demux), demux->srcpad);
  gst_element_no_more_pads (GST_ELEMENT (demux));
  GST_DEBUG ("Added src pad");
}

/* Handling data from the sink pad
*  @param pad - our sink pad
*  @param parent - our element
*  @param buf - the buffer we are handling
*  @return - TRUE if the event was handled
*
*  Calling thread: source
*
*  This function will not handle data if the streaming thread is running
*  MT-safe
*/
static GstFlowReturn
skippy_hls_demux_sink_data (GstPad * pad, GstObject * parent, GstBuffer * buf)
{
  SkippyHLSDemux *demux = SKIPPY_HLS_DEMUX (parent);

  GST_OBJECT_LOCK (demux);
  if (demux->playlist == NULL) {
    demux->playlist = buf;
  } else {
    demux->playlist = gst_buffer_append (demux->playlist, buf);
  }
  GST_OBJECT_UNLOCK (demux);

  return GST_FLOW_OK;
}

/* Handling events from the sink pad
*  @param pad - our sink pad
*  @param parent - our element
*  @param event - the event we are handling
*  @return - TRUE if the event was handled
*
*  Calling thread: source
*
*  This function will not handle events if the streaming thread is running
*  MT-safe
*/
static gboolean
skippy_hls_demux_sink_event (GstPad * pad, GstObject * parent, GstEvent * event)
{
  SkippyHLSDemux *demux;

  demux = SKIPPY_HLS_DEMUX (parent);

  GST_DEBUG_OBJECT (pad, "Got %" GST_PTR_FORMAT, event);

  switch (event->type) {
    case GST_EVENT_EOS:
      GST_DEBUG_OBJECT (demux, "Got EOS on the sink pad: main playlist fetched");
      // Stream loop should not be running when this is called
      skippy_hls_demux_handle_first_playlist (demux);
      gst_event_unref (event); // we don't want to forward the EOS
      return TRUE;
    case GST_EVENT_SEGMENT:
      // Swallow new segments, we'll push our own
      gst_event_unref (event);
      return TRUE;
    default:
      break;
  }

  return gst_pad_event_default (pad, parent, event);
}

// Called upon source pad events i.e seeking
//
// MT-safe
static gboolean
skippy_hls_demux_src_event (GstPad * pad, GstObject * parent, GstEvent * event)
{
  SkippyHLSDemux *demux = SKIPPY_HLS_DEMUX (parent);

  GST_DEBUG_OBJECT (pad, "Got %" GST_PTR_FORMAT, event);

  switch (event->type) {
    case GST_EVENT_SEEK:
      return skippy_hls_demux_handle_seek (demux, event);
    default:
      break;
  }

  return gst_pad_event_default (pad, parent, event);
}

// Handles seek events: Pauses the streaming thread, seeks the M3U8 parser to correct position,
// modifies segment data in downloader element, set seeked flag, sends flush events onto output queue,
// then restarts streaming thread.
//
// MT-safe
static gboolean
skippy_hls_demux_handle_seek (SkippyHLSDemux *demux, GstEvent * event)
{
  gdouble rate;
  GstFormat format;
  GstSeekFlags flags;
  GstSeekType start_type, stop_type;
  gint64 start, stop;
  GstSegment segment;

  GST_INFO ("Received GST_EVENT_SEEK");

  // Not seeking on a live stream
  if (skippy_m3u8_client_is_live (demux->client)) {
    GST_WARNING_OBJECT (demux, "Received seek event for live stream");
    gst_event_unref (event);
    return FALSE;
  }

  // Parse seek event
  gst_event_parse_seek (event, &rate, &format, &flags, &start_type, &start, &stop_type, &stop);
  if (format != GST_FORMAT_TIME) {
    GST_WARNING ("Received seek event not in time format");
    gst_event_unref (event);
    return FALSE;
  }

  GST_DEBUG_OBJECT (demux, "Seek event, rate: %f start: %" GST_TIME_FORMAT " stop: %" GST_TIME_FORMAT,
    rate, GST_TIME_ARGS (start), GST_TIME_ARGS (stop));

  // Pausing streaming task (blocking)
  skippy_hls_demux_pause (demux);
  // At this point we can be sure the stream loop is paused

  // Seek on M3U8 data model
  skippy_m3u8_client_seek_to (demux->client, (GstClockTime) start);

  // Update downloader segment after seek
  segment = skippy_uri_downloader_get_segment (demux->downloader);
  gst_segment_do_seek (&segment, rate, format, flags, start_type, start, stop_type, stop, NULL);
  skippy_uri_downloader_set_segment (demux->downloader, segment);

  // Flush start
  if (flags & GST_SEEK_FLAG_FLUSH) {
    GST_DEBUG_OBJECT (demux, "Sending flush start");
    gst_pad_send_event (demux->queue_sinkpad, gst_event_new_flush_start ());
  }

  // Flush stop
  if (flags & GST_SEEK_FLAG_FLUSH) {
    GST_DEBUG_OBJECT (demux, "Sending flush stop");
    gst_pad_send_event (demux->queue_sinkpad, gst_event_new_flush_stop (TRUE));
  }

  // Restart the streaming task
  GST_DEBUG ("Restarting streaming task");
  gst_task_start (demux->stream_task);

  // Handle and swallow event
  gst_event_unref (event);
  return TRUE;
}

// Handles duration, URI and seeking queries: only access MT-safe M3U8 client to do this
//
// MT-safe
static gboolean
skippy_hls_demux_src_query (GstPad * pad, GstObject * parent, GstQuery * query)
{
  SkippyHLSDemux *demux = SKIPPY_HLS_DEMUX (parent);
  gboolean ret = FALSE;
  GstFormat fmt;
  gint64 stop = -1;
  GstClockTime duration;
  gchar* uri;

  GST_DEBUG ("Got %" GST_PTR_FORMAT, query);

  if (query == NULL)
    return FALSE;
  switch (query->type) {
    case GST_QUERY_DURATION:
      gst_query_parse_duration (query, &fmt, NULL);
      if (fmt != GST_FORMAT_TIME) {
        GST_WARNING ("Can't process duration query that is not in time format");
        break;
      }
      duration = skippy_m3u8_client_get_total_duration (demux->client);
      if (GST_CLOCK_TIME_IS_VALID (duration) && duration > 0) {
        gst_query_set_duration (query, GST_FORMAT_TIME, duration);
        GST_TRACE_OBJECT (demux, "Duration query: %" GST_TIME_FORMAT, GST_TIME_ARGS (duration));
        ret = TRUE;
      } else {
        GST_WARNING ("Bad duration: %" GST_TIME_FORMAT, GST_TIME_ARGS (duration));
      }
      break;
    case GST_QUERY_URI:
      uri = skippy_m3u8_client_get_uri (demux->client);
      gst_query_set_uri (query, uri);
      g_free (uri);
      ret = TRUE;
      break;
    case GST_QUERY_SEEKING:
      gst_query_parse_seeking (query, &fmt, NULL, NULL, NULL);
      if (fmt != GST_FORMAT_TIME) {
        GST_DEBUG ("Can't process seeking query that is not in time format");
        break;
      }
      duration = skippy_m3u8_client_get_total_duration (demux->client);
      if (GST_CLOCK_TIME_IS_VALID (duration) && duration > 0) {
        stop = duration;
        gst_query_set_seeking (query, fmt, !skippy_m3u8_client_is_live (demux->client), 0, stop);
        GST_INFO_OBJECT (demux, "Seeking query stop time: %" GST_TIME_FORMAT, GST_TIME_ARGS (stop));
        ret = TRUE;
      } else {
        GST_DEBUG ("Can't process seeking query that is not in time format");
      }
      break;
    default:
      /* Don't fordward queries upstream because of the special nature of this
       * "demuxer", which relies on the upstream element only to be fed with the
       * first playlist */
      break;
  }
  return ret;
}

// Handles end of playlist: Sets streaming thread to paused state and pushes EOS event
//
// MT-safe
static void
skippy_hls_handle_end_of_playlist (SkippyHLSDemux * demux)
{
  GST_DEBUG_OBJECT (demux, "Reached end of playlist, sending EOS");
  // Schedule task for pause
  GST_OBJECT_LOCK (demux);
  demux->position = 0;
  GST_OBJECT_UNLOCK (demux);
  gst_task_pause (demux->stream_task);
  // Send EOS event
  gst_pad_send_event (demux->queue_sinkpad, gst_event_new_eos ());
}

// Simply wraps caching allowed flag of M3U8 manifest to eventually add custom policy
//
// MT-safe
static gboolean
skippy_hls_demux_is_caching_allowed (SkippyHLSDemux * demux)
{
  gboolean ret;
  ret = skippy_m3u8_client_is_caching_allowed (demux->client);
  return ret;
}

// Checks in our parent object for properties to know what kind of max buffer size we
// should apply
//
// MT-safe
static GstClockTime
skippy_hls_demux_get_max_buffer_duration (SkippyHLSDemux * demux)
{
  GstObject *parent = gst_element_get_parent(demux);
  GObjectClass *klass = G_OBJECT_GET_CLASS(G_OBJECT(parent));
  GstClockTime res = DEFAULT_BUFFER_DURATION;

  if (parent) {
    // Check for conventional UriDecodeBin or DecodeBin properties in our parent object
    if (g_object_class_find_property (klass, "buffer-duration")) {
      g_object_get (parent, "buffer-duration", &res, NULL);
    } else if (g_object_class_find_property (klass, "max-size-time")) {
      g_object_get (parent, "max-size-time", &res, NULL);
    }
    gst_object_unref (parent);
  }

  // Create and activate new source pad (linked as a ghost pad to our queue source pad)
  templ = gst_static_pad_template_get (&srctemplate);
  queue_srcpad = gst_element_get_static_pad (demux->queue, "src");
  demux->srcpad = gst_ghost_pad_new_from_template ("src", queue_srcpad, templ);
  gst_object_unref (templ);
  gst_object_unref (queue_srcpad);

  GST_DEBUG ("Max buffer duration: %" GST_TIME_FORMAT, GST_TIME_ARGS (res));

  return res;
}

// Refreshes playlist - only called from streaming thread
//
// MT-safe
static gboolean
skippy_hls_demux_refresh_playlist (SkippyHLSDemux * demux)
{
  SkippyFragment *download;
  GstBuffer *buf = NULL;
  GError* err = NULL;
  SkippyUriDownloaderFetchReturn fetch_ret;
  gboolean ret = FALSE;
  gchar *current_playlist = skippy_m3u8_client_get_current_playlist (demux->client);
  gchar *main_playlist_uri = skippy_m3u8_client_get_uri (demux->client);

  if (!current_playlist) {
    return FALSE;
  }

  // Create a download
  download = skippy_fragment_new (current_playlist);
  download->start_time = 0;
  download->stop_time = skippy_m3u8_client_get_total_duration (demux->client);

  // Download it
  fetch_ret = skippy_uri_downloader_fetch_fragment (demux->playlist_downloader,
    download, // Media fragment to load
    main_playlist_uri, // Referrer
    TRUE, // Compress (good for playlists)
    TRUE, // Refresh (wipe out cached stuff)
    skippy_hls_demux_is_caching_allowed (demux), // Allow caching directive
    &err // Error
  );

  // Handle fetch result
  switch (fetch_ret) {
  case SKIPPY_URI_DOWNLOADER_COMPLETED:
    skippy_hls_demux_post_stat_msg (demux, STAT_TIME_TO_PLAYLIST, download->download_stop_time - download->download_start_time, 0);
    // Load M3U8 buffer into parser
    buf = skippy_uri_downloader_get_buffer (demux->playlist_downloader);
    if (!skippy_m3u8_client_load_playlist (demux->client, current_playlist, buf)) {
      GST_ELEMENT_ERROR (demux, STREAM, DECODE, ("Invalid playlist"), (NULL));
      ret = FALSE;
      break;
    }
    ret = TRUE;
    break;
  case SKIPPY_URI_DOWNLOADER_FAILED:
  case SKIPPY_URI_DOWNLOADER_CANCELLED:
  case SKIPPY_URI_DOWNLOADER_VOID:
    if (err) {
      GST_ERROR ("Error updating playlist: %s", err->message);
      g_clear_error (&err);
    }
    ret = FALSE;
    break;
  }

  if (buf) {
    gst_buffer_unref (buf);
  }

  g_free (main_playlist_uri);
  g_free (current_playlist);
  return ret;
}

static GstClockTime
skippy_hls_demux_get_time_until_retry (SkippyHLSDemux * demux)
{
  double power;
  double retry_timer = RETRY_TIME_BASE;

  if (demux->download_failed_count >= RETRY_THRESHOLD) {
    //
    // Use this JavaScript function in a console to test the series of ouputs with a 500 ms basetime or to tune the setting here
    // backoff = function(fails, threshold) { if (fails >= threshold) { return 500 * Math.exp (fails/6) / Math.E } else { return 500; } }
    // Below is the same algorithm
    power = ((double) demux->download_failed_count) / ((double) RETRY_THRESHOLD);
    retry_timer = RETRY_TIME_BASE * (exp ( power ) / M_E);
    // Cap the value
    if (retry_timer > RETRY_MAX_TIME_UNTIL) {retry_timer = RETRY_MAX_TIME_UNTIL;}
  }

  return (GstClockTime) retry_timer;
}

// Waits for task condition with timeout - Unlocks the GST object mutex, expects it to be locked
// @param max_wait Max time this should wait in Gstreamer clock time units
static void
skippy_hls_stream_loop_wait_locked (SkippyHLSDemux * demux, GstClockTime max_wait)
{
  // Monotonic time in GLib is in useconds
  gint64 max_wait_ms = ( ( (gint64) max_wait) / GST_USECOND );
  while (!demux->continuing) {
    GST_DEBUG ("Will wait for a max time of %" GST_TIME_FORMAT, GST_TIME_ARGS (max_wait));
    if (!g_cond_wait_until(GST_TASK_GET_COND (demux->stream_task),
      GST_OBJECT_GET_LOCK (demux), g_get_monotonic_time () + max_wait_ms)) {
      GST_TRACE ("Waiting timed out now");
      break;
    } else {
      GST_TRACE ("Wait got interrupted");
    }
  }
  GST_TRACE ("Continuing stream task now");
}

// Checks wether we should download another segment with respect to buffer size.
// Only runs in the streaming thread.
//
// MT-safe
static gboolean
skippy_hls_check_buffer_ahead (SkippyHLSDemux * demux)
{
  GstClockTime pos, max_buffer_duration, max_wait = 0;

  // Check if we are linked yet (did we receive a proper playlist?)
  GST_OBJECT_LOCK (demux);
  if (!demux->srcpad) {
    GST_OBJECT_UNLOCK (demux);
    GST_TRACE ("No src pad yet");
    // Just sleep a bit before trying again
    g_usleep (100);
    return FALSE;
  }

  // Check if wait condition is enabled - if not we can just continue
  if (demux->continuing) {
    GST_OBJECT_UNLOCK (demux);
    // Continue downloading
    return TRUE;
  }
  GST_OBJECT_UNLOCK (demux);

  // Check upfront position relative to stream position
  // If we branch here this means we might want to wait
  pos = skippy_hls_demux_query_position (demux);
  max_buffer_duration = skippy_hls_demux_get_max_buffer_duration (demux);

  GST_TRACE ("Position is %" GST_TIME_FORMAT " , Max buffer duration is %" GST_TIME_FORMAT,
    GST_TIME_ARGS (pos), GST_TIME_ARGS(max_buffer_duration));

  // Check for wether we should limit downloading
  if (pos != GST_CLOCK_TIME_NONE && max_buffer_duration != GST_CLOCK_TIME_NONE && pos >= 2*RETRY_TIME_BASE
    && demux->position > pos + max_buffer_duration) {
    // Diff' between current playhead and buffer-head in microseconds
    max_wait = demux->position - pos - max_buffer_duration;
    GST_TRACE ("Waiting in task as we have preloaded enough (until %" GST_TIME_FORMAT " of media position)",
      GST_TIME_ARGS (demux->position));
    // Timed-cond wait here
    GST_OBJECT_LOCK (demux);
    skippy_hls_stream_loop_wait_locked (demux, max_wait);
    GST_OBJECT_UNLOCK (demux);
    return FALSE;
  }

  // No waiting needed
  return TRUE;
}

// Streaming task function - implements all the HLS logic.
// When this runs the streaming task mutex is/must be locked.
//
// MT-safe
static void
skippy_hls_demux_stream_loop (SkippyHLSDemux * demux)
{
  SkippyFragment *fragment = NULL;
  SkippyUriDownloaderFetchReturn fetch_ret = SKIPPY_URI_DOWNLOADER_VOID;
  GError *err = NULL;
  guint queue_level;
  gchar* referrer_uri = NULL;
  gboolean playlist_refresh = FALSE;
  GstClockTime time_until_retry;

  GST_TRACE_OBJECT (demux, "Entering stream task");

  // Monitor queue levels
  g_object_get (demux->queue, "current-level-buffers", &queue_level, NULL);
  GST_TRACE ("Current internal queue level: %d buffers", (int) queue_level);

  // Check current playback position against buffer levels
  // Blocks and schedules timed-cond until next download
  // Might be interrupted by a seek event and continue
  if (!skippy_hls_check_buffer_ahead (demux)) {
    return;
  }
  GST_DEBUG ("Will try to fetch next fragment ...");

  // Get next fragment from M3U8 list
  referrer_uri = skippy_m3u8_client_get_uri (demux->client);
  fragment = skippy_m3u8_client_get_current_fragment (demux->client);
  if (fragment) {
    GST_INFO_OBJECT (demux, "Pushing data for next fragment: %s (Byte-Range=%" G_GINT64_FORMAT " - %" G_GINT64_FORMAT ")",
      fragment->uri, fragment->range_start, fragment->range_end);
    // Tell downloader to push data
    fetch_ret = skippy_uri_downloader_fetch_fragment (demux->downloader,
      fragment, // Media fragment to load
      referrer_uri, // Referrer
      FALSE, // Compress (useless with coded media data)
      FALSE, // Refresh disabled (don't wipe out cache)
      skippy_hls_demux_is_caching_allowed (demux), // Allow caching directive
      &err
    );
  } else {
    GST_INFO_OBJECT (demux, "This playlist doesn't contain more fragments");
  }

  GST_DEBUG ("Returning finished fragment");

  // Handle result from current attempt
  switch (fetch_ret) {
  // This case means the download did not do anything
  case SKIPPY_URI_DOWNLOADER_VOID:
    // Error & fragment should be NULL
    skippy_hls_handle_end_of_playlist (demux);
    break;
  case SKIPPY_URI_DOWNLOADER_CANCELLED:
    GST_DEBUG ("Fragment fetch got cancelled on purpose");
    break;
  case SKIPPY_URI_DOWNLOADER_FAILED:
    // When failed
    GST_INFO ("Fragment fetch error: %s", err->message);
    // Actual download failure
    GST_OBJECT_LOCK (demux);
    demux->download_failed_count++;
    GST_DEBUG ("Failed to fetch fragment for %d times.", demux->download_failed_count);
    GST_OBJECT_UNLOCK (demux);
    // We only want to refetch the playlist if we get a 403 or a 404
    if (g_error_matches (err, GST_RESOURCE_ERROR, GST_RESOURCE_ERROR_NOT_AUTHORIZED)) {
      GST_DEBUG_OBJECT (demux, "Updating the playlist because of 403 or 404");
      skippy_hls_demux_refresh_playlist (demux);
      playlist_refresh = TRUE;
    }
    break;
  case SKIPPY_URI_DOWNLOADER_COMPLETED:
    GST_DEBUG ("Fragment download completed");
    // Post stats message
    skippy_hls_demux_post_stat_msg (demux, STAT_TIME_TO_DOWNLOAD_FRAGMENT,
      fragment->download_stop_time - fragment->download_start_time, fragment->size);
    // Reset failure counter, position and scheduling condition
    GST_OBJECT_LOCK (demux);
    demux->position = fragment->start_time;
    demux->download_failed_count = 0;
    demux->continuing = FALSE;
    GST_OBJECT_UNLOCK (demux);
    // Go to next fragment
    skippy_m3u8_client_advance_to_next_fragment (demux->client);
    break;
  }

  GST_DEBUG_OBJECT (demux, "Exiting task now ...");

  // Handle error
  if (err && !playlist_refresh) {
    GST_OBJECT_LOCK (demux);
    time_until_retry = skippy_hls_demux_get_time_until_retry (demux);
    GST_DEBUG ("Next retry scheduled in: %" GST_TIME_FORMAT, GST_TIME_ARGS (time_until_retry));
    // Waits before retrying - might be interrupted by a PAUSED -> PLAYING transition or by a seek event
    skippy_hls_stream_loop_wait_locked (demux, time_until_retry);
    // If there was an error we should not schedule but retry right away
    demux->continuing = TRUE;
    GST_OBJECT_UNLOCK (demux);
  }

  // Unref current fragment
  if (fragment) {
    g_object_unref (fragment);
  }
  g_free (referrer_uri);
  g_clear_error (&err);
}

G_GNUC_INTERNAL
void skippy_hlsdemux_setup (void)
{
  gst_element_register (NULL, "skippyhlsdemux", GST_RANK_PRIMARY + 100,
      TYPE_SKIPPY_HLS_DEMUX);
}

