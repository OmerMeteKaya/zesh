/* Weak stubs for AFL++ runtime symbols.
 * When building with `cargo build`, these resolve the linker references.
 * When building with `cargo afl build`, the real AFL++ runtime overrides them. */

#include <stdint.h>

static uint8_t  _afl_fuzz_buf[1] = {0};
static uint32_t _afl_fuzz_len_val = 0;

__attribute__((weak)) void      __afl_manual_init(void) {}
__attribute__((weak)) int       __afl_persistent_loop(unsigned int count) { (void)count; return 0; }
__attribute__((weak)) uint8_t  *__afl_fuzz_ptr = _afl_fuzz_buf;
__attribute__((weak)) uint32_t *__afl_fuzz_len = &_afl_fuzz_len_val;
