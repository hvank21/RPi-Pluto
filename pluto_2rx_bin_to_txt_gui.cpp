/*
 * pluto_2rx_bin_to_txt_gui.cpp
 *
 * Окрема GUI-утиліта для конвертації PlutoSDR 2RX binary IQ-файлу у TXT.
 *
 * Вхідний binary формат:
 *   int16 little-endian: I0 Q0 I1 Q1 I0 Q0 I1 Q1 ...
 *
 * Вихідний TXT формат без заголовка:
 *   I0    Q0    I1    Q1
 *
 * Залежності:
 *   sudo apt install -y g++ pkg-config libgtk-3-dev
 *
 * Компіляція:
 *   g++ -O2 -Wall -Wextra pluto_2rx_bin_to_txt_gui.cpp -o pluto_2rx_bin_to_txt_gui \
 *       $(pkg-config --cflags --libs gtk+-3.0)
 */

#include <gtk/gtk.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <sys/stat.h>

struct AppWidgets {
    GtkWidget *window = nullptr;
    GtkWidget *input_entry = nullptr;
    GtkWidget *output_entry = nullptr;
    GtkWidget *samples_spin = nullptr;
    GtkWidget *status_label = nullptr;
    GtkWidget *progress_bar = nullptr;
};

static bool file_exists(const std::string &path) {
    struct stat st{};
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

static long long file_size_bytes(const std::string &path) {
    struct stat st{};
    if (stat(path.c_str(), &st) != 0) return -1;
    return static_cast<long long>(st.st_size);
}

static std::string dirname_of(const std::string &path) {
    const std::size_t pos = path.find_last_of('/');
    if (pos == std::string::npos) return ".";
    if (pos == 0) return "/";
    return path.substr(0, pos);
}

static bool directory_writable(const std::string &dir) {
    std::string test_path = dir + "/.pluto_txt_write_test.tmp";
    std::ofstream f(test_path, std::ios::binary);
    if (!f) return false;
    f << "test";
    f.close();
    std::remove(test_path.c_str());
    return true;
}

static void set_status(AppWidgets *app, const std::string &msg) {
    gtk_label_set_text(GTK_LABEL(app->status_label), msg.c_str());
    while (gtk_events_pending()) gtk_main_iteration();
}

static void show_message(GtkWindow *parent, GtkMessageType type, const std::string &title, const std::string &msg) {
    GtkWidget *dialog = gtk_message_dialog_new(parent,
                                               GTK_DIALOG_MODAL,
                                               type,
                                               GTK_BUTTONS_OK,
                                               "%s",
                                               msg.c_str());
    gtk_window_set_title(GTK_WINDOW(dialog), title.c_str());
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

static std::string default_txt_name_from_bin(const std::string &bin_path) {
    std::string out = bin_path;
    const std::size_t pos = out.find_last_of('.');
    if (pos != std::string::npos) {
        out = out.substr(0, pos);
    }
    out += ".txt";
    return out;
}

static void on_choose_input(GtkButton *, gpointer user_data) {
    AppWidgets *app = static_cast<AppWidgets *>(user_data);

    GtkWidget *dialog = gtk_file_chooser_dialog_new("Вибрати binary IQ файл",
                                                    GTK_WINDOW(app->window),
                                                    GTK_FILE_CHOOSER_ACTION_OPEN,
                                                    "Скасувати", GTK_RESPONSE_CANCEL,
                                                    "Вибрати", GTK_RESPONSE_ACCEPT,
                                                    nullptr);

    GtkFileFilter *filter_bin = gtk_file_filter_new();
    gtk_file_filter_set_name(filter_bin, "Binary files (*.bin, *.dat)");
    gtk_file_filter_add_pattern(filter_bin, "*.bin");
    gtk_file_filter_add_pattern(filter_bin, "*.dat");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter_bin);

    GtkFileFilter *filter_all = gtk_file_filter_new();
    gtk_file_filter_set_name(filter_all, "All files");
    gtk_file_filter_add_pattern(filter_all, "*");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter_all);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        char *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        if (filename) {
            std::string in_path(filename);
            gtk_entry_set_text(GTK_ENTRY(app->input_entry), in_path.c_str());

            const std::string out_path = default_txt_name_from_bin(in_path);
            gtk_entry_set_text(GTK_ENTRY(app->output_entry), out_path.c_str());

            long long sz = file_size_bytes(in_path);
            if (sz > 0) {
                const long long max_samples = sz / 8; // 4 * int16 = 8 bytes per 2RX sample
                gtk_spin_button_set_range(GTK_SPIN_BUTTON(app->samples_spin), 1, static_cast<double>(max_samples));
                gtk_spin_button_set_value(GTK_SPIN_BUTTON(app->samples_spin), static_cast<double>(std::min<long long>(max_samples, 100000)));

                std::ostringstream ss;
                ss << "Файл вибрано. Розмір: " << sz << " байт. Максимум семплів: " << max_samples;
                set_status(app, ss.str());
            }
            g_free(filename);
        }
    }

    gtk_widget_destroy(dialog);
}

static void on_choose_output(GtkButton *, gpointer user_data) {
    AppWidgets *app = static_cast<AppWidgets *>(user_data);

    GtkWidget *dialog = gtk_file_chooser_dialog_new("Зберегти TXT файл",
                                                    GTK_WINDOW(app->window),
                                                    GTK_FILE_CHOOSER_ACTION_SAVE,
                                                    "Скасувати", GTK_RESPONSE_CANCEL,
                                                    "Зберегти", GTK_RESPONSE_ACCEPT,
                                                    nullptr);
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dialog), TRUE);

    const char *current_out = gtk_entry_get_text(GTK_ENTRY(app->output_entry));
    if (current_out && std::strlen(current_out) > 0) {
        gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(dialog), current_out);
    }

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        char *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        if (filename) {
            gtk_entry_set_text(GTK_ENTRY(app->output_entry), filename);
            g_free(filename);
        }
    }

    gtk_widget_destroy(dialog);
}

static bool convert_bin_to_txt(AppWidgets *app,
                               const std::string &input_path,
                               const std::string &output_path,
                               long long samples_to_convert) {
    const long long sz = file_size_bytes(input_path);
    if (sz < 8) {
        show_message(GTK_WINDOW(app->window), GTK_MESSAGE_ERROR, "Помилка", "Вхідний файл занадто малий або недоступний.");
        return false;
    }

    if (sz % 8 != 0) {
        show_message(GTK_WINDOW(app->window), GTK_MESSAGE_WARNING, "Увага", "Розмір файлу не кратний 8 байтам. Останні неповні байти буде проігноровано.");
    }

    const long long max_samples = sz / 8;
    if (samples_to_convert <= 0 || samples_to_convert > max_samples) {
        std::ostringstream ss;
        ss << "Некоректна кількість семплів. Доступно максимум: " << max_samples;
        show_message(GTK_WINDOW(app->window), GTK_MESSAGE_ERROR, "Помилка", ss.str());
        return false;
    }

    const std::string out_dir = dirname_of(output_path);
    if (!directory_writable(out_dir)) {
        show_message(GTK_WINDOW(app->window), GTK_MESSAGE_ERROR, "Помилка", "Каталог для TXT не доступний для запису поточним користувачем.");
        return false;
    }

    std::ifstream fin(input_path, std::ios::binary);
    if (!fin) {
        show_message(GTK_WINDOW(app->window), GTK_MESSAGE_ERROR, "Помилка", "Не вдалося відкрити binary-файл.");
        return false;
    }

    std::ofstream fout(output_path);
    if (!fout) {
        show_message(GTK_WINDOW(app->window), GTK_MESSAGE_ERROR, "Помилка", "Не вдалося створити TXT-файл.");
        return false;
    }

    // Без заголовка. Тільки числові стовпці:
    // I0 Q0 I1 Q1
    constexpr std::size_t BLOCK_SAMPLES = 8192;
    int16_t buffer[BLOCK_SAMPLES * 4];

    long long done = 0;
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app->progress_bar), 0.0);
    set_status(app, "Конвертація запущена...");

    while (done < samples_to_convert) {
        const long long remain = samples_to_convert - done;
        const std::size_t cur_samples = static_cast<std::size_t>(std::min<long long>(remain, BLOCK_SAMPLES));
        const std::size_t values_to_read = cur_samples * 4;
        const std::size_t bytes_to_read = values_to_read * sizeof(int16_t);

        fin.read(reinterpret_cast<char *>(buffer), static_cast<std::streamsize>(bytes_to_read));
        const std::streamsize got_bytes = fin.gcount();
        if (got_bytes <= 0) break;

        const std::size_t got_values = static_cast<std::size_t>(got_bytes) / sizeof(int16_t);
        const std::size_t got_samples = got_values / 4;

        for (std::size_t i = 0; i < got_samples; ++i) {
            const int16_t i0 = buffer[4 * i + 0];
            const int16_t q0 = buffer[4 * i + 1];
            const int16_t i1 = buffer[4 * i + 2];
            const int16_t q1 = buffer[4 * i + 3];
            fout << i0 << '\t' << q0 << '\t' << i1 << '\t' << q1 << '\n';
        }

        done += static_cast<long long>(got_samples);

        const double frac = static_cast<double>(done) / static_cast<double>(samples_to_convert);
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app->progress_bar), frac);

        std::ostringstream ss;
        ss << "Конвертовано: " << done << " / " << samples_to_convert << " семплів";
        set_status(app, ss.str());

        if (got_samples < cur_samples) break;
    }

    fout.flush();
    fout.close();
    fin.close();

    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app->progress_bar), 1.0);

    std::ostringstream ss;
    ss << "Готово. TXT збережено: " << output_path << " | семплів: " << done;
    set_status(app, ss.str());

    return true;
}

static void on_convert(GtkButton *, gpointer user_data) {
    AppWidgets *app = static_cast<AppWidgets *>(user_data);

    const char *in_c = gtk_entry_get_text(GTK_ENTRY(app->input_entry));
    const char *out_c = gtk_entry_get_text(GTK_ENTRY(app->output_entry));

    std::string input_path = in_c ? in_c : "";
    std::string output_path = out_c ? out_c : "";

    if (input_path.empty()) {
        show_message(GTK_WINDOW(app->window), GTK_MESSAGE_ERROR, "Помилка", "Не вибрано binary-файл.");
        return;
    }
    if (!file_exists(input_path)) {
        show_message(GTK_WINDOW(app->window), GTK_MESSAGE_ERROR, "Помилка", "Binary-файл не існує.");
        return;
    }
    if (output_path.empty()) {
        show_message(GTK_WINDOW(app->window), GTK_MESSAGE_ERROR, "Помилка", "Не задано TXT-файл.");
        return;
    }

    const long long samples = static_cast<long long>(gtk_spin_button_get_value(GTK_SPIN_BUTTON(app->samples_spin)));

    if (convert_bin_to_txt(app, input_path, output_path, samples)) {
        show_message(GTK_WINDOW(app->window), GTK_MESSAGE_INFO, "Готово", "Конвертацію завершено.");
    }
}

static GtkWidget *make_label(const char *text) {
    GtkWidget *label = gtk_label_new(text);
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    return label;
}

int main(int argc, char **argv) {
    gtk_init(&argc, &argv);

    AppWidgets app;

    app.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(app.window), "Pluto 2RX BIN → TXT Converter");
    gtk_window_set_default_size(GTK_WINDOW(app.window), 760, 300);
    gtk_container_set_border_width(GTK_CONTAINER(app.window), 12);
    g_signal_connect(app.window, "destroy", G_CALLBACK(gtk_main_quit), nullptr);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_container_add(GTK_CONTAINER(app.window), grid);

    GtkWidget *input_label = make_label("Binary IQ файл:");
    app.input_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(app.input_entry), "/mnt/ssd/pluto_2rx_....bin");
    GtkWidget *input_btn = gtk_button_new_with_label("Вибрати...");
    g_signal_connect(input_btn, "clicked", G_CALLBACK(on_choose_input), &app);

    GtkWidget *output_label = make_label("TXT файл:");
    app.output_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(app.output_entry), "/mnt/ssd/pluto_2rx_....txt");
    GtkWidget *output_btn = gtk_button_new_with_label("Зберегти як...");
    g_signal_connect(output_btn, "clicked", G_CALLBACK(on_choose_output), &app);

    GtkWidget *samples_label = make_label("Кількість семплів для конвертації:");
    app.samples_spin = gtk_spin_button_new_with_range(1, 1000000000.0, 1);
    gtk_spin_button_set_digits(GTK_SPIN_BUTTON(app.samples_spin), 0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(app.samples_spin), 100000);

    GtkWidget *format_label = make_label("Формат TXT без заголовка: I0    Q0    I1    Q1");

    GtkWidget *convert_btn = gtk_button_new_with_label("Конвертувати BIN → TXT");
    g_signal_connect(convert_btn, "clicked", G_CALLBACK(on_convert), &app);

    app.progress_bar = gtk_progress_bar_new();
    app.status_label = gtk_label_new("Оберіть binary-файл.");
    gtk_widget_set_halign(app.status_label, GTK_ALIGN_START);

    gtk_grid_attach(GTK_GRID(grid), input_label, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), app.input_entry, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), input_btn, 2, 0, 1, 1);

    gtk_grid_attach(GTK_GRID(grid), output_label, 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), app.output_entry, 1, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), output_btn, 2, 1, 1, 1);

    gtk_grid_attach(GTK_GRID(grid), samples_label, 0, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), app.samples_spin, 1, 2, 1, 1);

    gtk_grid_attach(GTK_GRID(grid), format_label, 1, 3, 2, 1);
    gtk_grid_attach(GTK_GRID(grid), convert_btn, 1, 4, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), app.progress_bar, 1, 5, 2, 1);
    gtk_grid_attach(GTK_GRID(grid), app.status_label, 0, 6, 3, 1);

    gtk_widget_set_hexpand(app.input_entry, TRUE);
    gtk_widget_set_hexpand(app.output_entry, TRUE);
    gtk_widget_set_hexpand(app.progress_bar, TRUE);

    gtk_widget_show_all(app.window);
    gtk_main();

    return 0;
}
