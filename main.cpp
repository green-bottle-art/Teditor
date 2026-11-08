// Simple Code Editor - GTK4 + GtkSourceView 5
//
// Fitur: Project tree, Open Project, Autopairs, Search & Replace, dua mode
//
// Mode:
//   EDITOR  (default) : mengedit file seperti biasa.
//   COMMAND           : tekan Esc di editor -> muncul command bar di bawah.
//       new <file>               buat file baru (folder induk dibuat bila perlu) lalu dibuka
//       mkdir <folder>           buat folder baru (termasuk folder induknya)
//       rename <lama> <baru>     ganti nama file/folder (seperti mv)
//       rename <baru>            ganti nama file yang sedang dibuka
//       project <folder>         tampilkan folder itu di sidebar (project tree)
//       help                     tampilkan daftar perintah
//     Enter = jalankan, Esc = kembali ke mode editor.
//     Path relatif dihitung dari folder project (atau folder file yang sedang dibuka).
//     Nama dengan spasi: pakai tanda kutip, mis.  new "catatan baru.txt"
//
// Shortcut tetap ada (tanpa menu): Ctrl+O buka file, Ctrl+Shift+O buka project,
//   Ctrl+S simpan, Ctrl+Shift+S simpan sebagai, Ctrl+F cari, Ctrl+H ganti,
//   F3 / Shift+F3 berikutnya/sebelumnya, Ctrl+B sidebar, Ctrl+Q keluar.
//
// Build:
//   g++ -Wall -Wextra main.cpp -o syet-proto $(pkg-config --cflags --libs gtk4 gtksourceview-5)
//
// Paket Debian: libgtk-4-dev libgtksourceview-5-dev pkg-config
// Butuh GTK >= 4.10 (GtkFileDialog / GtkAlertDialog).

#include <gtk/gtk.h>
#include <gtksourceview/gtksource.h>
#include <cstring>

enum class Mode { Editor, Command };

struct EditorApp {
    GtkApplication *gapp;
    GtkWidget *window;
    GtkSourceView *source_view;
    GtkSourceBuffer *source_buffer;
    GtkSourceFile *source_file;  // menyimpan lokasi (GFile) file yang sedang dibuka
    GtkWidget *status_left;      // nama file + status simpan
    GtkWidget *status_right;     // posisi kursor + bahasa
    GtkWidget *status_msg;       // pesan hasil perintah terakhir
    GtkWidget *mode_label;       // "EDITOR" / "COMMAND"

    // Mode + command bar
    Mode mode;
    GtkWidget *command_bar;
    GtkWidget *command_entry;
    GtkWidget *command_msg;

    // Project tree
    GFile *project_root;
    GtkWidget *sidebar;
    GtkWidget *project_label;
    GtkWidget *tree_view;

    // Search & replace
    GtkSourceSearchSettings *search_settings;
    GtkSourceSearchContext *search_context;
    GtkWidget *search_bar;
    GtkWidget *search_entry;
    GtkWidget *replace_row;
    GtkWidget *replace_entry;
    GtkWidget *match_label;
};

static void update_match_label(EditorApp *app);
static void request_open(EditorApp *app, GFile *file);
static void set_project(EditorApp *app, GFile *root);
static void detect_language(EditorApp *app);

// ---------------------------------------------------------------------------
// Util
// ---------------------------------------------------------------------------

static void show_error(EditorApp *app, const char *what, const GError *error) {
    GtkAlertDialog *dlg = gtk_alert_dialog_new("%s", what);
    gtk_alert_dialog_set_detail(dlg, error ? error->message : "");
    gtk_alert_dialog_show(dlg, GTK_WINDOW(app->window));
    g_object_unref(dlg);
}

// Dialog ditutup pengguna (Cancel / Esc) bukan error sungguhan.
static bool is_dismissed(const GError *error) {
    return g_error_matches(error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED) ||
           g_error_matches(error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_CANCELLED);
}

// Status bar di dalam jendela: tetap terlihat di window manager tanpa title bar
// (mis. i3 dengan border "pixel"), tidak seperti judul jendela.
//   kiri : mode + nama file + status simpan
//   kanan: baris/kolom kursor + bahasa
static void update_status_bar(EditorApp *app) {
    GtkTextBuffer *buf = GTK_TEXT_BUFFER(app->source_buffer);
    GFile *loc = gtk_source_file_get_location(app->source_file);
    bool modified = gtk_text_buffer_get_modified(buf);

    char *name = loc ? g_file_get_basename(loc) : g_strdup("Untitled");
    const char *state = modified ? "● Unsaved changes"
                                 : (loc ? "Saved" : "");
    char *left = g_strdup_printf("%s   %s", name, state);
    gtk_label_set_text(GTK_LABEL(app->status_left), left);

    GtkTextIter it;
    gtk_text_buffer_get_iter_at_mark(buf, &it, gtk_text_buffer_get_insert(buf));
    GtkSourceLanguage *lang = gtk_source_buffer_get_language(app->source_buffer);
    char *right = g_strdup_printf("Ln %d, Col %d   %s",
                                  gtk_text_iter_get_line(&it) + 1,
                                  gtk_text_iter_get_line_offset(&it) + 1,
                                  lang ? gtk_source_language_get_name(lang) : "Plain Text");
    gtk_label_set_text(GTK_LABEL(app->status_right), right);

    g_free(name);
    g_free(left);
    g_free(right);
}

static void update_window_title(EditorApp *app) {
    GFile *loc = gtk_source_file_get_location(app->source_file);
    char *name = loc ? g_file_get_parse_name(loc) : nullptr;
    bool modified = gtk_text_buffer_get_modified(GTK_TEXT_BUFFER(app->source_buffer));

    char *title = g_strdup_printf("Simple Code Editor - %s%s",
                                  name ? name : "Untitled",
                                  modified ? " *" : "");
    gtk_window_set_title(GTK_WINDOW(app->window), title);
    g_free(title);
    g_free(name);

    update_status_bar(app);
}

// Deteksi bahasa dari nama file untuk syntax highlighting.
// Tanpa lokasi atau ekstensi tak dikenal -> plain text (bahasa NULL).
static void detect_language(EditorApp *app) {
    GFile *loc = gtk_source_file_get_location(app->source_file);
    GtkSourceLanguage *lang = nullptr;
    if (loc) {
        char *base = g_file_get_basename(loc);
        GtkSourceLanguageManager *manager = gtk_source_language_manager_get_default();
        lang = gtk_source_language_manager_guess_language(manager, base, nullptr);
        g_free(base);
    }
    gtk_source_buffer_set_language(app->source_buffer, lang);
}

static void on_modified_changed(GtkTextBuffer *, gpointer user_data) {
    update_window_title((EditorApp *)user_data);
}

static void on_cursor_moved(GObject *, GParamSpec *, gpointer user_data) {
    EditorApp *app = (EditorApp *)user_data;
    update_status_bar(app);
    if (app->search_bar && gtk_widget_get_visible(app->search_bar))
        update_match_label(app);  // "3 dari 12" ikut berubah saat kursor pindah
}

// ---------------------------------------------------------------------------
// Mode editor / mode command
// ---------------------------------------------------------------------------

static const char *COMMAND_HINT =
    "new <file>  |  mkdir <folder>  |  rename [lama] <baru>  |  project <folder>  |  Esc: kembali";

static void set_status_message(EditorApp *app, const char *text) {
    gtk_label_set_text(GTK_LABEL(app->status_msg), text ? text : "");
}

static void set_command_message(EditorApp *app, const char *text, bool is_error) {
    gtk_label_set_text(GTK_LABEL(app->command_msg), text);
    if (is_error) gtk_widget_add_css_class(app->command_msg, "error");
    else gtk_widget_remove_css_class(app->command_msg, "error");
}

static void enter_editor_mode(EditorApp *app, bool grab_focus) {
    app->mode = Mode::Editor;  // diset dulu supaya handler focus-leave tidak berulang
    gtk_label_set_text(GTK_LABEL(app->mode_label), "EDITOR");
    gtk_widget_set_visible(app->command_bar, FALSE);
    if (grab_focus) gtk_widget_grab_focus(GTK_WIDGET(app->source_view));
}

static void enter_command_mode(EditorApp *app) {
    app->mode = Mode::Command;
    gtk_label_set_text(GTK_LABEL(app->mode_label), "COMMAND");
    gtk_editable_set_text(GTK_EDITABLE(app->command_entry), "");
    set_command_message(app, COMMAND_HINT, false);
    set_status_message(app, "");
    gtk_widget_set_visible(app->command_bar, TRUE);
    gtk_widget_grab_focus(app->command_entry);
}

// ---------------------------------------------------------------------------
// Buka file (GtkSourceFileLoader: deteksi encoding, tidak masuk undo)
// ---------------------------------------------------------------------------

struct LoadCtx {
    EditorApp *app;
    GtkSourceFile *file;  // dimiliki ctx sampai berhasil dimuat
};

static void on_load_done(GObject *source, GAsyncResult *res, gpointer user_data) {
    LoadCtx *ctx = (LoadCtx *)user_data;
    GError *error = nullptr;

    if (gtk_source_file_loader_load_finish(GTK_SOURCE_FILE_LOADER(source), res, &error)) {
        g_object_unref(ctx->app->source_file);
        ctx->app->source_file = ctx->file;  // ownership pindah ke app
        detect_language(ctx->app);
    } else {
        show_error(ctx->app, "Gagal membuka file", error);
        g_clear_error(&error);
        g_object_unref(ctx->file);
    }
    update_window_title(ctx->app);
    g_free(ctx);
}

static void open_location(EditorApp *app, GFile *location) {
    GtkSourceFile *file = gtk_source_file_new();
    gtk_source_file_set_location(file, location);

    GtkSourceFileLoader *loader = gtk_source_file_loader_new(app->source_buffer, file);
    LoadCtx *ctx = g_new0(LoadCtx, 1);
    ctx->app = app;
    ctx->file = file;

    gtk_source_file_loader_load_async(loader, G_PRIORITY_DEFAULT, nullptr,
                                      nullptr, nullptr, nullptr,
                                      on_load_done, ctx);
    g_object_unref(loader);  // GTask menahan referensi selama operasi berjalan
}

// Buka file, tapi tanya dulu kalau ada perubahan yang belum disimpan.
// Penting untuk project tree: double-click tak sengaja tidak boleh membuang kerjaan.
struct OpenReq {
    EditorApp *app;
    GFile *file;
};

static void on_discard_answer(GObject *source, GAsyncResult *res, gpointer user_data) {
    OpenReq *req = (OpenReq *)user_data;
    GError *error = nullptr;
    int choice = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(source), res, &error);
    if (!error && choice == 1)
        open_location(req->app, req->file);
    g_clear_error(&error);
    g_object_unref(req->file);
    g_free(req);
}

static void request_open(EditorApp *app, GFile *file) {
    if (!gtk_text_buffer_get_modified(GTK_TEXT_BUFFER(app->source_buffer))) {
        open_location(app, file);
        return;
    }

    OpenReq *req = g_new0(OpenReq, 1);
    req->app = app;
    req->file = (GFile *)g_object_ref(file);

    GtkAlertDialog *dlg = gtk_alert_dialog_new("%s", "Perubahan belum disimpan");
    gtk_alert_dialog_set_detail(dlg, "Membuka file lain akan membuang perubahan yang belum disimpan.");
    const char *buttons[] = {"Batal", "Buang & Buka", nullptr};
    gtk_alert_dialog_set_buttons(dlg, buttons);
    gtk_alert_dialog_set_cancel_button(dlg, 0);
    gtk_alert_dialog_set_default_button(dlg, 0);
    gtk_alert_dialog_choose(dlg, GTK_WINDOW(app->window), nullptr, on_discard_answer, req);
    g_object_unref(dlg);
}

// ---------------------------------------------------------------------------
// Simpan file (GtkSourceFileSaver: menulis lewat g_file_replace, aman saat crash)
// ---------------------------------------------------------------------------

struct SaveCtx {
    EditorApp *app;
    GFile *target;  // NULL = simpan ke lokasi sekarang
};

static void on_save_done(GObject *source, GAsyncResult *res, gpointer user_data) {
    SaveCtx *ctx = (SaveCtx *)user_data;
    GError *error = nullptr;

    if (gtk_source_file_saver_save_finish(GTK_SOURCE_FILE_SAVER(source), res, &error)) {
        if (ctx->target)
            gtk_source_file_set_location(ctx->app->source_file, ctx->target);
        detect_language(ctx->app);
    } else {
        show_error(ctx->app, "Gagal menyimpan file", error);
        g_clear_error(&error);
    }
    update_window_title(ctx->app);
    g_clear_object(&ctx->target);
    g_free(ctx);
}

static void start_save(EditorApp *app, GFile *target) {
    GtkSourceFileSaver *saver =
        target ? gtk_source_file_saver_new_with_target(app->source_buffer, app->source_file, target)
               : gtk_source_file_saver_new(app->source_buffer, app->source_file);

    SaveCtx *ctx = g_new0(SaveCtx, 1);
    ctx->app = app;
    ctx->target = target ? (GFile *)g_object_ref(target) : nullptr;

    gtk_source_file_saver_save_async(saver, G_PRIORITY_DEFAULT, nullptr,
                                     nullptr, nullptr, nullptr,
                                     on_save_done, ctx);
    g_object_unref(saver);
}

// ---------------------------------------------------------------------------
// Project tree (GtkListView + GtkTreeListModel + GtkDirectoryList)
// ---------------------------------------------------------------------------

static const char *TREE_ATTRS =
    "standard::name,standard::display-name,standard::type,standard::icon,standard::is-hidden";

static gboolean tree_filter_visible(gpointer item, gpointer) {
    return !g_file_info_get_is_hidden(G_FILE_INFO(item));
}

// Folder dulu, baru file; masing-masing urut abjad.
static int tree_compare(gconstpointer a, gconstpointer b, gpointer) {
    GFileInfo *ia = G_FILE_INFO((gpointer)a);
    GFileInfo *ib = G_FILE_INFO((gpointer)b);
    bool da = g_file_info_get_file_type(ia) == G_FILE_TYPE_DIRECTORY;
    bool db = g_file_info_get_file_type(ib) == G_FILE_TYPE_DIRECTORY;
    if (da != db) return da ? -1 : 1;
    return g_utf8_collate(g_file_info_get_display_name(ia), g_file_info_get_display_name(ib));
}

// Isi satu folder: daftar direktori (dipantau, jadi file baru muncul otomatis)
// -> filter file tersembunyi -> urutkan.
static GListModel *make_dir_model(GFile *dir) {
    GtkDirectoryList *dl = gtk_directory_list_new(TREE_ATTRS, dir);
    gtk_directory_list_set_monitored(dl, TRUE);

    GtkCustomFilter *filter = gtk_custom_filter_new(tree_filter_visible, nullptr, nullptr);
    GtkFilterListModel *filtered = gtk_filter_list_model_new(G_LIST_MODEL(dl), GTK_FILTER(filter));

    GtkCustomSorter *sorter = gtk_custom_sorter_new(tree_compare, nullptr, nullptr);
    GtkSortListModel *sorted = gtk_sort_list_model_new(G_LIST_MODEL(filtered), GTK_SORTER(sorter));
    return G_LIST_MODEL(sorted);
}

// Dipanggil GtkTreeListModel saat sebuah baris folder dibuka.
static GListModel *tree_create_children(gpointer item, gpointer) {
    GFileInfo *info = G_FILE_INFO(item);
    if (g_file_info_get_file_type(info) != G_FILE_TYPE_DIRECTORY) return nullptr;
    GFile *dir = G_FILE(g_file_info_get_attribute_object(info, "standard::file"));
    if (!dir) return nullptr;
    return make_dir_model(dir);
}

static void tree_item_setup(GtkSignalListItemFactory *, GtkListItem *item, gpointer) {
    GtkWidget *expander = gtk_tree_expander_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *icon = gtk_image_new();
    GtkWidget *label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_MIDDLE);
    gtk_box_append(GTK_BOX(box), icon);
    gtk_box_append(GTK_BOX(box), label);
    gtk_tree_expander_set_child(GTK_TREE_EXPANDER(expander), box);
    gtk_list_item_set_child(item, expander);
}

static void tree_item_bind(GtkSignalListItemFactory *, GtkListItem *item, gpointer) {
    GtkTreeListRow *row = GTK_TREE_LIST_ROW(gtk_list_item_get_item(item));
    GtkWidget *expander = gtk_list_item_get_child(item);
    gtk_tree_expander_set_list_row(GTK_TREE_EXPANDER(expander), row);

    GFileInfo *info = G_FILE_INFO(gtk_tree_list_row_get_item(row));  // ref baru
    GtkWidget *box = gtk_tree_expander_get_child(GTK_TREE_EXPANDER(expander));
    GtkWidget *icon = gtk_widget_get_first_child(box);
    GtkWidget *label = gtk_widget_get_last_child(box);

    GIcon *gicon = g_file_info_get_icon(info);
    if (gicon) gtk_image_set_from_gicon(GTK_IMAGE(icon), gicon);
    gtk_label_set_text(GTK_LABEL(label), g_file_info_get_display_name(info));
    g_object_unref(info);
}

// Double-click / Enter: folder -> buka/tutup, file -> buka di editor.
static void on_tree_activate(GtkListView *view, guint position, gpointer user_data) {
    EditorApp *app = (EditorApp *)user_data;
    GListModel *model = G_LIST_MODEL(gtk_list_view_get_model(view));
    GtkTreeListRow *row = GTK_TREE_LIST_ROW(g_list_model_get_item(model, position));
    if (!row) return;

    GFileInfo *info = G_FILE_INFO(gtk_tree_list_row_get_item(row));
    if (g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY) {
        gtk_tree_list_row_set_expanded(row, !gtk_tree_list_row_get_expanded(row));
    } else {
        GFile *file = G_FILE(g_file_info_get_attribute_object(info, "standard::file"));
        if (file) request_open(app, file);
    }
    g_object_unref(info);
    g_object_unref(row);
}

static void set_project(EditorApp *app, GFile *root) {
    GFile *new_root = (GFile *)g_object_ref(root);  // root bisa saja app->project_root sendiri
    g_clear_object(&app->project_root);
    app->project_root = new_root;

    GtkTreeListModel *tree = gtk_tree_list_model_new(make_dir_model(new_root),
                                                     FALSE,  // passthrough
                                                     FALSE,  // autoexpand
                                                     tree_create_children, nullptr, nullptr);
    GtkSingleSelection *sel = gtk_single_selection_new(G_LIST_MODEL(tree));
    gtk_single_selection_set_autoselect(sel, FALSE);
    gtk_single_selection_set_can_unselect(sel, TRUE);
    gtk_list_view_set_model(GTK_LIST_VIEW(app->tree_view), GTK_SELECTION_MODEL(sel));
    g_object_unref(sel);

    char *base = g_file_get_basename(new_root);
    char *path = g_file_get_parse_name(new_root);
    gtk_label_set_text(GTK_LABEL(app->project_label), base);
    gtk_widget_set_tooltip_text(app->project_label, path);
    g_free(base);
    g_free(path);

    gtk_widget_set_visible(app->sidebar, TRUE);
}

static void on_open_project_done(GObject *source, GAsyncResult *res, gpointer user_data) {
    EditorApp *app = (EditorApp *)user_data;
    GError *error = nullptr;
    GFile *folder = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(source), res, &error);

    if (folder) {
        set_project(app, folder);
        g_object_unref(folder);
    } else {
        if (!is_dismissed(error)) show_error(app, "Gagal membuka project", error);
        g_clear_error(&error);
    }
}

static void on_toggle_sidebar(GSimpleAction *, GVariant *, gpointer user_data) {
    EditorApp *app = (EditorApp *)user_data;
    gtk_widget_set_visible(app->sidebar, !gtk_widget_get_visible(app->sidebar));
}

static void on_open_project(GSimpleAction *, GVariant *, gpointer user_data) {
    EditorApp *app = (EditorApp *)user_data;
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Open Project (pilih folder)");
    gtk_file_dialog_set_accept_label(dialog, "_Open");
    if (app->project_root)
        gtk_file_dialog_set_initial_folder(dialog, app->project_root);
    gtk_file_dialog_select_folder(dialog, GTK_WINDOW(app->window), nullptr,
                                  on_open_project_done, app);
    g_object_unref(dialog);
}


// ---------------------------------------------------------------------------
// Autopairs: ( ) [ ] { } " ' `
//   - ketik pembuka  -> sisipkan pasangan, kursor di tengah
//   - ketik penutup  -> kalau karakter berikutnya sama, cukup lompati
//   - ada seleksi    -> bungkus seleksi dengan pasangan
//   - Backspace di tengah pasangan kosong -> hapus keduanya
//   - Enter di antara {} [] () -> pecah jadi blok dengan indentasi
// ---------------------------------------------------------------------------

static gunichar closer_for(gunichar c) {
    switch (c) {
        case '(': return ')';
        case '[': return ']';
        case '{': return '}';
        case '"': return '"';
        case '\'': return '\'';
        case '`': return '`';
        default: return 0;
    }
}

static bool is_closer(gunichar c) { return c == ')' || c == ']' || c == '}'; }
static bool is_quote(gunichar c) { return c == '"' || c == '\'' || c == '`'; }
static bool is_word_char(gunichar c) { return g_unichar_isalnum(c) || c == '_'; }

static gunichar char_before(const GtkTextIter *it) {
    GtkTextIter p = *it;
    if (!gtk_text_iter_backward_char(&p)) return 0;
    return gtk_text_iter_get_char(&p);
}

static gunichar char_after(const GtkTextIter *it) {
    return gtk_text_iter_get_char(it);  // 0 di akhir buffer
}

static char *indent_unit(EditorApp *app) {
    if (!gtk_source_view_get_insert_spaces_instead_of_tabs(app->source_view))
        return g_strdup("\t");
    int w = gtk_source_view_get_indent_width(app->source_view);
    if (w <= 0) w = gtk_source_view_get_tab_width(app->source_view);
    return g_strnfill(w, ' ');
}

static void scroll_to_cursor(EditorApp *app) {
    gtk_text_view_scroll_mark_onscreen(
        GTK_TEXT_VIEW(app->source_view),
        gtk_text_buffer_get_insert(GTK_TEXT_BUFFER(app->source_buffer)));
}

static gboolean on_key_pressed(GtkEventControllerKey *, guint keyval, guint,
                               GdkModifierType state, gpointer user_data) {
    EditorApp *app = (EditorApp *)user_data;
    GtkTextBuffer *buf = GTK_TEXT_BUFFER(app->source_buffer);

    // Biarkan shortcut (Ctrl+S, dst.) lewat.
    if (state & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK | GDK_META_MASK))
        return FALSE;

    // Esc di editor -> pindah ke mode command.
    if (keyval == GDK_KEY_Escape) {
        enter_command_mode(app);
        return TRUE;
    }

    GtkTextIter start, end, cur;
    bool has_sel = gtk_text_buffer_get_selection_bounds(buf, &start, &end);
    gtk_text_buffer_get_iter_at_mark(buf, &cur, gtk_text_buffer_get_insert(buf));

    // --- Backspace di tengah pasangan kosong ---
    if (keyval == GDK_KEY_BackSpace && !has_sel) {
        gunichar prev = char_before(&cur), next = char_after(&cur);
        if (prev && closer_for(prev) == next) {
            GtkTextIter a = cur, b = cur;
            gtk_text_iter_backward_char(&a);
            gtk_text_iter_forward_char(&b);
            gtk_text_buffer_begin_user_action(buf);
            gtk_text_buffer_delete(buf, &a, &b);
            gtk_text_buffer_end_user_action(buf);
            return TRUE;
        }
        return FALSE;
    }

    // --- Enter di antara pasangan kurung ---
    if ((keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) && !has_sel) {
        gunichar prev = char_before(&cur), next = char_after(&cur);
        if ((prev == '{' && next == '}') || (prev == '[' && next == ']') ||
            (prev == '(' && next == ')')) {
            // Indentasi baris sekarang (spasi/tab di awal baris)
            GtkTextIter ls = cur;
            gtk_text_iter_set_line_offset(&ls, 0);
            GtkTextIter le = ls;
            while (!gtk_text_iter_ends_line(&le)) {
                gunichar c = gtk_text_iter_get_char(&le);
                if (c != ' ' && c != '\t') break;
                gtk_text_iter_forward_char(&le);
            }
            char *indent = gtk_text_iter_get_text(&ls, &le);
            char *unit = indent_unit(app);
            char *text = g_strdup_printf("\n%s%s\n%s", indent, unit, indent);
            int target = gtk_text_iter_get_offset(&cur) + 1 + (int)strlen(indent) + (int)strlen(unit);

            gtk_text_buffer_begin_user_action(buf);
            gtk_text_buffer_insert(buf, &cur, text, -1);
            gtk_text_buffer_end_user_action(buf);

            GtkTextIter pos;
            gtk_text_buffer_get_iter_at_offset(buf, &pos, target);
            gtk_text_buffer_place_cursor(buf, &pos);
            scroll_to_cursor(app);

            g_free(indent);
            g_free(unit);
            g_free(text);
            return TRUE;
        }
        return FALSE;
    }

    gunichar ch = gdk_keyval_to_unicode(keyval);
    if (!ch || g_unichar_iscntrl(ch)) return FALSE;
    gunichar close = closer_for(ch);

    // --- Bungkus seleksi ---
    if (has_sel && close) {
        int start_off = gtk_text_iter_get_offset(&start);
        char *text = gtk_text_buffer_get_text(buf, &start, &end, FALSE);
        int len = (int)g_utf8_strlen(text, -1);

        char o[7] = {0}, c[7] = {0};
        g_unichar_to_utf8(ch, o);
        g_unichar_to_utf8(close, c);
        char *wrapped = g_strconcat(o, text, c, nullptr);

        gtk_text_buffer_begin_user_action(buf);
        gtk_text_buffer_delete(buf, &start, &end);
        gtk_text_buffer_insert(buf, &start, wrapped, -1);
        gtk_text_buffer_end_user_action(buf);

        // Seleksi tetap menandai teks aslinya (di dalam pasangan)
        GtkTextIter s2, e2;
        gtk_text_buffer_get_iter_at_offset(buf, &s2, start_off + 1);
        gtk_text_buffer_get_iter_at_offset(buf, &e2, start_off + 1 + len);
        gtk_text_buffer_select_range(buf, &e2, &s2);

        g_free(text);
        g_free(wrapped);
        return TRUE;
    }

    if (has_sel) return FALSE;

    // --- Lompati penutup yang sudah ada ---
    if ((is_closer(ch) || is_quote(ch)) && char_after(&cur) == ch) {
        gtk_text_iter_forward_char(&cur);
        gtk_text_buffer_place_cursor(buf, &cur);
        return TRUE;
    }

    // --- Sisipkan pasangan ---
    if (close) {
        gunichar prev = char_before(&cur), next = char_after(&cur);
        if (is_word_char(next)) return FALSE;  // jangan pecah kata yang sudah ada
        // Apostrof di dalam kata ("don't") atau kutip ketiga ("""): jangan dipasangkan
        if (is_quote(ch) && (is_word_char(prev) || prev == ch)) return FALSE;

        char pair[14] = {0};
        int n = g_unichar_to_utf8(ch, pair);
        g_unichar_to_utf8(close, pair + n);

        gtk_text_buffer_begin_user_action(buf);
        gtk_text_buffer_insert_at_cursor(buf, pair, -1);
        gtk_text_buffer_end_user_action(buf);

        gtk_text_buffer_get_iter_at_mark(buf, &cur, gtk_text_buffer_get_insert(buf));
        gtk_text_iter_backward_char(&cur);
        gtk_text_buffer_place_cursor(buf, &cur);
        return TRUE;
    }

    return FALSE;
}

// ---------------------------------------------------------------------------
// Search & Replace (GtkSourceSearchContext: highlight + hitung hasil otomatis)
// ---------------------------------------------------------------------------

static bool has_search_text(EditorApp *app) {
    const char *t = gtk_source_search_settings_get_search_text(app->search_settings);
    return t && *t;
}

static void update_match_label(EditorApp *app) {
    if (!app->match_label) return;
    GtkLabel *lbl = GTK_LABEL(app->match_label);

    if (!has_search_text(app)) {
        gtk_label_set_text(lbl, "");
        return;
    }

    GError *err = gtk_source_search_context_get_regex_error(app->search_context);
    if (err) {
        gtk_label_set_text(lbl, "Regex tidak valid");
        g_error_free(err);
        return;
    }

    int count = gtk_source_search_context_get_occurrences_count(app->search_context);
    char *text;
    if (count < 0) {
        text = g_strdup("...");  // masih menghitung
    } else if (count == 0) {
        text = g_strdup("Tidak ada hasil");
    } else {
        GtkTextIter a, b;
        int pos = 0;
        if (gtk_text_buffer_get_selection_bounds(GTK_TEXT_BUFFER(app->source_buffer), &a, &b))
            pos = gtk_source_search_context_get_occurrence_position(app->search_context, &a, &b);
        text = pos > 0 ? g_strdup_printf("%d dari %d", pos, count)
                       : g_strdup_printf("%d hasil", count);
    }
    gtk_label_set_text(lbl, text);
    g_free(text);
}

static void on_search_state_changed(GObject *, GParamSpec *, gpointer user_data) {
    update_match_label((EditorApp *)user_data);
}

static void find_from(EditorApp *app, const GtkTextIter *from, bool forward) {
    GtkTextIter ms, me;
    gboolean wrapped = FALSE;
    gboolean ok = forward
        ? gtk_source_search_context_forward(app->search_context, from, &ms, &me, &wrapped)
        : gtk_source_search_context_backward(app->search_context, from, &ms, &me, &wrapped);
    if (ok) {
        gtk_text_buffer_select_range(GTK_TEXT_BUFFER(app->source_buffer), &ms, &me);
        scroll_to_cursor(app);
    }
}

static void find_next(EditorApp *app, bool forward) {
    if (!has_search_text(app)) return;
    GtkTextBuffer *buf = GTK_TEXT_BUFFER(app->source_buffer);
    GtkTextIter s, e;
    if (!gtk_text_buffer_get_selection_bounds(buf, &s, &e)) {
        gtk_text_buffer_get_iter_at_mark(buf, &s, gtk_text_buffer_get_insert(buf));
        e = s;
    }
    find_from(app, forward ? &e : &s, forward);
}

static void on_search_changed(GtkSearchEntry *entry, gpointer user_data) {
    EditorApp *app = (EditorApp *)user_data;
    gtk_source_search_settings_set_search_text(app->search_settings,
                                               gtk_editable_get_text(GTK_EDITABLE(entry)));
    // Incremental: lompat ke hasil pertama dari posisi kursor sambil mengetik.
    if (has_search_text(app)) {
        GtkTextBuffer *buf = GTK_TEXT_BUFFER(app->source_buffer);
        GtkTextIter s, e;
        if (!gtk_text_buffer_get_selection_bounds(buf, &s, &e))
            gtk_text_buffer_get_iter_at_mark(buf, &s, gtk_text_buffer_get_insert(buf));
        find_from(app, &s, true);
    }
    update_match_label(app);
}

static void on_find_next_clicked(GtkWidget *, gpointer user_data) { find_next((EditorApp *)user_data, true); }
static void on_find_prev_clicked(GtkWidget *, gpointer user_data) { find_next((EditorApp *)user_data, false); }
static void on_search_activate(GtkWidget *, gpointer user_data) { find_next((EditorApp *)user_data, true); }
static void on_search_next_match(GtkSearchEntry *, gpointer user_data) { find_next((EditorApp *)user_data, true); }
static void on_search_prev_match(GtkSearchEntry *, gpointer user_data) { find_next((EditorApp *)user_data, false); }

static void on_case_toggled(GtkToggleButton *btn, gpointer user_data) {
    EditorApp *app = (EditorApp *)user_data;
    gtk_source_search_settings_set_case_sensitive(app->search_settings,
                                                  gtk_toggle_button_get_active(btn));
}

static void on_regex_toggled(GtkToggleButton *btn, gpointer user_data) {
    EditorApp *app = (EditorApp *)user_data;
    gtk_source_search_settings_set_regex_enabled(app->search_settings,
                                                 gtk_toggle_button_get_active(btn));
}

static void do_replace(EditorApp *app) {
    if (!has_search_text(app)) return;
    GtkTextBuffer *buf = GTK_TEXT_BUFFER(app->source_buffer);
    GtkTextIter s, e;

    // Ganti hanya jika seleksi sekarang memang sebuah hasil pencarian.
    if (gtk_text_buffer_get_selection_bounds(buf, &s, &e) &&
        gtk_source_search_context_get_occurrence_position(app->search_context, &s, &e) > 0) {
        const char *rep = gtk_editable_get_text(GTK_EDITABLE(app->replace_entry));
        GError *err = nullptr;
        if (!gtk_source_search_context_replace(app->search_context, &s, &e, rep, -1, &err)) {
            show_error(app, "Gagal mengganti teks", err);
            g_clear_error(&err);
            return;
        }
        find_from(app, &e, true);  // e menunjuk akhir teks pengganti
    } else {
        find_next(app, true);
    }
}

static void do_replace_all(EditorApp *app) {
    if (!has_search_text(app)) return;
    const char *rep = gtk_editable_get_text(GTK_EDITABLE(app->replace_entry));
    GError *err = nullptr;
    guint n = gtk_source_search_context_replace_all(app->search_context, rep, -1, &err);
    if (err) {
        show_error(app, "Gagal mengganti semua", err);
        g_clear_error(&err);
        return;
    }
    char *msg = g_strdup_printf("%u diganti", n);
    gtk_label_set_text(GTK_LABEL(app->match_label), msg);
    g_free(msg);
}

static void on_replace_clicked(GtkWidget *, gpointer user_data) { do_replace((EditorApp *)user_data); }
static void on_replace_all_clicked(GtkWidget *, gpointer user_data) { do_replace_all((EditorApp *)user_data); }

static void hide_search_bar(EditorApp *app) {
    gtk_widget_set_visible(app->search_bar, FALSE);
    gtk_source_search_context_set_highlight(app->search_context, FALSE);
    gtk_widget_grab_focus(GTK_WIDGET(app->source_view));
}

static void on_close_search_clicked(GtkWidget *, gpointer user_data) {
    hide_search_bar((EditorApp *)user_data);
}

// Esc di mana pun dalam search bar -> tutup.
static gboolean on_search_bar_key(GtkEventControllerKey *, guint keyval, guint,
                                  GdkModifierType, gpointer user_data) {
    if (keyval == GDK_KEY_Escape) {
        hide_search_bar((EditorApp *)user_data);
        return TRUE;
    }
    return FALSE;
}

static void show_search_bar(EditorApp *app, bool with_replace) {
    bool was_visible = gtk_widget_get_visible(app->search_bar);
    gtk_widget_set_visible(app->search_bar, TRUE);
    if (with_replace) gtk_widget_set_visible(app->replace_row, TRUE);
    else if (!was_visible) gtk_widget_set_visible(app->replace_row, FALSE);
    gtk_source_search_context_set_highlight(app->search_context, TRUE);

    // Teks yang diseleksi (satu baris) jadi kata kunci awal.
    GtkTextBuffer *buf = GTK_TEXT_BUFFER(app->source_buffer);
    GtkTextIter s, e;
    if (gtk_text_buffer_get_selection_bounds(buf, &s, &e) &&
        gtk_text_iter_get_line(&s) == gtk_text_iter_get_line(&e)) {
        char *text = gtk_text_buffer_get_text(buf, &s, &e, FALSE);
        gtk_editable_set_text(GTK_EDITABLE(app->search_entry), text);
        g_free(text);
    }

    if (with_replace && has_search_text(app)) {
        gtk_widget_grab_focus(app->replace_entry);
    } else {
        gtk_widget_grab_focus(app->search_entry);
        gtk_editable_select_region(GTK_EDITABLE(app->search_entry), 0, -1);
    }
    update_match_label(app);
}

static void on_find(GSimpleAction *, GVariant *, gpointer user_data) { show_search_bar((EditorApp *)user_data, false); }
static void on_replace(GSimpleAction *, GVariant *, gpointer user_data) { show_search_bar((EditorApp *)user_data, true); }
static void on_find_next_action(GSimpleAction *, GVariant *, gpointer user_data) { find_next((EditorApp *)user_data, true); }
static void on_find_prev_action(GSimpleAction *, GVariant *, gpointer user_data) { find_next((EditorApp *)user_data, false); }

static GtkWidget *make_flat_button(const char *icon_name, const char *tooltip) {
    GtkWidget *btn = gtk_button_new_from_icon_name(icon_name);
    gtk_widget_add_css_class(btn, "flat");
    gtk_widget_set_tooltip_text(btn, tooltip);
    return btn;
}

static GtkWidget *build_search_bar(EditorApp *app) {
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_start(bar, 6);
    gtk_widget_set_margin_end(bar, 6);
    gtk_widget_set_margin_top(bar, 4);
    gtk_widget_set_margin_bottom(bar, 4);

    // Baris 1: cari
    GtkWidget *row1 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);

    app->search_entry = gtk_search_entry_new();
    gtk_widget_set_hexpand(app->search_entry, TRUE);
    gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(app->search_entry), "Cari...");
    g_signal_connect(app->search_entry, "search-changed", G_CALLBACK(on_search_changed), app);
    g_signal_connect(app->search_entry, "activate", G_CALLBACK(on_search_activate), app);
    g_signal_connect(app->search_entry, "next-match", G_CALLBACK(on_search_next_match), app);
    g_signal_connect(app->search_entry, "previous-match", G_CALLBACK(on_search_prev_match), app);

    GtkWidget *prev = make_flat_button("go-up-symbolic", "Sebelumnya (Shift+F3)");
    GtkWidget *next = make_flat_button("go-down-symbolic", "Berikutnya (F3)");
    g_signal_connect(prev, "clicked", G_CALLBACK(on_find_prev_clicked), app);
    g_signal_connect(next, "clicked", G_CALLBACK(on_find_next_clicked), app);

    GtkWidget *case_btn = gtk_toggle_button_new_with_label("Aa");
    gtk_widget_set_tooltip_text(case_btn, "Case sensitive");
    g_signal_connect(case_btn, "toggled", G_CALLBACK(on_case_toggled), app);

    GtkWidget *regex_btn = gtk_toggle_button_new_with_label(".*");
    gtk_widget_set_tooltip_text(regex_btn, "Regular expression (ganti dengan \\1, \\2, ...)");
    g_signal_connect(regex_btn, "toggled", G_CALLBACK(on_regex_toggled), app);

    app->match_label = gtk_label_new("");
    gtk_widget_set_size_request(app->match_label, 110, -1);
    gtk_widget_add_css_class(app->match_label, "dim-label");

    GtkWidget *close = make_flat_button("window-close-symbolic", "Tutup (Esc)");
    g_signal_connect(close, "clicked", G_CALLBACK(on_close_search_clicked), app);

    gtk_box_append(GTK_BOX(row1), app->search_entry);
    gtk_box_append(GTK_BOX(row1), prev);
    gtk_box_append(GTK_BOX(row1), next);
    gtk_box_append(GTK_BOX(row1), case_btn);
    gtk_box_append(GTK_BOX(row1), regex_btn);
    gtk_box_append(GTK_BOX(row1), app->match_label);
    gtk_box_append(GTK_BOX(row1), close);

    // Baris 2: ganti
    app->replace_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);

    app->replace_entry = gtk_entry_new();
    gtk_widget_set_hexpand(app->replace_entry, TRUE);
    gtk_entry_set_placeholder_text(GTK_ENTRY(app->replace_entry), "Ganti dengan...");
    g_signal_connect(app->replace_entry, "activate", G_CALLBACK(on_replace_clicked), app);

    GtkWidget *rep_btn = gtk_button_new_with_label("Replace");
    GtkWidget *rep_all_btn = gtk_button_new_with_label("Replace All");
    g_signal_connect(rep_btn, "clicked", G_CALLBACK(on_replace_clicked), app);
    g_signal_connect(rep_all_btn, "clicked", G_CALLBACK(on_replace_all_clicked), app);

    gtk_box_append(GTK_BOX(app->replace_row), app->replace_entry);
    gtk_box_append(GTK_BOX(app->replace_row), rep_btn);
    gtk_box_append(GTK_BOX(app->replace_row), rep_all_btn);
    gtk_widget_set_visible(app->replace_row, FALSE);

    gtk_box_append(GTK_BOX(bar), row1);
    gtk_box_append(GTK_BOX(bar), app->replace_row);

    // Esc menutup bar (fase capture supaya menang dari handler entry)
    GtkEventControllerKey *key = GTK_EVENT_CONTROLLER_KEY(gtk_event_controller_key_new());
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(key), GTK_PHASE_CAPTURE);
    g_signal_connect(key, "key-pressed", G_CALLBACK(on_search_bar_key), app);
    gtk_widget_add_controller(bar, GTK_EVENT_CONTROLLER(key));

    gtk_widget_set_visible(bar, FALSE);
    return bar;
}

// ---------------------------------------------------------------------------
// Aksi file (shortcut): Open, Save, Save As, Quit
// ---------------------------------------------------------------------------

// Open File
static void on_open_dialog_done(GObject *source, GAsyncResult *res, gpointer user_data) {
    EditorApp *app = (EditorApp *)user_data;
    GError *error = nullptr;
    GFile *file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), res, &error);

    if (file) {
        request_open(app, file);
        g_object_unref(file);
    } else {
        if (!is_dismissed(error)) show_error(app, "Gagal memilih file", error);
        g_clear_error(&error);
    }
}

static void on_open_file(GSimpleAction *, GVariant *, gpointer user_data) {
    EditorApp *app = (EditorApp *)user_data;
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Open File");
    gtk_file_dialog_set_accept_label(dialog, "_Open");
    gtk_file_dialog_open(dialog, GTK_WINDOW(app->window), nullptr, on_open_dialog_done, app);
    g_object_unref(dialog);
}

// Save As
static void on_save_as_dialog_done(GObject *source, GAsyncResult *res, gpointer user_data) {
    EditorApp *app = (EditorApp *)user_data;
    GError *error = nullptr;
    GFile *file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(source), res, &error);

    if (file) {
        start_save(app, file);
        g_object_unref(file);
    } else {
        if (!is_dismissed(error)) show_error(app, "Gagal memilih lokasi simpan", error);
        g_clear_error(&error);
    }
}

static void on_save_as_file(GSimpleAction *, GVariant *, gpointer user_data) {
    EditorApp *app = (EditorApp *)user_data;
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Save File As");
    gtk_file_dialog_set_accept_label(dialog, "_Save");

    GFile *loc = gtk_source_file_get_location(app->source_file);
    if (loc)
        gtk_file_dialog_set_initial_file(dialog, loc);
    else
        gtk_file_dialog_set_initial_name(dialog, "untitled.txt");

    gtk_file_dialog_save(dialog, GTK_WINDOW(app->window), nullptr, on_save_as_dialog_done, app);
    g_object_unref(dialog);
}

// Save
static void on_save_file(GSimpleAction *action, GVariant *param, gpointer user_data) {
    EditorApp *app = (EditorApp *)user_data;
    if (gtk_source_file_get_location(app->source_file) == nullptr)
        on_save_as_file(action, param, user_data);  // belum pernah disimpan -> Save As
    else
        start_save(app, nullptr);
}

// Exit
static void on_quit(GSimpleAction *, GVariant *, gpointer user_data) {
    EditorApp *app = (EditorApp *)user_data;
    gtk_window_close(GTK_WINDOW(app->window));
}

// ---------------------------------------------------------------------------
// Mode command: new, mkdir, rename
// ---------------------------------------------------------------------------

struct CmdResult {
    char *msg;    // pesan sukses untuk status bar
    GFile *open;  // file yang harus dibuka di editor setelah perintah (boleh NULL)
};

// Folder acuan untuk path relatif: root project, kalau tidak ada folder file
// yang sedang dibuka, kalau tidak ada juga folder kerja saat ini.
static GFile *base_dir(EditorApp *app) {
    if (app->project_root) return (GFile *)g_object_ref(app->project_root);
    GFile *loc = gtk_source_file_get_location(app->source_file);
    if (loc) {
        GFile *parent = g_file_get_parent(loc);
        if (parent) return parent;
    }
    char *cwd = g_get_current_dir();
    GFile *f = g_file_new_for_path(cwd);
    g_free(cwd);
    return f;
}

// "~/x" -> home, "/x" -> absolut, selain itu relatif terhadap base_dir().
static GFile *resolve_path(EditorApp *app, const char *arg) {
    if (arg[0] == '~' && (arg[1] == '\0' || arg[1] == '/')) {
        char *p = g_build_filename(g_get_home_dir(), arg + 1, nullptr);
        GFile *f = g_file_new_for_path(p);
        g_free(p);
        return f;
    }
    if (g_path_is_absolute(arg)) return g_file_new_for_path(arg);

    GFile *base = base_dir(app);
    GFile *f = g_file_resolve_relative_path(base, arg);
    g_object_unref(base);
    return f;
}

static bool usage_error(GError **err, const char *text) {
    g_set_error_literal(err, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, text);
    return false;
}

// new <file>
static bool cmd_new(EditorApp *app, int argc, char **argv, CmdResult *r, GError **err) {
    if (argc != 2) return usage_error(err, "Pemakaian: nf <file>");

    GFile *file = resolve_path(app, argv[1]);

    // Buat folder induk kalau belum ada (new src/util/a.cpp).
    GFile *parent = g_file_get_parent(file);
    if (parent) {
        GError *perr = nullptr;
        if (!g_file_make_directory_with_parents(parent, nullptr, &perr)) {
            if (g_error_matches(perr, G_IO_ERROR, G_IO_ERROR_EXISTS)) {
                g_error_free(perr);
            } else {
                g_propagate_error(err, perr);
                g_object_unref(parent);
                g_object_unref(file);
                return false;
            }
        }
        g_object_unref(parent);
    }

    GFileOutputStream *out = g_file_create(file, G_FILE_CREATE_NONE, nullptr, err);
    if (!out) {
        g_object_unref(file);
        return false;
    }
    g_output_stream_close(G_OUTPUT_STREAM(out), nullptr, nullptr);
    g_object_unref(out);

    char *pn = g_file_get_parse_name(file);
    r->msg = g_strdup_printf("File dibuat: %s", pn);
    g_free(pn);
    r->open = file;  // ownership pindah ke pemanggil
    return true;
}

// mkdir <folder>
static bool cmd_mkdir(EditorApp *app, int argc, char **argv, CmdResult *r, GError **err) {
    if (argc != 2) return usage_error(err, "Pemakaian: dir <folder>");

    GFile *dir = resolve_path(app, argv[1]);
    if (!g_file_make_directory_with_parents(dir, nullptr, err)) {
        g_object_unref(dir);
        return false;
    }
    char *pn = g_file_get_parse_name(dir);
    r->msg = g_strdup_printf("Folder dibuat: %s", pn);
    g_free(pn);
    g_object_unref(dir);
    return true;
}

// rename <lama> <baru>   atau   rename <baru>  (file yang sedang dibuka)
static bool cmd_rename(EditorApp *app, int argc, char **argv, CmdResult *r, GError **err) {
    if (argc != 2 && argc != 3) return usage_error(err, "Pemakaian: rn [lama] <baru>");

    GFile *old_f = nullptr;
    GFile *new_f = nullptr;

    if (argc == 3) {
        old_f = resolve_path(app, argv[1]);
        new_f = resolve_path(app, argv[2]);
    } else {
        GFile *cur = gtk_source_file_get_location(app->source_file);
        if (!cur) return usage_error(err, "Belum ada file yang terbuka. Pemakaian: rn <lama> <baru>");
        old_f = (GFile *)g_object_ref(cur);
        if (strchr(argv[1], '/') || argv[1][0] == '~') {
            new_f = resolve_path(app, argv[1]);
        } else {
            // Nama polos -> tetap di folder yang sama dengan file lama.
            GFile *parent = g_file_get_parent(old_f);
            new_f = parent ? g_file_get_child(parent, argv[1]) : resolve_path(app, argv[1]);
            if (parent) g_object_unref(parent);
        }
    }

    if (!g_file_query_exists(old_f, nullptr)) {
        char *pn = g_file_get_parse_name(old_f);
        g_set_error(err, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Tidak ditemukan: %s", pn);
        g_free(pn);
        g_object_unref(old_f);
        g_object_unref(new_f);
        return false;
    }

    if (!g_file_move(old_f, new_f, G_FILE_COPY_NONE, nullptr, nullptr, nullptr, err)) {
        g_object_unref(old_f);
        g_object_unref(new_f);
        return false;
    }

    // File yang sedang dibuka ikut pindah (langsung, atau karena folder induknya diganti).
    GFile *cur = gtk_source_file_get_location(app->source_file);
    if (cur) {
        GFile *newloc = nullptr;
        if (g_file_equal(cur, old_f)) {
            newloc = (GFile *)g_object_ref(new_f);
        } else {
            char *rel = g_file_get_relative_path(old_f, cur);
            if (rel) {
                newloc = g_file_resolve_relative_path(new_f, rel);
                g_free(rel);
            }
        }
        if (newloc) {
            gtk_source_file_set_location(app->source_file, newloc);
            g_object_unref(newloc);
            detect_language(app);
            update_window_title(app);
        }
    }

    // Root project diganti namanya -> pohon dibuat ulang di lokasi baru.
    if (app->project_root && g_file_equal(app->project_root, old_f))
        set_project(app, new_f);

    char *a = g_file_get_basename(old_f);
    char *b = g_file_get_basename(new_f);
    r->msg = g_strdup_printf("Diganti: %s -> %s", a, b);
    g_free(a);
    g_free(b);
    g_object_unref(old_f);
    g_object_unref(new_f);
    return true;
}

// project <folder>  -> isi sidebar dengan folder itu
static bool cmd_project(EditorApp *app, int argc, char **argv, CmdResult *r, GError **err) {
    if (argc != 2) return usage_error(err, "Pemakaian: project <folder>  (atau Ctrl+Shift+O)");

    GFile *dir = resolve_path(app, argv[1]);
    if (g_file_query_file_type(dir, G_FILE_QUERY_INFO_NONE, nullptr) != G_FILE_TYPE_DIRECTORY) {
        char *pn = g_file_get_parse_name(dir);
        g_set_error(err, G_IO_ERROR, G_IO_ERROR_NOT_DIRECTORY, "Bukan folder: %s", pn);
        g_free(pn);
        g_object_unref(dir);
        return false;
    }
    set_project(app, dir);
    char *pn = g_file_get_parse_name(dir);
    r->msg = g_strdup_printf("Project: %s", pn);
    g_free(pn);
    g_object_unref(dir);
    return true;
}

static void run_command(EditorApp *app, const char *raw) {
    char *line = g_strstrip(g_strdup(raw));
    if (!*line) {
        g_free(line);
        return;
    }

    int argc = 0;
    char **argv = nullptr;
    GError *err = nullptr;

    if (!g_shell_parse_argv(line, &argc, &argv, &err)) {
        char *m = g_strdup_printf("Perintah tidak valid: %s", err->message);
        set_command_message(app, m, true);
        g_free(m);
        g_clear_error(&err);
        g_free(line);
        return;
    }
    g_free(line);

    const char *name = argv[0];
    CmdResult r = {};
    bool ok = false;

// #############################################################################
//                                  Command 
// ##############################################################################
    // bisa menambahkan command baru tetapi buat fungsinya terlebih dahuli
    if (!g_strcmp0(name, "help") || !g_strcmp0(name, "?")) {
        set_command_message(app, COMMAND_HINT, false);
        g_strfreev(argv);
        return;
        // membuat file baru
    } else if (!g_strcmp0(name, "nf") {
        ok = cmd_new(app, argc, argv, &r, &err);
        // Membuat folder baru
    } else if (!g_strcmp0(name, "dir") {
        ok = cmd_mkdir(app, argc, argv, &r, &err);
        // Rename file/folder
    } else if (!g_strcmp0(name, "rn")) {
        ok = cmd_rename(app, argc, argv, &r, &err);
        // membuka folder projek
    } else if (!g_strcmp0(name, "project") {
        ok = cmd_project(app, argc, argv, &r, &err);
    } else {
        g_set_error(&err, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "Perintah tidak dikenal: %s (ketik help)", name);
    }
    g_strfreev(argv);

    if (!ok) {
        set_command_message(app, err ? err->message : "Gagal", true);
        g_clear_error(&err);
        return;  // tetap di mode command supaya bisa dikoreksi
    }

    // Sukses: kembali ke mode editor, lalu buka file baru (kalau ada).
    set_status_message(app, r.msg);
    g_free(r.msg);
    enter_editor_mode(app, true);
    if (r.open) {
        request_open(app, r.open);
        g_object_unref(r.open);
    }
}

static void on_command_activate(GtkEntry *entry, gpointer user_data) {
    run_command((EditorApp *)user_data, gtk_editable_get_text(GTK_EDITABLE(entry)));
}

// Esc di command bar -> kembali ke mode editor.
static gboolean on_command_bar_key(GtkEventControllerKey *, guint keyval, guint,
                                   GdkModifierType, gpointer user_data) {
    if (keyval == GDK_KEY_Escape) {
        enter_editor_mode((EditorApp *)user_data, true);
        return TRUE;
    }
    return FALSE;
}

// Klik ke editor / tree / dll. -> otomatis kembali ke mode editor.
static void on_command_focus_leave(GtkEventControllerFocus *, gpointer user_data) {
    EditorApp *app = (EditorApp *)user_data;
    if (app->mode == Mode::Command && gtk_window_is_active(GTK_WINDOW(app->window)))
        enter_editor_mode(app, false);
}

static GtkWidget *build_command_bar(EditorApp *app) {
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_margin_start(bar, 8);
    gtk_widget_set_margin_end(bar, 8);
    gtk_widget_set_margin_top(bar, 4);
    gtk_widget_set_margin_bottom(bar, 2);

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *prompt = gtk_label_new(":");
    gtk_widget_add_css_class(prompt, "heading");

    app->command_entry = gtk_entry_new();
    gtk_widget_set_hexpand(app->command_entry, TRUE);
    gtk_entry_set_placeholder_text(GTK_ENTRY(app->command_entry), "nf | dir | rn ...");
    g_signal_connect(app->command_entry, "activate", G_CALLBACK(on_command_activate), app);

    gtk_box_append(GTK_BOX(row), prompt);
    gtk_box_append(GTK_BOX(row), app->command_entry);

    app->command_msg = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(app->command_msg), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(app->command_msg), PANGO_ELLIPSIZE_END);
    gtk_widget_add_css_class(app->command_msg, "dim-label");

    gtk_box_append(GTK_BOX(bar), row);
    gtk_box_append(GTK_BOX(bar), app->command_msg);

    GtkEventControllerKey *key = GTK_EVENT_CONTROLLER_KEY(gtk_event_controller_key_new());
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(key), GTK_PHASE_CAPTURE);
    g_signal_connect(key, "key-pressed", G_CALLBACK(on_command_bar_key), app);
    gtk_widget_add_controller(bar, GTK_EVENT_CONTROLLER(key));

    GtkEventController *focus = gtk_event_controller_focus_new();
    g_signal_connect(focus, "leave", G_CALLBACK(on_command_focus_leave), app);
    gtk_widget_add_controller(bar, focus);

    gtk_widget_set_visible(bar, FALSE);
    return bar;
}

// ---------------------------------------------------------------------------
// UI
// ---------------------------------------------------------------------------

// Daftarkan aksi "win.<name>" ke jendela dan pasang shortcut-nya.
static void add_action(EditorApp *app, const char *name, GCallback callback, const char *accel) {
    GSimpleAction *action = g_simple_action_new(name, nullptr);
    g_signal_connect(action, "activate", callback, app);
    g_action_map_add_action(G_ACTION_MAP(app->window), G_ACTION(action));
    g_object_unref(action);

    if (accel) {
        char *detailed = g_strdup_printf("win.%s", name);
        const char *accels[] = {accel, nullptr};
        gtk_application_set_accels_for_action(app->gapp, detailed, accels);
        g_free(detailed);
    }
}

static GtkWidget *build_sidebar(EditorApp *app) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_size_request(box, 160, -1);

    app->project_label = gtk_label_new("Belum ada project");
    gtk_label_set_xalign(GTK_LABEL(app->project_label), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(app->project_label), PANGO_ELLIPSIZE_END);
    gtk_widget_add_css_class(app->project_label, "heading");
    gtk_widget_set_margin_start(app->project_label, 10);
    gtk_widget_set_margin_end(app->project_label, 10);
    gtk_widget_set_margin_top(app->project_label, 6);
    gtk_widget_set_margin_bottom(app->project_label, 6);
    gtk_box_append(GTK_BOX(box), app->project_label);

    GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
    g_signal_connect(factory, "setup", G_CALLBACK(tree_item_setup), nullptr);
    g_signal_connect(factory, "bind", G_CALLBACK(tree_item_bind), nullptr);

    app->tree_view = gtk_list_view_new(nullptr, factory);  // factory ownership pindah
    gtk_widget_add_css_class(app->tree_view, "navigation-sidebar");
    g_signal_connect(app->tree_view, "activate", G_CALLBACK(on_tree_activate), app);

    GtkWidget *scrolled = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), app->tree_view);
    gtk_widget_set_vexpand(scrolled, TRUE);
    gtk_box_append(GTK_BOX(box), scrolled);

    return box;
}

static void on_activate(GApplication *gapp, gpointer user_data) {
    EditorApp *app = (EditorApp *)user_data;
    app->gapp = GTK_APPLICATION(gapp);

    // Jendela utama
    app->window = gtk_application_window_new(app->gapp);
    gtk_application_window_set_show_menubar(GTK_APPLICATION_WINDOW(app->window), FALSE);
    gtk_window_set_default_size(GTK_WINDOW(app->window), 1000, 650);




 // #########################################################################################################################
    // Shortcut
    // Aksi + shortcut (tanpa menu bar). New file / folder / rename -> mode command.
    add_action(app, "open", G_CALLBACK(on_open_file), "<Control>o");
    add_action(app, "open-project", G_CALLBACK(on_open_project), "<Control><Shift>o");
    add_action(app, "save", G_CALLBACK(on_save_file), "<Control>s");
    add_action(app, "save-as", G_CALLBACK(on_save_as_file), "<Control><Shift>s");
    add_action(app, "quit", G_CALLBACK(on_quit), "<Control>q");
    add_action(app, "find", G_CALLBACK(on_find), "<Control>f");
    add_action(app, "replace", G_CALLBACK(on_replace), "<Control>h");
    add_action(app, "find-next", G_CALLBACK(on_find_next_action), "F3");
    add_action(app, "find-prev", G_CALLBACK(on_find_prev_action), "<Shift>F3");
    add_action(app, "toggle-sidebar", G_CALLBACK(on_toggle_sidebar), "<Control>b");

    // Layout: [sidebar | editor] di atas, command bar (mode command), status bar di bawah
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_window_set_child(GTK_WINDOW(app->window), vbox);

    // Editor GtkSourceView
    app->source_buffer = gtk_source_buffer_new(nullptr);
    app->source_file = gtk_source_file_new();

    GtkSourceStyleSchemeManager *style_manager = gtk_source_style_scheme_manager_get_default();
    GtkSourceStyleScheme *style_scheme =
        gtk_source_style_scheme_manager_get_scheme(style_manager, "oblivion");
    if (style_scheme)
        gtk_source_buffer_set_style_scheme(app->source_buffer, style_scheme);
    else {
        g_warning("GtkSourceView style scheme 'oblivion' was not found");
    }
    app->source_view = GTK_SOURCE_VIEW(gtk_source_view_new_with_buffer(app->source_buffer));
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(app->source_view), TRUE);
    gtk_source_view_set_show_line_numbers(app->source_view, TRUE);
    gtk_source_view_set_auto_indent(app->source_view, TRUE);
    gtk_source_view_set_tab_width(app->source_view, 4);
    gtk_source_view_set_highlight_current_line(app->source_view, TRUE);

    g_signal_connect(app->source_buffer, "modified-changed",
                     G_CALLBACK(on_modified_changed), app);
    g_signal_connect(app->source_buffer, "notify::cursor-position",
                     G_CALLBACK(on_cursor_moved), app);

    // Autopairs + Esc -> mode command: tangkap tombol sebelum GtkTextView memprosesnya
    GtkEventControllerKey *key = GTK_EVENT_CONTROLLER_KEY(gtk_event_controller_key_new());
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(key), GTK_PHASE_CAPTURE);
    g_signal_connect(key, "key-pressed", G_CALLBACK(on_key_pressed), app);
    gtk_widget_add_controller(GTK_WIDGET(app->source_view), GTK_EVENT_CONTROLLER(key));

    // Search & replace: settings + context (highlight hasil di buffer)
    app->search_settings = gtk_source_search_settings_new();
    gtk_source_search_settings_set_wrap_around(app->search_settings, TRUE);
    app->search_context = gtk_source_search_context_new(app->source_buffer, app->search_settings);
    gtk_source_search_context_set_highlight(app->search_context, FALSE);
    g_signal_connect(app->search_context, "notify::occurrences-count",
                     G_CALLBACK(on_search_state_changed), app);
    g_signal_connect(app->search_context, "notify::regex-error",
                     G_CALLBACK(on_search_state_changed), app);

    // Area scroll (GTK4: tanpa argumen, dan memakai set_child)
    GtkWidget *scrolled = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), GTK_WIDGET(app->source_view));
    gtk_widget_set_vexpand(scrolled, TRUE);

    // Sisi kanan: search bar (tersembunyi) di atas editor
    app->search_bar = build_search_bar(app);
    GtkWidget *editor_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(editor_box), app->search_bar);
    gtk_box_append(GTK_BOX(editor_box), scrolled);

    // Sisi kiri: project tree (tersembunyi sampai project dibuka)
    app->sidebar = build_sidebar(app);
    gtk_widget_set_visible(app->sidebar, FALSE);  // muncul saat project dibuka, Ctrl+B untuk tampil/sembunyi

    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_set_start_child(GTK_PANED(paned), app->sidebar);
    gtk_paned_set_end_child(GTK_PANED(paned), editor_box);
    gtk_paned_set_position(GTK_PANED(paned), 240);
    gtk_paned_set_resize_start_child(GTK_PANED(paned), FALSE);
    gtk_paned_set_shrink_start_child(GTK_PANED(paned), FALSE);
    gtk_paned_set_resize_end_child(GTK_PANED(paned), TRUE);
    gtk_paned_set_shrink_end_child(GTK_PANED(paned), FALSE);
    gtk_widget_set_vexpand(paned, TRUE);
    gtk_box_append(GTK_BOX(vbox), paned);

    // Command bar (hanya tampil di mode command)
    app->command_bar = build_command_bar(app);
    gtk_box_append(GTK_BOX(vbox), app->command_bar);

    // Status bar tipis di bawah editor
    GtkWidget *status = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_start(status, 10);
    gtk_widget_set_margin_end(status, 10);
    gtk_widget_set_margin_top(status, 3);
    gtk_widget_set_margin_bottom(status, 3);

    app->mode_label = gtk_label_new("EDITOR");
    gtk_widget_add_css_class(app->mode_label, "heading");
    gtk_widget_set_size_request(app->mode_label, 80, -1);
    gtk_label_set_xalign(GTK_LABEL(app->mode_label), 0.0);

    app->status_left = gtk_label_new("");
    gtk_widget_set_hexpand(app->status_left, TRUE);
    gtk_widget_set_halign(app->status_left, GTK_ALIGN_START);

    app->status_msg = gtk_label_new("");
    gtk_widget_add_css_class(app->status_msg, "dim-label");
    gtk_label_set_ellipsize(GTK_LABEL(app->status_msg), PANGO_ELLIPSIZE_END);

    app->status_right = gtk_label_new("");
    gtk_widget_set_halign(app->status_right, GTK_ALIGN_END);

    gtk_box_append(GTK_BOX(status), app->mode_label);
    gtk_box_append(GTK_BOX(status), app->status_left);
    gtk_box_append(GTK_BOX(status), app->status_msg);
    gtk_box_append(GTK_BOX(status), app->status_right);
    gtk_box_append(GTK_BOX(vbox), status);

    GtkSettings *settings = gtk_settings_get_default();
    g_object_set(settings,
        "gtk-theme-name",     "Tokyonight-Dark",
        "gtk-icon-theme-name", "Papirus-Dark",
        "gtk-font-name",      "JetBrainsMono Nerd Font Mono 11",
        NULL);

    app->mode = Mode::Editor;
    update_window_title(app);
    gtk_window_present(GTK_WINDOW(app->window));
    gtk_widget_grab_focus(GTK_WIDGET(app->source_view));
}

int main(int argc, char *argv[]) {
    gtk_source_init();

    EditorApp app = {};
    GtkApplication *gapp = gtk_application_new("local.teditor", G_APPLICATION_DEFAULT_FLAGS);

    GtkSettings *settings = gtk_settings_get_default();

    g_object_set(settings,
        "gtk-theme-name",     "Tokyonight-Dark",
        "gtk-icon-theme-name", "Papirus-Dark",
        "gtk-font-name",      "JetBrainsMono Nerd Font Mono 11",
        NULL);

    g_signal_connect(gapp, "activate", G_CALLBACK(on_activate), &app);

    int status = g_application_run(G_APPLICATION(gapp), argc, argv);

    if (app.source_file) g_object_unref(app.source_file);
    if (app.search_context) g_object_unref(app.search_context);
    if (app.search_settings) g_object_unref(app.search_settings);
    if (app.project_root) g_object_unref(app.project_root);
    g_object_unref(gapp);
    gtk_source_finalize();
    return status;
}
