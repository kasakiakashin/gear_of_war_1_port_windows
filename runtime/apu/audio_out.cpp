// audio_out.cpp — saída de som do host por WASAPI (modo partilhado, estéreo float 32 a 48 kHz).
//
// O driver de render (audio.cpp) entrega quadros de 256 amostras × 6 canais (float BE, planar: canal × 256, ordem
// FL FR C LFE BL BR). Aqui convertem-se para little-endian, faz-se o downmix para estéreo e põem-se numa fila;
// uma thread do host enche o buffer do WASAPI a cada evento do dispositivo. O ritmo do jogo (quando chamar a
// callback do XAudio) segue o enchimento desta fila, ou seja, o relógio real da placa de som.
//
// COM/WASAPI carregados em runtime (ole32.dll), como as DLLs do D3D12. GOW_AUDIO=0 desliga a saída (o driver volta a
// um temporizador); GOW_AUDIO_DUMP=ficheiro.wav grava também tudo o que é tocado.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#include "apu/apu.h"
#include "config.h"

namespace {

constexpr GUID kClsidMMDeviceEnumerator = {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
constexpr GUID kIidIMMDeviceEnumerator = {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
constexpr GUID kIidIAudioClient = {0x1CB9AD4C, 0xDBFA, 0x4C32, {0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2}};
constexpr GUID kIidIAudioRenderClient = {0xF294ACFC, 0x3146, 0x4483, {0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2}};
constexpr GUID kSubtypeFloat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};

constexpr uint32_t kRate = 48000, kChannels = 2;
constexpr size_t kRingFrames = kRate / 2;  // 0,5 s de capacidade (amostras estéreo)
constexpr size_t kMaxQueued = kRate * 60 / 1000;  // acima disto o som deixaria de acompanhar a imagem: descarta o mais antigo

std::mutex g_mu;
std::vector<float> g_ring(kRingFrames * kChannels);
size_t g_read = 0, g_count = 0;  // em amostras estéreo
std::atomic<bool> g_active{false};
std::atomic<uint64_t> g_underruns{0};
bool g_refilling = false;  // depois de esvaziar, só volta a tocar com folga: evita um crepitar de pequenos cortes
constexpr size_t kResumeFrames = 256 * 3;
FILE* g_dump = nullptr;
uint32_t g_dump_bytes = 0;

void write_wav_header(FILE* f, uint32_t data_bytes) {
  auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
  auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
  std::fseek(f, 0, SEEK_SET);
  std::fwrite("RIFF", 1, 4, f);
  u32(36 + data_bytes);
  std::fwrite("WAVEfmt ", 1, 8, f);
  u32(16);
  u16(3);  // IEEE float
  u16(kChannels);
  u32(kRate);
  u32(kRate * kChannels * 4);
  u16(kChannels * 4);
  u16(32);
  std::fwrite("data", 1, 4, f);
  u32(data_bytes);
  std::fseek(f, 0, SEEK_END);
}

size_t pop(float* out, size_t frames) {
  std::lock_guard<std::mutex> lock(g_mu);
  if (g_refilling) {
    if (g_count < kResumeFrames) return 0;
    g_refilling = false;
  }
  size_t n = frames < g_count ? frames : g_count;
  if (n < frames) g_refilling = true;
  for (size_t i = 0; i < n; i++) {
    size_t r = (g_read + i) % kRingFrames;
    out[2 * i] = g_ring[2 * r];
    out[2 * i + 1] = g_ring[2 * r + 1];
  }
  g_read = (g_read + n) % kRingFrames;
  g_count -= n;
  return n;
}

DWORD WINAPI wasapi_thread(LPVOID) {
  // MMCSS "Pro Audio": o agendador do Windows dá a esta thread fatias garantidas mesmo com todos os núcleos ocupados.
  if (HMODULE avrt = LoadLibraryA("avrt.dll")) {
    using SetMm = HANDLE(WINAPI*)(LPCWSTR, LPDWORD);
    if (auto set_mm = (SetMm)GetProcAddress(avrt, "AvSetMmThreadCharacteristicsW")) {
      DWORD task = 0;
      set_mm(L"Pro Audio", &task);
    }
  }
  HMODULE ole = LoadLibraryA("ole32.dll");
  auto co_init = ole ? (HRESULT(WINAPI*)(LPVOID, DWORD))GetProcAddress(ole, "CoInitializeEx") : nullptr;
  auto co_create = ole ? (HRESULT(WINAPI*)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID*))GetProcAddress(ole, "CoCreateInstance")
                       : nullptr;
  auto co_free = ole ? (void(WINAPI*)(LPVOID))GetProcAddress(ole, "CoTaskMemFree") : nullptr;
  if (!co_init || !co_create) {
    std::fprintf(stderr, "[audio] ole32.dll indisponível: sem saída de som\n");
    return 0;
  }
  co_init(nullptr, COINIT_MULTITHREADED);
  IMMDeviceEnumerator* en = nullptr;
  IMMDevice* dev = nullptr;
  IAudioClient* client = nullptr;
  IAudioRenderClient* render = nullptr;
  HRESULT hr = co_create(kClsidMMDeviceEnumerator, nullptr, CLSCTX_ALL, kIidIMMDeviceEnumerator, (void**)&en);
  if (SUCCEEDED(hr)) hr = en->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
  if (SUCCEEDED(hr)) hr = dev->Activate(kIidIAudioClient, CLSCTX_ALL, nullptr, (void**)&client);
  WAVEFORMATEXTENSIBLE fmt{};
  fmt.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
  fmt.Format.nChannels = kChannels;
  fmt.Format.nSamplesPerSec = kRate;
  fmt.Format.wBitsPerSample = 32;
  fmt.Format.nBlockAlign = kChannels * 4;
  fmt.Format.nAvgBytesPerSec = kRate * kChannels * 4;
  fmt.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
  fmt.Samples.wValidBitsPerSample = 32;
  fmt.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
  fmt.SubFormat = kSubtypeFloat;
  HANDLE ev = CreateEventA(nullptr, FALSE, FALSE, nullptr);
  const REFERENCE_TIME buffer_hns = 200000;  // 20 ms
  if (SUCCEEDED(hr))
    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                            AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                            buffer_hns, 0, &fmt.Format, nullptr);
  UINT32 buffer_frames = 0;
  if (SUCCEEDED(hr)) hr = client->SetEventHandle(ev);
  if (SUCCEEDED(hr)) hr = client->GetBufferSize(&buffer_frames);
  if (SUCCEEDED(hr)) hr = client->GetService(kIidIAudioRenderClient, (void**)&render);
  if (SUCCEEDED(hr)) hr = client->Start();
  if (FAILED(hr)) {
    std::fprintf(stderr, "[audio] WASAPI falhou (0x%08lX): sem saída de som\n", (unsigned long)hr);
    return 0;
  }
  (void)co_free;
  std::fprintf(stderr, "[audio] WASAPI: estéreo float %u Hz, buffer de %u amostras\n", kRate, buffer_frames);
  g_active = true;
  std::vector<float> tmp;
  for (;;) {
    WaitForSingleObject(ev, 100);
    UINT32 padding = 0;
    if (FAILED(client->GetCurrentPadding(&padding))) continue;
    UINT32 avail = buffer_frames - padding;
    if (!avail) continue;
    BYTE* data = nullptr;
    if (FAILED(render->GetBuffer(avail, &data))) continue;
    tmp.resize(avail * kChannels);
    size_t got = pop(tmp.data(), avail);
    if (got < avail) {
      std::memset(tmp.data() + got * kChannels, 0, (avail - got) * kChannels * sizeof(float));
      g_underruns.fetch_add(1, std::memory_order_relaxed);
    }
    std::memcpy(data, tmp.data(), avail * kChannels * sizeof(float));
    render->ReleaseBuffer(avail, 0);
  }
}

}  // namespace

namespace apu {

void output_init() {
  if (const char* e = std::getenv("GOW_AUDIO"); e && e[0] == '0') return;
  if (const char* d = std::getenv("GOW_AUDIO_DUMP")) {
    g_dump = std::fopen(d, "wb");
    if (g_dump) write_wav_header(g_dump, 0);
  }
  HANDLE h = CreateThread(nullptr, 1u << 20, wasapi_thread, nullptr, 0, nullptr);
  if (h) {
    SetThreadPriority(h, THREAD_PRIORITY_TIME_CRITICAL);
    CloseHandle(h);
  }
  // Espera curta para o dispositivo arrancar (o driver do XAudio decide o ritmo por output_active()).
  for (int i = 0; i < 50 && !g_active.load(); i++) Sleep(10);
}

bool output_active() { return g_active.load(); }

size_t output_queued() {
  std::lock_guard<std::mutex> lock(g_mu);
  return g_count;
}

// Quadro do XAudio: 256 amostras × 6 canais planar, float big-endian.
void output_submit_frame(const uint8_t* be_planar6) {
  constexpr int n = 256;
  float stereo[n * 2];
  auto rd = [&](int ch, int i) {
    uint32_t v;
    std::memcpy(&v, be_planar6 + 4 * (ch * n + i), 4);
    v = _byteswap_ulong(v);
    float f;
    std::memcpy(&f, &v, 4);
    return f;
  };
  constexpr float k = 0.7071f;
  const float volume = config::get().volume;
  for (int i = 0; i < n; i++) {
    float c = rd(2, i) * k, lfe = rd(3, i) * 0.5f;
    float l = (rd(0, i) + c + rd(4, i) * k + lfe) * volume;
    float r = (rd(1, i) + c + rd(5, i) * k + lfe) * volume;
    stereo[2 * i] = l < -1.f ? -1.f : l > 1.f ? 1.f : l;
    stereo[2 * i + 1] = r < -1.f ? -1.f : r > 1.f ? 1.f : r;
  }
  if (g_dump) {
    std::fwrite(stereo, sizeof(float), n * 2, g_dump);
    g_dump_bytes += n * 2 * sizeof(float);
    if ((g_dump_bytes & 0xFFFF) < n * 8) {  // atualiza o cabeçalho de vez em quando (o processo sai com _Exit)
      write_wav_header(g_dump, g_dump_bytes);
      std::fflush(g_dump);
    }
  }
  if (!g_active.load()) return;
  std::lock_guard<std::mutex> lock(g_mu);
  for (int i = 0; i < n; i++) {
    if (g_count >= kMaxQueued) {
      g_read = (g_read + 1) % kRingFrames;
      g_count--;
    }
    size_t w = (g_read + g_count) % kRingFrames;
    g_ring[2 * w] = stereo[2 * i];
    g_ring[2 * w + 1] = stereo[2 * i + 1];
    g_count++;
  }
}

uint64_t output_underruns() { return g_underruns.load(); }

}  // namespace apu
