#include "process_symbols.hpp"

#include <neko/log.hpp>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// dia2.h pulls in the full COM surface; keep it behind the pimpl and out of
// every consumer, including the unit tests.
#include <dia2.h>

#include "msdia_candidate.hpp"

namespace neko::pe {
namespace {

template <typename T>
struct com_ptr {
  T* p = nullptr;
  ~com_ptr() {
    if (p != nullptr) {
      p->Release();
    }
  }
  com_ptr() = default;
  com_ptr(const com_ptr&) = delete;
  com_ptr& operator=(const com_ptr&) = delete;
  T** operator&() { return &p; }
  T* operator->() const { return p; }
  T* get() const { return p; }
  explicit operator bool() const { return p != nullptr; }
};

std::wstring widen(std::string_view text) {
  const int chars =
      MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
  std::wstring out(static_cast<std::size_t>(chars), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), chars);
  return out;
}

std::string narrow(BSTR text) {
  if (text == nullptr) {
    return {};
  }
  const int bytes = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
  std::string out(static_cast<std::size_t>(bytes - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), bytes, nullptr, nullptr);
  return out;
}

std::string describe(const wchar_t* text) {
  if (text == nullptr) {
    return {};
  }
  const int bytes = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
  std::string out(static_cast<std::size_t>(bytes - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), bytes, nullptr, nullptr);
  return out;
}

std::string hex(HRESULT hr) {
  char buffer[16]{};
  std::snprintf(buffer, sizeof(buffer), "0x%08lX", static_cast<unsigned long>(hr));
  return buffer;
}

// {E6756135-1E65-4D17-8576-610761398C3C} = CLSID_DiaSource in this SDK's
// diaguids.lib; load msdia140.dll registration-free through its class
// object, because a clean VS 2022 install does not register the DLL.
constexpr CLSID clsid_dia_source = {
    0xE6756135, 0x1E65, 0x4D17, {0x85, 0x76, 0x61, 0x07, 0x61, 0x39, 0x8C, 0x3C}};

using get_class_object_t = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, LPVOID*);

bool source_from_module(const wchar_t* path, IDiaDataSource** out) {
  HMODULE module = LoadLibraryW(path);
  if (module == nullptr) {
    return false;
  }
  // The module stays loaded for the process lifetime: released COM objects
  // may still hold its classes. Two-step cast: clang-cl rightly refuses a
  // direct FARPROC-to-signature reinterpretation.
  const auto get_class_object = reinterpret_cast<get_class_object_t>(
      reinterpret_cast<void*>(GetProcAddress(module, "DllGetClassObject")));
  if (get_class_object == nullptr) {
    return false;
  }
  com_ptr<IClassFactory> factory;
  if (FAILED(get_class_object(clsid_dia_source, __uuidof(IClassFactory),
                              reinterpret_cast<void**>(&factory)))) {
    return false;
  }
  return SUCCEEDED(
      factory->CreateInstance(nullptr, __uuidof(IDiaDataSource), reinterpret_cast<void**>(out)));
}

// Candidate order: the NEKOMATA_MSDIA_DLL override, the path discovered at
// configure time (the VS installation of the building compiler, on any
// drive), the COM registration a fuller setup may hold, and finally the
// plain loader path.
IDiaDataSource* create_dia_source() {
  wchar_t override_path[MAX_PATH]{};
  if (GetEnvironmentVariableW(L"NEKOMATA_MSDIA_DLL", override_path, MAX_PATH) > 0) {
    IDiaDataSource* source = nullptr;
    if (source_from_module(override_path, &source)) {
      return source;
    }
    neko::log(neko::log_level::warn,
              "NEKOMATA_MSDIA_DLL points at an unusable library (%s); falling back\n",
              describe(override_path).c_str());
  }

  if (const wchar_t* candidate = L"" NEKO_MSDIA_CANDIDATE; *candidate != L'\0') {
    IDiaDataSource* source = nullptr;
    if (source_from_module(candidate, &source)) {
      return source;
    }
    neko::log(neko::log_level::warn,
              "the configured msdia candidate (%s) failed to load; falling back\n",
              describe(candidate).c_str());
  }

  IDiaDataSource* source = nullptr;
  if (SUCCEEDED(CoCreateInstance(clsid_dia_source, nullptr, CLSCTX_INPROC_SERVER,
                                 __uuidof(IDiaDataSource), reinterpret_cast<void**>(&source)))) {
    return source;
  }
  if (source_from_module(L"msdia140.dll", &source)) {
    return source;
  }
  throw std::runtime_error(
      "no usable DIA source: install Visual Studio (msdia140.dll) or set NEKOMATA_MSDIA_DLL");
}

} // namespace

struct process_symbols::state {
  IDiaSession* session = nullptr;
  IDiaSymbol* global = nullptr;
  std::uintptr_t image_base = 0;
};

process_symbols::process_symbols() : state_(std::make_unique<state>()) {
  wchar_t executable[MAX_PATH]{};
  const DWORD length = GetModuleFileNameW(nullptr, executable, MAX_PATH);
  if (length == 0 || length >= MAX_PATH) {
    throw std::runtime_error("could not resolve the current executable path");
  }

  IDiaDataSource* raw_source = create_dia_source();
  const HRESULT loaded = raw_source->loadDataForExe(executable, nullptr, nullptr);
  if (FAILED(loaded)) {
    raw_source->Release();
    throw std::runtime_error("could not load symbols for " + describe(executable) + " (" +
                             hex(loaded) +
                             "); build the host with /DEBUG and keep its PDB beside the "
                             "executable");
  }
  IDiaSession* session = nullptr;
  const HRESULT opened = raw_source->openSession(&session);
  raw_source->Release();
  if (FAILED(opened) || session == nullptr) {
    throw std::runtime_error("could not open the DIA session (" + hex(opened) + ")");
  }
  IDiaSymbol* global = nullptr;
  if (FAILED(session->get_globalScope(&global)) || global == nullptr) {
    session->Release();
    throw std::runtime_error("the DIA session has no global scope");
  }

  state_->session = session;
  state_->global = global;
  state_->image_base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
}

process_symbols::~process_symbols() {
  if (state_ != nullptr) {
    if (state_->global != nullptr) {
      state_->global->Release();
    }
    if (state_->session != nullptr) {
      state_->session->Release();
    }
  }
}

namespace {

struct site {
  std::uintptr_t rva;
  std::uint64_t size;
};

// Exact, case-sensitive matches across the two streams that can define a
// name, deduplicated by RVA: a public and its record share one address and
// count as one symbol. The preferred tag's extent wins.
std::vector<site> find_sites(IDiaSymbol* global, const std::wstring& name,
                             enum SymTagEnum preferred, enum SymTagEnum fallback) {
  std::vector<site> sites;
  const enum SymTagEnum tags[] = {preferred, fallback};
  for (enum SymTagEnum tag : tags) {
    com_ptr<IDiaEnumSymbols> matches;
    if (FAILED(global->findChildren(tag, name.c_str(), nsCaseSensitive, &matches)) || !matches) {
      continue;
    }
    IDiaSymbol* symbol = nullptr;
    ULONG fetched = 0;
    while (matches->Next(1, &symbol, &fetched) == S_OK && fetched == 1) {
      DWORD rva = 0;
      ULONGLONG length = 0;
      const bool located = SUCCEEDED(symbol->get_relativeVirtualAddress(&rva)) && rva != 0;
      const bool sized = SUCCEEDED(symbol->get_length(&length));
      symbol->Release();
      if (!located) {
        continue;
      }
      const auto existing =
          std::find_if(sites.begin(), sites.end(), [&](const site& s) { return s.rva == rva; });
      if (existing != sites.end()) {
        continue;
      }
      sites.push_back({rva, sized ? length : 0});
    }
  }
  return sites;
}

} // namespace

std::vector<backend::function_info> process_symbols::all_functions() const {
  std::vector<backend::function_info> functions;
  com_ptr<IDiaEnumSymbols> children;
  if (FAILED(state_->global->findChildren(SymTagFunction, nullptr, nsNone, &children)) ||
      !children) {
    return functions;
  }
  IDiaSymbol* symbol = nullptr;
  ULONG fetched = 0;
  while (children->Next(1, &symbol, &fetched) == S_OK && fetched == 1) {
    BSTR name = nullptr;
    DWORD rva = 0;
    ULONGLONG length = 0;
    if (SUCCEEDED(symbol->get_name(&name)) && SUCCEEDED(symbol->get_relativeVirtualAddress(&rva)) &&
        rva != 0) {
      symbol->get_length(&length);
      backend::function_info info;
      info.name = narrow(name);
      info.address = state_->image_base + rva;
      info.size = static_cast<std::size_t>(length);
      functions.push_back(std::move(info));
    }
    SysFreeString(name);
    symbol->Release();
  }
  return functions;
}

std::optional<backend::function_info>
process_symbols::function_by_name(std::string_view name) const {
  const auto sites = find_sites(state_->global, widen(name), SymTagFunction, SymTagPublicSymbol);
  if (sites.empty()) {
    return std::nullopt;
  }
  backend::function_info info;
  info.name = std::string(name);
  info.address = state_->image_base + sites.front().rva;
  info.size = static_cast<std::size_t>(sites.front().size);
  return info;
}

std::size_t process_symbols::count_functions(std::string_view name) const {
  return find_sites(state_->global, widen(name), SymTagFunction, SymTagPublicSymbol).size();
}

std::optional<backend::global_variable>
process_symbols::global_by_name(std::string_view name) const {
  const auto sites = find_sites(state_->global, widen(name), SymTagData, SymTagPublicSymbol);
  if (sites.empty()) {
    return std::nullopt;
  }
  backend::global_variable out;
  out.name = std::string(name);
  out.address = state_->image_base + sites.front().rva;
  out.size = static_cast<std::size_t>(sites.front().size);
  return out;
}

std::size_t process_symbols::count_globals(std::string_view name) const {
  return find_sites(state_->global, widen(name), SymTagData, SymTagPublicSymbol).size();
}

backend::type_layout process_symbols::layout_of(backend::type_id id) const {
  (void)id; // object layout migration is planned; nothing to report yet
  return {};
}

void* process_symbols::map_global(std::string_view name) {
  const auto global = global_by_name(name);
  if (!global) {
    return nullptr;
  }
  return reinterpret_cast<void*>(global->address);
}

} // namespace neko::pe
