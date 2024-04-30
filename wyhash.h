#include <linux/string.h>
#include <linux/timer.h>

static inline u64 wyhash64_stateless(u64 *seed)
{
    *seed += 0x60bee2bee120fc15;
    u128 tmp;
    tmp = (u128) *seed * 0xa3b195354a39b70d;
    u64 m1 = (tmp >> 64) ^ tmp;
    tmp = (u128) m1 * 0x1b03738712fad5c9;
    u64 m2 = (tmp >> 64) ^ tmp;
    return m2;
}

u64 wyhash64(void)
{
    u64 seed = (u64) ktime_to_ns(ktime_get());
    return wyhash64_stateless(&seed);
}