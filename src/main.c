#define COBJMACROS
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <stdlib.h>
#include <stdio.h>
#include <wchar.h>
#include "convert.h"

#define WM_CONVERTED (WM_APP + 1)
#define ID_ADD 101
#define ID_FORMAT 102
#define ID_QUALITY 103
#define ID_SOURCE 104
#define ID_CUSTOM 105
#define ID_BROWSE 106
#define ID_OPEN 107
#define ID_STOP 108
#define ID_CLEAR 109
#define ID_RETRY 110
#define ID_LIST 111
#define ID_DELETE_SOURCE 112
#define ID_ABOUT 0x1e10
#define MAX_JOBS 20000

typedef enum JobState { JOB_WAITING, JOB_RUNNING, JOB_DONE, JOB_FAILED, JOB_STOPPED } JobState;
typedef struct Job {
    wchar_t *source;
    wchar_t *directory;
    OutputFormat format;
    int quality;
    int delete_original;
    int source_deleted;
    int index;
    JobState state;
    ConvertResult result;
} Job;

typedef struct Application {
    HINSTANCE instance;
    HWND window, title, add, format_label, format, quality_label, quality, spin;
    HWND source_radio, custom_radio, delete_original, directory, browse, open, list, empty;
    HWND details, status, progress, stop, clear, retry;
    HFONT font, title_font;
    HBRUSH background;
    UINT dpi;
    Job **jobs;
    int count, capacity;
    HANDLE worker;
    volatile LONG cancel;
    int closing;
    int ignored;
    wchar_t *custom_directory;
} Application;

static Application app;
static void start_next(void);
static void refresh_status(void);

static int px(int value) { return MulDiv(value, app.dpi ? app.dpi : 96, 96); }

static void column_text(int row, int column, const wchar_t *text) {
    LVITEMW item = {0};
    item.iSubItem = column;
    item.pszText = (wchar_t *)text;
    SendMessageW(app.list, LVM_SETITEMTEXTW, row, (LPARAM)&item);
}

static void update_job(Job *job) {
    const wchar_t *state = L"等待中";
    if (job->state == JOB_RUNNING) state = L"转换中";
    else if (job->state == JOB_DONE) {
        if (job->delete_original) state = job->source_deleted ? L"完成·已删除" : L"完成·未删除";
        else state = job->result.animated ? L"完成（首帧）" : L"完成";
    }
    else if (job->state == JOB_FAILED) state = L"失败";
    else if (job->state == JOB_STOPPED) state = L"已停止";
    column_text(job->index, 1, job->format == OUTPUT_PNG ? L"PNG" : L"JPEG");
    column_text(job->index, 2, state);
    column_text(job->index, 3, job->result.message[0] ? job->result.message : (job->result.path ? job->result.path : L""));
}

static void set_busy(int busy) {
    EnableWindow(app.format, !busy);
    EnableWindow(app.source_radio, !busy);
    EnableWindow(app.custom_radio, !busy);
    EnableWindow(app.browse, !busy);
    EnableWindow(app.delete_original, !busy);
    int jpeg = SendMessageW(app.format, CB_GETCURSEL, 0, 0) == 1;
    EnableWindow(app.quality, !busy && jpeg);
    EnableWindow(app.spin, !busy && jpeg);
    EnableWindow(app.quality_label, !busy && jpeg);
    EnableWindow(app.clear, !busy && app.count > 0);
    EnableWindow(app.stop, busy);
    int failed = 0;
    for (int index = 0; index < app.count; ++index)
        if (app.jobs[index]->state == JOB_FAILED || app.jobs[index]->state == JOB_STOPPED) failed = 1;
    EnableWindow(app.retry, !busy && failed);
}

static void refresh_status(void) {
    int done = 0, failed = 0, stopped = 0;
    for (int index = 0; index < app.count; ++index) {
        switch (app.jobs[index]->state) {
            case JOB_DONE: ++done; break;
            case JOB_FAILED: ++failed; break;
            case JOB_STOPPED: ++stopped; break;
            default: break;
        }
    }
    wchar_t text[256];
    if (!app.count) wcscpy(text, L"就绪");
    else if (stopped) swprintf(text, 256, L"完成 %d/%d   失败 %d   停止 %d", done, app.count, failed, stopped);
    else swprintf(text, 256, L"完成 %d/%d   失败 %d", done, app.count, failed);
    SetWindowTextW(app.status, text);
    SendMessageW(app.progress, PBM_SETRANGE32, 0, app.count ? app.count : 1);
    SendMessageW(app.progress, PBM_SETPOS, done + failed + stopped, 0);
    ShowWindow(app.empty, app.count ? SW_HIDE : SW_SHOW);
    EnableWindow(app.open, app.count || (app.custom_directory &&
                 SendMessageW(app.custom_radio, BM_GETCHECK, 0, 0) == BST_CHECKED));
    set_busy(app.worker != NULL);
}

static DWORD WINAPI conversion_thread(void *parameter) {
    Job *job = parameter;
    ConvertRequest request = { job->source, job->directory, job->format, job->quality, &app.cancel };
    int success = convert_webp(&request, &job->result);
    if (success && job->delete_original)
        job->source_deleted = recycle_source(job->source, &job->result, &app.cancel);
    PostMessageW(app.window, WM_CONVERTED, (WPARAM)success, (LPARAM)job);
    return 0;
}

static void start_next(void) {
    if (app.worker || app.closing) return;
    for (int index = 0; index < app.count; ++index) {
        Job *job = app.jobs[index];
        if (job->state != JOB_WAITING) continue;
        InterlockedExchange(&app.cancel, 0);
        job->state = JOB_RUNNING;
        update_job(job);
        app.worker = CreateThread(NULL, 0, conversion_thread, job, 0, NULL);
        if (!app.worker) {
            job->state = JOB_FAILED;
            wcscpy(job->result.message, L"无法启动转换线程");
            update_job(job);
            continue;
        }
        break;
    }
    refresh_status();
}

static int add_file(const wchar_t *path) {
    const wchar_t *extension = wcsrchr(path, L'.');
    if (!extension || _wcsicmp(extension, L".webp")) { ++app.ignored; return 0; }
    if (app.count >= MAX_JOBS) { ++app.ignored; return 0; }
    wchar_t *full = absolute_path(path);
    if (!full) { ++app.ignored; return 0; }
    OutputFormat format = SendMessageW(app.format, CB_GETCURSEL, 0, 0) == 1 ? OUTPUT_JPEG : OUTPUT_PNG;
    int custom = SendMessageW(app.custom_radio, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const wchar_t *directory = custom && app.custom_directory ? app.custom_directory : L"";
    int quality = GetDlgItemInt(app.window, ID_QUALITY, NULL, FALSE);
    if (quality < 1) quality = 1;
    if (quality > 100) quality = 100;
    int delete_original = SendMessageW(app.delete_original, BM_GETCHECK, 0, 0) == BST_CHECKED;
    for (int index = 0; index < app.count; ++index) {
        Job *existing = app.jobs[index];
        if (!_wcsicmp(existing->source, full) && existing->format == format &&
            existing->delete_original == delete_original &&
            (format == OUTPUT_PNG || existing->quality == quality) &&
            !_wcsicmp(existing->directory, directory)) { free(full); ++app.ignored; return 0; }
    }
    if (app.count == app.capacity) {
        int capacity = app.capacity ? app.capacity * 2 : 64;
        Job **jobs = realloc(app.jobs, (size_t)capacity * sizeof(*jobs));
        if (!jobs) { free(full); ++app.ignored; return 0; }
        app.jobs = jobs; app.capacity = capacity;
    }
    Job *job = calloc(1, sizeof(*job));
    if (!job) { free(full); ++app.ignored; return 0; }
    job->source = full;
    job->directory = copy_text(directory);
    if (!job->directory) { free(full); free(job); ++app.ignored; return 0; }
    job->format = format; job->quality = quality; job->index = app.count;
    job->delete_original = delete_original;
    app.jobs[app.count++] = job;
    const wchar_t *name = wcsrchr(full, L'\\');
    LVITEMW item = {0};
    item.mask = LVIF_TEXT;
    item.iItem = job->index;
    item.pszText = (wchar_t *)(name ? name + 1 : full);
    SendMessageW(app.list, LVM_INSERTITEMW, 0, (LPARAM)&item);
    update_job(job);
    return 1;
}

static void add_path(const wchar_t *path) {
    DWORD attributes = GetFileAttributesW(path);
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY)) {
        wchar_t *pattern = join_path(path, L"*.webp");
        if (!pattern) { ++app.ignored; return; }
        WIN32_FIND_DATAW file;
        HANDLE find = FindFirstFileW(pattern, &file);
        free(pattern);
        if (find == INVALID_HANDLE_VALUE) { ++app.ignored; return; }
        do {
            if (!(file.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                wchar_t *full = join_path(path, file.cFileName);
                if (full) { add_file(full); free(full); }
            }
        } while (FindNextFileW(find, &file));
        FindClose(find);
    } else add_file(path);
}

static void finish_adding(void) {
    start_next();
    if (!app.worker && app.ignored) {
        wchar_t text[160];
        swprintf(text, 160, L"已忽略 %d 项（非 WebP、重复文件或空文件夹）", app.ignored);
        SetWindowTextW(app.status, text);
    }
    app.ignored = 0;
}

static HRESULT choose_dialog(int folder, int multiple, IFileOpenDialog **dialog) {
    HRESULT hr = CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER,
                                  &IID_IFileOpenDialog, (void **)dialog);
    if (FAILED(hr)) return hr;
    FILEOPENDIALOGOPTIONS options = 0;
    IFileOpenDialog_GetOptions(*dialog, &options);
    options |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST;
    if (folder) options |= FOS_PICKFOLDERS;
    else options |= FOS_FILEMUSTEXIST;
    if (multiple) options |= FOS_ALLOWMULTISELECT;
    IFileOpenDialog_SetOptions(*dialog, options);
    if (!folder) {
        COMDLG_FILTERSPEC filter[] = { { L"WebP 图片", L"*.webp" } };
        IFileOpenDialog_SetFileTypes(*dialog, 1, filter);
        IFileOpenDialog_SetTitle(*dialog, L"添加 WebP 图片");
    } else IFileOpenDialog_SetTitle(*dialog, L"选择输出文件夹");
    return IFileOpenDialog_Show(*dialog, app.window);
}

static void select_files(void) {
    IFileOpenDialog *dialog = NULL;
    if (SUCCEEDED(choose_dialog(0, 1, &dialog))) {
        IShellItemArray *items = NULL;
        if (SUCCEEDED(IFileOpenDialog_GetResults(dialog, &items))) {
            DWORD count = 0;
            IShellItemArray_GetCount(items, &count);
            SendMessageW(app.list, WM_SETREDRAW, FALSE, 0);
            for (DWORD index = 0; index < count; ++index) {
                IShellItem *item = NULL;
                PWSTR path = NULL;
                if (SUCCEEDED(IShellItemArray_GetItemAt(items, index, &item))) {
                    if (SUCCEEDED(IShellItem_GetDisplayName(item, SIGDN_FILESYSPATH, &path))) {
                        add_path(path); CoTaskMemFree(path);
                    }
                    IShellItem_Release(item);
                }
            }
            IShellItemArray_Release(items);
            SendMessageW(app.list, WM_SETREDRAW, TRUE, 0);
            InvalidateRect(app.list, NULL, TRUE);
            finish_adding();
        }
    }
    if (dialog) IFileOpenDialog_Release(dialog);
}

static void set_output_mode(int custom) {
    SendMessageW(app.source_radio, BM_SETCHECK, custom ? BST_UNCHECKED : BST_CHECKED, 0);
    SendMessageW(app.custom_radio, BM_SETCHECK, custom ? BST_CHECKED : BST_UNCHECKED, 0);
    SetWindowTextW(app.directory, custom && app.custom_directory
                   ? app.custom_directory : L"每张 WebP 所在的文件夹");
    EnableWindow(app.open, app.count || (custom && app.custom_directory));
}

static void select_directory(void) {
    IFileOpenDialog *dialog = NULL;
    if (SUCCEEDED(choose_dialog(1, 0, &dialog))) {
        IShellItem *item = NULL;
        PWSTR path = NULL;
        if (SUCCEEDED(IFileOpenDialog_GetResult(dialog, &item))) {
            if (SUCCEEDED(IShellItem_GetDisplayName(item, SIGDN_FILESYSPATH, &path))) {
                wchar_t *selected = copy_text(path);
                if (selected) { free(app.custom_directory); app.custom_directory = selected; set_output_mode(1); }
                CoTaskMemFree(path);
            }
            IShellItem_Release(item);
        }
    }
    if (dialog) IFileOpenDialog_Release(dialog);
    if (!app.custom_directory) set_output_mode(0);
}

static void open_output(void) {
    wchar_t *directory = NULL;
    int selected = ListView_GetNextItem(app.list, -1, LVNI_SELECTED);
    Job *job = selected >= 0 && selected < app.count ? app.jobs[selected] : NULL;
    if (!job && app.count) job = app.jobs[app.count - 1];
    if (job) directory = job->result.path ? parent_directory(job->result.path)
                       : (job->directory[0] ? copy_text(job->directory) : parent_directory(job->source));
    else if (app.custom_directory) directory = copy_text(app.custom_directory);
    if (directory) {
        ShellExecuteW(app.window, L"open", directory, NULL, NULL, SW_SHOWNORMAL);
        free(directory);
    }
}

static void clear_jobs(void) {
    if (app.worker) return;
    ListView_DeleteAllItems(app.list);
    for (int index = 0; index < app.count; ++index) {
        Job *job = app.jobs[index];
        free(job->source); free(job->directory); free(job->result.path); free(job);
    }
    app.count = 0;
    SetWindowTextW(app.details, L"");
    refresh_status();
}

static void stop_jobs(void) {
    InterlockedExchange(&app.cancel, 1);
    for (int index = 0; index < app.count; ++index) {
        Job *job = app.jobs[index];
        if (job->state == JOB_WAITING) { job->state = JOB_STOPPED; update_job(job); }
    }
    EnableWindow(app.stop, FALSE);
    SetWindowTextW(app.status, L"正在停止，等待当前图片处理结束…");
}

static void retry_jobs(void) {
    if (app.worker) return;
    for (int index = 0; index < app.count; ++index) {
        Job *job = app.jobs[index];
        if (job->state == JOB_FAILED || job->state == JOB_STOPPED) {
            job->delete_original = SendMessageW(app.delete_original, BM_GETCHECK, 0, 0) == BST_CHECKED;
            job->source_deleted = 0;
            free(job->result.path);
            ZeroMemory(&job->result, sizeof(job->result));
            job->state = JOB_WAITING;
            update_job(job);
        }
    }
    start_next();
}

static void show_about(void) {
    HRSRC resource = FindResourceW(app.instance, MAKEINTRESOURCEW(101), RT_RCDATA);
    HGLOBAL loaded = resource ? LoadResource(app.instance, resource) : NULL;
    const char *text = loaded ? LockResource(loaded) : NULL;
    int size = resource ? (int)SizeofResource(app.instance, resource) : 0;
    int count = text ? MultiByteToWideChar(CP_UTF8, 0, text, size, NULL, 0) : 0;
    wchar_t *license = calloc((size_t)count + 1, sizeof(wchar_t));
    if (license && count) MultiByteToWideChar(CP_UTF8, 0, text, size, license, count);
    TASKDIALOGCONFIG dialog = {0};
    dialog.cbSize = sizeof(dialog);
    dialog.hwndParent = app.window;
    dialog.dwCommonButtons = TDCBF_CLOSE_BUTTON;
    dialog.pszWindowTitle = L"关于 WebP 转换器";
    dialog.pszMainInstruction = L"WebP 转换器 1.0";
    dialog.pszContent = L"libwebp 1.6.0";
    dialog.pszExpandedInformation = license;
    dialog.pszExpandedControlText = L"libwebp 许可";
    dialog.pszCollapsedControlText = L"libwebp 许可";
    TaskDialogIndirect(&dialog, NULL, NULL, NULL);
    free(license);
}

static HWND control(const wchar_t *class_name, const wchar_t *text, DWORD style, int id) {
    HWND window = CreateWindowExW(0, class_name, text, WS_CHILD | WS_VISIBLE | style,
                                  0, 0, 10, 10, app.window, (HMENU)(INT_PTR)id, app.instance, NULL);
    SendMessageW(window, WM_SETFONT, (WPARAM)app.font, TRUE);
    return window;
}

static BOOL CALLBACK apply_font(HWND window, LPARAM parameter) {
    SendMessageW(window, WM_SETFONT, (WPARAM)app.font, FALSE);
    return TRUE;
}

static void make_fonts(void) {
    HFONT old_font = app.font, old_title = app.title_font;
    app.font = CreateFontW(-px(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                           OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH, L"Microsoft YaHei UI");
    app.title_font = CreateFontW(-px(22), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                DEFAULT_PITCH, L"Microsoft YaHei UI");
    if (app.window) {
        EnumChildWindows(app.window, apply_font, 0);
        SendMessageW(app.title, WM_SETFONT, (WPARAM)app.title_font, TRUE);
    }
    if (old_font) DeleteObject(old_font);
    if (old_title) DeleteObject(old_title);
}

static void move_control(HWND window, int x, int y, int width, int height) {
    MoveWindow(window, px(x), px(y), px(width), px(height), TRUE);
}

static void layout(void) {
    RECT client;
    GetClientRect(app.window, &client);
    int width = MulDiv(client.right, 96, app.dpi);
    int height = MulDiv(client.bottom, 96, app.dpi);
    if (width < 1 || height < 1) return;
    move_control(app.title, 24, 16, width - 210, 36);
    move_control(app.add, width - 160, 20, 136, 32);
    move_control(app.format_label, 24, 67, 42, 24);
    move_control(app.format, 72, 62, 196, 240);
    move_control(app.quality_label, 286, 67, 68, 24);
    move_control(app.quality, 358, 62, 54, 28);
    move_control(app.spin, 412, 62, 20, 28);
    move_control(app.open, width - 160, 62, 136, 32);
    move_control(app.source_radio, 24, 108, 160, 28);
    move_control(app.custom_radio, 194, 108, 160, 28);
    move_control(app.delete_original, 382, 108, width - 406, 28);
    move_control(app.directory, 24, 144, width - 172, 28);
    move_control(app.browse, width - 136, 144, 112, 28);
    move_control(app.list, 24, 192, width - 48, height - 308);
    move_control(app.empty, width / 2 - 140, 192 + (height - 308) / 2, 280, 28);
    move_control(app.details, 24, height - 104, width - 48, 24);
    move_control(app.progress, 24, height - 62, width - 422, 16);
    move_control(app.status, 24, height - 38, width - 422, 24);
    move_control(app.stop, width - 380, height - 62, 112, 34);
    move_control(app.retry, width - 258, height - 62, 112, 34);
    move_control(app.clear, width - 136, height - 62, 112, 34);
    ListView_SetColumnWidth(app.list, 0, px((width - 48) * 32 / 100));
    ListView_SetColumnWidth(app.list, 1, px(68));
    ListView_SetColumnWidth(app.list, 2, px(126));
    ListView_SetColumnWidth(app.list, 3, px((width - 48) * 68 / 100 - 220));
}

static void create_controls(void) {
    app.title = control(L"STATIC", L"WebP 批量转换", SS_LEFT, 0);
    SendMessageW(app.title, WM_SETFONT, (WPARAM)app.title_font, TRUE);
    app.add = control(L"BUTTON", L"添加图片…", BS_PUSHBUTTON | WS_TABSTOP, ID_ADD);
    app.format_label = control(L"STATIC", L"格式", SS_LEFT, 0);
    app.format = control(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, ID_FORMAT);
    SendMessageW(app.format, CB_ADDSTRING, 0, (LPARAM)L"PNG  (.png)");
    SendMessageW(app.format, CB_ADDSTRING, 0, (LPARAM)L"JPEG  (.jpg)");
    SendMessageW(app.format, CB_SETCURSEL, 0, 0);
    app.quality_label = control(L"STATIC", L"质量 %", SS_LEFT, 0);
    app.quality = control(L"EDIT", L"92", ES_NUMBER | ES_CENTER | WS_BORDER | WS_TABSTOP, ID_QUALITY);
    SendMessageW(app.quality, EM_SETLIMITTEXT, 3, 0);
    app.spin = control(UPDOWN_CLASSW, L"", UDS_SETBUDDYINT | UDS_ARROWKEYS, 0);
    SendMessageW(app.spin, UDM_SETBUDDY, (WPARAM)app.quality, 0);
    SendMessageW(app.spin, UDM_SETRANGE32, 1, 100);
    SendMessageW(app.spin, UDM_SETPOS32, 0, 92);
    app.open = control(L"BUTTON", L"打开输出文件夹", BS_PUSHBUTTON | WS_TABSTOP, ID_OPEN);
    app.source_radio = control(L"BUTTON", L"原图所在文件夹", BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP, ID_SOURCE);
    app.custom_radio = control(L"BUTTON", L"指定输出文件夹", BS_AUTORADIOBUTTON | WS_TABSTOP, ID_CUSTOM);
    app.delete_original = control(L"BUTTON", L"成功后删除原 WebP（回收站）", BS_AUTOCHECKBOX | WS_GROUP | WS_TABSTOP, ID_DELETE_SOURCE);
    app.directory = control(L"EDIT", L"", ES_READONLY | ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, 0);
    app.browse = control(L"BUTTON", L"选择目录…", BS_PUSHBUTTON | WS_TABSTOP, ID_BROWSE);
    app.list = control(WC_LISTVIEWW, L"", LVS_REPORT | LVS_SHOWSELALWAYS | WS_BORDER | WS_TABSTOP, ID_LIST);
    ListView_SetExtendedListViewStyle(app.list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    const wchar_t *headers[] = { L"WebP 文件", L"格式", L"状态", L"输出 / 错误信息" };
    for (int index = 0; index < 4; ++index) {
        LVCOLUMNW column = {0};
        column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        column.pszText = (wchar_t *)headers[index];
        column.cx = px(180); column.iSubItem = index;
        SendMessageW(app.list, LVM_INSERTCOLUMNW, index, (LPARAM)&column);
    }
    app.empty = control(L"STATIC", L"暂无 WebP 图片", SS_CENTER, 0);
    app.details = control(L"EDIT", L"", ES_READONLY | ES_AUTOHSCROLL | WS_TABSTOP, 0);
    app.progress = control(PROGRESS_CLASSW, L"", PBS_SMOOTH, 0);
    app.status = control(L"STATIC", L"就绪", SS_LEFT, 0);
    app.stop = control(L"BUTTON", L"停止", BS_PUSHBUTTON | WS_TABSTOP, ID_STOP);
    app.retry = control(L"BUTTON", L"重试未完成", BS_PUSHBUTTON | WS_TABSTOP, ID_RETRY);
    app.clear = control(L"BUTTON", L"清空列表", BS_PUSHBUTTON | WS_TABSTOP, ID_CLEAR);
    set_output_mode(0);
    refresh_status();
    DragAcceptFiles(app.window, TRUE);
}

static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_CREATE:
            app.window = window;
            app.dpi = GetDpiForWindow(window);
            make_fonts(); create_controls(); layout();
            AppendMenuW(GetSystemMenu(window, FALSE), MF_SEPARATOR, 0, NULL);
            AppendMenuW(GetSystemMenu(window, FALSE), MF_STRING, ID_ABOUT, L"关于 WebP 转换器…");
            return 0;
        case WM_SIZE: if (app.list) layout(); return 0;
        case WM_DPICHANGED: {
            app.dpi = HIWORD(wparam);
            make_fonts();
            RECT *rectangle = (RECT *)lparam;
            SetWindowPos(window, NULL, rectangle->left, rectangle->top,
                         rectangle->right - rectangle->left, rectangle->bottom - rectangle->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            layout(); return 0;
        }
        case WM_GETMINMAXINFO: {
            MINMAXINFO *limits = (MINMAXINFO *)lparam;
            limits->ptMinTrackSize.x = px(800);
            limits->ptMinTrackSize.y = px(540);
            return 0;
        }
        case WM_ERASEBKGND: {
            RECT client; GetClientRect(window, &client);
            FillRect((HDC)wparam, &client, app.background);
            return 1;
        }
        case WM_CTLCOLORSTATIC: {
            HDC dc = (HDC)wparam;
            SetTextColor(dc, (HWND)lparam == app.title ? RGB(25, 29, 34) : RGB(68, 72, 79));
            SetBkColor(dc, RGB(247, 248, 250));
            return (LRESULT)app.background;
        }
        case WM_DROPFILES: {
            HDROP drop = (HDROP)wparam;
            UINT count = DragQueryFileW(drop, 0xffffffff, NULL, 0);
            SendMessageW(app.list, WM_SETREDRAW, FALSE, 0);
            for (UINT index = 0; index < count; ++index) {
                UINT length = DragQueryFileW(drop, index, NULL, 0);
                wchar_t *path = calloc((size_t)length + 1, sizeof(wchar_t));
                if (path) { DragQueryFileW(drop, index, path, length + 1); add_path(path); free(path); }
            }
            DragFinish(drop);
            SendMessageW(app.list, WM_SETREDRAW, TRUE, 0);
            InvalidateRect(app.list, NULL, TRUE);
            SetFocus(app.list); finish_adding(); return 0;
        }
        case WM_CONVERTED: {
            Job *job = (Job *)lparam;
            CloseHandle(app.worker); app.worker = NULL;
            job->state = wparam ? JOB_DONE : (job->result.cancelled ? JOB_STOPPED : JOB_FAILED);
            update_job(job);
            if (ListView_GetNextItem(app.list, -1, LVNI_SELECTED) == job->index)
                SetWindowTextW(app.details, job->result.message[0] ? job->result.message : job->result.path);
            if (app.closing) DestroyWindow(window);
            else start_next();
            return 0;
        }
        case WM_SYSCOMMAND:
            if ((wparam & 0xfff0) == ID_ABOUT) { show_about(); return 0; }
            break;
        case WM_COMMAND:
            switch (LOWORD(wparam)) {
                case ID_ADD: select_files(); break;
                case ID_FORMAT: if (HIWORD(wparam) == CBN_SELCHANGE) set_busy(app.worker != NULL); break;
                case ID_SOURCE: set_output_mode(0); break;
                case ID_CUSTOM:
                    if (app.custom_directory) set_output_mode(1);
                    else select_directory();
                    break;
                case ID_BROWSE: select_directory(); break;
                case ID_OPEN: open_output(); break;
                case ID_STOP: stop_jobs(); break;
                case ID_RETRY: retry_jobs(); break;
                case ID_CLEAR: clear_jobs(); break;
                case ID_QUALITY:
                    if (HIWORD(wparam) == EN_KILLFOCUS) {
                        int quality = GetDlgItemInt(window, ID_QUALITY, NULL, FALSE);
                        if (quality < 1) quality = 1;
                        if (quality > 100) quality = 100;
                        SendMessageW(app.spin, UDM_SETPOS32, 0, quality);
                    }
                    break;
            }
            return 0;
        case WM_NOTIFY: {
            NMHDR *notification = (NMHDR *)lparam;
            if (notification->hwndFrom == app.list && notification->code == NM_DBLCLK) open_output();
            if (notification->hwndFrom == app.list && notification->code == LVN_ITEMCHANGED) {
                int selected = ListView_GetNextItem(app.list, -1, LVNI_SELECTED);
                if (selected >= 0 && selected < app.count) {
                    Job *job = app.jobs[selected];
                    SetWindowTextW(app.details, job->result.message[0] ? job->result.message
                        : (job->result.path ? job->result.path : job->source));
                }
            }
            return 0;
        }
        case WM_CLOSE:
            if (app.worker) { app.closing = 1; stop_jobs(); EnableWindow(window, FALSE); }
            else DestroyWindow(window);
            return 0;
        case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

static int command_line_convert(int count, wchar_t **arguments) {
    OutputFormat format = OUTPUT_PNG;
    int quality = 92, first_file = 2;
    wchar_t *directory = NULL;
    while (first_file < count && wcsncmp(arguments[first_file], L"--", 2) == 0) {
        const wchar_t *option = arguments[first_file++];
        if (!wcscmp(option, L"--png")) format = OUTPUT_PNG;
        else if (!wcscmp(option, L"--jpeg")) format = OUTPUT_JPEG;
        else if (!wcscmp(option, L"--output") && first_file < count) {
            free(directory); directory = absolute_path(arguments[first_file++]);
            if (!directory) return 2;
        } else if (!wcscmp(option, L"--quality") && first_file < count) {
            quality = _wtoi(arguments[first_file++]);
            if (quality < 1 || quality > 100) { free(directory); return 2; }
        } else { free(directory); return 2; }
    }
    if (first_file == count) { free(directory); return 2; }
    int failures = 0, converted = 0;
    for (int index = first_file; index < count; ++index) {
        wchar_t *source = absolute_path(arguments[index]);
        if (!source) { ++failures; continue; }
        ConvertRequest request = { source, directory, format, quality, NULL };
        ConvertResult result;
        if (convert_webp(&request, &result)) ++converted;
        else ++failures;
        free(result.path); free(source);
    }
    free(directory);
    char summary[100];
    int length = snprintf(summary, sizeof(summary), "converted=%d failed=%d\r\n", converted, failures);
    DWORD written;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), summary, (DWORD)length, &written, NULL);
    return failures ? 1 : 0;
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR command, int show) {
    int argument_count = 0;
    wchar_t **arguments = CommandLineToArgvW(GetCommandLineW(), &argument_count);
    if (!arguments) return 2;
    if (argument_count > 1 && !wcscmp(arguments[1], L"--convert")) {
        int code = command_line_convert(argument_count, arguments);
        LocalFree(arguments); return code;
    }
    app.instance = instance;
    app.dpi = 96;
    app.background = CreateSolidBrush(RGB(247, 248, 250));
    HRESULT initialized = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized)) { LocalFree(arguments); DeleteObject(app.background); return 2; }
    INITCOMMONCONTROLSEX controls = { sizeof(controls), ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS | ICC_UPDOWN_CLASS };
    InitCommonControlsEx(&controls);
    WNDCLASSEXW window_class = {0};
    window_class.cbSize = sizeof(window_class);
    window_class.style = CS_HREDRAW | CS_VREDRAW;
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = instance;
    window_class.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    window_class.hIconSm = window_class.hIcon;
    window_class.hCursor = LoadCursorW(NULL, IDC_ARROW);
    window_class.hbrBackground = app.background;
    window_class.lpszClassName = L"WebpDropWindow";
    RegisterClassExW(&window_class);
    UINT dpi = GetDpiForSystem();
    HWND window = CreateWindowExW(WS_EX_ACCEPTFILES, window_class.lpszClassName,
                                  L"WebP 转换器", WS_OVERLAPPEDWINDOW,
                                  CW_USEDEFAULT, CW_USEDEFAULT, MulDiv(980, dpi, 96), MulDiv(680, dpi, 96),
                                  NULL, NULL, instance, NULL);
    if (!window) { CoUninitialize(); LocalFree(arguments); DeleteObject(app.background); return 2; }
    ShowWindow(window, show);
    UpdateWindow(window);
    for (int index = 1; index < argument_count; ++index) add_path(arguments[index]);
    LocalFree(arguments);
    if (app.count) finish_adding();
    MSG message;
    while (GetMessageW(&message, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(window, &message)) {
            TranslateMessage(&message); DispatchMessageW(&message);
        }
    }
    for (int index = 0; index < app.count; ++index) {
        Job *job = app.jobs[index];
        free(job->source); free(job->directory); free(job->result.path); free(job);
    }
    free(app.jobs); free(app.custom_directory);
    DeleteObject(app.font); DeleteObject(app.title_font); DeleteObject(app.background);
    CoUninitialize();
    return 0;
}
