#ifndef CRASH_HANDLER_HPP
#define CRASH_HANDLER_HPP

// Install the extended crash signal handler for SIGSEGV, SIGABRT, SIGILL,
// SIGFPE, and SIGBUS. Call once at startup.
void install_crash_handler();

// Release the instance lock file. Safe to call from signal handlers.
void release_instance_lock();

#endif
