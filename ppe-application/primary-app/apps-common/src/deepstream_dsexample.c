/*
 * Copyright (c) 2018-2019, NVIDIA CORPORATION. All rights reserved.
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

#include "deepstream_common.h"
#include "deepstream_dsexample.h"


// Create bin, add queue and the element, link all elements and ghost pads,
// Set the element properties from the parsed config
gboolean
create_dsexample_bin (NvDsDsExampleConfig *config, NvDsDsExampleBin *bin)
{
  gboolean ret = FALSE;

  bin->bin = gst_bin_new ("dsexample_bin");
  if (!bin->bin) {
    NVGSTDS_ERR_MSG_V ("Failed to create 'dsexample_bin'");
    goto done;
  }

  bin->queue =
      gst_element_factory_make (NVDS_ELEM_QUEUE, "dsexample_queue");
  if (!bin->queue) {
    NVGSTDS_ERR_MSG_V ("Failed to create 'dsexample_queue'");
    goto done;
  }

  bin->elem_dsexample =
      gst_element_factory_make (NVDS_ELEM_DSEXAMPLE_ELEMENT, "dsexample0");
  if (!bin->elem_dsexample) {
    NVGSTDS_ERR_MSG_V ("Failed to create 'dsexample0'");
    goto done;
  }

  bin->pre_conv = gst_element_factory_make (NVDS_ELEM_VIDEO_CONV, "dsexample_conv0");
  if (!bin->pre_conv) {
    NVGSTDS_ERR_MSG_V ("Failed to create 'dsexample_conv0'");
    goto done;
  }

  gst_bin_add_many (GST_BIN (bin->bin), bin->queue,
      bin->pre_conv, bin->elem_dsexample, NULL);

  NVGSTDS_LINK_ELEMENT (bin->queue, bin->pre_conv);
  NVGSTDS_LINK_ELEMENT (bin->pre_conv, bin->elem_dsexample);

  NVGSTDS_BIN_ADD_GHOST_PAD (bin->bin, bin->queue, "sink");

  NVGSTDS_BIN_ADD_GHOST_PAD (bin->bin, bin->elem_dsexample, "src");

  g_object_set (G_OBJECT (bin->elem_dsexample),
      "full-frame", config->full_frame,
      "processing-width", config->processing_width,
      "processing-height", config->processing_height,
      "unique-id", config->unique_id,
      "gpu-id", config->gpu_id, NULL);

 g_object_set (G_OBJECT (bin->pre_conv), "gpu-id", config->gpu_id, NULL);

 g_object_set (G_OBJECT (bin->pre_conv), "nvbuf-memory-type",
     config->nvbuf_memory_type, NULL);

 ret = TRUE;

done:
  if (!ret) {
    NVGSTDS_ERR_MSG_V ("%s failed", __func__);
  }

  return ret;
}
