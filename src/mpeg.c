/*
** Copyright (C) 2019 Erik de Castro Lopo <erikd@mega-nerd.com>
** Copyright (C) 2019 Arthur Taylor <art@ified.ca>
**
** This program is free software ; you can redistribute it and/or modify
** it under the terms of the GNU Lesser General Public License as published by
** the Free Software Foundation ; either version 2.1 of the License, or
** (at your option) any later version.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY ; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU Lesser General Public License for more details.
**
** You should have received a copy of the GNU Lesser General Public License
** along with this program ; if not, write to the Free Software
** Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
*/

#include	"sfconfig.h"

#include	"sndfile.h"
#include	"common.h"

#if (ENABLE_EXPERIMENTAL_CODE && HAVE_LAME)

#include "mpeg.h"

#include <lame/lame.h>

/*
** TODO:
** - Reading/decoding support
** - Investigate write input clipping
*/

typedef struct
{	lame_t lamef ;
	unsigned char *block ;
	size_t len ;
	int max_frames ;
	struct
	{	float *l ;
		float *r ;
		} pcm ;
} MPEG_PRIVATE ;

typedef int (*mpeg_write_func) (MPEG_PRIVATE *, const void *, sf_count_t, sf_count_t) ;

static int	mpeg_close (SF_PRIVATE *psf) ;
static int	mpeg_init (SF_PRIVATE *psf) ;
static int	mpeg_write_header (SF_PRIVATE *psf, int calc_length) ;
static int	mpeg_command (SF_PRIVATE *psf, int command, void *data, int datasize) ;
static int	mpeg_byterate (SF_PRIVATE *psf) ;
static int	mpeg_encoder_construct (SF_PRIVATE *psf) ;
static void	mpeg_log_lame_config (SF_PRIVATE *psf, lame_t lamef) ;

static sf_count_t	mpeg_write_frames (SF_PRIVATE *psf, const void *ptr, sf_count_t len, mpeg_write_func write_func) ;
static sf_count_t	mpeg_write_s (SF_PRIVATE *psf, const short *ptr, sf_count_t len) ;
static sf_count_t	mpeg_write_i (SF_PRIVATE *psf, const int *ptr, sf_count_t len) ;
static sf_count_t	mpeg_write_f (SF_PRIVATE *psf, const float *ptr, sf_count_t len) ;
static sf_count_t	mpeg_write_d (SF_PRIVATE *psf, const double *ptr, sf_count_t len) ;

static int	mpeg_write_short_mono (MPEG_PRIVATE *pmpeg, const void *ptr, sf_count_t ntotal, sf_count_t nframes) ;
static int	mpeg_write_short_stereo (MPEG_PRIVATE *pmpeg, const void *ptr, sf_count_t ntotal, sf_count_t nframes) ;
static int	mpeg_write_int_mono (MPEG_PRIVATE *pmpeg, const void *ptr, sf_count_t ntotal, sf_count_t nframes) ;
static int	mpeg_write_int_stereo (MPEG_PRIVATE *pmpeg, const void *ptr, sf_count_t ntotal, sf_count_t nframes) ;
static int	mpeg_write_float_mono (MPEG_PRIVATE *pmpeg, const void *ptr, sf_count_t ntotal, sf_count_t nframes) ;
static int	mpeg_write_float_stereo (MPEG_PRIVATE *pmpeg, const void *ptr, sf_count_t ntotal, sf_count_t nframes) ;
static int	mpeg_write_float_mono_normal (MPEG_PRIVATE *pmpeg, const void *ptr, sf_count_t ntotal, sf_count_t nframes) ;
static int	mpeg_write_float_stereo_normal (MPEG_PRIVATE *pmpeg, const void *ptr, sf_count_t ntotal, sf_count_t nframes) ;
static int	mpeg_write_double_mono (MPEG_PRIVATE *pmpeg, const void *ptr, sf_count_t ntotal, sf_count_t nframes) ;
static int	mpeg_write_double_stereo (MPEG_PRIVATE *pmpeg, const void *ptr, sf_count_t ntotal, sf_count_t nframes) ;
static int	mpeg_write_double_mono_normal (MPEG_PRIVATE *pmpeg, const void *ptr, sf_count_t ntotal, sf_count_t nframes) ;
static int	mpeg_write_double_stereo_normal (MPEG_PRIVATE *pmpeg, const void *ptr, sf_count_t ntotal, sf_count_t nframes) ;

/*------------------------------------------------------------------------------
 * Public fuctions
 */

int
mpeg_open (SF_PRIVATE *psf)
{	MPEG_PRIVATE * pmpeg ;
	int error ;

	if (psf->file.mode == SFM_RDWR)
		return SFE_BAD_MODE_RW ;

	/* Initialize the codec */
	if ((error = mpeg_init (psf)))
		return error ;

	if (psf->file.mode == SFM_WRITE)
	{	/* ID3 support */
		psf->strings.flags = SF_STR_ALLOW_START ;

		psf->write_header = mpeg_write_header ;
		pmpeg = (MPEG_PRIVATE *) psf->codec_data ;

		/* Enable VBR by default */
		lame_set_VBR (pmpeg->lamef, 1) ;
		} ;

	psf->command =	mpeg_command ;
	psf->byterate =	mpeg_byterate ;

	return 0 ;
} /* mpeg_open */


/*------------------------------------------------------------------------------
 * Private static functions
 */

static int
mpeg_close (SF_PRIVATE *psf)
{	MPEG_PRIVATE* pmpeg = (MPEG_PRIVATE *) psf->codec_data ;
	int ret, len ;
	sf_count_t pos ;
	unsigned char *buffer ;

	if (psf->file.mode == SFM_WRITE)
	{	/* Magic number 7200 comes from a comment in lame.h */
		len = 7200 ;
		if (! (buffer = malloc (len)))
			return SFE_MALLOC_FAILED ;
		ret = lame_encode_flush (pmpeg->lamef, buffer, len) ;
		if (ret > 0)
			psf_fwrite (buffer, 1, ret, psf) ;

		/* Write an IDv1 trailer */
		ret = lame_get_id3v1_tag (pmpeg->lamef, 0, 0) ;
		if (ret > 0)
		{	if (ret > len)
			{	len = ret ;
				free (buffer) ;
				if (! (buffer = malloc (len)))
					return SFE_MALLOC_FAILED ;
				} ;
			psf_log_printf (psf, "  Writing ID3v1 trailer.\n") ;
			lame_get_id3v1_tag (pmpeg->lamef, buffer, len) ;
			psf_fwrite (buffer, 1, ret, psf) ;
			} ;

		/*
		** If possible, seek back and write the LAME/XING/Info headers. This
		** contains information about the whole file and a seek table, and can
		** only be written after encodeing.
		**
		** If enabled, Lame wrote an empty header at the begining of the data
		** that we now fill in.
		*/
		ret = lame_get_lametag_frame (pmpeg->lamef, 0, 0) ;
		if (ret > 0)
		{	if (ret > len)
			{	len = ret ;
				free (buffer) ;
				if (! (buffer = malloc (len)))
					return SFE_MALLOC_FAILED ;
				} ;
			psf_log_printf (psf, "  Writing LAME info header at offset %d, %d bytes.\n",
				psf->dataoffset, len) ;
			lame_get_lametag_frame (pmpeg->lamef, buffer, len) ;
			pos = psf_ftell (psf) ;
			if (psf_fseek (psf, psf->dataoffset, SEEK_SET) == psf->dataoffset)
			{	psf_fwrite (buffer, 1, ret, psf) ;
				psf_fseek (psf, pos, SEEK_SET) ;
				} ;
			} ;
		free (buffer) ;

		free (pmpeg->block) ;
		pmpeg->block = NULL ;

		if (pmpeg->lamef)
		{	lame_close (pmpeg->lamef) ;
			pmpeg->lamef = NULL ;
			} ;

		free (pmpeg->pcm.l) ;
		pmpeg->pcm.l = NULL ;
		free (pmpeg->pcm.r) ;
		pmpeg->pcm.r = NULL ;
		} ;

	return 0 ;
} /* mpeg_close */


/*
** Initialize the codec-specific aspect. The intent is that this function could
** be exported in future to provide mpeg layer 3 codec support to other major
** formats.
*/
static int
mpeg_init (SF_PRIVATE *psf)
{	MPEG_PRIVATE* pmpeg = NULL ;

	if (psf->file.mode == SFM_RDWR)
		return SFE_BAD_MODE_RW ;

	psf->codec_data = pmpeg = calloc (1, sizeof (MPEG_PRIVATE)) ;
	if (!pmpeg)
		return SFE_MALLOC_FAILED ;

	if (psf->file.mode == SFM_READ)
	{	return mpeg_dec_open (psf) ;
		} ;

	if (psf->file.mode == SFM_WRITE)
	{	if (psf->sf.channels < 1 || psf->sf.channels > 2)
			return SFE_BAD_OPEN_FORMAT ;

		if (! (pmpeg->lamef = lame_init ()))
			return SFE_MALLOC_FAILED ;

		lame_set_in_samplerate (pmpeg->lamef, psf->sf.samplerate) ;
		lame_set_num_channels (pmpeg->lamef, psf->sf.channels) ;
		if (lame_set_out_samplerate (pmpeg->lamef, psf->sf.samplerate) < 0)
			/* TODO */ return SFE_BAD_OPEN_FORMAT ;

		lame_set_quality (pmpeg->lamef, 2) ;
		lame_set_write_id3tag_automatic (pmpeg->lamef, 0) ;

		if (psf->is_pipe)
		{	/* Can't seek back, so force disable Xing/Lame/Info header. */
			lame_set_bWriteVbrTag (pmpeg->lamef, 0) ;
			} ;

		psf->write_short	= mpeg_write_s ;
		psf->write_int		= mpeg_write_i ;
		psf->write_float	= mpeg_write_f ;
		psf->write_double	= mpeg_write_d ;

		psf->datalength = 0 ;
		psf->dataoffset = 0 ;
		} ;

	psf->sf.seekable = 0 ;
	psf->codec_close	= mpeg_close ;

	return 0 ;
} /* mpeg_init */

static int
mpeg_encoder_construct (SF_PRIVATE *psf)
{	MPEG_PRIVATE *pmpeg = (MPEG_PRIVATE *) psf->codec_data ;

	if (lame_init_params (pmpeg->lamef) < 0)
	{	psf_log_printf (psf, "Failed to initialize lame encoder!\n") ;
		return SFE_INTERNAL ;
		} ;

	psf_log_printf (psf, "Initialized LAME encoder.\n") ;
	mpeg_log_lame_config (psf, pmpeg->lamef) ;

	pmpeg->len = lame_get_framesize (pmpeg->lamef) * 4 ;
	if (! (pmpeg->block = malloc (pmpeg->len)))
		return SFE_MALLOC_FAILED ;

	pmpeg->max_frames = lame_get_maximum_number_of_samples (
			pmpeg->lamef, pmpeg->len) ;

	pmpeg->pcm.l = (float *) malloc (sizeof (float) * pmpeg->max_frames) ;
	if (pmpeg->pcm.l == NULL)
		return SFE_MALLOC_FAILED ;

	if (psf->sf.channels == 2)
	{	pmpeg->pcm.r = (float *) malloc (sizeof (float) * pmpeg->max_frames) ;
		if (pmpeg->pcm.r == NULL)
			return SFE_MALLOC_FAILED ;
		} ;

	return 0 ;
} /* mpeg_encoder_construct */

static void
mpeg_log_lame_config (SF_PRIVATE *psf, lame_t lamef)
{	const char *version ;
	const char *chn_mode ;

	switch (lame_get_version (lamef))
	{	case 0 : version = "2" ; break ;
		case 1 : version = "1" ; break ;
		case 2 : version = "2.5" ; break ;
		default : version = "unknown!?" ; break ;
		} ;
	switch (lame_get_mode (lamef))
	{	case STEREO : chn_mode = "stereo" ; break ;
		case JOINT_STEREO : chn_mode = "joint-stereo" ; break ;
		case MONO : chn_mode = "mono" ; break ;
		default : chn_mode = "unknown!?" ; break ;
		} ;
	psf_log_printf (psf, "  MPEG Version      : %s\n", version) ;
	psf_log_printf (psf, "  Channel mode      : %s\n", chn_mode) ;
	psf_log_printf (psf, "  Samplerate        : %d\n", lame_get_out_samplerate (lamef)) ;
	psf_log_printf (psf, "  Encoder mode      : ") ;
	switch (lame_get_VBR (lamef))
	{	case vbr_off :
			psf_log_printf (psf, "CBR\n") ;
			psf_log_printf (psf, "  Compression ratio : %d\n", lame_get_compression_ratio (lamef)) ;
			psf_log_printf (psf, "  Bitrate           : %d kbps\n", lame_get_brate (lamef)) ;
			break ;

		case vbr_mt :
		case vbr_default :
			psf_log_printf (psf, "VBR\n") ;
			psf_log_printf (psf, "  Quality           : %d\n", lame_get_VBR_q (lamef)) ;
			break ;

		default:
			psf_log_printf (psf, "Unknown!? (%d)\n", lame_get_VBR (lamef)) ;
			break ;
		} ;

	psf_log_printf (psf, "  Encoder delay     : %d\n", lame_get_encoder_delay (lamef)) ;
	psf_log_printf (psf, "  Write INFO header : %d\n", lame_get_bWriteVbrTag (lamef)) ;
} /* mpeg_log_lame_config */

static int
mpeg_write_header (SF_PRIVATE *psf, int UNUSED (calc_length))
{	MPEG_PRIVATE *pmpeg = (MPEG_PRIVATE *) psf->codec_data ;
	unsigned char *id3v2_buffer ;
	int i, id3v2_size ;

	if (psf->have_written)
		return 0 ;

	if (!pmpeg->len && (psf->error = mpeg_encoder_construct (psf)))
		return 0 ;

	if (psf_fseek (psf, 0, SEEK_SET) != 0)
		return SFE_NOT_SEEKABLE ;

	/* Safe to call multiple times. */
	id3tag_init (pmpeg->lamef) ;

	for (i = 0 ; i < SF_MAX_STRINGS ; i++)
	{	switch (psf->strings.data [i].type)
		{	case SF_STR_TITLE :
				id3tag_set_title (pmpeg->lamef, psf->strings.storage + psf->strings.data [i].offset) ;
				break ;

			case SF_STR_ARTIST :
				id3tag_set_artist (pmpeg->lamef, psf->strings.storage + psf->strings.data [i].offset) ;
				break ;

			case SF_STR_ALBUM :
				id3tag_set_album (pmpeg->lamef, psf->strings.storage + psf->strings.data [i].offset) ;
				break ;

			case SF_STR_DATE :
				id3tag_set_year (pmpeg->lamef, psf->strings.storage + psf->strings.data [i].offset) ;
				break ;

			case SF_STR_COMMENT :
				id3tag_set_comment (pmpeg->lamef, psf->strings.storage + psf->strings.data [i].offset) ;
				break ;

			case SF_STR_GENRE :
				id3tag_set_genre (pmpeg->lamef, psf->strings.storage + psf->strings.data [i].offset) ;
				break ;

			case SF_STR_TRACKNUMBER :
				id3tag_set_track (pmpeg->lamef, psf->strings.storage + psf->strings.data [i].offset) ;
				break ;

			default:
				break ;
			} ;
		} ;

	/* The header in this case is the ID3v2 tag header. */
	id3v2_size = lame_get_id3v2_tag (pmpeg->lamef, 0, 0) ;
	if (id3v2_size > 0)
	{	psf_log_printf (psf, "Writing ID3v2 header.\n") ;
		if (! (id3v2_buffer = malloc (id3v2_size)))
			return SFE_MALLOC_FAILED ;
		lame_get_id3v2_tag (pmpeg->lamef, id3v2_buffer, id3v2_size) ;
		psf_fwrite (id3v2_buffer, 1, id3v2_size, psf) ;
		psf->dataoffset = id3v2_size ;
		free (id3v2_buffer) ;
		} ;

	return 0 ;
} /* mpeg_write_header */

static int
mpeg_command (SF_PRIVATE *psf, int command, void *data, int datasize)
{	MPEG_PRIVATE *pmpeg = (MPEG_PRIVATE *) psf->codec_data ;
	float quality ;

	switch (command)
	{	case SFC_SET_COMPRESSION_LEVEL :
			if (data == NULL || datasize != sizeof (double))
				return SF_FALSE ;
			if (psf->file.mode != SFM_WRITE || pmpeg->len)
				return SF_FALSE ;

			quality = *(float *) data ;
			psf_log_printf (psf, "%s : Setting SFC_SET_COMPRESSION_LEVEL to %f.\n", __func__, quality) ;
			if (lame_get_VBR (pmpeg->lamef) == vbr_off)
			{	/* Constant bitrate mode. Set bitrate. */
				if (lame_get_version (pmpeg->lamef) == 1)
				{	/* MPEG-1. Available bitrates are 32-320 */
					quality = 320.0 - (quality * 288.0) ;
					}
				else
				{	/* MPEG-2/2.5. Available bitrates are 8-160 */
					quality = 160.0 - (quality * 152.0) ;
					} ;
				if (!lame_set_brate (pmpeg->lamef, (int) quality))
					return SF_TRUE ;
				}
			else
			{	/* Variable bitrate mode. Set quality. */
				if (!lame_set_VBR_quality (pmpeg->lamef, quality * 10.0))
					return SF_TRUE ;
				} ;

			// fall-through
		default :
			return SF_FALSE ;
		} ;

	return SF_FALSE ;
} /* mpeg_command */

static int
mpeg_byterate (SF_PRIVATE *psf)
{	MPEG_PRIVATE *pmpeg = (MPEG_PRIVATE *) psf->codec_data ;
	if (psf->file.mode == SFM_WRITE)
	{	/* TODO: For VBR this returns the minimum byterate. */
		return lame_get_brate (pmpeg->lamef) / 8 ;
		}

	return 0 ;
} /* mpeg_byterate */


static sf_count_t
mpeg_write_frames (SF_PRIVATE *psf, const void *ptr, sf_count_t frames, mpeg_write_func write_func)
{	MPEG_PRIVATE *pmpeg = (MPEG_PRIVATE*) psf->codec_data ;
	sf_count_t ntotal, nframes, nwritten ;
	int ret ;

	if (!pmpeg->len && (psf->error = mpeg_encoder_construct (psf)))
		return 0 ;

	for (ntotal = 0 ; ntotal < frames ; ntotal += nframes)
	{	nframes = SF_MIN ((int) (frames - ntotal), pmpeg->max_frames) ;
		ret = write_func (pmpeg, ptr, ntotal, nframes) ;
		if (ret < 0)
		{	psf_log_printf (psf, "lame_encode_buffer returned %d\n", ret) ;
			break ;
			} ;

		if (ret)
		{	nwritten = psf_fwrite (pmpeg->block, 1, ret, psf) ;
			if (nwritten != ret)
			{	psf_log_printf (psf, "*** Warning : short write (%d != %d).\n", nwritten, ret) ;
				} ;
			} ;
		} ;

	return ntotal ;
} /* mpeg_write_frames */

static int
mpeg_write_short_mono (MPEG_PRIVATE *pmpeg, const void *ptr, sf_count_t ntotal, sf_count_t nframes)
{	return lame_encode_buffer
		(pmpeg->lamef, (const short *) ptr + ntotal, NULL, nframes, pmpeg->block, pmpeg->len) ;
} /* mpeg_write_short_mono */

static int
mpeg_write_short_stereo (MPEG_PRIVATE *pmpeg, const void *ptr, sf_count_t ntotal, sf_count_t nframes)
{	return lame_encode_buffer_interleaved
		(pmpeg->lamef, (const short *) ptr + ntotal, nframes, pmpeg->block, pmpeg->len) ;
} /* mpeg_write_short_stereo */

static int
mpeg_write_int_mono (MPEG_PRIVATE *pmpeg, const void *ptr, sf_count_t ntotal, sf_count_t nframes)
{	return lame_encode_buffer_int
		(pmpeg->lamef, (const int *) ptr + ntotal, NULL, nframes, pmpeg->block, pmpeg->len) ;
} /* mpeg_write_int_mono */

static int
mpeg_write_int_stereo (MPEG_PRIVATE *pmpeg, const void *ptr, sf_count_t ntotal, sf_count_t nframes)
{	return lame_encode_buffer_interleaved_int
		(pmpeg->lamef, (const int *) ptr + ntotal, nframes, pmpeg->block, pmpeg->len) ;
} /* mpeg_write_int_stereo */

static int
mpeg_write_float_mono (MPEG_PRIVATE *pmpeg, const void *ptr, sf_count_t ntotal, sf_count_t nframes)
{	return lame_encode_buffer_float
		(pmpeg->lamef, (const float *) ptr + ntotal, NULL, nframes, pmpeg->block, pmpeg->len) ;
} /* mpeg_write_float_mono */

static int
mpeg_write_float_stereo (MPEG_PRIVATE *pmpeg, const void *ptr, sf_count_t ntotal, sf_count_t nframes)
{	const float *fptr = (const float *) ptr + ntotal ;
	sf_count_t n ;

	/* No lame function for non-normalized interleaved float. */
	for (n = 0 ; n < nframes ; n++)
	{	pmpeg->pcm.l [n] = *fptr++ ;
		pmpeg->pcm.r [n] = *fptr++ ;
		} ;

	return lame_encode_buffer_float
		(pmpeg->lamef, pmpeg->pcm.l, pmpeg->pcm.r, nframes, pmpeg->block, pmpeg->len) ;
} /* mpeg_write_float_stereo */

static int
mpeg_write_float_mono_normal (MPEG_PRIVATE *pmpeg, const void *ptr, sf_count_t ntotal, sf_count_t nframes)
{	return lame_encode_buffer_ieee_float
		(pmpeg->lamef, (const float *) ptr + ntotal, NULL, nframes, pmpeg->block, pmpeg->len) ;
} /* mpeg_write_float_mono_normal */

static int
mpeg_write_float_stereo_normal (MPEG_PRIVATE *pmpeg, const void *ptr, sf_count_t ntotal, sf_count_t nframes)
{	return lame_encode_buffer_interleaved_ieee_float
		(pmpeg->lamef, (const float *) ptr + ntotal, nframes, pmpeg->block, pmpeg->len) ;
} /* mpeg_write_float_stereo_normal */

static int
mpeg_write_double_mono (MPEG_PRIVATE *pmpeg, const void *ptr, sf_count_t ntotal, sf_count_t nframes)
{	const double *dptr = (const double *) ptr + ntotal ;
	sf_count_t n ;

	/* No lame function for non-normalized double. */
	for (n = 0 ; n < nframes ; n++)
		pmpeg->pcm.l [n] = *dptr++ ;

	return lame_encode_buffer_float
		(pmpeg->lamef, pmpeg->pcm.l, NULL, nframes, pmpeg->block, pmpeg->len) ;
} /* mpeg_write_double_mono */

static int
mpeg_write_double_stereo (MPEG_PRIVATE *pmpeg, const void *ptr, sf_count_t ntotal, sf_count_t nframes)
{	const double *dptr = (const double *) ptr + ntotal ;
	sf_count_t n ;

	/* No lame function for non-normalized interleaved double. */
	for (n = 0 ; n < nframes ; n++)
	{	pmpeg->pcm.l [n] = *dptr++ ;
		pmpeg->pcm.r [n] = *dptr++ ;
		} ;

	return lame_encode_buffer_float
		(pmpeg->lamef, pmpeg->pcm.l, pmpeg->pcm.r, nframes, pmpeg->block, pmpeg->len) ;
} /* mpeg_write_double_stereo */

static int
mpeg_write_double_mono_normal (MPEG_PRIVATE *pmpeg, const void *ptr, sf_count_t ntotal, sf_count_t nframes)
{	return lame_encode_buffer_ieee_double
		(pmpeg->lamef, (const double *) ptr + ntotal, NULL, nframes, pmpeg->block, pmpeg->len) ;
} /* mpeg_write_double_mono_normal */

static int
mpeg_write_double_stereo_normal (MPEG_PRIVATE *pmpeg, const void *ptr, sf_count_t ntotal, sf_count_t nframes)
{	return lame_encode_buffer_interleaved_ieee_double
		(pmpeg->lamef, (const double *) ptr + ntotal, nframes, pmpeg->block, pmpeg->len) ;
} /* mpeg_write_double_stereo_normal */

static sf_count_t
mpeg_write_s (SF_PRIVATE *psf, const short *ptr, sf_count_t len)
{	if (psf->sf.channels == 2)
		return 2 * mpeg_write_frames (psf, ptr, len / 2, mpeg_write_short_stereo) ;
	else
		return mpeg_write_frames (psf, ptr, len, mpeg_write_short_mono) ;
} /* mpeg_write_s */

static sf_count_t
mpeg_write_i (SF_PRIVATE *psf, const int *ptr, sf_count_t len)
{	if (psf->sf.channels == 2)
		return 2 * mpeg_write_frames (psf, ptr, len / 2, mpeg_write_int_stereo) ;
	else
		return mpeg_write_frames (psf, ptr, len, mpeg_write_int_mono) ;
} /* mpeg_write_i */

static sf_count_t
mpeg_write_f (SF_PRIVATE *psf, const float *ptr, sf_count_t len)
{	if (psf->sf.channels == 2)
		return 2 * mpeg_write_frames (psf, ptr, len / 2,
			psf->norm_float ? mpeg_write_float_stereo_normal : mpeg_write_float_stereo) ;
	else
		return mpeg_write_frames (psf, ptr, len,
			psf->norm_float ? mpeg_write_float_mono_normal : mpeg_write_float_mono) ;
} /* mpeg_write_f */

static sf_count_t
mpeg_write_d (SF_PRIVATE *psf, const double *ptr, sf_count_t len)
{	if (psf->sf.channels == 2)
		return 2 * mpeg_write_frames (psf, ptr, len / 2,
			psf->norm_double ? mpeg_write_double_stereo_normal : mpeg_write_double_stereo) ;
	else
		return mpeg_write_frames (psf, ptr, len,
			psf->norm_double ? mpeg_write_double_mono_normal : mpeg_write_double_mono) ;
} /* mpeg_write_d */

#else /* ENABLE_EXPERIMENTAL_CODE && HAVE_LAME */

int mpeg_open (PSF_PRIVATE *psf)
{
	psf_log_printf (psf, "This version of libsndfile was compiled without MP3 support.\n") ;
	return SFE_UNIMPLEMENTED ;
} /* mpeg_open */

#endif