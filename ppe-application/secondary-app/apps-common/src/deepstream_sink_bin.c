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


#include <sys/types.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>

#include "deepstream_common.h"
#include "deepstream_sinks.h"
#include <gst/rtsp-server/rtsp-server.h>

static guint uid = 0;
static GstRTSPServer *server [MAX_SINK_BINS];
static guint server_count = 0;
static GMutex server_cnt_lock;

GST_DEBUG_CATEGORY_EXTERN (NVDS_APP);

/**
 * Function to create sink bin for Display / Fakesink.
 */
static gboolean
create_render_bin (NvDsSinkRenderConfig * config, NvDsSinkBinSubBin * bin)
{
  gboolean ret = FALSE;
  gchar elem_name[50];
  GstElement *connect_to;
  GstCaps *caps = NULL;

  uid++;

  g_snprintf (elem_name, sizeof (elem_name), "sink_sub_bin%d", uid);
  bin->bin = gst_bin_new (elem_name);
  if (!bin->bin) {
    NVGSTDS_ERR_MSG_V ("Failed to create '%s'", elem_name);
    goto done;
  }

  g_snprintf (elem_name, sizeof (elem_name), "sink_sub_bin_sink%d", uid);
  switch (config->type) {
    case NV_DS_SINK_RENDER_EGL:
      GST_CAT_INFO (NVDS_APP, "NVvideo renderer\n");
      bin->sink = gst_element_factory_make (NVDS_ELEM_SINK_EGL, elem_name);
      g_object_set (G_OBJECT (bin->sink), "window-x", config->offset_x,
          "window-y", config->offset_y, "window-width", config->width,
          "window-height", config->height, NULL);
      g_object_set (G_OBJECT (bin->sink), "enable-last-sample", FALSE, NULL);
      break;
    case NV_DS_SINK_RENDER_OVERLAY:
#ifndef IS_TEGRA
      NVGSTDS_ERR_MSG_V ("Overlay is only supported for Jetson");
      return FALSE;
#endif
      GST_CAT_INFO (NVDS_APP, "NVvideo renderer\n");
      bin->sink = gst_element_factory_make (NVDS_ELEM_SINK_OVERLAY, elem_name);
      g_object_set (G_OBJECT (bin->sink), "display-id", config->display_id,
          NULL);
      g_object_set (G_OBJECT (bin->sink), "overlay", config->overlay_id, NULL);
      g_object_set (G_OBJECT (bin->sink), "overlay-x", config->offset_x, NULL);
      g_object_set (G_OBJECT (bin->sink), "overlay-y", config->offset_y, NULL);
      g_object_set (G_OBJECT (bin->sink), "overlay-w", config->width, NULL);
      g_object_set (G_OBJECT (bin->sink), "overlay-h", config->height, NULL);
      break;
    case NV_DS_SINK_FAKE:
      bin->sink = gst_element_factory_make (NVDS_ELEM_SINK_FAKESINK, elem_name);
      g_object_set (G_OBJECT (bin->sink), "enable-last-sample", FALSE, NULL);
      break;
    default:
      return FALSE;
  }

  if (!bin->sink) {
    NVGSTDS_ERR_MSG_V ("Failed to create '%s'", elem_name);
    goto done;
  }

  g_object_set (G_OBJECT (bin->sink), "sync", config->sync, "max-lateness", -1,
      "async", FALSE, "qos", config->qos, NULL);

#ifndef IS_TEGRA
  g_snprintf (elem_name, sizeof (elem_name), "sink_sub_bin_cap_filter%d", uid);
  bin->cap_filter = gst_element_factory_make (NVDS_ELEM_CAPS_FILTER, elem_name);
  if (!bin->cap_filter) {
    NVGSTDS_ERR_MSG_V ("Failed to create '%s'", elem_name);
    goto done;
  }
  gst_bin_add (GST_BIN (bin->bin), bin->cap_filter);
#endif

  g_snprintf (elem_name, sizeof (elem_name), "sink_sub_bin_transform%d", uid);
  if (config->type == NV_DS_SINK_RENDER_EGL) {
#ifdef IS_TEGRA
    bin->transform =
        gst_element_factory_make (NVDS_ELEM_EGLTRANSFORM, elem_name);
#else
    bin->transform = gst_element_factory_make (NVDS_ELEM_VIDEO_CONV, elem_name);
#endif
    if (!bin->transform) {
      NVGSTDS_ERR_MSG_V ("Failed to create '%s'", elem_name);
      goto done;
    }
    gst_bin_add (GST_BIN (bin->bin), bin->transform);

#ifndef IS_TEGRA
    caps = gst_caps_new_empty_simple ("video/x-raw");

    GstCapsFeatures *feature = NULL;
    feature = gst_caps_features_new (MEMORY_FEATURES, NULL);
    gst_caps_set_features (caps, 0, feature);
    g_object_set (G_OBJECT (bin->cap_filter), "caps", caps, NULL);

    g_object_set (G_OBJECT (bin->transform), "gpu-id", config->gpu_id, NULL);
    g_object_set (G_OBJECT (bin->transform), "nvbuf-memory-type",
        config->nvbuf_memory_type, NULL);
#endif
  }

  g_snprintf (elem_name, sizeof (elem_name), "render_queue%d", uid);
  bin->queue = gst_element_factory_make (NVDS_ELEM_QUEUE, elem_name);
  if (!bin->queue) {
    NVGSTDS_ERR_MSG_V ("Failed to create '%s'", elem_name);
    goto done;
  }

  gst_bin_add_many (GST_BIN (bin->bin), bin->queue, bin->sink, NULL);

  connect_to = bin->sink;

  if (bin->cap_filter) {
    NVGSTDS_LINK_ELEMENT (bin->cap_filter, connect_to);
    connect_to = bin->cap_filter;
  }

  if (bin->transform) {
    NVGSTDS_LINK_ELEMENT (bin->transform, connect_to);
    connect_to = bin->transform;
  }

  NVGSTDS_LINK_ELEMENT (bin->queue, connect_to);

  NVGSTDS_BIN_ADD_GHOST_PAD (bin->bin, bin->queue, "sink");

  ret = TRUE;

done:
  if (caps) {
    gst_caps_unref (caps);
  }
  if (!ret) {
    NVGSTDS_ERR_MSG_V ("%s failed", __func__);
  }
  return ret;
}

static void broker_queue_overrun (GstElement* sink_queue, gpointer user_data)
{
    (void)sink_queue;
    (void)user_data;
    NVGSTDS_WARN_MSG_V("nvmsgbroker queue overrun; Older Message Buffer "
                       "Dropped; Network bandwidth might be insufficient\n");
}

/**
 * Function to create sink bin to generate meta-msg, convert to json based on
 * a schema and send over msgbroker.
 */
static gboolean
create_msg_conv_broker_bin (NvDsSinkMsgConvBrokerConfig *config,
    NvDsSinkBinSubBin *bin)
{
  /** Create the subbin: -> q -> msgconv -> msgbroker bin */
  gboolean ret = FALSE;
  gchar elem_name[50];

  uid++;

  g_snprintf (elem_name, sizeof (elem_name), "sink_sub_bin%d", uid);
  bin->bin = gst_bin_new (elem_name);
  if (!bin->bin) {
    NVGSTDS_ERR_MSG_V ("Failed to create '%s'", elem_name);
    goto done;
  }
  g_snprintf (elem_name, sizeof (elem_name), "sink_sub_bin_queue%d", uid);
  bin->queue = gst_element_factory_make (NVDS_ELEM_QUEUE, elem_name);
  if (!bin->queue) {
    NVGSTDS_ERR_MSG_V ("Failed to create '%s'", elem_name);
    goto done;
  }

  /** set threshold on queue to avoid pipeline choke when broker is stuck on network
   * leaky=2 (2): downstream       - Leaky on downstream (old buffers) */
  g_object_set(G_OBJECT(bin->queue), "leaky", 2, NULL);
  g_object_set(G_OBJECT(bin->queue), "max-size-buffers", 20, NULL);
  g_signal_connect (G_OBJECT (bin->queue), "overrun",
        G_CALLBACK (broker_queue_overrun), bin);

  /* Create msg converter to generate payload from buffer metadata */
  g_snprintf (elem_name, sizeof (elem_name), "sink_sub_bin_transform%d", uid);
  if (config->disable_msgconv)
  {
      bin->transform = gst_element_factory_make ("queue", elem_name);
  }
  else
  {
      bin->transform = gst_element_factory_make (NVDS_ELEM_MSG_CONV, elem_name);
  }
  if (!bin->transform) {
    NVGSTDS_ERR_MSG_V ("Failed to create '%s'", elem_name);
    goto done;
  }

  if (!config->disable_msgconv)
    g_object_set (G_OBJECT(bin->transform), "config", config->config_file_path,
                "msg2p-lib", (config->conv_msg2p_lib ? config->conv_msg2p_lib : NULL),
                "payload-type", config->conv_payload_type,
                "comp-id", config->conv_comp_id,
                "debug-payload-dir", config->debug_payload_dir,
                "multiple-payloads", config->multiple_payloads,
                NULL);

  /* Create msg broker to send payload to server */
  g_snprintf (elem_name, sizeof (elem_name), "sink_sub_bin_sink%d", uid);
  bin->sink = gst_element_factory_make (NVDS_ELEM_MSG_BROKER, elem_name);
  if (!bin->sink) {
    NVGSTDS_ERR_MSG_V ("Failed to create '%s'", elem_name);
    goto done;
  }
  g_object_set (G_OBJECT(bin->sink), "proto-lib", config->proto_lib,
                "conn-str", config->conn_str,
                "topic", config->topic,
                "sync", config->sync, "async", FALSE,
                "config", config->broker_config_file_path,
                "comp-id", config->broker_comp_id,
                "new-api", config->new_api,
                NULL);

  gst_bin_add_many (GST_BIN (bin->bin),
      bin->queue, bin->transform, bin->sink,
      NULL);

  NVGSTDS_LINK_ELEMENT (bin->queue, bin->transform);
  NVGSTDS_LINK_ELEMENT (bin->transform, bin->sink);

  NVGSTDS_BIN_ADD_GHOST_PAD (bin->bin, bin->queue, "sink");

  ret = TRUE;

done:
  if (!ret) {
    NVGSTDS_ERR_MSG_V ("%s failed", __func__);
  }
  return ret;
}

/**
 * Function to create sink bin to generate encoded output.
 */
static gboolean
create_encode_file_bin (NvDsSinkEncoderConfig * config, NvDsSinkBinSubBin * bin)
{
  GstCaps *caps = NULL;
  gboolean ret = FALSE;
  gchar elem_name[50];
  gulong bitrate = config->bitrate;
  guint profile = config->profile;

  uid++;

  g_snprintf (elem_name, sizeof (elem_name), "sink_sub_bin%d", uid);
  bin->bin = gst_bin_new (elem_name);
  if (!bin->bin) {
    NVGSTDS_ERR_MSG_V ("Failed to create '%s'", elem_name);
    goto done;
  }

  g_snprintf (elem_name, sizeof (elem_name), "sink_sub_bin_queue%d", uid);
  bin->queue = gst_element_factory_make (NVDS_ELEM_QUEUE, elem_name);
  if (!bin->queue) {
    NVGSTDS_ERR_MSG_V ("Failed to create '%s'", elem_name);
    goto done;
  }

  g_snprintf (elem_name, sizeof (elem_name), "sink_sub_bin_transform%d", uid);
  bin->transform = gst_element_factory_make (NVDS_ELEM_VIDEO_CONV, elem_name);
  if (!bin->transform) {
    NVGSTDS_ERR_MSG_V ("Failed to create '%s'", elem_name);
    goto done;
  }

  g_snprintf (elem_name, sizeof (elem_name), "sink_sub_bin_cap_filter%d", uid);
  bin->cap_filter = gst_element_factory_make (NVDS_ELEM_CAPS_FILTER, elem_name);
  if (!bin->cap_filter) {
    NVGSTDS_ERR_MSG_V ("Failed to create '%s'", elem_name);
    goto done;
  }
  if (config->codec == NV_DS_ENCODER_MPEG4 || config->enc_type == NV_DS_ENCODER_TYPE_SW)
    caps = gst_caps_from_string ("video/x-raw, format=I420");
  else
    caps = gst_caps_from_string ("video/x-raw(memory:NVMM), format=I420");
  g_object_set (G_OBJECT (bin->cap_filter), "caps", caps, NULL);

  g_snprintf (elem_name, sizeof (elem_name), "sink_sub_bin_encoder%d", uid);
  switch (config->codec) {
    case NV_DS_ENCODER_H264:
      if (config->enc_type == NV_DS_ENCODER_TYPE_SW)
        bin->encoder = gst_element_factory_make (NVDS_ELEM_ENC_H264_SW, elem_name);
      else
        bin->encoder = gst_element_factory_make (NVDS_ELEM_ENC_H264_HW, elem_name);
      break;
    case NV_DS_ENCODER_H265:
      if (config->enc_type == NV_DS_ENCODER_TYPE_SW)
        bin->encoder = gst_element_factory_make (NVDS_ELEM_ENC_H265_SW, elem_name);
      else
        bin->encoder = gst_element_factory_make (NVDS_ELEM_ENC_H265_HW, elem_name);
      break;
    case NV_DS_ENCODER_MPEG4:
        bin->encoder = gst_element_factory_make (NVDS_ELEM_ENC_MPEG4, elem_name);
      break;
    default:
      goto done;
  }
  if (!bin->encoder) {
    NVGSTDS_ERR_MSG_V ("Failed to create '%s'", elem_name);
    goto done;
  }

  #ifdef IS_TEGRA
    g_object_set (G_OBJECT (bin->encoder), "bufapi-version", 1, NULL);
  #endif

  if (config->codec == NV_DS_ENCODER_MPEG4)
    config->enc_type = NV_DS_ENCODER_TYPE_SW;

  if (config->enc_type == NV_DS_ENCODER_TYPE_HW) {
    g_object_set (G_OBJECT (bin->encoder), "profile", profile, NULL);
    g_object_set (G_OBJECT (bin->encoder), "iframeinterval", config->iframeinterval, NULL);
    g_object_set (G_OBJECT (bin->encoder), "bitrate", bitrate, NULL);
  } else {
      if (config->codec == NV_DS_ENCODER_MPEG4)
        g_object_set (G_OBJECT (bin->encoder), "bitrate", bitrate, NULL);
      else {
        //bitrate is in kbits/sec for software encoder x264enc and x265enc
        g_object_set (G_OBJECT (bin->encoder), "bitrate", bitrate / 1000, NULL);
      }
  }

  switch (config->codec) {
    case NV_DS_ENCODER_H264:
      bin->codecparse = gst_element_factory_make ("h264parse", "h264-parser");
      break;
    case NV_DS_ENCODER_H265:
      bin->codecparse = gst_element_factory_make ("h265parse", "h265-parser");
      break;
    case NV_DS_ENCODER_MPEG4:
      bin->codecparse = gst_element_factory_make ("mpeg4videoparse", "mpeg4-parser");
      break;
    default:
      goto done;
  }

  g_snprintf (elem_name, sizeof (elem_name), "sink_sub_bin_mux%d", uid);
  switch (config->container) {
    case NV_DS_CONTAINER_MP4:
      bin->mux = gst_element_factory_make (NVDS_ELEM_MUX_MP4, elem_name);
      break;
    case NV_DS_CONTAINER_MKV:
      bin->mux = gst_element_factory_make (NVDS_ELEM_MKV, elem_name);
      break;
    default:
      goto done;
  }
  if (!bin->mux) {
    NVGSTDS_ERR_MSG_V ("Failed to create '%s'", elem_name);
    goto done;
  }

  g_snprintf (elem_name, sizeof (elem_name), "sink_sub_bin_sink%d", uid);
  bin->sink = gst_element_factory_make (NVDS_ELEM_SINK_FILE, elem_name);
  if (!bin->sink) {
    NVGSTDS_ERR_MSG_V ("Failed to create '%s'", elem_name);
    goto done;
  }

  g_object_set (G_OBJECT (bin->sink), "location", config->output_file_path,
      "sync", config->sync, "async", FALSE, NULL);
  g_object_set (G_OBJECT (bin->transform), "gpu-id", config->gpu_id, NULL);
  gst_bin_add_many (GST_BIN (bin->bin), bin->queue,
      bin->transform, bin->codecparse, bin->cap_filter,
      bin->encoder, bin->mux, bin->sink, NULL);

  NVGSTDS_LINK_ELEMENT (bin->queue, bin->transform);

  NVGSTDS_LINK_ELEMENT (bin->transform, bin->cap_filter);
  NVGSTDS_LINK_ELEMENT (bin->cap_filter, bin->encoder);

  NVGSTDS_LINK_ELEMENT (bin->encoder, bin->codecparse);
  NVGSTDS_LINK_ELEMENT (bin->codecparse, bin->mux);
  NVGSTDS_LINK_ELEMENT (bin->mux, bin->sink);

  NVGSTDS_BIN_ADD_GHOST_PAD (bin->bin, bin->queue, "sink");

  ret = TRUE;

done:
  if (caps) {
    gst_caps_unref (caps);
  }
  if (!ret) {
    NVGSTDS_ERR_MSG_V ("%s failed", __func__);
  }
  return ret;
}

static gboolean
start_rtsp_streaming (guint rtsp_port_num, guint updsink_port_num,
                      NvDsEncoderType enctype, guint64 udp_buffer_size)
{
  GstRTSPMountPoints *mounts;
  GstRTSPMediaFactory *factory;
  char udpsrc_pipeline[512];

  char port_num_Str[64] = { 0 };
  char *encoder_name;

  if (enctype == NV_DS_ENCODER_H264) {
    encoder_name = "H264";
  } else if (enctype == NV_DS_ENCODER_H265) {
    encoder_name = "H265";
  } else {
    NVGSTDS_ERR_MSG_V ("%s failed", __func__);
    return FALSE;
  }

  if (udp_buffer_size == 0)
    udp_buffer_size = 512 * 1024;

  sprintf (udpsrc_pipeline,
      "( udpsrc name=pay0 port=%d buffer-size=%lu caps=\"application/x-rtp, media=video, "
      "clock-rate=90000, encoding-name=%s, payload=96 \" )",
      updsink_port_num, udp_buffer_size, encoder_name);

  sprintf (port_num_Str, "%d", rtsp_port_num);

  g_mutex_lock (&server_cnt_lock);

  server [server_count] = gst_rtsp_server_new ();
  g_object_set (server [server_count], "service", port_num_Str, NULL);

  mounts = gst_rtsp_server_get_mount_points (server [server_count]);

  factory = gst_rtsp_media_factory_new ();
  gst_rtsp_media_factory_set_launch (factory, udpsrc_pipeline);

  gst_rtsp_mount_points_add_factory (mounts, "/ds-test", factory);

  g_object_unref (mounts);

  gst_rtsp_server_attach (server [server_count], NULL);

  server_count++;

  g_mutex_unlock (&server_cnt_lock);

  g_print
      ("\n *** DeepStream: Launched RTSP Streaming at rtsp://localhost:%d/ds-test ***\n\n",
      rtsp_port_num);

  return TRUE;
}

static gboolean
create_udpsink_bin (NvDsSinkEncoderConfig * config, NvDsSinkBinSubBin * bin)
{
  GstCaps *caps = NULL;
  gboolean ret = FALSE;
  gchar elem_name[50];
  gchar encode_name[50];
  gchar rtppay_name[50];

  //guint rtsp_port_num = g_rtsp_port_num++;
  uid++;

  g_snprintf (elem_name, sizeof (elem_name), "sink_sub_bin%d", uid);
  bin->bin = gst_bin_new (elem_name);
  if (!bin->bin) {
    NVGSTDS_ERR_MSG_V ("Failed to create '%s'", elem_name);
    goto done;
  }

  g_snprintf (elem_name, sizeof (elem_name), "sink_sub_bin_queue%d", uid);
  bin->queue = gst_element_factory_make (NVDS_ELEM_QUEUE, elem_name);
  if (!bin->queue) {
    NVGSTDS_ERR_MSG_V ("Failed to create '%s'", elem_name);
    goto done;
  }

  g_snprintf (elem_name, sizeof (elem_name), "sink_sub_bin_transform%d", uid);
  bin->transform = gst_element_factory_make (NVDS_ELEM_VIDEO_CONV, elem_name);
  if (!bin->transform) {
    NVGSTDS_ERR_MSG_V ("Failed to create '%s'", elem_name);
    goto done;
  }

  g_snprintf (elem_name, sizeof (elem_name), "sink_sub_bin_cap_filter%d", uid);
  bin->cap_filter = gst_element_factory_make (NVDS_ELEM_CAPS_FILTER, elem_name);
  if (!bin->cap_filter) {
    NVGSTDS_ERR_MSG_V ("Failed to create '%s'", elem_name);
    goto done;
  }

  if (config->enc_type == NV_DS_ENCODER_TYPE_SW)
    caps = gst_caps_from_string ("video/x-raw, format=I420");
  else
    caps = gst_caps_from_string ("video/x-raw(memory:NVMM), format=I420");

  g_object_set (G_OBJECT (bin->cap_filter), "caps", caps, NULL);

  g_snprintf (encode_name, sizeof (encode_name), "sink_sub_bin_encoder%d", uid);
  g_snprintf (rtppay_name, sizeof (rtppay_name), "sink_sub_bin_rtppay%d", uid);

  switch (config->codec) {
    case NV_DS_ENCODER_H264:
      bin->codecparse = gst_element_factory_make ("h264parse", "h264-parser");
      bin->rtppay = gst_element_factory_make ("rtph264pay", rtppay_name);
      if (config->enc_type == NV_DS_ENCODER_TYPE_SW)
        bin->encoder = gst_element_factory_make (NVDS_ELEM_ENC_H264_SW, encode_name);
      else
        bin->encoder = gst_element_factory_make (NVDS_ELEM_ENC_H264_HW, encode_name);
      break;
    case NV_DS_ENCODER_H265:
      bin->codecparse = gst_element_factory_make ("h265parse", "h265-parser");
      bin->rtppay = gst_element_factory_make ("rtph265pay", rtppay_name);
      if (config->enc_type == NV_DS_ENCODER_TYPE_SW)
        bin->encoder = gst_element_factory_make (NVDS_ELEM_ENC_H265_SW, encode_name);
      else
        bin->encoder = gst_element_factory_make (NVDS_ELEM_ENC_H265_HW, encode_name);
      break;
    default:
      goto done;
  }

  if (!bin->encoder) {
    NVGSTDS_ERR_MSG_V ("Failed to create '%s'", encode_name);
    goto done;
  }

  if (!bin->rtppay) {
    NVGSTDS_ERR_MSG_V ("Failed to create '%s'", rtppay_name);
    goto done;
  }

  if (config->enc_type  == NV_DS_ENCODER_TYPE_SW) {
    //bitrate is in kbits/sec for software encoder x264enc and x265enc
    g_object_set (G_OBJECT (bin->encoder), "bitrate", config->bitrate/1000, NULL);
  } else {
      g_object_set (G_OBJECT (bin->encoder), "bitrate", config->bitrate, NULL);
      g_object_set (G_OBJECT (bin->encoder), "profile", config->profile, NULL);
      g_object_set (G_OBJECT (bin->encoder), "iframeinterval", config->iframeinterval, NULL);
  }

#ifdef IS_TEGRA
  g_object_set (G_OBJECT (bin->encoder), "preset-level", 1, NULL);
  g_object_set (G_OBJECT (bin->encoder), "insert-sps-pps", 1, NULL);
  g_object_set (G_OBJECT (bin->encoder), "bufapi-version", 1, NULL);
#else
  g_object_set (G_OBJECT (bin->transform), "gpu-id", config->gpu_id, NULL);
#endif

  g_snprintf (elem_name, sizeof (elem_name), "sink_sub_bin_udpsink%d", uid);
  bin->sink = gst_element_factory_make ("udpsink", elem_name);
  if (!bin->sink) {
    NVGSTDS_ERR_MSG_V ("Failed to create '%s'", elem_name);
    goto done;
  }

  g_object_set (G_OBJECT (bin->sink), "host", "224.224.255.255", "port",
      config->udp_port, "async", FALSE, "sync", 0, NULL);

  gst_bin_add_many (GST_BIN (bin->bin),
      bin->queue, bin->cap_filter, bin->transform,
      bin->encoder, bin->codecparse, bin->rtppay, bin->sink, NULL);

  NVGSTDS_LINK_ELEMENT (bin->queue, bin->transform);
  NVGSTDS_LINK_ELEMENT (bin->transform, bin->cap_filter);
  NVGSTDS_LINK_ELEMENT (bin->cap_filter, bin->encoder);
  NVGSTDS_LINK_ELEMENT (bin->encoder, bin->codecparse);
  NVGSTDS_LINK_ELEMENT (bin->codecparse, bin->rtppay);
  NVGSTDS_LINK_ELEMENT (bin->rtppay, bin->sink);

  NVGSTDS_BIN_ADD_GHOST_PAD (bin->bin, bin->queue, "sink");

  ret = TRUE;

  ret = start_rtsp_streaming (config->rtsp_port, config->udp_port, config->codec,
          config->udp_buffer_size);
  if (ret != TRUE) {
    g_print ("%s: start_rtsp_straming function failed\n", __func__);
  }

done:
  if (caps) {
    gst_caps_unref (caps);
  }
  if (!ret) {
    NVGSTDS_ERR_MSG_V ("%s failed", __func__);
  }
  return ret;
}

gboolean
create_sink_bin (guint num_sub_bins, NvDsSinkSubBinConfig * config_array,
    NvDsSinkBin * bin, guint index)
{
  gboolean ret = FALSE;
  guint i;

  bin->bin = gst_bin_new ("sink_bin");
  if (!bin->bin) {
    NVGSTDS_ERR_MSG_V ("Failed to create element 'sink_bin'");
    goto done;
  }

  bin->queue = gst_element_factory_make (NVDS_ELEM_QUEUE, "sink_bin_queue");
  if (!bin->queue) {
    NVGSTDS_ERR_MSG_V ("Failed to create element 'sink_bin_queue'");
    goto done;
  }

  gst_bin_add (GST_BIN (bin->bin), bin->queue);

  NVGSTDS_BIN_ADD_GHOST_PAD (bin->bin, bin->queue, "sink");

  bin->tee = gst_element_factory_make (NVDS_ELEM_TEE, "sink_bin_tee");
  if (!bin->tee) {
    NVGSTDS_ERR_MSG_V ("Failed to create element 'sink_bin_tee'");
    goto done;
  }

  gst_bin_add (GST_BIN (bin->bin), bin->tee);

  NVGSTDS_LINK_ELEMENT (bin->queue, bin->tee);

  g_object_set (G_OBJECT (bin->tee), "allow-not-linked", TRUE, NULL);

  for (i = 0; i < num_sub_bins; i++) {
    if (!config_array[i].enable) {
      continue;
    }
    if (config_array[i].source_id != index) {
      continue;
    }
    if (config_array[i].link_to_demux) {
      continue;
    }
    switch (config_array[i].type) {
      case NV_DS_SINK_RENDER_EGL:
      case NV_DS_SINK_RENDER_OVERLAY:
      case NV_DS_SINK_FAKE:
        config_array[i].render_config.type = config_array[i].type;
        config_array[i].render_config.sync = config_array[i].sync;
        if (!create_render_bin (&config_array[i].render_config,
                &bin->sub_bins[i]))
          goto done;
        break;
      case NV_DS_SINK_ENCODE_FILE:
        config_array[i].encoder_config.sync = config_array[i].sync;
        if (!create_encode_file_bin (&config_array[i].encoder_config,
                &bin->sub_bins[i]))
          goto done;
        break;
      case NV_DS_SINK_UDPSINK:
        if (!create_udpsink_bin (&config_array[i].encoder_config,
                &bin->sub_bins[i]))
          goto done;
        break;
      case NV_DS_SINK_MSG_CONV_BROKER:
        config_array[i].msg_conv_broker_config.sync = config_array[i].sync;
        if (!create_msg_conv_broker_bin (&config_array[i].msg_conv_broker_config,
                &bin->sub_bins[i]))
          goto done;
        break;
      default:
        goto done;
    }

    if(config_array[i].type != NV_DS_SINK_MSG_CONV_BROKER) {
      gst_bin_add (GST_BIN (bin->bin), bin->sub_bins[i].bin);
      if (!link_element_to_tee_src_pad (bin->tee, bin->sub_bins[i].bin)) {
        goto done;
      }
    }
    bin->num_bins++;
  }

  if (bin->num_bins == 0) {
    NvDsSinkRenderConfig config;
    config.type = NV_DS_SINK_FAKE;
    if (!create_render_bin (&config, &bin->sub_bins[0]))
      goto done;
    gst_bin_add (GST_BIN (bin->bin), bin->sub_bins[0].bin);
    if (!link_element_to_tee_src_pad (bin->tee, bin->sub_bins[0].bin)) {
      goto done;
    }
    bin->num_bins = 1;
  }

  ret = TRUE;
done:
  if (!ret) {
    NVGSTDS_ERR_MSG_V ("%s failed", __func__);
  }
  return ret;
}


gboolean
create_demux_sink_bin (guint num_sub_bins, NvDsSinkSubBinConfig * config_array,
    NvDsSinkBin * bin, guint index)
{
  gboolean ret = FALSE;
  guint i;

  bin->bin = gst_bin_new ("sink_bin");
  if (!bin->bin) {
    NVGSTDS_ERR_MSG_V ("Failed to create element 'sink_bin'");
    goto done;
  }

  bin->queue = gst_element_factory_make (NVDS_ELEM_QUEUE, "sink_bin_queue");
  if (!bin->queue) {
    NVGSTDS_ERR_MSG_V ("Failed to create element 'sink_bin_queue'");
    goto done;
  }

  gst_bin_add (GST_BIN (bin->bin), bin->queue);

  NVGSTDS_BIN_ADD_GHOST_PAD (bin->bin, bin->queue, "sink");

  bin->tee = gst_element_factory_make (NVDS_ELEM_TEE, "sink_bin_tee");
  if (!bin->tee) {
    NVGSTDS_ERR_MSG_V ("Failed to create element 'sink_bin_tee'");
    goto done;
  }

  gst_bin_add (GST_BIN (bin->bin), bin->tee);

  NVGSTDS_LINK_ELEMENT (bin->queue, bin->tee);

  for (i = 0; i < num_sub_bins; i++) {
    if (!config_array[i].enable) {
      continue;
    }
    if (!config_array[i].link_to_demux) {
      continue;
    }
    switch (config_array[i].type) {
      case NV_DS_SINK_RENDER_EGL:
      case NV_DS_SINK_RENDER_OVERLAY:
      case NV_DS_SINK_FAKE:
        config_array[i].render_config.type = config_array[i].type;
        config_array[i].render_config.sync = config_array[i].sync;
        if (!create_render_bin (&config_array[i].render_config,
                &bin->sub_bins[i]))
          goto done;
        break;
      case NV_DS_SINK_ENCODE_FILE:
        config_array[i].encoder_config.sync = config_array[i].sync;
        if (!create_encode_file_bin (&config_array[i].encoder_config,
                &bin->sub_bins[i]))
          goto done;
        break;
      case NV_DS_SINK_UDPSINK:
        if (!create_udpsink_bin (&config_array[i].encoder_config,
                &bin->sub_bins[i]))
          goto done;
        break;
      case NV_DS_SINK_MSG_CONV_BROKER:
        config_array[i].msg_conv_broker_config.sync = config_array[i].sync;
        if (!create_msg_conv_broker_bin (&config_array[i].msg_conv_broker_config,
                &bin->sub_bins[i]))
          goto done;
        break;
      default:
        goto done;
    }

    if(config_array[i].type != NV_DS_SINK_MSG_CONV_BROKER) {
      gst_bin_add (GST_BIN (bin->bin), bin->sub_bins[i].bin);
      if (!link_element_to_tee_src_pad (bin->tee, bin->sub_bins[i].bin)) {
        goto done;
      }
    }
    bin->num_bins++;
  }

  if (bin->num_bins == 0) {
    NvDsSinkRenderConfig config;
    config.type = NV_DS_SINK_FAKE;
    if (!create_render_bin (&config, &bin->sub_bins[0]))
      goto done;
    gst_bin_add (GST_BIN (bin->bin), bin->sub_bins[0].bin);
    if (!link_element_to_tee_src_pad (bin->tee, bin->sub_bins[0].bin)) {
      goto done;
    }
    bin->num_bins = 1;
  }

  ret = TRUE;
done:
  if (!ret) {
    NVGSTDS_ERR_MSG_V ("%s failed", __func__);
  }
  return ret;
}

static GstRTSPFilterResult
client_filter (GstRTSPServer * server, GstRTSPClient * client,
    gpointer user_data)
{
  return GST_RTSP_FILTER_REMOVE;
}

void
destroy_sink_bin ()
{
  GstRTSPMountPoints *mounts;
  GstRTSPSessionPool *pool;
  guint i = 0;
  for (i = 0; i < server_count; i++) {
    mounts = gst_rtsp_server_get_mount_points (server [i]);
    gst_rtsp_mount_points_remove_factory (mounts, "/ds-test");
    g_object_unref (mounts);
    gst_rtsp_server_client_filter (server [i], client_filter, NULL);
    pool = gst_rtsp_server_get_session_pool (server [i]);
    gst_rtsp_session_pool_cleanup (pool);
    g_object_unref (pool);
  }
}
