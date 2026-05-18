/*
 * pluto_2rx_spectrum_zoom_gui.cpp
 *
 * Raspberry Pi 4 + PlutoSDR 2RX binary IQ analyzer
 * --------------------------------------------------
 * Input binary format:
 *   int16 little-endian interleaved samples:
 *   I0 Q0 I1 Q1 I0 Q0 I1 Q1 ...
 *
 * Functions:
 *   1) Select .bin file from GUI
 *   2) Build averaged spectrum over the whole recording using FFT block length N
 *   3) Display RX0 and RX1 spectra with interactive zoom/pan
 *   4) Convert selected number of samples from binary to TXT without header
 *      TXT format per line:
 *        I0 Q0 I1 Q1
 *
 * Dependencies:
 *   sudo apt install -y g++ pkg-config libgtk-3-dev libfftw3-dev
 *
 * Build:
 *   g++ -O2 -Wall -Wextra pluto_2rx_spectrum_zoom_gui.cpp -o pluto_2rx_spectrum_zoom_gui \
 *       $(pkg-config --cflags --libs gtk+-3.0) -lfftw3 -lm
 *
 * Run:
 *   ./pluto_2rx_spectrum_zoom_gui
 */

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

struct SpectrumData {
    std::vector<double> freq_mhz;
    std::vector<double> rx0_db;
    std::vector<double> rx1_db;
    bool valid = false;
    int fft_n = 8092;
    double fs_mhz = 4.0;
    long long total_samples = 0;
    long long used_blocks = 0;
};

struct PlotState {
    double x_min = -2.0;
    double x_max =  2.0;
    double y_min = -120.0;
    double y_max =  0.0;

    double full_x_min = -2.0;
    double full_x_max =  2.0;
    double full_y_min = -120.0;
    double full_y_max =  0.0;

    bool dragging = false;
    double drag_last_x = 0.0;
    double drag_last_y = 0.0;
};

struct AppWidgets {
    GtkWidget *window = nullptr;
    GtkWidget *file_chooser = nullptr;
    GtkWidget *entry_fft_n = nullptr;
    GtkWidget *entry_fs_mhz = nullptr;
    GtkWidget *entry_txt_samples = nullptr;
    GtkWidget *drawing_area = nullptr;
    GtkWidget *status_label = nullptr;
    GtkWidget *check_rx0 = nullptr;
    GtkWidget *check_rx1 = nullptr;

    SpectrumData spec;
    PlotState plot;
};

static std::string get_selected_filename(GtkWidget *chooser)
{
    char *fname = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser));
    if (!fname) return std::string();
    std::string s(fname);
    g_free(fname);
    return s;
}

static void set_status(AppWidgets *app, const std::string &msg)
{
    gtk_label_set_text(GTK_LABEL(app->status_label), msg.c_str());
    while (gtk_events_pending()) gtk_main_iteration();
}

static bool parse_int_entry(GtkWidget *entry, int &value, int minv, int maxv)
{
    const char *txt = gtk_entry_get_text(GTK_ENTRY(entry));
    if (!txt || !*txt) return false;
    char *end = nullptr;
    long v = std::strtol(txt, &end, 10);
    if (end == txt || *end != '\0') return false;
    if (v < minv || v > maxv) return false;
    value = static_cast<int>(v);
    return true;
}

static bool parse_ll_entry(GtkWidget *entry, long long &value, long long minv, long long maxv)
{
    const char *txt = gtk_entry_get_text(GTK_ENTRY(entry));
    if (!txt || !*txt) return false;
    char *end = nullptr;
    long long v = std::strtoll(txt, &end, 10);
    if (end == txt || *end != '\0') return false;
    if (v < minv || v > maxv) return false;
    value = v;
    return true;
}

static bool parse_double_entry(GtkWidget *entry, double &value, double minv, double maxv)
{
    const char *txt = gtk_entry_get_text(GTK_ENTRY(entry));
    if (!txt || !*txt) return false;
    char *end = nullptr;
    double v = std::strtod(txt, &end);
    if (end == txt || *end != '\0') return false;
    if (v < minv || v > maxv) return false;
    value = v;
    return true;
}

static void show_error(GtkWindow *parent, const std::string &msg)
{
    GtkWidget *dialog = gtk_message_dialog_new(parent,
                                               GTK_DIALOG_MODAL,
                                               GTK_MESSAGE_ERROR,
                                               GTK_BUTTONS_OK,
                                               "%s", msg.c_str());
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

static void show_info(GtkWindow *parent, const std::string &msg)
{
    GtkWidget *dialog = gtk_message_dialog_new(parent,
                                               GTK_DIALOG_MODAL,
                                               GTK_MESSAGE_INFO,
                                               GTK_BUTTONS_OK,
                                               "%s", msg.c_str());
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

static void fftshift(std::vector<double> &v)
{
    const size_t n = v.size();
    const size_t h = n / 2;
    std::rotate(v.begin(), v.begin() + h, v.end());
}

static bool build_average_spectrum(const std::string &filename,
                                   int N,
                                   double fs_mhz,
                                   SpectrumData &out,
                                   std::string &err)
{
    std::ifstream fin(filename, std::ios::binary);
    if (!fin) {
        err = "Не вдалося відкрити binary-файл.";
        return false;
    }

    fin.seekg(0, std::ios::end);
    const std::streamoff bytes = fin.tellg();
    fin.seekg(0, std::ios::beg);

    const std::streamoff bytes_per_sample_2rx = 4 * static_cast<std::streamoff>(sizeof(int16_t));
    if (bytes < bytes_per_sample_2rx * N) {
        err = "Файл занадто малий для заданого N.";
        return false;
    }

    const long long total_samples = static_cast<long long>(bytes / bytes_per_sample_2rx);
    const long long blocks = total_samples / N;
    if (blocks <= 0) {
        err = "Недостатньо повних FFT-блоків.";
        return false;
    }

    std::vector<int16_t> raw(static_cast<size_t>(N) * 4);
    std::vector<double> acc0(N, 0.0), acc1(N, 0.0);
    std::vector<double> window(N);

    double win_power = 0.0;
    for (int i = 0; i < N; ++i) {
        window[i] = 0.5 - 0.5 * std::cos(2.0 * M_PI * i / (N - 1)); // Hann
        win_power += window[i] * window[i];
    }

    fftw_complex *in0  = reinterpret_cast<fftw_complex *>(fftw_malloc(sizeof(fftw_complex) * N));
    fftw_complex *out0 = reinterpret_cast<fftw_complex *>(fftw_malloc(sizeof(fftw_complex) * N));
    fftw_complex *in1  = reinterpret_cast<fftw_complex *>(fftw_malloc(sizeof(fftw_complex) * N));
    fftw_complex *out1 = reinterpret_cast<fftw_complex *>(fftw_malloc(sizeof(fftw_complex) * N));

    if (!in0 || !out0 || !in1 || !out1) {
        err = "Помилка виділення пам'яті FFTW.";
        if (in0) fftw_free(in0);
        if (out0) fftw_free(out0);
        if (in1) fftw_free(in1);
        if (out1) fftw_free(out1);
        return false;
    }

    fftw_plan p0 = fftw_plan_dft_1d(N, in0, out0, FFTW_FORWARD, FFTW_ESTIMATE);
    fftw_plan p1 = fftw_plan_dft_1d(N, in1, out1, FFTW_FORWARD, FFTW_ESTIMATE);

    if (!p0 || !p1) {
        err = "Помилка створення FFTW-плану.";
        if (p0) fftw_destroy_plan(p0);
        if (p1) fftw_destroy_plan(p1);
        fftw_free(in0); fftw_free(out0); fftw_free(in1); fftw_free(out1);
        return false;
    }

    const long long max_blocks = blocks; // use whole file
    for (long long b = 0; b < max_blocks; ++b) {
        fin.read(reinterpret_cast<char *>(raw.data()), raw.size() * sizeof(int16_t));
        if (!fin) break;

        double mean_i0 = 0.0, mean_q0 = 0.0, mean_i1 = 0.0, mean_q1 = 0.0;
        for (int i = 0; i < N; ++i) {
            mean_i0 += raw[4 * i + 0];
            mean_q0 += raw[4 * i + 1];
            mean_i1 += raw[4 * i + 2];
            mean_q1 += raw[4 * i + 3];
        }
        mean_i0 /= N; mean_q0 /= N; mean_i1 /= N; mean_q1 /= N;

        for (int i = 0; i < N; ++i) {
            const double w = window[i];
            in0[i][0] = (static_cast<double>(raw[4 * i + 0]) - mean_i0) * w;
            in0[i][1] = (static_cast<double>(raw[4 * i + 1]) - mean_q0) * w;
            in1[i][0] = (static_cast<double>(raw[4 * i + 2]) - mean_i1) * w;
            in1[i][1] = (static_cast<double>(raw[4 * i + 3]) - mean_q1) * w;
        }

        fftw_execute(p0);
        fftw_execute(p1);

        for (int k = 0; k < N; ++k) {
            const double re0 = out0[k][0];
            const double im0 = out0[k][1];
            const double re1 = out1[k][0];
            const double im1 = out1[k][1];
            acc0[k] += (re0 * re0 + im0 * im0) / win_power;
            acc1[k] += (re1 * re1 + im1 * im1) / win_power;
        }
    }

    fftw_destroy_plan(p0);
    fftw_destroy_plan(p1);
    fftw_free(in0); fftw_free(out0); fftw_free(in1); fftw_free(out1);

    for (int k = 0; k < N; ++k) {
        acc0[k] /= static_cast<double>(max_blocks);
        acc1[k] /= static_cast<double>(max_blocks);
    }

    fftshift(acc0);
    fftshift(acc1);

    out.freq_mhz.resize(N);
    out.rx0_db.resize(N);
    out.rx1_db.resize(N);

    for (int k = 0; k < N; ++k) {
        const double f = (static_cast<double>(k) - static_cast<double>(N) / 2.0) * fs_mhz / static_cast<double>(N);
        out.freq_mhz[k] = f;
        out.rx0_db[k] = 10.0 * std::log10(acc0[k] + 1e-20);
        out.rx1_db[k] = 10.0 * std::log10(acc1[k] + 1e-20);
    }

    out.valid = true;
    out.fft_n = N;
    out.fs_mhz = fs_mhz;
    out.total_samples = total_samples;
    out.used_blocks = max_blocks;
    return true;
}

static void autoscale_plot(AppWidgets *app)
{
    if (!app->spec.valid || app->spec.freq_mhz.empty()) return;

    double xmin = app->spec.freq_mhz.front();
    double xmax = app->spec.freq_mhz.back();
    double ymin = std::numeric_limits<double>::infinity();
    double ymax = -std::numeric_limits<double>::infinity();

    for (double v : app->spec.rx0_db) { ymin = std::min(ymin, v); ymax = std::max(ymax, v); }
    for (double v : app->spec.rx1_db) { ymin = std::min(ymin, v); ymax = std::max(ymax, v); }

    if (!std::isfinite(ymin) || !std::isfinite(ymax) || ymin == ymax) {
        ymin = -120.0;
        ymax = 0.0;
    }

    const double ypad = std::max(3.0, (ymax - ymin) * 0.08);
    ymin -= ypad;
    ymax += ypad;

    app->plot.full_x_min = xmin;
    app->plot.full_x_max = xmax;
    app->plot.full_y_min = ymin;
    app->plot.full_y_max = ymax;

    app->plot.x_min = xmin;
    app->plot.x_max = xmax;
    app->plot.y_min = ymin;
    app->plot.y_max = ymax;
}

static double map_x(const PlotState &p, double x, int left, int width)
{
    return left + (x - p.x_min) / (p.x_max - p.x_min) * width;
}

static double map_y(const PlotState &p, double y, int top, int height)
{
    return top + (p.y_max - y) / (p.y_max - p.y_min) * height;
}

static double inv_x(const PlotState &p, double sx, int left, int width)
{
    return p.x_min + (sx - left) / width * (p.x_max - p.x_min);
}

static double inv_y(const PlotState &p, double sy, int top, int height)
{
    return p.y_max - (sy - top) / height * (p.y_max - p.y_min);
}

static void draw_text(cairo_t *cr, double x, double y, const std::string &txt, double size = 12.0)
{
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, size);
    cairo_move_to(cr, x, y);
    cairo_show_text(cr, txt.c_str());
}

static gboolean on_draw(GtkWidget *widget, cairo_t *cr, gpointer user_data)
{
    AppWidgets *app = static_cast<AppWidgets *>(user_data);

    const int W = gtk_widget_get_allocated_width(widget);
    const int H = gtk_widget_get_allocated_height(widget);

    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_paint(cr);

    const int left = 70;
    const int right = 25;
    const int top = 35;
    const int bottom = 55;
    const int pw = std::max(10, W - left - right);
    const int ph = std::max(10, H - top - bottom);

    cairo_set_source_rgb(cr, 0, 0, 0);
    draw_text(cr, 20, 22, "Averaged spectrum over whole recording. Wheel = zoom, Left drag = pan, Reset Zoom = full view", 12);

    cairo_rectangle(cr, left, top, pw, ph);
    cairo_set_source_rgb(cr, 0.97, 0.97, 0.97);
    cairo_fill_preserve(cr);
    cairo_set_source_rgb(cr, 0, 0, 0);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);

    if (!app->spec.valid) {
        cairo_set_source_rgb(cr, 0.1, 0.1, 0.1);
        draw_text(cr, left + 25, top + 35, "Select binary file and press Build spectrum", 16);
        return FALSE;
    }

    const PlotState &p = app->plot;

    // grid
    cairo_set_source_rgb(cr, 0.82, 0.82, 0.82);
    cairo_set_line_width(cr, 0.6);
    for (int i = 0; i <= 10; ++i) {
        double sx = left + i * pw / 10.0;
        cairo_move_to(cr, sx, top);
        cairo_line_to(cr, sx, top + ph);
    }
    for (int i = 0; i <= 8; ++i) {
        double sy = top + i * ph / 8.0;
        cairo_move_to(cr, left, sy);
        cairo_line_to(cr, left + pw, sy);
    }
    cairo_stroke(cr);

    // axis labels
    cairo_set_source_rgb(cr, 0, 0, 0);
    for (int i = 0; i <= 10; ++i) {
        double xval = p.x_min + i * (p.x_max - p.x_min) / 10.0;
        double sx = left + i * pw / 10.0;
        std::ostringstream ss;
        ss << std::fixed << std::setprecision(2) << xval;
        draw_text(cr, sx - 18, top + ph + 20, ss.str(), 10);
    }
    for (int i = 0; i <= 8; ++i) {
        double yval = p.y_max - i * (p.y_max - p.y_min) / 8.0;
        double sy = top + i * ph / 8.0;
        std::ostringstream ss;
        ss << std::fixed << std::setprecision(1) << yval;
        draw_text(cr, 8, sy + 4, ss.str(), 10);
    }

    draw_text(cr, left + pw / 2.0 - 45, H - 12, "Frequency, MHz", 12);
    draw_text(cr, 8, top - 12, "Power, dB", 12);

    cairo_save(cr);
    cairo_rectangle(cr, left, top, pw, ph);
    cairo_clip(cr);

    auto draw_curve = [&](const std::vector<double> &y, double r, double g, double b) {
        if (app->spec.freq_mhz.size() < 2 || y.size() != app->spec.freq_mhz.size()) return;
        cairo_set_source_rgb(cr, r, g, b);
        cairo_set_line_width(cr, 1.1);

        bool started = false;
        for (size_t i = 0; i < app->spec.freq_mhz.size(); ++i) {
            double xval = app->spec.freq_mhz[i];
            if (xval < p.x_min || xval > p.x_max) continue;
            double sx = map_x(p, xval, left, pw);
            double sy = map_y(p, y[i], top, ph);
            if (!started) {
                cairo_move_to(cr, sx, sy);
                started = true;
            } else {
                cairo_line_to(cr, sx, sy);
            }
        }
        if (started) cairo_stroke(cr);
    };

    const bool show_rx0 = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(app->check_rx0));
    const bool show_rx1 = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(app->check_rx1));

    if (show_rx0) draw_curve(app->spec.rx0_db, 0.10, 0.25, 0.95);
    if (show_rx1) draw_curve(app->spec.rx1_db, 0.85, 0.15, 0.10);

    cairo_restore(cr);

    // legend
    int lx = left + pw - 160;
    int ly = top + 18;
    if (show_rx0) {
        cairo_set_source_rgb(cr, 0.10, 0.25, 0.95);
        cairo_move_to(cr, lx, ly);
        cairo_line_to(cr, lx + 28, ly);
        cairo_stroke(cr);
        cairo_set_source_rgb(cr, 0, 0, 0);
        draw_text(cr, lx + 35, ly + 4, "RX0", 11);
        ly += 18;
    }
    if (show_rx1) {
        cairo_set_source_rgb(cr, 0.85, 0.15, 0.10);
        cairo_move_to(cr, lx, ly);
        cairo_line_to(cr, lx + 28, ly);
        cairo_stroke(cr);
        cairo_set_source_rgb(cr, 0, 0, 0);
        draw_text(cr, lx + 35, ly + 4, "RX1", 11);
    }

    std::ostringstream info;
    info << "N=" << app->spec.fft_n
         << "   Fs=" << app->spec.fs_mhz << " MS/s"
         << "   Samples=" << app->spec.total_samples
         << "   Blocks=" << app->spec.used_blocks;
    cairo_set_source_rgb(cr, 0, 0, 0);
    draw_text(cr, left + 5, top + 16, info.str(), 11);

    return FALSE;
}

static gboolean on_scroll(GtkWidget *widget, GdkEventScroll *event, gpointer user_data)
{
    AppWidgets *app = static_cast<AppWidgets *>(user_data);
    if (!app->spec.valid) return FALSE;

    const int W = gtk_widget_get_allocated_width(widget);
    const int H = gtk_widget_get_allocated_height(widget);
    const int left = 70, right = 25, top = 35, bottom = 55;
    const int pw = std::max(10, W - left - right);
    const int ph = std::max(10, H - top - bottom);

    if (event->x < left || event->x > left + pw || event->y < top || event->y > top + ph) return FALSE;

    double factor = 1.0;
    if (event->direction == GDK_SCROLL_UP) factor = 0.80;
    else if (event->direction == GDK_SCROLL_DOWN) factor = 1.25;
    else return FALSE;

    double cx = inv_x(app->plot, event->x, left, pw);
    double cy = inv_y(app->plot, event->y, top, ph);

    double new_x_min = cx - (cx - app->plot.x_min) * factor;
    double new_x_max = cx + (app->plot.x_max - cx) * factor;
    double new_y_min = cy - (cy - app->plot.y_min) * factor;
    double new_y_max = cy + (app->plot.y_max - cy) * factor;

    if ((new_x_max - new_x_min) > 1e-9 && (new_y_max - new_y_min) > 1e-9) {
        app->plot.x_min = new_x_min;
        app->plot.x_max = new_x_max;
        app->plot.y_min = new_y_min;
        app->plot.y_max = new_y_max;
    }

    gtk_widget_queue_draw(widget);
    return TRUE;
}

static gboolean on_button_press(GtkWidget *, GdkEventButton *event, gpointer user_data)
{
    AppWidgets *app = static_cast<AppWidgets *>(user_data);
    if (event->button == 1 && app->spec.valid) {
        app->plot.dragging = true;
        app->plot.drag_last_x = event->x;
        app->plot.drag_last_y = event->y;
        return TRUE;
    }
    return FALSE;
}

static gboolean on_button_release(GtkWidget *, GdkEventButton *event, gpointer user_data)
{
    AppWidgets *app = static_cast<AppWidgets *>(user_data);
    if (event->button == 1) {
        app->plot.dragging = false;
        return TRUE;
    }
    return FALSE;
}

static gboolean on_motion(GtkWidget *widget, GdkEventMotion *event, gpointer user_data)
{
    AppWidgets *app = static_cast<AppWidgets *>(user_data);
    if (!app->plot.dragging || !app->spec.valid) return FALSE;

    const int W = gtk_widget_get_allocated_width(widget);
    const int H = gtk_widget_get_allocated_height(widget);
    const int left = 70, right = 25, top = 35, bottom = 55;
    const int pw = std::max(10, W - left - right);
    const int ph = std::max(10, H - top - bottom);

    const double dx_pix = event->x - app->plot.drag_last_x;
    const double dy_pix = event->y - app->plot.drag_last_y;

    const double dx_val = -dx_pix / pw * (app->plot.x_max - app->plot.x_min);
    const double dy_val =  dy_pix / ph * (app->plot.y_max - app->plot.y_min);

    app->plot.x_min += dx_val;
    app->plot.x_max += dx_val;
    app->plot.y_min += dy_val;
    app->plot.y_max += dy_val;

    app->plot.drag_last_x = event->x;
    app->plot.drag_last_y = event->y;

    gtk_widget_queue_draw(widget);
    return TRUE;
}

static void on_build_spectrum(GtkButton *, gpointer user_data)
{
    AppWidgets *app = static_cast<AppWidgets *>(user_data);
    std::string fname = get_selected_filename(app->file_chooser);
    if (fname.empty()) {
        show_error(GTK_WINDOW(app->window), "Оберіть binary-файл.");
        return;
    }

    int N = 0;
    double fs_mhz = 0.0;
    if (!parse_int_entry(app->entry_fft_n, N, 256, 1048576)) {
        show_error(GTK_WINDOW(app->window), "Некоректне N FFT. Допустимо 256...1048576. Наприклад 8092.");
        return;
    }
    if (!parse_double_entry(app->entry_fs_mhz, fs_mhz, 0.001, 1000.0)) {
        show_error(GTK_WINDOW(app->window), "Некоректна Fs у MS/s.");
        return;
    }

    set_status(app, "Обчислення спектра по всьому файлу...");
    SpectrumData tmp;
    std::string err;
    if (!build_average_spectrum(fname, N, fs_mhz, tmp, err)) {
        set_status(app, "Помилка.");
        show_error(GTK_WINDOW(app->window), err);
        return;
    }

    app->spec = std::move(tmp);
    autoscale_plot(app);
    gtk_widget_queue_draw(app->drawing_area);

    std::ostringstream ss;
    ss << "Готово. Побудовано усереднений спектр: samples=" << app->spec.total_samples
       << ", blocks=" << app->spec.used_blocks;
    set_status(app, ss.str());
}

static void on_reset_zoom(GtkButton *, gpointer user_data)
{
    AppWidgets *app = static_cast<AppWidgets *>(user_data);
    if (!app->spec.valid) return;
    app->plot.x_min = app->plot.full_x_min;
    app->plot.x_max = app->plot.full_x_max;
    app->plot.y_min = app->plot.full_y_min;
    app->plot.y_max = app->plot.full_y_max;
    gtk_widget_queue_draw(app->drawing_area);
}

static void on_toggle_curve(GtkToggleButton *, gpointer user_data)
{
    AppWidgets *app = static_cast<AppWidgets *>(user_data);
    gtk_widget_queue_draw(app->drawing_area);
}

static void on_convert_txt(GtkButton *, gpointer user_data)
{
    AppWidgets *app = static_cast<AppWidgets *>(user_data);
    std::string fname = get_selected_filename(app->file_chooser);
    if (fname.empty()) {
        show_error(GTK_WINDOW(app->window), "Оберіть binary-файл.");
        return;
    }

    long long samples_to_convert = 0;
    if (!parse_ll_entry(app->entry_txt_samples, samples_to_convert, 1, 1000000000LL)) {
        show_error(GTK_WINDOW(app->window), "Некоректна кількість семплів для TXT.");
        return;
    }

    GtkWidget *dialog = gtk_file_chooser_dialog_new("Зберегти TXT",
                                                    GTK_WINDOW(app->window),
                                                    GTK_FILE_CHOOSER_ACTION_SAVE,
                                                    "Cancel", GTK_RESPONSE_CANCEL,
                                                    "Save", GTK_RESPONSE_ACCEPT,
                                                    NULL);
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dialog), "converted_iq.txt");

    int response = gtk_dialog_run(GTK_DIALOG(dialog));
    if (response != GTK_RESPONSE_ACCEPT) {
        gtk_widget_destroy(dialog);
        return;
    }

    char *outname_c = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
    std::string outname = outname_c ? outname_c : "converted_iq.txt";
    g_free(outname_c);
    gtk_widget_destroy(dialog);

    std::ifstream fin(fname, std::ios::binary);
    if (!fin) {
        show_error(GTK_WINDOW(app->window), "Не вдалося відкрити binary-файл.");
        return;
    }

    std::ofstream fout(outname);
    if (!fout) {
        show_error(GTK_WINDOW(app->window), "Не вдалося створити TXT-файл.");
        return;
    }

    set_status(app, "Конвертація binary -> TXT...");

    const size_t chunk_samples = 8192;
    std::vector<int16_t> raw(chunk_samples * 4);
    long long converted = 0;

    while (converted < samples_to_convert) {
        const long long remain = samples_to_convert - converted;
        const size_t now_samples = static_cast<size_t>(std::min<long long>(remain, chunk_samples));
        fin.read(reinterpret_cast<char *>(raw.data()), now_samples * 4 * sizeof(int16_t));
        const std::streamsize got_bytes = fin.gcount();
        const size_t got_samples = static_cast<size_t>(got_bytes / (4 * sizeof(int16_t)));
        if (got_samples == 0) break;

        for (size_t i = 0; i < got_samples; ++i) {
            fout << raw[4 * i + 0] << '\t'
                 << raw[4 * i + 1] << '\t'
                 << raw[4 * i + 2] << '\t'
                 << raw[4 * i + 3] << '\n';
        }
        converted += static_cast<long long>(got_samples);
    }

    fout.close();
    fin.close();

    std::ostringstream ss;
    ss << "TXT готовий без заголовка. Конвертовано семплів: " << converted;
    set_status(app, ss.str());
    show_info(GTK_WINDOW(app->window), ss.str());
}

static GtkWidget *make_labeled_entry(const char *label, const char *default_text, GtkWidget **entry_out)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    GtkWidget *lab = gtk_label_new(label);
    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(entry), default_text);
    gtk_widget_set_size_request(entry, 100, -1);
    gtk_box_pack_start(GTK_BOX(box), lab, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), entry, FALSE, FALSE, 0);
    *entry_out = entry;
    return box;
}

int main(int argc, char **argv)
{
    gtk_init(&argc, &argv);

    AppWidgets app;

    app.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(app.window), "Pluto 2RX Spectrum Viewer with Zoom");
    gtk_window_set_default_size(GTK_WINDOW(app.window), 1150, 720);
    g_signal_connect(app.window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    GtkWidget *main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(main_box), 8);
    gtk_container_add(GTK_CONTAINER(app.window), main_box);

    app.file_chooser = gtk_file_chooser_button_new("Оберіть Pluto 2RX binary file", GTK_FILE_CHOOSER_ACTION_OPEN);
    gtk_box_pack_start(GTK_BOX(main_box), app.file_chooser, FALSE, FALSE, 0);

    GtkWidget *controls1 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_box_pack_start(GTK_BOX(main_box), controls1, FALSE, FALSE, 0);

    GtkWidget *fft_box = make_labeled_entry("N FFT:", "8092", &app.entry_fft_n);
    GtkWidget *fs_box = make_labeled_entry("Fs, MS/s:", "4.0", &app.entry_fs_mhz);
    gtk_box_pack_start(GTK_BOX(controls1), fft_box, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(controls1), fs_box, FALSE, FALSE, 0);

    GtkWidget *btn_build = gtk_button_new_with_label("Build spectrum");
    GtkWidget *btn_reset = gtk_button_new_with_label("Reset Zoom");
    gtk_box_pack_start(GTK_BOX(controls1), btn_build, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(controls1), btn_reset, FALSE, FALSE, 0);

    app.check_rx0 = gtk_check_button_new_with_label("RX0");
    app.check_rx1 = gtk_check_button_new_with_label("RX1");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app.check_rx0), TRUE);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app.check_rx1), TRUE);
    gtk_box_pack_start(GTK_BOX(controls1), app.check_rx0, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(controls1), app.check_rx1, FALSE, FALSE, 0);

    GtkWidget *controls2 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_box_pack_start(GTK_BOX(main_box), controls2, FALSE, FALSE, 0);

    GtkWidget *txt_box = make_labeled_entry("TXT samples:", "10000", &app.entry_txt_samples);
    GtkWidget *btn_txt = gtk_button_new_with_label("Convert selected samples to TXT");
    gtk_box_pack_start(GTK_BOX(controls2), txt_box, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(controls2), btn_txt, FALSE, FALSE, 0);

    app.drawing_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(app.drawing_area, 1000, 520);
    gtk_box_pack_start(GTK_BOX(main_box), app.drawing_area, TRUE, TRUE, 0);

    gtk_widget_add_events(app.drawing_area,
                          GDK_SCROLL_MASK |
                          GDK_BUTTON_PRESS_MASK |
                          GDK_BUTTON_RELEASE_MASK |
                          GDK_POINTER_MOTION_MASK |
                          GDK_BUTTON_MOTION_MASK);

    app.status_label = gtk_label_new("Готово. Оберіть binary-файл.");
    gtk_box_pack_start(GTK_BOX(main_box), app.status_label, FALSE, FALSE, 0);

    g_signal_connect(app.drawing_area, "draw", G_CALLBACK(on_draw), &app);
    g_signal_connect(app.drawing_area, "scroll-event", G_CALLBACK(on_scroll), &app);
    g_signal_connect(app.drawing_area, "button-press-event", G_CALLBACK(on_button_press), &app);
    g_signal_connect(app.drawing_area, "button-release-event", G_CALLBACK(on_button_release), &app);
    g_signal_connect(app.drawing_area, "motion-notify-event", G_CALLBACK(on_motion), &app);

    g_signal_connect(btn_build, "clicked", G_CALLBACK(on_build_spectrum), &app);
    g_signal_connect(btn_reset, "clicked", G_CALLBACK(on_reset_zoom), &app);
    g_signal_connect(btn_txt, "clicked", G_CALLBACK(on_convert_txt), &app);
    g_signal_connect(app.check_rx0, "toggled", G_CALLBACK(on_toggle_curve), &app);
    g_signal_connect(app.check_rx1, "toggled", G_CALLBACK(on_toggle_curve), &app);

    gtk_widget_show_all(app.window);
    gtk_main();
    return 0;
}
