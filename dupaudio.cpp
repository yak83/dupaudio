/* File: dupaudio.cpp
 *
 * Description: duplicates audio stream from one output to one or more additional outputs 
 * Author: Mike Fowler
 * Date: 23/11/2026
 * Version: 1.0
 * 
 * Details:
 * - A source output is selected. This can be the primary audio output, or optionally a
 *   virtual output e.g. VB-Audio (https://vb-audio.com/Cable/). The audio stream is
 *   captured using WASAPI loopback
 * - Captured audio data is fed into a unique ring buffer per device
 * - A thread per device services the local ring buffer, outputting the audio stream
 *   based on a hardware clock to prevent drift. Sample corrections are applied to
 *   maintain the target latency over longer sessions
 * - Devices reconnect based on selected name or ID, so an unexpected disconnect should
 *   be rapidly resolved with simply reconnecting the device to the same USB port
 * - If a duplicate output source becomes the default Windows output device the
 *   duplication is stopped to prevent a "feedback" scenario
 * - A basic UI is provided using a right-click NotifyIcon tray menu
 * - Configuration can be preset through a dupaudio.ini file. Additional settings are
 *   available within the file, e.g. setting target latency
 * - No installation required, just copy and run!
 * 
 * Usage:
 * - dupaudio.exe                   Runs app in the background. Right-click the tray NotifyIcon for basic settings
 * - dupaudio.exe --console         As above, but with debug console output
 * - dupaudio.exe --list            Lists the currently-available output devices to the console and quits
 * - dupaudio.exe --stop            Stops a running instance
 * - dupaudio.exe --config <file>   Uses the config file <file> rather than dupaudio.ini
 * 
 * Build Info:
 * - Windows-only; targetted for and tested on Windows 11 (Version 10.0.26300 Build 26300)
 * - C++ 17
 * - MS Visual C++ 2026 tools
 * - Requires standard libraries: ole32.lib avrt.lib shell32.lib user32.lib advapi32.lib gdi32.lib
 * - No other dependencies, no external packages/libraries
 * - basic build steps: (compile resources, compile code, clean up temporary files)
 *   > rc /nologo dupaudio.rc
 *   > cl /nologo /O2 /EHsc /std:c++17 /W4 /MT /DUNICODE /D_UNICODE dupaudio.cpp dupaudio.res /Fe:dupaudio.exe /link /SUBSYSTEM:WINDOWS ole32.lib avrt.lib shell32.lib user32.lib advapi32.lib gdi32.lib
 *   > del /q dupaudio.obj dupaudio.res 2>nul
 */

// use unicode to support fancy names on any sound output devices
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

// only need basic services - reduce the compiled size
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

// windows API libraries
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>
#include <mmreg.h>
#include <shellapi.h>

// C++ std libraries
#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _MSC_VER
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "avrt.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "gdi32.lib")
#endif

// helpers for expanding __FILE__, __LINE__ macros
#define S1(x) #x
#define S2(x) S1(x)
#define WIDEN2(x) L##x
#define WIDEN(x) WIDEN2(#x)

// required GUID constants
static const PROPERTYKEY kPkeyFriendlyName = {
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};
static const GUID kSubtypePcm = {
    0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
static const GUID kSubtypeFloat = {
    0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

static const wchar_t* kStopEventName = L"Local\\DupAudio_Stop";
static const wchar_t* kInstanceMutexName = L"Local\\DupAudio_Instance";

static const DWORD kRetryMs = 2000;                     // reconnect interval - 2-sec
static const REFERENCE_TIME kCaptureBuffer = 2000000;   // capture buffer timeslots - 200-ms (100-ns units)
static const REFERENCE_TIME kRenderBuffer = 200000;     // rendering buffer timeslots - 20-ms (100-ns units)
static const size_t kRingFrames = 192000;               // 1-s total ring buffer size at 192 kHz, stereo float

//
// COM functions
//

template <class T>
class Com
{
public:
    Com() = default;
    ~Com() { reset(); }
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    T* operator->() const { return p_; }
    T* get() const { return p_; }
    T** put() { reset(); return &p_; }
    void** putVoid() { reset(); return reinterpret_cast<void**>(&p_); }
    void reset() { if (p_) { p_->Release(); p_ = nullptr; } }
    explicit operator bool() const { return p_ != nullptr; }
private:
    T* p_ = nullptr;
};

struct CoTaskFree { void operator()(void* p) const { CoTaskMemFree(p); } };
using WaveFormatPtr = std::unique_ptr<WAVEFORMATEX, CoTaskFree>;

struct HandleCloser
{
    HANDLE h = nullptr;
    explicit HandleCloser(HANDLE x) : h(x) {}
    ~HandleCloser() { if (h) CloseHandle(h); }
    HandleCloser(const HandleCloser&) = delete;
    HandleCloser& operator=(const HandleCloser&) = delete;
};

struct ComInit
{
    ComInit() { CoInitializeEx(nullptr, COINIT_MULTITHREADED); }
    ~ComInit() { CoUninitialize(); }
};

struct MmcssScope
{
    HANDLE h = nullptr;
    MmcssScope() { DWORD idx = 0; h = AvSetMmThreadCharacteristicsW(L"Pro Audio", &idx); }
    ~MmcssScope() { if (h) AvRevertMmThreadCharacteristics(h); }
};


//
// logging to file/console
//

static std::mutex g_logMutex;
static std::wstring g_logPath;
static bool g_logToConsole = false;     // set by command-line flag "dupaudio --console"
static HANDLE g_pipeStop = nullptr;     // manual-reset: tells the audio threads to stop
static HANDLE g_quitEvent = nullptr;    // named: set by "dupaudio --stop" to exit the app

static void Log(const wchar_t* fmt, ...)
{
  wchar_t msg[1024];
  va_list ap;
  va_start(ap, fmt);
  _vsnwprintf_s(msg, _countof(msg), _TRUNCATE, fmt, ap);
  va_end(ap);

  // format message text
  SYSTEMTIME st;
  GetLocalTime(&st);
  wchar_t line[1200];
  _snwprintf_s(line, _countof(line), _TRUNCATE, L"%04u-%02u-%02u %02u:%02u:%02u  %ls\n", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, msg);

  // grab log file mutex
  std::lock_guard<std::mutex> lock(g_logMutex);

  // write to console (if flag set)
  if (g_logToConsole)
  { 
    fputws(line, stdout);
    fflush(stdout);
  }
  if (g_logPath.empty()) return;

  // roll over log file if oversize (1mb)
  WIN32_FILE_ATTRIBUTE_DATA fad;
  if (GetFileAttributesExW(g_logPath.c_str(), GetFileExInfoStandard, &fad) && (fad.nFileSizeHigh != 0 || fad.nFileSizeLow > 1024 * 1024))
  {
    std::wstring old = g_logPath + L".old";
    MoveFileExW(g_logPath.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING);
  }

  // append line to logfile, use UTF-8 encoding
  FILE* f = nullptr;
  if (_wfopen_s(&f, g_logPath.c_str(), L"a, ccs=UTF-8") == 0 && f)
  {
    fputws(line, f);
    fclose(f);
  }

  // mutex released in lock_guard destructor - manual unlock() call causes double unlock - undefined behaviour
}

// default log line formatting of hError results
#define FormatHrText(w,h) HrText(w, h, WIDEN(__FILE__), WIDEN(S2(__LINE__)), __FUNCTIONW__)
static std::wstring HrText(const wchar_t* what, HRESULT hr, wchar_t* file, wchar_t* line, wchar_t* func)
{
  wchar_t buf[160];
  const wchar_t* hint = L"";
  switch (hr)
  {
    case AUDCLNT_E_DEVICE_INVALIDATED: hint = L" (device removed or reconfigured)"; break;
    case AUDCLNT_E_DEVICE_IN_USE: hint = L" (device in exclusive use by another app)"; break;
    case AUDCLNT_E_SERVICE_NOT_RUNNING: hint = L" (Windows Audio service not running)"; break;
    case AUDCLNT_E_UNSUPPORTED_FORMAT: hint = L" (unsupported format)"; break;
    default: hint = L" (unknown error)"; break;
  }
  _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"%ls(%ls)%ls(): %ls failed, hr=0x%08lX%ls", file, line, func, what, static_cast<unsigned long>(hr), hint);
  return buf;
}

static bool StopRequested(DWORD waitMs = 0)
{
  return WaitForSingleObject(g_pipeStop, waitMs) == WAIT_OBJECT_0;
}

//
// config reader/writer code
//

// default config
struct Config
{
  std::wstring source = L"default";
  std::wstring sourceLabel;
  std::vector<std::wstring> outputs;
  std::vector<std::wstring> outputLabels;  // same size as outputs
  int latencyMs = 50;
};

static std::wstring Trim(const std::wstring& s)
{
  size_t a = 0, b = s.size();
  while (a < b && iswspace(s[a])) ++a;
  while (b > a && iswspace(s[b - 1])) --b;
  return s.substr(a, b - a);
}

static std::wstring Lower(std::wstring s)
{
  for (auto& c : s) c = static_cast<wchar_t>(towlower(c));
  return s;
}

static bool LoadConfig(const std::wstring& path, Config& cfg, std::wstring& err)
{
  HandleCloser file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
  
  if (file.h == INVALID_HANDLE_VALUE)
  {
    file.h = nullptr; err = L"cannot open " + path; return false;
  }

  LARGE_INTEGER size{};
  if (!GetFileSizeEx(file.h, &size) || size.QuadPart > 1024 * 1024)
  {
    // sanity check size of the config file
    err = L"bad config file size"; return false;
  }
  
  std::string bytes(static_cast<size_t>(size.QuadPart), '\0');
  DWORD read = 0;
  if (!bytes.empty() && !ReadFile(file.h, &bytes[0], static_cast<DWORD>(bytes.size()), &read, nullptr))
  {
    err = L"cannot read " + path; return false;
  }
  bytes.resize(read);
  if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF && static_cast<unsigned char>(bytes[1]) == 0xBB && static_cast<unsigned char>(bytes[2]) == 0xBF)
    bytes.erase(0, 3);  // UTF-8 BOM

  std::wstring text;
  if (!bytes.empty())
  {
    int n = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    text.resize(n);
    MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), &text[0], n);
  }

  size_t pos = 0;
  int lineNo = 0;
  while (pos <= text.size()) {
    size_t end = text.find(L'\n', pos);
    if (end == std::wstring::npos)
      end = text.size();
    std::wstring line = Trim(text.substr(pos, end - pos));

    pos = end + 1;
    ++lineNo;
    if (line.empty() || line[0] == L';' || line[0] == L'#' || line[0] == L'[')
      continue;

      size_t eq = line.find(L'=');
    if (eq == std::wstring::npos)
    {
      Log(L"Config line %d ignored (no '='): %ls", lineNo, line.c_str());
      continue;
    }

    std::wstring key = Lower(Trim(line.substr(0, eq)));
    std::wstring val = Trim(line.substr(eq + 1));
    std::wstring label;
    size_t semi = val.find(L" ;");
    if (semi == std::wstring::npos)
      semi = val.find(L"\t;");
    if (semi != std::wstring::npos)
    {
      label = Trim(val.substr(semi + 2));
      val = Trim(val.substr(0, semi));
    }

    if (val.size() >= 2 && val.front() == L'"' && val.back() == L'"')
      val = val.substr(1, val.size() - 2);

    if (key == L"source")
    {
      cfg.source = val; cfg.sourceLabel = label;
    }
    else if (key == L"output")
    {
      if (!val.empty()) { cfg.outputs.push_back(val); cfg.outputLabels.push_back(label); }
    }
    else if (key == L"latency_ms")
      cfg.latencyMs = std::max(20, std::min(500, _wtoi(val.c_str())));
    else
      Log(L"Config line %d: unknown key '%ls'", lineNo, key.c_str());
  }

  if (cfg.source.empty())
    cfg.source = L"default";
  
  return true;
}

static bool SaveConfig(const std::wstring& path, const Config& cfg)
{
  auto entry = [](const wchar_t* key, const std::wstring& val, const std::wstring& label)
  {
    std::wstring s = std::wstring(key) + L"=" + val;
    if (!label.empty()) s += L"   ; " + label;
    return s + L"\r\n";
  };

  std::wstring text =
    L"; dupaudio configuration. Normally changed from the dupaudio tray icon menu.\r\n"
    L"; source = device whose sound is copied (\"default\" = Windows default output).\r\n"
    L"; output = a device that gets a copy (one line each).\r\n"
    L"; Values can be part of a device name or a full device ID (see dupaudio --list).\r\n"
    L"; Text after \" ;\" is just a label.\r\n\r\n";
  text += entry(L"source", cfg.source, cfg.sourceLabel);
  text += L"\r\n";

  for (size_t i = 0; i < cfg.outputs.size(); ++i)
    text += entry(L"output", cfg.outputs[i], i < cfg.outputLabels.size() ? cfg.outputLabels[i] : L"");

  text += L"\r\n; Buffer per output in milliseconds (20-500). Raise to 80-100 if you hear crackles.\r\n";
  text += L"latency_ms=" + std::to_wstring(cfg.latencyMs) + L"\r\n";

  std::string utf8;
  int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
  utf8.resize(static_cast<size_t>(n));
  WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), &utf8[0], n, nullptr, nullptr);

  // write to a temp file, then swap it in, so a crash never leaves a half-written config.
  const std::wstring tmp = path + L".tmp";
  {
    HandleCloser file(CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (file.h == INVALID_HANDLE_VALUE)
    {
      file.h = nullptr;
      Log(L"Cannot write %ls", tmp.c_str());
      return false;
    }
    DWORD written = 0;
    if (!WriteFile(file.h, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr) || written != utf8.size())
    {
      Log(L"Failed to write config entry '%ls'", tmp.c_str());
      return false;
    }
  }

  if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
  {
    Log(L"Cannot replace config file %ls (error %lu)", path.c_str(), GetLastError());
    return false;
  }
  return true;
}

// windows device lookup
struct DeviceInfo { std::wstring id, name; };

static std::wstring DeviceId(IMMDevice* dev) {
  std::wstring out;
  LPWSTR id = nullptr;
  if (SUCCEEDED(dev->GetId(&id)) && id)
  {
    out = id;
    CoTaskMemFree(id);
  }
  return out;
}

static std::wstring DeviceName(IMMDevice* dev) {
  std::wstring out;
  Com<IPropertyStore> props;

  if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, props.put())))
  {
    PROPVARIANT pv;
    PropVariantInit(&pv);

    if (SUCCEEDED(props->GetValue(kPkeyFriendlyName, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal)
      out = pv.pwszVal;

    PropVariantClear(&pv);
  }
  return out;
}

static std::vector<DeviceInfo> ListRenderDevices(IMMDeviceEnumerator* en)
{
  std::vector<DeviceInfo> list;
  Com<IMMDeviceCollection> col;
  if (FAILED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, col.put())))
    return list;

  UINT count = 0;
  col->GetCount(&count);
  for (UINT i = 0; i < count; ++i)
  {
    Com<IMMDevice> dev;
    if (FAILED(col->Item(i, dev.put())))
      continue;
    list.push_back({DeviceId(dev.get()), DeviceName(dev.get())});
  }

  return list;
}

static std::wstring DefaultRenderId(IMMDeviceEnumerator* en)
{
  Com<IMMDevice> dev;
  if (FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, dev.put())))
    return L"";
  
  return DeviceId(dev.get());
}

// Finds the best matching device based on search string
// 'search' parameter:
//    "default"
//    a full device ID ("{0.0.0.00000000}.{...}")
//    part of the device name
static bool FindDevice(IMMDeviceEnumerator* en, const std::wstring& search, Com<IMMDevice>& dev, std::wstring& id, std::wstring& name, std::wstring& err)
{
  if (Lower(search) == L"default")
  {
    HRESULT hr = en->GetDefaultAudioEndpoint(eRender, eConsole, dev.put());
    if (FAILED(hr))
    {
      err = FormatHrText(L"GetDefaultAudioEndpoint", hr);
      return false;
    }
  }
  else if (search.rfind(L"{0.0.", 0) == 0)
  {
    HRESULT hr = en->GetDevice(search.c_str(), dev.put());
    if (FAILED(hr))
    {
      err = L"device ID not present: " + search;
      return false;
    }
    DWORD state = 0;
    if (FAILED(dev->GetState(&state)) || state != DEVICE_STATE_ACTIVE)
    {
      err = L"device not connected: " + search;
      return false;
    }
  }
  else
  {
    std::wstring want = Lower(search);
    std::wstring matchId;
    int matches = 0;
    for (const auto& d : ListRenderDevices(en))
    {
      if (Lower(d.name).find(want) != std::wstring::npos)
      {
        if (matches == 0) matchId = d.id;
        ++matches;
      }
    }

    if (matches == 0)
    {
      err = L"no connected playback device matches '" + search + L"'";
      return false;
    }
    if (matches > 1)
      Log(L"Warning: '%ls' matches %d devices; using the first. Use a device ID (see --list) to target specific device", search.c_str(), matches);
    
    HRESULT hr = en->GetDevice(matchId.c_str(), dev.put());
    if (FAILED(hr))
    {
      err = FormatHrText(L"GetDevice", hr);
      return false;
    }
  }

  id = DeviceId(dev.get());
  name = DeviceName(dev.get());
  return true;
}

//
// audio & sampling
//
enum class SampleFmt
{
  Float32,
  Int16,
  Int24,
  Int32,
  Unsupported,
};

static SampleFmt Classify(const WAVEFORMATEX* wf)
{
  if (!wf || wf->nChannels == 0)
    return SampleFmt::Unsupported;

  WORD tag = wf->wFormatTag;
  // check audio format - currently only supporting IEEE-float & PCM
  if (tag == WAVE_FORMAT_EXTENSIBLE && wf->cbSize >= 22)
  {
    const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wf);
    if (IsEqualGUID(ext->SubFormat, kSubtypeFloat))
      tag = WAVE_FORMAT_IEEE_FLOAT;

    else if (IsEqualGUID(ext->SubFormat, kSubtypePcm))
      tag = WAVE_FORMAT_PCM;

    else
      return SampleFmt::Unsupported;
  }

  // return sample data in correct form
  const unsigned bytes = wf->nBlockAlign / wf->nChannels;
  if (tag == WAVE_FORMAT_IEEE_FLOAT && bytes == 4)
    return SampleFmt::Float32;

  if (tag == WAVE_FORMAT_PCM)
  {
    switch (bytes)
    {
      case 2:
        return SampleFmt::Int16;
    
      case 3:
        return SampleFmt::Int24;
    
      case 4:
        return SampleFmt::Int32;
    }
  }

  return SampleFmt::Unsupported;
}

static inline float ReadSample(const BYTE* p, SampleFmt fmt)
{
  // for a given sample, convert byte data into float and return scaled value

  switch (fmt) {
    case SampleFmt::Float32:
    {
      float v;
      memcpy(&v, p, 4);
      return v;
    }
    case SampleFmt::Int16:
    {
      int16_t v;
      memcpy(&v, p, 2);
      return v * (1.0f / 32768.0f);
    }
    case SampleFmt::Int24:
    {
      int32_t v = static_cast<int32_t>((static_cast<uint32_t>(p[0]) << 8) | (static_cast<uint32_t>(p[1]) << 16) | (static_cast<uint32_t>(p[2]) << 24));
      return v * (1.0f / 2147483648.0f);
    }
    case SampleFmt::Int32:
    {
      int32_t v;
      memcpy(&v, p, 4);
      return v * (1.0f / 2147483648.0f); 
    }

    // return silence for unrecognised sample format data
    default:
      return 0.0f;
  }
}

static inline void WriteSample(BYTE* p, SampleFmt fmt, float v)
{
  // clamp value to -1.0 : +1.0 range
  v = std::max(-1.0f, std::min(1.0f, v));

  // format value according to requested sampling format, scaling up to appropriate integer range
  switch (fmt)
  {
    case SampleFmt::Float32:
      memcpy(p, &v, 4);
      break;

    case SampleFmt::Int16:
    {
      int16_t s = static_cast<int16_t>(v * 32767.0f);
      memcpy(p, &s, 2);
      break;
    }
    case SampleFmt::Int24:
    {
      int32_t s = static_cast<int32_t>(v * 8388607.0f);
      p[0] = static_cast<BYTE>(s);
      p[1] = static_cast<BYTE>(s >> 8);
      p[2] = static_cast<BYTE>(s >> 16);
      break;
    }
    case SampleFmt::Int32:
    {
      int32_t s = static_cast<int32_t>(static_cast<double>(v) * 2147483647.0);
      memcpy(p, &s, 4);
      break;
    }

    default:
      break;
  }
}

// lock-free ring buffer for stereo float frames
class StereoRing
{
public:
  explicit StereoRing(size_t frames) : cap_(frames), buf_(frames * 2, 0.0f) {}

  // producer side. frames that don't fit are dropped (consumer is gone or stalled - audio drop will occur).
  void Push(const float* stereo, size_t n)
  {
    const size_t w = w_.load(std::memory_order_relaxed);
    const size_t r = r_.load(std::memory_order_acquire);
    const size_t space = cap_ - (w - r);

    if (n > space)
      n = space;

    for (size_t i = 0; i < n; ++i)
    {
      const size_t k = ((w + i) % cap_) * 2;
      buf_[k] = stereo[i * 2];
      buf_[k + 1] = stereo[i * 2 + 1];
    }

    w_.store(w + n, std::memory_order_release);
  }

  // consumer side
  size_t Available() const
  {
    return w_.load(std::memory_order_acquire) - r_.load(std::memory_order_relaxed);
  }

  bool Pop(float& l, float& r)
  {
    const size_t ri = r_.load(std::memory_order_relaxed);

    if (w_.load(std::memory_order_acquire) == ri)
      return false;

    const size_t k = (ri % cap_) * 2;
    l = buf_[k];
    r = buf_[k + 1];
    r_.store(ri + 1, std::memory_order_release);

    return true;
  }

  void Skip(size_t n)
  {
    n = std::min(n, Available());
    r_.store(r_.load(std::memory_order_relaxed) + n, std::memory_order_release);
  }

  void Clear()
  {
    Skip(Available());
  }

private:
  const size_t cap_;
  std::vector<float> buf_;

  #pragma warning (push)                  // disable distracting compiler warning about padding for alignment
    #pragma warning(disable:4324)
    alignas(64) std::atomic<size_t> w_{0};
    alignas(64) std::atomic<size_t> r_{0};
  #pragma warning(pop)                    // re-enable compiler warning C4324
};

// shared state store
struct Shared
{
  Config cfg;
  std::vector<std::unique_ptr<StereoRing>> rings;   // one per output
  std::unique_ptr<std::atomic<bool>[]> outputOpen;  // one per output, for the tray tooltip
  std::atomic<bool> sourceOpen{false};
  std::atomic<uint32_t> sourceRate{48000};

  void SetSource(const std::wstring& id, const std::wstring& name)
  {
    std::lock_guard<std::mutex> l(m_);
    sourceId_ = id;
    sourceName_ = name;
  }

  std::wstring SourceId()
  {
    std::lock_guard<std::mutex> l(m_);
    return sourceId_;
  }

  std::wstring SourceName()
  {
    std::lock_guard<std::mutex> l(m_);
    return sourceName_;
  }

  private:
  std::mutex m_;
  std::wstring sourceId_, sourceName_;
};

// result of one streaming session: retryNow = reopen immediately (no error).
struct RunResult
{
  bool retryNow;
  std::wstring error;
};

// capture (WASAPI loopback of the source endpoint)
static RunResult RunCapture(Shared& sh, std::vector<float>& stereo, std::wstring& lastErr)
{
  Com<IMMDeviceEnumerator> en;
  HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), en.putVoid());

  if (FAILED(hr))
    return {false, FormatHrText(L"CoCreateInstance(MMDeviceEnumerator)", hr)};

  Com<IMMDevice> dev;
  std::wstring id, name, err;

  if (!FindDevice(en.get(), sh.cfg.source, dev, id, name, err))
    return {false, err};
  const bool followDefault = Lower(sh.cfg.source) == L"default";

  Com<IAudioClient> client;
  hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, client.putVoid());
  if (FAILED(hr))
    return {false, FormatHrText(L"Activate(source)", hr)};

  WAVEFORMATEX *raw = nullptr;
  hr = client->GetMixFormat(&raw);
  if (FAILED(hr))
    return {false, FormatHrText(L"GetMixFormat(source)", hr)};
  WaveFormatPtr wf(raw);
  const SampleFmt fmt = Classify(wf.get());
  if (fmt == SampleFmt::Unsupported)
    return {false, L"source mix format not supported"};

  hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, kCaptureBuffer, 0, wf.get(), nullptr);
  if (FAILED(hr))
    return {false, FormatHrText(L"Initialize(source loopback)", hr)};

  Com<IAudioCaptureClient> capture;
  hr = client->GetService(__uuidof(IAudioCaptureClient), capture.putVoid());
  if (FAILED(hr))
    return {false, FormatHrText(L"GetService(IAudioCaptureClient)", hr)};

  sh.SetSource(id, name);
  sh.sourceRate.store(wf->nSamplesPerSec);
  hr = client->Start();
  if (FAILED(hr))
    return {false, FormatHrText(L"Start(source)", hr)};
  sh.sourceOpen = true;

  Log(L"Source opened: %ls  [%u Hz, %u channels]", name.c_str(), wf->nSamplesPerSec, wf->nChannels);
  lastErr.clear();

  const unsigned channels = wf->nChannels;
  const unsigned bytesPerSample = wf->nBlockAlign / channels;
  unsigned ticks = 0;
  RunResult result{true, L""};

  while (!StopRequested(5))
  {
    if (followDefault && ++ticks >= 200) // roughly every 1-3 s
    {
      ticks = 0;
      if (DefaultRenderId(en.get()) != id)
      {
        Log(L"Default playback device changed; switching source.");
        result = {true, L""};
        break;
      }
    }
    bool failed = false;

    for (;;)  // infinite loop, exits when sample packet buffer emptied or on error
    {
      UINT32 packet = 0;
      hr = capture->GetNextPacketSize(&packet);
      if (FAILED(hr))
      {
        result = {false, FormatHrText(L"GetNextPacketSize", hr)};
        failed = true;
        break;
      }
      if (packet == 0)
        break;

      BYTE *data = nullptr;
      UINT32 frames = 0;
      DWORD flags = 0;
      hr = capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
      if (FAILED(hr))
      {
        result = {false, FormatHrText(L"GetBuffer(capture)", hr)};
        failed = true;
        break;
      }

      if (stereo.size() < frames * 2u)
        stereo.resize(frames * 2u);

      if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) || !data)
      {
        std::fill(stereo.begin(), stereo.begin() + frames * 2u, 0.0f);
      }
      else
      {
        for (UINT32 i = 0; i < frames; ++i)
        {
          const BYTE *f = data + static_cast<size_t>(i) * wf->nBlockAlign;
          const float l = ReadSample(f, fmt);
          const float r = channels > 1 ? ReadSample(f + bytesPerSample, fmt) : l;
          stereo[i * 2] = l;
          stereo[i * 2 + 1] = r;
        }
      }
      capture->ReleaseBuffer(frames);
      for (auto &ring : sh.rings)
        ring->Push(stereo.data(), frames);
    }

    if (failed)
      break;
  }

  client->Stop();
  return result;
}

// thread entrypoint for capture threads (single thread per app instance)
static void CaptureThread(Shared *sh)
{
  ComInit com;
  MmcssScope mmcss;
  std::vector<float> stereo(48000 * 2);
  std::wstring lastErr;

  while (!StopRequested())
  {
    RunResult r{false, L""};
    try
    {
      r = RunCapture(*sh, stereo, lastErr);
    }
    catch (...)
    {
      r = {false, L"Enexpected exception in capture"};
    }

    sh->sourceOpen = false;

    if (StopRequested())
      break;

    if (r.retryNow)
      continue;

    if (r.error != lastErr)
    {
      Log(L"Source: %ls (retrying every %u s)", r.error.c_str(), kRetryMs / 1000);
      lastErr = r.error;
    }

    if (StopRequested(kRetryMs))
      break;
  }
}

// render (one per output endpoint)
static RunResult RunRender(Shared &sh, size_t index, std::wstring &lastErr)
{
  const std::wstring &spec = sh.cfg.outputs[index];
  StereoRing &ring = *sh.rings[index];

  Com<IMMDeviceEnumerator> en;
  HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), en.putVoid());
  if (FAILED(hr))
    return {false, FormatHrText(L"CoCreateInstance(MMDeviceEnumerator)", hr)};

  Com<IMMDevice> dev;
  std::wstring id, name, err;
  if (!FindDevice(en.get(), spec, dev, id, name, err))
    return {false, err};
  if (id == sh.SourceId())
    return {false, L"\"" + name + L"\" is the source device; skipped to avoid a feedback loop"};

  Com<IAudioClient> client;
  hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, client.putVoid());
  if (FAILED(hr))
    return {false, FormatHrText(L"Activate(output)", hr)};

  WAVEFORMATEX *raw = nullptr;
  hr = client->GetMixFormat(&raw);
  if (FAILED(hr))
    return {false, FormatHrText(L"GetMixFormat(output)", hr)};
  WaveFormatPtr wf(raw);
  const SampleFmt fmt = Classify(wf.get());
  if (fmt == SampleFmt::Unsupported)
    return {false, L"output mix format not supported"};

  HandleCloser event(CreateEventW(nullptr, FALSE, FALSE, nullptr));
  if (!event.h)
    return {false, L"CreateEvent failed"};

  hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, kRenderBuffer, 0, wf.get(), nullptr);
  if (FAILED(hr))
    return {false, FormatHrText(L"Initialize(output)", hr)};
  hr = client->SetEventHandle(event.h);
  if (FAILED(hr))
    return {false, FormatHrText(L"SetEventHandle", hr)};

  UINT32 bufferFrames = 0;
  hr = client->GetBufferSize(&bufferFrames);
  if (FAILED(hr))
    return {false, FormatHrText(L"GetBufferSize", hr)};

  Com<IAudioRenderClient> render;
  hr = client->GetService(__uuidof(IAudioRenderClient), render.putVoid());
  if (FAILED(hr))
    return {false, FormatHrText(L"GetService(IAudioRenderClient)", hr)};

  // start with a buffer of silence so the device has something to play
  BYTE *out = nullptr;
  if (SUCCEEDED(render->GetBuffer(bufferFrames, &out)))
    render->ReleaseBuffer(bufferFrames, AUDCLNT_BUFFERFLAGS_SILENT);

  ring.Clear();
  hr = client->Start();
  if (FAILED(hr))
    return {false, FormatHrText(L"Start(output)", hr)};
  sh.outputOpen[index] = true;

  Log(L"Output %u opened: %ls  [%u Hz, %u ch]", static_cast<unsigned>(index + 1), name.c_str(),
      wf->nSamplesPerSec, wf->nChannels);
  lastErr.clear();

  const unsigned channels = wf->nChannels;
  const unsigned bytesPerSample = wf->nBlockAlign / channels;
  const double dstRate = static_cast<double>(wf->nSamplesPerSec);

  bool playing = false;
  double frac = 0.0; // position between current and next source frame
  float cur[2] = {0, 0}, nxt[2] = {0, 0};
  double fillAvg = 0.0; // smoothed ring fill level (frames)
  RunResult result{false, L""};
  HANDLE waits[2] = {g_pipeStop, event.h};
  unsigned ticks = 0;

  for (;;)
  {
    const DWORD w = WaitForMultipleObjects(2, waits, FALSE, 2000);
    if (w == WAIT_OBJECT_0)
    {
      result = {true, L""};
      break;
    }
    if (w != WAIT_OBJECT_0 + 1)
    {
      result = {false, L"output stopped responding"};
      break;
    }

    // if the source has since become this same device (e.g. the Windows default changed), stop playing here so we never feed our own output back in.
    if (++ticks >= 100)
    {
      ticks = 0;
      if (sh.SourceId() == id)
      {
        result = {false, L"\"" + name + L"\" is now the source device; paused to avoid a feedback loop"};
        break;
      }
    }

    UINT32 padding = 0;
    hr = client->GetCurrentPadding(&padding);
    if (FAILED(hr))
    {
      result = {false, FormatHrText(L"GetCurrentPadding", hr)};
      break;
    }
    const UINT32 avail = bufferFrames - padding;
    if (avail == 0)
      continue;

    hr = render->GetBuffer(avail, &out);
    if (FAILED(hr))
    {
      result = {false, FormatHrText(L"GetBuffer(render)", hr)};
      break;
    }

    const double srcRate = static_cast<double>(sh.sourceRate.load());
    const size_t target = static_cast<size_t>(srcRate * sh.cfg.latencyMs / 1000.0);
    const size_t fill = ring.Available();

    if (!playing)
    {
      // wait until a full target's worth of audio has arrived, then start.
      if (fill >= target && fill >= 2)
      {
        ring.Skip(fill - target);
        ring.Pop(cur[0], cur[1]);
        ring.Pop(nxt[0], nxt[1]);
        frac = 0.0;
        fillAvg = static_cast<double>(target);
        playing = true;
      }
    }
    else if (fill > target * 4)
    {
      // far behind (e.g. after a system stall): jump back to the target delay.
      ring.Skip(fill - target);
      fillAvg = static_cast<double>(target);
    }

    if (!playing)
    {
      render->ReleaseBuffer(avail, AUDCLNT_BUFFERFLAGS_SILENT);
      continue;
    }

    // clock-drift correction: nudge the playback speed so the buffer sits at the target.
    fillAvg += 0.02 * (static_cast<double>(fill) - fillAvg);
    const double errorSec = (fillAvg - static_cast<double>(target)) / srcRate;
    const double adjust = std::max(-0.003, std::min(0.003, errorSec * 0.05));
    const double step = (srcRate / dstRate) * (1.0 + adjust);

    for (UINT32 i = 0; i < avail; ++i)
    {
      while (frac >= 1.0)
      {
        cur[0] = nxt[0];
        cur[1] = nxt[1];
        if (!ring.Pop(nxt[0], nxt[1]))
        {
          playing = false;
          break;
        }
        frac -= 1.0;
      }
      BYTE *frame = out + static_cast<size_t>(i) * wf->nBlockAlign;
      if (!playing)
      { // source ran dry: pad with silence and re-buffer
        memset(frame, 0, static_cast<size_t>(avail - i) * wf->nBlockAlign);
        break;
      }
      const float t = static_cast<float>(frac);
      const float l = cur[0] + (nxt[0] - cur[0]) * t;
      const float r = cur[1] + (nxt[1] - cur[1]) * t;
      if (channels == 1)
      {
        WriteSample(frame, fmt, 0.5f * (l + r));
      }
      else
      {
        WriteSample(frame, fmt, l);
        WriteSample(frame + bytesPerSample, fmt, r);
        for (unsigned c = 2; c < channels; ++c)
          WriteSample(frame + c * bytesPerSample, fmt, 0.0f);
      }
      frac += step;
    }
    render->ReleaseBuffer(avail, 0);
  }
  client->Stop();
  return result;
}

struct RenderArgs
{
  Shared *sh;
  size_t index;
};

static void RenderThread(RenderArgs args)
{
  ComInit com;
  MmcssScope mmcss;
  std::wstring lastErr;
  while (!StopRequested())
  {
    RunResult r{false, L""};
    try
    {
      r = RunRender(*args.sh, args.index, lastErr);
    }
    catch (...)
    {
      r = {false, L"unexpected exception in output"};
    }
    args.sh->outputOpen[args.index] = false;
    if (StopRequested())
      break;
    if (r.retryNow)
      continue;
    if (r.error != lastErr)
    {
      Log(L"Output %u: %ls (retrying every %u s)", static_cast<unsigned>(args.index + 1), r.error.c_str(),
          kRetryMs / 1000);
      lastErr = r.error;
    }
    if (StopRequested(kRetryMs))
      break;
  }
}

// audio pipeline: capture thread + one render thread per output, restartable
class Pipeline
{
public:
  ~Pipeline() { Stop(); }

  void Start(const Config &cfg)
  {
    // prevent duplicate operation and clear any previous pipeline Stop event signal
    Stop();
    ResetEvent(g_pipeStop);

    // create shared state with latest config
    sh_ = std::make_unique<Shared>();
    sh_->cfg = cfg;

    const size_t n = cfg.outputs.size();
    sh_->outputOpen.reset(new std::atomic<bool>[n ? n : 1]);

    // create output ring buffer for each output
    for (size_t i = 0; i < n; ++i)
    {
      sh_->outputOpen[i] = false;
      sh_->rings.push_back(std::make_unique<StereoRing>(kRingFrames));
    }
    Log(L"Audio started. Source: \"%ls\", %u output(s), latency %d ms.", cfg.source.c_str(), static_cast<unsigned>(n), cfg.latencyMs);

    // log output status
    for (size_t i = 0; i < n; ++i)
      Log(L"  Output %u: \"%ls\"", static_cast<unsigned>(i + 1), cfg.outputs[i].c_str());
    
    if (n == 0)
      return; // nothing to copy to; don't hold the source open
    
    // add capture thread to thread list array
    threads_.emplace_back(CaptureThread, sh_.get());

    // add each output thread to thread list array
    for (size_t i = 0; i < n; ++i)
      threads_.emplace_back(RenderThread, RenderArgs{sh_.get(), i});
  }

  void Stop()
  {
    if (!sh_)
      return;
    
    // set global pipeline shutdown event
    SetEvent(g_pipeStop);

    // block on each thread until execution completes (upon detecting g_pipeStop flag) - required before destruction
    for (auto &t : threads_)
      t.join();

    // clear thread array and reset shared state
    threads_.clear();
    sh_.reset();
  }

  Shared *shared() const { return sh_.get(); }

private:
  std::unique_ptr<Shared> sh_;
  std::vector<std::thread> threads_;
};


// local HKCU registry settings for "Start with Windows" setting (local user only, no admin rights needed)
static const wchar_t *kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t *kRunValue = L"dupaudio";

static std::wstring ExePath()
{
  wchar_t path[MAX_PATH * 4];
  DWORD n = GetModuleFileNameW(nullptr, path, _countof(path));
  return std::wstring(path, n);
}

static std::wstring ExeDir()
{
  std::wstring s = ExePath();
  size_t slash = s.find_last_of(L"\\/");
  return slash == std::wstring::npos ? L"." : s.substr(0, slash);
}

static bool IsStartWithWindows()
{
  DWORD size = 0;
  return RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, RRF_RT_REG_SZ, nullptr, nullptr, &size) == ERROR_SUCCESS;
}

static void SetStartWithWindows(bool on)
{
  if (on)
  {
    // create key if setting enabled (overwrites if key already exists)
    std::wstring cmd = L"\"" + ExePath() + L"\"";
    LSTATUS rc = RegSetKeyValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, REG_SZ, cmd.c_str(), static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
    Log(rc == ERROR_SUCCESS ? L"Start with Windows: on" : L"Start with Windows: could not set (error %ld)", rc);
  }
  else
  {
    // delete key if unset (ignore error, probably just missing key)
    RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey, kRunValue);
    Log(L"Start with Windows: off");
  }
}

// NotifyIcon tray icon, using the app icon (two speakers back to back) from the exe's resources.
static const int kAppIconId = 1;    // from dupaudio.rc

static HICON LoadAppIcon(HINSTANCE inst, int size)
{
  HICON icon = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(kAppIconId), IMAGE_ICON, size, size, 0));
  return icon ? icon : static_cast<HICON>(LoadImageW(nullptr, IDI_APPLICATION, IMAGE_ICON, size, size, LR_SHARED));
}

static HICON MakeTrayIcon()
{
  const int s = GetSystemMetricsForDpi(SM_CXSMICON, GetDpiForSystem());
  return LoadAppIcon(GetModuleHandleW(nullptr), s);
}

// tray application
static const UINT WM_TRAYICON = WM_APP + 1;
static const UINT_PTR kStatusTimer = 1;
enum : UINT
{
  ID_EXIT = 100,
  ID_STARTUP,
  ID_OPENLOG,
  ID_SOUNDSETTINGS,
  ID_OUTPUT_BASE = 1000,  // + index into App::menuDevices
  ID_MISSING_BASE = 3000, // + index into Config::outputs (configured but not connected)
  ID_SOURCE_DEFAULT = 4999,
  ID_SOURCE_BASE = 5000, // + index into App::menuDevices
};

struct App
{
  HWND hwnd = nullptr;
  HICON icon = nullptr;
  UINT taskbarCreated = 0;
  std::wstring configPath;
  Config cfg;
  Pipeline pipeline;
  std::vector<DeviceInfo> menuDevices; // snapshot taken when the menu opens
  std::wstring lastTip;
};
static App *g_app = nullptr;

static bool SpecMatches(const std::wstring &spec, const DeviceInfo &d)
{
  if (spec.rfind(L"{0.0.", 0) == 0)
    return _wcsicmp(spec.c_str(), d.id.c_str()) == 0;
  if (Lower(spec) == L"default" || spec.empty())
    return false;
  return Lower(d.name).find(Lower(spec)) != std::wstring::npos;
}

static std::wstring SpecLabel(const std::wstring &spec, const std::wstring &label)
{
  return label.empty() ? spec : label;
}

static void TrayAdd(App &app)
{
  NOTIFYICONDATAW nid = {};
  nid.cbSize = sizeof(nid);
  nid.hWnd = app.hwnd;
  nid.uID = 1;
  nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
  nid.uCallbackMessage = WM_TRAYICON;
  nid.hIcon = app.icon;
  wcscpy_s(nid.szTip, L"dupaudio");
  Shell_NotifyIconW(NIM_ADD, &nid);
  app.lastTip.clear();
}

static void TrayRemove(App &app)
{
  NOTIFYICONDATAW nid = {};
  nid.cbSize = sizeof(nid);
  nid.hWnd = app.hwnd;
  nid.uID = 1;
  Shell_NotifyIconW(NIM_DELETE, &nid);
}

static void TrayBalloon(App &app, const wchar_t *title, const wchar_t *text)
{
  NOTIFYICONDATAW nid = {};
  nid.cbSize = sizeof(nid);
  nid.hWnd = app.hwnd;
  nid.uID = 1;
  nid.uFlags = NIF_INFO;
  nid.dwInfoFlags = NIIF_INFO;
  wcsncpy_s(nid.szInfoTitle, title, _TRUNCATE);
  wcsncpy_s(nid.szInfo, text, _TRUNCATE);
  Shell_NotifyIconW(NIM_MODIFY, &nid);
}

static std::wstring StatusText(App &app)
{
  const size_t total = app.cfg.outputs.size();
  if (total == 0)
    return L"No outputs selected - click to choose";
  Shared *sh = app.pipeline.shared();

  // pre-start message
  if (!sh || !sh->sourceOpen)
    return L"Stopped - waiting for source: " + SpecLabel(app.cfg.source, app.cfg.sourceLabel);

  // running message
  size_t open = 0;
  for (size_t i = 0; i < total; ++i)
    open += sh->outputOpen[i] ? 1 : 0;
  return L"Running - duplicating audio to " + std::to_wstring(open) + L" of " + std::to_wstring(total) +
         (total == 1 ? L" device" : L" devices");
}

static void UpdateTip(App &app)
{
  std::wstring tip = L"dupaudio\n" + StatusText(app);
  if (tip == app.lastTip)
    return;
  
  app.lastTip = tip;

  NOTIFYICONDATAW nid = {};
  nid.cbSize = sizeof(nid);
  nid.hWnd = app.hwnd;
  nid.uID = 1;
  nid.uFlags = NIF_TIP;
  wcsncpy_s(nid.szTip, tip.c_str(), _TRUNCATE);

  // update NotifyIcon with latest data
  Shell_NotifyIconW(NIM_MODIFY, &nid);
}

static void ApplyConfig(App &app)
{
  SaveConfig(app.configPath, app.cfg);
  app.pipeline.Start(app.cfg);
  app.lastTip.clear();
  UpdateTip(app);
}

static void HandleMenuCommand(App &app, UINT cmd)
{
  Config &cfg = app.cfg;
  if (cmd == 0)
    return;

  // "Exit" menu option
  if (cmd == ID_EXIT)
  {
    DestroyWindow(app.hwnd);
    return;
  }

  // "Start with Windows" menu option
  if (cmd == ID_STARTUP)
  {
    SetStartWithWindows(!IsStartWithWindows());
    return;
  }

  // "Open log file" menu option
  if (cmd == ID_OPENLOG)
  {
    ShellExecuteW(nullptr, L"open", g_logPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return;
  }

  // "Windows Sound settings..." menu option
  if (cmd == ID_SOUNDSETTINGS)
  {
    ShellExecuteW(nullptr, L"open", L"ms-settings:sound", nullptr, nullptr, SW_SHOWNORMAL);
    return;
  }

  // "Copy sound from -> Windows default" menu option
  if (cmd == ID_SOURCE_DEFAULT)
  {
    cfg.source = L"default";
    cfg.sourceLabel.clear();
    ApplyConfig(app);
    return;
  }
  // "Copy sound from -> [output device]" menu options
  if (cmd >= ID_SOURCE_BASE && cmd < ID_SOURCE_BASE + app.menuDevices.size())
  {
    const DeviceInfo &d = app.menuDevices[cmd - ID_SOURCE_BASE];
    cfg.source = d.id;
    cfg.sourceLabel = d.name;
    ApplyConfig(app);
    return;
  }
  // "Copy sound to: [output device]" menu options
  if (cmd >= ID_OUTPUT_BASE && cmd < ID_OUTPUT_BASE + app.menuDevices.size())
  {
    const DeviceInfo &d = app.menuDevices[cmd - ID_OUTPUT_BASE];
    bool removed = false;
    for (size_t j = cfg.outputs.size(); j-- > 0;)
    {
      if (SpecMatches(cfg.outputs[j], d))
      {
        cfg.outputs.erase(cfg.outputs.begin() + static_cast<std::ptrdiff_t>(j));
        cfg.outputLabels.erase(cfg.outputLabels.begin() + static_cast<std::ptrdiff_t>(j));
        removed = true;
      }
    }
    if (!removed)
    {
      cfg.outputs.push_back(d.id);
      cfg.outputLabels.push_back(d.name);
    }
    ApplyConfig(app);
    return;
  }
  // missing devices - erase from menu
  if (cmd >= ID_MISSING_BASE && cmd < ID_MISSING_BASE + cfg.outputs.size())
  {
    const size_t j = cmd - ID_MISSING_BASE;
    cfg.outputs.erase(cfg.outputs.begin() + static_cast<std::ptrdiff_t>(j));
    cfg.outputLabels.erase(cfg.outputLabels.begin() + static_cast<std::ptrdiff_t>(j));
    ApplyConfig(app);
    return;
  }
}

static void ShowTrayMenu(App &app)
{
  Com<IMMDeviceEnumerator> en;
  std::wstring defaultId;
  app.menuDevices.clear();
  if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), en.putVoid())))
  {
    app.menuDevices = ListRenderDevices(en.get());
    defaultId = DefaultRenderId(en.get());
  }

  // menu option sorting using ugly lambda function - TODO: be less lazy
  std::sort(app.menuDevices.begin(), app.menuDevices.end(),
    [](const DeviceInfo &a, const DeviceInfo &b)
    {
      return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
    }
  );

  const Config &cfg = app.cfg;
  const bool sourceIsDefault = Lower(cfg.source) == L"default";
  std::wstring sourceId = sourceIsDefault ? defaultId : L"";
  if (!sourceIsDefault)
  {
    for (const auto &d : app.menuDevices)
    {
      if (SpecMatches(cfg.source, d))
      {
        sourceId = d.id;
        break;
      }
    }
  }

  HMENU menu = CreatePopupMenu();
  AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, StatusText(app).c_str());
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"Copy sound to:");

  std::vector<bool> specShown(cfg.outputs.size(), false);
  for (size_t i = 0; i < app.menuDevices.size(); ++i)
  {
    const DeviceInfo &d = app.menuDevices[i];
    const bool isSource = d.id == sourceId;
    bool checked = false;
    for (size_t j = 0; j < cfg.outputs.size(); ++j)
      if (SpecMatches(cfg.outputs[j], d))
      {
        checked = true;
        specShown[j] = true;
      }
    std::wstring text = d.name + (isSource ? L" (source)" : L"");
    UINT flags = MF_STRING | (checked && !isSource ? MF_CHECKED : 0u) | (isSource ? MF_GRAYED : 0u);
    AppendMenuW(menu, flags, ID_OUTPUT_BASE + i, text.c_str());
  }

  for (size_t j = 0; j < cfg.outputs.size(); ++j)
  {
    if (specShown[j])
      continue;
    
    std::wstring text = SpecLabel(cfg.outputs[j], cfg.outputLabels[j]) + L" (not connected)";
    AppendMenuW(menu, MF_STRING | MF_CHECKED, ID_MISSING_BASE + j, text.c_str());
  }

  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  HMENU sourceMenu = CreatePopupMenu();
  AppendMenuW(sourceMenu, MF_STRING | (sourceIsDefault ? MF_CHECKED : 0u), ID_SOURCE_DEFAULT, L"Windows default output");
  AppendMenuW(sourceMenu, MF_SEPARATOR, 0, nullptr);

  for (size_t i = 0; i < app.menuDevices.size(); ++i)
  {
    const bool checked = !sourceIsDefault && app.menuDevices[i].id == sourceId;
    AppendMenuW(sourceMenu, MF_STRING | (checked ? MF_CHECKED : 0u), ID_SOURCE_BASE + i, app.menuDevices[i].name.c_str());
  }

  std::wstring sourceText = L"Copy sound from: " + (sourceIsDefault ? std::wstring(L"Windows default") : SpecLabel(cfg.source, cfg.sourceLabel));
  AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(sourceMenu), sourceText.c_str());
  if (!sourceIsDefault && !sourceId.empty() && sourceId != defaultId)
    AppendMenuW(menu, MF_STRING, ID_SOUNDSETTINGS, L"Note: the source is not the Windows default output - open Sound settings...");

  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING | (IsStartWithWindows() ? MF_CHECKED : 0u), ID_STARTUP, L"Start with Windows");
  AppendMenuW(menu, MF_STRING, ID_SOUNDSETTINGS, L"Windows Sound settings...");
  AppendMenuW(menu, MF_STRING, ID_OPENLOG, L"Open log");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, ID_EXIT, L"Exit");

  POINT pt;
  GetCursorPos(&pt);
  SetForegroundWindow(app.hwnd);    // required so the menu closes when clicking elsewhere
  const UINT cmd = static_cast<UINT>(TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, app.hwnd, nullptr));
  PostMessageW(app.hwnd, WM_NULL, 0, 0);
  DestroyMenu(menu); // also destroys the submenu
  HandleMenuCommand(app, cmd);
}

static LRESULT CALLBACK TrayWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
  App *app = g_app;
  if (app && app->taskbarCreated != 0 && msg == app->taskbarCreated)
  { // Windows' Explorer shell restarted
    TrayAdd(*app);
    UpdateTip(*app);
    return 0;
  }

  // process messages
  switch (msg)
  {
  case WM_TRAYICON:   // open menu on left- or right-click - nothing else to use left-click for after all
    if (app && (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_LBUTTONUP))
      ShowTrayMenu(*app);
    return 0;

  case WM_TIMER:      // update tray icon tooltip
    if (app)
      UpdateTip(*app);
    return 0;

  case WM_DESTROY:    // clean up on exit
    KillTimer(hwnd, kStatusTimer);
    if (app)
      TrayRemove(*app);
    PostQuitMessage(0);
    return 0;

  default:            // unhandled locally - pass to default handler
    return DefWindowProcW(hwnd, msg, wp, lp);
  }
}

// core app and main entrypoint
static bool OpenConsole(bool preferParent)
{
  bool created = false;
  if (!preferParent || !AttachConsole(ATTACH_PARENT_PROCESS))
  {
    AllocConsole();
    created = true;
  }
  FILE *f = nullptr;
  freopen_s(&f, "CONOUT$", "w", stdout);
  freopen_s(&f, "CONOUT$", "w", stderr);
  freopen_s(&f, "CONIN$", "r", stdin);
  return created;
}

static void PauseIfOwnWindow(bool created)
{
  // skip if not our own window (presumably existing console so output will remain visible without pause to hold window opwn)
  if (!created)
    return;

  // pause to hold console open for logging output in case of unexpected exit. rough, but good enough for v1
  wprintf(L"\nPress any key to close.");
  fflush(stdout);
  getwchar();
}

static BOOL WINAPI ConsoleCtrl(DWORD)
{
  if (g_quitEvent)
    SetEvent(g_quitEvent);
  return TRUE;
}

static int ListDevices()
{
  ComInit com;
  Com<IMMDeviceEnumerator> en;
  HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator), en.putVoid());
  if (FAILED(hr))
  {
    wprintf(L"Cannot enumerate audio devices (hr=0x%08lX)\n", static_cast<unsigned long>(hr));
    return 1;
  }
  const std::wstring def = DefaultRenderId(en.get());
  wprintf(L"\nConnected playback devices (* = Windows default):\n\n");
  for (const auto &d : ListRenderDevices(en.get()))
    wprintf(L" %lc %ls\n     %ls\n\n", d.id == def ? L'*' : L' ', d.name.c_str(), d.id.c_str());
  wprintf(L"Use part of a name, or a full ID, in dupaudio.ini.\n");
  return 0;
}

// first run (no config file): pick VB-Cable as the source if it's installed.
static Config DefaultConfig()
{
  Config cfg;
  Com<IMMDeviceEnumerator> en;
  if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                 __uuidof(IMMDeviceEnumerator), en.putVoid())))
    for (const auto &d : ListRenderDevices(en.get()))
      if (Lower(d.name).find(L"cable input") != std::wstring::npos)
      {
        cfg.source = L"CABLE Input";
        break;
      }
  return cfg;
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int)
{
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

  int argc = 0;
  LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  bool list = false, stop = false, console = false;
  std::wstring configPath = ExeDir() + L"\\dupaudio.ini";
  for (int i = 1; argv && i < argc; ++i)
  {
    std::wstring a = Lower(argv[i]);
    if (a == L"--list")
      list = true;
    else if (a == L"--stop")
      stop = true;
    else if (a == L"--console")
      console = true;
    else if (a == L"--config" && i + 1 < argc)
      configPath = argv[++i];
  }
  if (argv)
    LocalFree(argv);

  if (list)
  {
    const bool own = OpenConsole(true);
    const int rc = ListDevices();
    PauseIfOwnWindow(own);    // pause on exit
    return rc;
  }

  if (stop)
  {
    const bool own = OpenConsole(true);
    int rc = 0;
    HANDLE ev = OpenEventW(EVENT_MODIFY_STATE, FALSE, kStopEventName);
    if (!ev)
    {
      wprintf(L"dupaudio is not running.\n");
      rc = 1;
    }
    else
    {
      SetEvent(ev);
      CloseHandle(ev);
      wprintf(L"Stop signal sent.\n");
    }
    PauseIfOwnWindow(own);    // pause on exit
    return rc;
  }

  if (console)
  {
    OpenConsole(false);
    g_logToConsole = true;
  }
  g_logPath = ExeDir() + L"\\dupaudio.log";

  HANDLE instance = CreateMutexW(nullptr, TRUE, kInstanceMutexName);
  if (!instance || GetLastError() == ERROR_ALREADY_EXISTS)
  {
    Log(L"Another instance is already running; exiting.");
    MessageBoxW(nullptr,
                L"dupaudio is already running.\n\nLook for the blue speaker icon in the notification area "
                L"at the right of the taskbar (click ^ if it is hidden).",
                L"dupaudio", MB_ICONINFORMATION);
    if (instance)
      CloseHandle(instance);
    return 1;
  }

  // COM for the UI thread (device listing in the menu). Audio threads use their own MTA.
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

  g_quitEvent = CreateEventW(nullptr, TRUE, FALSE, kStopEventName);
  g_pipeStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!g_quitEvent || !g_pipeStop)
  {
    Log(L"Cannot create events.");
    return 1;
  }
  if (console)
    SetConsoleCtrlHandler(ConsoleCtrl, TRUE);

  auto app = std::make_unique<App>();
  g_app = app.get();
  app->configPath = configPath;

  bool firstRun = false;
  std::wstring err;
  if (GetFileAttributesW(configPath.c_str()) == INVALID_FILE_ATTRIBUTES)
  {
    app->cfg = DefaultConfig();
    SaveConfig(configPath, app->cfg);
    firstRun = true;
    Log(L"Created %ls", configPath.c_str());
  }
  else if (!LoadConfig(configPath, app->cfg, err))
  {
    Log(L"Config error: %ls - starting with no outputs.", err.c_str());
    app->cfg = Config();
  }
  if (app->cfg.outputLabels.size() != app->cfg.outputs.size())
    app->cfg.outputLabels.resize(app->cfg.outputs.size());

  // hidden window that owns the tray icon. (Not message-only, so it receives
  // the "TaskbarCreated" broadcast and can restore the icon if Windows' Explorer shell restarts.)
  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = TrayWndProc;
  wc.hInstance = hInstance;
  wc.lpszClassName = L"dupaudio_tray";
  wc.hIcon = LoadAppIcon(hInstance, GetSystemMetrics(SM_CXICON));
  RegisterClassExW(&wc);
  app->hwnd = CreateWindowExW(0, wc.lpszClassName, L"dupaudio", WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr, hInstance, nullptr);
  if (!app->hwnd)
  {
    Log(L"Cannot create window.");
    return 1;
  }
  app->taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
  app->icon = MakeTrayIcon();
  TrayAdd(*app);

  // start audio duplication threads
  Log(L"dupaudio started.");
  app->pipeline.Start(app->cfg);
  UpdateTip(*app);
  SetTimer(app->hwnd, kStatusTimer, 2000, nullptr);

  // set initial message for first run/empty config states
  if (firstRun || app->cfg.outputs.empty())
    TrayBalloon(*app, L"dupaudio is running", L"Click the speaker icon to choose which headsets get the sound.");

  // message loop that also wakes for "dupaudio --stop".
  bool running = true;
  while (running)
  {
    // check for global stop flag, shut down if active
    const DWORD r = MsgWaitForMultipleObjects(1, &g_quitEvent, FALSE, INFINITE, QS_ALLINPUT);
    if (r == WAIT_OBJECT_0 && IsWindow(app->hwnd))
      DestroyWindow(app->hwnd);

    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
    {
      if (msg.message == WM_QUIT)
      {
        running = false;
        break;
      }
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
  }

  // stop running threads
  Log(L"Stopping...");
  app->pipeline.Stop();
  if (app->icon)
    DestroyIcon(app->icon);
  g_app = nullptr;
  app.reset();
  Log(L"dupaudio stopped.");

  // clean up
  CloseHandle(g_pipeStop);
  CloseHandle(g_quitEvent);
  CoUninitialize();
  ReleaseMutex(instance);
  CloseHandle(instance);

  return 0;
}
