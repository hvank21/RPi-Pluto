// pluto_2rx_spectrogram_gui.cpp
// GUI utility for ADALM-Pluto 2RX binary IQ files:
// 1) Select .bin file with interleaved int16: I0 Q0 I1 Q1
// 2) Build spectrograms for RX0 and RX1 using selectable FFT length N
// 3) Convert selected number of samples to TXT columns: I0 Q0 I1 Q1
//
// Build:
// g++ -O2 -Wall -Wextra pluto_2rx_spectrogram_gui.cpp -o pluto_2rx_spectrogram_gui \
//     $(pkg-config --cflags --libs gtk+-3.0) -lfftw3 -lm

#include <gtk/gtk.h>
#include <cairo.h>
#include <fftw3.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

struct ComplexF {
    float re;
    float im;
};

struct SpectrogramData {
    int nfft = 8092;
    int hop = 4046;
    double fs_hz = 4.0e6;
    int frames = 0;
    int bins = 0;
    double min_db = -120.0;
    double max_db = 0.0;
    std::vector<float> rx0_db; // frames * bins, fftshifted
    std::vector<float> rx1_db;
};

struct AppWidgets {
    GtkWidget *window = nullptr;
    GtkWidget *file_entry = nullptr;
    GtkWidget *nfft_entry = nullptr;
    GtkWidget *fs_entry = nullptr;
    GtkWidget *max_frames_entry = nullptr;
    GtkWidget *convert_samples_entry = nullptr;
    GtkWidget *status_label = nullptr;
    GtkWidget *draw_rx0 = nullptr;
    GtkWidget *draw_rx1 = nullptr;
    GtkWidget *convert_button = nullptr;
    GtkWidget *spectrogram_button = nullptr;
    SpectrogramData spec;
};

static std::string gtk_entry_text(GtkWidget *entry) {
    const char *t = gtk_entry_get_text(GTK_ENTRY(entry));
    return t ? std::string(t) : std::string();
}

static void set_status(AppWidgets *app, const std::string &msg) {
    gtk_label_set_text(GTK_LABEL(app->status_label), msg.c_str());
    while (gtk_events_pending()) gtk_main_iteration();
}

static bool parse_int(const std::string &s, int &v) {
    try {
        size_t idx = 0;
        long x = std::stol(s, &idx);
        if (idx != s.size()) return false;
        if (x <= 0 || x > std::numeric_limits<int>::max()) return false;
        v = static_cast<int>(x);
        return true;
    } catch (...) {
        return false;
    }
}

static bool parse_double(const std::string &s, double &v) {
    try {
        size_t idx = 0;
        double x = std::stod(s, &idx);
        if (idx != s.size()) return false;
        if (!(x > 0.0)) return false;
        v = x;
        return true;
    } catch (...) {
        return false;
    }
}

static std::string default_txt_name(const std::string &bin_path) {
    size_t p = bin_path.find_last_of('.');
    if (p == std::string::npos) return bin_path + ".txt";
    return bin_path.substr(0, p) + ".txt";
}

static uint64_t file_size_bytes(const std::string &path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return 0;
    return static_cast<uint64_t>(f.tellg());
}

static bool read_samples_for_spectrogram(const std::string &path,
                                         int nfft,
                                         int max_frames,
                                         std::vector<ComplexF> &rx0,
                                         std::vector<ComplexF> &rx1,
                                         std::string &err) {
    const int hop = std::max(1, nfft / 2);
    uint64_t sz = file_size_bytes(path);
    if (sz == 0) {
        err = "Файл не відкривається або має нульовий розмір";
        return false;
    }
    if (sz % 8 != 0) {
        err = "Розмір файлу не кратний 8 байтам. Очікується формат int16 I0 Q0 I1 Q1";
        return false;
    }

    uint64_t total_samples = sz / 8;
    if (total_samples < static_cast<uint64_t>(nfft)) {
        err = "У файлі менше семплів, ніж N FFT";
        return false;
    }

    uint64_t possible_frames = 1 + (total_samples - nfft) / hop;
    uint64_t frames_to_use = std::min<uint64_t>(possible_frames, static_cast<uint64_t>(max_frames));
    uint64_t samples_to_read = nfft + (frames_to_use - 1) * hop;

    rx0.assign(samples_to_read, {0, 0});
    rx1.assign(samples_to_read, {0, 0});

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        err = "Не вдалося відкрити файл";
        return false;
    }

    const size_t block_samples = 262144;
    std::vector<int16_t> raw(block_samples * 4);
    uint64_t written = 0;

    while (written < samples_to_read) {
        uint64_t remain = samples_to_read - written;
        size_t n = static_cast<size_t>(std::min<uint64_t>(remain, block_samples));
        in.read(reinterpret_cast<char *>(raw.data()), n * 4 * sizeof(int16_t));
        size_t got_bytes = static_cast<size_t>(in.gcount());
        size_t got_samples = got_bytes / (4 * sizeof(int16_t));
        if (got_samples == 0) break;

        for (size_t i = 0; i < got_samples; ++i) {
            int16_t i0 = raw[4*i + 0];
            int16_t q0 = raw[4*i + 1];
            int16_t i1 = raw[4*i + 2];
            int16_t q1 = raw[4*i + 3];
            rx0[written + i] = {static_cast<float>(i0), static_cast<float>(q0)};
            rx1[written + i] = {static_cast<float>(i1), static_cast<float>(q1)};
        }
        written += got_samples;
    }

    if (written < samples_to_read) {
        rx0.resize(written);
        rx1.resize(written);
    }

    if (rx0.size() < static_cast<size_t>(nfft)) {
        err = "Недостатньо семплів після читання";
        return false;
    }
    return true;
}

static void remove_dc(std::vector<ComplexF> &x) {
    if (x.empty()) return;
    long double sr = 0.0, si = 0.0;
    for (const auto &v : x) {
        sr += v.re;
        si += v.im;
    }
    float mr = static_cast<float>(sr / x.size());
    float mi = static_cast<float>(si / x.size());
    for (auto &v : x) {
        v.re -= mr;
        v.im -= mi;
    }
}

static bool compute_spectrogram(const std::vector<ComplexF> &rx0,
                                const std::vector<ComplexF> &rx1,
                                int nfft,
                                double fs_hz,
                                SpectrogramData &spec,
                                std::string &err) {
    if (rx0.size() != rx1.size() || rx0.size() < static_cast<size_t>(nfft)) {
        err = "Невірний розмір масиву для спектрограми";
        return false;
    }

    int hop = std::max(1, nfft / 2);
    int frames = 1 + static_cast<int>((rx0.size() - nfft) / hop);
    if (frames <= 0) {
        err = "Немає кадрів для спектрограми";
        return false;
    }

    spec.nfft = nfft;
    spec.hop = hop;
    spec.fs_hz = fs_hz;
    spec.frames = frames;
    spec.bins = nfft;
    spec.rx0_db.assign(static_cast<size_t>(frames) * nfft, -120.0f);
    spec.rx1_db.assign(static_cast<size_t>(frames) * nfft, -120.0f);

    std::vector<double> win(nfft);
    for (int n = 0; n < nfft; ++n) {
        win[n] = 0.5 - 0.5 * std::cos(2.0 * M_PI * n / (nfft - 1));
    }

    fftw_complex *in = reinterpret_cast<fftw_complex *>(fftw_malloc(sizeof(fftw_complex) * nfft));
    fftw_complex *out = reinterpret_cast<fftw_complex *>(fftw_malloc(sizeof(fftw_complex) * nfft));
    if (!in || !out) {
        err = "Не вдалося виділити FFTW буфери";
        if (in) fftw_free(in);
        if (out) fftw_free(out);
        return false;
    }

    fftw_plan plan = fftw_plan_dft_1d(nfft, in, out, FFTW_FORWARD, FFTW_ESTIMATE);
    if (!plan) {
        err = "Не вдалося створити FFTW plan";
        fftw_free(in);
        fftw_free(out);
        return false;
    }

    auto process_channel = [&](const std::vector<ComplexF> &x, std::vector<float> &dst) {
        for (int f = 0; f < frames; ++f) {
            size_t off = static_cast<size_t>(f) * hop;
            for (int n = 0; n < nfft; ++n) {
                in[n][0] = x[off + n].re * win[n];
                in[n][1] = x[off + n].im * win[n];
            }
            fftw_execute(plan);

            for (int k = 0; k < nfft; ++k) {
                int ks = (k + nfft / 2) % nfft; // fftshift
                double p = out[ks][0] * out[ks][0] + out[ks][1] * out[ks][1];
                double db = 10.0 * std::log10(p / (nfft * nfft) + 1e-20);
                dst[static_cast<size_t>(f) * nfft + k] = static_cast<float>(db);
            }
        }
    };

    process_channel(rx0, spec.rx0_db);
    process_channel(rx1, spec.rx1_db);

    fftw_destroy_plan(plan);
    fftw_free(in);
    fftw_free(out);

    // Robust color scale by percentiles using combined values sampled.
    std::vector<float> vals;
    vals.reserve(std::min<size_t>(spec.rx0_db.size() + spec.rx1_db.size(), 200000));
    size_t step = std::max<size_t>(1, (spec.rx0_db.size() + spec.rx1_db.size()) / 200000);
    for (size_t i = 0; i < spec.rx0_db.size(); i += step) vals.push_back(spec.rx0_db[i]);
    for (size_t i = 0; i < spec.rx1_db.size(); i += step) vals.push_back(spec.rx1_db[i]);
    if (!vals.empty()) {
        std::sort(vals.begin(), vals.end());
        size_t i5 = static_cast<size_t>(0.05 * (vals.size() - 1));
        size_t i99 = static_cast<size_t>(0.99 * (vals.size() - 1));
        spec.min_db = vals[i5];
        spec.max_db = vals[i99];
        if (spec.max_db <= spec.min_db + 1.0) spec.max_db = spec.min_db + 60.0;
    }

    return true;
}

static void draw_color(cairo_t *cr, double t) {
    // Simple blue-green-yellow-red palette without external dependencies.
    t = std::max(0.0, std::min(1.0, t));
    double r, g, b;
    if (t < 0.25) {
        double u = t / 0.25;
        r = 0.0; g = u; b = 0.5 + 0.5 * u;
    } else if (t < 0.5) {
        double u = (t - 0.25) / 0.25;
        r = 0.0; g = 1.0; b = 1.0 - u;
    } else if (t < 0.75) {
        double u = (t - 0.5) / 0.25;
        r = u; g = 1.0; b = 0.0;
    } else {
        double u = (t - 0.75) / 0.25;
        r = 1.0; g = 1.0 - u; b = 0.0;
    }
    cairo_set_source_rgb(cr, r, g, b);
}

static gboolean on_draw_spectrogram(GtkWidget *widget, cairo_t *cr, gpointer user_data) {
    AppWidgets *app = static_cast<AppWidgets *>(user_data);
    bool is_rx1 = (widget == app->draw_rx1);
    const SpectrogramData &s = app->spec;
    const std::vector<float> &data = is_rx1 ? s.rx1_db : s.rx0_db;

    int W = gtk_widget_get_allocated_width(widget);
    int H = gtk_widget_get_allocated_height(widget);

    cairo_set_source_rgb(cr, 0.05, 0.05, 0.05);
    cairo_paint(cr);

    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 14);
    cairo_move_to(cr, 10, 20);
    cairo_show_text(cr, is_rx1 ? "RX1 spectrogram" : "RX0 spectrogram");

    if (data.empty() || s.frames <= 0 || s.bins <= 0) {
        cairo_set_font_size(cr, 13);
        cairo_move_to(cr, 10, 45);
        cairo_show_text(cr, "Натисніть Build Spectrogram");
        return FALSE;
    }

    int left = 55, right = 15, top = 30, bottom = 32;
    int plotW = std::max(1, W - left - right);
    int plotH = std::max(1, H - top - bottom);

    for (int x = 0; x < plotW; ++x) {
        int frame = std::min(s.frames - 1, static_cast<int>((static_cast<double>(x) / plotW) * s.frames));
        for (int y = 0; y < plotH; ++y) {
            int bin = std::min(s.bins - 1, static_cast<int>((1.0 - static_cast<double>(y) / plotH) * s.bins));
            float db = data[static_cast<size_t>(frame) * s.bins + bin];
            double t = (db - s.min_db) / (s.max_db - s.min_db);
            draw_color(cr, t);
            cairo_rectangle(cr, left + x, top + y, 1.0, 1.0);
            cairo_fill(cr);
        }
    }

    // Border and labels.
    cairo_set_source_rgb(cr, 0.9, 0.9, 0.9);
    cairo_set_line_width(cr, 1.0);
    cairo_rectangle(cr, left, top, plotW, plotH);
    cairo_stroke(cr);

    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 11);

    char buf[128];
    snprintf(buf, sizeof(buf), "+%.3f MHz", s.fs_hz / 2.0 / 1e6);
    cairo_move_to(cr, 5, top + 8);
    cairo_show_text(cr, buf);
    snprintf(buf, sizeof(buf), "0");
    cairo_move_to(cr, 30, top + plotH / 2);
    cairo_show_text(cr, buf);
    snprintf(buf, sizeof(buf), "-%.3f MHz", s.fs_hz / 2.0 / 1e6);
    cairo_move_to(cr, 5, top + plotH - 2);
    cairo_show_text(cr, buf);

    double duration = (static_cast<double>(s.frames - 1) * s.hop + s.nfft) / s.fs_hz;
    snprintf(buf, sizeof(buf), "time: %.3f s, N=%d, dB scale %.1f...%.1f", duration, s.nfft, s.min_db, s.max_db);
    cairo_move_to(cr, left, H - 10);
    cairo_show_text(cr, buf);

    return FALSE;
}

static void on_choose_file(GtkButton *, gpointer user_data) {
    AppWidgets *app = static_cast<AppWidgets *>(user_data);
    GtkWidget *dialog = gtk_file_chooser_dialog_new("Вибрати Pluto 2RX binary file",
                                                    GTK_WINDOW(app->window),
                                                    GTK_FILE_CHOOSER_ACTION_OPEN,
                                                    "Cancel", GTK_RESPONSE_CANCEL,
                                                    "Open", GTK_RESPONSE_ACCEPT,
                                                    nullptr);
    gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dialog), "/mnt/ssd");
    GtkFileFilter *filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "Binary IQ files (*.bin)");
    gtk_file_filter_add_pattern(filter, "*.bin");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        char *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        gtk_entry_set_text(GTK_ENTRY(app->file_entry), filename);
        g_free(filename);
    }
    gtk_widget_destroy(dialog);
}

static void on_build_spectrogram(GtkButton *, gpointer user_data) {
    AppWidgets *app = static_cast<AppWidgets *>(user_data);
    std::string path = gtk_entry_text(app->file_entry);
    if (path.empty()) {
        set_status(app, "Помилка: оберіть .bin файл");
        return;
    }

    int nfft = 0, max_frames = 0;
    double fs_mhz = 0.0;
    if (!parse_int(gtk_entry_text(app->nfft_entry), nfft)) {
        set_status(app, "Помилка: N FFT має бути додатнім числом, наприклад 8092 або 8192");
        return;
    }
    if (nfft < 256 || nfft > 262144) {
        set_status(app, "Помилка: N FFT поза межами 256...262144");
        return;
    }
    if (!parse_double(gtk_entry_text(app->fs_entry), fs_mhz)) {
        set_status(app, "Помилка: Fs має бути додатнім числом у MS/s");
        return;
    }
    if (!parse_int(gtk_entry_text(app->max_frames_entry), max_frames)) {
        set_status(app, "Помилка: Max frames має бути додатнім числом");
        return;
    }

    set_status(app, "Читаю IQ файл...");
    std::vector<ComplexF> rx0, rx1;
    std::string err;
    if (!read_samples_for_spectrogram(path, nfft, max_frames, rx0, rx1, err)) {
        set_status(app, "Помилка: " + err);
        return;
    }

    set_status(app, "Видаляю DC та обчислюю FFT...");
    remove_dc(rx0);
    remove_dc(rx1);
    if (!compute_spectrogram(rx0, rx1, nfft, fs_mhz * 1e6, app->spec, err)) {
        set_status(app, "Помилка FFT: " + err);
        return;
    }

    gtk_widget_queue_draw(app->draw_rx0);
    gtk_widget_queue_draw(app->draw_rx1);

    std::ostringstream ss;
    uint64_t total_samples = file_size_bytes(path) / 8;
    ss << "Готово. Файл семплів: " << total_samples
       << "; показано кадрів: " << app->spec.frames
       << "; N=" << app->spec.nfft;
    set_status(app, ss.str());
}

static void on_convert_txt(GtkButton *, gpointer user_data) {
    AppWidgets *app = static_cast<AppWidgets *>(user_data);
    std::string path = gtk_entry_text(app->file_entry);
    if (path.empty()) {
        set_status(app, "Помилка: оберіть .bin файл");
        return;
    }

    int nsamp = 0;
    if (!parse_int(gtk_entry_text(app->convert_samples_entry), nsamp)) {
        set_status(app, "Помилка: кількість семплів для TXT має бути додатнім числом");
        return;
    }

    uint64_t total_samples = file_size_bytes(path) / 8;
    if (total_samples == 0) {
        set_status(app, "Помилка: файл не відкривається або порожній");
        return;
    }
    uint64_t to_convert = std::min<uint64_t>(total_samples, static_cast<uint64_t>(nsamp));

    GtkWidget *dialog = gtk_file_chooser_dialog_new("Зберегти TXT",
                                                    GTK_WINDOW(app->window),
                                                    GTK_FILE_CHOOSER_ACTION_SAVE,
                                                    "Cancel", GTK_RESPONSE_CANCEL,
                                                    "Save", GTK_RESPONSE_ACCEPT,
                                                    nullptr);
    std::string def = default_txt_name(path);
    gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(dialog), def.c_str());
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dialog), TRUE);

    std::string out_path;
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        char *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        out_path = filename;
        g_free(filename);
    }
    gtk_widget_destroy(dialog);

    if (out_path.empty()) {
        set_status(app, "Конвертацію скасовано");
        return;
    }

    std::ifstream in(path, std::ios::binary);
    std::ofstream out(out_path);
    if (!in || !out) {
        set_status(app, "Помилка: не вдалося відкрити вхідний або вихідний файл");
        return;
    }

    set_status(app, "Конвертую binary -> TXT...");
    out << "I0\tQ0\tI1\tQ1\n";

    const size_t block_samples = 262144;
    std::vector<int16_t> raw(block_samples * 4);
    uint64_t converted = 0;

    while (converted < to_convert) {
        uint64_t remain = to_convert - converted;
        size_t n = static_cast<size_t>(std::min<uint64_t>(remain, block_samples));
        in.read(reinterpret_cast<char *>(raw.data()), n * 4 * sizeof(int16_t));
        size_t got_bytes = static_cast<size_t>(in.gcount());
        size_t got_samples = got_bytes / (4 * sizeof(int16_t));
        if (got_samples == 0) break;

        for (size_t i = 0; i < got_samples; ++i) {
            out << raw[4*i + 0] << '\t'
                << raw[4*i + 1] << '\t'
                << raw[4*i + 2] << '\t'
                << raw[4*i + 3] << '\n';
        }
        converted += got_samples;

        if ((converted % (block_samples * 4)) == 0) {
            std::ostringstream ss;
            ss << "Конвертовано " << converted << " / " << to_convert << " семплів";
            set_status(app, ss.str());
        }
    }

    std::ostringstream ss;
    ss << "TXT готовий: " << out_path << "; семплів: " << converted;
    set_status(app, ss.str());
}

static GtkWidget *make_labeled_entry(GtkWidget *grid, const char *label, const char *def, int row) {
    GtkWidget *lab = gtk_label_new(label);
    gtk_widget_set_halign(lab, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), lab, 0, row, 1, 1);
    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(entry), def);
    gtk_grid_attach(GTK_GRID(grid), entry, 1, row, 1, 1);
    return entry;
}

int main(int argc, char **argv) {
    gtk_init(&argc, &argv);

    AppWidgets app;
    app.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(app.window), "Pluto 2RX Spectrogram + Binary to TXT");
    gtk_window_set_default_size(GTK_WINDOW(app.window), 1100, 760);
    g_signal_connect(app.window, "destroy", G_CALLBACK(gtk_main_quit), nullptr);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 8);
    gtk_container_add(GTK_CONTAINER(app.window), vbox);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_box_pack_start(GTK_BOX(vbox), grid, FALSE, FALSE, 0);

    GtkWidget *file_lab = gtk_label_new("Binary file:");
    gtk_widget_set_halign(file_lab, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), file_lab, 0, 0, 1, 1);
    app.file_entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(app.file_entry), "/mnt/ssd/pluto_2rx.bin");
    gtk_grid_attach(GTK_GRID(grid), app.file_entry, 1, 0, 1, 1);
    GtkWidget *choose_btn = gtk_button_new_with_label("Choose...");
    gtk_grid_attach(GTK_GRID(grid), choose_btn, 2, 0, 1, 1);
    g_signal_connect(choose_btn, "clicked", G_CALLBACK(on_choose_file), &app);

    app.nfft_entry = make_labeled_entry(grid, "N FFT:", "8092", 1);
    app.fs_entry = make_labeled_entry(grid, "Fs, MS/s:", "4.0", 2);
    app.max_frames_entry = make_labeled_entry(grid, "Max spectrogram frames:", "800", 3);
    app.convert_samples_entry = make_labeled_entry(grid, "TXT samples:", "100000", 4);

    GtkWidget *hbtn = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(vbox), hbtn, FALSE, FALSE, 0);
    app.spectrogram_button = gtk_button_new_with_label("Build Spectrogram RX0/RX1");
    app.convert_button = gtk_button_new_with_label("Convert selected samples to TXT");
    gtk_box_pack_start(GTK_BOX(hbtn), app.spectrogram_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hbtn), app.convert_button, FALSE, FALSE, 0);
    g_signal_connect(app.spectrogram_button, "clicked", G_CALLBACK(on_build_spectrogram), &app);
    g_signal_connect(app.convert_button, "clicked", G_CALLBACK(on_convert_txt), &app);

    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
    gtk_box_pack_start(GTK_BOX(vbox), paned, TRUE, TRUE, 0);

    app.draw_rx0 = gtk_drawing_area_new();
    app.draw_rx1 = gtk_drawing_area_new();
    gtk_widget_set_size_request(app.draw_rx0, 1000, 320);
    gtk_widget_set_size_request(app.draw_rx1, 1000, 320);
    gtk_paned_pack1(GTK_PANED(paned), app.draw_rx0, TRUE, FALSE);
    gtk_paned_pack2(GTK_PANED(paned), app.draw_rx1, TRUE, FALSE);
    g_signal_connect(app.draw_rx0, "draw", G_CALLBACK(on_draw_spectrogram), &app);
    g_signal_connect(app.draw_rx1, "draw", G_CALLBACK(on_draw_spectrogram), &app);

    app.status_label = gtk_label_new("Готово. Оберіть binary файл I0 Q0 I1 Q1, задайте N і натисніть Build Spectrogram.");
    gtk_widget_set_halign(app.status_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(vbox), app.status_label, FALSE, FALSE, 0);

    gtk_widget_show_all(app.window);
    gtk_main();
    return 0;
}
