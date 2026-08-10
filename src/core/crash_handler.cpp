#include "core/crash_handler.hpp"
#include "version.hpp"

#include <csignal>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <elf.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/ucontext.h>
#include <time.h>
#include <unistd.h>

namespace {

// Text segment bounds (defined by the linker)
extern "C" {
extern char __executable_start[] __attribute__((weak));
extern char etext[] __attribute__((weak));
}

// Async-signal-safe: write a C string to an fd
static void safe_write(int fd, const char *str) {
  write(fd, str, strlen(str));
}

// Async-signal-safe: write unsigned long as hex
static void safe_write_hex(int fd, unsigned long val) {
  char buf[20];
  char *p = buf + sizeof(buf) - 1;
  *p = '\0';
  if (val == 0) {
    *(--p) = '0';
  } else {
    while (val > 0 && p > buf) {
      unsigned int digit = val & 0xf;
      *(--p) = digit < 10 ? '0' + digit : 'a' + (digit - 10);
      val >>= 4;
    }
  }
  safe_write(fd, p);
}

// Async-signal-safe: write 8-digit zero-padded hex
static void safe_write_hex32(int fd, unsigned long val) {
  char buf[9];
  for (int i = 7; i >= 0; --i) {
    unsigned int digit = val & 0xf;
    buf[i] = digit < 10 ? '0' + digit : 'a' + (digit - 10);
    val >>= 4;
  }
  buf[8] = '\0';
  safe_write(fd, buf);
}

// Async-signal-safe memory probe via pipe write
static bool mem_readable(int pipe_wr_fd, const void *addr, size_t len) {
  if (pipe_wr_fd < 0)
    return false;
  return write(pipe_wr_fd, addr, len) == (ssize_t)len;
}

// Enhanced crash handler with diagnostics
static void crash_signal_handler_extended(int sig, siginfo_t *info,
                                          void *context) {
  constexpr const char *kCrashReportPath = "/tmp/prudynt_crash.log";

  int crash_fd = open(kCrashReportPath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (crash_fd < 0)
    crash_fd = STDERR_FILENO;

  time_t now = time(nullptr);
  char timebuf[64];
  struct tm tm_info;
  localtime_r(&now, &tm_info);
  strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", &tm_info);

  safe_write(crash_fd, "\n=== PRUDYNT CRASH REPORT ===\n");
  safe_write(crash_fd, "Timestamp: ");
  safe_write(crash_fd, timebuf);
  safe_write(crash_fd, "\nVersion: " FULL_VERSION_STRING "\n");
  safe_write(crash_fd, "PID: ");
  safe_write_hex(crash_fd, getpid());
  safe_write(crash_fd, "\n\n");

  safe_write(crash_fd, "Signal: ");
  const char *signame = "UNKNOWN";
  switch (sig) {
  case SIGSEGV: signame = "SIGSEGV (Segmentation fault)"; break;
  case SIGABRT: signame = "SIGABRT (Abort)"; break;
  case SIGILL:  signame = "SIGILL (Illegal instruction)"; break;
  case SIGFPE:  signame = "SIGFPE (Floating point exception)"; break;
  case SIGBUS:  signame = "SIGBUS (Bus error)"; break;
  }
  safe_write(crash_fd, signame);
  safe_write(crash_fd, "\n");

  if (info) {
    safe_write(crash_fd, "Signal code: ");
    safe_write_hex(crash_fd, info->si_code);
    if (sig == SIGILL) {
      safe_write(crash_fd, " (");
      switch (info->si_code) {
      case ILL_ILLOPC: safe_write(crash_fd, "illegal opcode"); break;
      case ILL_ILLOPN: safe_write(crash_fd, "illegal operand"); break;
      case ILL_ILLADR: safe_write(crash_fd, "illegal addressing mode"); break;
      case ILL_ILLTRP: safe_write(crash_fd, "illegal trap"); break;
      case ILL_PRVOPC: safe_write(crash_fd, "privileged opcode"); break;
      case ILL_PRVREG: safe_write(crash_fd, "privileged register"); break;
      case ILL_COPROC: safe_write(crash_fd, "coprocessor error"); break;
      case ILL_BADSTK: safe_write(crash_fd, "internal stack error"); break;
      default: safe_write(crash_fd, "unknown"); break;
      }
      safe_write(crash_fd, ")");
    } else if (sig == SIGSEGV) {
      safe_write(crash_fd, " (");
      switch (info->si_code) {
      case SEGV_MAPERR: safe_write(crash_fd, "address not mapped"); break;
      case SEGV_ACCERR: safe_write(crash_fd, "invalid permissions"); break;
      default: safe_write(crash_fd, "unknown"); break;
      }
      safe_write(crash_fd, ")");
    }
    safe_write(crash_fd, "\nFault address: 0x");
    safe_write_hex(crash_fd, (unsigned long)info->si_addr);
    safe_write(crash_fd, "\n");
  }

  unsigned long sp_val = 0;
  if (context) {
    ucontext_t *uc = (ucontext_t *)context;
    safe_write(crash_fd, "\nRegisters:\n");

#if defined(__mips__)
    safe_write(crash_fd, "PC (Instruction Address): 0x");
    safe_write_hex(crash_fd, uc->uc_mcontext.pc);
    safe_write(crash_fd, "\nSP (Stack Pointer): 0x");
    safe_write_hex(crash_fd, uc->uc_mcontext.gregs[29]);
    safe_write(crash_fd, "\nRA (Return Address): 0x");
    safe_write_hex(crash_fd, uc->uc_mcontext.gregs[31]);
    safe_write(crash_fd, "\n");

    static const char *const mips_reg_names[32] = {
        "zero","at","v0","v1","a0","a1","a2","a3",
        "t0","t1","t2","t3","t4","t5","t6","t7",
        "s0","s1","s2","s3","s4","s5","s6","s7",
        "t8","t9","k0","k1","gp","sp","fp","ra"};
    safe_write(crash_fd, "\nGPRs:\n");
    for (int i = 0; i < 32; i++) {
      safe_write(crash_fd, "  ");
      safe_write(crash_fd, mips_reg_names[i]);
      for (size_t pad = strlen(mips_reg_names[i]); pad < 4; pad++)
        safe_write(crash_fd, " ");
      safe_write(crash_fd, "=");
      safe_write_hex32(crash_fd, (unsigned long)uc->uc_mcontext.gregs[i]);
      if ((i & 3) == 3)
        safe_write(crash_fd, "\n");
    }
    sp_val = (unsigned long)uc->uc_mcontext.gregs[29];
#elif defined(__arm__)
    safe_write(crash_fd, "PC: 0x");
    safe_write_hex(crash_fd, uc->uc_mcontext.arm_pc);
    safe_write(crash_fd, "\nSP: 0x");
    safe_write_hex(crash_fd, uc->uc_mcontext.arm_sp);
    safe_write(crash_fd, "\nLR: 0x");
    safe_write_hex(crash_fd, uc->uc_mcontext.arm_lr);
    safe_write(crash_fd, "\n");
    sp_val = (unsigned long)uc->uc_mcontext.arm_sp;
#else
    safe_write(crash_fd, "(register dump not available for this architecture)\n");
#endif
  }

  // Stack dump + return-address scan
  if (sp_val && (sp_val & 3) == 0) {
    int probe_fds[2] = {-1, -1};
    if (pipe(probe_fds) != 0)
      probe_fds[0] = probe_fds[1] = -1;
    const unsigned long *sp_words = (const unsigned long *)sp_val;

    safe_write(crash_fd, "\nStack (256 bytes from SP):\n");
    for (int line = 0; line < 16; line++) {
      const unsigned long *row = sp_words + line * 4;
      if (!mem_readable(probe_fds[1], row, 4 * sizeof(*row))) {
        safe_write(crash_fd, "  <unreadable>\n");
        break;
      }
      safe_write(crash_fd, "  ");
      safe_write_hex32(crash_fd, (unsigned long)row);
      safe_write(crash_fd, ":");
      for (int k = 0; k < 4; k++) {
        safe_write(crash_fd, " ");
        safe_write_hex32(crash_fd, row[k]);
      }
      safe_write(crash_fd, "\n");
    }

    unsigned long text_lo = (unsigned long)__executable_start;
    unsigned long text_hi = (unsigned long)etext;
    if (text_lo && text_hi > text_lo) {
      safe_write(crash_fd, "\nText addresses on stack (2KB scan):\n");
      for (int i = 0; i < 512; i++) {
        const unsigned long *w = sp_words + i;
        if (!mem_readable(probe_fds[1], w, sizeof(*w)))
          break;
        if (*w >= text_lo && *w < text_hi) {
          safe_write(crash_fd, "  SP+0x");
          safe_write_hex(crash_fd, i * sizeof(*w));
          safe_write(crash_fd, ": 0x");
          safe_write_hex32(crash_fd, *w);
          safe_write(crash_fd, "\n");
        }
      }
    }
    if (probe_fds[0] >= 0) {
      close(probe_fds[0]);
      close(probe_fds[1]);
    }
  }

#if defined(HAS_BACKTRACE) && HAS_BACKTRACE
  safe_write(crash_fd, "\nBacktrace:\n");
  void *backtrace_buffer[64];
  int frame_count = backtrace(backtrace_buffer, 64);
  backtrace_symbols_fd(backtrace_buffer, frame_count, crash_fd);

  safe_write(crash_fd, "\nDetailed backtrace:\n");
  for (int i = 0; i < frame_count; i++) {
    Dl_info dlinfo;
    if (dladdr(backtrace_buffer[i], &dlinfo)) {
      safe_write(crash_fd, "#");
      safe_write_hex(crash_fd, i);
      safe_write(crash_fd, " 0x");
      safe_write_hex(crash_fd, (unsigned long)backtrace_buffer[i]);
      safe_write(crash_fd, " in ");
      safe_write(crash_fd, dlinfo.dli_sname ? dlinfo.dli_sname : "???");
      safe_write(crash_fd, " from ");
      safe_write(crash_fd, dlinfo.dli_fname ? dlinfo.dli_fname : "???");
      safe_write(crash_fd, "\n");
    }
  }
#endif

  safe_write(crash_fd, "\n=== END CRASH REPORT ===\n");

  if (crash_fd != STDERR_FILENO) {
    safe_write(STDERR_FILENO, "\nPrudynt crashed! Crash report saved to ");
    safe_write(STDERR_FILENO, kCrashReportPath);
    safe_write(STDERR_FILENO, "\nSignal: ");
    safe_write(STDERR_FILENO, signame);
    if (info) {
      safe_write(STDERR_FILENO, " at address 0x");
      safe_write_hex(STDERR_FILENO, (unsigned long)info->si_addr);
    }
    safe_write(STDERR_FILENO, "\n");
    close(crash_fd);
  }

  release_instance_lock();
  signal(sig, SIG_DFL);
  raise(sig);
}

} // anonymous namespace

void install_crash_handler() {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = crash_signal_handler_extended;
  sa.sa_flags = SA_SIGINFO;
  sigaction(SIGSEGV, &sa, nullptr);
  sigaction(SIGABRT, &sa, nullptr);
  sigaction(SIGILL, &sa, nullptr);
  sigaction(SIGFPE, &sa, nullptr);
  sigaction(SIGBUS, &sa, nullptr);
}
