/*
    This file is part of darktable,
    Copyright (C) 2022-23 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

/* The refavg values are calculated in raw-RGB-cube3 space
   We calculate all color channels in the 3x3 photosite area, this can be understaood as a "superpixel",
   the "asking" location is in the centre.
   As this works for bayer and xtrans sensors we don't have a fixed ratio but calculate the average
   for every color channel first.
   refavg for one of red, green or blue is defined as means of both other color channels (opposing).
   
   The basic idea / observation for the _process_opposed algorithm is, the refavg is a good estimate
   for any clipped color channel in the vast majority of images, working mostly fine both for small specular
   highlighted spots and large areas.
   
   The correction via some sort of global chrominance further helps to correct color casts.
   The chrominace data are taken from the areas morphologically very close to clipped data.
   Failures of the algorithm (color casts) are in most cases related to
    a) very large differences between optimal white balance coefficients vs what we have as D65 in the darktable pipeline
    b) complicated lightings so the gradients are not well related
    c) a wrong whitepoint setting in the rawprepare module. 
    d) the maths might not be best

   Again the algorithm has been developed in collaboration by @garagecoder and @Iain from gmic team and @jenshannoschwalm from dt.
*/
// #define DT_OPPCHROMA_HISTORY

static inline float _calc_linear_refavg(const float *in, const int color)
{
  const dt_aligned_pixel_t ins = { powf(fmaxf(0.0f, in[0]), 1.0f / HL_POWERF),
                                   powf(fmaxf(0.0f, in[1]), 1.0f / HL_POWERF),
                                   powf(fmaxf(0.0f, in[2]), 1.0f / HL_POWERF), 0.0f };
  const dt_aligned_pixel_t opp = { 0.5f*(ins[1]+ins[2]), 0.5f*(ins[0]+ins[2]), 0.5f*(ins[0]+ins[1]), 0.0f};

  return powf(opp[color], HL_POWERF);
}

static inline size_t _raw_to_cmap(const size_t width, const size_t row, const size_t col)
{
  return (row / 3) * width + (col / 3);
}

static inline char _mask_dilated(const char *in, const size_t w1)
{
  if(in[0])
    return 1;

  if(in[-w1-1] | in[-w1] | in[-w1+1] | in[-1] | in[1] | in[w1-1] | in[w1] | in[w1+1])
    return 1;

  const size_t w2 = 2*w1;
  const size_t w3 = 3*w1;
  return  in[-w3-2] | in[-w3-1] | in[-w3] | in[-w3+1] | in[-w3+2] |
          in[-w2-3] | in[-w2-2] | in[-w2-1] | in[-w2] | in[-w2+1] | in[-w2+2] | in[-w2+3] |
          in[-w1-3] | in[-w1-2] | in[-w1+2] | in[-w1+3] |
          in[-3]    | in[-2]    | in[2]     | in[3] |
          in[w1-3]  | in[w1-2]  | in[w1+2]  | in[w1+3] |
          in[w2-3]  | in[w2-2]  | in[w2-1]  | in[w2]  | in[w2+1]  | in[w2+2]  | in[w2+3] |
          in[w3-2]  | in[w3-1]  | in[w3]  | in[w3+1]  | in[w3+2];
}


// A slightly modified version for sraws
static void _process_linear_opposed(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const float *const input, float *const output,
                         const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out, const gboolean quality)
{
  dt_iop_highlights_data_t *d = (dt_iop_highlights_data_t *)piece->data;
  const float clipval = 0.987f * d->clip;
  const dt_iop_buffer_dsc_t *dsc = &piece->pipe->dsc;
  const gboolean wbon = dsc->temperature.enabled;
  const dt_aligned_pixel_t icoeffs = { wbon ? dsc->temperature.coeffs[0] : 1.0f,
                                       wbon ? dsc->temperature.coeffs[1] : 1.0f,
                                       wbon ? dsc->temperature.coeffs[2] : 1.0f};
  const dt_aligned_pixel_t clips = { clipval * icoeffs[0], clipval * icoeffs[1], clipval * icoeffs[2]}; 
  const dt_aligned_pixel_t clipdark = { 0.03f * clips[0], 0.125f * clips[1], 0.03f * clips[2] };   

  const size_t mwidth  = roi_in->width / 3;
  const size_t mheight = roi_in->height / 3;
  const size_t msize = dt_round_size((size_t) (mwidth+1) * (mheight+1), 64);

  dt_aligned_pixel_t chrominance = {d->chroma_correction[0], d->chroma_correction[1], d->chroma_correction[2], 0.0f};

  if(!feqf(_color_magic(piece), d->chroma_correction[3], 1e-6f))
  {
    char *mask = (quality) ? dt_calloc_align(64, 6 * msize * sizeof(char)) : NULL;
    if(mask)
    {
      gboolean anyclipped = FALSE;
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  reduction( | : anyclipped) \
  dt_omp_firstprivate(clips, input, roi_in, mask) \
  dt_omp_sharedconst(msize, mwidth) \
  schedule(static)
#endif
      for(size_t row = 1; row < roi_in->height -1; row++)
      {
        for(size_t col = 1; col < roi_in->width -1; col++)
        {
          const size_t idx = (row * roi_in->width + col) * 4;
          for_each_channel(c)
          {
            if(input[idx+c] >= clips[c])
            {
              mask[c * msize + _raw_to_cmap(mwidth, row, col)] |= 1;
              anyclipped |= TRUE;
            }
          }
        }
      }
      /* We want to use the photosites closely around clipped data to be taken into account.
         The mask buffers holds data for each color channel, we dilate the mask buffer slightly
         to get those locations.
      */
      if(anyclipped)
      {
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(mask) \
  dt_omp_sharedconst(mwidth, mheight, msize) \
  schedule(static) collapse(2)
#endif
        for(size_t row = 3; row < mheight - 3; row++)
        {
          for(size_t col = 3; col < mwidth - 3; col++)
          {
            const size_t mx = row * mwidth + col;
            mask[3*msize + mx] = _mask_dilated(mask + mx, mwidth);
            mask[4*msize + mx] = _mask_dilated(mask + msize + mx, mwidth);
            mask[5*msize + mx] = _mask_dilated(mask + 2*msize + mx, mwidth);
          }
        }

        dt_aligned_pixel_t cr_sum = {0.0f, 0.0f, 0.0f, 0.0f};
        dt_aligned_pixel_t cr_cnt = {0.0f, 0.0f, 0.0f, 0.0f};
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(input, roi_in, clips, clipdark, mask) \
  reduction(+ : cr_sum, cr_cnt) \
  dt_omp_sharedconst(msize, mwidth) \
  schedule(static)
#endif
        for(size_t row = 1; row < roi_in->height-1; row++)
        {
          for(size_t col = 1; col < roi_in->width - 1; col++)
          {
            const size_t idx = (row * roi_in->width + col) * 4;
            for_each_channel(c)
            {
              const float inval = fmaxf(0.0f, input[idx+c]); 
              if((inval > clipdark[c]) && (inval < clips[c]) && (mask[(c+3) * msize + _raw_to_cmap(mwidth, row, col)]))
              {
                cr_sum[c] += inval - _calc_linear_refavg(&input[idx], c);
                cr_cnt[c] += 1.0f;
              }
            }
          }
        }
        for_each_channel(c)
          chrominance[c] = cr_sum[c] / fmaxf(1.0f, cr_cnt[c]);
      }

      // also checking for an altered image to avoid xmp writing if not desired
      if((piece->pipe->type == DT_DEV_PIXELPIPE_FULL)
         && (abs((int)(roi_out->width / roi_out->scale) - piece->buf_in.width) < 10)
         && (abs((int)(roi_out->height / roi_out->scale) - piece->buf_in.height) < 10)
         && dt_image_altered(piece->pipe->image.id))
      {
        dt_iop_highlights_params_t *p = (dt_iop_highlights_params_t *)self->params;
        for(int c = 0; c < 3; c++)
          d->chroma_correction[c] = p->chroma_correction[c] = chrominance[c];
        d->chroma_correction[3] = p->chroma_correction[3] = _color_magic(piece);
        dt_dev_add_history_item(darktable.develop, self, TRUE);
#ifdef DT_OPPCHROMA_HISTORY
        fprintf(stderr, "[new linear chroma history] %f %f %f\n", chrominance[0], chrominance[1], chrominance[2]);
#endif
      }
    }
    dt_free_align(mask);
  }
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(output, input, roi_in, roi_out, chrominance, clips) \
  schedule(static) collapse(2)
#endif
  for(ssize_t row = 0; row < roi_out->height; row++)
  {
    for(ssize_t col = 0; col < roi_out->width; col++)
    {
      const ssize_t odx = (row * roi_out->width + col) * 4;
      const ssize_t inrow = MIN(row, roi_in->height-1);
      const ssize_t incol = MIN(col, roi_in->width-1);
      const ssize_t idx = (inrow * roi_in->width + incol) * 4;
      for_each_channel(c)
      {
        const float ref = _calc_linear_refavg(&input[idx], c);
        const float inval = fmaxf(0.0f, input[idx+c]);
        output[odx+c] = (inval >= clips[c]) ? fmaxf(inval, ref + chrominance[c]) : inval;
      }
    }
  }
}

static float *_process_opposed(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const float *const input, float *const output,
                         const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out,
                         const gboolean keep, const gboolean quality)
{
  dt_iop_highlights_data_t *d = (dt_iop_highlights_data_t *)piece->data;
  const uint8_t(*const xtrans)[6] = (const uint8_t(*const)[6])piece->pipe->dsc.xtrans;
  const uint32_t filters = piece->pipe->dsc.filters;
  const float clipval = 0.987f * d->clip;
  const dt_iop_buffer_dsc_t *dsc = &piece->pipe->dsc;
  const gboolean wbon = dsc->temperature.enabled;
  const dt_aligned_pixel_t icoeffs = { wbon ? dsc->temperature.coeffs[0] : 1.0f,
                                       wbon ? dsc->temperature.coeffs[1] : 1.0f,
                                       wbon ? dsc->temperature.coeffs[2] : 1.0f};
  const dt_aligned_pixel_t clips = { clipval * icoeffs[0], clipval * icoeffs[1], clipval * icoeffs[2]};
  const dt_aligned_pixel_t clipdark = { 0.03f * clips[0], 0.125f * clips[1], 0.03f * clips[2] };

  const size_t mwidth  = roi_in->width / 3;
  const size_t mheight = roi_in->height / 3;
  const size_t msize = dt_round_size((size_t) (mwidth+1) * (mheight+1), 64);

  dt_aligned_pixel_t chrominance = {d->chroma_correction[0], d->chroma_correction[1], d->chroma_correction[2], 0.0f};

  if(!feqf(_color_magic(piece), d->chroma_correction[3], 1e-6f))
  {
    char *mask = (quality) ? dt_calloc_align(64, 6 * msize * sizeof(char)) : NULL;
    if(mask)
    {
      gboolean anyclipped = FALSE;
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  reduction( | : anyclipped) \
  dt_omp_firstprivate(clips, input, roi_in, xtrans, mask) \
  dt_omp_sharedconst(filters, msize, mwidth) \
  schedule(static) collapse(2)
#endif
      for(size_t row = 1; row < roi_in->height -1; row++)
      {
        for(size_t col = 1; col < roi_in->width -1; col++)
        {
          const size_t idx = row * roi_in->width + col;
          const int color = (filters == 9u) ? FCxtrans(row, col, roi_in, xtrans) : FC(row, col, filters);
          if(fmaxf(0.0f, input[idx]) >= clips[color])
          {
            mask[color * msize + _raw_to_cmap(mwidth, row, col)] |= 1;
            anyclipped |= TRUE;
          }
        }
      }
      /* We want to use the photosites closely around clipped data to be taken into account.
         The mask buffers holds data for each color channel, we dilate the mask buffer slightly
         to get those locations.
         If there are no clipped locations we keep the chrominance correction at 0 but make it valid 
      */
      if(anyclipped)
      {
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(mask) \
  dt_omp_sharedconst(mwidth, mheight, msize) \
  schedule(static) collapse(2)
#endif
        for(size_t row = 3; row < mheight - 3; row++)
        {
          for(size_t col = 3; col < mwidth - 3; col++)
          {
            const size_t mx = row * mwidth + col;
            mask[3*msize + mx] = _mask_dilated(mask + mx, mwidth);
            mask[4*msize + mx] = _mask_dilated(mask + msize + mx, mwidth);
            mask[5*msize + mx] = _mask_dilated(mask + 2*msize + mx, mwidth);
          }
        }

        /* After having the surrounding mask for each color channel we can calculate the chrominance corrections. */ 
        dt_aligned_pixel_t cr_sum = {0.0f, 0.0f, 0.0f, 0.0f};
        dt_aligned_pixel_t cr_cnt = {0.0f, 0.0f, 0.0f, 0.0f};
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(input, roi_in, xtrans, clips, clipdark, mask) \
  reduction(+ : cr_sum, cr_cnt) \
  dt_omp_sharedconst(filters, msize, mwidth) \
  schedule(static) collapse(2)
#endif
        for(size_t row = 1; row < roi_in->height - 1; row++)
        {
          for(size_t col = 1; col < roi_in->width - 1; col++)
          {
            const size_t idx = row * roi_in->width + col;
            const int color = (filters == 9u) ? FCxtrans(row, col, roi_in, xtrans) : FC(row, col, filters);
            const float inval = fmaxf(0.0f, input[idx]); 
            /* we only use the unclipped photosites very close the true clipped data to calculate the chrominance offset */
            if((inval < clips[color]) && (inval > clipdark[color]) && (mask[(color+3) * msize + _raw_to_cmap(mwidth, row, col)]))
            {
              cr_sum[color] += inval - _calc_refavg(&input[idx], xtrans, filters, row, col, roi_in, TRUE);
              cr_cnt[color] += 1.0f;
            }
          }
        }
        for_each_channel(c)
          chrominance[c] = cr_sum[c] / fmaxf(1.0f, cr_cnt[c]);
      }

      // also checking for an altered image to avoid xmp writing if not desired
      if((piece->pipe->type == DT_DEV_PIXELPIPE_FULL) && dt_image_altered(piece->pipe->image.id))
      {
        dt_iop_highlights_params_t *p = (dt_iop_highlights_params_t *)self->params;
        for(int c = 0; c < 3; c++)
          d->chroma_correction[c] = p->chroma_correction[c] = chrominance[c];
        d->chroma_correction[3] = p->chroma_correction[3] = _color_magic(piece);
        dt_dev_add_history_item(darktable.develop, self, TRUE);
#ifdef DT_OPPCHROMA_HISTORY
        fprintf(stderr, "[new chroma history] %f %f %f\n", chrominance[0], chrominance[1], chrominance[2]);
#endif
      }
    }
    dt_free_align(mask);
  }
 
  float *tmpout = (keep) ? dt_alloc_align_float(roi_in->width * roi_in->height) : NULL;
  if(tmpout)
  {
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(clips, input, tmpout, roi_in, xtrans, chrominance) \
  dt_omp_sharedconst(filters) \
  schedule(static) collapse(2)
#endif
    for(size_t row = 0; row < roi_in->height; row++)
    {
      for(size_t col = 0; col < roi_in->width; col++)
      {
        const size_t idx = row * roi_in->width + col;
        const int color = (filters == 9u) ? FCxtrans(row, col, roi_in, xtrans) : FC(row, col, filters);
        const float inval = fmaxf(0.0f, input[idx]);
        if((inval >= clips[color]) && (col > 0) && (col < roi_in->width - 1) && (row > 0) && (row < roi_in->height - 1))
        {
          const float ref = _calc_refavg(&input[idx], xtrans, filters, row, col, roi_in, TRUE);
          tmpout[idx] = fmaxf(inval, ref + chrominance[color]);
        }
        else
          tmpout[idx] = inval;
      }
    }
  }
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(output, input, tmpout, chrominance, clips, xtrans, roi_in, roi_out) \
  dt_omp_sharedconst(filters) \
  schedule(static) collapse(2)
#endif
  for(size_t row = 0; row < roi_out->height; row++)
  {
    for(size_t col = 0; col < roi_out->width; col++) 
    {
      const size_t odx = row * roi_out->width + col;
      const size_t irow = row + roi_out->y;
      const size_t icol = col + roi_out->x;
      const size_t ix = irow * roi_in->width + icol;
      float oval = 0.0f;
      if((irow < roi_in->height) && (icol < roi_in->width))
      {
        if(tmpout)
          oval = tmpout[ix];
        else
        { 
          const int color = (filters == 9u) ? FCxtrans(irow, icol, roi_in, xtrans) : FC(irow, icol, filters);
          const gboolean inrefs = (irow > 0) && (icol > 0) && (irow < roi_in->height-1) && (icol < roi_in->width-1);
          oval = fmaxf(0.0f, input[ix]);
          if(inrefs && (oval >= clips[color]))
          {
            const float ref = _calc_refavg(&input[ix], xtrans, filters, irow, icol, roi_in, TRUE);
            oval = fmaxf(oval, ref + chrominance[color]);
          }
        }
      }
      output[odx] = oval;
    }
  }
  return tmpout;
}

#ifdef HAVE_OPENCL
static cl_int process_opposed_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece,
                                         cl_mem dev_in, cl_mem dev_out,
                                         const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_highlights_data_t *d = (dt_iop_highlights_data_t *)piece->data;
  const dt_iop_highlights_global_data_t *gd = (dt_iop_highlights_global_data_t *)self->global_data;

  const int devid = piece->pipe->devid;
  const uint32_t filters = piece->pipe->dsc.filters;
  const float clipval = 0.987f * d->clip;
  const dt_iop_buffer_dsc_t *dsc = &piece->pipe->dsc;
  const gboolean wbon = dsc->temperature.enabled;
  const dt_aligned_pixel_t icoeffs = { wbon ? dsc->temperature.coeffs[0] : 1.0f,
                                       wbon ? dsc->temperature.coeffs[1] : 1.0f,
                                       wbon ? dsc->temperature.coeffs[2] : 1.0f};

  dt_aligned_pixel_t clips = { clipval * icoeffs[0], clipval * icoeffs[1], clipval * icoeffs[2], 1.0f};
  dt_aligned_pixel_t chrominance = { d->chroma_correction[0], d->chroma_correction[1], d->chroma_correction[2], 0.0f};
  dt_aligned_pixel_t clipdark = { 0.03f * clips[0], 0.125f * clips[1], 0.03f * clips[2], 0.0f };

  cl_int err = DT_OPENCL_DEFAULT_ERROR;
  cl_mem dev_chrominance = NULL;
  cl_mem dev_dark = NULL;
  cl_mem dev_xtrans = NULL;
  cl_mem dev_clips = NULL;
  cl_mem dev_inmask = NULL;
  cl_mem dev_outmask = NULL;
  cl_mem dev_accu = NULL;

  const size_t iwidth = ROUNDUPDWD(roi_in->width, devid);
  const size_t iheight = ROUNDUPDHT(roi_in->height, devid);

  const int mwidth  = roi_in->width / 3;
  const int mheight = roi_in->height / 3;
  const int msize = dt_round_size((size_t) (mwidth+1) * (mheight+1), 64);

  dev_xtrans = dt_opencl_copy_host_to_device_constant(devid, sizeof(piece->pipe->dsc.xtrans), piece->pipe->dsc.xtrans);
  if(dev_xtrans == NULL) goto error;

  dev_clips = dt_opencl_copy_host_to_device_constant(devid, 4 * sizeof(float), clips);
  if(dev_clips == NULL) goto error;

  if(!feqf(_color_magic(piece), d->chroma_correction[3], 1e-6f))
  {
    // We don't have valid chrominance correction so go the hard way
    dev_dark = dt_opencl_copy_host_to_device_constant(devid, 4 * sizeof(float), clipdark);
    if(dev_dark == NULL) goto error;

    dev_inmask = dt_opencl_alloc_device_buffer(devid, sizeof(char) * 3 * msize);
    if(dev_inmask == NULL) goto error;

    dev_outmask =  dt_opencl_alloc_device_buffer(devid, sizeof(char) * 3 * msize);
    if(dev_outmask == NULL) goto error;

    dev_accu = dt_opencl_alloc_device_buffer(devid, sizeof(float) * 8);
    if(dev_accu == NULL) goto error;

    err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_highlights_initmask, iwidth, iheight,
            CLARG(dev_in), CLARG(dev_inmask), CLARG(roi_in->width), CLARG(roi_in->height), CLARG(msize), CLARG(mwidth),
            CLARG(filters), CLARG(dev_xtrans), CLARG(dev_clips));
    if(err != CL_SUCCESS) goto error;

    err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_highlights_dilatemask, mwidth, mheight,
            CLARG(dev_inmask), CLARG(dev_outmask), CLARG(mwidth), CLARG(mheight), CLARG(msize));
    if(err != CL_SUCCESS) goto error;

    float accu[8] = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
    err = dt_opencl_write_buffer_to_device(devid, accu, dev_accu, 0, 8 * sizeof(float), TRUE);
    if(err != CL_SUCCESS) goto error;

    err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_highlights_chroma, iwidth, iheight,
            CLARG(dev_in), CLARG(dev_outmask), CLARG(dev_accu),
            CLARG(roi_in->width), CLARG(roi_in->height),
            CLARG(mwidth), CLARG(mheight), CLARG(msize),
            CLARG(filters), CLARG(dev_xtrans),
            CLARG(dev_clips), CLARG(dev_dark)); 
    if(err != CL_SUCCESS) goto error;

    err = dt_opencl_read_buffer_from_device(devid, accu, dev_accu, 0, 8 * sizeof(float), TRUE);
    if(err != CL_SUCCESS) goto error;

    for(int c = 0; c < 3; c++)
      chrominance[c] = accu[c] / fmaxf(1.0f, accu[c+4]);

    // also checking for an altered image to avoid xmp writing if not desired
    if((piece->pipe->type == DT_DEV_PIXELPIPE_FULL) && dt_image_altered(piece->pipe->image.id))
    {
      dt_iop_highlights_params_t *p = (dt_iop_highlights_params_t *)self->params;
      for(int c = 0; c < 3; c++)
        d->chroma_correction[c] = p->chroma_correction[c] = chrominance[c];
      d->chroma_correction[3] = p->chroma_correction[3] = _color_magic(piece);
      dt_dev_add_history_item(darktable.develop, self, TRUE);
#ifdef DT_OPPCHROMA_HISTORY
      fprintf(stderr, "[new OpenCL chroma history] %f %f %f\n", chrominance[0], chrominance[1], chrominance[2]);
#endif
    }
  }

  dev_chrominance = dt_opencl_copy_host_to_device_constant(devid, 4 * sizeof(float), chrominance);
  if(dev_chrominance == NULL) goto error;

  err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_highlights_opposed, iwidth, iheight,
          CLARG(dev_in), CLARG(dev_out),
          CLARG(roi_out->width), CLARG(roi_out->height), CLARG(roi_in->width), CLARG(roi_in->height),
          CLARG(roi_out->x), CLARG(roi_out->y), CLARG(filters), CLARG(dev_xtrans),
          CLARG(dev_clips), CLARG(dev_chrominance));
  if(err != CL_SUCCESS) goto error;

  dt_opencl_release_mem_object(dev_clips);
  dt_opencl_release_mem_object(dev_xtrans);
  dt_opencl_release_mem_object(dev_chrominance);
  dt_opencl_release_mem_object(dev_dark);
  dt_opencl_release_mem_object(dev_inmask);
  dt_opencl_release_mem_object(dev_outmask);
  dt_opencl_release_mem_object(dev_accu);
  return CL_SUCCESS;

  error:
  // just in case the last error was generated via a copy function
  if(err == CL_SUCCESS) err = DT_OPENCL_DEFAULT_ERROR;

  dt_opencl_release_mem_object(dev_clips);
  dt_opencl_release_mem_object(dev_xtrans);
  dt_opencl_release_mem_object(dev_chrominance);
  dt_opencl_release_mem_object(dev_dark);
  dt_opencl_release_mem_object(dev_inmask);
  dt_opencl_release_mem_object(dev_outmask);
  dt_opencl_release_mem_object(dev_accu);
  return err;
}
#endif

