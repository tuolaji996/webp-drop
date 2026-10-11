#define COBJMACROS
#include <windows.h>
#include <shellapi.h>
#include <wincodec.h>
#include <shlobj.h>
#include <stdio.h>

static LPWSTR *only_program_argument(LPCWSTR command, int *count) {
    LPWSTR *arguments = CommandLineToArgvW(command, count);
    if (arguments) *count = 1;
    return arguments;
}

#define CommandLineToArgvW only_program_argument
#define wWinMain application_main
#include "../src/main.c"
#undef wWinMain
#undef CommandLineToArgvW

static wchar_t *test_input, *test_output;
static int test_failed;

static void log_check(const char *text, int passed) {
    char buffer[500];
    int count = snprintf(buffer, sizeof(buffer), "%s: %s\r\n", passed ? "PASS" : "FAIL", text);
    DWORD written;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), buffer, count, &written, NULL);
    if (!passed) test_failed = 1;
}

static int capture(const wchar_t *name) {
    RECT rectangle;
    GetWindowRect(app.window, &rectangle);
    int width = rectangle.right - rectangle.left;
    int height = rectangle.bottom - rectangle.top;
    HDC screen = GetDC(NULL), memory = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateCompatibleBitmap(screen, width, height);
    HGDIOBJ previous = SelectObject(memory, bitmap);
    BOOL painted = PrintWindow(app.window, memory, 2);
    SelectObject(memory, previous);
    ReleaseDC(NULL, screen); DeleteDC(memory);
    wchar_t *path = join_path(test_output, name);
    IWICImagingFactory *factory = NULL;
    IWICBitmap *image = NULL;
    IWICStream *stream = NULL;
    IWICBitmapEncoder *encoder = NULL;
    IWICBitmapFrameEncode *frame = NULL;
    HRESULT hr = CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                                  &IID_IWICImagingFactory, (void **)&factory);
    if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateBitmapFromHBITMAP(factory, bitmap, NULL, WICBitmapIgnoreAlpha, &image);
    if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateStream(factory, &stream);
    if (SUCCEEDED(hr)) hr = IWICStream_InitializeFromFilename(stream, path, GENERIC_WRITE);
    if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateEncoder(factory, &GUID_ContainerFormatPng, NULL, &encoder);
    if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_Initialize(encoder, (IStream *)stream, WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_CreateNewFrame(encoder, &frame, NULL);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_Initialize(frame, NULL);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_SetSize(frame, width, height);
    GUID format = GUID_WICPixelFormat24bppBGR;
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_SetPixelFormat(frame, &format);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_WriteSource(frame, (IWICBitmapSource *)image, NULL);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_Commit(frame);
    if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_Commit(encoder);
    if (frame) IWICBitmapFrameEncode_Release(frame);
    if (encoder) IWICBitmapEncoder_Release(encoder);
    if (stream) IWICStream_Release(stream);
    if (image) IWICBitmap_Release(image);
    if (factory) IWICImagingFactory_Release(factory);
    DeleteObject(bitmap); free(path);
    return painted && SUCCEEDED(hr);
}

static int drop_folder_files(void) {
    wchar_t *pattern = join_path(test_input, L"*.webp");
    WIN32_FIND_DATAW data;
    HANDLE find = FindFirstFileW(pattern, &data);
    free(pattern);
    if (find == INVALID_HANDLE_VALUE) return 0;
    wchar_t **paths = NULL;
    int count = 0;
    size_t characters = 1;
    do {
        if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            paths = realloc(paths, (count + 1) * sizeof(*paths));
            paths[count] = join_path(test_input, data.cFileName);
            characters += wcslen(paths[count]) + 1;
            ++count;
        }
    } while (FindNextFileW(find, &data));
    FindClose(find);
    HGLOBAL block = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DROPFILES) + characters * sizeof(wchar_t));
    DROPFILES *drop = GlobalLock(block);
    drop->pFiles = sizeof(DROPFILES);
    drop->fWide = TRUE;
    wchar_t *cursor = (wchar_t *)((BYTE *)drop + sizeof(DROPFILES));
    for (int index = 0; index < count; ++index) {
        wcscpy(cursor, paths[index]);
        cursor += wcslen(paths[index]) + 1;
        free(paths[index]);
    }
    free(paths);
    GlobalUnlock(block);
    SendMessageW(app.window, WM_DROPFILES, (WPARAM)block, 0);
    return count;
}

static int wait_idle(void) {
    DWORD started = GetTickCount();
    while (GetTickCount() - started < 60000) {
        SendMessageW(app.window, WM_NULL, 0, 0);
        if (IsWindowEnabled(app.clear)) return 1;
        Sleep(25);
    }
    return 0;
}

static int check_rows(int expected, OutputFormat format, int custom) {
    int valid = ListView_GetItemCount(app.list) == expected;
    int done = 0, failed = 0;
    for (int index = 0; index < app.count; ++index) {
        Job *job = app.jobs[index];
        valid = valid && job->format == format;
        if (job->state == JOB_DONE) {
            ++done;
            wchar_t *parent = parent_directory(job->result.path);
            valid = valid && !_wcsicmp(parent, custom ? app.custom_directory : test_input);
            free(parent);
        } else if (job->state == JOB_FAILED) ++failed;
        else valid = 0;
    }
    return valid && done == expected - 1 && failed == 1;
}

static BOOL CALLBACK check_bounds(HWND window, LPARAM parameter) {
    if (!IsWindowVisible(window)) return TRUE;
    RECT child, client;
    GetWindowRect(window, &child);
    GetClientRect(app.window, &client);
    MapWindowPoints(app.window, NULL, (POINT *)&client, 2);
    if (child.left < client.left || child.top < client.top ||
        child.right > client.right || child.bottom > client.bottom)
        *(int *)parameter = 0;
    return TRUE;
}

static DWORD WINAPI test_driver(void *parameter) {
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    DWORD started = GetTickCount();
    while ((!app.window || !app.list) && GetTickCount() - started < 15000) Sleep(10);
    if (!app.window || !app.list) { log_check("GUI creation", 0); CoUninitialize(); return 1; }
    SendMessageW(app.window, WM_NULL, 0, 0);
    log_check("GUI defaults to PNG, source folder, and disabled JPEG quality",
        SendMessageW(app.format, CB_GETCURSEL, 0, 0) == 0 &&
        SendMessageW(app.source_radio, BM_GETCHECK, 0, 0) == BST_CHECKED &&
        !IsWindowEnabled(app.quality));
    log_check("delete-original checkbox is unchecked by default",
              SendMessageW(app.delete_original, BM_GETCHECK, 0, 0) == BST_UNCHECKED);
    log_check("default window screenshot", capture(L"ui-default.png"));
    int count = drop_folder_files();
    log_check("WM_DROPFILES batch completes and contains one deliberate damaged-image failure",
              count > 1 && wait_idle() && check_rows(count, OUTPUT_PNG, 0));
    log_check("completed PNG window screenshot", capture(L"ui-png-complete.png"));
    SetWindowPos(app.window, NULL, 0, 0, px(800), px(540), SWP_NOMOVE | SWP_NOZORDER);
    int bounded = 1;
    EnumChildWindows(app.window, check_bounds, (LPARAM)&bounded);
    log_check("all controls remain inside the minimum-size window", bounded);
    log_check("compact window screenshot", capture(L"ui-compact.png"));
    SetWindowPos(app.window, NULL, 0, 0, px(980), px(680), SWP_NOMOVE | SWP_NOZORDER);

    SendMessageW(app.window, WM_COMMAND, ID_CLEAR, 0);
    drop_folder_files();
    SendMessageW(app.window, WM_COMMAND, ID_STOP, 0);
    int idle = wait_idle(), stopped = 0;
    for (int index = 0; index < app.count; ++index) if (app.jobs[index]->state == JOB_STOPPED) ++stopped;
    log_check("stop cancels queued work and returns the GUI to an idle state", idle && stopped > 0);
    SendMessageW(app.window, WM_COMMAND, ID_RETRY, 0);
    log_check("retry completes the stopped batch and isolates the damaged input", wait_idle() && check_rows(count, OUTPUT_PNG, 0));

    SendMessageW(app.window, WM_COMMAND, ID_CLEAR, 0);
    SendMessageW(app.format, CB_SETCURSEL, 1, 0);
    SendMessageW(app.window, WM_COMMAND, MAKEWPARAM(ID_FORMAT, CBN_SELCHANGE), (LPARAM)app.format);
    log_check("selecting JPEG enables its quality control", IsWindowEnabled(app.quality));
    app.custom_directory = join_path(test_output, L"gui-jpeg-output");
    CreateDirectoryW(app.custom_directory, NULL);
    SendMessageW(app.window, WM_COMMAND, ID_CUSTOM, 0);
    count = drop_folder_files();
    log_check("JPEG GUI batch uses the selected output folder", wait_idle() && check_rows(count, OUTPUT_JPEG, 1));
    log_check("JPEG window screenshot", capture(L"ui-jpeg-custom.png"));

    SendMessageW(app.window, WM_COMMAND, ID_CLEAR, 0);
    SendMessageW(app.format, CB_SETCURSEL, 0, 0);
    SendMessageW(app.window, WM_COMMAND, MAKEWPARAM(ID_FORMAT, CBN_SELCHANGE), (LPARAM)app.format);
    SendMessageW(app.window, WM_COMMAND, ID_SOURCE, 0);
    wchar_t *original_input = test_input;
    test_input = join_path(test_output, L"gui-delete-input");
    SendMessageW(app.delete_original, BM_SETCHECK, BST_CHECKED, 0);
    count = drop_folder_files();
    SendMessageW(app.window, WM_COMMAND, ID_STOP, 0);
    idle = wait_idle();
    int retained = idle;
    for (int index = 0; index < app.count; ++index)
        if (app.jobs[index]->state != JOB_DONE || !app.jobs[index]->source_deleted)
            retained = retained && GetFileAttributesW(app.jobs[index]->source) != INVALID_FILE_ATTRIBUTES;
    log_check("stopped or unsuccessful deletion-enabled jobs retain their original WebP", retained);
    SendMessageW(app.delete_original, BM_SETCHECK, BST_UNCHECKED, 0);
    SendMessageW(app.window, WM_COMMAND, ID_RETRY, 0);
    retained = wait_idle() && check_rows(count, OUTPUT_PNG, 0);
    for (int index = 0; index < app.count; ++index)
        retained = retained && !app.jobs[index]->delete_original &&
                   GetFileAttributesW(app.jobs[index]->source) != INVALID_FILE_ATTRIBUTES;
    log_check("unchecking deletion before retry keeps all originals", retained);

    SendMessageW(app.window, WM_COMMAND, ID_CLEAR, 0);
    SendMessageW(app.delete_original, BM_SETCHECK, BST_CHECKED, 0);
    count = drop_folder_files();
    int deleted = wait_idle() && check_rows(count, OUTPUT_PNG, 0);
    for (int index = 0; index < app.count; ++index) {
        Job *job = app.jobs[index];
        DWORD attributes = GetFileAttributesW(job->source);
        if (job->state == JOB_DONE)
            deleted = deleted && job->source_deleted && attributes == INVALID_FILE_ATTRIBUTES &&
                      GetFileAttributesW(job->result.path) != INVALID_FILE_ATTRIBUTES;
        else deleted = deleted && !job->source_deleted && attributes != INVALID_FILE_ATTRIBUTES;
    }
    log_check("checked deletion recycles only successfully converted WebP and preserves failed inputs", deleted);
    log_check("deletion-enabled screenshot", capture(L"ui-delete-complete.png"));

    SendMessageW(app.window, WM_COMMAND, ID_CLEAR, 0);
    free(test_input);
    test_input = join_path(test_output, L"gui-delete-blocked");
    wchar_t *locked_path = join_path(test_input, L"locked.webp");
    HANDLE locked_file = CreateFileW(locked_path, GENERIC_READ, FILE_SHARE_READ, NULL,
                                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    count = drop_folder_files();
    int protected = locked_file != INVALID_HANDLE_VALUE && wait_idle() && count == 1 && app.count == 1 &&
                    app.jobs[0]->state == JOB_DONE && !app.jobs[0]->source_deleted &&
                    app.jobs[0]->result.message[0] &&
                    GetFileAttributesW(locked_path) != INVALID_FILE_ATTRIBUTES &&
                    GetFileAttributesW(app.jobs[0]->result.path) != INVALID_FILE_ATTRIBUTES;
    log_check("a locked original is retained, with valid converted output and a deletion warning", protected);
    if (locked_file != INVALID_HANDLE_VALUE) CloseHandle(locked_file);
    free(locked_path);

    wchar_t *changed_path = join_path(test_output, L"identity-change.webp");
    ConvertRequest identity_request = { changed_path, NULL, OUTPUT_PNG, 92, NULL };
    ConvertResult identity_result;
    int encoded = convert_webp(&identity_request, &identity_result);
    HANDLE changed_file = CreateFileW(changed_path, FILE_WRITE_ATTRIBUTES, 0, NULL,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    int changed = 0;
    if (changed_file != INVALID_HANDLE_VALUE && encoded) {
        ULARGE_INTEGER timestamp;
        timestamp.LowPart = identity_result.source_identity.ftLastWriteTime.dwLowDateTime;
        timestamp.HighPart = identity_result.source_identity.ftLastWriteTime.dwHighDateTime;
        timestamp.QuadPart += 10000000;
        FILETIME modified = { timestamp.LowPart, timestamp.HighPart };
        changed = SetFileTime(changed_file, NULL, NULL, &modified);
    }
    if (changed_file != INVALID_HANDLE_VALUE) CloseHandle(changed_file);
    int recycled = encoded && changed ? recycle_source(changed_path, &identity_result, NULL) : 1;
    log_check("an original modified after conversion is retained",
        encoded && changed && !recycled &&
        wcsstr(identity_result.message, L"原文件已变化") &&
        GetFileAttributesW(changed_path) != INVALID_FILE_ATTRIBUTES &&
        GetFileAttributesW(identity_result.path) != INVALID_FILE_ATTRIBUTES);
    free(changed_path); free(identity_result.path);
    free(test_input); test_input = original_input;
    SendMessageW(app.window, WM_CLOSE, 0, 0);
    CoUninitialize();
    return 0;
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR command, int show) {
    int count;
    wchar_t **arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (count != 3) { LocalFree(arguments); return 2; }
    test_input = absolute_path(arguments[1]);
    test_output = absolute_path(arguments[2]);
    LocalFree(arguments);
    HANDLE driver = CreateThread(NULL, 0, test_driver, NULL, 0, NULL);
    int result = application_main(instance, previous, command, SW_SHOWNORMAL);
    WaitForSingleObject(driver, 20000);
    CloseHandle(driver);
    free(test_input); free(test_output);
    return test_failed ? 1 : result;
}
