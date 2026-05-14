// pluto_2rx_spectrum_gui.cpp
// GUI utility for ADALM-Pluto 2RX binary IQ files.
// Input binary format: int16 little-endian interleaved I0 Q0 I1 Q1 I0 Q0 I1 Q1 ...
// Functions:
//   1) Select .bin file
//   2) Build averaged spectrum over the whole recording using block length N
//   3) Convert selected number of samples to TXT without header: I0 Q0 I1 Q1
//
// Build:
// g++ -O2 -Wall -Wextra pluto_2rx_spectrum_gui.cpp -o pluto_2rx_spectrum_gui \
//     $(pkg-config --cflags --libs gtk+-3.0) -lfftw3 -lm

#include <gtk/gtk.h>
#include <cairo.h>
#include <fftw3.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

struct SpectrumData {
    int nfft = 8192;
    double fs_hz = 4.0e6;
    uint64_t total_samples = 0;
    uint64_t used_blocks = 0;
    double min_db = -120.0;
    double max_db = 0.0;
    std::vector<double> freq_mhz;   // fftshifted frequency axis, -Fs/2 ... +Fs/2
    std::vector<double> rx0_db;     // averaged PSD, fftshifted
    std::vector<double> rx1_db;     // averaged PSD, fftshifted
};

struct AppWidgets {
    GtkWidget *window = nullptr;
    GtkWidget *file_entry = nullptr;
    GtkWidget *nfft_entry = nullptr;
    GtkWidget *fs_entry = nullptr;
    GtkWidget *convert_samples_entry = nullptr;
    GtkWidget *status_label = nullptr;
    GtkWidget *draw_rx0 = nullptr;
    GtkWidget *draw_rx1 = nullptr;
    GtkWidget *spectrum_button = nullptr;
    GtkWidget *convert_button = nullptr;
    SpectrumData spec;
};

static std::string gtk_entry_text(GtkWidget *entry) {
    const char *t = gtk_entry_get_text(GTK_ENTRY(entry));
    return t ? std::string(t) : std::string();
}

static void set_status(AppWidgets *app, const std::string &msg) {
    gtk_label_set_text(GTK_LABEL(app->status_label), msg.c_str());
    while (gtk_events_pending()) gtk_main_iteration();
}

static bool parse_int64_positive(const std::string &s, uint64_t &v) {
    try {
        size_t idx = 0;
        unsigned long long x = std::stoull(s, &idx);
        if (idx != s.size() || x == 0) return false;
        v = static_cast<uint64_t>(x);
        return true;
    } catch (...) {
        return false;
    }
}

static bool parse_int_positive(const std::string &s, int &v) {
    uint64_t x = 0;
    if (!parse_int64_positive(s, x)) return false;
    if (x > static_cast<uint64_t>(std::numeric_limits<int>::max())) return false;
    v = static_cast<int>(x);
    return true;
}

static bool parse_double_positive(const std::string &s, double &v) {
    try {
        size_t idx = 0;
        double x = std::stod(s, &idx);
        if (idx != s.size() || !(x > 0.0)) return false;
        v = x;
        return true;
    } catch (...) {
        return false;
    }
}

static uint64_t file_size_bytes(const std::string &path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return 0;
    return static_cast<uint64_t>(f.tellg());
}

static std::string default_txt_name(const std::string &bin_path) {
    size_t p = bin_path.find_last_of('.');
    if (p == std::string::npos) return bin_path + ".txt";
    return bin_path.substr(0, p) + ".txt";
}

static void update_db_limits(SpectrumData &spec) {
    if (spec.rx0_db.empty() || spec.rx1_db.empty()) {
        spec.min_db = -120.0;
        spec.max_db = 0.0;
        return;
    }

    std::vector<double> vals;
    vals.reserve(spec.rx0_db.size() + spec.rx1_db.size());
    vals.insert(vals.end(), spec.rx0_db.begin(), spec.rx0_db.end());
    vals.insert(vals.end(), spec.rx1_db.begin(), spec.rx1_db.end());
    std::sort(vals.begin(), vals.end());

    size_t i1 = static_cast<size_t>(0.01 * (vals.size() - 1));
    size_t i99 = static_cast<size_t>(0.99 * (vals.size() - 1));
    spec.min_db = vals[i1];
    spec.max_db = vals[i99];

    if (spec.max_db <= spec.min_db + 5.0) {
        spec.max_db = spec.min_db + 60.0;
    }
}

static bool compute_average_spectrum_whole_file(const std::string &path,
                                                int nfft,
                                                double fs_hz,
                                                SpectrumData &spec,
                                                AppWidgets *app,
                                                std::string &err) {
    const uint64_t sz = file_size_bytes(path);
    if (sz == 0) {
        err = "Файл не відкривається або має нульовий розмір";
        return false;
    }
    if (sz % 8 != 0) {
        err = "Розмір файлу не кратний 8 байтам. Очікується int16 I0 Q0 I1 Q1";
        return false;
    }

    const uint64_t total_samples = sz / 8; // one sample contains I0,Q0,I1,Q1
    if (total_samples < static_cast<uint64_t>(nfft)) {
        err = "У файлі менше семплів, ніж N";
        return false;
    }

    const uint64_t total_blocks = total_samples / static_cast<uint64_t>(nfft); // no overlap, whole complete N-blocks
    if (total_blocks == 0) {
        err = "Немає повного FFT-блоку";
        return false;
    }

    fftw_complex *in0 = reinterpret_cast<fftw_complex *>(fftw_malloc(sizeof(fftw_complex) * nfft));
    fftw_complex *in1 = reinterpret_cast<fftw_complex *>(fftw_malloc(sizeof(fftw_complex) * nfft));
    fftw_complex *out0 = reinterpret_cast<fftw_complex *>(fftw_malloc(sizeof(fftw_complex) * nfft));
    fftw_complex *out1 = reinterpret_cast<fftw_complex *>(fftw_malloc(sizeof(fftw_complex) * nfft));
    if (!in0 || !in1 || !out0 || !out1) {
        err = "Не вдалося виділити FFTW буфери";
        if (in0) fftw_free(in0);
        if (in1) fftw_free(in1);
        if (out0) fftw_free(out0);
        if (out1) fftw_free(out1);
        return false;
    }

    fftw_plan p0 = fftw_plan_dft_1d(nfft, in0, out0, FFTW_FORWARD, FFTW_ESTIMATE);
    fftw_plan p1 = fftw_plan_dft_1d(nfft, in1, out1, FFTW_FORWARD, FFTW_ESTIMATE);
    if (!p0 || !p1) {
        err = "Не вдалося створити FFTW plan";
        if (p0) fftw_destroy_plan(p0);
        if (p1) fftw_destroy_plan(p1);
        fftw_free(in0); fftw_free(in1); fftw_free(out0); fftw_free(out1);
        return false;
    }

    std::vector<double> win(nfft);
    double win_power = 0.0;
    for (int n = 0; n < nfft; ++n) {
        win[n] = 0.5 - 0.5 * std::cos(2.0 * M_PI * n / static_cast<double>(nfft - 1));
        win_power += win[n] * win[n];
    }

    std::vector<double> acc0(nfft, 0.0);
    std::vector<double> acc1(nfft, 0.0);
    std::vector<int16_t> raw(static_cast<size_t>(nfft) * 4);

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        err = "Не вдалося відкрити файл";
        fftw_destroy_plan(p0); fftw_destroy_plan(p1);
        fftw_free(in0); fftw_free(in1); fftw_free(out0); fftw_free(out1);
        return false;
    }

    uint64_t used_blocks = 0;
    for (uint64_t b = 0; b < total_blocks; ++b) {
        in.read(reinterpret_cast<char *>(raw.data()), static_cast<std::streamsize>(raw.size() * sizeof(int16_t)));
        const size_t got_bytes = static_cast<size_t>(in.gcount());
        if (got_bytes < raw.size() * sizeof(int16_t)) break;

        long double m0r = 0.0, m0i = 0.0, m1r = 0.0, m1i = 0.0;
        for (int n = 0; n < nfft; ++n) {
            m0r += raw[4*n + 0];
            m0i += raw[4*n + 1];
            m1r += raw[4*n + 2];
            m1i += raw[4*n + 3];
        }
        const double dc0r = static_cast<double>(m0r / nfft);
        const double dc0i = static_cast<double>(m0i / nfft);
        const double dc1r = static_cast<double>(m1r / nfft);
        const double dc1i = static_cast<double>(m1i / nfft);

        for (int n = 0; n < nfft; ++n) {
            const double w = win[n];
            in0[n][0] = (static_cast<double>(raw[4*n + 0]) - dc0r) * w;
            in0[n][1] = (static_cast<double>(raw[4*n + 1]) - dc0i) * w;
            in1[n][0] = (static_cast<double>(raw[4*n + 2]) - dc1r) * w;
            in1[n][1] = (static_cast<double>(raw[4*n + 3]) - dc1i) * w;
        }

        fftw_execute(p0);
        fftw_execute(p1);

        for (int k = 0; k < nfft; ++k) {
            const double pwr0 = out0[k][0] * out0[k][0] + out0[k][1] * out0[k][1];
            const double pwr1 = out1[k][0] * out1[k][0] + out1[k][1] * out1[k][1];
            acc0[k] += pwr0;
            acc1[k] += pwr1;
        }
        ++used_blocks;

        if ((b % 64) == 0 || b + 1 == total_blocks) {
            std::ostringstream ss;
            ss << "FFT average: блок " << (b + 1) << " / " << total_blocks;
            set_status(app, ss.str());
        }
    }

    fftw_destroy_plan(p0);
    fftw_destroy_plan(p1);
    fftw_free(in0); fftw_free(in1); fftw_free(out0); fftw_free(out1);

    if (used_blocks == 0) {
        err = "Не прочитано жодного повного FFT-блоку";
        return false;
    }

    spec.nfft = nfft;
    spec.fs_hz = fs_hz;
    spec.total_samples = total_samples;
    spec.used_blocks = used_blocks;
    spec.freq_mhz.assign(nfft, 0.0);
    spec.rx0_db.assign(nfft, 0.0);
    spec.rx1_db.assign(nfft, 0.0);

    const double norm = static_cast<double>(used_blocks) * win_power;
    for (int k = 0; k < nfft; ++k) {
        const int ks = (k + nfft / 2) % nfft; // fftshift source bin
        const double f_hz = (static_cast<double>(k) / nfft - 0.5) * fs_hz;
        spec.freq_mhz[k] = f_hz / 1e6;
        spec.rx0_db[k] = 10.0 * std::log10(acc0[ks] / norm + 1e-30);
        spec.rx1_db[k] = 10.0 * std::log10(acc1[ks] / norm + 1e-30);
    }

    update_db_limits(spec);
    return true;
}

static gboolean on_draw_spectrum(GtkWidget *widget, cairo_t *cr, gpointer user_data) {
    AppWidgets *app = static_cast<AppWidgets *>(user_data);
    const bool is_rx1 = (widget == app->draw_rx1);
    const SpectrumData &s = app->spec;
    const std::vector<double> &y = is_rx1 ? s.rx1_db : s.rx0_db;

    const int W = gtk_widget_get_allocated_width(widget);
    const int H = gtk_widget_get_allocated_height(widget);

    cairo_set_source_rgb(cr, 0.05, 0.05, 0.05);
    cairo_paint(cr);

    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 14);
    cairo_move_to(cr, 10, 20);
    cairo_show_text(cr, is_rx1 ? "RX1 averaged spectrum over whole recording" : "RX0 averaged spectrum over whole recording");

    if (y.empty()) {
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 13);
        cairo_move_to(cr, 10, 48);
        cairo_show_text(cr, "Оберіть файл, задайте N і натисніть Build Average Spectrum");
        return FALSE;
    }

    const int left = 70, right = 20, top = 35, bottom = 45;
    const int plotW = std::max(1, W - left - right);
    const int plotH = std::max(1, H - top - bottom);
    const double ymin = s.min_db;
    const double ymax = s.max_db;

    // Grid and border
    cairo_set_source_rgb(cr, 0.18, 0.18, 0.18);
    cairo_set_line_width(cr, 1.0);
    for (int i = 0; i <= 5; ++i) {
        double yy = top + plotH * i / 5.0;
        cairo_move_to(cr, left, yy);
        cairo_line_to(cr, left + plotW, yy);
    }
    for (int i = 0; i <= 8; ++i) {
        double xx = left + plotW * i / 8.0;
        cairo_move_to(cr, xx, top);
        cairo_line_to(cr, xx, top + plotH);
    }
    cairo_stroke(cr);

    cairo_set_source_rgb(cr, 0.85, 0.85, 0.85);
    cairo_rectangle(cr, left, top, plotW, plotH);
    cairo_stroke(cr);

    // Spectrum curve
    cairo_set_source_rgb(cr, 0.20, 0.85, 0.35);
    cairo_set_line_width(cr, 1.2);
    for (int k = 0; k < s.nfft; ++k) {
        double x = left + plotW * (static_cast<double>(k) / (s.nfft - 1));
        double t = (y[k] - ymin) / (ymax - ymin);
        t = std::max(0.0, std::min(1.0, t));
        double yy = top + plotH * (1.0 - t);
        if (k == 0) cairo_move_to(cr, x, yy);
        else cairo_line_to(cr, x, yy);
    }
    cairo_stroke(cr);

    // Labels
    cairo_set_source_rgb(cr, 0.95, 0.95, 0.95);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 11);

    char buf[160];
    for (int i = 0; i <= 5; ++i) {
        double val = ymax - (ymax - ymin) * i / 5.0;
        double yy = top + plotH * i / 5.0 + 4;
        snprintf(buf, sizeof(buf), "%.1f dB", val);
        cairo_move_to(cr, 5, yy);
        cairo_show_text(cr, buf);
    }

    snprintf(buf, sizeof(buf), "-%.3f MHz", s.fs_hz / 2.0 / 1e6);
    cairo_move_to(cr, left, H - 15);
    cairo_show_text(cr, buf);
    snprintf(buf, sizeof(buf), "0");
    cairo_move_to(cr, left + plotW / 2 - 4, H - 15);
    cairo_show_text(cr, buf);
    snprintf(buf, sizeof(buf), "+%.3f MHz", s.fs_hz / 2.0 / 1e6);
    cairo_move_to(cr, left + plotW - 75, H - 15);
    cairo_show_text(cr, buf);

    snprintf(buf, sizeof(buf), "N=%d, averaged blocks=%llu, samples=%llu",
             s.nfft,
             static_cast<unsigned long long>(s.used_blocks),
             static_cast<unsigned long long>(s.total_samples));
    cairo_move_to(cr, left, top + 16);
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

static void on_build_spectrum(GtkButton *, gpointer user_data) {
    AppWidgets *app = static_cast<AppWidgets *>(user_data);
    const std::string path = gtk_entry_text(app->file_entry);
    if (path.empty()) {
        set_status(app, "Помилка: оберіть .bin файл");
        return;
    }

    int nfft = 0;
    double fs_mhz = 0.0;
    if (!parse_int_positive(gtk_entry_text(app->nfft_entry), nfft)) {
        set_status(app, "Помилка: N має бути додатнім числом, наприклад 8092 або 8192");
        return;
    }
    if (nfft < 256 || nfft > 262144) {
        set_status(app, "Помилка: N поза межами 256...262144");
        return;
    }
    if (!parse_double_positive(gtk_entry_text(app->fs_entry), fs_mhz)) {
        set_status(app, "Помилка: Fs має бути додатнім числом у MS/s");
        return;
    }

    set_status(app, "Обчислюю усереднений спектр по всьому запису...");
    std::string err;
    if (!compute_average_spectrum_whole_file(path, nfft, fs_mhz * 1e6, app->spec, app, err)) {
        set_status(app, "Помилка: " + err);
        return;
    }

    gtk_widget_queue_draw(app->draw_rx0);
    gtk_widget_queue_draw(app->draw_rx1);

    std::ostringstream ss;
    ss << "Готово. Усереднено " << app->spec.used_blocks
       << " FFT-блоків по N=" << app->spec.nfft
       << "; семплів у файлі: " << app->spec.total_samples
       << "; залишок без обробки: " << (app->spec.total_samples % app->spec.nfft);
    set_status(app, ss.str());
}

static void on_convert_txt(GtkButton *, gpointer user_data) {
    AppWidgets *app = static_cast<AppWidgets *>(user_data);
    const std::string path = gtk_entry_text(app->file_entry);
    if (path.empty()) {
        set_status(app, "Помилка: оберіть .bin файл");
        return;
    }

    uint64_t nsamp = 0;
    if (!parse_int64_positive(gtk_entry_text(app->convert_samples_entry), nsamp)) {
        set_status(app, "Помилка: кількість семплів для TXT має бути додатнім числом");
        return;
    }

    const uint64_t sz = file_size_bytes(path);
    if (sz == 0 || sz % 8 != 0) {
        set_status(app, "Помилка: файл порожній або не має формату int16 I0 Q0 I1 Q1");
        return;
    }
    const uint64_t total_samples = sz / 8;
    const uint64_t to_convert = std::min<uint64_t>(total_samples, nsamp);

    GtkWidget *dialog = gtk_file_chooser_dialog_new("Зберегти TXT без заголовка",
                                                    GTK_WINDOW(app->window),
                                                    GTK_FILE_CHOOSER_ACTION_SAVE,
                                                    "Cancel", GTK_RESPONSE_CANCEL,
                                                    "Save", GTK_RESPONSE_ACCEPT,
                                                    nullptr);
    const std::string def = default_txt_name(path);
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

    set_status(app, "Конвертую binary -> TXT без заголовка...");

    const size_t block_samples = 262144;
    std::vector<int16_t> raw(block_samples * 4);
    uint64_t converted = 0;

    while (converted < to_convert) {
        const uint64_t remain = to_convert - converted;
        const size_t n = static_cast<size_t>(std::min<uint64_t>(remain, block_samples));
        in.read(reinterpret_cast<char *>(raw.data()), static_cast<std::streamsize>(n * 4 * sizeof(int16_t)));
        const size_t got_bytes = static_cast<size_t>(in.gcount());
        const size_t got_samples = got_bytes / (4 * sizeof(int16_t));
        if (got_samples == 0) break;

        for (size_t i = 0; i < got_samples; ++i) {
            // Без назви стовпців. Формат рядка: I0 Q0 I1 Q1
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
    ss << "TXT готовий: " << out_path << "; семплів: " << converted << "; заголовок відсутній";
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
    gtk_window_set_title(GTK_WINDOW(app.window), "Pluto 2RX Average Spectrum + Binary to TXT");
    gtk_window_set_default_size(GTK_WINDOW(app.window), 1100, 720);
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

    app.nfft_entry = make_labeled_entry(grid, "N FFT / block:", "8092", 1);
    app.fs_entry = make_labeled_entry(grid, "Fs, MS/s:", "4.0", 2);
    app.convert_samples_entry = make_labeled_entry(grid, "TXT samples:", "100000", 3);

    GtkWidget *hbtn = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(vbox), hbtn, FALSE, FALSE, 0);

    app.spectrum_button = gtk_button_new_with_label("Build Average Spectrum RX0/RX1 over whole file");
    app.convert_button = gtk_button_new_with_label("Convert selected samples to TXT without header");
    gtk_box_pack_start(GTK_BOX(hbtn), app.spectrum_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hbtn), app.convert_button, FALSE, FALSE, 0);
    g_signal_connect(app.spectrum_button, "clicked", G_CALLBACK(on_build_spectrum), &app);
    g_signal_connect(app.convert_button, "clicked", G_CALLBACK(on_convert_txt), &app);

    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
    gtk_box_pack_start(GTK_BOX(vbox), paned, TRUE, TRUE, 0);

    app.draw_rx0 = gtk_drawing_area_new();
    app.draw_rx1 = gtk_drawing_area_new();
    gtk_widget_set_size_request(app.draw_rx0, 1000, 300);
    gtk_widget_set_size_request(app.draw_rx1, 1000, 300);
    gtk_paned_pack1(GTK_PANED(paned), app.draw_rx0, TRUE, FALSE);
    gtk_paned_pack2(GTK_PANED(paned), app.draw_rx1, TRUE, FALSE);
    g_signal_connect(app.draw_rx0, "draw", G_CALLBACK(on_draw_spectrum), &app);
    g_signal_connect(app.draw_rx1, "draw", G_CALLBACK(on_draw_spectrum), &app);

    app.status_label = gtk_label_new("Готово. Оберіть binary файл I0 Q0 I1 Q1, задайте N і натисніть Build Average Spectrum.");
    gtk_widget_set_halign(app.status_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(vbox), app.status_label, FALSE, FALSE, 0);

    gtk_widget_show_all(app.window);
    gtk_main();
    return 0;
}
