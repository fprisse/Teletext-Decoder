/*
 * ttxd.c  —  DVB Teletext from HDHomeRun → UDP → Node-RED
 *
 * Build:
 *   gcc -O2 -Wall -o ttxd ttxd.c $(pkg-config --cflags --libs zvbi) -lcurl
 *
 * Usage:
 *   ttxd <hdhomerun-ip> <channel> <teletext-pid> <udp-port>
 *
 * Example:
 *   ttxd 192.168.1.50 21 409 5555
 *
 * Outputs one JSON object per complete teletext page to UDP 127.0.0.1:<port>
 * Each datagram is a self-contained JSON object terminated with newline.
 *
 * JSON format:
 *   {
 *     "page":    100,          // teletext page number (decimal)
 *     "subpage": 0,            // subpage number
 *     "ts":      1708789200,   // unix timestamp
 *     "lines": [               // 25 rows, row 0 is the header row
 *       "line 0 text",
 *       ...
 *       "line 24 text"
 *     ]
 *   }
 *
 * Lines have trailing spaces stripped.
 * Teletext graphics/mosaic characters are replaced with space.
 * Text is UTF-8.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <libzvbi.h>
#include <curl/curl.h>

/* ------------------------------------------------------------------ */
#define TS_PACKET_SIZE  188
#define TS_SYNC_BYTE    0x47
#define MAX_PES_SIZE    65548   /* 65536 + PES header overhead        */
#define UDP_MAX_PAYLOAD 8192    /* max JSON datagram size              */

/* ------------------------------------------------------------------ */
static vbi_dvb_demux     *g_demux    = NULL;
static vbi_decoder       *g_dec      = NULL;
static int                g_udp_fd   = -1;
static struct sockaddr_in g_dest;
static int                g_pid      = 0;
static volatile int       g_running  = 1;

/* TS alignment carry buffer — spans curl write callback boundaries */
static uint8_t  g_carry[TS_PACKET_SIZE];
static int      g_carry_len = 0;

/* PES accumulation */
static uint8_t  g_pes[MAX_PES_SIZE];
static int      g_pes_len    = 0;
static int      g_pes_target = 0;   /* expected total PES size, 0 = unbounded */

/* ------------------------------------------------------------------ */
static void signal_handler(int sig)
{
    (void)sig;
    g_running = 0;
}

/* ------------------------------------------------------------------ */
/* Encode a Unicode codepoint to UTF-8.  Returns bytes written.       */
static int utf8_encode(char *buf, unsigned int cp)
{
    if (cp < 0x80) {
        buf[0] = (char)cp;
        return 1;
    } else if (cp < 0x800) {
        buf[0] = (char)(0xC0 | (cp >> 6));
        buf[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    } else {
        buf[0] = (char)(0xE0 | (cp >>  12));
        buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
}

/* ------------------------------------------------------------------ */
/* JSON-escape a UTF-8 string into dst.  Returns bytes written.       */
static int json_escape(char *dst, int dst_size,
                       const char *src, int src_len)
{
    int out = 0;
    for (int i = 0; i < src_len && out < dst_size - 6; i++) {
        unsigned char c = (unsigned char)src[i];
        switch (c) {
        case '"':  dst[out++] = '\\'; dst[out++] = '"';  break;
        case '\\': dst[out++] = '\\'; dst[out++] = '\\'; break;
        case '\n': dst[out++] = '\\'; dst[out++] = 'n';  break;
        case '\r': dst[out++] = '\\'; dst[out++] = 'r';  break;
        case '\t': dst[out++] = '\\'; dst[out++] = 't';  break;
        default:
            if (c < 0x20) {
                /* control character — emit \u00XX */
                out += snprintf(dst + out, dst_size - out, "\\u%04x", c);
            } else {
                dst[out++] = (char)c;
            }
        }
    }
    dst[out] = '\0';
    return out;
}

/* ------------------------------------------------------------------ */
/* Send a null-terminated string as a single UDP datagram             */
static void udp_send(const char *s, int len)
{
    ssize_t sent = sendto(g_udp_fd, s, (size_t)len, 0,
                          (struct sockaddr *)&g_dest, sizeof(g_dest));
    if (sent < 0)
        fprintf(stderr, "ttxd: udp sendto: %s\n", strerror(errno));
}

/* ------------------------------------------------------------------ */
/* VBI event callback — fires when a complete TTX page is decoded     */
static void ttx_event_cb(vbi_event *ev, void *user_data)
{
    (void)user_data;
    if (ev->type != VBI_EVENT_TTX_PAGE) return;

    int pgno  = ev->ev.ttx_page.pgno;
    int subno = ev->ev.ttx_page.subno & 0xFFFF;

    vbi_page page;
    if (!vbi_fetch_vt_page(g_dec, &page, pgno, subno,
                           VBI_WST_LEVEL_1p5, 25, TRUE))
        return;

    /* --- Build JSON ------------------------------------------------ */
    static char   buf[UDP_MAX_PAYLOAD];
    static char   row_utf8[256];
    static char   row_esc[512];
    int           pos = 0;

    pos += snprintf(buf + pos, sizeof(buf) - pos,
                    "{\"page\":%d,\"subpage\":%d,\"ts\":%ld,\"lines\":[",
                    pgno, subno, (long)time(NULL));

    int cols = page.columns;  /* usually 40 */
    int rows = page.rows;     /* usually 25 */

    for (int row = 0; row < rows; row++) {
        int rlen = 0;
        for (int col = 0; col < cols; col++) {
            unsigned int cp = page.text[row * cols + col].unicode;

            /* Replace control chars, mosaic chars (>= 0xEE00) and   */
            /* soft-hyphen with plain space                            */
            if (cp < 0x20 || cp == 0x00AD || cp >= 0xEE00)
                cp = 0x20;

            if (rlen < (int)sizeof(row_utf8) - 4)
                rlen += utf8_encode(row_utf8 + rlen, cp);
        }
        /* Trim trailing spaces */
        while (rlen > 0 && row_utf8[rlen - 1] == ' ') rlen--;
        row_utf8[rlen] = '\0';

        if (row > 0 && pos < (int)sizeof(buf) - 2)
            buf[pos++] = ',';

        if (pos < (int)sizeof(buf) - 4)
            buf[pos++] = '"';

        int elen = json_escape(row_esc, sizeof(row_esc), row_utf8, rlen);
        if (pos + elen < (int)sizeof(buf) - 4) {
            memcpy(buf + pos, row_esc, elen);
            pos += elen;
        }

        if (pos < (int)sizeof(buf) - 2)
            buf[pos++] = '"';
    }

    if (pos < (int)sizeof(buf) - 4)
        pos += snprintf(buf + pos, sizeof(buf) - pos, "]}\n");

    buf[pos] = '\0';

    vbi_unref_page(&page);
    udp_send(buf, pos);
}

/* ------------------------------------------------------------------ */
/* Feed PES data payload (past the PES header) into libzvbi           */
static void feed_pes_data(const uint8_t *data, int len)
{
    const uint8_t  *p   = data;
    unsigned int    rem = (unsigned int)len;

    while (rem > 0) {
        vbi_sliced   sliced[64];
        int64_t      pts     = 0;

        unsigned int lines = vbi_dvb_demux_cor(g_demux,
                                               sliced, 64,
                                               &pts,
                                               &p, &rem);
        if (lines > 0)
            vbi_decode(g_dec, sliced, (int)lines,
                       (double)pts / 90000.0);

        /* If no lines were produced and rem didn't shrink, break     */
        if (lines == 0 && rem == (unsigned int)(p - data + rem))
            break;
    }
}

/* ------------------------------------------------------------------ */
/* Parse PES header and pass the data payload to feed_pes_data        */
/*                                                                     */
/* PES header layout (ISO 13818-1 §2.4.3.7):                         */
/*   0..2  : start code 0x00 0x00 0x01                                */
/*   3     : stream_id                                                 */
/*   4..5  : PES_packet_length (0 = unbounded for video/audio)        */
/*   6     : '10' + flags                                              */
/*   7     : flags                                                     */
/*   8     : PES_header_data_length (N)                               */
/*   9..9+N: optional fields (PTS, DTS, ...)                          */
/*   9+N.. : payload (for teletext: data_identifier + data units)     */
/* ------------------------------------------------------------------ */
static void dispatch_pes(void)
{
    if (g_pes_len < 9)   return;
    if (g_pes[0] != 0x00 || g_pes[1] != 0x00 || g_pes[2] != 0x01)
        return;                         /* missing start code         */

    int hdr_data_len = g_pes[8];
    int data_start   = 9 + hdr_data_len;

    if (data_start >= g_pes_len) return;

    feed_pes_data(g_pes + data_start, g_pes_len - data_start);
}

/* ------------------------------------------------------------------ */
/* Process one 188-byte TS packet                                      */
static void process_ts_packet(const uint8_t *pkt)
{
    if (pkt[0] != TS_SYNC_BYTE)    return;
    if (pkt[1] & 0x80)             return;  /* transport error        */

    int pid = ((pkt[1] & 0x1F) << 8) | pkt[2];
    if (pid != g_pid)              return;

    int pus            = (pkt[1] >> 6) & 1;  /* payload_unit_start   */
    int has_adaptation = (pkt[3] & 0x20) != 0;
    int has_payload    = (pkt[3] & 0x10) != 0;

    if (!has_payload) return;

    int payload_offset = 4;
    if (has_adaptation) {
        payload_offset = 5 + pkt[4];
        if (payload_offset >= TS_PACKET_SIZE) return;
    }

    const uint8_t *payload     = pkt + payload_offset;
    int            payload_len = TS_PACKET_SIZE - payload_offset;
    if (payload_len <= 0) return;

    if (pus) {
        /* Dispatch whatever PES we have accumulated */
        if (g_pes_len > 0)
            dispatch_pes();

        g_pes_len    = 0;
        g_pes_target = 0;

        /* Read expected PES size from new packet's header */
        if (payload_len >= 6) {
            int pes_pkt_len = (payload[4] << 8) | payload[5];
            /* 0 = unbounded (common for video); for teletext it is set */
            g_pes_target = (pes_pkt_len > 0) ? 6 + pes_pkt_len : 0;
        }
    }

    /* Accumulate payload bytes */
    if (g_pes_len + payload_len <= MAX_PES_SIZE) {
        memcpy(g_pes + g_pes_len, payload, payload_len);
        g_pes_len += payload_len;
    } else {
        fprintf(stderr, "ttxd: PES overflow, resetting\n");
        g_pes_len    = 0;
        g_pes_target = 0;
        return;
    }

    /* Dispatch as soon as PES is complete (bounded PES) */
    if (g_pes_target > 0 && g_pes_len >= g_pes_target) {
        dispatch_pes();
        g_pes_len    = 0;
        g_pes_target = 0;
    }
}

/* ------------------------------------------------------------------ */
/* libcurl write callback — receives chunks of raw MPEG-TS bytes      */
/* Chunks may not be aligned to 188-byte packet boundaries            */
/* ------------------------------------------------------------------ */
static size_t curl_write_cb(char *ptr, size_t size, size_t nmemb,
                            void *userdata)
{
    (void)userdata;
    if (!g_running) return 0;   /* causes curl to abort the transfer */

    size_t total  = size * nmemb;
    size_t offset = 0;

    /* 1. Drain the carry buffer first */
    if (g_carry_len > 0) {
        size_t need = (size_t)(TS_PACKET_SIZE - g_carry_len);
        size_t take = (total < need) ? total : need;
        memcpy(g_carry + g_carry_len, ptr, take);
        g_carry_len += (int)take;
        offset       = take;

        if (g_carry_len == TS_PACKET_SIZE) {
            process_ts_packet(g_carry);
            g_carry_len = 0;
        }
    }

    /* 2. Process complete packets directly */
    while (offset + TS_PACKET_SIZE <= total) {
        process_ts_packet((uint8_t *)ptr + offset);
        offset += TS_PACKET_SIZE;
    }

    /* 3. Save remainder in carry */
    size_t leftover = total - offset;
    if (leftover > 0) {
        memcpy(g_carry, ptr + offset, leftover);
        g_carry_len = (int)leftover;
    }

    return total;
}

/* ------------------------------------------------------------------ */
/* Create (or recreate) the libzvbi demux and decoder                 */
/* ------------------------------------------------------------------ */
static int zvbi_init(void)
{
    if (g_demux) { vbi_dvb_demux_delete(g_demux); g_demux = NULL; }
    if (g_dec)   { vbi_decoder_delete(g_dec);     g_dec   = NULL; }

    g_demux = vbi_dvb_demux_new(NULL, NULL);
    if (!g_demux) {
        fprintf(stderr, "ttxd: vbi_dvb_demux_new failed\n");
        return 0;
    }

    g_dec = vbi_decoder_new();
    if (!g_dec) {
        fprintf(stderr, "ttxd: vbi_decoder_new failed\n");
        return 0;
    }

    if (!vbi_event_handler_add(g_dec, VBI_EVENT_TTX_PAGE,
                               ttx_event_cb, NULL)) {
        fprintf(stderr, "ttxd: vbi_event_handler_add failed\n");
        return 0;
    }

    return 1;
}

/* ------------------------------------------------------------------ */
int main(int argc, char *argv[])
{
    if (argc != 5) {
        fprintf(stderr,
            "Usage: %s <hdhomerun-ip> <channel> <teletext-pid> <udp-port>\n"
            "\n"
            "  hdhomerun-ip  IP of the HDHomeRun device\n"
            "  channel       Channel number (e.g. 21)\n"
            "  teletext-pid  Teletext PID in decimal (e.g. 409)\n"
            "                Find with: ffprobe http://<ip>/auto/v<ch> 2>&1"
            " | grep teletext\n"
            "  udp-port      UDP port to send JSON to on 127.0.0.1"
            " (e.g. 5555)\n",
            argv[0]);
        return 1;
    }

    const char *hdhomerun_ip = argv[1];
    int         channel      = atoi(argv[2]);
    g_pid                    = atoi(argv[3]);
    int         udp_port     = atoi(argv[4]);

    if (g_pid <= 0 || g_pid > 8191) {
        fprintf(stderr, "ttxd: invalid PID %d\n", g_pid);
        return 1;
    }
    if (udp_port <= 0 || udp_port > 65535) {
        fprintf(stderr, "ttxd: invalid UDP port %d\n", udp_port);
        return 1;
    }

    signal(SIGINT,  signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);

    /* UDP socket ---------------------------------------------------- */
    g_udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_udp_fd < 0) { perror("socket"); return 1; }

    memset(&g_dest, 0, sizeof(g_dest));
    g_dest.sin_family      = AF_INET;
    g_dest.sin_port        = htons((uint16_t)udp_port);
    g_dest.sin_addr.s_addr = inet_addr("127.0.0.1");

    /* libzvbi ------------------------------------------------------- */
    if (!zvbi_init()) return 1;

    /* libcurl ------------------------------------------------------- */
    curl_global_init(CURL_GLOBAL_ALL);
    CURL *curl = curl_easy_init();
    if (!curl) {
        fprintf(stderr, "ttxd: curl_easy_init failed\n");
        return 1;
    }

    char url[256];
    snprintf(url, sizeof(url),
             "http://%s/auto/v%d", hdhomerun_ip, channel);

    fprintf(stderr,
            "ttxd: stream=%s  PID=%d  → udp://127.0.0.1:%d\n",
            url, g_pid, udp_port);

    /* Main reconnect loop ------------------------------------------- */
    while (g_running) {
        /* Reset accumulation state on each connection attempt */
        g_carry_len  = 0;
        g_pes_len    = 0;
        g_pes_target = 0;

        /* Recreate demuxer so its internal state is clean */
        if (!zvbi_init()) break;

        curl_easy_reset(curl);
        curl_easy_setopt(curl, CURLOPT_URL,            url);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  curl_write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA,      NULL);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT,        0L);   /* no timeout  */
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);  /* 10s connect */
        curl_easy_setopt(curl, CURLOPT_BUFFERSIZE,     65536L);
        curl_easy_setopt(curl, CURLOPT_FORBID_REUSE,   1L);

        CURLcode res = curl_easy_perform(curl);

        if (!g_running) break;

        fprintf(stderr,
                "ttxd: stream ended: %s — retrying in 5s\n",
                curl_easy_strerror(res));
        sleep(5);
    }

    fprintf(stderr, "ttxd: shutting down\n");

    curl_easy_cleanup(curl);
    curl_global_cleanup();

    if (g_dec)   vbi_decoder_delete(g_dec);
    if (g_demux) vbi_dvb_demux_delete(g_demux);
    close(g_udp_fd);

    return 0;
}
