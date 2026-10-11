#define COBJMACROS
#include "convert.h"
#include <wincodec.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include "src/webp/decode.h"
#include "src/webp/demux.h"

wchar_t *copy_text(const wchar_t *text) {
    size_t bytes = (wcslen(text) + 1) * sizeof(wchar_t);
    wchar_t *copy = malloc(bytes);
    if (copy) memcpy(copy, text, bytes);
    return copy;
}

wchar_t *join_path(const wchar_t *directory, const wchar_t *name) {
    size_t length = wcslen(directory);
    int slash = length && directory[length - 1] != L'\\' && directory[length - 1] != L'/';
    size_t count = length + wcslen(name) + slash + 1;
    wchar_t *path = malloc(count * sizeof(wchar_t));
    if (path) swprintf(path, count, L"%ls%ls%ls", directory, slash ? L"\\" : L"", name);
    return path;
}

wchar_t *parent_directory(const wchar_t *path) {
    wchar_t *copy = copy_text(path);
    if (!copy) return NULL;
    wchar_t *slash = wcsrchr(copy, L'\\');
    if (!slash) slash = wcsrchr(copy, L'/');
    if (!slash) { free(copy); return copy_text(L"."); }
    if (slash == copy + 2 && copy[1] == L':') slash[1] = L'\0';
    else *slash = L'\0';
    return copy;
}

wchar_t *absolute_path(const wchar_t *path) {
    DWORD count = GetFullPathNameW(path, 0, NULL, NULL);
    if (!count) return NULL;
    wchar_t *full = calloc(count + 1, sizeof(wchar_t));
    if (full && !GetFullPathNameW(path, count + 1, full, NULL)) { free(full); full = NULL; }
    return full;
}

static wchar_t *file_api_path(const wchar_t *path) {
    if (wcsncmp(path, L"\\\\?\\", 4) == 0) return copy_text(path);
    size_t count = wcslen(path) + 9;
    wchar_t *extended = malloc(count * sizeof(wchar_t));
    if (extended) {
        if (wcsncmp(path, L"\\\\", 2) == 0)
            swprintf(extended, count, L"\\\\?\\UNC\\%ls", path + 2);
        else swprintf(extended, count, L"\\\\?\\%ls", path);
    }
    return extended;
}

static int cancelled(const ConvertRequest *request) {
    return request->cancel && InterlockedCompareExchange(request->cancel, 0, 0);
}

static void windows_error(ConvertResult *result, const wchar_t *operation, DWORD error) {
    wchar_t description[256] = L"";
    FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   NULL, error, 0, description, 256, NULL);
    size_t end = wcslen(description);
    while (end && (description[end - 1] == L'\r' || description[end - 1] == L'\n'))
        description[--end] = L'\0';
    swprintf(result->message, 512, L"%ls：%ls (0x%08lX)", operation, description, error);
}

static int read_source(const wchar_t *path, uint8_t **bytes, size_t *length,
                       const ConvertRequest *request, ConvertResult *result) {
    wchar_t *extended = file_api_path(path);
    if (!extended) { wcscpy(result->message, L"内存不足"); return 0; }
    HANDLE file = CreateFileW(extended, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                             NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    free(extended);
    if (file == INVALID_HANDLE_VALUE) { windows_error(result, L"无法读取图片", GetLastError()); return 0; }
    LARGE_INTEGER size;
    if (!GetFileSizeEx(file, &size)) {
        windows_error(result, L"无法读取文件大小", GetLastError()); CloseHandle(file); return 0;
    }
    if (size.QuadPart <= 0 || size.QuadPart > 256LL * 1024 * 1024) {
        wcscpy(result->message, L"文件为空或超过 256 MB"); CloseHandle(file); return 0;
    }
    *length = (size_t)size.QuadPart;
    *bytes = malloc(*length);
    if (!*bytes) { wcscpy(result->message, L"内存不足"); CloseHandle(file); return 0; }
    size_t offset = 0;
    while (offset < *length && !cancelled(request)) {
        DWORD read = 0;
        DWORD chunk = (DWORD)((*length - offset) > 1024 * 1024 ? 1024 * 1024 : *length - offset);
        BOOL read_ok = ReadFile(file, *bytes + offset, chunk, &read, NULL);
        if (!read_ok || !read) {
            windows_error(result, L"图片读取失败", read_ok ? ERROR_HANDLE_EOF : GetLastError());
            CloseHandle(file); return 0;
        }
        offset += read;
    }
    if (offset == *length)
        result->source_identity_known = GetFileInformationByHandle(file, &result->source_identity);
    CloseHandle(file);
    return offset == *length;
}

int recycle_source(const wchar_t *source, ConvertResult *result, volatile LONG *cancel) {
    if (!result->path || !result->source_identity_known) {
        wcscpy(result->message, L"图片已保存，无法确认原文件身份，原 WebP 已保留");
        return 0;
    }
    wchar_t *extended = file_api_path(source);
    if (!extended) { wcscpy(result->message, L"图片已保存，内存不足，原 WebP 已保留"); return 0; }
    HANDLE file = CreateFileW(extended, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                             NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        windows_error(result, L"图片已保存，无法删除原 WebP", GetLastError());
        free(extended); return 0;
    }
    BY_HANDLE_FILE_INFORMATION current;
    BOOL read = GetFileInformationByHandle(file, &current);
    const BY_HANDLE_FILE_INFORMATION *original = &result->source_identity;
    if (!read || current.dwVolumeSerialNumber != original->dwVolumeSerialNumber ||
        current.nFileIndexHigh != original->nFileIndexHigh || current.nFileIndexLow != original->nFileIndexLow ||
        current.nFileSizeHigh != original->nFileSizeHigh || current.nFileSizeLow != original->nFileSizeLow ||
        CompareFileTime(&current.ftLastWriteTime, &original->ftLastWriteTime)) {
        wcscpy(result->message, L"图片已保存，原文件已变化或无法验证，原 WebP 已保留");
        CloseHandle(file); free(extended); return 0;
    }
    IFileOperation *operation = NULL;
    IShellItem *item = NULL;
    HRESULT initialized = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    HRESULT hr = initialized;
    BOOL aborted = FALSE;
    if (SUCCEEDED(hr))
        hr = CoCreateInstance(&CLSID_FileOperation, NULL, CLSCTX_INPROC_SERVER,
                              &IID_IFileOperation, (void **)&operation);
    if (SUCCEEDED(hr))
        hr = IFileOperation_SetOperationFlags(operation, FOF_ALLOWUNDO | FOF_NOCONFIRMATION |
                                              FOF_NOERRORUI | FOF_SILENT | FOFX_RECYCLEONDELETE);
    if (SUCCEEDED(hr)) hr = SHCreateItemFromParsingName(source, NULL, &IID_IShellItem, (void **)&item);
    if (SUCCEEDED(hr)) hr = IFileOperation_DeleteItem(operation, item, NULL);
    if (SUCCEEDED(hr)) {
        if (cancel && InterlockedCompareExchange(cancel, 0, 0)) hr = HRESULT_FROM_WIN32(ERROR_CANCELLED);
        else {
            hr = IFileOperation_PerformOperations(operation);
            HRESULT checked = IFileOperation_GetAnyOperationsAborted(operation, &aborted);
            if (SUCCEEDED(hr) && FAILED(checked)) hr = checked;
            if (SUCCEEDED(hr) && aborted) hr = HRESULT_FROM_WIN32(ERROR_CANCELLED);
        }
    }
    if (item) IShellItem_Release(item);
    if (operation) IFileOperation_Release(operation);
    if (SUCCEEDED(initialized)) CoUninitialize();
    CloseHandle(file);
    int deleted = 0;
    if (SUCCEEDED(hr)) {
        DWORD attributes = GetFileAttributesW(extended);
        DWORD error = GetLastError();
        deleted = attributes == INVALID_FILE_ATTRIBUTES &&
                  (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND);
        if (!deleted) hr = HRESULT_FROM_WIN32(attributes == INVALID_FILE_ATTRIBUTES ? error : ERROR_ACCESS_DENIED);
    }
    if (!deleted) windows_error(result, L"图片已保存，原 WebP 未删除", (DWORD)hr);
    free(extended);
    return deleted;
}

static wchar_t *create_temporary(const wchar_t *directory, ConvertResult *result) {
    static volatile LONG serial = 0;
    for (int attempt = 0; attempt < 1000; ++attempt) {
        wchar_t name[100];
        swprintf(name, 100, L".webpdrop-%lu-%lu.tmp", GetCurrentProcessId(),
                 (DWORD)InterlockedIncrement(&serial));
        wchar_t *path = join_path(directory, name);
        wchar_t *extended = path ? file_api_path(path) : NULL;
        if (!extended) { free(path); wcscpy(result->message, L"内存不足"); return NULL; }
        HANDLE file = CreateFileW(extended, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                                 FILE_ATTRIBUTE_NORMAL, NULL);
        DWORD error = GetLastError();
        free(extended);
        if (file != INVALID_HANDLE_VALUE) { CloseHandle(file); return path; }
        free(path);
        if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS) {
            windows_error(result, L"无法写入输出目录", error); return NULL;
        }
    }
    wcscpy(result->message, L"无法创建临时文件");
    return NULL;
}

static int publish_image(const ConvertRequest *request, const wchar_t *directory,
                         const wchar_t *temporary, ConvertResult *result) {
    const wchar_t *base = wcsrchr(request->source, L'\\');
    base = base ? base + 1 : request->source;
    const wchar_t *dot = wcsrchr(base, L'.');
    size_t stem_length = dot ? (size_t)(dot - base) : wcslen(base);
    wchar_t *stem = calloc(stem_length + 1, sizeof(wchar_t));
    wchar_t *name = malloc((stem_length + 40) * sizeof(wchar_t));
    wchar_t *from = file_api_path(temporary);
    if (!stem || !name || !from) {
        free(stem); free(name); free(from); wcscpy(result->message, L"内存不足"); return 0;
    }
    memcpy(stem, base, stem_length * sizeof(wchar_t));
    const wchar_t *extension = request->format == OUTPUT_PNG ? L"png" : L"jpg";
    int success = 0;
    for (unsigned number = 0; number < 100000 && !cancelled(request); ++number) {
        if (number) swprintf(name, stem_length + 40, L"%ls (%u).%ls", stem, number + 1, extension);
        else swprintf(name, stem_length + 40, L"%ls.%ls", stem, extension);
        wchar_t *destination = join_path(directory, name);
        wchar_t *to = destination ? file_api_path(destination) : NULL;
        if (!to) { free(destination); wcscpy(result->message, L"内存不足"); break; }
        if (MoveFileW(from, to)) { result->path = destination; success = 1; free(to); break; }
        DWORD error = GetLastError();
        free(to); free(destination);
        if (error != ERROR_ALREADY_EXISTS && error != ERROR_FILE_EXISTS) {
            windows_error(result, L"无法保存图片", error); break;
        }
    }
    if (!success && !result->message[0]) wcscpy(result->message, L"同名文件过多，无法保存");
    free(stem); free(name); free(from);
    return success;
}

int convert_webp(const ConvertRequest *request, ConvertResult *result) {
    memset(result, 0, sizeof(*result));
    uint8_t *source = NULL, *pixels = NULL, *rgb = NULL;
    size_t source_size = 0;
    int owns_pixels = 0, success = 0;
    wchar_t *directory = NULL, *temporary = NULL, *extended = NULL;
    IWICImagingFactory *factory = NULL;
    IWICStream *stream = NULL;
    IWICBitmapEncoder *encoder = NULL;
    IWICBitmapFrameEncode *frame = NULL;
    IPropertyBag2 *options = NULL;
    IWICColorContext *color = NULL;
    WebPAnimDecoder *animation = NULL;
    WebPDemuxer *demux = NULL;
    HRESULT initialized = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    HRESULT hr = S_OK;
    const wchar_t *operation = L"图片编码失败";
    if (FAILED(initialized)) { hr = initialized; goto cleanup; }
    if (cancelled(request)) goto cleanup;
    if (!read_source(request->source, &source, &source_size, request, result)) goto cleanup;
    WebPBitstreamFeatures features;
    if (WebPGetFeatures(source, source_size, &features) != VP8_STATUS_OK) {
        wcscpy(result->message, L"不是有效的 WebP 图片，或文件已损坏"); goto cleanup;
    }
    if (features.width <= 0 || features.height <= 0 ||
        (uint64_t)features.width * features.height * 4 > 512ULL * 1024 * 1024) {
        wcscpy(result->message, L"图片尺寸过大，解码需要超过 512 MB"); goto cleanup;
    }
    result->width = features.width;
    result->height = features.height;
    result->animated = features.has_animation;
    WebPData data = { source, source_size };
    if (features.has_animation) {
        WebPAnimDecoderOptions settings;
        if (!WebPAnimDecoderOptionsInit(&settings)) { wcscpy(result->message, L"解码器初始化失败"); goto cleanup; }
        settings.color_mode = MODE_BGRA;
        settings.use_threads = 1;
        animation = WebPAnimDecoderNew(&data, &settings);
        int timestamp = 0;
        if (!animation || !WebPAnimDecoderGetNext(animation, &pixels, &timestamp)) {
            wcscpy(result->message, L"动画 WebP 解码失败"); goto cleanup;
        }
    } else {
        pixels = WebPDecodeBGRA(source, source_size, &result->width, &result->height);
        owns_pixels = 1;
        if (!pixels) { wcscpy(result->message, L"WebP 解码失败，文件可能已损坏"); goto cleanup; }
    }
    if (cancelled(request)) goto cleanup;
    directory = request->output_directory && request->output_directory[0]
        ? copy_text(request->output_directory) : parent_directory(request->source);
    if (!directory) { wcscpy(result->message, L"内存不足"); goto cleanup; }
    temporary = create_temporary(directory, result);
    if (!temporary) goto cleanup;
    extended = file_api_path(temporary);
    if (!extended) { wcscpy(result->message, L"内存不足"); goto cleanup; }
    hr = CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                          &IID_IWICImagingFactory, (void **)&factory);
    if (FAILED(hr)) goto cleanup;
    hr = IWICImagingFactory_CreateStream(factory, &stream);
    if (FAILED(hr)) goto cleanup;
    hr = IWICStream_InitializeFromFilename(stream, extended, GENERIC_WRITE);
    if (FAILED(hr)) goto cleanup;
    const GUID *container = request->format == OUTPUT_PNG ? &GUID_ContainerFormatPng : &GUID_ContainerFormatJpeg;
    hr = IWICImagingFactory_CreateEncoder(factory, container, NULL, &encoder);
    if (FAILED(hr)) goto cleanup;
    hr = IWICBitmapEncoder_Initialize(encoder, (IStream *)stream, WICBitmapEncoderNoCache);
    if (FAILED(hr)) goto cleanup;
    hr = IWICBitmapEncoder_CreateNewFrame(encoder, &frame, &options);
    if (FAILED(hr)) goto cleanup;
    if (request->format == OUTPUT_JPEG) {
        PROPBAG2 property = {0};
        property.pstrName = L"ImageQuality";
        VARIANT value;
        VariantInit(&value);
        value.vt = VT_R4;
        int quality = request->quality < 1 ? 1 : (request->quality > 100 ? 100 : request->quality);
        value.fltVal = quality / 100.0f;
        hr = IPropertyBag2_Write(options, 1, &property, &value);
        if (FAILED(hr)) goto cleanup;
        property.pstrName = L"JpegYCrCbSubsampling";
        value.vt = VT_UI1;
        value.bVal = WICJpegYCrCbSubsampling444;
        hr = IPropertyBag2_Write(options, 1, &property, &value);
        if (FAILED(hr)) goto cleanup;
    }
    hr = IWICBitmapFrameEncode_Initialize(frame, options);
    if (FAILED(hr)) goto cleanup;
    hr = IWICBitmapFrameEncode_SetSize(frame, result->width, result->height);
    if (FAILED(hr)) goto cleanup;
    GUID format = request->format == OUTPUT_PNG ? GUID_WICPixelFormat32bppBGRA : GUID_WICPixelFormat24bppBGR;
    GUID requested_format = format;
    hr = IWICBitmapFrameEncode_SetPixelFormat(frame, &format);
    if (FAILED(hr)) goto cleanup;
    if (!IsEqualGUID(&format, &requested_format)) { hr = WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT; goto cleanup; }
    demux = WebPDemux(&data);
    WebPChunkIterator profile;
    if (demux && WebPDemuxGetChunk(demux, "ICCP", 1, &profile)) {
        if (profile.chunk.size <= UINT32_MAX &&
            SUCCEEDED(IWICImagingFactory_CreateColorContext(factory, &color)) &&
            SUCCEEDED(IWICColorContext_InitializeFromMemory(color, profile.chunk.bytes, (UINT)profile.chunk.size))) {
            IWICBitmapFrameEncode_SetColorContexts(frame, 1, &color);
        }
        WebPDemuxReleaseChunkIterator(&profile);
    }
    const UINT rows_per_chunk = 32;
    UINT stride = result->width * (request->format == OUTPUT_PNG ? 4 : 3);
    if (request->format == OUTPUT_JPEG) {
        rgb = malloc((size_t)stride * rows_per_chunk);
        if (!rgb) { wcscpy(result->message, L"内存不足"); goto cleanup; }
    }
    for (UINT row = 0; row < (UINT)result->height; row += rows_per_chunk) {
        if (cancelled(request)) goto cleanup;
        UINT rows = (UINT)result->height - row;
        if (rows > rows_per_chunk) rows = rows_per_chunk;
        uint8_t *block = pixels + (size_t)row * result->width * 4;
        if (rgb) {
            size_t count = (size_t)rows * result->width;
            for (size_t index = 0; index < count; ++index) {
                unsigned alpha = block[index * 4 + 3];
                for (int channel = 0; channel < 3; ++channel)
                    rgb[index * 3 + channel] = (uint8_t)((block[index * 4 + channel] * alpha +
                                             255 * (255 - alpha) + 127) / 255);
            }
            block = rgb;
        }
        hr = IWICBitmapFrameEncode_WritePixels(frame, rows, stride, stride * rows, block);
        if (FAILED(hr)) goto cleanup;
    }
    if (cancelled(request)) goto cleanup;
    hr = IWICBitmapFrameEncode_Commit(frame);
    if (FAILED(hr)) goto cleanup;
    hr = IWICBitmapEncoder_Commit(encoder);
    if (FAILED(hr)) goto cleanup;
    IWICBitmapFrameEncode_Release(frame); frame = NULL;
    IWICBitmapEncoder_Release(encoder); encoder = NULL;
    IWICStream_Release(stream); stream = NULL;
    success = publish_image(request, directory, temporary, result);

cleanup:
    if (color) IWICColorContext_Release(color);
    if (options) IPropertyBag2_Release(options);
    if (frame) IWICBitmapFrameEncode_Release(frame);
    if (encoder) IWICBitmapEncoder_Release(encoder);
    if (stream) IWICStream_Release(stream);
    if (factory) IWICImagingFactory_Release(factory);
    if (demux) WebPDemuxDelete(demux);
    if (owns_pixels) WebPFree(pixels);
    if (animation) WebPAnimDecoderDelete(animation);
    free(source); free(rgb); free(extended); free(directory);
    if (!success && temporary) {
        wchar_t *path = file_api_path(temporary);
        if (path) { DeleteFileW(path); free(path); }
    }
    free(temporary);
    if (!success && cancelled(request)) {
        result->cancelled = 1; wcscpy(result->message, L"已停止");
    } else if (!success && FAILED(hr)) {
        windows_error(result, operation, (DWORD)hr);
    }
    if (SUCCEEDED(initialized)) CoUninitialize();
    return success;
}
