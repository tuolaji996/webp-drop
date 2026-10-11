#ifndef WEBPDROP_CONVERT_H
#define WEBPDROP_CONVERT_H

#include <windows.h>

typedef enum OutputFormat { OUTPUT_PNG, OUTPUT_JPEG } OutputFormat;

typedef struct ConvertRequest {
    const wchar_t *source;
    const wchar_t *output_directory;
    OutputFormat format;
    int quality;
    volatile LONG *cancel;
} ConvertRequest;

typedef struct ConvertResult {
    wchar_t *path;
    wchar_t message[512];
    int animated;
    int cancelled;
    int width;
    int height;
    int source_identity_known;
    BY_HANDLE_FILE_INFORMATION source_identity;
} ConvertResult;

int convert_webp(const ConvertRequest *request, ConvertResult *result);
int recycle_source(const wchar_t *source, ConvertResult *result, volatile LONG *cancel);
wchar_t *copy_text(const wchar_t *text);
wchar_t *join_path(const wchar_t *directory, const wchar_t *name);
wchar_t *parent_directory(const wchar_t *path);
wchar_t *absolute_path(const wchar_t *path);

#endif
