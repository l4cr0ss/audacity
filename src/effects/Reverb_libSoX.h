/**********************************************************************

  Audacity: A Digital Audio Editor

  Reverb_libSoX.h
  Stereo reverberation effect from libSoX,
  adapted for Audacity

  Copyright (c) 2007-2013 robs@users.sourceforge.net
  Licence: LGPL v2.1
  Filter configuration based on freeverb by Jezar Wakefield.

**********************************************************************/

#include <cstring>
#include <cstdlib>
#ifdef __WXMSW__
   #define M_LN10   2.30258509299404568402 /* log_e 10 */
#else
   #include <cmath>
#endif
#include <algorithm>
using std::min;
using std::max;

#define array_length(a) (sizeof(a)/sizeof(a[0]))
#define dB_to_linear(x) exp((x) * M_LN10 * 0.05)
#define midi_to_freq(n) (440 * pow(2,((n)-69)/12.))
#define FIFO_SIZE_T size_t
#define FIFO_MIN 0x4000
#define fifo_read_ptr(f) fifo_read(f, (FIFO_SIZE_T)0, NULL)
#define lsx_zalloc(var, n) (var.reinit(n, true), var.get())
#define filter_advance(p) if (--(p)->ptr < (p)->buffer.get()) (p)->ptr += (p)->size

typedef struct {
   ArrayOf<char> data;
   size_t allocation {};   /* Number of bytes allocated for data. */
   size_t item_size {};    /* Size of each item in data */
   size_t begin {};        /* Offset of the first byte to read. */
   size_t end {};          /* 1 + Offset of the last byte byte to read. */
} fifo_t;

static void fifo_clear(fifo_t * f)
{
   f->end = f->begin = 0;
}

static void * fifo_reserve(fifo_t * f, FIFO_SIZE_T n)
{
   n *= f->item_size;

   if (f->begin == f->end)
      fifo_clear(f);

   while (1) {
      if (f->end + n <= f->allocation) {
         void *p = f->data.get() + f->end;

         f->end += n;
         return p;
      }
      if (f->begin > FIFO_MIN) {
         memmove(f->data.get(), f->data.get() + f->begin, f->end - f->begin);
         f->end -= f->begin;
         f->begin = 0;
         continue;
      }
      f->allocation += n;
      f->data.reinit(f->allocation);
   }
}

static void * fifo_write(fifo_t * f, FIFO_SIZE_T n, void const * data)
{
   void * s = fifo_reserve(f, n);
   if (data)
      memcpy(s, data, n * f->item_size);
   return s;
}

static void * fifo_read(fifo_t * f, FIFO_SIZE_T n, void * data)
{
   char * ret = f->data.get() + f->begin;
   n *= f->item_size;
   if (n > (FIFO_SIZE_T)(f->end - f->begin))
      return NULL;
   if (data)
      memcpy(data, ret, (size_t)n);
   f->begin += n;
   return ret;
}

static void fifo_create(fifo_t * f, FIFO_SIZE_T item_size)
{
   f->item_size = item_size;
   f->allocation = FIFO_MIN;
   f->data.reinit(f->allocation);
   fifo_clear(f);
}

typedef struct {
   size_t  size;
   ArrayOf<float> buffer;
   float   * ptr;
   float   store;
} filter_t;

static float comb_process(filter_t * p,  /* gcc -O2 will inline this */
      float const * input, float const * feedback, float const * hf_damping)
{
   float output = *p->ptr;
   p->store = output + (p->store - output) * *hf_damping;
   *p->ptr = *input + p->store * *feedback;
   filter_advance(p);
   return output;
}

static float allpass_process(filter_t * p,  /* gcc -O2 will inline this */
      float const * input)
{
   float output = *p->ptr;
   *p->ptr = *input + output * .5;
   filter_advance(p);
   return output - *input;
}

typedef struct {double b0, b1, a1, i1, o1;} one_pole_t;

static float one_pole_process(one_pole_t * p, float i0)
{
   float o0 = i0*p->b0 + p->i1*p->b1 - p->o1*p->a1;
   p->i1 = i0;
   return p->o1 = o0;
}

static const size_t /* Filter delay lengths in samples (44100Hz sample-rate) */
   comb_lengths[] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617},
   allpass_lengths[] = {225, 341, 441, 556}, stereo_adjust = 12;

typedef struct {
   filter_t comb   [array_length(comb_lengths)];
   filter_t allpass[array_length(allpass_lengths)];
   one_pole_t one_pole[2];
} filter_array_t;

static void filter_array_create(filter_array_t * p, double rate,
      double scale, double offset, double fc_highpass, double fc_lowpass)
{
   size_t i;
   double r = rate * (1 / 44100.); /* Compensate for actual sample-rate */

   for (i = 0; i < array_length(comb_lengths); ++i, offset = -offset)
   {
      filter_t * pcomb = &p->comb[i];
      pcomb->size = (size_t)(scale * r * (comb_lengths[i] + stereo_adjust * offset) + .5);
      pcomb->ptr = lsx_zalloc(pcomb->buffer, pcomb->size);
   }
   for (i = 0; i < array_length(allpass_lengths); ++i, offset = -offset)
   {
      filter_t * pallpass = &p->allpass[i];
      pallpass->size = (size_t)(r * (allpass_lengths[i] + stereo_adjust * offset) + .5);
      pallpass->ptr = lsx_zalloc(pallpass->buffer, pallpass->size);
   }
   { /* EQ: highpass */
      one_pole_t * q = &p->one_pole[0];
      q->a1 = -exp(-2 * M_PI * fc_highpass / rate);
      q->b0 = (1 - q->a1)/2, q->b1 = -q->b0;
   }
   { /* EQ: lowpass */
      one_pole_t * q = &p->one_pole[1];
      q->a1 = -exp(-2 * M_PI * fc_lowpass / rate);
      q->b0 = 1 + q->a1, q->b1 = 0;
   }
}

static void filter_array_process(filter_array_t * p,
      size_t length, float const * input, float * output,
      float const * feedback, float const * hf_damping, float const * gain)
{
   while (length--) {
      float out = 0, in = *input++;

      size_t i = array_length(comb_lengths) - 1;
      do out += comb_process(p->comb + i, &in, feedback, hf_damping);
      while (i--);

      i = array_length(allpass_lengths) - 1;
      do out = allpass_process(p->allpass + i, &out);
      while (i--);

      out = one_pole_process(&p->one_pole[0], out);
      out = one_pole_process(&p->one_pole[1], out);
      *output++ = out * *gain;
   }
}

typedef struct {
   float feedback {};
   float hf_damping {};
   float gain {};
   fifo_t input_fifo {};
   filter_array_t chan[2];
   ArrayOf<float> out[2];
} reverb_t;

static void reverb_create(reverb_t * p, double sample_rate_Hz,
      double wet_gain_dB,
      double room_scale,     /* % */
      double reverberance,   /* % */
      double hf_damping,     /* % */
      double pre_delay_ms,
      double stereo_depth,
      double tone_low,       /* % */
      double tone_high,      /* % */
      size_t buffer_size,
      float * * out)
{
   size_t i, delay = pre_delay_ms / 1000 * sample_rate_Hz + .5;
   double scale = room_scale / 100 * .9 + .1;
   double depth = stereo_depth / 100;
   double a =  -1 /  log(1 - /**/.3 /**/);           /* Set minimum feedback */
   double b = 100 / (log(1 - /**/.98/**/) * a + 1);  /* Set maximum feedback */
   double fc_highpass = midi_to_freq(72 - tone_low / 100 * 48);
   double fc_lowpass  = midi_to_freq(72 + tone_high/ 100 * 48);

   memset(p, 0, sizeof(*p));
   p->feedback = 1 - exp((reverberance - b) / (a * b));
   p->hf_damping = hf_damping / 100 * .3 + .2;
   p->gain = dB_to_linear(wet_gain_dB) * .015;
   fifo_create(&p->input_fifo, sizeof(float));
   memset(fifo_write(&p->input_fifo, delay, 0), 0, delay * sizeof(float));
   for (i = 0; i <= ceil(depth); ++i) {
      filter_array_create(p->chan + i, sample_rate_Hz, scale, i * depth, fc_highpass, fc_lowpass);
      out[i] = lsx_zalloc(p->out[i], buffer_size);
   }
}

static void reverb_process(reverb_t * p, size_t length)
{
   size_t i;
   for (i = 0; i < 2 && p->out[i]; ++i)
      filter_array_process(p->chan + i, length, (float *) fifo_read_ptr(&p->input_fifo), p->out[i].get(), &p->feedback, &p->hf_damping, &p->gain);
   fifo_read(&p->input_fifo, length, NULL);
}

