/*
 * pluto_2rx_sum_channels_gui.cpp
 *
 * GUI utility for Raspberry Pi / Linux.
 * Converts a Pluto 2RX binary IQ file:
 *     int16: I0 Q0 I1 Q1 I0 Q0 I1 Q1 ...
 * into a summed single-channel IQ binary file:
 *     int16: Isum Qsum Isum Qsum ...
 * where:
 *     Isum = saturate_int16(I0 + I1)
 *     Qsum = saturate_int16(Q0 + Q1)
 *
 * GUI: GTK3
 * Progress bar is mandatory and updated during processing.
 *
 * Build:
 *   g++ -O2 -Wall -Wextra pluto_2rx_sum_channels_gui.cpp -o pluto_2rx_sum_channels_gui \
 *       $(pkg-config --cflags --libs gtk+-3.0)
 *
 * Run:
 *   ./pluto_2rx_sum_channels_gui
 */

#include <gtk/gtk.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <limits>

struct AppWidgets {
    GtkWidget *window = nullptr;
    GtkWidget *entry_input = nullptr;
    GtkWidget *entry_output = nullptr;
    GtkWidget *button_input = nullptr;
    GtkWidget *button_output = nullptr;
    GtkWidget *button_start = nullptr;
    GtkWidget *button_quit = nullptr;
    GtkWidget *progress = nullptr;
    GtkWidget *label_status = nullptr;
    GtkWidget *check_normalize_half = nullptr;
};

static int16_t clamp_int16(int value)
{
    if (value > 32767) return 32767;
    if (value < -32768) return -32768;
    return static_cast<int16_t>(value);
}

static std::string get_text(GtkWidget *entry)
{
    const char *txt = gtk_entry_get_text(GTK_ENTRY(entry));
    return txt ? std::string(txt) : std::string();
}

static void set_status(AppWidgets *app, const std::string &msg)
{
    gtk_label_set_text(GTK_LABEL(app->label_status), msg.c_str());
    while (gtk_events_pending()) gtk_main_iteration();
}

static void set_controls_sensitive(AppWidgets *app, gboolean state)
{
    gtk_widget_set_sensitive(app->button_input, state);
    gtk_widget_set_sensitive(app->button_output, state);
    gtk_widget_set_sensitive(app->button_start, state);
    gtk_widget_set_sensitive(app->button_quit, state);
    gtk_widget_set_sensitive(app->entry_input, state);
    gtk_widget_set_sensitive(app->entry_output, state);
    gtk_widget_set_sensitive(app->check_normalize_half, state);
    while (gtk_events_pending()) gtk_main_iteration();
}

static void show_message(GtkWindow *parent, GtkMessageType type, const std::string &text)
{
    GtkWidget *dialog = gtk_message_dialog_new(
        parent,
        GTK_DIALOG_MODAL,
        type,
        GTK_BUTTONS_OK,
        "%s",
        text.c_str()
    );
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

static bool file_exists_and_readable(const std::string &path)
{
    std::ifstream f(path, std::ios::binary);
    return f.good();
}

static uint64_t get_file_size_bytes(const std::string &path)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return 0;
    std::streampos pos = f.tellg();
    if (pos < 0) return 0;
    return static_cast<uint64_t>(pos);
}

static void choose_input_file(GtkButton *, gpointer user_data)
{
    AppWidgets *app = static_cast<AppWidgets *>(user_data);

    GtkWidget *dialog = gtk_file_chooser_dialog_new(
        "Select input Pluto 2RX binary file",
        GTK_WINDOW(app->window),
        GTK_FILE_CHOOSER_ACTION_OPEN,
        "Cancel", GTK_RESPONSE_CANCEL,
        "Open", GTK_RESPONSE_ACCEPT,
        nullptr
    );

    GtkFileFilter *filter_bin = gtk_file_filter_new();
    gtk_file_filter_set_name(filter_bin, "Binary files (*.bin)");
    gtk_file_filter_add_pattern(filter_bin, "*.bin");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter_bin);

    GtkFileFilter *filter_all = gtk_file_filter_new();
    gtk_file_filter_set_name(filter_all, "All files");
    gtk_file_filter_add_pattern(filter_all, "*");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter_all);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        char *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        if (filename) {
            gtk_entry_set_text(GTK_ENTRY(app->entry_input), filename);

            std::string out = std::string(filename);
            size_t p = out.find_last_of('.');
            if (p != std::string::npos) out = out.substr(0, p);
            out += "_sum_iq.bin";
            gtk_entry_set_text(GTK_ENTRY(app->entry_output), out.c_str());

            g_free(filename);
        }
    }

    gtk_widget_destroy(dialog);
}

static void choose_output_file(GtkButton *, gpointer user_data)
{
    AppWidgets *app = static_cast<AppWidgets *>(user_data);

    GtkWidget *dialog = gtk_file_chooser_dialog_new(
        "Select output summed IQ binary file",
        GTK_WINDOW(app->window),
        GTK_FILE_CHOOSER_ACTION_SAVE,
        "Cancel", GTK_RESPONSE_CANCEL,
        "Save", GTK_RESPONSE_ACCEPT,
        nullptr
    );

    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dialog), TRUE);

    const std::string current = get_text(app->entry_output);
    if (!current.empty()) {
        gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(dialog), current.c_str());
    }

    GtkFileFilter *filter_bin = gtk_file_filter_new();
    gtk_file_filter_set_name(filter_bin, "Binary files (*.bin)");
    gtk_file_filter_add_pattern(filter_bin, "*.bin");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter_bin);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        char *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        if (filename) {
            gtk_entry_set_text(GTK_ENTRY(app->entry_output), filename);
            g_free(filename);
        }
    }

    gtk_widget_destroy(dialog);
}

static bool process_sum_file(AppWidgets *app)
{
    const std::string input_path = get_text(app->entry_input);
    const std::string output_path = get_text(app->entry_output);

    if (input_path.empty()) {
        show_message(GTK_WINDOW(app->window), GTK_MESSAGE_ERROR, "Input file is not selected.");
        return false;
    }

    if (output_path.empty()) {
        show_message(GTK_WINDOW(app->window), GTK_MESSAGE_ERROR, "Output file is not selected.");
        return false;
    }

    if (input_path == output_path) {
        show_message(GTK_WINDOW(app->window), GTK_MESSAGE_ERROR, "Input and output files must be different.");
        return false;
    }

    if (!file_exists_and_readable(input_path)) {
        show_message(GTK_WINDOW(app->window), GTK_MESSAGE_ERROR, "Input file cannot be opened.");
        return false;
    }

    const uint64_t file_size = get_file_size_bytes(input_path);
    if (file_size == 0) {
        show_message(GTK_WINDOW(app->window), GTK_MESSAGE_ERROR, "Input file is empty or cannot be measured.");
        return false;
    }

    // One input sample for two RX channels is 4 x int16 = 8 bytes.
    if (file_size % 8 != 0) {
        std::ostringstream oss;
        oss << "Warning: input file size is not divisible by 8 bytes.\n"
            << "The incomplete tail will be ignored.\n\n"
            << "File size: " << file_size << " bytes";
        show_message(GTK_WINDOW(app->window), GTK_MESSAGE_WARNING, oss.str());
    }

    const uint64_t total_samples = file_size / 8;
    if (total_samples == 0) {
        show_message(GTK_WINDOW(app->window), GTK_MESSAGE_ERROR, "No complete 2RX samples found in input file.");
        return false;
    }

    std::ifstream fin(input_path, std::ios::binary);
    if (!fin) {
        show_message(GTK_WINDOW(app->window), GTK_MESSAGE_ERROR, "Cannot open input file.");
        return false;
    }

    std::ofstream fout(output_path, std::ios::binary | std::ios::trunc);
    if (!fout) {
        show_message(GTK_WINDOW(app->window), GTK_MESSAGE_ERROR, "Cannot create output file.");
        return false;
    }

    const bool normalize_half = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(app->check_normalize_half));

    // Number of 2RX samples per block.
    // Input per sample: 4 int16. Output per sample: 2 int16.
    const size_t block_samples = 1u << 20; // 1,048,576 samples
    std::vector<int16_t> inbuf(block_samples * 4);
    std::vector<int16_t> outbuf(block_samples * 2);

    uint64_t processed_samples = 0;
    uint64_t saturated_count = 0;

    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app->progress), 0.0);
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(app->progress), "0 %");

    set_controls_sensitive(app, FALSE);
    set_status(app, "Processing...");

    while (processed_samples < total_samples) {
        const uint64_t samples_left = total_samples - processed_samples;
        const size_t samples_now = static_cast<size_t>(std::min<uint64_t>(samples_left, block_samples));

        const size_t input_words = samples_now * 4;
        const size_t output_words = samples_now * 2;

        fin.read(reinterpret_cast<char *>(inbuf.data()), static_cast<std::streamsize>(input_words * sizeof(int16_t)));
        const std::streamsize got_bytes = fin.gcount();
        const size_t got_words = static_cast<size_t>(got_bytes / sizeof(int16_t));
        const size_t got_samples = got_words / 4;

        if (got_samples == 0) {
            break;
        }

        for (size_t i = 0; i < got_samples; ++i) {
            const int i0 = inbuf[4 * i + 0];
            const int q0 = inbuf[4 * i + 1];
            const int i1 = inbuf[4 * i + 2];
            const int q1 = inbuf[4 * i + 3];

            int isum = i0 + i1;
            int qsum = q0 + q1;

            if (normalize_half) {
                isum /= 2;
                qsum /= 2;
            }

            if (isum > 32767 || isum < -32768) ++saturated_count;
            if (qsum > 32767 || qsum < -32768) ++saturated_count;

            outbuf[2 * i + 0] = clamp_int16(isum);
            outbuf[2 * i + 1] = clamp_int16(qsum);
        }

        fout.write(reinterpret_cast<const char *>(outbuf.data()), static_cast<std::streamsize>(output_words * sizeof(int16_t)));
        if (!fout) {
            set_controls_sensitive(app, TRUE);
            show_message(GTK_WINDOW(app->window), GTK_MESSAGE_ERROR, "Write error. Check output disk space and permissions.");
            return false;
        }

        processed_samples += got_samples;

        const double frac = static_cast<double>(processed_samples) / static_cast<double>(total_samples);
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app->progress), std::min(1.0, frac));

        std::ostringstream ptxt;
        ptxt.setf(std::ios::fixed);
        ptxt.precision(1);
        ptxt << (100.0 * frac) << " %";
        gtk_progress_bar_set_text(GTK_PROGRESS_BAR(app->progress), ptxt.str().c_str());

        std::ostringstream status;
        status << "Processed " << processed_samples << " / " << total_samples << " samples";
        gtk_label_set_text(GTK_LABEL(app->label_status), status.str().c_str());

        while (gtk_events_pending()) gtk_main_iteration();
    }

    fout.flush();
    fin.close();
    fout.close();

    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app->progress), 1.0);
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(app->progress), "100 %");

    set_controls_sensitive(app, TRUE);

    std::ostringstream done;
    done << "Done. Output file created.\n\n"
         << "Input samples : " << total_samples << "\n"
         << "Output samples: " << processed_samples << "\n"
         << "Output format : int16 I Q\n"
         << "Output file   : " << output_path;

    if (!normalize_half) {
        done << "\nSaturated I/Q values: " << saturated_count;
    } else {
        done << "\nNormalize /2 mode: ON";
    }

    set_status(app, "Done.");
    show_message(GTK_WINDOW(app->window), GTK_MESSAGE_INFO, done.str());
    return true;
}

static void start_processing(GtkButton *, gpointer user_data)
{
    AppWidgets *app = static_cast<AppWidgets *>(user_data);
    process_sum_file(app);
}

static GtkWidget *make_labeled_entry_row(const char *label_text, GtkWidget **entry, GtkWidget **button, const char *button_text)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *label = gtk_label_new(label_text);
    gtk_widget_set_size_request(label, 90, -1);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);

    *entry = gtk_entry_new();
    gtk_widget_set_hexpand(*entry, TRUE);

    *button = gtk_button_new_with_label(button_text);

    gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), *entry, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), *button, FALSE, FALSE, 0);

    return box;
}

int main(int argc, char **argv)
{
    gtk_init(&argc, &argv);

    AppWidgets app;

    app.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(app.window), "Pluto 2RX Sum Channels: (I0+I1), (Q0+Q1)");
    gtk_window_set_default_size(GTK_WINDOW(app.window), 760, 300);
    gtk_container_set_border_width(GTK_CONTAINER(app.window), 12);

    GtkWidget *main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_add(GTK_CONTAINER(app.window), main_box);

    GtkWidget *title = gtk_label_new("Pluto 2RX binary IQ -> summed 1RX IQ binary");
    gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
    gtk_box_pack_start(GTK_BOX(main_box), title, FALSE, FALSE, 0);

    GtkWidget *info = gtk_label_new("Input: int16 I0 Q0 I1 Q1  |  Output: int16 Isum Qsum");
    gtk_label_set_xalign(GTK_LABEL(info), 0.0f);
    gtk_box_pack_start(GTK_BOX(main_box), info, FALSE, FALSE, 0);

    GtkWidget *row_input = make_labeled_entry_row("Input .bin", &app.entry_input, &app.button_input, "Browse...");
    gtk_box_pack_start(GTK_BOX(main_box), row_input, FALSE, FALSE, 0);

    GtkWidget *row_output = make_labeled_entry_row("Output .bin", &app.entry_output, &app.button_output, "Save as...");
    gtk_box_pack_start(GTK_BOX(main_box), row_output, FALSE, FALSE, 0);

    app.check_normalize_half = gtk_check_button_new_with_label("Normalize by 2: Isum=(I0+I1)/2, Qsum=(Q0+Q1)/2  (recommended to avoid clipping)");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app.check_normalize_half), TRUE);
    gtk_box_pack_start(GTK_BOX(main_box), app.check_normalize_half, FALSE, FALSE, 0);

    app.progress = gtk_progress_bar_new();
    gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(app.progress), TRUE);
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(app.progress), "0 %");
    gtk_box_pack_start(GTK_BOX(main_box), app.progress, FALSE, FALSE, 0);

    app.label_status = gtk_label_new("Ready.");
    gtk_label_set_xalign(GTK_LABEL(app.label_status), 0.0f);
    gtk_box_pack_start(GTK_BOX(main_box), app.label_status, FALSE, FALSE, 0);

    GtkWidget *button_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_end(GTK_BOX(main_box), button_box, FALSE, FALSE, 0);

    app.button_start = gtk_button_new_with_label("Start summing");
    app.button_quit = gtk_button_new_with_label("Quit");

    gtk_box_pack_end(GTK_BOX(button_box), app.button_quit, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(button_box), app.button_start, FALSE, FALSE, 0);

    g_signal_connect(app.window, "destroy", G_CALLBACK(gtk_main_quit), nullptr);
    g_signal_connect(app.button_quit, "clicked", G_CALLBACK(gtk_main_quit), nullptr);
    g_signal_connect(app.button_input, "clicked", G_CALLBACK(choose_input_file), &app);
    g_signal_connect(app.button_output, "clicked", G_CALLBACK(choose_output_file), &app);
    g_signal_connect(app.button_start, "clicked", G_CALLBACK(start_processing), &app);

    gtk_widget_show_all(app.window);
    gtk_main();

    return 0;
}
