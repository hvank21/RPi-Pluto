/* iq_recording_2r.c
 *
 *  Запис IQ з PlutoSDR у 2R-режимі (cf-ad9361-lpc, 4 канали: I0,Q0,I1,Q1).
 *  Без MVDR/GSC, без AXI-запису коефіцієнтів, без NEON-математики —
 *  лише отримання вибірок та збереження у файл.
 *
 *  Конфігурування зберігається через прапорці командного рядка.
 *
 *  Компіляція (macOS, iio.framework):
 *    cc -O2 -Wall -I/Library/Frameworks/iio.framework/Headers \
 *       -F/Library/Frameworks -framework iio -framework CoreFoundation \
 *       iq_recording_2r.c -o iq_recording_2r
 *
 *  Компіляція (Linux):
 *    cc -O2 -Wall iq_recording_2r.c -o iq_recording_2r -liio
 *
 *  ────────────────────────────────────────────────────────────────────────
 *  ЯК ЗАПУСКАТИ
 *  ────────────────────────────────────────────────────────────────────────
 *
 *  Передумови:
 *    - PlutoSDR прошитий у 2R2T (cf-ad9361-lpc показує voltage0..3 на input).
 *      Перевірити:  iio_info -s         → побачити пристрій
 *                   iio_info -u <URI> | grep "cf-ad9361-lpc" -A 6
 *    - На самому Pluto не повинна виконуватись інша програма, що тримає
 *      DMA-буфер (gps_proxy_*, lc_stap_*, iio_readdev тощо). Інакше
 *      iio_device_create_buffer повертає "Resource busy" / "Open unlocked: -16".
 *
 *  Вибір URI:
 *    -u usb:<bus.port.dev>   найшвидший на macOS, точну адресу дає `iio_info -s`
 *    -u ip:192.168.2.1       через USB-Ethernet gadget
 *    -u ip:pluto.local       якщо mDNS працює (за замовчуванням)
 *
 *  Прапорці:
 *    -u uri      URI Pluto                            (def: ip:pluto.local)
 *    -s rate     Sample-rate, MS/s                    (def: 2.6)
 *    -b bw       RX bandwidth, MHz                    (def: 5)
 *    -f lo       RX LO, GHz                           (def: 1.575420 — GPS L1)
 *    -a gain     RX gain, dB (manual, обидва канали)  (def: 25.0)
 *    -k kbufs    Кількість kernel-буферів             (def: 8)
 *    -n samp    Вибірок на один RX-блок              (def: 32000)
 *    -N total    Скільки всього записати, 0 = до Ctrl-C (def: 50000)
 *    -o file     Шлях до вихідного файла              (def: /tmp/iq.txt)
 *    --bin       Бінарний вивід (4×int16 на семпл) замість тексту
 *    -h          Допомога
 *
 *  Формат файла:
 *    text  → один семпл на рядок: "  I0   Q0   I1   Q1\n"  (рядкове %6d)
 *    --bin → послідовність int16 LE: I0,Q0,I1,Q1, I0,Q0,I1,Q1, …
 *            (8 байт на семпл; зчитування у Python:
 *              np.fromfile(path, dtype=np.int16).reshape(-1,4))
 *
 *  Приклади:
 *    # 50 000 семплів у текст за замовчуванням (USB)
 *    ./iq_recording_2r -u usb:1.1.5
 *
 *    # 1 секунда @ 2.6 MS/s у бінарник, GPS L1, gain 30 dB
 *    ./iq_recording_2r -u usb:1.1.5 -s 2.6 -a 30 \
 *                      -N 2600000 -o /tmp/iq.bin --bin
 *
 *    # Безперервний запис (Ctrl-C для зупинки), більший блок для меншого
 *    # навантаження на USB
 *    ./iq_recording_2r -u ip:192.168.2.1 -n 65536 -k 16 -N 0 \
 *                      -o /tmp/long.bin --bin
 *
 *    # Інший центральний канал (наприклад, 2.4 ГГц), ширша смуга
 *    ./iq_recording_2r -u usb:1.1.5 -f 2.4 -b 20 -s 20
 *
 *  Зупинка:
 *    Ctrl-C — коректне завершення (буфери закриваються, файл закривається).
 *    Або задайте -N <total> для авто-зупинки після N семплів.
 *
 *  Поширені помилки:
 *    "Resource busy" / "Open unlocked: -16"
 *        На Pluto щось тримає DMA — зайдіть по ssh та зніміть процес,
 *        або перепідключіть Pluto.
 *    "Required IIO devices not found"
 *        Прошивка не у 2R-режимі або URI вказує не на Pluto.
 *    Дуже маленькі значення (|I|,|Q| < 20)
 *        Антена не підключена або gain замалий — підніміть -a.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <signal.h>
#include <getopt.h>
#include <errno.h>
#include <unistd.h>
#include <iio.h>

#define MHZ(x)         ((long long)((x) * 1e6 + 0.5))
#define GHZ(x)         ((long long)((x) * 1e9 + 0.5))

#define DEF_URI        "ip:pluto.local"
#define DEF_SR_MSPS    2.6
#define DEF_BW_MHZ     5.0
#define DEF_LO_GHZ     1.575420
#define DEF_KBUF       8
#define DEF_RX_GAIN_DB 25.0
#define DEF_BLOCK_SAMP 32000
#define DEF_TOTAL_SAMP 50000LL
#define DEF_OUT_PATH   "/tmp/iq.txt"

struct stream_cfg {
    long long bw_hz;
    long long fs_hz;
    long long lo_hz;
    const char *rfport;
    double gain_db;
};

static volatile bool stop = false;

static void sig_handler(int sig) { (void)sig; stop = true; }

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [options]\n"
        "  -u uri      Pluto URI (default %s)\n"
        "  -s rate     Sample-rate MS/s (default %.3g)\n"
        "  -b bw       RX bandwidth MHz (default %.3g)\n"
        "  -f lo       RX LO frequency GHz (default %.6f)\n"
        "  -a gain     RX gain dB (default %.1f)\n"
        "  -k kbufs    Kernel buffers (default %d)\n"
        "  -n samp     Samples per RX block (default %d)\n"
        "  -N total    Total samples to record, 0 = until Ctrl-C (default %lld)\n"
        "  -o file     Output filename (default %s)\n"
        "  --bin       Binary output (4×int16 per sample) instead of text\n"
        "  -h          This help\n",
        prog, DEF_URI, DEF_SR_MSPS, DEF_BW_MHZ, DEF_LO_GHZ,
        DEF_RX_GAIN_DB, DEF_KBUF, DEF_BLOCK_SAMP, DEF_TOTAL_SAMP, DEF_OUT_PATH);
}

int main(int argc, char **argv) {
    const char *uri = DEF_URI;
    double sr_msps = DEF_SR_MSPS;
    double bw_mhz  = DEF_BW_MHZ;
    double lo_ghz  = DEF_LO_GHZ;
    double rx_gain = DEF_RX_GAIN_DB;
    int kbufs      = DEF_KBUF;
    int block_samp = DEF_BLOCK_SAMP;
    long long total_samp = DEF_TOTAL_SAMP;
    const char *out_path = DEF_OUT_PATH;
    bool binary = false;

    static struct option long_opts[] = {
        { "bin",  no_argument, 0, 'B' },
        { "help", no_argument, 0, 'h' },
        { 0, 0, 0, 0 }
    };
    int opt;
    while ((opt = getopt_long(argc, argv, "u:s:b:f:a:k:n:N:o:h", long_opts, NULL)) != -1) {
        switch (opt) {
            case 'u': uri = optarg; break;
            case 's': sr_msps = atof(optarg); break;
            case 'b': bw_mhz  = atof(optarg); break;
            case 'f': lo_ghz  = atof(optarg); break;
            case 'a': rx_gain = atof(optarg); break;
            case 'k': kbufs   = atoi(optarg); break;
            case 'n': block_samp = atoi(optarg); break;
            case 'N': total_samp = atoll(optarg); break;
            case 'o': out_path = optarg; break;
            case 'B': binary = true; break;
            case 'h': default: usage(argv[0]); return EXIT_FAILURE;
        }
    }
    if (sr_msps <= 0 || kbufs < 2 || block_samp < 16 || total_samp < 0) {
        fprintf(stderr, "Invalid parameters, see: %s -h\n", argv[0]);
        return EXIT_FAILURE;
    }

    struct stream_cfg rx = {
        .bw_hz   = MHZ(bw_mhz),
        .fs_hz   = MHZ(sr_msps),
        .lo_hz   = (long long)(lo_ghz * 1e9 + 0.5),
        .rfport  = "A_BALANCED",
        .gain_db = rx_gain,
    };

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    struct iio_context *ctx = iio_create_context_from_uri(uri);
    if (!ctx) {
        fprintf(stderr, "iio_create_context_from_uri(%s): %s\n", uri, strerror(errno));
        return EXIT_FAILURE;
    }
    struct iio_device *phy   = iio_context_find_device(ctx, "ad9361-phy");
    struct iio_device *rxdev = iio_context_find_device(ctx, "cf-ad9361-lpc");
    if (!phy || !rxdev) {
        fprintf(stderr, "Required IIO devices not found\n");
        iio_context_destroy(ctx);
        return EXIT_FAILURE;
    }

    struct iio_channel *phy_rx = iio_device_find_channel(phy, "voltage0", false);
    if (!phy_rx) {
        fprintf(stderr, "ad9361-phy voltage0 (rx) channel not found\n");
        iio_context_destroy(ctx);
        return EXIT_FAILURE;
    }
    iio_channel_attr_write(phy_rx,          "rf_port_select",     rx.rfport);
    iio_channel_attr_write_longlong(phy_rx, "rf_bandwidth",       rx.bw_hz);
    iio_channel_attr_write_longlong(phy_rx, "sampling_frequency", rx.fs_hz);
    iio_channel_attr_write_double  (phy_rx, "hardwaregain",       rx.gain_db);

    /* Both RX paths get the same gain in 2R mode */
    struct iio_channel *phy_rx1 = iio_device_find_channel(phy, "voltage1", false);
    if (phy_rx1) {
        iio_channel_attr_write_double(phy_rx1, "hardwaregain", rx.gain_db);
    }

    struct iio_channel *lo_rx = iio_device_find_channel(phy, "altvoltage0", true);
    if (!lo_rx) {
        fprintf(stderr, "RX_LO channel not found\n");
        iio_context_destroy(ctx);
        return EXIT_FAILURE;
    }
    iio_channel_attr_write_longlong(lo_rx, "frequency", rx.lo_hz);
    iio_channel_attr_write_bool    (lo_rx, "powerdown", false);

    struct iio_channel *rx0_i = iio_device_find_channel(rxdev, "voltage0", false);
    struct iio_channel *rx0_q = iio_device_find_channel(rxdev, "voltage1", false);
    struct iio_channel *rx1_i = iio_device_find_channel(rxdev, "voltage2", false);
    struct iio_channel *rx1_q = iio_device_find_channel(rxdev, "voltage3", false);
    if (!rx0_i || !rx0_q || !rx1_i || !rx1_q) {
        fprintf(stderr, "RX IQ channels not found — is Pluto in 2R mode?\n");
        iio_context_destroy(ctx);
        return EXIT_FAILURE;
    }
    iio_channel_enable(rx0_i);
    iio_channel_enable(rx0_q);
    iio_channel_enable(rx1_i);
    iio_channel_enable(rx1_q);

    iio_device_set_kernel_buffers_count(rxdev, kbufs);
    struct iio_buffer *rxbuf = iio_device_create_buffer(rxdev, block_samp, false);
    if (!rxbuf) {
        fprintf(stderr, "RX buffer creation failed: %s\n", strerror(errno));
        iio_context_destroy(ctx);
        return EXIT_FAILURE;
    }
    iio_buffer_set_blocking_mode(rxbuf, true);

    FILE *out = fopen(out_path, binary ? "wb" : "w");
    if (!out) {
        fprintf(stderr, "fopen(%s): %s\n", out_path, strerror(errno));
        iio_buffer_destroy(rxbuf);
        iio_context_destroy(ctx);
        return EXIT_FAILURE;
    }

    fprintf(stderr,
        "Recording from %s — %.3g MS/s, BW %.3g MHz, LO %.6f GHz, gain %.1f dB\n"
        "Block %d samp, %d kbufs → %s (%s)\n"
        "Target: %lld samples (%s) | Ctrl-C to stop\n",
        uri, sr_msps, bw_mhz, lo_ghz, rx_gain,
        block_samp, kbufs, out_path, binary ? "binary" : "text",
        total_samp, total_samp == 0 ? "until Ctrl-C" : "then exit");

    long long written = 0;
    while (!stop) {
        ssize_t got = iio_buffer_refill(rxbuf);
        if (got < 0) {
            fprintf(stderr, "iio_buffer_refill: %s\n", strerror(-got));
            break;
        }
        const int16_t *raw = (const int16_t *)iio_buffer_start(rxbuf);
        long long avail = block_samp;
        long long take  = avail;
        if (total_samp > 0) {
            long long left = total_samp - written;
            if (left <= 0) break;
            if (take > left) take = left;
        }

        if (binary) {
            size_t n = fwrite(raw, sizeof(int16_t) * 4, (size_t)take, out);
            if (n != (size_t)take) {
                fprintf(stderr, "fwrite short (%zu/%lld): %s\n", n, take, strerror(errno));
                break;
            }
        } else {
            for (long long k = 0; k < take; ++k) {
                int16_t i0 = raw[4*k+0], q0 = raw[4*k+1];
                int16_t i1 = raw[4*k+2], q1 = raw[4*k+3];
                fprintf(out, "%6d %6d %6d %6d\n", i0, q0, i1, q1);
            }
        }
        written += take;
        if (total_samp > 0 && written >= total_samp) {
            fprintf(stderr, "Reached target: %lld samples\n", written);
            break;
        }
    }

    fflush(out);
    fclose(out);
    iio_buffer_destroy(rxbuf);
    iio_channel_disable(rx0_i);
    iio_channel_disable(rx0_q);
    iio_channel_disable(rx1_i);
    iio_channel_disable(rx1_q);
    iio_context_destroy(ctx);
    fprintf(stderr, "Done. %lld samples written to %s\n", written, out_path);
    return EXIT_SUCCESS;
}
