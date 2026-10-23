/* Worker-owned diagnostic cache. Each field is atomic; the bounded query is
 * approximate across fields and never performs an SPMI transaction. */
static struct {
    unsigned state, version, pon_apid, gpio_apid, status, pon, gpio;
    unsigned raw, stable, delivered, samples, events, retries, errors;
    int enabled, error, thread_error;
} gkeys_status = { .pon_apid = 1024, .gpio_apid = 1024, .enabled = 1 };
#define GK_STATUS_SET(field, value) __atomic_store_n(&gkeys_status.field, (value), __ATOMIC_RELAXED)
#define GK_STATUS_GET(field) __atomic_load_n(&gkeys_status.field, __ATOMIC_RELAXED)
#define GK_STATUS_INC(field) ((void)__atomic_fetch_add(&gkeys_status.field, 1u, __ATOMIC_RELAXED))

void giulia_keys_debug(void);
void giulia_keys_debug(void)
{
    unsigned ready = giulia_remote_ready();
    printf("GIULIA KEYS: compiled=1 state=%u giulia_keys=%d buttons_service=%u remote_touch=%u remote_keyboard=%u remote_buttons=%u cache=approximate\n",
        GK_STATUS_GET(state), GK_STATUS_GET(enabled), !!(ready & 4),
        !!(ready & 1), !!(ready & 2), !!(ready & 4));
    printf("GIULIA KEYS: version=%#x pon_apid=%u gpio_apid=%u status=%#x error=%d thread_error=%d errors=%u\n",
        GK_STATUS_GET(version), GK_STATUS_GET(pon_apid), GK_STATUS_GET(gpio_apid),
        GK_STATUS_GET(status), GK_STATUS_GET(error), GK_STATUS_GET(thread_error), GK_STATUS_GET(errors));
    printf("GIULIA KEYS: samples=%u pon=%#x gpio=%#x raw=%#x stable=%#x delivered=%#x events=%u retries=%u debounce_ms=40 poll_ms=10\n",
        GK_STATUS_GET(samples), GK_STATUS_GET(pon), GK_STATUS_GET(gpio),
        GK_STATUS_GET(raw), GK_STATUS_GET(stable), GK_STATUS_GET(delivered),
        GK_STATUS_GET(events), GK_STATUS_GET(retries));
    printf("GIULIA KEYS: state_names=0:not-started,1:starting,2:polling,3:disabled,4:releasing,5:stopped,6:thread-failed\n");
}
