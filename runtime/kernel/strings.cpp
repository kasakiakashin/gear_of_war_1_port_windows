// strings.cpp — printf do convidado: sprintf, _vsnprintf e DbgPrint (xboxkrnl), com varargs do ABI do Xbox 360.
//
// Varargs: cada argumento ocupa um slot de 64 bits. Chamada direta: r3..r10 e depois a pilha do chamador
// (r1 + 0x50 + 8 × (i − 8)); os double variádicos vão nos GPR. va_list é um ponteiro para slots de 8 bytes BE.
// Formatos da CRT da Microsoft: %S / %ls / %ws = texto UTF-16, %hs = ANSI, I64 / ll = 64 bits, %p = 8 dígitos.
#include <algorithm>
#include <bit>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "kernel/kernel.h"
#include "xam/xam.h"

using namespace kernel;

namespace {

struct Args {
  const PPCContext& ctx;
  uint8_t* m;
  int index = 0;      // próximo argumento (chamada direta)
  uint32_t va = 0;    // ou ponteiro de va_list
  uint64_t next() {
    if (va) {
      uint64_t v = ppc_ld64(m, va);
      va += 8;
      return v;
    }
    int i = index++;
    return i < 8 ? ctx.r[3 + i] : ppc_ld64(m, (uint32_t)ctx.r[1] + 0x50 + 8 * (i - 8));
  }
};

template <typename T>
void append(std::string& out, const std::string& spec, T value) {
  int n = std::snprintf(nullptr, 0, spec.c_str(), value);
  if (n <= 0) return;
  std::vector<char> buf((size_t)n + 1);
  std::snprintf(buf.data(), buf.size(), spec.c_str(), value);
  out.append(buf.data(), (size_t)n);
}

std::string format(uint8_t* m, uint32_t fmt_ptr, Args& a) {
  std::string out;
  if (!fmt_ptr) return out;
  const char* f = reinterpret_cast<const char*>(PPC_HOST(m, fmt_ptr));
  while (*f) {
    if (*f != '%') {
      out += *f++;
      continue;
    }
    const char* start = f++;
    if (*f == '%') {
      out += '%', f++;
      continue;
    }
    std::string spec = "%";
    while (*f && std::strchr("-+ #0", *f)) spec += *f++;
    if (*f == '*') {
      spec += std::to_string((int32_t)a.next()), f++;
    } else {
      while (std::isdigit((unsigned char)*f)) spec += *f++;
    }
    if (*f == '.') {
      spec += *f++;
      if (*f == '*') spec += std::to_string((int32_t)a.next()), f++;
      else while (std::isdigit((unsigned char)*f)) spec += *f++;
    }
    bool is64 = false, half = false, wide = false, narrow = false;
    for (;;) {
      if (*f == 'h') half = narrow = true, f++;
      else if (*f == 'l' && f[1] == 'l') is64 = true, f += 2;
      else if (*f == 'l' || *f == 'w') wide = true, f++;
      else if (*f == 'L' || *f == 'q' || *f == 'j') is64 = true, f++;
      else if (*f == 'I' && f[1] == '6' && f[2] == '4') is64 = true, f += 3;
      else if (*f == 'I' && f[1] == '3' && f[2] == '2') f += 3;
      else if (*f == 'I' || *f == 'z' || *f == 't') f++;
      else break;
    }
    char c = *f ? *f++ : 0;
    switch (c) {
      case 'd':
      case 'i': {
        uint64_t v = a.next();
        int64_t s = is64 ? (int64_t)v : half ? (int64_t)(int16_t)v : (int64_t)(int32_t)v;
        append(out, spec + "lld", (long long)s);
        break;
      }
      case 'u':
      case 'x':
      case 'X':
      case 'o': {
        uint64_t v = a.next();
        if (!is64) v = half ? (uint16_t)v : (uint32_t)v;
        append(out, spec + "ll" + c, (unsigned long long)v);
        break;
      }
      case 'c':
      case 'C': {
        uint32_t ch = (uint32_t)a.next() & ((c == 'C' && !narrow) || wide ? 0xFFFF : 0xFF);
        append(out, spec + "c", (int)(ch < 0x100 ? ch : '?'));
        break;
      }
      case 's':
      case 'S': {
        uint32_t p = (uint32_t)a.next();
        bool w = (c == 'S' && !narrow) || wide;
        std::string s = !p ? "(null)" : w ? xam::read_utf16(m, p) : std::string(reinterpret_cast<const char*>(PPC_HOST(m, p)));
        append(out, spec + "s", s.c_str());
        break;
      }
      case 'p':
        append(out, "%08X", (unsigned)(uint32_t)a.next());
        break;
      case 'f':
      case 'F':
      case 'e':
      case 'E':
      case 'g':
      case 'G':
      case 'a':
      case 'A':
        append(out, spec + c, std::bit_cast<double>(a.next()));
        break;
      case 'n':
        out32(m, (uint32_t)a.next(), (uint32_t)out.size());
        break;
      default:  // conversão desconhecida: copia literalmente
        out.append(start, f);
        break;
    }
  }
  return out;
}

void store(uint8_t* m, uint32_t buf, const std::string& s, size_t limit) {
  size_t n = std::min(s.size(), limit);
  for (size_t i = 0; i < n; i++) ppc_st8(m, buf + (uint32_t)i, (uint8_t)s[i]);
  if (n < limit) ppc_st8(m, buf + (uint32_t)n, 0);
}

}  // namespace

// int sprintf(char* Buffer, const char* Format, ...)
XBOX_IMPORT(sprintf) {
  Args a{ctx, mem_base, 2};
  std::string s = format(mem_base, arg(ctx, 1), a);
  store(mem_base, arg(ctx, 0), s, s.size() + 1);
  ret(ctx, (uint64_t)(int64_t)(int32_t)s.size());
}

// int _vsnprintf(char* Buffer, size_t Count, const char* Format, va_list Args): -1 se não couber
// (sem terminador quando o texto ocupa exatamente Count).
XBOX_IMPORT(_vsnprintf) {
  uint32_t buf = arg(ctx, 0), count = arg(ctx, 1);
  Args a{ctx, mem_base, 0, arg(ctx, 3)};
  std::string s = format(mem_base, arg(ctx, 2), a);
  if (buf) store(mem_base, buf, s, count);
  ret_status(ctx, s.size() <= count ? (uint32_t)s.size() : 0xFFFFFFFFu);
}

// ULONG DbgPrint(PCSTR Format, ...)
XBOX_IMPORT(DbgPrint) {
  Args a{ctx, mem_base, 1};
  std::string s = format(mem_base, arg(ctx, 0), a);
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
  std::fprintf(stderr, "[dbg] %s\n", s.c_str());
  ret_status(ctx, kStatusSuccess);
}
