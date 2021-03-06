#include <string>
#include <mutex>
#include <string.h> // for memcpy

#include "skippy_m3u8.h"
#include "skippy_fragment.h"

#include "skippy_m3u8_parser.hpp"
#include "skippyHLS/skippy_hls.h"
#include "skippy_hls_priv.h"


GST_DEBUG_CATEGORY_STATIC (skippy_m3u8_debug);
#define GST_CAT_DEFAULT skippy_m3u8_debug

#define NANOSECONDS_TO_GST_TIME(t) ((GstClockTime)t*GST_NSECOND)

using namespace std;

struct SkippyM3U8ClientPrivate
{
  SkippyM3U8ClientPrivate ()
  :current_index(0)
  ,playlist_raw(NULL)
  ,playlist("")
  {

  }

  ~SkippyM3U8ClientPrivate ()
  {
    g_free (playlist_raw);
  }

  int current_index;
  gchar* playlist_raw;
  SkippyM3UPlaylist playlist;
  recursive_mutex mutex;
};

static gpointer skippy_m3u8_client_init_once (gpointer user_data)
{
  GST_DEBUG_CATEGORY_INIT (skippy_m3u8_debug, "skippyhls-m3u8", 0, "M3U8 client");
  return NULL;
}

SkippyM3U8Client *skippy_m3u8_client_new ()
{
  // Static one-time initialization
  static GOnce init_once = G_ONCE_INIT;
  g_once (&init_once, skippy_m3u8_client_init_once, NULL);

  SkippyM3U8Client* client = g_slice_new0 (SkippyM3U8Client);
  client->priv = new SkippyM3U8ClientPrivate();
  return client;
}

void skippy_m3u8_client_free (SkippyM3U8Client * client)
{
  delete client->priv;
  g_slice_free(SkippyM3U8Client, client);
}

static gchar* buf_to_utf8_playlist (GstBuffer * buf)
{
  GstMapInfo info;
  gchar *playlist;

  if (!gst_buffer_map (buf, &info, GST_MAP_READ)) {
    return NULL;
  }

  // We make the copy before the validation to exclude any
  // possibility of the validator mutating the data in some way
  /* alloc size + 1 to end with a null character */
  playlist = (gchar*) g_malloc0 (info.size + 1);
  memcpy (playlist, info.data, info.size);

  GST_DEBUG ("\n\n\nM3U8 data dump:\n\n%s\n\n", playlist);

  if (!g_utf8_validate ((const gchar *) info.data, info.size, NULL)) {
    GST_ERROR ("M3U8 was not valid UTF-8 data");
    gst_buffer_unmap (buf, &info);
    g_free (playlist);
    return NULL;
  }

  gst_buffer_unmap (buf, &info);
  return playlist;
}

// Update/set/identify variant (sub-) playlist by URIs advertised in master playlist
SkippyHlsInternalError skippy_m3u8_client_load_playlist (SkippyM3U8Client * client, const gchar *uri, GstBuffer* playlist_buffer)
{
  SkippyM3UParser p;
  gchar* playlist = buf_to_utf8_playlist (playlist_buffer);
    
  if (!playlist) {
    return PLAYLIST_INVALID_UTF_CONTENT;
  }
  {
    lock_guard<recursive_mutex> lock(client->priv->mutex);
    string loaded_playlist_uri = (uri != NULL) ? uri : client->priv->playlist.uri;
    SkippyM3UPlaylist loaded_playlist = p.parse(loaded_playlist_uri, playlist);
    
    //update raw playlist
    g_free (client->priv->playlist_raw);
    client->priv->playlist_raw = playlist;
    
    if (!loaded_playlist.isComplete) {
      return PLAYLIST_INCOMPLETE;
    }
    
    client->priv->playlist = loaded_playlist;
    return NO_ERROR;
  }
}

gchar* skippy_m3u8_client_get_current_raw_data (SkippyM3U8Client * client) {
  lock_guard<recursive_mutex> lock(client->priv->mutex);
  return client->priv->playlist_raw;
}

// Called to get the next fragment
SkippyFragment* skippy_m3u8_client_get_fragment (SkippyM3U8Client * client, guint64 sequence_number)
{
  lock_guard<recursive_mutex> lock(client->priv->mutex);

  SkippyFragment *fragment;
  SkippyM3UItem item;

  if (sequence_number >= client->priv->playlist.items.size()) {
    return NULL;
  }

  item = client->priv->playlist.items.at (sequence_number);

  fragment = skippy_fragment_new (item.url.c_str());
  fragment->start_time = NANOSECONDS_TO_GST_TIME (item.start);
  fragment->stop_time = NANOSECONDS_TO_GST_TIME (item.end);
  fragment->duration = NANOSECONDS_TO_GST_TIME (item.duration);
  return fragment;
}

SkippyFragment* skippy_m3u8_client_get_current_fragment (SkippyM3U8Client * client)
{
  lock_guard<recursive_mutex> lock(client->priv->mutex);
  
  SkippyFragment *fragment;
  SkippyM3UItem item;
  
  if (client->priv->current_index >= client->priv->playlist.items.size()) {
    return NULL;
  }
  
  item = client->priv->playlist.items.at (client->priv->current_index);
  
  fragment = skippy_fragment_new (item.url.c_str());
  fragment->start_time = NANOSECONDS_TO_GST_TIME (item.start);
  fragment->stop_time = NANOSECONDS_TO_GST_TIME (item.end);
  fragment->duration = NANOSECONDS_TO_GST_TIME (item.duration);
  return fragment;
}

void skippy_m3u8_client_advance_to_next_fragment (SkippyM3U8Client * client)
{
  lock_guard<recursive_mutex> lock(client->priv->mutex);

  if (client->priv->current_index < client->priv->playlist.items.size()) {
    client->priv->current_index++;
  }
}

gboolean skippy_m3u8_client_seek_to (SkippyM3U8Client * client, GstClockTime target)
{
  lock_guard<recursive_mutex> lock(client->priv->mutex);

  SkippyM3UItem item;
  guint64 target_pos = (guint64) GST_TIME_AS_NSECONDS(target);

  GST_LOG ("Seek to target: %" GST_TIME_FORMAT " ns", GST_TIME_ARGS(GST_NSECOND * target_pos));

  for (int i=0;i<client->priv->playlist.items.size();i++) {
    item = client->priv->playlist.items.at(i);
    if (target_pos >= item.start && target_pos < item.end)
    {
      GST_LOG ("Seeked to index %d, interval %ld - %ld", i, (long) item.start, (long) item.end);
      client->priv->current_index = i;
      return TRUE;
    }
  }
  return FALSE;
}

gchar* skippy_m3u8_client_get_uri(SkippyM3U8Client * client)
{
  lock_guard<recursive_mutex> lock(client->priv->mutex);
  return g_strdup(client->priv->playlist.uri.c_str());
}

gchar* skippy_m3u8_client_get_playlist_for_bitrate (SkippyM3U8Client * client, guint bitrate)
{
  //lock_guard<recursive_mutex> lock(client->priv->mutex);
  return NULL;
}

gchar *skippy_m3u8_client_get_current_playlist (SkippyM3U8Client * client)
{
  lock_guard<recursive_mutex> lock(client->priv->mutex);
  return g_strdup(client->priv->playlist.uri.c_str());
}

void skippy_m3u8_client_set_current_playlist (SkippyM3U8Client * client, const gchar *uri)
{
  //lock_guard<recursive_mutex> lock(client->priv->mutex);
}

GstClockTime skippy_m3u8_client_get_total_duration (SkippyM3U8Client * client)
{
  lock_guard<recursive_mutex> lock(client->priv->mutex);
  return NANOSECONDS_TO_GST_TIME (client->priv->playlist.totalDuration);
}

GstClockTime skippy_m3u8_client_get_target_duration (SkippyM3U8Client * client)
{
  lock_guard<recursive_mutex> lock(client->priv->mutex);
  return NANOSECONDS_TO_GST_TIME (client->priv->playlist.targetDuration);
}

gboolean skippy_m3u8_client_has_variant_playlist(SkippyM3U8Client * client)
{
  //lock_guard<recursive_mutex> lock(client->priv->mutex);
  return FALSE;
}

gboolean skippy_m3u8_client_is_live(SkippyM3U8Client * client)
{
  lock_guard<recursive_mutex> lock(client->priv->mutex);
  if (client->priv->playlist.type == "event") {
    return TRUE;
  }
  // Defaults to "vod"
  return FALSE;
}

gboolean skippy_m3u8_client_is_caching_allowed(SkippyM3U8Client * client)
{
  //lock_guard<recursive_mutex> lock(client->priv->mutex);
  return TRUE;
}
