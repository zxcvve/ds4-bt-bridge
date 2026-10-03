#define COBJMACROS
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include "audio.h"

/* In C the SDK headers only declare these. */
static const CLSID clsid_enumerator = { 0xBCDE0395, 0xE52F, 0x467C, { 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E } };
static const IID iid_enumerator = { 0xA95664D2, 0x9614, 0x4F35, { 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6 } };
static const IID iid_audio_client = { 0x1CB9AD4C, 0xDBFA, 0x4C32, { 0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2 } };
static const IID iid_capture_client = { 0xC8ADBD64, 0xE71E, 0x48A0, { 0xA4, 0xDE, 0x18, 0x5C, 0x39, 0x5C, 0xD3, 0x17 } };
static const PROPERTYKEY pkey_friendly_name = {
    { 0xA45C254E, 0xDF1C, 0x4EFD, { 0x80, 0x20, 0x67, 0xD1, 0x46, 0xA8, 0x50, 0xE0 } }, 14
};

/* First active output device whose name contains name (any case). With list set, prints every name, matches none. */
static IMMDevice *find_device(IMMDeviceEnumerator *en, const char *name, int list)
{
    WCHAR want[128], have[256];
    IMMDeviceCollection *all;
    IMMDevice *found = NULL;
    UINT n = 0;

    MultiByteToWideChar(CP_UTF8, 0, name, -1, want, ARRAYSIZE(want));
    _wcslwr(want);
    if (FAILED(IMMDeviceEnumerator_EnumAudioEndpoints(en, eRender, DEVICE_STATE_ACTIVE, &all)))
        return NULL;
    IMMDeviceCollection_GetCount(all, &n);
    for (UINT i = 0; i < n && !found; i++) {
        IMMDevice *dev;
        IPropertyStore *props;
        PROPVARIANT v;
        if (FAILED(IMMDeviceCollection_Item(all, i, &dev)))
            continue;
        PropVariantInit(&v);
        if (SUCCEEDED(IMMDevice_OpenPropertyStore(dev, STGM_READ, &props))) {
            if (SUCCEEDED(IPropertyStore_GetValue(props, &pkey_friendly_name, &v)) && v.vt == VT_LPWSTR) {
                if (list)
                    fwprintf(stderr, L"  %ls\n", v.pwszVal);
                wcsncpy(have, v.pwszVal, ARRAYSIZE(have) - 1);
                have[ARRAYSIZE(have) - 1] = L'\0';
                if (!list && wcsstr(_wcslwr(have), want)) {
                    wprintf(L"Audio: playing what plays on \"%ls\" on the pad's speaker.\n", v.pwszVal);
                    found = dev;
                }
            }
            PropVariantClear(&v);
            IPropertyStore_Release(props);
        }
        if (!found)
            IMMDevice_Release(dev);
    }
    IMMDeviceCollection_Release(all);
    return found;
}

/* ponytail: returns for good when the device goes away or Windows invalidates the stream (format change in Sound
 * settings); restart the bridge then. Reopen in a loop here if that turns out to happen often. */
void audio_capture(const char *name, int chunk, void (*sink)(const short *pcm))
{
    IMMDeviceEnumerator *en;
    IMMDevice *dev;
    IAudioClient *client;
    IAudioCaptureClient *capture;
    WAVEFORMATEX fmt = { WAVE_FORMAT_PCM, 2, 32000, 32000 * 4, 4, 16, 0 };

    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr) || FAILED(hr = CoCreateInstance(&clsid_enumerator, NULL, CLSCTX_ALL, &iid_enumerator, (void **)&en))) {
        fprintf(stderr, "Audio: no audio device enumerator (0x%08lX).\n", (unsigned long)hr);
        return;
    }
    if (!(dev = find_device(en, name, 0))) {
        fprintf(stderr, "Audio: no active output device named like \"%s\" (config.toml). Output devices:\n", name);
        find_device(en, "", 1);
        return;
    }
    /* Shared-mode loopback; AUTOCONVERTPCM has Windows resample the device's mix to 32 kHz s16. 100 ms buffer. */
    if (FAILED(hr = IMMDevice_Activate(dev, &iid_audio_client, CLSCTX_ALL, NULL, (void **)&client)) ||
        FAILED(hr = IAudioClient_Initialize(client, AUDCLNT_SHAREMODE_SHARED,
                                            AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                            AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY, 1000000, 0, &fmt, NULL)) ||
        FAILED(hr = IAudioClient_GetService(client, &iid_capture_client, (void **)&capture)) ||
        FAILED(hr = IAudioClient_Start(client))) {
        fprintf(stderr, "Audio: can't record the device (0x%08lX).\n", (unsigned long)hr);
        return;
    }

    /* Loopback delivers nothing while the device plays nothing, so the pad just goes quiet then. */
    short *buf = malloc((size_t)chunk * 2 * sizeof *buf);
    int have = 0;
    UINT32 packet;
    for (;;) {
        Sleep(5);
        while (SUCCEEDED(hr = IAudioCaptureClient_GetNextPacketSize(capture, &packet)) && packet) {
            BYTE *data;
            UINT32 frames;
            DWORD flags;
            if (FAILED(hr = IAudioCaptureClient_GetBuffer(capture, &data, &frames, &flags, NULL, NULL)))
                break;
            const short *pcm = (const short *)data;
            for (UINT32 i = 0; i < frames; i++) {
                buf[2 * have] = flags & AUDCLNT_BUFFERFLAGS_SILENT ? 0 : pcm[2 * i];
                buf[2 * have + 1] = flags & AUDCLNT_BUFFERFLAGS_SILENT ? 0 : pcm[2 * i + 1];
                if (++have == chunk) {
                    sink(buf);
                    have = 0;
                }
            }
            IAudioCaptureClient_ReleaseBuffer(capture, frames);
        }
        if (FAILED(hr)) {
            fprintf(stderr, "Audio: recording stopped (0x%08lX); restart the bridge to resume.\n", (unsigned long)hr);
            return;
        }
    }
}
