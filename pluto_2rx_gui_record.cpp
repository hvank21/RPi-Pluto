/*
 * pluto_2rx_gui_record.cpp
 *
 * Raspberry Pi 4 + ADALM-Pluto / Pluto+ 2RX recorder.
 * GUI dialog for parameters, then binary IQ recording to mounted SSD.
 * Output format: int16 little-endian interleaved I0,Q0,I1,Q1 per sample.
 * AGC is disabled by setting gain_control_mode = manual on RX0/RX1.
 *
 * Build on Raspberry Pi OS:
 *   sudo apt update
 *   sudo apt install -y g++ pkg-config libiio-dev libgtk-3-dev
 *   g++ -O2 -Wall -Wextra pluto_2rx_gui_record.cpp -o pluto_2rx_gui_record \
 *       $(pkg-config --cflags --libs gtk+-3.0) -liio
 *
 * Run:
 *   ./pluto_2rx_gui_record
 */

#include <gtk/gtk.h>
#include <iio.h>

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

static std::atomic<bool> g_stop(false);

static void sig_handler(int) {
    g_stop.store(true);
}

struct AppCfg {
    std::string uri = "ip:192.168.2.1";
    std::string out_dir = "/mnt/ssd";
    std::string out_file = "";

    double duration_sec = 60.0;      // max 180
    double fs_msps = 4.0;
    double bw_mhz = 3.0;
    double lo_mhz = 1575.420;
    double gain0_db = 35.0;
    double gain1_db = 35.0;

    int block_samples = 65536;
    int kernel_buffers = 8;
};

struct GuiWidgets {
    GtkWidget *uri;
    GtkWidget *out_dir;
    GtkWidget *duration;
    GtkWidget *fs;
    GtkWidget *bw;
    GtkWidget *lo;
    GtkWidget *gain0;
    GtkWidget *gain1;
    GtkWidget *block;
    GtkWidget *kbuf;
};

static long long mhz(double x) {
    return static_cast<long long>(x * 1e6 + 0.5);
}

static std::string timestamp_file_name() {
    char buf[128];
    std::time_t t = std::time(nullptr);
    std::tm tmv{};
    localtime_r(&t, &tmv);
    std::strftime(buf, sizeof(buf), "pluto_2rx_%Y%m%d_%H%M%S.bin", &tmv);
    return std::string(buf);
}

static bool dir_exists(const std::string &path) {
    struct stat st{};
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

static void show_error(GtkWindow *parent, const char *msg) {
    GtkWidget *d = gtk_message_dialog_new(
        parent,
        GTK_DIALOG_MODAL,
        GTK_MESSAGE_ERROR,
        GTK_BUTTONS_CLOSE,
        "%s",
        msg
    );
    gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
}

static GtkWidget *grid_label(const char *txt) {
    GtkWidget *l = gtk_label_new(txt);
    gtk_widget_set_halign(l, GTK_ALIGN_START);
    return l;
}

static GtkWidget *entry_text(const char *txt) {
    GtkWidget *e = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(e), txt);
    gtk_widget_set_hexpand(e, TRUE);
    return e;
}

static GtkWidget *spin_double(double val, double minv, double maxv, double step, int digits) {
    GtkAdjustment *adj = gtk_adjustment_new(val, minv, maxv, step, step * 10.0, 0.0);
    GtkWidget *s = gtk_spin_button_new(adj, step, digits);
    gtk_spin_button_set_numeric(GTK_SPIN_BUTTON(s), TRUE);
    return s;
}

static GtkWidget *spin_int(int val, int minv, int maxv, int step) {
    GtkAdjustment *adj = gtk_adjustment_new(val, minv, maxv, step, step * 10, 0.0);
    GtkWidget *s = gtk_spin_button_new(adj, step, 0);
    gtk_spin_button_set_numeric(GTK_SPIN_BUTTON(s), TRUE);
    return s;
}

static bool get_config_from_gui(AppCfg &cfg) {
    int argc = 0;
    char **argv = nullptr;
    gtk_init(&argc, &argv);

    GtkWidget *dialog = gtk_dialog_new_with_buttons(
        "Pluto 2RX binary recorder",
        nullptr,
        GTK_DIALOG_MODAL,
        "Start recording", GTK_RESPONSE_OK,
        "Cancel", GTK_RESPONSE_CANCEL,
        nullptr
    );

    gtk_window_set_default_size(GTK_WINDOW(dialog), 520, 420);

    GtkWidget *area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 12);
    gtk_container_add(GTK_CONTAINER(area), grid);

    GuiWidgets w{};
    w.uri = entry_text(cfg.uri.c_str());
    w.out_dir = entry_text(cfg.out_dir.c_str());
    w.duration = spin_double(cfg.duration_sec, 1.0, 180.0, 1.0, 0);
    w.fs = spin_double(cfg.fs_msps, 0.520, 30.72, 0.1, 3);
    w.bw = spin_double(cfg.bw_mhz, 0.2, 56.0, 0.1, 3);
    w.lo = spin_double(cfg.lo_mhz, 70.0, 6000.0, 0.001, 6);
    w.gain0 = spin_double(cfg.gain0_db, -10.0, 73.0, 1.0, 1);
    w.gain1 = spin_double(cfg.gain1_db, -10.0, 73.0, 1.0, 1);
    w.block = spin_int(cfg.block_samples, 1024, 1048576, 1024);
    w.kbuf = spin_int(cfg.kernel_buffers, 2, 32, 1);

    int r = 0;
    gtk_grid_attach(GTK_GRID(grid), grid_label("Pluto URI"),            0, r, 1, 1); gtk_grid_attach(GTK_GRID(grid), w.uri,      1, r++, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), grid_label("SSD directory"),        0, r, 1, 1); gtk_grid_attach(GTK_GRID(grid), w.out_dir,  1, r++, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), grid_label("Record time, sec"),     0, r, 1, 1); gtk_grid_attach(GTK_GRID(grid), w.duration, 1, r++, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), grid_label("Sample rate, MS/s"),    0, r, 1, 1); gtk_grid_attach(GTK_GRID(grid), w.fs,       1, r++, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), grid_label("RX bandwidth, MHz"),    0, r, 1, 1); gtk_grid_attach(GTK_GRID(grid), w.bw,       1, r++, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), grid_label("RX LO, MHz"),           0, r, 1, 1); gtk_grid_attach(GTK_GRID(grid), w.lo,       1, r++, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), grid_label("RX0 gain, dB manual"),  0, r, 1, 1); gtk_grid_attach(GTK_GRID(grid), w.gain0,    1, r++, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), grid_label("RX1 gain, dB manual"),  0, r, 1, 1); gtk_grid_attach(GTK_GRID(grid), w.gain1,    1, r++, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), grid_label("Block samples"),        0, r, 1, 1); gtk_grid_attach(GTK_GRID(grid), w.block,    1, r++, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), grid_label("Kernel buffers"),       0, r, 1, 1); gtk_grid_attach(GTK_GRID(grid), w.kbuf,     1, r++, 1, 1);

    GtkWidget *info = gtk_label_new("Output format: binary int16 LE: I0,Q0,I1,Q1.  AGC = OFF / manual gain.");
    gtk_widget_set_halign(info, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), info, 0, r++, 2, 1);

    gtk_widget_show_all(dialog);

    bool ok = false;
    while (true) {
        int resp = gtk_dialog_run(GTK_DIALOG(dialog));
        if (resp != GTK_RESPONSE_OK) {
            ok = false;
            break;
        }

        cfg.uri = gtk_entry_get_text(GTK_ENTRY(w.uri));
        cfg.out_dir = gtk_entry_get_text(GTK_ENTRY(w.out_dir));
        cfg.duration_sec = gtk_spin_button_get_value(GTK_SPIN_BUTTON(w.duration));
        cfg.fs_msps = gtk_spin_button_get_value(GTK_SPIN_BUTTON(w.fs));
        cfg.bw_mhz = gtk_spin_button_get_value(GTK_SPIN_BUTTON(w.bw));
        cfg.lo_mhz = gtk_spin_button_get_value(GTK_SPIN_BUTTON(w.lo));
        cfg.gain0_db = gtk_spin_button_get_value(GTK_SPIN_BUTTON(w.gain0));
        cfg.gain1_db = gtk_spin_button_get_value(GTK_SPIN_BUTTON(w.gain1));
        cfg.block_samples = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(w.block));
        cfg.kernel_buffers = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(w.kbuf));

        if (cfg.uri.empty()) {
            show_error(GTK_WINDOW(dialog), "URI is empty.");
            continue;
        }
        if (!dir_exists(cfg.out_dir)) {
            show_error(GTK_WINDOW(dialog), "SSD directory does not exist. Mount SSD first, for example /mnt/ssd.");
            continue;
        }
        if (access(cfg.out_dir.c_str(), W_OK) != 0) {
            show_error(GTK_WINDOW(dialog), "SSD directory is not writable by current user.");
            continue;
        }
        if (cfg.duration_sec < 1.0 || cfg.duration_sec > 180.0) {
            show_error(GTK_WINDOW(dialog), "Record time must be 1...180 seconds.");
            continue;
        }
        if (cfg.bw_mhz > cfg.fs_msps * 1.8) {
            show_error(GTK_WINDOW(dialog), "Bandwidth is too wide for selected sample rate. Reduce BW or increase Fs.");
            continue;
        }

        cfg.out_file = cfg.out_dir + "/" + timestamp_file_name();
        ok = true;
        break;
    }

    gtk_widget_destroy(dialog);
    while (gtk_events_pending()) gtk_main_iteration();
    return ok;
}

static int wr_str(iio_channel *ch, const char *attr, const char *val, const char *name) {
    int ret = iio_channel_attr_write(ch, attr, val);
    if (ret < 0) {
        std::fprintf(stderr, "ERROR: %s %s = %s failed: %s\n", name, attr, val, std::strerror(-ret));
    }
    return ret;
}

static int wr_ll(iio_channel *ch, const char *attr, long long val, const char *name) {
    int ret = iio_channel_attr_write_longlong(ch, attr, val);
    if (ret < 0) {
        std::fprintf(stderr, "ERROR: %s %s = %lld failed: %s\n", name, attr, val, std::strerror(-ret));
    }
    return ret;
}

static int wr_double(iio_channel *ch, const char *attr, double val, const char *name) {
    int ret = iio_channel_attr_write_double(ch, attr, val);
    if (ret < 0) {
        std::fprintf(stderr, "ERROR: %s %s = %.3f failed: %s\n", name, attr, val, std::strerror(-ret));
    }
    return ret;
}

static void try_config_phy_channel(iio_channel *ch, const char *name, const AppCfg &cfg, double gain_db) {
    if (!ch) return;

    // AGC OFF: manual gain mode.
    wr_str(ch, "gain_control_mode", "manual", name);

    // Port and analog/DSP rates. Some firmware exposes common attributes only on voltage0;
    // failed writes are printed but do not stop recording.
    wr_str(ch, "rf_port_select", "A_BALANCED", name);
    wr_ll(ch, "rf_bandwidth", mhz(cfg.bw_mhz), name);
    wr_ll(ch, "sampling_frequency", mhz(cfg.fs_msps), name);
    wr_double(ch, "hardwaregain", gain_db, name);
}

static int record_2rx_binary(const AppCfg &cfg) {
    const long long total_samples = static_cast<long long>(cfg.duration_sec * cfg.fs_msps * 1e6 + 0.5);
    const double bytes_total = static_cast<double>(total_samples) * 4.0 * sizeof(int16_t);

    std::printf("\n=== Pluto 2RX binary recording ===\n");
    std::printf("URI              : %s\n", cfg.uri.c_str());
    std::printf("Output           : %s\n", cfg.out_file.c_str());
    std::printf("Duration         : %.0f sec\n", cfg.duration_sec);
    std::printf("Fs               : %.6f MS/s\n", cfg.fs_msps);
    std::printf("BW               : %.6f MHz\n", cfg.bw_mhz);
    std::printf("LO               : %.6f MHz\n", cfg.lo_mhz);
    std::printf("Gain RX0/RX1     : %.1f / %.1f dB, AGC OFF\n", cfg.gain0_db, cfg.gain1_db);
    std::printf("Samples          : %lld\n", total_samples);
    std::printf("Expected size    : %.2f MiB\n", bytes_total / 1024.0 / 1024.0);
    std::printf("Format           : int16 I0,Q0,I1,Q1\n");
    std::printf("Stop             : Ctrl+C\n\n");

    iio_context *ctx = iio_create_context_from_uri(cfg.uri.c_str());
    if (!ctx) {
        std::fprintf(stderr, "ERROR: cannot connect to Pluto URI %s\n", cfg.uri.c_str());
        return 1;
    }

    iio_device *phy = iio_context_find_device(ctx, "ad9361-phy");
    iio_device *rxdev = iio_context_find_device(ctx, "cf-ad9361-lpc");
    if (!phy || !rxdev) {
        std::fprintf(stderr, "ERROR: devices ad9361-phy/cf-ad9361-lpc not found. Check Pluto firmware and URI.\n");
        iio_context_destroy(ctx);
        return 1;
    }

    iio_channel *phy_rx0 = iio_device_find_channel(phy, "voltage0", false);
    iio_channel *phy_rx1 = iio_device_find_channel(phy, "voltage1", false);
    iio_channel *rx_lo = iio_device_find_channel(phy, "altvoltage0", true);

    if (!phy_rx0 || !rx_lo) {
        std::fprintf(stderr, "ERROR: RX PHY channels not found.\n");
        iio_context_destroy(ctx);
        return 1;
    }

    try_config_phy_channel(phy_rx0, "RX0", cfg, cfg.gain0_db);
    try_config_phy_channel(phy_rx1, "RX1", cfg, cfg.gain1_db);
    wr_ll(rx_lo, "frequency", mhz(cfg.lo_mhz), "RX_LO");

    iio_channel *rx0_i = iio_device_find_channel(rxdev, "voltage0", false);
    iio_channel *rx0_q = iio_device_find_channel(rxdev, "voltage1", false);
    iio_channel *rx1_i = iio_device_find_channel(rxdev, "voltage2", false);
    iio_channel *rx1_q = iio_device_find_channel(rxdev, "voltage3", false);

    if (!rx0_i || !rx0_q || !rx1_i || !rx1_q) {
        std::fprintf(stderr, "ERROR: 4 DMA channels voltage0..3 not found. Pluto must be in 2R/2R2T mode.\n");
        iio_context_destroy(ctx);
        return 1;
    }

    iio_channel_enable(rx0_i);
    iio_channel_enable(rx0_q);
    iio_channel_enable(rx1_i);
    iio_channel_enable(rx1_q);

    int ret = iio_device_set_kernel_buffers_count(rxdev, cfg.kernel_buffers);
    if (ret < 0) {
        std::fprintf(stderr, "WARNING: kernel buffer count set failed: %s\n", std::strerror(-ret));
    }

    iio_buffer *rxbuf = iio_device_create_buffer(rxdev, cfg.block_samples, false);
    if (!rxbuf) {
        std::fprintf(stderr, "ERROR: RX buffer creation failed. Another process may use Pluto DMA.\n");
        iio_channel_disable(rx0_i);
        iio_channel_disable(rx0_q);
        iio_channel_disable(rx1_i);
        iio_channel_disable(rx1_q);
        iio_context_destroy(ctx);
        return 1;
    }
    iio_buffer_set_blocking_mode(rxbuf, true);

    FILE *out = std::fopen(cfg.out_file.c_str(), "wb");
    if (!out) {
        std::fprintf(stderr, "ERROR: cannot open output file %s: %s\n", cfg.out_file.c_str(), std::strerror(errno));
        iio_buffer_destroy(rxbuf);
        iio_context_destroy(ctx);
        return 1;
    }

    long long written_samples = 0;
    std::time_t t0 = std::time(nullptr);

    while (!g_stop.load() && written_samples < total_samples) {
        ssize_t nbytes = iio_buffer_refill(rxbuf);
        if (nbytes < 0) {
            std::fprintf(stderr, "ERROR: iio_buffer_refill failed: %s\n", std::strerror(-nbytes));
            break;
        }

        const int16_t *raw = reinterpret_cast<const int16_t *>(iio_buffer_start(rxbuf));

        long long available_samples = nbytes / (4LL * static_cast<long long>(sizeof(int16_t)));
        if (available_samples <= 0) {
            std::fprintf(stderr, "ERROR: empty buffer.\n");
            break;
        }

        long long left = total_samples - written_samples;
        long long take_samples = available_samples < left ? available_samples : left;

        size_t wr = std::fwrite(raw, 4 * sizeof(int16_t), static_cast<size_t>(take_samples), out);
        if (wr != static_cast<size_t>(take_samples)) {
            std::fprintf(stderr, "ERROR: fwrite failed or disk full: %s\n", std::strerror(errno));
            break;
        }

        written_samples += take_samples;

        if (written_samples % (cfg.block_samples * 10LL) == 0 || written_samples >= total_samples) {
            double pct = 100.0 * static_cast<double>(written_samples) / static_cast<double>(total_samples);
            double mb = static_cast<double>(written_samples) * 8.0 / 1024.0 / 1024.0;
            std::time_t now = std::time(nullptr);
            std::printf("Progress: %6.2f %% | %lld / %lld samples | %.2f MiB | %ld sec\r",
                        pct, written_samples, total_samples, mb, static_cast<long>(now - t0));
            std::fflush(stdout);
        }
    }

    std::printf("\n");
    std::fflush(out);
    std::fclose(out);

    iio_buffer_destroy(rxbuf);
    iio_channel_disable(rx0_i);
    iio_channel_disable(rx0_q);
    iio_channel_disable(rx1_i);
    iio_channel_disable(rx1_q);
    iio_context_destroy(ctx);

    double final_mb = static_cast<double>(written_samples) * 8.0 / 1024.0 / 1024.0;
    std::printf("Done. Written: %lld samples, %.2f MiB\n", written_samples, final_mb);
    std::printf("File: %s\n", cfg.out_file.c_str());

    return written_samples > 0 ? 0 : 1;
}

int main() {
    std::signal(SIGINT, sig_handler);
    std::signal(SIGTERM, sig_handler);

    AppCfg cfg;
    if (!get_config_from_gui(cfg)) {
        std::printf("Cancelled.\n");
        return 0;
    }

    return record_2rx_binary(cfg);
}
