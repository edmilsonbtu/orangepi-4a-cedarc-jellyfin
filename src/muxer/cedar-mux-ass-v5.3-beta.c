#include "cedar-ass-nv12-v2.h"
#include <unistd.h>
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include "memoryAdapter.h"
#include "veAdapter.h"
#include "veInterface.h"
#include "sc_interface.h"
#include "vdecoder.h"
#include "vencoder.h"

#define ALIGN16(x) (((x) + 15) & ~15)

static int video_width = 1920;
static int video_height = 1080;
static int storage_width = 1920;
static int storage_height = 1088;

#define WIDTH storage_width
#define HEIGHT storage_height
/* Decoder keeps ALIGN16 dimensions; only 1366x768 uses a padded encoder. */
static int encoder_width = 1920;
static int encoder_height = 1088;
#define ENC_WIDTH encoder_width
#define ENC_HEIGHT encoder_height

static int read_dimension(const char *name, int fallback)
{
    const char *value = getenv(name);
    if (!value || !*value)
        return fallback;

    char *end = NULL;
    errno = 0;
    long n = strtol(value, &end, 10);

    if (errno || end == value || *end != '\0' ||
        n < 16 || n > 4096 || (n & 1)) {
        fprintf(stderr, "ERRO: %s invalido\n", name);
        return -1;
    }

    return (int)n;
}
#define MAX_PACKET (4u*1024u*1024u)
#define MAX_PACKETS 100000
#define MAX_CALLS 1000000
#define MAX_SECONDS 900.0

static struct timespec start;
static int packets, frames, encoded, calls, errors;

/* V4.1: diagnostico experimental do SBM. */
static unsigned long long sbm_retry_count = 0;
static unsigned long long sbm_wait_count = 0;
static unsigned long long sbm_recovery_count = 0;
static unsigned long long sbm_max_retries = 0;
static int last_progress_packet, last_progress_frames;

/* V5.1: controle de recuperacao preventiva. */
static double v53_last_progress_sec = 0.0;
static int v5_recovery_armed = 1;
static unsigned long long v5_recovery_attempts = 0;
static unsigned long long v5_recovery_successes = 0;
static unsigned long long v5_recovery_timeouts = 0;

static int result_counts[16];
static VideoEncoder *encoder;
static FILE *out;
static unsigned char *codec_header;
static size_t codec_header_size;
static int64_t cut_pts = -1;
static int skipped_frames = 0;



static double seconds(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)(now.tv_sec-start.tv_sec)+(now.tv_nsec-start.tv_nsec)/1e9;
}



/* SPS_WRITER_V781: infraestrutura de escrita RBSP. */
typedef struct {
    unsigned char data[4096];
    size_t bit;
    int error;
} SpsWriter;

static void sps_write_bits(SpsWriter *w, unsigned value, unsigned count)
{
    if (count > 32 || w->bit > sizeof(w->data) * 8 ||
        count > sizeof(w->data) * 8 - w->bit) {
        w->error = 1;
        return;
    }

    for (unsigned i = 0; i < count; i++) {
        unsigned shift = count - 1 - i;
        unsigned bit = (value >> shift) & 1u;
        size_t pos = w->bit++;

        if (bit)
            w->data[pos / 8] |= (unsigned char)(1u << (7 - pos % 8));
    }
}

static void sps_write_ue(SpsWriter *w, unsigned value)
{
    if (value == UINT32_MAX) {
        w->error = 1;
        return;
    }

    unsigned code = value + 1;
    unsigned bits = 0;

    for (unsigned x = code; x; x >>= 1)
        bits++;

    sps_write_bits(w, 0, bits - 1);
    sps_write_bits(w, code, bits);
}

static void sps_write_trailing(SpsWriter *w)
{
    sps_write_bits(w, 1, 1);

    while (!w->error && (w->bit & 7))
        sps_write_bits(w, 0, 1);
}

/* SPS_PARSER_V78: diagnostico somente, sem alterar o bitstream. */
typedef struct {
    const unsigned char *data;
    size_t size;
    size_t bit;
    int error;
} SpsReader;

static unsigned sps_bits(SpsReader *r, unsigned count)
{
    unsigned value = 0;
    if (count > 32 || r->bit > r->size * 8 ||
        count > r->size * 8 - r->bit) {
        r->error = 1;
        return 0;
    }
    for (unsigned i = 0; i < count; i++) {
        size_t p = r->bit++;
        value = (value << 1) |
            ((r->data[p / 8] >> (7 - (p % 8))) & 1u);
    }
    return value;
}

static unsigned sps_ue(SpsReader *r)
{
    unsigned zeros = 0;
    while (!r->error && sps_bits(r, 1) == 0) {
        if (++zeros > 30) {
            r->error = 1;
            return 0;
        }
    }
    if (r->error) return 0;
    return ((1u << zeros) - 1u) + sps_bits(r, zeros);
}

static int sps_diagnostic(const unsigned char *nal, size_t n)
{
    unsigned char rbsp[4096];
    size_t used = 0;
    unsigned zeros = 0;

    if (n < 5 || n > sizeof(rbsp) ||
        (nal[0] & 0x1f) != 7)
        return -1;

    for (size_t i = 1; i < n; i++) {
        unsigned char c = nal[i];

        if (zeros >= 2 && c == 3) {
            if (i + 1 >= n || nal[i + 1] > 3)
                return -1;
            zeros = 0;
            continue;
        }

        rbsp[used++] = c;
        zeros = (c == 0) ? zeros + 1 : 0;
    }

    SpsReader r = {rbsp, used, 0, 0};

    unsigned profile = sps_bits(&r, 8);
    sps_bits(&r, 8);
    unsigned level = sps_bits(&r, 8);
    sps_ue(&r);

    unsigned chroma = 1;
    unsigned separate = 0;

    if (profile == 100 || profile == 110 ||
        profile == 122 || profile == 244 ||
        profile == 44 || profile == 83 ||
        profile == 86 || profile == 118 ||
        profile == 128 || profile == 138 ||
        profile == 139 || profile == 134 ||
        profile == 135) {
        chroma = sps_ue(&r);
        if (chroma > 3) return -1;
        if (chroma == 3)
            separate = sps_bits(&r, 1);

        sps_ue(&r);
        sps_ue(&r);
        sps_bits(&r, 1);

        if (sps_bits(&r, 1)) {
            /* Scaling lists require a separate parser. */
            fprintf(stderr, "SPS_DIAG_SCALING_LIST_UNSUPPORTED\n");
            return -1;
        }
    }

    sps_ue(&r);
    unsigned poc = sps_ue(&r);

    if (poc == 0) {
        sps_ue(&r);
    } else if (poc == 1) {
        sps_bits(&r, 1);
        sps_ue(&r);
        sps_ue(&r);
        unsigned cycles = sps_ue(&r);
        if (cycles > 255) return -1;
        for (unsigned i = 0; i < cycles; i++)
            sps_ue(&r);
    } else if (poc != 2) {
        return -1;
    }

    sps_ue(&r);
    sps_bits(&r, 1);

    unsigned mbs_w = sps_ue(&r) + 1;
    unsigned map_h = sps_ue(&r) + 1;
    unsigned frame_only = sps_bits(&r, 1);

    if (!frame_only)
        sps_bits(&r, 1);

    sps_bits(&r, 1);
    sps_bits(&r, 1);

    unsigned left = 0, right = 0, top = 0, bottom = 0;

    if (sps_bits(&r, 1)) {
        left = sps_ue(&r);
        right = sps_ue(&r);
        top = sps_ue(&r);
        bottom = sps_ue(&r);
    }

    if (r.error || !mbs_w || !map_h ||
        mbs_w > 256 || map_h > 256)
        return -1;

    unsigned crop_x = 1, crop_y = 2 - frame_only;

    if (chroma != 0 && !separate) {
        unsigned sub_w = chroma == 3 ? 1 : 2;
        unsigned sub_h = chroma == 1 ? 2 : 1;
        crop_x = sub_w;
        crop_y = sub_h * (2 - frame_only);
    }

    unsigned coded_w = mbs_w * 16;
    unsigned coded_h = map_h * 16 * (2 - frame_only);
    unsigned cropped_w = (left + right) * crop_x;
    unsigned cropped_h = (top + bottom) * crop_y;

    if (cropped_w >= coded_w || cropped_h >= coded_h)
        return -1;

    unsigned visible_w = coded_w - cropped_w;
    unsigned visible_h = coded_h - cropped_h;

    fprintf(stderr,
        "SPS_DIAG profile=%u level=%u coded=%ux%u "
        "visible=%ux%u crop=%u,%u,%u,%u expected=%dx%d\n",
        profile, level, coded_w, coded_h,
        visible_w, visible_h,
        left, right, top, bottom,
        video_width, video_height);

    return 0;
}


/* SPS_DYNAMIC_V781: gerador restrito, somente diagnostico. */
static int sps_generate_crop(const unsigned char *nal, size_t n,
                             unsigned char *dst, size_t cap,
                             size_t *dst_size)
{
    unsigned char rbsp[4096] = {0};
    size_t used = 0;
    unsigned zeros = 0;

    if (!nal || !dst || !dst_size || n < 5 ||
        n > sizeof(rbsp) || (nal[0] & 0x1f) != 7)
        return -1;

    for (size_t i = 1; i < n; i++) {
        unsigned char c = nal[i];

        if (zeros >= 2 && c == 3) {
            if (i + 1 >= n || nal[i + 1] > 3)
                return -1;
            zeros = 0;
            continue;
        }

        rbsp[used++] = c;
        zeros = c == 0 ? zeros + 1 : 0;
    }

    SpsReader r = {rbsp, used, 0, 0};

    unsigned profile = sps_bits(&r, 8);
    sps_bits(&r, 8);
    unsigned level = sps_bits(&r, 8);
    sps_ue(&r); /* seq_parameter_set_id */
    sps_ue(&r); /* log2_max_frame_num_minus4 */

    unsigned poc = sps_ue(&r);

    if (profile != 66 || level != 40 || poc != 0)
        return -1;

    sps_ue(&r); /* log2_max_pic_order_cnt_lsb_minus4 */
    sps_ue(&r); /* max_num_ref_frames */
    sps_bits(&r, 1); /* gaps_in_frame_num_value_allowed_flag */

    unsigned mbs_w = sps_ue(&r) + 1;
    unsigned map_h = sps_ue(&r) + 1;
    unsigned frame_only = sps_bits(&r, 1);

    if (frame_only != 1)
        return -1;

    sps_bits(&r, 1); /* direct_8x8_inference_flag */

    size_t crop_position = r.bit;
    unsigned old_crop = sps_bits(&r, 1);

    if (r.error || old_crop)
        return -1;

    size_t vui_position = r.bit;
    unsigned vui = sps_bits(&r, 1);

    fprintf(stderr,
            "SPS_V790_TRAILING_DIAG "
            "profile=%u level=%u "
            "coded=%ux%u "
            "rbsp_bits=%zu consumed_bits=%zu "
            "remaining_bits=%zu vui=%u error=%d\\n",
            profile, level,
            mbs_w * 16, map_h * 16,
            used * 8, r.bit,
            used * 8 >= r.bit ? used * 8 - r.bit : 0,
            vui, r.error);

    if (r.error || vui)
        return -1;

    size_t remaining = used * 8 - r.bit;

    if (remaining < 1 || remaining > 8)
        return -1;

    if (sps_bits(&r, 1) != 1 || r.error)
        return -1;

    while (r.bit < used * 8) {
        if (sps_bits(&r, 1) != 0 || r.error)
            return -1;
    }

    unsigned coded_w = mbs_w * 16;
    unsigned coded_h = map_h * 16;

    if (coded_w != (unsigned)ENC_WIDTH ||
        coded_h != (unsigned)ENC_HEIGHT ||
        video_width <= 0 || video_height <= 0 ||
        (unsigned)video_width > coded_w ||
        (unsigned)video_height > coded_h)
        return -1;

    unsigned dw = coded_w - (unsigned)video_width;
    unsigned dh = coded_h - (unsigned)video_height;

    if ((dw & 1) || (dh & 1))
        return -1;

    unsigned right = dw / 2;
    unsigned bottom = dh / 2;

    SpsWriter w = {0};

    SpsReader copy = {rbsp, used, 0, 0};

    while (copy.bit < crop_position && !copy.error)
        sps_write_bits(&w, sps_bits(&copy, 1), 1);

    if (copy.error || w.error)
        return -1;

    sps_write_bits(&w, 1, 1); /* frame_cropping_flag */
    sps_write_ue(&w, 0);     /* left */
    sps_write_ue(&w, right);
    sps_write_ue(&w, 0);     /* top */
    sps_write_ue(&w, bottom);

    /* Preservar vui_parameters_present_flag = 0. */
    sps_write_bits(&w,
                   (rbsp[vui_position / 8] >>
                    (7 - vui_position % 8)) & 1u, 1);

    sps_write_trailing(&w);

    if (w.error || (w.bit & 7))
        return -1;

    size_t bytes = w.bit / 8;
    size_t out = 0;
    unsigned zero_run = 0;

    if (cap < 1)
        return -1;

    dst[out++] = nal[0];

    for (size_t i = 0; i < bytes; i++) {
        unsigned char c = w.data[i];

        if (zero_run >= 2 && c <= 3) {
            if (out >= cap)
                return -1;
            dst[out++] = 3;
            zero_run = 0;
        }

        if (out >= cap)
            return -1;

        dst[out++] = c;
        zero_run = c == 0 ? zero_run + 1 : 0;
    }

    *dst_size = out;

    fprintf(stderr,
            "SPS_DYNAMIC_V781 coded=%ux%u visible=%dx%d "
            "right=%u bottom=%u bytes=%zu\n",
            coded_w, coded_h, video_width, video_height,
            right, bottom, out);

    return 0;
}

/* SPS replacement is deliberately restricted to the exact known T527 encoder SPS. */
static int write_sps_pps(VideoEncoder *enc, FILE *f) {
    if (video_width != 1920 || video_height != 1080) {
        fprintf(stderr,
                "SPS_DIAGNOSTIC_ONLY=%dx%d\n",
                video_width, video_height);
    }
    VencHeaderData h = {0};
    static const unsigned char old_sps[] = {0x67,0x42,0x00,0x28,0x96,0x54,0x03,0xc0,0x11,0x22};
    static const unsigned char new_sps[] = {0x67,0x42,0x00,0x28,0x96,0x54,0x03,0xc0,0x11,0x2f,0x2a};
    if (VideoEncGetParameter(enc,VENC_IndexParamH264SPSPPS,&h)!=0 || !h.pBuffer || !h.nLength) {
        fprintf(stderr,"ERRO: SPS/PPS indisponivel\n"); return -1;
    }
    fprintf(stderr, "SPS_RAW_LENGTH=%u\n",
            (unsigned)h.nLength);

    {
        char hex[257];
        unsigned count = (unsigned)h.nLength;

        if (count > 128)
            count = 128;

        for (unsigned i = 0; i < count; i++)
            snprintf(hex + i * 2, 3, "%02x",
                     ((unsigned char *)h.pBuffer)[i]);

        hex[count * 2] = '\0';

        fprintf(stderr, "SPS_RAW_HEX=%s\n", hex);
    }

    const unsigned char *b=(const unsigned char *)h.pBuffer;
    size_t n=(size_t)h.nLength, pos=n, end=n;
    for (size_t i=0;i+4<=n;i++) {
        size_t sc=0;
        if (b[i]==0 && b[i+1]==0 && b[i+2]==0 && b[i+3]==1) sc=4;
        else if (b[i]==0 && b[i+1]==0 && b[i+2]==1) sc=3;
        if (sc && i+sc<n && (b[i+sc]&0x1f)==7) {
            pos=i+sc;
            for (size_t j=pos+1;j+3<=n;j++) {
                if (b[j]==0 && b[j+1]==0 && (b[j+2]==1 ||
                    (j+3<n && b[j+2]==0 && b[j+3]==1))) {end=j;break;}
            }
            break;
        }
    }

    if (pos < n && end > pos) {
        if (sps_diagnostic(b + pos, end - pos) != 0)
            fprintf(stderr, "SPS_DIAG_PARSE_FAILED\n");
    } else {
        fprintf(stderr, "SPS_DIAG_NAL_NOT_FOUND\n");
    }


    if (video_width == 1920 && video_height == 1080) {
        unsigned char generated[4096];
        size_t generated_size = 0;
        static const unsigned char reference[] = {
            0x67,0x42,0x00,0x28,0x96,0x54,
            0x03,0xc0,0x11,0x2f,0x2a
        };

        int ok = pos < n && end > pos &&
            sps_generate_crop(b + pos, end - pos,
                              generated, sizeof(generated),
                              &generated_size) == 0 &&
            generated_size == sizeof(reference) &&
            memcmp(generated, reference,
                   sizeof(reference)) == 0;

        fprintf(stderr, "SPS_DYNAMIC_MATCH=%s\n",
                ok ? "YES" : "NO");

        if (!ok)
            return -1;
    }

    if (video_width == 640 && video_height == 480) {
        static const unsigned char expected[] = {
            0x00,0x00,0x00,0x01,0x67,0x42,0x00,0x28,
            0x96,0x54,0x05,0x01,0xe8,0x80,0x00,0x00,
            0x00,0x01,0x68,0xce,0x3c,0x80
        };

        if (h.nLength != sizeof(expected) ||
            memcmp(h.pBuffer, expected, sizeof(expected)) != 0) {
            fprintf(stderr, "ERRO: SPS 480p desconhecido\n");
            return -1;
        }

        if (fwrite(h.pBuffer, 1, h.nLength, f) != h.nLength)
            return -1;

        fprintf(stderr, "SPS_480P_ORIGINAL=OK\n");
        return 0;
    }

    if (video_width == 1280 && video_height == 720) {
        if (h.nLength != 22 ||
            memcmp(h.pBuffer,
                   "\x00\x00\x00\x01\x67\x42\x00\x28"
                   "\x96\x54\x02\x80\x2d\x88\x00\x00"
                   "\x00\x01\x68\xce\x3c\x80", 22) != 0) {
            fprintf(stderr, "ERRO: SPS 720p diferente do validado\n");
            return -1;
        }

        if (fwrite(h.pBuffer, 1, h.nLength, f) != h.nLength)
            return -1;

        fprintf(stderr, "SPS_720P_ORIGINAL=OK\n");
        return 0;
    }

    if (video_width == 854 && video_height == 480) {
        static const unsigned char expected_raw[] = {
            0x00,0x00,0x00,0x01,
            0x67,0x42,0x00,0x28,0x96,0x54,
            0x06,0xc1,0xe8,0x80,
            0x00,0x00,0x00,0x01,
            0x68,0xce,0x3c,0x80
        };

        if (n != sizeof(expected_raw) ||
            memcmp(b, expected_raw, sizeof(expected_raw)) != 0 ||
            pos >= n || end <= pos || end >= n) {
            fprintf(stderr, "ERRO: SPS 854x480 original inesperado\\n");
            return -1;
        }

        unsigned char generated[4096];
        size_t generated_size = 0;

        if (sps_generate_crop(b + pos, end - pos,
                              generated, sizeof(generated),
                              &generated_size) != 0 ||
            generated_size == 0) {
            fprintf(stderr, "ERRO: geracao SPS 854x480\\n");
            return -1;
        }

        fprintf(stderr,
                "SPS_854X480_GENERATED_BYTES=%zu\\n",
                generated_size);

        if (fwrite(b, 1, pos, f) != pos ||
            fwrite(generated, 1, generated_size, f) != generated_size ||
            fwrite(b + end, 1, n - end, f) != n - end) {
            fprintf(stderr, "ERRO: escrita SPS 854x480\\n");
            return -1;
        }

        fprintf(stderr, "SPS_854X480_DYNAMIC_WRITE=OK\\n");
        return 0;
    }


    if (video_width == 1024 && video_height == 576) {
        static const unsigned char expected[] = {
            0x00,0x00,0x00,0x01,
            0x67,0x42,0x00,0x28,0x96,0x54,
            0x02,0x00,0x24,0x88,
            0x00,0x00,0x00,0x01,
            0x68,0xce,0x3c,0x80
        };

        if (n != sizeof(expected) ||
            memcmp(b, expected, sizeof(expected)) != 0) {
            fprintf(stderr, "ERRO: SPS 1024x576 inesperado\n");
            return -1;
        }

        if (fwrite(b, 1, n, f) != n)
            return -1;

        fprintf(stderr, "SPS_1024X576_ORIGINAL=OK\n");
        return 0;
    }




    if (video_width == 1408 && video_height == 768) {
        static const unsigned char expected[] = {
            0x00,0x00,0x00,0x01,
            0x67,0x42,0x00,0x28,0x96,0x54,
            0x02,0xc0,0x30,0x88,
            0x00,0x00,0x00,0x01,
            0x68,0xce,0x3c,0x80
        };

        if (n != sizeof(expected) ||
            memcmp(b, expected, sizeof(expected)) != 0) {
            fprintf(stderr, "ERRO: SPS 1408x768 inesperado\n");
            return -1;
        }

        if (fwrite(b, 1, n, f) != n) {
            fprintf(stderr, "ERRO: escrita SPS 1408x768\n");
            return -1;
        }

        fprintf(stderr, "SPS_1408X768_ORIGINAL=OK\n");
        return 0;
    }

    if (video_width == 1376 && video_height == 768) {
        static const unsigned char expected[] = {
            0x00,0x00,0x00,0x01,
            0x67,0x42,0x00,0x28,0x96,0x54,
            0x02,0xb0,0x30,0x88,
            0x00,0x00,0x00,0x01,
            0x68,0xce,0x3c,0x80
        };

        if (n != sizeof(expected) ||
            memcmp(b, expected, sizeof(expected)) != 0) {
            fprintf(stderr, "ERRO: SPS 1376x768 inesperado\n");
            return -1;
        }

        if (fwrite(b, 1, n, f) != n)
            return -1;

        fprintf(stderr, "SPS_1376X768_ORIGINAL=OK\n");
        return 0;
    }

    if (video_width == 1366 && video_height == 768) {
        static const unsigned char expected_raw[] = {
            0x00,0x00,0x00,0x01,
            0x67,0x42,0x00,0x28,0x96,0x54,
            0x02,0xc0,0x30,0x88,
            0x00,0x00,0x00,0x01,
            0x68,0xce,0x3c,0x80
        };

        if (n != sizeof(expected_raw) ||
            memcmp(b, expected_raw, sizeof(expected_raw)) != 0 ||
            pos >= n || end <= pos || end >= n) {
            fprintf(stderr, "ERRO: SPS 1366x768 original inesperado\n");
            return -1;
        }

        unsigned char generated[4096];
        size_t generated_size = 0;

        if (sps_generate_crop(b + pos, end - pos,
                              generated, sizeof(generated),
                              &generated_size) != 0 ||
            generated_size == 0) {
            fprintf(stderr, "ERRO: geracao SPS 1366x768\n");
            return -1;
        }

        fprintf(stderr,
                "SPS_1366X768_GENERATED_BYTES=%zu\n",
                generated_size);

        if (fwrite(b, 1, pos, f) != pos ||
            fwrite(generated, 1, generated_size, f) != generated_size ||
            fwrite(b + end, 1, n - end, f) != n - end) {
            fprintf(stderr, "ERRO: escrita SPS 1366x768\n");
            return -1;
        }

        fprintf(stderr, "SPS_1366X768_PAD1408_DYNAMIC_WRITE=OK\n");
        return 0;
    }

    if (video_width != 1920 || video_height != 1080) {
        fprintf(stderr, "SPS_VALIDATION_PENDING\n");
        return -1;
    }

    if (pos==n || end==n || end-pos!=sizeof(old_sps) ||
        memcmp(b+pos,old_sps,sizeof(old_sps))!=0) {
        fprintf(stderr,"ERRO: SPS desconhecido; nao aplicar patch\n");return -1;
    }
    unsigned char dynamic_sps[4096];
    size_t dynamic_size = 0;

    if (sps_generate_crop(b + pos, end - pos,
                          dynamic_sps, sizeof(dynamic_sps),
                          &dynamic_size) != 0 ||
        dynamic_size != sizeof(new_sps) ||
        memcmp(dynamic_sps, new_sps, sizeof(new_sps)) != 0) {
        fprintf(stderr, "ERRO: SPS dinamico invalido\\n");
        return -1;
    }

    if (fwrite(b,1,pos,f)!=pos ||
        fwrite(dynamic_sps,1,dynamic_size,f)!=dynamic_size ||
        fwrite(b+end,1,n-end,f)!=n-end) return -1;
    fprintf(stderr,"SPS_DYNAMIC_WRITE=OK\n");
    return 0;
}

static int write_frame_header(uint32_t size, int64_t pts) {
    unsigned char hdr[12];
    uint64_t ts=(uint64_t)pts;

    for (int i=0;i<4;i++)
        hdr[i]=(unsigned char)(size>>(8*i));

    for (int i=0;i<8;i++)
        hdr[4+i]=(unsigned char)(ts>>(8*i));

    return fwrite(hdr,1,sizeof(hdr),out)==sizeof(hdr) ? 0 : -1;
}

static int encode_picture(VideoPicture *pic) {
    static int ass_initialized = 0;
    if (!ass_initialized) {
        if (cedar_ass_init(video_width, video_height) != 0) {
            fprintf(stderr, "CEDAR_ASS_INIT_FAILED\n");
            return -1;
        }
        ass_initialized = 1;
    }

    static int layout_logged = 0;

    if (!layout_logged) {
        fprintf(stderr,
            "DECODER_LAYOUT_V790 "
            "visible=%dx%d storage=%dx%d "
            "pic=%dx%d stride=%d "
            "format=%d afbc=%d "
            "y=%d uv=%d\\n",
            video_width, video_height,
            storage_width, storage_height,
            pic->nWidth, pic->nHeight,
            pic->nLineStride,
            pic->ePixelFormat,
            pic->bEnableAfbcFlag,
            pic->pData0 != NULL,
            pic->pData1 != NULL);

        layout_logged = 1;
    }

    if (pic->ePixelFormat!=PIXEL_FORMAT_NV12 || pic->nWidth!=WIDTH ||
        pic->nHeight!=HEIGHT || pic->nLineStride!=WIDTH ||
        !pic->pData0 || !pic->pData1 || pic->bEnableAfbcFlag) {
        fprintf(stderr,"ERRO: layout NV12 inesperado fmt=%d %dx%d stride=%d afbc=%d\n",
            pic->ePixelFormat,pic->nWidth,pic->nHeight,pic->nLineStride,pic->bEnableAfbcFlag);
        return -1;
    }
    VencInputBuffer in={0};
    int r=GetOneAllocInputBuffer(encoder,&in);
    if (r!=0 || !in.pAddrVirY || !in.pAddrVirC) {
        fprintf(stderr,"ERRO: GetOneAllocInputBuffer=%d\n",r);return -1;
    }
    /* Copy before ReturnPicture: decoder owns the source buffer. */
    if (video_width == 1366 && video_height == 768) {
        const size_t src_stride = (size_t)pic->nLineStride;
        const size_t dst_stride = (size_t)ENC_WIDTH;
        const size_t active = (size_t)video_width;
        if (src_stride != 1376 || dst_stride != 1408 ||
            ENC_HEIGHT != 768 || active > src_stride || active > dst_stride) {
            fprintf(stderr, "ERRO: stride de padding inesperado\n");
            ReturnOneAllocInputBuffer(encoder,&in);
            return -1;
        }
        unsigned char *dst_y = (unsigned char *)in.pAddrVirY;
        unsigned char *dst_uv = (unsigned char *)in.pAddrVirC;
        const unsigned char *src_y = (const unsigned char *)pic->pData0;
        const unsigned char *src_uv = (const unsigned char *)pic->pData1;
        for (int y = 0; y < ENC_HEIGHT; y++) {
            const unsigned char *src = src_y + (size_t)y * src_stride;
            unsigned char *dst = dst_y + (size_t)y * dst_stride;
            memcpy(dst, src, active);
            memset(dst + active, src[active - 1], dst_stride - active);
        }
        for (int y = 0; y < ENC_HEIGHT / 2; y++) {
            const unsigned char *src = src_uv + (size_t)y * src_stride;
            unsigned char *dst = dst_uv + (size_t)y * dst_stride;
            memcpy(dst, src, active);
            for (size_t x = active; x < dst_stride; x += 2) {
                dst[x] = src[active - 2];
                dst[x + 1] = src[active - 1];
            }
        }
        static int pad_logged = 0;
        if (!pad_logged) {
            fprintf(stderr, "NV12_PAD_V7103 src_stride=%zu dst_stride=%zu active=%zu right=%zu\n",
                    src_stride, dst_stride, active, dst_stride - active);
            pad_logged = 1;
        }
    } else {
        memcpy(in.pAddrVirY,pic->pData0,(size_t)WIDTH*HEIGHT);
        memcpy(in.pAddrVirC,pic->pData1,(size_t)WIDTH*HEIGHT/2);
    }

    cedar_ass_render_nv12(
        (uint8_t *)in.pAddrVirY,
        (uint8_t *)in.pAddrVirC,
        ENC_WIDTH,
        video_width,
        video_height,
        pic->nPts
    );

    in.nPts=pic->nPts;
    if ((r=FlushCacheAllocInputBuffer(encoder,&in))!=0) {
        fprintf(stderr,"ERRO: FlushCache=%d\n",r);
        ReturnOneAllocInputBuffer(encoder,&in);return -1;
    }
    if ((r=AddOneInputBuffer(encoder,&in))!=0) {
        fprintf(stderr,"ERRO: AddOneInputBuffer=%d\n",r);
        ReturnOneAllocInputBuffer(encoder,&in);return -1;
    }
    /* On errors after enqueue, stop; encoder teardown owns the queued input. */
    if ((r=VideoEncodeOneFrame(encoder))!=0) {
        fprintf(stderr,"ERRO: VideoEncodeOneFrame=%d\n",r);return -1;
    }
    VencOutputBuffer bs={0};
    if ((r=GetOneBitstreamFrame(encoder,&bs))!=0) {
        fprintf(stderr,"ERRO: GetOneBitstreamFrame=%d\n",r);return -1;
    }
    int ok=1;
    size_t payload_size=(size_t)bs.nSize0+bs.nSize1+bs.nSize2;
    size_t prefix_size=(encoded==0)?codec_header_size:0;

    if (payload_size+prefix_size>UINT32_MAX ||
        write_frame_header((uint32_t)(payload_size+prefix_size),
                           (int64_t)bs.nPts)!=0)
        ok=0;

    if (ok && prefix_size &&
        fwrite(codec_header,1,prefix_size,out)!=prefix_size)
        ok=0;

    if (ok && bs.nSize0>0 &&
        (!bs.pData0 ||
         fwrite(bs.pData0,1,bs.nSize0,out)!=(size_t)bs.nSize0))
        ok=0;

    if (ok && bs.nSize1>0 &&
        (!bs.pData1 ||
         fwrite(bs.pData1,1,bs.nSize1,out)!=(size_t)bs.nSize1))
        ok=0;

    if (ok && bs.nSize2>0 &&
        (!bs.pData2 ||
         fwrite(bs.pData2,1,bs.nSize2,out)!=(size_t)bs.nSize2))
        ok=0;

    FreeOneBitStreamFrame(encoder,&bs);
    VencInputBuffer used={0};
    if ((r=AlreadyUsedInputBuffer(encoder,&used))!=0) {
        fprintf(stderr,"ERRO: AlreadyUsedInputBuffer=%d\n",r);return -1;
    }
    if ((r=ReturnOneAllocInputBuffer(encoder,&used))!=0) {
        fprintf(stderr,"ERRO: ReturnOneAllocInputBuffer=%d\n",r);return -1;
    }
    if (!ok) {fprintf(stderr,"ERRO: gravacao H264\n");return -1;}
    encoded++;
    if (encoded<=5 || encoded%300==0) fprintf(stderr,"ENCODED=%d PTS=%lld\n",encoded,(long long)pic->nPts);
    return 0;
}

static int collect(VideoDecoder *dec) {
    VideoPicture *pic;
    while ((pic=RequestPicture(dec,0))!=NULL) {
        frames++;
        int r=0;

        if (cut_pts >= 0 && pic->nPts < cut_pts) {
            skipped_frames++;

            if (skipped_frames <= 5 || skipped_frames % 50 == 0)
                fprintf(stderr,
                    "SKIPPED=%d PTS=%lld CUT=%lld\n",
                    skipped_frames,
                    (long long)pic->nPts,
                    (long long)cut_pts);
        } else {
            r=encode_picture(pic);
        }

        int rr=ReturnPicture(dec,pic);
        if (rr!=0) {fprintf(stderr,"ERRO: ReturnPicture=%d\n",rr);return -1;}
        if (r!=0) return -1;
    }
    return 0;
}

static void log_buffer_state(VideoDecoder *dec, const char *reason) {
    int capacity = VideoStreamBufferSize(dec, 0);
    int used = VideoStreamDataSize(dec, 0);
    int pending = VideoStreamFrameNum(dec, 0);

    int total = TotalPictureBufferNum(dec, 0);
    int empty = EmptyPictureBufferNum(dec, 0);
    int valid = ValidPictureNum(dec, 0);

    fprintf(stderr,
        "BUFFER_MON reason=%s packet=%d frames=%d "
        "sbm_used=%d sbm_capacity=%d sbm_pending=%d "
        "fbm_total=%d fbm_empty=%d fbm_valid=%d\n",
        reason, packets, frames,
        used, capacity, pending,
        total, empty, valid);
}

static int decode_step(VideoDecoder *dec,int eos) {
    if (++calls>MAX_CALLS || seconds()>MAX_SECONDS) {
        fprintf(stderr,"ERRO: limite de chamadas/tempo\n");return -1;
    }
    int r=DecodeVideoStream(dec,eos,0,0,0);
    if (collect(dec)!=0) return -1;
    if (r>=0 && r<16) result_counts[r]++;
    if (frames>last_progress_frames) {
        last_progress_frames=frames;
        last_progress_packet=packets;
        v53_last_progress_sec=seconds();
        v5_recovery_armed=1;
        if (frames%100==0) fprintf(stderr,"PROGRESS packet=%d frames=%d calls=%d ret=%d sec=%.2f\n",packets,frames,calls,r,seconds());
    }

    /* Instrumentacao experimental: nao modifica a decodificacao. */
    if (packets-last_progress_packet>=16 &&
        (calls%16)==0) {
        fprintf(stderr,
            "STALL_DIAG packet=%d frames=%d encoded=%d "
            "lag=%d since_progress=%d calls=%d ret=%d sec=%.3f\n",
            packets,frames,encoded,packets-frames,
            packets-last_progress_packet,calls,r,seconds());
    }
    if (r<VDECODE_RESULT_UNSUPPORTED || r>VDECODE_RESULT_RESOLUTION_CHANGE ||
        r==VDECODE_RESULT_UNSUPPORTED || r==VDECODE_RESULT_RESOLUTION_CHANGE) {
        fprintf(stderr,"ERRO: DecodeVideoStream=%d\n",r);return -1;
    }
    return r;
}

int main(int argc,char **argv) {
    FILE *input=NULL;
    struct ScMemOpsS *memops=NULL;
    VeOpsS *veops=NULL;
    void *dec_ve=NULL,*enc_ve=NULL;
    VideoDecoder *decoder=NULL;
    int mem_open=0,enc_initialized=0,enc_buffers=0,ret=1,r;
    clock_gettime(CLOCK_MONOTONIC,&start);
    setbuf(stdout,NULL);
    setbuf(stderr,NULL);

    /*
     * CedarC escreve logs diretamente no descritor 1.
     * Reservamos uma copia do stdout original para o video.
     */
    int video_fd=dup(STDOUT_FILENO);

    if (video_fd<0) {
        perror("dup stdout");
        return 2;
    }

    if (dup2(STDERR_FILENO,STDOUT_FILENO)<0) {
        perror("dup2 stderr");
        close(video_fd);
        return 2;
    }
    if (argc != 3 && argc != 4) {
        fprintf(stderr,
            "Uso: %s packets.bin saida.h264 [cut_pts_us]\n",
            argv[0]);
        return 2;
    }

    if (argc == 4) {
        char *end = NULL;
        cut_pts = strtoll(argv[3], &end, 10);

        if (end == argv[3] || *end != '\0' || cut_pts < 0) {
            fprintf(stderr, "ERRO: cut_pts invalido\n");
            return 2;
        }

        fprintf(stderr, "CUT_PTS=%lld\\n",
                (long long)cut_pts);
    }
    input=(strcmp(argv[1],"-")==0) ? stdin : fopen(argv[1],"rb");
    if (!input) {perror("entrada");return 2;}
    if (strcmp(argv[2],"-")==0) {
        out=fdopen(video_fd,"wb");
        if (!out) {
            perror("fdopen video");
            close(video_fd);
            if (input!=stdin) fclose(input);
            return 2;
        }
    } else {
        close(video_fd);
        out=fopen(argv[2],"wb");
    }
    if (!out) {
        perror("saida");
        if (input!=stdin) fclose(input);
        return 2;
    }
    video_width = read_dimension("CEDAR_WIDTH", 1920);
    video_height = read_dimension("CEDAR_HEIGHT", 1080);

    if (video_width < 0 || video_height < 0) {
        fprintf(stderr, "ERRO: dimensoes invalidas\n");
        goto cleanup;
    }

    storage_width = ALIGN16(video_width);
    storage_height = ALIGN16(video_height);
    encoder_width = storage_width;
    encoder_height = storage_height;
    if (video_width == 1366 && video_height == 768)
        encoder_width = 1408;
    fprintf(stderr, "ENCODER_STORAGE=%dx%d DECODER_STORAGE=%dx%d\n",
            encoder_width, encoder_height, storage_width, storage_height);

    fprintf(stderr,
        "VIDEO_SIZE=%dx%d STORAGE_SIZE=%dx%d\n",
        video_width, video_height,
        storage_width, storage_height);

    memops=MemAdapterGetOpsS();
    if (!memops || CdcMemOpen(memops)!=0) {fprintf(stderr,"ERRO: CdcMemOpen\n");goto cleanup;}
    mem_open=1;
    veops=GetVeOpsS(VE_OPS_TYPE_NORMAL);
    if (!veops) {fprintf(stderr,"ERRO: GetVeOpsS\n");goto cleanup;}
    VeConfig dc={0};dc.nEncoderFlag=0;dc.nWidth=WIDTH;dc.memops=memops;
    dec_ve=CdcVeInit(veops,&dc);
    if (!dec_ve) {fprintf(stderr,"ERRO: CdcVeInit decoder\n");goto cleanup;}
    AddVDPlugin();
    decoder=CreateVideoDecoder();
    if (!decoder) {fprintf(stderr,"ERRO: CreateVideoDecoder\n");goto cleanup;}
    int encoder_fps=24;
    const char *fps_env=getenv("CEDAR_FPS");

    if (fps_env && *fps_env) {
        char *end=NULL;
        errno=0;
        long value=strtol(fps_env,&end,10);

        if (errno || end==fps_env || *end!='\0' ||
            value<1 || value>60) {
            fprintf(stderr,"ERRO: CEDAR_FPS invalido\n");
            goto cleanup;
        }

        encoder_fps=(int)value;
    }

    fprintf(stderr,"CEDAR_FPS=%d\n",encoder_fps);
    int input_codec=VIDEO_CODEC_FORMAT_H264;
    const char *codec_env=getenv("CEDAR_CODEC");

    if (codec_env && *codec_env) {
        if (strcmp(codec_env,"h264")==0) {
            input_codec=VIDEO_CODEC_FORMAT_H264;
        } else if (strcmp(codec_env,"hevc")==0 ||
                   strcmp(codec_env,"h265")==0) {
            input_codec=VIDEO_CODEC_FORMAT_H265;
        } else {
            fprintf(stderr,"ERRO: CEDAR_CODEC invalido\n");
            goto cleanup;
        }
    }

    fprintf(stderr,"CEDAR_CODEC=%s\n",
            input_codec==VIDEO_CODEC_FORMAT_H264 ?
            "h264" : "hevc");

    VideoStreamInfo info={0};
    info.eCodecFormat=input_codec;
    info.nWidth=video_width;info.nHeight=video_height;info.nFrameRate=encoder_fps*1000;
    info.nFrameDuration=1000000/encoder_fps;info.bIsFramePackage=1;
    VConfig cfg={0};cfg.memops=memops;cfg.veOpsS=veops;
    cfg.pVeOpsSelf=dec_ve;cfg.eOutputPixelFormat=PIXEL_FORMAT_NV12;
    cfg.nFrameBufferNum=8;
    r=InitializeVideoDecoder(decoder,&info,&cfg);
    fprintf(stderr,"INITIALIZE_RESULT=%d\n",r);
    if (r!=0) {fprintf(stderr,"ERRO: InitializeVideoDecoder=%d\n",r);goto cleanup;}

    VeConfig ec={0};ec.nEncoderFlag=1;ec.nWidth=ENC_WIDTH;ec.memops=memops;
    enc_ve=CdcVeInit(veops,&ec);
    if (!enc_ve) {fprintf(stderr,"ERRO: CdcVeInit encoder\n");goto cleanup;}
    encoder=VideoEncCreate(VENC_CODEC_H264);
    if (!encoder) {fprintf(stderr,"ERRO: VideoEncCreate\n");goto cleanup;}
    VencBaseConfig base={0};
    base.nInputWidth=ENC_WIDTH;base.nInputHeight=ENC_HEIGHT;
    base.nDstWidth=ENC_WIDTH;base.nDstHeight=ENC_HEIGHT;
    base.nStride=ENC_WIDTH;base.eInputFormat=VENC_PIXEL_YUV420SP;
    base.memops=memops;base.veOpsS=veops;base.pVeOpsSelf=enc_ve;
    int encoder_bitrate=6000000;
    const char *bitrate_env=getenv("CEDAR_BITRATE");

    if (bitrate_env && *bitrate_env) {
        char *end=NULL;
        errno=0;
        long value=strtol(bitrate_env,&end,10);

        if (errno || end==bitrate_env || *end!='\0' ||
            value<100000 || value>20000000) {
            fprintf(stderr,"ERRO: CEDAR_BITRATE invalido\n");
            goto cleanup;
        }

        encoder_bitrate=(int)value;
    }

    fprintf(stderr,"ENCODER_BITRATE=%d\n",encoder_bitrate);
    VencH264Param h={0};
    h.sProfileLevel.nProfile=VENC_H264ProfileBaseline;
    h.sProfileLevel.nLevel=40;
    h.sQPRange.nMinqp=20;h.sQPRange.nMaxqp=45;
    h.nFramerate=encoder_fps;h.nSrcFramerate=encoder_fps;
    h.nBitrate=encoder_bitrate;h.nMaxKeyInterval=encoder_fps*3;
    h.nCodingMode=VENC_FRAME_CODING;
    r=VideoEncSetParameter(encoder,VENC_IndexParamH264Param,&h);
    if (r!=0) {fprintf(stderr,"ERRO: VideoEncSetParameter=%d\n",r);goto cleanup;}
    r=VideoEncInit(encoder,&base);
    fprintf(stderr,"ENCODER_INIT=%d\n",r);
    if (r!=0) goto cleanup;
    enc_initialized=1;
    VencAllocateBufferParam alloc={0};
    alloc.nBufferNum=2;alloc.nSizeY=ENC_WIDTH*ENC_HEIGHT;alloc.nSizeC=ENC_WIDTH*ENC_HEIGHT/2;
    r=AllocInputBuffer(encoder,&alloc);
    if (r!=0) {fprintf(stderr,"ERRO: AllocInputBuffer=%d\n",r);goto cleanup;}
    enc_buffers=1;
    FILE *header_file=tmpfile();
    if (!header_file) {
        fprintf(stderr,"ERRO: tmpfile SPS/PPS\n");
        goto cleanup;
    }
    if (write_sps_pps(encoder,header_file)!=0 ||
        fflush(header_file)!=0 ||
        fseek(header_file,0,SEEK_END)!=0) {
        fclose(header_file);
        goto cleanup;
    }
    long header_len=ftell(header_file);
    if (header_len<=0 || header_len>65536 ||
        fseek(header_file,0,SEEK_SET)!=0) {
        fclose(header_file);
        goto cleanup;
    }
    codec_header_size=(size_t)header_len;
    codec_header=malloc(codec_header_size);
    if (!codec_header ||
        fread(codec_header,1,codec_header_size,header_file)!=codec_header_size) {
        fclose(header_file);
        goto cleanup;
    }
    fclose(header_file);
    fprintf(stderr,"CODEC_HEADER_BYTES=%zu\n",codec_header_size);

    for (;;) {
        unsigned char hdr[4];
        size_t got=fread(hdr,1,4,input);
        if (got==0 && feof(input)) break;
        if (got!=4) {fprintf(stderr,"ERRO: cabecalho incompleto\n");errors++;break;}
        uint32_t size=(uint32_t)hdr[0]|((uint32_t)hdr[1]<<8)|
            ((uint32_t)hdr[2]<<16)|((uint32_t)hdr[3]<<24);
        if (!size || size>MAX_PACKET || packets>=MAX_PACKETS) {
            fprintf(stderr,"ERRO: pacote invalido=%u\n",size);errors++;break;
        }
        unsigned char ptsbuf[8];
        if (fread(ptsbuf,1,8,input)!=8) {
            fprintf(stderr,"ERRO: PTS incompleto\n");errors++;break;
        }
        uint64_t raw_pts=0;
        for (int j=0;j<8;j++)
            raw_pts |= ((uint64_t)ptsbuf[j]) << (8*j);
        int64_t packet_pts=(int64_t)raw_pts;
        char *b1=NULL,*b2=NULL;int n1=0,n2=0,ready=0;
        unsigned long long packet_retries = 0;

        for (int attempt=0;attempt<256;attempt++) {
            r=RequestVideoStreamBuffer(decoder,(int)size,&b1,&n1,&b2,&n2,0);

            if (r==0 && b1 && n1>=0 && n2>=0 &&
                (uint64_t)n1+(uint64_t)n2>=size) {
                ready=1;
                if (packet_retries > 0)
                    sbm_recovery_count++;
                break;
            }

            sbm_retry_count++;
            packet_retries++;

            if (decode_step(decoder,0)<0) break;

            /* V4 experimental: aguardar 10 ms entre tentativas. */
            struct timespec retry_pause = {
                .tv_sec = 0,
                .tv_nsec = 10000000L
            };
            sbm_wait_count++;
            nanosleep(&retry_pause, NULL);
        }

        if (packet_retries > sbm_max_retries)
            sbm_max_retries = packet_retries;
        if (!ready) {fprintf(stderr,"ERRO: RequestVideoStreamBuffer sem espaco\n");errors++;break;}
        size_t first=size<(uint32_t)n1?size:(size_t)n1;
        if (fread(b1,1,first,input)!=first ||
            (size>first && (!b2 || fread(b2,1,size-first,input)!=size-first))) {
            fprintf(stderr,"ERRO: leitura incompleta\n");errors++;break;
        }
        VideoStreamDataInfo data={0};
        data.pData=b1;data.nLength=(int)size;
        data.nPts=packet_pts;data.nPcr=-1;
        data.bIsFirstPart=1;data.bIsLastPart=1;data.bValid=1;
        data.nStreamIndex=0;
        r=SubmitVideoStreamData(decoder,&data,0);
        if (r!=0) {fprintf(stderr,"ERRO: SubmitVideoStreamData=%d\n",r);errors++;break;}
        packets++;
        if (packets%100==0) {
            fprintf(stderr,
                "V53_MON packet=%d frames=%d encoded=%d "
                "no_progress_ms=%.1f "
                "recovery_attempts=%llu recovery_successes=%llu "
                "recovery_timeouts=%llu sec=%.3f\n",
                packets,frames,encoded,
                v53_last_progress_sec > 0.0 ?
                    (seconds()-v53_last_progress_sec)*1000.0 : -1.0,
                v5_recovery_attempts,
                v5_recovery_successes,
                v5_recovery_timeouts,
                seconds());

            log_buffer_state(decoder, "periodic");
            fprintf(stderr,"FEED packet=%d frames=%d lag=%d size=%u last_progress_packet=%d\n",
                packets,frames,packets-frames,size,last_progress_packet);

            fprintf(stderr,
                "SBM_STATS packets=%d retries=%llu waits=%llu recoveries=%llu max_retries=%llu\n",
                packets,
                sbm_retry_count,
                sbm_wait_count,
                sbm_recovery_count,
                sbm_max_retries);
        }
        /*
         * V5.3 BETA: recuperacao preventiva do decoder.
         * Nao modifica o bitstream nem reinicializa o CedarC.
         */
        if (v5_recovery_armed &&
            packets-last_progress_packet>=32 &&
            packets-last_progress_packet<=180 &&
            VideoStreamFrameNum(decoder,0)>0) {

            v5_recovery_armed=0;
            v5_recovery_attempts++;

            int before=frames;
            int pending=VideoStreamFrameNum(decoder,0);

            fprintf(stderr,
                "V5_RECOVERY_BEGIN packet=%d frames=%d "
                "pending=%d lag=%d sec=%.3f\n",
                packets,frames,pending,packets-frames,seconds());

            struct timespec pause={
                .tv_sec=0,
                .tv_nsec=10000000L
            };

            for (int k=0;k<50;k++) {
                nanosleep(&pause,NULL);

                r=decode_step(decoder,0);

                if (r<0) {
                    errors++;
                    break;
                }

                if (frames>before)
                    break;
            }

            fprintf(stderr,
                "V5_RECOVERY_END packet=%d "
                "frames_before=%d frames_after=%d "
                "recovered=%d sec=%.3f\n",
                packets,before,frames,
                frames>before,seconds());

            if (frames>before)
                v5_recovery_successes++;
            else
                v5_recovery_timeouts++;

            if (errors)
                break;
        }

        if (packets-last_progress_packet>180) {
            int before=frames;
            fprintf(stderr,
                "WAIT_BEGIN packet=%d frames=%d encoded=%d calls=%d\\n",
                packets,frames,encoded,calls);

            struct timespec pause={.tv_sec=0,.tv_nsec=10000000L};
            int failed=0;

            for (int k=0;k<300;k++) {
                nanosleep(&pause,NULL);
                r=decode_step(decoder,0);
                if (r<0) {
                    failed=1;
                    break;
                }
                if (frames>before)
                    break;
            }

            fprintf(stderr,
                "WAIT_END packet=%d frames_before=%d frames_after=%d recovered=%d calls=%d\\n",
                packets,before,frames,frames>before,calls);

            if (failed || frames==before) {
                fprintf(stderr,"STALL_PERSISTENTE\\n");
                errors++;
                break;
            }
        }
        for (int j=0;j<16;j++) {
            r=decode_step(decoder,0);
            if (r<0) {errors++;break;}
            if (r==VDECODE_RESULT_NO_BITSTREAM || r==VDECODE_RESULT_NO_FRAME_BUFFER) break;
        }
        if (errors) break;
    }

    int drained=0;

    if (!errors) {
        int previous_frames=frames;
        int idle=0;

        fprintf(stderr,
            "DRAIN_BEGIN packets=%d frames=%d encoded=%d\n",
            packets,frames,encoded);

        struct timespec pause={
            .tv_sec=0,
            .tv_nsec=10000000L
        };

        for (int j=0;j<3000;j++) {
            r=decode_step(decoder,1);

            if (r<0) {
                errors++;
                break;
            }

            if (frames>previous_frames) {
                previous_frames=frames;
                idle=0;
            } else {
                idle++;
            }

            if (j%100==0) {
                fprintf(stderr,
                    "DRAIN_PROGRESS iteration=%d frames=%d encoded=%d ret=%d idle=%d\n",
                    j,frames,encoded,r,idle);
            }

            if (r==VDECODE_RESULT_NO_BITSTREAM && idle>=100) {
                drained=1;
                break;
            }

            if (idle>=500) {
                fprintf(stderr,
                    "DRAIN_STALL idle=%d ret=%d\n",idle,r);
                break;
            }

            nanosleep(&pause,NULL);
        }

        fprintf(stderr,
            "DRAIN_END packets=%d frames=%d encoded=%d drained=%d\n",
            packets,frames,encoded,drained);
    }

    if (!errors && !drained) {
        fprintf(stderr,"ERRO: drenagem nao concluida\n");
        errors++;
    }
    if (ferror(input)) {fprintf(stderr,"ERRO: leitura de entrada\n");errors++;}
    if (fflush(out)!=0) {fprintf(stderr,"ERRO: fflush\n");errors++;}
    if (!errors && (packets==0 || frames!=packets || encoded + skipped_frames != frames)) {
        fprintf(stderr,"ERRO: contagem divergente packets=%d frames=%d encoded=%d\n",packets,frames,encoded);
        errors++;
    }
    ret=(errors==0)?0:1;
cleanup:
    fprintf(stderr,
        "SBM_RETRY_COUNT=%llu\n"
        "SBM_WAIT_COUNT=%llu\n"
        "SBM_RECOVERY_COUNT=%llu\n"
        "SBM_MAX_RETRIES=%llu\n",
        sbm_retry_count,
        sbm_wait_count,
        sbm_recovery_count,
        sbm_max_retries);

    fprintf(stderr,
        "V5_RECOVERY_STATS attempts=%llu successes=%llu timeouts=%llu\n",
        v5_recovery_attempts,
        v5_recovery_successes,
        v5_recovery_timeouts);

    fprintf(stderr,"RESULT_COUNTS:");
    for(int i=0;i<16;i++) if(result_counts[i]) fprintf(stderr," %d=%d",i,result_counts[i]);
    fprintf(stderr,"\nLAST_PROGRESS_PACKET=%d LAST_PROGRESS_FRAMES=%d\n",last_progress_packet,last_progress_frames);
    fprintf(stderr,"PACKETS=%d\nFRAMES=%d\nENCODED=%d\nERRORS=%d\nDECODE_CALLS=%d\nSECONDS=%.3f\nFPS=%.2f\n",
        packets,frames,encoded,errors,calls,seconds(),seconds()>0?encoded/seconds():0);
    if (decoder) DestroyVideoDecoder(decoder);
    if (enc_buffers) ReleaseAllocInputBuffer(encoder);
    if (encoder) {if (enc_initialized) VideoEncUnInit(encoder);VideoEncDestroy(encoder);}
    if (enc_ve) CdcVeRelease(veops,enc_ve);
    if (dec_ve) CdcVeRelease(veops,dec_ve);
    if (mem_open) CdcMemClose(memops);
    if (input && input!=stdin) fclose(input);
    if (out && fclose(out)!=0) ret=1;
    fprintf(stderr,"EXIT_CODE=%d\n",ret);
    return ret;
}
