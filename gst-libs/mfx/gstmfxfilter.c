/*
 *  Copyright (C) 2016 Intel Corporation
 *    Author: Ishmael Visayana Sameen <ishmael.visayana.sameen@intel.com>
 *    Author: Puunithaaraj Gopal <puunithaaraj.gopal@intel.com>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public License
 *  as published by the Free Software Foundation; either version 2.1
 *  of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free
 *  Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 *  Boston, MA 02110-1301 USA
 */

#include "sysdeps.h"

#include "gstmfxfilter.h"
#include "gstmfxtaskaggregator.h"
#include "gstmfxtask.h"
#include "gstmfxallocator.h"
#include "gstmfxsurfacepool.h"
#include "gstmfxsurface.h"

#define DEBUG 1
#include "gstmfxdebug.h"

typedef struct _GstMfxFilterOpData GstMfxFilterOpData;

struct _GstMfxFilterOpData
{
  GstMfxFilterType type;
  gpointer filter;
  gsize size;
};

struct _GstMfxFilter
{
  /*< private > */
  GstObject parent_instance;
  GstMfxTaskAggregator *aggregator;
  GstMfxTask *vpp[2];
  GstMfxSurfacePool *out_pool;
  gboolean inited;

  mfxSession session;
  mfxVideoParam params;
  mfxFrameInfo frame_info;
  mfxFrameAllocResponse response;

  /* VPP output parameters */
  mfxU32 fourcc;
  mfxU16 width;
  mfxU16 height;
  mfxU16 fps_n;
  mfxU16 fps_d;

  /* FilterType */
  guint filter_op;
  GPtrArray *filter_op_data;

  mfxExtBuffer **ext_buffer;
  mfxExtVPPDoUse vpp_use;
};

G_DEFINE_TYPE (GstMfxFilter, gst_mfx_filter, GST_TYPE_OBJECT)

void
gst_mfx_filter_set_frame_info (GstMfxFilter * filter, mfxFrameInfo * info)
{
  g_return_if_fail (filter != NULL);

  filter->frame_info = *info;
}

void
gst_mfx_filter_set_frame_info_from_gst_video_info (GstMfxFilter * filter,
    const GstVideoInfo * info)
{
  g_return_if_fail (filter != NULL);

  filter->frame_info.ChromaFormat = MFX_CHROMAFORMAT_YUV420;
  filter->frame_info.FourCC =
      gst_video_format_to_mfx_fourcc (GST_VIDEO_INFO_FORMAT (info));
  filter->frame_info.PicStruct =
      GST_VIDEO_INFO_IS_INTERLACED (info) ? (GST_VIDEO_INFO_FLAG_IS_SET (info,
          GST_VIDEO_FRAME_FLAG_TFF) ? MFX_PICSTRUCT_FIELD_TFF :
      MFX_PICSTRUCT_FIELD_BFF)
      : MFX_PICSTRUCT_PROGRESSIVE;

  filter->frame_info.CropX = 0;
  filter->frame_info.CropY = 0;
  filter->frame_info.CropW = info->width;
  filter->frame_info.CropH = info->height;
  filter->frame_info.FrameRateExtN = info->fps_n ? info->fps_n : 30;
  filter->frame_info.FrameRateExtD = info->fps_d;
  filter->frame_info.AspectRatioW = info->par_n;
  filter->frame_info.AspectRatioH = info->par_d;
  filter->frame_info.BitDepthChroma = filter->frame_info.BitDepthLuma =
      (MFX_FOURCC_P010 == filter->frame_info.FourCC) ? 10 : 8;

  filter->frame_info.Width = GST_ROUND_UP_32 (info->width);
  filter->frame_info.Height = GST_ROUND_UP_32 (info->height);
}


static GstMfxFilterOpData *
find_filter_op_data (GstMfxFilter * filter, GstMfxFilterType type)
{
  GstMfxFilterOpData *op;
  guint i;

  for (i = 0; i < filter->filter_op_data->len; i++) {
    op = (GstMfxFilterOpData *) g_ptr_array_index (filter->filter_op_data, i);
    if (type == op->type)
      return op;
  }
  return NULL;
}

static void
free_filter_op_data (gpointer data)
{
  GstMfxFilterOpData *op = (GstMfxFilterOpData *) data;
  g_slice_free1 (op->size, op->filter);
  g_slice_free (GstMfxFilterOpData, op);
}

static gboolean
is_filter_supported (GstMfxFilter * filter, mfxU32 alg)
{
  mfxVideoParam param = { 0 };
  mfxExtVPPDoUse vpp_use = { 0 };
  mfxExtBuffer *extbuf[1];
  mfxStatus sts;
  gboolean supported = TRUE;

  vpp_use.NumAlg = 1;
  vpp_use.AlgList = g_slice_alloc (1 * sizeof (mfxU32));
  vpp_use.Header.BufferId = MFX_EXTBUFF_VPP_DOUSE;
  param.NumExtParam = 1;

  extbuf[0] = (mfxExtBuffer *) & vpp_use;
  param.ExtParam = (mfxExtBuffer **) & extbuf[0];

  vpp_use.AlgList[0] = alg;
  sts = MFXVideoVPP_Query (filter->session, NULL, &param);
  if (MFX_ERR_NONE != sts)
    supported = FALSE;

  g_slice_free1 (1 * sizeof (mfxU32), vpp_use.AlgList);
  return supported;
}

static gboolean
configure_filters (GstMfxFilter * filter)
{
  GstMfxFilterOpData *op;
  mfxExtBuffer *ext_buf;
  guint i, len = filter->filter_op_data->len;

  /* If AlgList is available when filter is already initialized
   * and if current number of filters is not equal to new number of
   * filters requested, deallocate resources when resetting */
  if (filter->vpp_use.AlgList && (len != filter->vpp_use.NumAlg)) {
    g_slice_free1 (filter->vpp_use.NumAlg * sizeof (mfxU32),
        filter->vpp_use.AlgList);
    g_slice_free1 (filter->params.NumExtParam * sizeof (mfxExtBuffer *),
        filter->ext_buffer);
    filter->vpp_use.NumAlg = 0;
  }

  if (len && (len != filter->vpp_use.NumAlg)) {
    filter->vpp_use.Header.BufferId = MFX_EXTBUFF_VPP_DOUSE;
    filter->vpp_use.Header.BufferSz = sizeof (mfxExtVPPDoUse);
    filter->vpp_use.NumAlg = len;
    filter->vpp_use.AlgList = g_slice_alloc (len * sizeof (mfxU32));
    if (!filter->vpp_use.AlgList)
      return FALSE;

    filter->ext_buffer = g_slice_alloc ((len + 1) * sizeof (mfxExtBuffer *));
    if (!filter->ext_buffer)
      return FALSE;

    for (i = 0; i < len; i++) {
      op = (GstMfxFilterOpData *) g_ptr_array_index (filter->filter_op_data, i);
      ext_buf = (mfxExtBuffer *) op->filter;
      filter->vpp_use.AlgList[i] = ext_buf->BufferId;
      filter->ext_buffer[i + 1] = (mfxExtBuffer *) op->filter;
    }

    filter->ext_buffer[0] = (mfxExtBuffer *) & filter->vpp_use;

    filter->params.NumExtParam = len + 1;
    filter->params.ExtParam = (mfxExtBuffer **) & filter->ext_buffer[0];
  }
  return TRUE;
}

static void
init_params (GstMfxFilter * filter)
{
  filter->params.vpp.In = filter->frame_info;
  filter->params.vpp.Out = filter->frame_info;

  /* If VPP is shared with encoder task, ensure alignment requirements */
  if (gst_mfx_task_get_task_type (filter->vpp[1]) != GST_MFX_TASK_VPP_OUT) { // shared task
    filter->params.vpp.Out.Width = GST_ROUND_UP_32 (filter->frame_info.CropW);
    filter->params.vpp.Out.Height = GST_ROUND_UP_32 (filter->frame_info.CropH);
  }

  if (filter->width) {
    filter->params.vpp.Out.CropW = filter->width;
    filter->params.vpp.Out.Width = GST_ROUND_UP_32 (filter->width);
  }
  if (filter->height) {
    filter->params.vpp.Out.CropH = filter->height;
    filter->params.vpp.Out.Height = GST_ROUND_UP_32 (filter->height);
  }
  if (filter->filter_op & GST_MFX_FILTER_FRAMERATE_CONVERSION &&
      (filter->fps_n && filter->fps_d)) {
    filter->params.vpp.Out.FrameRateExtN = filter->fps_n;
    filter->params.vpp.Out.FrameRateExtD = filter->fps_d;
  }
  if (filter->filter_op & GST_MFX_FILTER_DEINTERLACING) {
    gdouble frame_rate;
    /* Set up special double frame rate deinterlace mode */
    gst_util_fraction_to_double (filter->params.vpp.In.FrameRateExtN,
        filter->params.vpp.In.FrameRateExtD, &frame_rate);
    if ((filter->frame_info.PicStruct != MFX_PICSTRUCT_PROGRESSIVE)
        && (int) (frame_rate + 0.5) == 60)
      filter->params.vpp.In.FrameRateExtN /= 2;

    filter->params.vpp.Out.PicStruct = MFX_PICSTRUCT_PROGRESSIVE;
  }
  if (filter->fourcc) {
    filter->params.vpp.Out.FourCC = filter->fourcc;
    if (MFX_FOURCC_P010 == filter->fourcc) {
      filter->params.vpp.Out.BitDepthLuma = 10;
      filter->params.vpp.Out.BitDepthChroma = 10;
      filter->params.vpp.Out.Shift = 1;

      mfxStatus sts = MFXVideoVPP_Query (filter->session, &filter->params,
          &filter->params);
      if (MFX_ERR_NONE != sts)
        filter->params.vpp.Out.Shift = 0;
    } else {
      filter->params.vpp.Out.BitDepthLuma = 8;
      filter->params.vpp.Out.BitDepthChroma = 8;
      filter->params.vpp.Out.Shift = 0;
    }
  }
  configure_filters (filter);
}

gboolean
gst_mfx_filter_prepare (GstMfxFilter * filter)
{
  mfxFrameAllocRequest request[2];
  mfxStatus sts = MFX_ERR_NONE;

  /* Input / output memtypes may have been changed at this point by mfxvpp */
  gst_mfx_task_update_video_params (filter->vpp[1], &filter->params);
  init_params (filter);

  sts = MFXVideoVPP_QueryIOSurf (filter->session, &filter->params, request);
  if (sts < 0) {
    GST_ERROR ("Unable to query VPP allocation request %d", sts);
    return FALSE;
  } else if (sts > 0) {
    filter->params.IOPattern =
        MFX_IOPATTERN_IN_SYSTEM_MEMORY | MFX_IOPATTERN_OUT_SYSTEM_MEMORY;
  }

  if (filter->vpp[0]) {
    mfxFrameAllocRequest *req0 = gst_mfx_task_get_request (filter->vpp[0]);
    req0->NumFrameSuggested += request[0].NumFrameSuggested;
    req0->NumFrameMin += request[0].NumFrameMin;
    req0->Type |= MFX_MEMTYPE_FROM_VPPIN;
  }

  if (gst_mfx_task_get_task_type (filter->vpp[1]) == GST_MFX_TASK_VPP_OUT) {
    gst_mfx_task_set_request (filter->vpp[1], &request[1]);
  } else {
    mfxFrameAllocRequest *req1 = gst_mfx_task_get_request (filter->vpp[1]);
    req1->NumFrameSuggested += request[1].NumFrameSuggested;
    req1->NumFrameMin += request[1].NumFrameMin;
    req1->Type |= MFX_MEMTYPE_FROM_VPPOUT;
  }

  gst_mfx_task_set_video_params (filter->vpp[1], &filter->params);

  return TRUE;
}

static gboolean
gst_mfx_filter_create (GstMfxFilter * filter,
    GstMfxTaskAggregator * aggregator,
    gboolean is_system_in, gboolean is_system_out)
{
  filter->params.IOPattern |= is_system_in ?
      MFX_IOPATTERN_IN_SYSTEM_MEMORY : MFX_IOPATTERN_IN_VIDEO_MEMORY;
  filter->params.IOPattern |= is_system_out ?
      MFX_IOPATTERN_OUT_SYSTEM_MEMORY : MFX_IOPATTERN_OUT_VIDEO_MEMORY;
  filter->aggregator = gst_mfx_task_aggregator_ref (aggregator);

  if (!filter->vpp[1]) {
    if (!filter->session) {
      filter->vpp[1] =
          gst_mfx_task_new (filter->aggregator, GST_MFX_TASK_VPP_OUT);
      filter->session = gst_mfx_task_get_session (filter->vpp[1]);
    } else {
      /* is_joined is FALSE since parent task will take care of
       * disjoining / closing the session when it is destroyed */
      filter->vpp[1] = gst_mfx_task_new_with_session (filter->aggregator,
          filter->session, GST_MFX_TASK_VPP_OUT, FALSE);
    }
    if (!filter->vpp[1])
      return FALSE;
  }

  /* Initialize the array of operation data */
  filter->filter_op_data = g_ptr_array_new_with_free_func (free_filter_op_data);

  return TRUE;
}

static void
gst_mfx_filter_finalize (GObject * object)
{
  GstMfxFilter *filter = GST_MFX_FILTER (object);
  guint i;

  MFXVideoVPP_Close (filter->session);

  gst_mfx_surface_pool_replace (&filter->out_pool, NULL);
  /* Make sure frame allocator points to the right task to free surfaces */
  gst_mfx_task_aggregator_set_current_task (filter->aggregator, filter->vpp[1]);
  gst_mfx_task_frame_free (filter->aggregator, &filter->response);

  for (i = 0; i < 2; i++)
    gst_mfx_task_replace (&filter->vpp[i], NULL);

  /* Free allocated memory for filters */
  g_slice_free1 ((sizeof (mfxU32) * filter->vpp_use.NumAlg),
      filter->vpp_use.AlgList);
  g_slice_free1 ((sizeof (mfxExtBuffer *) * filter->params.NumExtParam),
      filter->ext_buffer);
  g_ptr_array_free (filter->filter_op_data, TRUE);
  gst_mfx_task_aggregator_unref (filter->aggregator);

  G_OBJECT_CLASS (gst_mfx_filter_parent_class)->finalize (object);
}

static void
gst_mfx_filter_init (GstMfxFilter * filter)
{
  filter->inited = FALSE;
  filter->filter_op = GST_MFX_FILTER_NONE;
}

static void
gst_mfx_filter_class_init (GstMfxFilterClass * klass)
{
  GObjectClass *const object_class = G_OBJECT_CLASS (klass);
  object_class->finalize = gst_mfx_filter_finalize;
}

GstMfxFilter *
gst_mfx_filter_new (GstMfxTaskAggregator * aggregator,
    gboolean is_system_in, gboolean is_system_out)
{
  GstMfxFilter *filter;

  g_return_val_if_fail (aggregator != NULL, NULL);

  filter = g_object_new (GST_TYPE_MFX_FILTER, NULL);
  if (!filter)
    return NULL;

  if (!gst_mfx_filter_create (filter, aggregator, is_system_in, is_system_out))
    goto error;
  return filter;

error:
  gst_object_unref (filter);
  return NULL;
}

GstMfxFilter *
gst_mfx_filter_new_with_task (GstMfxTaskAggregator * aggregator,
    GstMfxTask * task, GstMfxTaskType type,
    gboolean is_system_in, gboolean is_system_out)
{
  GstMfxFilter *filter;

  g_return_val_if_fail (aggregator != NULL, NULL);
  g_return_val_if_fail (task != NULL, NULL);

  filter = g_object_new (GST_TYPE_MFX_FILTER, NULL);
  if (!filter)
    return NULL;

  filter->session = gst_mfx_task_get_session (task);
  filter->vpp[!!(type & GST_MFX_TASK_VPP_OUT)] = gst_mfx_task_ref (task);

  gst_mfx_task_set_task_type (task, gst_mfx_task_get_task_type (task) | type);

  if (!gst_mfx_filter_create (filter, aggregator, is_system_in, is_system_out))
    goto error;
  return filter;

error:
  gst_object_unref (filter);
  return NULL;
}

GstMfxFilter *
gst_mfx_filter_ref (GstMfxFilter * filter)
{
  g_return_val_if_fail (filter != NULL, NULL);

  return gst_object_ref (GST_OBJECT (filter));
}

void
gst_mfx_filter_unref (GstMfxFilter * filter)
{
  gst_object_unref (GST_OBJECT (filter));
}

void
gst_mfx_filter_replace (GstMfxFilter ** old_filter_ptr,
    GstMfxFilter * new_filter)
{
  g_return_if_fail (old_filter_ptr != NULL);

  gst_object_replace ((GstObject **) old_filter_ptr, GST_OBJECT (new_filter));
}

gboolean
gst_mfx_filter_set_format (GstMfxFilter * filter, mfxU32 fourcc)
{
  g_return_val_if_fail (filter != NULL, FALSE);
  g_return_val_if_fail (MFX_FOURCC_NV12 == fourcc
      || MFX_FOURCC_RGB4 == fourcc
      || MFX_FOURCC_YUY2 == fourcc
      || MFX_FOURCC_A2RGB10 == fourcc
      || MFX_FOURCC_P010 == fourcc, FALSE);

  filter->fourcc = fourcc;

  return TRUE;
}

gboolean
gst_mfx_filter_set_size (GstMfxFilter * filter, mfxU16 width, mfxU16 height)
{
  g_return_val_if_fail (filter != NULL, FALSE);
  g_return_val_if_fail (width > 0 && width <= 8192, FALSE);
  g_return_val_if_fail (height > 0 && height <= 8192, FALSE);

  filter->width = width;
  filter->height = height;

  return TRUE;
}

static gpointer
init_deinterlacing_default ()
{
  mfxExtVPPDeinterlacing *ext_deinterlacing;

  ext_deinterlacing = g_slice_alloc0 (sizeof (mfxExtVPPDeinterlacing));
  if (!ext_deinterlacing)
    return NULL;
  ext_deinterlacing->Header.BufferId = MFX_EXTBUFF_VPP_DEINTERLACING;
  ext_deinterlacing->Header.BufferSz = sizeof (mfxExtVPPDeinterlacing);
  ext_deinterlacing->Mode = MFX_DEINTERLACING_ADVANCED; //Set as default

  return ext_deinterlacing;
}

static gpointer
init_procamp_default ()
{
  mfxExtVPPProcAmp *ext_procamp;

  ext_procamp = g_slice_alloc0 (sizeof (mfxExtVPPProcAmp));
  if (!ext_procamp)
    return NULL;
  ext_procamp->Header.BufferId = MFX_EXTBUFF_VPP_PROCAMP;
  ext_procamp->Header.BufferSz = sizeof (mfxExtVPPProcAmp);
  ext_procamp->Brightness = 0.0;
  ext_procamp->Contrast = 1.0;
  ext_procamp->Hue = 0.0;
  ext_procamp->Saturation = 1.0;

  return ext_procamp;
}

static gpointer
init_denoise_default ()
{
  mfxExtVPPDenoise *ext_denoise;

  ext_denoise = g_slice_alloc0 (sizeof (mfxExtVPPDenoise));
  if (!ext_denoise)
    return NULL;
  ext_denoise->Header.BufferId = MFX_EXTBUFF_VPP_DENOISE;
  ext_denoise->Header.BufferSz = sizeof (mfxExtVPPDenoise);
  ext_denoise->DenoiseFactor = 0;

  return ext_denoise;
}

static gpointer
init_detail_default ()
{
  mfxExtVPPDetail *ext_detail;

  ext_detail = g_slice_alloc0 (sizeof (mfxExtVPPDetail));
  if (!ext_detail)
    return NULL;
  ext_detail->Header.BufferId = MFX_EXTBUFF_VPP_DETAIL;
  ext_detail->Header.BufferSz = sizeof (mfxExtVPPDetail);
  ext_detail->DetailFactor = 0;

  return ext_detail;
}

static gpointer
init_rotation_default ()
{
  mfxExtVPPRotation *ext_rotation;

  ext_rotation = g_slice_alloc0 (sizeof (mfxExtVPPRotation));
  if (!ext_rotation)
    return NULL;
  ext_rotation->Header.BufferId = MFX_EXTBUFF_VPP_ROTATION;
  ext_rotation->Header.BufferSz = sizeof (mfxExtVPPRotation);
  ext_rotation->Angle = MFX_ANGLE_0;

  return ext_rotation;
}

#if MSDK_CHECK_VERSION(1,19)
static gpointer
init_mirroring_default ()
{
  mfxExtVPPMirroring *ext_mirroring;

  ext_mirroring = g_slice_alloc0 (sizeof (mfxExtVPPMirroring));
  if (!ext_mirroring)
    return NULL;
  ext_mirroring->Header.BufferId = MFX_EXTBUFF_VPP_MIRRORING;
  ext_mirroring->Header.BufferSz = sizeof (mfxExtVPPMirroring);
  ext_mirroring->Type = MFX_MIRRORING_DISABLED;

  return ext_mirroring;
}

static gpointer
init_scaling_default ()
{
  mfxExtVPPScaling *ext_scaling;

  ext_scaling = g_slice_alloc0 (sizeof (mfxExtVPPScaling));
  if (!ext_scaling)
    return NULL;
  ext_scaling->Header.BufferId = MFX_EXTBUFF_VPP_SCALING;
  ext_scaling->Header.BufferSz = sizeof (mfxExtVPPScaling);
  ext_scaling->ScalingMode = MFX_SCALING_MODE_DEFAULT;

  return ext_scaling;
}
#endif // MSDK_CHECK_VERSION

static gpointer
init_frc_default ()
{
  mfxExtVPPFrameRateConversion *ext_frc;

  ext_frc = g_slice_alloc0 (sizeof (mfxExtVPPFrameRateConversion));
  if (!ext_frc)
    return NULL;
  ext_frc->Header.BufferId = MFX_EXTBUFF_VPP_FRAME_RATE_CONVERSION;
  ext_frc->Header.BufferSz = sizeof (mfxExtVPPFrameRateConversion);
  ext_frc->Algorithm = 0;

  return ext_frc;
}

gboolean
gst_mfx_filter_set_saturation (GstMfxFilter * filter, gfloat value)
{
  GstMfxFilterOpData *op;
  mfxExtVPPProcAmp *ext_procamp;

  g_return_val_if_fail (filter != NULL, FALSE);
  g_return_val_if_fail (value <= 10.0, FALSE);
  g_return_val_if_fail (value >= 0.0, FALSE);

  op = find_filter_op_data (filter, GST_MFX_FILTER_PROCAMP);
  if (NULL == op) {
    if (!is_filter_supported (filter, MFX_EXTBUFF_VPP_PROCAMP))
      return FALSE;
    op = g_slice_alloc (sizeof (GstMfxFilterOpData));
    if (NULL == op)
      return FALSE;
    op->type = GST_MFX_FILTER_PROCAMP;
    filter->filter_op |= GST_MFX_FILTER_PROCAMP;
    op->size = sizeof (mfxExtVPPProcAmp);
    op->filter = init_procamp_default ();
    if (NULL == op->filter) {
      g_slice_free (GstMfxFilterOpData, op);
      return FALSE;
    }
    g_ptr_array_add (filter->filter_op_data, op);
  }
  ext_procamp = (mfxExtVPPProcAmp *) op->filter;
  ext_procamp->Saturation = value;

  return TRUE;
}

gboolean
gst_mfx_filter_set_brightness (GstMfxFilter * filter, gfloat value)
{
  GstMfxFilterOpData *op;
  mfxExtVPPProcAmp *ext_procamp;

  g_return_val_if_fail (filter != NULL, FALSE);
  g_return_val_if_fail (value <= 100.0, FALSE);
  g_return_val_if_fail (value >= -100.0, FALSE);

  op = find_filter_op_data (filter, GST_MFX_FILTER_PROCAMP);
  if (NULL == op) {
    if (!is_filter_supported (filter, MFX_EXTBUFF_VPP_PROCAMP)) {
      g_warning ("Color control filters not supported for this platform.");
      return FALSE;
    }
    op = g_slice_alloc (sizeof (GstMfxFilterOpData));
    if (NULL == op)
      return FALSE;
    op->type = GST_MFX_FILTER_PROCAMP;
    filter->filter_op |= GST_MFX_FILTER_PROCAMP;
    op->size = sizeof (mfxExtVPPProcAmp);
    op->filter = init_procamp_default ();
    if (NULL == op->filter) {
      g_slice_free (GstMfxFilterOpData, op);
      return FALSE;
    }
    g_ptr_array_add (filter->filter_op_data, op);
  }
  ext_procamp = (mfxExtVPPProcAmp *) op->filter;
  ext_procamp->Brightness = value;

  return TRUE;
}

gboolean
gst_mfx_filter_set_contrast (GstMfxFilter * filter, gfloat value)
{
  GstMfxFilterOpData *op;
  mfxExtVPPProcAmp *ext_procamp;

  g_return_val_if_fail (filter != NULL, FALSE);
  g_return_val_if_fail (value <= 10.0, FALSE);
  g_return_val_if_fail (value >= 0.0, FALSE);

  op = find_filter_op_data (filter, GST_MFX_FILTER_PROCAMP);
  if (NULL == op) {
    if (!is_filter_supported (filter, MFX_EXTBUFF_VPP_PROCAMP)) {
      g_warning ("Color control filters not supported for this platform.");
      return FALSE;
    }
    op = g_slice_alloc (sizeof (GstMfxFilterOpData));
    if (NULL == op)
      return FALSE;
    op->type = GST_MFX_FILTER_PROCAMP;
    filter->filter_op |= GST_MFX_FILTER_PROCAMP;
    op->size = sizeof (mfxExtVPPProcAmp);
    op->filter = init_procamp_default ();
    if (NULL == op->filter) {
      g_slice_free (GstMfxFilterOpData, op);
      return FALSE;
    }
    g_ptr_array_add (filter->filter_op_data, op);
  }
  ext_procamp = (mfxExtVPPProcAmp *) op->filter;
  ext_procamp->Contrast = value;

  return TRUE;
}

gboolean
gst_mfx_filter_set_hue (GstMfxFilter * filter, gfloat value)
{
  GstMfxFilterOpData *op;
  mfxExtVPPProcAmp *ext_procamp;

  g_return_val_if_fail (filter != NULL, FALSE);
  g_return_val_if_fail (value <= 180.0, FALSE);
  g_return_val_if_fail (value >= -180.0, FALSE);

  op = find_filter_op_data (filter, GST_MFX_FILTER_PROCAMP);
  if (NULL == op) {
    if (!is_filter_supported (filter, MFX_EXTBUFF_VPP_PROCAMP)) {
      g_warning ("Color control filters not supported for this platform.");
      return FALSE;
    }
    op = g_slice_alloc (sizeof (GstMfxFilterOpData));
    if (NULL == op)
      return FALSE;
    op->type = GST_MFX_FILTER_PROCAMP;
    filter->filter_op |= GST_MFX_FILTER_PROCAMP;
    op->size = sizeof (mfxExtVPPProcAmp);
    op->filter = init_procamp_default ();
    if (NULL == op->filter) {
      g_slice_free (GstMfxFilterOpData, op);
      return FALSE;
    }
    g_ptr_array_add (filter->filter_op_data, op);
  }
  ext_procamp = (mfxExtVPPProcAmp *) op->filter;
  ext_procamp->Hue = value;

  return TRUE;
}

gboolean
gst_mfx_filter_set_denoising_level (GstMfxFilter * filter, guint level)
{
  GstMfxFilterOpData *op;
  mfxExtVPPDenoise *ext_denoise;

  g_return_val_if_fail (filter != NULL, FALSE);
  g_return_val_if_fail (level <= 100, FALSE);

  op = find_filter_op_data (filter, GST_MFX_FILTER_DENOISE);
  if (NULL == op) {
    if (!is_filter_supported (filter, MFX_EXTBUFF_VPP_DENOISE)) {
      g_warning ("Denoising filter not supported for this platform.");
      return FALSE;
    }
    op = g_slice_alloc (sizeof (GstMfxFilterOpData));
    if (NULL == op)
      return FALSE;
    op->type = GST_MFX_FILTER_DENOISE;
    filter->filter_op |= GST_MFX_FILTER_DENOISE;
    op->size = sizeof (mfxExtVPPDenoise);
    op->filter = init_denoise_default ();
    if (NULL == op->filter) {
      g_slice_free (GstMfxFilterOpData, op);
      return FALSE;
    }
    g_ptr_array_add (filter->filter_op_data, op);
  }
  ext_denoise = (mfxExtVPPDenoise *) op->filter;
  ext_denoise->DenoiseFactor = level;

  return TRUE;
}

gboolean
gst_mfx_filter_set_detail_level (GstMfxFilter * filter, guint level)
{
  GstMfxFilterOpData *op;
  mfxExtVPPDetail *ext_detail;

  g_return_val_if_fail (filter != NULL, FALSE);
  g_return_val_if_fail (level <= 100, FALSE);

  op = find_filter_op_data (filter, GST_MFX_FILTER_DETAIL);
  if (NULL == op) {
    if (!is_filter_supported(filter, MFX_EXTBUFF_VPP_DETAIL)) {
      g_warning ("Detail filter not supported for this platform.");
      return FALSE;
    }
    op = g_slice_alloc (sizeof (GstMfxFilterOpData));
    if (NULL == op)
      return FALSE;
    op->type = GST_MFX_FILTER_DETAIL;
    filter->filter_op |= GST_MFX_FILTER_DETAIL;
    op->size = sizeof (mfxExtVPPDetail);
    op->filter = init_detail_default ();
    if (NULL == op->filter) {
      g_slice_free (GstMfxFilterOpData, op);
      return FALSE;
    }
    g_ptr_array_add (filter->filter_op_data, op);
  }
  ext_detail = (mfxExtVPPDetail *) op->filter;
  ext_detail->DetailFactor = level;

  return TRUE;
}

gboolean
gst_mfx_filter_set_rotation (GstMfxFilter * filter, GstMfxRotation angle)
{
  GstMfxFilterOpData *op;
  mfxExtVPPRotation *ext_rotation;

  g_return_val_if_fail (filter != NULL, FALSE);
  g_return_val_if_fail (angle == 0 ||
      angle == 90 || angle == 180 || angle == 270, FALSE);

  op = find_filter_op_data (filter, GST_MFX_FILTER_ROTATION);
  if (NULL == op) {
    if (!is_filter_supported (filter, MFX_EXTBUFF_VPP_ROTATION)) {
      g_warning ("Rotation filter not supported for this platform.");
      return FALSE;
    }
    op = g_slice_alloc (sizeof (GstMfxFilterOpData));
    if (NULL == op)
      return FALSE;
    op->type = GST_MFX_FILTER_ROTATION;
    filter->filter_op |= GST_MFX_FILTER_ROTATION;
    op->size = sizeof (mfxExtVPPRotation);
    op->filter = init_rotation_default ();
    if (NULL == op->filter) {
      g_slice_free (GstMfxFilterOpData, op);
      return FALSE;
    }
    g_ptr_array_add (filter->filter_op_data, op);
  }
  ext_rotation = (mfxExtVPPRotation *) op->filter;
  ext_rotation->Angle = angle;

  return TRUE;
}

#if MSDK_CHECK_VERSION(1,19)
gboolean
gst_mfx_filter_set_mirroring (GstMfxFilter * filter, GstMfxMirroring mode)
{
  GstMfxFilterOpData *op;
  mfxExtVPPMirroring *ext_mirroring;

  g_return_val_if_fail (filter != NULL, FALSE);
  g_return_val_if_fail (GST_MFX_MIRRORING_DISABLED == mode
      || GST_MFX_MIRRORING_HORIZONTAL == mode
      || GST_MFX_MIRRORING_VERTICAL == mode, FALSE);

  op = find_filter_op_data (filter, GST_MFX_FILTER_MIRRORING);
  if (NULL == op) {
    if (!is_filter_supported (filter, MFX_EXTBUFF_VPP_MIRRORING)) {
      g_warning ("Mirroring filter not supported for this platform.");
      return FALSE;
    }
    op = g_slice_alloc0 (sizeof (GstMfxFilterOpData));
    if (NULL == op)
      return FALSE;
    op->type = GST_MFX_FILTER_MIRRORING;
    filter->filter_op |= GST_MFX_FILTER_MIRRORING;
    op->size = sizeof (mfxExtVPPMirroring);
    op->filter = init_mirroring_default ();
    if (NULL == op->filter) {
      g_slice_free (GstMfxFilterOpData, op);
      return FALSE;
    }
    g_ptr_array_add (filter->filter_op_data, op);
  }
  ext_mirroring = (mfxExtVPPMirroring *) op->filter;
  ext_mirroring->Type = mode;

  return TRUE;
}

gboolean
gst_mfx_filter_set_scaling_mode (GstMfxFilter * filter, GstMfxScalingMode mode)
{
  GstMfxFilterOpData *op;
  mfxExtVPPScaling *ext_scaling;

  g_return_val_if_fail (filter != NULL, FALSE);
  g_return_val_if_fail (GST_MFX_SCALING_DEFAULT == mode
      || GST_MFX_SCALING_LOWPOWER == mode
      || GST_MFX_SCALING_QUALITY == mode, FALSE);

  op = find_filter_op_data (filter, GST_MFX_FILTER_SCALING_MODE);
  if (NULL == op) {
    if (!is_filter_supported (filter, MFX_EXTBUFF_VPP_SCALING)) {
      g_warning ("Scaling mode filter not supported for this platform.");
      return FALSE;
    }
    op = g_slice_alloc0 (sizeof (GstMfxFilterOpData));
    if (NULL == op)
      return FALSE;
    op->type = GST_MFX_FILTER_SCALING_MODE;
    filter->filter_op |= GST_MFX_FILTER_SCALING_MODE;
    op->size = sizeof (mfxExtVPPScaling);
    op->filter = init_scaling_default ();
    if (NULL == op->filter) {
      g_slice_free (GstMfxFilterOpData, op);
      return FALSE;
    }
    g_ptr_array_add (filter->filter_op_data, op);
  }
  ext_scaling = (mfxExtVPPScaling *) op->filter;
  ext_scaling->ScalingMode = mode;

  return TRUE;
}
#endif // MSDK_CHECK_VERSION

gboolean
gst_mfx_filter_set_deinterlace_method (GstMfxFilter * filter,
    GstMfxDeinterlaceMethod method)
{
  GstMfxFilterOpData *op;
  mfxExtVPPDeinterlacing *ext_deinterlacing;

  g_return_val_if_fail (filter != NULL, FALSE);

#if MSDK_CHECK_VERSION(1,19)
  g_return_val_if_fail (GST_MFX_DEINTERLACE_METHOD_BOB == method
      || GST_MFX_DEINTERLACE_METHOD_ADVANCED == method
      || GST_MFX_DEINTERLACE_METHOD_ADVANCED_NOREF == method
      || GST_MFX_DEINTERLACE_METHOD_ADVANCED_SCD == method
      || GST_MFX_DEINTERLACE_METHOD_FIELD_WEAVING == method, FALSE);
#else
  g_return_val_if_fail (GST_MFX_DEINTERLACE_METHOD_BOB == method
      || GST_MFX_DEINTERLACE_METHOD_ADVANCED == method
      || GST_MFX_DEINTERLACE_METHOD_ADVANCED_NOREF == method, FALSE);
#endif

  op = find_filter_op_data (filter, GST_MFX_FILTER_DEINTERLACING);
  if (NULL == op) {
    if (!is_filter_supported (filter, MFX_EXTBUFF_VPP_DEINTERLACING)) {
      g_warning ("Deinterlacing filter not supported for this platform.");
      return FALSE;
    }
    op = g_slice_alloc (sizeof (GstMfxFilterOpData));
    if (NULL == op)
      return FALSE;
    op->type = GST_MFX_FILTER_DEINTERLACING;
    filter->filter_op |= GST_MFX_FILTER_DEINTERLACING;
    op->size = sizeof (mfxExtVPPDeinterlacing);
    op->filter = init_deinterlacing_default ();
    if (NULL == op->filter) {
      g_slice_free (GstMfxFilterOpData, op);
      return FALSE;
    }
    g_ptr_array_add (filter->filter_op_data, op);
  }

  ext_deinterlacing = (mfxExtVPPDeinterlacing *) op->filter;
  ext_deinterlacing->Mode = method;

  return TRUE;
}

gboolean
gst_mfx_filter_set_framerate (GstMfxFilter * filter,
    guint16 fps_n, guint16 fps_d)
{
  g_return_val_if_fail (filter != NULL, FALSE);
  g_return_val_if_fail ((0 != fps_n && 0 != fps_d), FALSE);

  filter->fps_n = fps_n;
  filter->fps_d = fps_d;

  return TRUE;
}

gboolean
gst_mfx_filter_set_async_depth (GstMfxFilter * filter, mfxU16 async_depth)
{
  g_return_val_if_fail (async_depth <= 20, FALSE);

  filter->params.AsyncDepth = async_depth;
  return TRUE;
}

gboolean
gst_mfx_filter_set_iopattern_commit_to_task (GstMfxFilter * filter, mfxU16 iopattern)
{
  g_return_val_if_fail (filter != NULL, FALSE);

  filter->params.IOPattern = iopattern;
  gst_mfx_task_set_video_params (filter->vpp[1], &filter->params);
  return TRUE;
}

gboolean
gst_mfx_filter_set_frc_algorithm (GstMfxFilter * filter, GstMfxFrcAlgorithm alg)
{
  GstMfxFilterOpData *op;
  mfxExtVPPFrameRateConversion *ext_frc;

  g_return_val_if_fail (filter != NULL, FALSE);
#if 0
  g_return_val_if_fail (GST_MFX_FRC_PRESERVE_TIMESTAMP == alg
      || GST_MFX_FRC_DISTRIBUTED_TIMESTAMP == alg
      || GST_MFX_FRC_FRAME_INTERPOLATION == alg
      || GST_MFX_FRC_FI_PRESERVE_TIMESTAMP == alg
      || GST_MFX_FRC_FI_DISTRIBUTED_TIMESTAMP == alg, FALSE);
#else
  g_return_val_if_fail (GST_MFX_FRC_PRESERVE_TIMESTAMP == alg
      || GST_MFX_FRC_DISTRIBUTED_TIMESTAMP == alg, FALSE);
#endif // 0

  op = find_filter_op_data (filter, GST_MFX_FILTER_FRAMERATE_CONVERSION);
  if (NULL == op) {
    if (!is_filter_supported (filter, MFX_EXTBUFF_VPP_FRAME_RATE_CONVERSION)) {
      g_warning ("FRC filter not supported for this platform.");
      return FALSE;
    }
    op = g_slice_alloc (sizeof (GstMfxFilterOpData));
    if (NULL == op)
      return FALSE;
    op->type = GST_MFX_FILTER_FRAMERATE_CONVERSION;
    filter->filter_op |= GST_MFX_FILTER_FRAMERATE_CONVERSION;
    op->size = sizeof (mfxExtVPPFrameRateConversion);
    op->filter = init_frc_default ();
    if (NULL == op->filter) {
      g_slice_free (GstMfxFilterOpData, op);
      return FALSE;
    }
    g_ptr_array_add (filter->filter_op_data, op);
  }

  ext_frc = (mfxExtVPPFrameRateConversion *) op->filter;
  ext_frc->Algorithm = alg;
  return TRUE;
}

GstMfxFilterStatus
gst_mfx_filter_reset (GstMfxFilter * filter)
{
  mfxStatus sts = MFX_ERR_NONE;
  configure_filters (filter);

  /* If filter is not initialized and reset
   * is called by before_transform method,
   * return GST_MFX_FILTER_STATUS_SUCCESS */
  if (!filter->inited)
    return GST_MFX_FILTER_STATUS_SUCCESS;

  sts = MFXVideoVPP_Reset (filter->session, &filter->params);
  if (sts < 0) {
    GST_ERROR ("Error resetting MFX VPP %d", sts);
    return GST_MFX_FILTER_STATUS_ERROR_OPERATION_FAILED;
  }
  return GST_MFX_FILTER_STATUS_SUCCESS;
}

static GstMfxFilterStatus
gst_mfx_filter_start (GstMfxFilter * filter)
{
  mfxFrameAllocRequest *request;
  mfxStatus sts = MFX_ERR_NONE;
  gboolean memtype_is_system;

  /* Get updated video params if modified by peer MFX element */
  gst_mfx_task_update_video_params (filter->vpp[1], &filter->params);

  request = gst_mfx_task_get_request (filter->vpp[1]);
  if (!request) {
    GST_ERROR
        ("Unable to retrieve task parameters from VPP allocation request.");
    return GST_MFX_FILTER_STATUS_ERROR_INVALID_PARAMETER;
  }

  memtype_is_system =
      !(filter->params.IOPattern & MFX_IOPATTERN_OUT_VIDEO_MEMORY);
  if (!memtype_is_system) {
    gst_mfx_task_use_video_memory (filter->vpp[1]);

    /* Make sure frame allocator points to the right task to allocate surfaces */
    gst_mfx_task_aggregator_set_current_task (filter->aggregator,
        filter->vpp[1]);
    sts = gst_mfx_task_frame_alloc (filter->aggregator,
        request, &filter->response);
    if (MFX_ERR_NONE != sts)
      return GST_MFX_FILTER_STATUS_ERROR_ALLOCATION_FAILED;
  } else {
    gst_mfx_task_ensure_memtype_is_system (filter->vpp[1]);
  }

  filter->out_pool = gst_mfx_surface_pool_new_with_task (filter->vpp[1]);
  if (!filter->out_pool)
    return GST_MFX_FILTER_STATUS_ERROR_ALLOCATION_FAILED;

  sts = MFXVideoVPP_Init (filter->session, &filter->params);
  if (sts < 0) {
    GST_ERROR ("Error initializing MFX VPP %d", sts);
    return GST_MFX_FILTER_STATUS_ERROR_OPERATION_FAILED;
  }

  GST_INFO ("Initialized MFX VPP output task using %s memory",
      memtype_is_system ? "system" : "video");

  return GST_MFX_FILTER_STATUS_SUCCESS;
}

GstMfxFilterStatus
gst_mfx_filter_process (GstMfxFilter * filter, GstMfxSurface * surface,
    GstMfxSurface ** out_surface)
{
  mfxFrameSurface1 *insurf, *outsurf = NULL;
  mfxSyncPoint syncp;
  mfxStatus sts = MFX_ERR_NONE;
  GstMfxFilterStatus ret = GST_MFX_FILTER_STATUS_SUCCESS;
  gboolean more_surface = FALSE;

  /* Delayed VPP initialization to enable surface pool sharing with
   * encoder plugin */
  if (G_UNLIKELY (!filter->inited)) {
    ret = gst_mfx_filter_start (filter);
    if (ret != GST_MFX_FILTER_STATUS_SUCCESS)
      return ret;
    filter->inited = TRUE;
  }

  insurf = gst_mfx_surface_get_frame_surface (surface);

  do {
    *out_surface = gst_mfx_surface_new_from_pool (filter->out_pool);
    if (!*out_surface)
      return GST_MFX_FILTER_STATUS_ERROR_ALLOCATION_FAILED;

    outsurf = gst_mfx_surface_get_frame_surface (*out_surface);
    sts = MFXVideoVPP_RunFrameVPPAsync (filter->session, insurf, outsurf,
        NULL, &syncp);

    if (MFX_WRN_INCOMPATIBLE_VIDEO_PARAM == sts)
      sts = MFX_ERR_NONE;

    if (MFX_WRN_DEVICE_BUSY == sts)
      g_usleep (500);
  } while (MFX_WRN_DEVICE_BUSY == sts);

  if (MFX_ERR_MORE_DATA == sts)
    return GST_MFX_FILTER_STATUS_ERROR_MORE_DATA;

  /* The current frame is ready. Hence treat it
   * as MFX_ERR_NONE and request for more surface
   */
  if (MFX_ERR_MORE_SURFACE == sts) {
    sts = MFX_ERR_NONE;
    more_surface = TRUE;
  }

  if (MFX_ERR_NONE != sts) {
    GST_ERROR ("MFXVideoVPP_RunFrameVPPAsync() error status: %d", sts);
    return GST_MFX_FILTER_STATUS_ERROR_OPERATION_FAILED;
  }

  if (syncp) {
    if (!gst_mfx_task_has_type (filter->vpp[1], GST_MFX_TASK_ENCODER))
      do {
        sts = MFXVideoCORE_SyncOperation (filter->session, syncp, 1000);
        if (MFX_ERR_NONE != sts && sts < 0) {
          GST_ERROR ("MFXVideoCORE_SyncOperation() error status: %d", sts);
          return GST_MFX_FILTER_STATUS_ERROR_OPERATION_FAILED;
        }
      } while (MFX_WRN_IN_EXECUTION == sts);

    *out_surface =
        gst_mfx_surface_pool_find_surface (filter->out_pool, outsurf);
  }

  if (more_surface)
    return GST_MFX_FILTER_STATUS_ERROR_MORE_SURFACE;

  return GST_MFX_FILTER_STATUS_SUCCESS;
}
