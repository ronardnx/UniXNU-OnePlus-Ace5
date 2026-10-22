/* Shared kernel/daemon ABI. Fixed-width fields; no pointers. */
#ifndef GIULIA_DEBUG_ABI_H
#define GIULIA_DEBUG_ABI_H
#include <sys/ioccom.h>
#include <stdint.h>
struct giulia_shell_state { uint32_t active, generation, columns, rows; };
#define GIULIA_SHELL_STATE _IOR('G', 1, struct giulia_shell_state)
#define GIULIA_SHELL_END _IO('G', 2)
#define GIULIA_SHELL_ESCAPE 0x1d
#define GIULIA_SHELL_BEGIN "\r\nGIULIA SHELL BEGIN\r\n"
#define GIULIA_SHELL_FINISH "\r\nGIULIA SHELL END\r\n"
#endif
