/*
 * Copyright (c) 2018-2020, NVIDIA CORPORATION. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <string.h>
#include <stdio.h>

#include "gstnvdsmeta.h"
#include "deepstream_common.h"
#include "deepstream_sources.h"
#include "deepstream_dewarper.h"
#include <gst/rtp/gstrtcpbuffer.h>
#include <gst/rtsp/gstrtsptransport.h>

#include "nvdsgstutils.h"
#include "gst-nvdssr.h"

#define SRC_CONFIG_KEY "src_config"
#define SOURCE_RESET_INTERVAL_SEC 60

GST_DEBUG_CATEGORY_EXTERN (NVDS_APP);
GST_DEBUG_CATEGORY_EXTERN (APP_CFG_PARSER_CAT);

static gboolean install_mux_eosmonitor_probe = FALSE;

static gboolean
set_camera_csi_params (NvDsSourceConfig * config, NvDsSrcBin * bin)
{
  g_object_set (G_OBJECT (bin->src_elem), "sensor-id",
      config->camera_csi_sensor_id, NULL);

  GST_CAT_DEBUG (NVDS_APP, "Setting csi camera params successful");

  return TRUE;
}

static gboolean
set_camera_v4l2_params (NvDsSourceConfig * config, NvDsSrcBin * bin)
{
  gchar device[64];

  g_snprintf (device, sizeof (device), "/dev/video%d",
      config->camera_v4l2_dev_node);
  g_object_set (G_OBJECT (bin->src_elem), "device", device, NULL);

  GST_CAT_DEBUG (NVDS_APP, "Setting v4l2 camera params successful");

  return TRUE;
}

static gboolean
create_camera_source_bin (NvDsSourceConfig * config, NvDsSrcBin * bin)
{
  GstCaps *caps = NULL, *caps1 = NULL, *convertCaps = NULL;
  gboolean ret = FALSE;

  switch (config->type) {
    case NV_DS_SOURCE_CAMERA_CSI:
      bin->src_elem =
          gst_element_factory_make (NVDS_ELEM_SRC_CAMERA_CSI, "src_elem");
      g_object_set (G_OBJECT (bin->src_elem), "bufapi-version", TRUE, NULL);
      g_object_set (G_OBJECT (bin->src_elem), "maxperf", TRUE, NULL);
      break;
    case NV_DS_SOURCE_CAMERA_V4L2:
      bin->src_elem =
          gst_element_factory_make (NVDS_ELEM_SRC_CAMERA_V4L2, "src_elem");
      bin->cap_filter1 =
          gst_element_factory_make (NVDS_ELEM_CAPS_FILTER, "src_cap_filter1");
      if (!bin->cap_filter1) {
        NVGSTDS_ERR_MSG_V ("Could not create 'src_cap_filter1'");
        goto done;
      }
      caps1 = gst_caps_new_simple ("video/x-raw",
          "width", G_TYPE_INT, config->source_width, "height", G_TYPE_INT,
          config->source_height, "framerate", GST_TYPE_FRACTION,
          config->source_fps_n, config->source_fps_d, NULL);
      break;
    default:
      NVGSTDS_ERR_MSG_V ("Unsupported source type");
      goto done;
  }

  if (!bin->src_elem) {
    NVGSTDS_ERR_MSG_V ("Could not create 'src_elem'");
    goto done;
  }

  bin->cap_filter =
      gst_element_factory_make (NVDS_ELEM_CAPS_FILTER, "src_cap_filter");
  if (!bin->cap_filter) {
    NVGSTDS_ERR_MSG_V ("Could not create 'src_cap_filter'");
    goto done;
  }
  caps = gst_caps_new_simple ("video/x-raw", "format", G_TYPE_STRING, "NV12",
      "width", G_TYPE_INT, config->source_width, "height", G_TYPE_INT,
      config->source_height, "framerate", GST_TYPE_FRACTION,
      config->source_fps_n, config->source_fps_d, NULL);

  if (config->type == NV_DS_SOURCE_CAMERA_CSI) {
    GstCapsFeatures *feature = NULL;
    feature = gst_caps_features_new ("memory:NVMM", NULL);
    gst_caps_set_features (caps, 0, feature);
  }

  if (config->type == NV_DS_SOURCE_CAMERA_V4L2) {
    GstElement *nvvidconv2;
    GstCapsFeatures *feature = NULL;

#ifndef IS_TEGRA
    GstElement *nvvidconv1;
    nvvidconv1 = gst_element_factory_make ("videoconvert", "nvvidconv1");
    if (!nvvidconv1) {
      NVGSTDS_ERR_MSG_V ("Failed to create 'nvvidconv1'");
      goto done;
    }
#endif

    feature = gst_caps_features_new ("memory:NVMM", NULL);
    gst_caps_set_features (caps, 0, feature);
    g_object_set (G_OBJECT (bin->cap_filter), "caps", caps, NULL);

    g_object_set (G_OBJECT (bin->cap_filter1), "caps", caps1, NULL);

    nvvidconv2 = gst_element_factory_make (NVDS_ELEM_VIDEO_CONV, "nvvidconv2");
    if (!nvvidconv2) {
      NVGSTDS_ERR_MSG_V ("Failed to create 'nvvidconv2'");
      goto done;
    }

    g_object_set (G_OBJECT (nvvidconv2), "gpu-id", config->gpu_id,
        "nvbuf-memory-type", config->nvbuf_memory_type, NULL);

#ifndef IS_TEGRA
    gst_bin_add_many (GST_BIN (bin->bin), bin->src_elem, bin->cap_filter1,
        nvvidconv1, nvvidconv2, bin->cap_filter,
        NULL);
#else
    gst_bin_add_many (GST_BIN (bin->bin), bin->src_elem, bin->cap_filter1,
         nvvidconv2, bin->cap_filter, NULL);
#endif

    NVGSTDS_LINK_ELEMENT (bin->src_elem, bin->cap_filter1);

#ifndef IS_TEGRA
    NVGSTDS_LINK_ELEMENT (bin->cap_filter1, nvvidconv1);

    NVGSTDS_LINK_ELEMENT (nvvidconv1, nvvidconv2);
#else
    NVGSTDS_LINK_ELEMENT (bin->cap_filter1, nvvidconv2);
#endif

    NVGSTDS_LINK_ELEMENT (nvvidconv2, bin->cap_filter);

    NVGSTDS_BIN_ADD_GHOST_PAD (bin->bin, bin->cap_filter, "src");

  } else {

    g_object_set (G_OBJECT (bin->cap_filter), "caps", caps, NULL);

    gst_bin_add_many (GST_BIN (bin->bin), bin->src_elem, bin->cap_filter, NULL);

    NVGSTDS_LINK_ELEMENT (bin->src_elem, bin->cap_filter);

    NVGSTDS_BIN_ADD_GHOST_PAD (bin->bin, bin->cap_filter, "src");
  }

  switch (config->type) {
    case NV_DS_SOURCE_CAMERA_CSI:
      if (!set_camera_csi_params (config, bin)) {
        NVGSTDS_ERR_MSG_V ("Could not set CSI camera properties");
        goto done;
      }
      break;
    case NV_DS_SOURCE_CAMERA_V4L2:
      if (!set_camera_v4l2_params (config, bin)) {
        NVGSTDS_ERR_MSG_V ("Could not set V4L2 camera properties");
        goto done;
      }
      break;
    default:
      NVGSTDS_ERR_MSG_V ("Unsupported source type");
      goto done;
  }

  ret = TRUE;

  GST_CAT_DEBUG (NVDS_APP, "Created camera source bin successfully");

done:
  if (caps)
    gst_caps_unref (caps);

  if (convertCaps)
    gst_caps_unref (convertCaps);

  if (!ret) {
    NVGSTDS_ERR_MSG_V ("%s failed", __func__);
  }
  return ret;
}

static void
cb_newpad (GstElement * decodebin, GstPad * pad, gpointer data)
{
  GstCaps *caps = gst_pad_query_caps (pad, NULL);
  const GstStructure *str = gst_caps_get_structure (caps, 0);
  const gchar *name = gst_structure_get_name (str);

  if (!strncmp (name, "video", 5)) {
    NvDsSrcBin *bin = (NvDsSrcBin *) data;
    GstPad *sinkpad = gst_element_get_static_pad (bin->tee, "sink");
    if (gst_pad_link (pad, sinkpad) != GST_PAD_LINK_OK) {

      NVGSTDS_ERR_MSG_V ("Failed to link decodebin to pipeline");
    } else {
      NvDsSourceConfig *config =
          (NvDsSourceConfig *) g_object_get_data (G_OBJECT (bin->cap_filter),
          SRC_CONFIG_KEY);

      gst_structure_get_int (str, "width", &config->source_width);
      gst_structure_get_int (str, "height", &config->source_height);
      gst_structure_get_fraction (str, "framerate", &config->source_fps_n,
          &config->source_fps_d);

      GST_CAT_DEBUG (NVDS_APP, "Decodebin linked to pipeline");
    }
    gst_object_unref (sinkpad);
  }
  gst_caps_unref (caps);
}

static void
cb_sourcesetup (GstElement * object, GstElement * arg0, gpointer data)
{
  NvDsSrcBin *bin = (NvDsSrcBin *) data;
  if (g_object_class_find_property (G_OBJECT_GET_CLASS (arg0), "latency")) {
    g_print ("cb_sourcesetup set %d latency\n", bin->latency);
    g_object_set (G_OBJECT (arg0), "latency", bin->latency, NULL);
  }
}

/*
 * Function to seek the source stream to start.
 * It is required to play the stream in loop.
 */
static gboolean
seek_decode (gpointer data)
{
  NvDsSrcBin *bin = (NvDsSrcBin *) data;
  gboolean ret = TRUE;

  gst_element_set_state (bin->bin, GST_STATE_PAUSED);

  ret = gst_element_seek (bin->bin, 1.0, GST_FORMAT_TIME,
      (GstSeekFlags) (GST_SEEK_FLAG_KEY_UNIT | GST_SEEK_FLAG_FLUSH),
      GST_SEEK_TYPE_SET, 0, GST_SEEK_TYPE_NONE, GST_CLOCK_TIME_NONE);

  if (!ret)
    GST_WARNING ("Error in seeking pipeline");

  gst_element_set_state (bin->bin, GST_STATE_PLAYING);

  return FALSE;
}

/**
 * Probe function to drop certain events to support custom
 * logic of looping of each source stream.
 */
static GstPadProbeReturn
restart_stream_buf_prob (GstPad * pad, GstPadProbeInfo * info,
    gpointer u_data)
{
  GstEvent *event = GST_EVENT (info->data);
  NvDsSrcBin *bin = (NvDsSrcBin *) u_data;

  if ((info->type & GST_PAD_PROBE_TYPE_BUFFER)) {
    GST_BUFFER_PTS(GST_BUFFER(info->data)) += bin->prev_accumulated_base;
  }
  if ((info->type & GST_PAD_PROBE_TYPE_EVENT_BOTH)) {
    if (GST_EVENT_TYPE (event) == GST_EVENT_EOS) {
      g_timeout_add (1, seek_decode, bin);
    }

    if (GST_EVENT_TYPE (event) == GST_EVENT_SEGMENT) {
      GstSegment *segment;

      gst_event_parse_segment (event, (const GstSegment **) &segment);
      segment->base = bin->accumulated_base;
      bin->prev_accumulated_base = bin->accumulated_base;
      bin->accumulated_base += segment->stop;
    }
    switch (GST_EVENT_TYPE (event)) {
      case GST_EVENT_EOS:
        /* QOS events from downstream sink elements cause decoder to drop
         * frames after looping the file since the timestamps reset to 0.
         * We should drop the QOS events since we have custom logic for
         * looping individual sources. */
      case GST_EVENT_QOS:
      case GST_EVENT_SEGMENT:
      case GST_EVENT_FLUSH_START:
      case GST_EVENT_FLUSH_STOP:
        return GST_PAD_PROBE_DROP;
      default:
        break;
    }
  }
  return GST_PAD_PROBE_OK;
}


static void
decodebin_child_added (GstChildProxy * child_proxy, GObject * object,
    gchar * name, gpointer user_data)
{
  NvDsSrcBin *bin = (NvDsSrcBin *) user_data;
  NvDsSourceConfig *config = bin->config;
  if (g_strrstr (name, "decodebin") == name) {
    g_signal_connect (G_OBJECT (object), "child-added",
        G_CALLBACK (decodebin_child_added), user_data);
  }
  if ((g_strrstr (name, "h264parse") == name) ||
      (g_strrstr (name, "h265parse") == name)) {
      g_object_set (object, "config-interval", -1, NULL);
  }
  if (g_strrstr (name, "fakesink") == name) {
      g_object_set (object, "enable-last-sample", FALSE, NULL);
  }
  if (g_strrstr (name, "nvcuvid") == name) {
    g_object_set (object, "gpu-id", config->gpu_id, NULL);

    g_object_set (G_OBJECT (object), "cuda-memory-type",
        config->cuda_memory_type, NULL);

    g_object_set (object, "source-id", config->camera_id, NULL);
    g_object_set (object, "num-decode-surfaces", config->num_decode_surfaces,
        NULL);
    if (config->Intra_decode)
      g_object_set (object, "Intra-decode", config->Intra_decode, NULL);
  }
  if (g_strstr_len (name, -1, "omx") == name) {
    if (config->Intra_decode)
      g_object_set (object, "skip-frames", 2, NULL);
    g_object_set (object, "disable-dvfs", TRUE, NULL);
  }
  if (g_strstr_len (name, -1, "nvjpegdec") == name) {
    g_object_set (object, "DeepStream", TRUE, NULL);
  }
  if (g_strstr_len (name, -1, "nvv4l2decoder") == name) {
    if (config->Intra_decode)
      g_object_set (object, "skip-frames", 2, NULL);
#ifdef __aarch64__
    g_object_set (object, "enable-max-performance", TRUE, NULL);
#else
    g_object_set (object, "gpu-id", config->gpu_id, NULL);
    g_object_set (G_OBJECT (object), "cudadec-memtype",
        config->cuda_memory_type, NULL);
#endif
    g_object_set (object, "drop-frame-interval", config->drop_frame_interval, NULL);
    g_object_set (object, "num-extra-surfaces", config->num_extra_surfaces,
        NULL);

    /* Seek only if file is the source. */
    if (config->loop && g_strstr_len(config->uri, -1, "file:/") == config->uri) {
      NVGSTDS_ELEM_ADD_PROBE (bin->src_buffer_probe, GST_ELEMENT(object),
          "sink", restart_stream_buf_prob,
          (GstPadProbeType) (GST_PAD_PROBE_TYPE_EVENT_BOTH |
              GST_PAD_PROBE_TYPE_EVENT_FLUSH | GST_PAD_PROBE_TYPE_BUFFER),
          bin);
    }
  }
done:
  return;
}

static void
cb_newpad2 (GstElement * decodebin, GstPad * pad, gpointer data)
{
  GstCaps *caps = gst_pad_query_caps (pad, NULL);
  const GstStructure *str = gst_caps_get_structure (caps, 0);
  const gchar *name = gst_structure_get_name (str);

  if (!strncmp (name, "video", 5)) {
    NvDsSrcBin *bin = (NvDsSrcBin *) data;
    GstPad *sinkpad = gst_element_get_static_pad (bin->cap_filter, "sink");
    if (gst_pad_link (pad, sinkpad) != GST_PAD_LINK_OK) {

      NVGSTDS_ERR_MSG_V ("Failed to link decodebin to pipeline");
    } else {
      NvDsSourceConfig *config =
          (NvDsSourceConfig *) g_object_get_data (G_OBJECT (bin->cap_filter),
          SRC_CONFIG_KEY);

      gst_structure_get_int (str, "width", &config->source_width);
      gst_structure_get_int (str, "height", &config->source_height);
      gst_structure_get_fraction (str, "framerate", &config->source_fps_n,
          &config->source_fps_d);

      GST_CAT_DEBUG (NVDS_APP, "Decodebin linked to pipeline");
    }
    gst_object_unref (sinkpad);
  }
  gst_caps_unref (caps);
}


static void
cb_newpad3 (GstElement * decodebin, GstPad * pad, gpointer data)
{
  GstCaps *caps = gst_pad_query_caps (pad, NULL);
  const GstStructure *str = gst_caps_get_structure (caps, 0);
  const gchar *name = gst_structure_get_name (str);

  if (g_strrstr (name, "x-rtp")) {
    NvDsSrcBin *bin = (NvDsSrcBin *) data;
    GstPad *sinkpad = gst_element_get_static_pad (bin->depay, "sink");
    if (gst_pad_link (pad, sinkpad) != GST_PAD_LINK_OK) {

      NVGSTDS_ERR_MSG_V ("Failed to link depay loader to rtsp src");
    }
    gst_object_unref (sinkpad);
  }
  gst_caps_unref (caps);
}

/* Returning FALSE from this callback will make rtspsrc ignore the stream.
 * Ignore audio and add the proper depay element based on codec. */
static gboolean
cb_rtspsrc_select_stream (GstElement *rtspsrc, guint num, GstCaps *caps,
        gpointer user_data)
{
  GstStructure *str = gst_caps_get_structure (caps, 0);
  const gchar *media = gst_structure_get_string (str, "media");
  const gchar *encoding_name = gst_structure_get_string (str, "encoding-name");
  gchar elem_name[50];
  NvDsSrcBin *bin = (NvDsSrcBin *) user_data;
  gboolean ret = FALSE;

  gboolean is_video = (!g_strcmp0 (media, "video"));

  if (!is_video)
    return FALSE;

  /* Create and add depay element only if it is not created yet. */
  if (!bin->depay) {
    g_snprintf (elem_name, sizeof (elem_name), "depay_elem%d", bin->bin_id);

    /* Add the proper depay element based on codec. */
    if (!g_strcmp0 (encoding_name, "H264")) {
      bin->depay = gst_element_factory_make ("rtph264depay", elem_name);
      g_snprintf (elem_name, sizeof (elem_name), "h264parse_elem%d", bin->bin_id);
      bin->parser = gst_element_factory_make ("h264parse", elem_name);
    } else if (!g_strcmp0 (encoding_name, "H265")) {
      bin->depay = gst_element_factory_make ("rtph265depay", elem_name);
      g_snprintf (elem_name, sizeof (elem_name), "h265parse_elem%d", bin->bin_id);
      bin->parser = gst_element_factory_make ("h265parse", elem_name);
    } else {
      NVGSTDS_WARN_MSG_V ("%s not supported", encoding_name);
      return FALSE;
    }

    if (!bin->depay) {
      NVGSTDS_ERR_MSG_V ("Failed to create '%s'", elem_name);
      return FALSE;
    }

    gst_bin_add_many (GST_BIN (bin->bin), bin->depay, bin->parser, NULL);

    NVGSTDS_LINK_ELEMENT (bin->depay, bin->parser);
    NVGSTDS_LINK_ELEMENT (bin->parser, bin->tee_rtsp_pre_decode);

    if (!gst_element_sync_state_with_parent (bin->depay)) {
      NVGSTDS_ERR_MSG_V ("'%s' failed to sync state with parent", elem_name);
      return FALSE;
    }
    gst_element_sync_state_with_parent (bin->parser);
  }

  ret = TRUE;
done:
  return ret;
}

void destroy_smart_record_bin (gpointer data)
{
  unsigned int i = 0;
  NvDsSrcBin *src_bin;
  NvDsSrcParentBin *pbin = (NvDsSrcParentBin *) data;

  g_return_if_fail (pbin);

  for (i = 0; i < pbin->num_bins; i++) {

    src_bin = &pbin->sub_bins[i];
    if (src_bin && src_bin->recordCtx)
      NvDsSRDestroy ((NvDsSRContext *) src_bin->recordCtx);
  }
}

static gpointer
smart_record_callback (NvDsSRRecordingInfo *info, gpointer userData)
{
  static GMutex mutex;
  FILE *logfile = NULL;
  g_return_val_if_fail (info, NULL);

  g_mutex_lock (&mutex);
  logfile = fopen ("smart_record.log", "a");
  if (logfile) {
    fprintf (logfile, "%d:%d:%d:%ldms:%s:%s\n",
      info->sessionId, info->width, info->height, info->duration,
      info->dirpath, info->filename);
    fclose (logfile);
  } else {
    g_print ("Error in opeing smart record log file\n");
  }
  g_mutex_unlock (&mutex);

  return NULL;
}

/**
 * Function called at regular interval to start and stop video recording.
 * This is dummy implementation to show the usages of smart record APIs.
 * startTime and Duration can be adjusted as per usecase.
*/
static gboolean
smart_record_event_generator (gpointer data)
{
  NvDsSRSessionId sessId = 0;
  NvDsSrcBin *src_bin = (NvDsSrcBin *) data;
  guint startTime = 7;
  guint duration = 8;

  if (src_bin->config->smart_rec_duration >= 0)
    duration = src_bin->config->smart_rec_duration;

  if (src_bin->config->smart_rec_start_time >= 0)
    startTime = src_bin->config->smart_rec_start_time;

  if (src_bin->recordCtx && !src_bin->reconfiguring) {
    NvDsSRContext *ctx = (NvDsSRContext *) src_bin->recordCtx;
    if (ctx->recordOn) {
      NvDsSRStop (ctx, 0);
    } else {
      NvDsSRStart (ctx, &sessId, startTime, duration, NULL);
    }
  }
  return TRUE;
}

/**
 * Function called at regular interval to check if NV_DS_SOURCE_RTSP type
 * source in the pipeline is down / disconnected. This function try to
 * reconnect the source by resetting that source pipeline.
 */
static gboolean
watch_source_status (gpointer data)
{
  NvDsSrcBin *src_bin = (NvDsSrcBin *) data;
  struct timeval current_time;
  gettimeofday (&current_time, NULL);
  static struct timeval last_reset_time_global = {0, 0};
  gdouble time_diff_msec_since_last_reset =
      1000.0 * (current_time.tv_sec - last_reset_time_global.tv_sec) +
      (current_time.tv_usec - last_reset_time_global.tv_usec) / 1000.0;

  if (src_bin->reconfiguring) {
    guint time_since_last_reconnect_sec =
        current_time.tv_sec - src_bin->last_reconnect_time.tv_sec;
    if (time_since_last_reconnect_sec >= SOURCE_RESET_INTERVAL_SEC) {
      if (time_diff_msec_since_last_reset > 3000) {
        last_reset_time_global = current_time;
        // source is still not up, reconfigure it again.
        reset_source_pipeline (src_bin);
      }
    }
  } else {
    gint time_since_last_buf_sec = 0;
    g_mutex_lock (&src_bin->bin_lock);
    if (src_bin->last_buffer_time.tv_sec != 0) {
      time_since_last_buf_sec =
          current_time.tv_sec - src_bin->last_buffer_time.tv_sec;
    }
    g_mutex_unlock (&src_bin->bin_lock);

    // Reset source bin if no buffers are received in the last
    // `rtsp_reconnect_interval_sec` seconds.
    if (src_bin->rtsp_reconnect_interval_sec > 0 &&
            time_since_last_buf_sec >= src_bin->rtsp_reconnect_interval_sec) {
      if (time_diff_msec_since_last_reset > 3000) {
        last_reset_time_global = current_time;

        NVGSTDS_WARN_MSG_V ("No data from source %d since last %u sec. Trying reconnection",
              src_bin->bin_id, time_since_last_buf_sec);
        reset_source_pipeline (src_bin);
      }
    }

  }
  return TRUE;
}

/**
 * Function called at regular interval when source bin is
 * changing state async. This function watches the state of
 * the source bin and sets it to PLAYING if the state of source
 * bin stops at PAUSED when changing state ASYNC.
 */
static gboolean
watch_source_async_state_change (gpointer data)
{
  NvDsSrcBin *src_bin = (NvDsSrcBin *) data;
  GstState state, pending;
  GstStateChangeReturn ret;

  ret = gst_element_get_state (src_bin->bin, &state, &pending, 0);

  GST_CAT_DEBUG (NVDS_APP, "Bin %d %p: state:%s pending:%s ret:%s",
          src_bin->bin_id, src_bin, gst_element_state_get_name(state),
          gst_element_state_get_name(pending),
          gst_element_state_change_return_get_name(ret));

  // Bin is still changing state ASYNC. Wait for some more time.
  if (ret == GST_STATE_CHANGE_ASYNC)
    return TRUE;

  // Bin state change failed / failed to get state
  if (ret == GST_STATE_CHANGE_FAILURE) {
    src_bin->async_state_watch_running = FALSE;
    return FALSE;
  }

  // Bin successfully changed state to PLAYING. Stop watching state
  if (state == GST_STATE_PLAYING) {
    src_bin->reconfiguring = FALSE;
    src_bin->async_state_watch_running = FALSE;
    return FALSE;
  }

  // Bin has stopped ASYNC state change but has not gone into
  // PLAYING. Expliclity set state to PLAYING and keep watching
  // state
  gst_element_set_state (src_bin->bin, GST_STATE_PLAYING);

  return TRUE;
}

/**
 * Probe function to monitor data output from rtspsrc.
 */
static GstPadProbeReturn
rtspsrc_monitor_probe_func (GstPad * pad, GstPadProbeInfo * info,
    gpointer u_data)
{
  NvDsSrcBin *bin = (NvDsSrcBin *) u_data;
  if (info->type & GST_PAD_PROBE_TYPE_BUFFER) {
    g_mutex_lock(&bin->bin_lock);
    gettimeofday (&bin->last_buffer_time, NULL);
    g_mutex_unlock(&bin->bin_lock);
  }
  return GST_PAD_PROBE_OK;
}

/**
 * Probe function to drop EOS events from nvstreammux when RTSP sources
 * are being used so that app does not quit from EOS in case of RTSP
 * connection errors and tries to reconnect.
 */
static GstPadProbeReturn
nvstreammux_eosmonitor_probe_func (GstPad * pad, GstPadProbeInfo * info,
    gpointer u_data)
{
  if (info->type & GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM) {
    GstEvent *event = (GstEvent *) info->data;
    if (GST_EVENT_TYPE (event) == GST_EVENT_EOS)
      return GST_PAD_PROBE_DROP;
  }
  return GST_PAD_PROBE_OK;
}

static gboolean
create_rtsp_src_bin (NvDsSourceConfig * config, NvDsSrcBin * bin)
{
  NvDsSRContext *ctx = NULL;
  gboolean ret = FALSE;
  gchar elem_name[50];
  bin->config = config;
  GstCaps *caps = NULL;
  GstCapsFeatures *feature = NULL;

  bin->latency = config->latency;
  bin->rtsp_reconnect_interval_sec = config->rtsp_reconnect_interval_sec;

  g_snprintf (elem_name, sizeof (elem_name), "src_elem%d", bin->bin_id);
  bin->src_elem = gst_element_factory_make ("rtspsrc", elem_name);
  if (!bin->src_elem) {
    NVGSTDS_ERR_MSG_V ("Failed to create '%s'", elem_name);
    goto done;
  }

  g_signal_connect (G_OBJECT(bin->src_elem), "select-stream",
                    G_CALLBACK(cb_rtspsrc_select_stream),
                    bin);

  g_object_set (G_OBJECT (bin->src_elem), "location", config->uri, NULL);
  g_object_set (G_OBJECT (bin->src_elem), "latency", config->latency, NULL);
  g_object_set (G_OBJECT (bin->src_elem), "drop-on-latency", TRUE, NULL);
  configure_source_for_ntp_sync (bin->src_elem);

  // 0x4 for TCP and 0x7 for All (UDP/UDP-MCAST/TCP)
  if ((config->select_rtp_protocol == GST_RTSP_LOWER_TRANS_TCP)
      || (config->select_rtp_protocol == (GST_RTSP_LOWER_TRANS_UDP |
              GST_RTSP_LOWER_TRANS_UDP_MCAST | GST_RTSP_LOWER_TRANS_TCP))) {
    g_object_set (G_OBJECT (bin->src_elem), "protocols",
        config->select_rtp_protocol, NULL);
    GST_DEBUG_OBJECT (bin->src_elem,
        "RTP Protocol=0x%x (0x4=TCP and 0x7=UDP,TCP,UDPMCAST)----\n",
        config->select_rtp_protocol);
  }
  g_signal_connect (G_OBJECT (bin->src_elem), "pad-added",
      G_CALLBACK (cb_newpad3), bin);

  g_snprintf (elem_name, sizeof (elem_name), "tee_rtsp_elem%d", bin->bin_id);
  bin->tee_rtsp_pre_decode = gst_element_factory_make ("tee", elem_name);
  if (!bin->tee_rtsp_pre_decode) {
    NVGSTDS_ERR_MSG_V ("Failed to create '%s'", elem_name);
    goto done;
  }

  if (config->smart_record) {
    NvDsSRInitParams params = {0};
    params.containerType = (NvDsSRContainerType) config->smart_rec_container;
    if (config->file_prefix)
      params.fileNamePrefix = g_strdup_printf ("%s_%d", config->file_prefix, config->camera_id);
    params.dirpath = config->dir_path;
    params.videoCacheSize = config->video_cache_size;
    params.defaultDuration = config->smart_rec_def_duration;
    params.callback = smart_record_callback;
    if (NvDsSRCreate (&ctx, &params) != NVDSSR_STATUS_OK) {
      NVGSTDS_ERR_MSG_V ("Failed to create smart record bin");
      g_free (params.fileNamePrefix);
      goto done;
    }
    g_free (params.fileNamePrefix);
    gst_bin_add (GST_BIN(bin->bin), ctx->recordbin);
    bin->recordCtx = (gpointer) ctx;
  }

  g_snprintf (elem_name, sizeof (elem_name), "dec_que%d", bin->bin_id);
  bin->dec_que = gst_element_factory_make ("queue", elem_name);
  if (!bin->dec_que) {
    NVGSTDS_ERR_MSG_V ("Failed to create '%s'", elem_name);
    goto done;
  }

  if (bin->rtsp_reconnect_interval_sec > 0) {
    NVGSTDS_ELEM_ADD_PROBE (bin->rtspsrc_monitor_probe, bin->dec_que,
        "sink", rtspsrc_monitor_probe_func,
        GST_PAD_PROBE_TYPE_BUFFER,
        bin);
    install_mux_eosmonitor_probe = TRUE;
  }

  g_snprintf (elem_name, sizeof (elem_name), "decodebin_elem%d", bin->bin_id);
  bin->decodebin = gst_element_factory_make ("decodebin", elem_name);
  if (!bin->decodebin) {
    NVGSTDS_ERR_MSG_V ("Failed to create '%s'", elem_name);
    goto done;
  }

  g_signal_connect (G_OBJECT (bin->decodebin), "pad-added",
      G_CALLBACK (cb_newpad2), bin);
  g_signal_connect (G_OBJECT (bin->decodebin), "child-added",
      G_CALLBACK (decodebin_child_added), bin);


  g_snprintf (elem_name, sizeof (elem_name), "src_que%d", bin->bin_id);
  bin->cap_filter = gst_element_factory_make (NVDS_ELEM_QUEUE, elem_name);
  if (!bin->cap_filter) {
    NVGSTDS_ERR_MSG_V ("Failed to create '%s'", elem_name);
    goto done;
  }

  g_mutex_init (&bin->bin_lock);
  if (config->dewarper_config.enable) {
    if (!create_dewarper_bin (&config->dewarper_config, &bin->dewarper_bin)) {
      g_print ("Failed to create dewarper bin \n");
      goto done;
    }
    gst_bin_add_many (GST_BIN (bin->bin), bin->src_elem,
        bin->tee_rtsp_pre_decode,
        bin->dec_que,
        bin->decodebin, bin->dewarper_bin.bin, bin->cap_filter, NULL);
  }else{
    g_snprintf(elem_name, sizeof(elem_name), "nvvidconv_elem%d", bin->bin_id);
    bin->nvvidconv = gst_element_factory_make(NVDS_ELEM_VIDEO_CONV, elem_name);
    if (!bin->nvvidconv)
    {
      NVGSTDS_ERR_MSG_V("Could not create element 'nvvidconv_elem'");
      goto done;
    }
    caps = gst_caps_new_empty_simple("video/x-raw");
    feature = gst_caps_features_new("memory:NVMM", NULL);
    gst_caps_set_features(caps, 0, feature);

    bin->cap_filter1 =
        gst_element_factory_make(NVDS_ELEM_CAPS_FILTER, "src_cap_filter_nvvidconv");
    if (!bin->cap_filter1)
    {
      NVGSTDS_ERR_MSG_V("Could not create 'queue'");
      goto done;
    }

    g_object_set(G_OBJECT(bin->cap_filter1), "caps", caps, NULL);
    gst_caps_unref(caps);
    gst_bin_add_many(GST_BIN(bin->bin), bin->src_elem, bin->tee_rtsp_pre_decode,
                     bin->dec_que, bin->decodebin, bin->cap_filter, bin->nvvidconv, bin->cap_filter1, NULL);
  }

  link_element_to_tee_src_pad(bin->tee_rtsp_pre_decode, bin->dec_que);
  NVGSTDS_LINK_ELEMENT (bin->dec_que, bin->decodebin);

  if (ctx)
    link_element_to_tee_src_pad(bin->tee_rtsp_pre_decode, ctx->recordbin);

  if (config->dewarper_config.enable) {
    NVGSTDS_LINK_ELEMENT (bin->cap_filter, bin->dewarper_bin.bin);
    NVGSTDS_BIN_ADD_GHOST_PAD (bin->bin, bin->dewarper_bin.bin, "src");
  } else{
    NVGSTDS_LINK_ELEMENT (bin->cap_filter, bin->nvvidconv);
    NVGSTDS_LINK_ELEMENT (bin->nvvidconv, bin->cap_filter1);
    NVGSTDS_BIN_ADD_GHOST_PAD (bin->bin, bin->cap_filter1, "src");
  }

  ret = TRUE;

  g_timeout_add (1000, watch_source_status, bin);

  // Enable local start / stop events in addition to the one
  // received from the server.
  if (config->smart_record == 2) {
    if (bin->config->smart_rec_interval)
      g_timeout_add (bin->config->smart_rec_interval * 1000, smart_record_event_generator, bin);
    else
      g_timeout_add (10000, smart_record_event_generator, bin);
  }

  GST_CAT_DEBUG (NVDS_APP,
      "Decode bin created. Waiting for a new pad from decodebin to link");

done:

  if (!ret) {
    NVGSTDS_ERR_MSG_V ("%s failed", __func__);
  }
  return ret;
}

static gboolean
create_uridecode_src_bin (NvDsSourceConfig * config, NvDsSrcBin * bin)
{
  gboolean ret = FALSE;
  GstCaps *caps = NULL;
  GstCapsFeatures *feature = NULL;
  bin->config = config;

  bin->src_elem = gst_element_factory_make (NVDS_ELEM_SRC_URI, "src_elem");
  if (!bin->src_elem) {
    NVGSTDS_ERR_MSG_V ("Could not create element 'src_elem'");
    goto done;
  }

  if (config->dewarper_config.enable) {
    if (!create_dewarper_bin (&config->dewarper_config, &bin->dewarper_bin)) {
      g_print ("Creating Dewarper bin failed \n");
      goto done;
    }
  }
  bin->latency = config->latency;

  if (g_strrstr (config->uri, "file:/")) {
    config->live_source = FALSE;
  }
  if (g_strrstr (config->uri, "rtsp://") == config->uri) {
    configure_source_for_ntp_sync (bin->src_elem);
  }

  g_object_set (G_OBJECT (bin->src_elem), "uri", config->uri, NULL);
  g_signal_connect (G_OBJECT (bin->src_elem), "pad-added",
      G_CALLBACK (cb_newpad), bin);
  g_signal_connect (G_OBJECT (bin->src_elem), "child-added",
      G_CALLBACK (decodebin_child_added), bin);
  g_signal_connect (G_OBJECT (bin->src_elem), "source-setup",
      G_CALLBACK (cb_sourcesetup), bin);
  bin->cap_filter = gst_element_factory_make (NVDS_ELEM_QUEUE, "queue");
  if (!bin->cap_filter) {
    NVGSTDS_ERR_MSG_V ("Could not create 'queue'");
    goto done;
  }

  bin->nvvidconv = gst_element_factory_make (NVDS_ELEM_VIDEO_CONV, "nvvidconv_elem");
  if (!bin->nvvidconv) {
    NVGSTDS_ERR_MSG_V ("Could not create element 'nvvidconv_elem'");
    goto done;
  }

  caps = gst_caps_new_empty_simple ("video/x-raw");
  feature = gst_caps_features_new ("memory:NVMM", NULL);
  gst_caps_set_features (caps, 0, feature);

  bin->cap_filter1 =
    gst_element_factory_make (NVDS_ELEM_CAPS_FILTER, "src_cap_filter_nvvidconv");
  if (!bin->cap_filter1)
  {
    NVGSTDS_ERR_MSG_V ("Could not create 'queue'");
    goto done;
  }

  g_object_set(G_OBJECT(bin->cap_filter1), "caps", caps, NULL);
  gst_caps_unref(caps);

  g_object_set_data (G_OBJECT (bin->cap_filter), SRC_CONFIG_KEY, config);

  gst_bin_add_many (GST_BIN (bin->bin), bin->src_elem, bin->cap_filter,
                    bin->nvvidconv, bin->cap_filter1, NULL);

  NVGSTDS_BIN_ADD_GHOST_PAD (bin->bin, bin->cap_filter1, "src");

  bin->fakesink = gst_element_factory_make ("fakesink", "src_fakesink");
  if (!bin->fakesink) {
    NVGSTDS_ERR_MSG_V ("Could not create 'src_fakesink'");
    goto done;
  }

  bin->fakesink_queue = gst_element_factory_make ("queue", "fakequeue");
  if (!bin->fakesink_queue) {
    NVGSTDS_ERR_MSG_V ("Could not create 'fakequeue'");
    goto done;
  }

  bin->tee = gst_element_factory_make ("tee", NULL);
  if (!bin->tee) {
    NVGSTDS_ERR_MSG_V ("Could not create 'tee'");
    goto done;
  }
  gst_bin_add_many (GST_BIN (bin->bin), bin->fakesink, bin->tee,
      bin->fakesink_queue, NULL);


  NVGSTDS_LINK_ELEMENT (bin->fakesink_queue, bin->fakesink);

  if (config->dewarper_config.enable) {
    gst_bin_add_many (GST_BIN (bin->bin), bin->dewarper_bin.bin, NULL);
    NVGSTDS_LINK_ELEMENT (bin->tee, bin->dewarper_bin.bin);
    NVGSTDS_LINK_ELEMENT (bin->dewarper_bin.bin, bin->cap_filter);
  } else {
    link_element_to_tee_src_pad (bin->tee, bin->cap_filter);
  }
  NVGSTDS_LINK_ELEMENT (bin->cap_filter, bin->nvvidconv);
  NVGSTDS_LINK_ELEMENT (bin->nvvidconv, bin->cap_filter1);
  link_element_to_tee_src_pad (bin->tee, bin->fakesink_queue);

  g_object_set (G_OBJECT (bin->fakesink), "sync", FALSE, "async", FALSE, NULL);
  g_object_set (G_OBJECT (bin->fakesink), "enable-last-sample", FALSE, NULL);

  ret = TRUE;

  GST_CAT_DEBUG (NVDS_APP,
      "Decode bin created. Waiting for a new pad from decodebin to link");

done:

  if (!ret) {
    NVGSTDS_ERR_MSG_V ("%s failed", __func__);
  }
  return ret;
}

gboolean
create_source_bin (NvDsSourceConfig * config, NvDsSrcBin * bin)
{
  static guint bin_cnt = 0;
  gchar bin_name[64];
  g_snprintf(bin_name, 64, "src_bin_%d", bin_cnt++);
  bin->bin = gst_bin_new (bin_name);
  if (!bin->bin) {
    NVGSTDS_ERR_MSG_V ("Failed to create 'src_bin'");
    return FALSE;
  }

  switch (config->type) {
    case NV_DS_SOURCE_CAMERA_V4L2:
      if (!create_camera_source_bin (config, bin)) {
        return FALSE;
      }
      break;
    case NV_DS_SOURCE_URI:
      if (!create_uridecode_src_bin (config, bin)) {
        return FALSE;
      }
      bin->live_source = config->live_source;
      break;
    case NV_DS_SOURCE_RTSP:
      if (!create_rtsp_src_bin (config, bin)) {
        return FALSE;
      }
      break;
    default:
      NVGSTDS_ERR_MSG_V ("Source type not yet implemented!\n");
      return FALSE;
  }

  GST_CAT_DEBUG (NVDS_APP, "Source bin created");

  return TRUE;
}

gboolean
create_multi_source_bin (guint num_sub_bins, NvDsSourceConfig * configs,
    NvDsSrcParentBin * bin)
{
  gboolean ret = FALSE;
  guint i = 0;

  bin->reset_thread = NULL;

  bin->bin = gst_bin_new ("multi_src_bin");
  if (!bin->bin) {
    NVGSTDS_ERR_MSG_V ("Failed to create element 'multi_src_bin'");
    goto done;
  }

  g_object_set (bin->bin, "message-forward", TRUE, NULL);

  bin->streammux =
      gst_element_factory_make (NVDS_ELEM_STREAM_MUX, "src_bin_muxer");
  if (!bin->streammux) {
    NVGSTDS_ERR_MSG_V ("Failed to create element 'src_bin_muxer'");
    goto done;
  }
  gst_bin_add (GST_BIN (bin->bin), bin->streammux);

  for (i = 0; i < num_sub_bins; i++) {
    if (!configs[i].enable) {
      continue;
    }

    gchar elem_name[50];
    g_snprintf (elem_name, sizeof (elem_name), "src_sub_bin%d", i);
    bin->sub_bins[i].bin = gst_bin_new (elem_name);
    if (!bin->sub_bins[i].bin) {
      NVGSTDS_ERR_MSG_V ("Failed to create '%s'", elem_name);
      goto done;
    }

    bin->sub_bins[i].bin_id = bin->sub_bins[i].source_id = i;
    configs[i].live_source = TRUE;
    bin->live_source = TRUE;
    bin->sub_bins[i].eos_done = TRUE;
    bin->sub_bins[i].reset_done = TRUE;

    switch (configs[i].type) {
      case NV_DS_SOURCE_CAMERA_CSI:
      case NV_DS_SOURCE_CAMERA_V4L2:
        if (!create_camera_source_bin (&configs[i], &bin->sub_bins[i])) {
          return FALSE;
        }
        break;
      case NV_DS_SOURCE_URI:
        if (!create_uridecode_src_bin (&configs[i], &bin->sub_bins[i])) {
          return FALSE;
        }
        bin->live_source = configs[i].live_source;
        break;
      case NV_DS_SOURCE_RTSP:
        if (!create_rtsp_src_bin (&configs[i], &bin->sub_bins[i])) {
          return FALSE;
        }
        break;
      default:
        NVGSTDS_ERR_MSG_V ("Source type not yet implemented!\n");
        return FALSE;
    }

    gst_bin_add (GST_BIN (bin->bin), bin->sub_bins[i].bin);

    if (!link_element_to_streammux_sink_pad (bin->streammux,
            bin->sub_bins[i].bin, i)) {
      NVGSTDS_ERR_MSG_V ("source %d cannot be linked to mux's sink pad %p\n", i, bin->streammux);
      goto done;
    }

    if(configs->dewarper_config.enable) {
        g_object_set(G_OBJECT(bin->sub_bins[i].dewarper_bin.nvdewarper), "source-id",
                configs[i].source_id, NULL);
    }

    bin->num_bins++;
  }
  NVGSTDS_BIN_ADD_GHOST_PAD (bin->bin, bin->streammux, "src");

  if (install_mux_eosmonitor_probe) {
    NVGSTDS_ELEM_ADD_PROBE (bin->nvstreammux_eosmonitor_probe, bin->streammux,
        "src", nvstreammux_eosmonitor_probe_func,
        GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
        bin);
  }

  ret = TRUE;

done:
  if (!ret) {
    NVGSTDS_ERR_MSG_V ("%s failed", __func__);
  }
  return ret;
}

gboolean
reset_source_pipeline (gpointer data)
{
  NvDsSrcBin *src_bin = (NvDsSrcBin *) data;
  GstState state, pending;
  GstStateChangeReturn ret;

  g_mutex_lock(&src_bin->bin_lock);
  gettimeofday (&src_bin->last_buffer_time, NULL);
  gettimeofday (&src_bin->last_reconnect_time, NULL);
  g_mutex_unlock(&src_bin->bin_lock);

  if (gst_element_set_state (src_bin->bin,
          GST_STATE_NULL) == GST_STATE_CHANGE_FAILURE) {
    GST_ERROR_OBJECT (src_bin->bin, "Can't set source bin to NULL");
    return FALSE;
  }
  NVGSTDS_INFO_MSG_V ("Resetting source %d", src_bin->bin_id);

  GST_CAT_INFO (NVDS_APP, "Reset source pipeline %s %p\n,", __func__, src_bin);
  if (!gst_element_sync_state_with_parent (src_bin->bin)) {
    GST_ERROR_OBJECT (src_bin->bin, "Couldn't sync state with parent");
  }

  ret = gst_element_get_state (src_bin->bin, &state, &pending, 0);

  GST_CAT_DEBUG (NVDS_APP, "Bin %d %p: state:%s pending:%s ret:%s",
          src_bin->bin_id, src_bin, gst_element_state_get_name(state),
          gst_element_state_get_name(pending),
          gst_element_state_change_return_get_name(ret));

  if (ret == GST_STATE_CHANGE_ASYNC || ret == GST_STATE_CHANGE_NO_PREROLL) {
    if (!src_bin->async_state_watch_running)
      g_timeout_add (20, watch_source_async_state_change, src_bin);
    src_bin->async_state_watch_running = TRUE;
    src_bin->reconfiguring = TRUE;
  } else if (ret == GST_STATE_CHANGE_SUCCESS && state == GST_STATE_PLAYING) {
    src_bin->reconfiguring = FALSE;
  }
  return FALSE;
}

gboolean
set_source_to_playing (gpointer data)
{
  NvDsSrcBin *subBin = (NvDsSrcBin *) data;
  if (subBin->reconfiguring) {
    gst_element_set_state (subBin->bin, GST_STATE_PLAYING);
    GST_CAT_INFO (NVDS_APP, "Reconfiguring %s  %p\n,", __func__, subBin);

    subBin->reconfiguring = FALSE;
  }
  return FALSE;
}

gpointer
reset_encodebin (gpointer data)
{
  NvDsSrcBin *src_bin = (NvDsSrcBin *) data;
  g_usleep (10000);
  GST_CAT_INFO (NVDS_APP, "Reset called %s %p\n,", __func__, src_bin);

  GST_CAT_INFO (NVDS_APP, "Reset setting null for sink %s %p\n,", __func__,
      src_bin);
  src_bin->reset_done = TRUE;

  return NULL;
}
