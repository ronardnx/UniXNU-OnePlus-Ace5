/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Resident protocol mailbox. No MMIO, DMA, provider or driver lifecycle API.
 */
#include <sys/systm.h>
#include <sys/errno.h>
#include "../common/giulia_hotload.h"
#include "native_hotload.h"

#if !GIULIA_HOTLOAD_ENABLED
#error "The resident bridge must be explicitly enabled by hotload composition"
#endif
#ifndef GIULIA_HOTLOAD_SOURCE_ID
#define GIULIA_HOTLOAD_SOURCE_ID "unbound"
#endif

_Static_assert(GH_MAX_WORDS == 32u, "mailbox completion bitmap");
_Static_assert(__atomic_always_lock_free(sizeof(unsigned), 0), "mailbox admission");
_Static_assert(__atomic_always_lock_free(sizeof(uint64_t), 0), "mailbox identifiers");

const char giulia_hotload_build[] =
    "NLH gateway source_sha256=" GIULIA_HOTLOAD_SOURCE_ID;
static const char gh_source_id[] = GIULIA_HOTLOAD_SOURCE_ID;
static const char gh_names[GH_DOMAIN_COUNT][32] = {
    GH_NAME_GPU, GH_NAME_WIFI, GH_NAME_AUDIO, GH_NAME_MODEM
};

enum { GH_INIT_NEW, GH_INIT_BUSY, GH_INIT_READY, GH_INIT_FAILED };
/* Prefix cannot be mistaken for a canonical ARM64 kernel VA. Ticket numbers
 * are still opaque: no code casts them to pointers or interprets their bits.
 * Exhaustion is permanent; a ticket is never recycled, even on a failed call.
 */
#define GH_TICKET_TAG 0x4748000000000000ull
#define GH_TICKET_LIMIT 0x0000ffffffffffffull
#define GH_TICKET_BUSY 0x8000000000000000ull
static unsigned gh_init_state;
static int gh_init_error = ENXIO;
static uint64_t gh_ticket_sequence, gh_invalid_requests, gh_denied_platform;

struct gh_counters {
    uint64_t attempts, dispatches, executed, calls, fallbacks, rejected;
    uint64_t busy, stale, unavailable, module_errors, protocol_errors;
    uint64_t platform_denied, request_reads, response_writes;
    uint64_t image_cookie, activation_epoch, invocation_id;
    uint32_t image_version, slot_generation;
    int last_error;
};
static struct gh_mailbox {
    unsigned admission, poisoned;
    uint64_t ticket, high_epoch;
    /* Private while admission=1; a ticket CAS additionally excludes overlap
     * with retirement and stale callbacks after mailbox reuse. Status reads
     * only the atomic fields/counters above.
     */
    uint64_t epoch;
    uint32_t operation, request_words, response_words;
    uint64_t request[GH_MAX_WORDS], response[GH_MAX_WORDS];
    uint32_t read_mask, write_mask;
    unsigned bound;
    int fault;
    struct nlh_module_identity metadata;
    uint64_t image_cookie, activation_epoch, invocation_id;
    uint32_t slot_generation;
    struct gh_counters stats;
} gh_mailboxes[GH_DOMAIN_COUNT];

#define GH_INC(value) __atomic_fetch_add(&(value), 1, __ATOMIC_RELAXED)
#define GH_LOAD(value) __atomic_load_n(&(value), __ATOMIC_RELAXED)

static int gh_source_valid(void)
{
    if (sizeof(gh_source_id) != 65u) return 0;
    for (unsigned i = 0; i + 1 < sizeof(gh_source_id); i++) {
        char c = gh_source_id[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return 0;
    }
    return 1;
}

static int gh_fallback(struct gh_mailbox *box, int error, int rejected)
{
    GH_INC(box->stats.fallbacks);
    if (rejected) GH_INC(box->stats.rejected);
    if (error == EBUSY) GH_INC(box->stats.busy);
    if (error == ESTALE) GH_INC(box->stats.stale);
    if (error == ENOENT || error == ENXIO || error == ENOTSUP)
        GH_INC(box->stats.unavailable);
    __atomic_store_n(&box->stats.last_error, error, __ATOMIC_RELAXED);
    return error;
}

static int gh_deny(struct gh_mailbox *box, int error, int taint)
{
    GH_INC(gh_denied_platform);
    if (box) {
        GH_INC(box->stats.platform_denied);
        if (taint) {
            int expected = 0;
            __atomic_compare_exchange_n(&box->fault, &expected, error, 0,
                __ATOMIC_RELAXED, __ATOMIC_RELAXED);
        }
    }
    return error;
}

static int gh_platform(const struct nlh_invocation *ctx, uint32_t op,
    uint64_t a, uint64_t b, uint64_t *out)
{
    /* ctx is an authenticated runtime-owned copy, never module a/b memory.
     * root_origin alone grants nothing; dependency and lifecycle calls fail.
     */
    if (!ctx || ctx->abi != NLH_PLATFORM_ABI || ctx->bytes != sizeof(*ctx) ||
        ctx->origin != NLH_ORIGIN_RESIDENT || ctx->root_origin != NLH_ORIGIN_RESIDENT ||
        ctx->depth || ctx->phase != NLH_CALL || ctx->role != NLH_ROLE_ACTIVE ||
        !ctx->invocation_id || ctx->root_invocation_id != ctx->invocation_id)
        return gh_deny(NULL, EACCES, 0);

    unsigned index = 0;
    for (; index < GH_DOMAIN_COUNT; index++)
        if (!memcmp(ctx->metadata.name, gh_names[index], sizeof(ctx->metadata.name))) break;
    if (index == GH_DOMAIN_COUNT) return gh_deny(NULL, ENOTSUP, 0);
    struct gh_mailbox *box = &gh_mailboxes[index];
    uint64_t ticket = ctx->argument;
    if (!ticket || (ticket & GH_TICKET_BUSY)) return gh_deny(box, ESTALE, 0);
    uint64_t expected = ticket;
    if (!__atomic_compare_exchange_n(&box->ticket, &expected, ticket | GH_TICKET_BUSY,
            0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
        return gh_deny(box, expected == (ticket | GH_TICKET_BUSY) ? EBUSY : ESTALE,
            expected == (ticket | GH_TICKET_BUSY));
    int error = 0;
    if (a != ticket) {
        error = ESTALE;
    } else if (!out) {
        error = EINVAL;
    } else if (ctx->metadata.interface_abi != GH_INTERFACE_ABI ||
        ctx->metadata.state_abi != GH_STATE_ABI || ctx->metadata.kind != NLH_SERVICE ||
        ctx->metadata.dependency_count || !ctx->metadata.version || !ctx->image_cookie ||
        !ctx->activation_epoch || !ctx->slot_generation) {
        error = EPROGMISMATCH;
    } else {
        if (!box->bound) {
            box->metadata = ctx->metadata;
            box->image_cookie = ctx->image_cookie;
            box->activation_epoch = ctx->activation_epoch;
            box->invocation_id = ctx->invocation_id;
            box->slot_generation = ctx->slot_generation;
            box->bound = 1;
            GH_INC(box->stats.executed);
            __atomic_store_n(&box->stats.image_cookie, ctx->image_cookie, __ATOMIC_RELAXED);
            __atomic_store_n(&box->stats.activation_epoch, ctx->activation_epoch, __ATOMIC_RELAXED);
            __atomic_store_n(&box->stats.invocation_id, ctx->invocation_id, __ATOMIC_RELAXED);
            __atomic_store_n(&box->stats.image_version, ctx->metadata.version, __ATOMIC_RELAXED);
            __atomic_store_n(&box->stats.slot_generation, ctx->slot_generation, __ATOMIC_RELAXED);
        } else if (memcmp(&box->metadata, &ctx->metadata, sizeof(box->metadata)) ||
            box->image_cookie != ctx->image_cookie || box->activation_epoch != ctx->activation_epoch ||
            box->invocation_id != ctx->invocation_id || box->slot_generation != ctx->slot_generation) {
            error = ESTALE;
        }
        if (!error) {
            if (op == GH_PLATFORM_GET_META && b < GH_META_COUNT) {
                switch ((uint32_t)b) {
                case GH_META_DOMAIN: *out = index + 1; break;
                case GH_META_EPOCH: *out = box->epoch; break;
                case GH_META_OPERATION: *out = box->operation; break;
                case GH_META_REQUEST_WORDS: *out = box->request_words; break;
                case GH_META_RESPONSE_WORDS: *out = box->response_words; break;
                case GH_META_ABI: *out = GH_ABI; break;
                }
            } else if (op >= GH_PLATFORM_READ_REQUEST_BASE &&
                op - GH_PLATFORM_READ_REQUEST_BASE < box->request_words && !b) {
                unsigned word = op - GH_PLATFORM_READ_REQUEST_BASE;
                *out = box->request[word];
                box->read_mask |= 1u << word;
                GH_INC(box->stats.request_reads);
            } else if (op >= GH_PLATFORM_WRITE_RESPONSE_BASE &&
                op - GH_PLATFORM_WRITE_RESPONSE_BASE < box->response_words) {
                unsigned word = op - GH_PLATFORM_WRITE_RESPONSE_BASE;
                box->response[word] = b;
                box->write_mask |= 1u << word;
                *out = 0;
                GH_INC(box->stats.response_writes);
            } else {
                error = EINVAL;
            }
        }
    }
    if (error) gh_deny(box, error, 1);
    expected = ticket | GH_TICKET_BUSY;
    /* Retirement of an escaped callback keeps the mailbox closed. Never
     * resurrect a ticket that the resident caller has already invalidated.
     */
    __atomic_compare_exchange_n(&box->ticket, &expected, ticket,
        0, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
    return error;
}

int giulia_hotload_init(void)
{
    unsigned expected = GH_INIT_NEW;
    if (!__atomic_compare_exchange_n(&gh_init_state, &expected, GH_INIT_BUSY,
            0, __ATOMIC_ACQUIRE, __ATOMIC_ACQUIRE)) {
        if (expected == GH_INIT_READY) return 0;
        return expected == GH_INIT_FAILED ? GH_LOAD(gh_init_error) : EBUSY;
    }
    int error = !gh_source_valid() ? EINVAL :
        !nlh_memory_enabled() ? ENOTSUP : nlh_register_platform_v2(NLH_PLATFORM_ABI, gh_platform);
    __atomic_store_n(&gh_init_error, error, __ATOMIC_RELAXED);
    __atomic_store_n(&gh_init_state, error ? GH_INIT_FAILED : GH_INIT_READY, __ATOMIC_RELEASE);
    return error;
}

int giulia_hotload_select(uint32_t domain, uint64_t epoch, uint32_t operation,
    const uint64_t *request, uint32_t request_words,
    uint64_t *response, uint32_t response_words)
{
    if (domain < GH_DOMAIN_GPU || domain > GH_DOMAIN_MODEM) {
        GH_INC(gh_invalid_requests);
        return EINVAL;
    }
    struct gh_mailbox *box = &gh_mailboxes[domain - 1];
    GH_INC(box->stats.attempts);
    if (!request || !response || !request_words || request_words > GH_MAX_WORDS ||
        !response_words || response_words > GH_MAX_WORDS)
        return gh_fallback(box, EINVAL, 1);
    if (__atomic_load_n(&gh_init_state, __ATOMIC_ACQUIRE) != GH_INIT_READY)
        return gh_fallback(box, ENXIO, 0);

    unsigned expected = 0;
    if (!__atomic_compare_exchange_n(&box->admission, &expected, 1,
            0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
        return gh_fallback(box, EBUSY, 0);
    int error = 0;
    if (epoch < GH_LOAD(box->high_epoch)) {
        error = ESTALE;
        goto release;
    }
    uint64_t sequence = __atomic_load_n(&gh_ticket_sequence, __ATOMIC_RELAXED);
    unsigned retries = 0;
    for (;;) {
        if (sequence >= GH_TICKET_LIMIT) { error = EOVERFLOW; goto release; }
        if (__atomic_compare_exchange_n(&gh_ticket_sequence, &sequence, sequence + 1,
                0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) break;
        if (++retries == 8) { error = EBUSY; goto release; }
    }
    __atomic_store_n(&box->high_epoch, epoch, __ATOMIC_RELAXED);
    box->epoch = epoch;
    box->operation = operation;
    box->request_words = request_words;
    box->response_words = response_words;
    box->read_mask = box->write_mask = 0;
    box->bound = 0;
    __atomic_store_n(&box->fault, 0, __ATOMIC_RELAXED);
    memcpy(box->request, request, request_words * sizeof(*request));
    bzero(box->response, sizeof(box->response));
    uint64_t ticket = GH_TICKET_TAG | (sequence + 1);
    __atomic_store_n(&box->ticket, ticket, __ATOMIC_RELEASE);
    GH_INC(box->stats.dispatches);
    uint64_t scalar = 0;
    error = nlh_dispatch_resident(gh_names[domain - 1], GH_INTERFACE_ABI,
        ticket, &scalar);

    uint64_t expected_ticket = ticket;
    if (!__atomic_compare_exchange_n(&box->ticket, &expected_ticket, 0,
            0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        /* A compliant callback cannot outlive synchronous dispatch. Preserve
         * static storage and permanently close this mailbox if it does. No
         * wait, reuse, response publication or unsafe cleanup is attempted.
         */
        __atomic_store_n(&box->ticket, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&box->poisoned, 1, __ATOMIC_RELEASE);
        GH_INC(box->stats.protocol_errors);
        return gh_fallback(box, EBUSY, 1);
    }
    if (error) {
        if (box->bound) GH_INC(box->stats.module_errors);
    } else {
        uint32_t complete = response_words == GH_MAX_WORDS ? ~0u : (1u << response_words) - 1u;
        error = __atomic_load_n(&box->fault, __ATOMIC_RELAXED);
        if (!error && (!box->bound || !box->read_mask || box->write_mask != complete))
            error = EPROTO;
        if (error) GH_INC(box->stats.protocol_errors);
        else {
            memcpy(response, box->response, response_words * sizeof(*response));
            GH_INC(box->stats.calls);
            __atomic_store_n(&box->stats.last_error, 0, __ATOMIC_RELAXED);
        }
    }
    bzero(box->request, sizeof(box->request));
    bzero(box->response, sizeof(box->response));
    bzero(&box->metadata, sizeof(box->metadata));
    box->bound = 0;
release:
    __atomic_store_n(&box->admission, 0, __ATOMIC_RELEASE);
    return error ? gh_fallback(box, error,
        error != EBUSY && error != ENOENT && error != ENXIO && error != ENOTSUP) : 0;
}

void giulia_hotload_status(void)
{
    unsigned state = __atomic_load_n(&gh_init_state, __ATOMIC_ACQUIRE);
    printf("NLH gateway abi=1 enabled=%u init_state=%u init_error=%d provider_registered=%u domains=4 max_words=32 hardware_access=0 driver_readiness=not-asserted\n",
        nlh_memory_enabled() != 0, state, GH_LOAD(gh_init_error), state == GH_INIT_READY);
    printf("%s\n", giulia_hotload_build);
    printf("NLH gateway invalid_requests=%llu denied_platform=%llu\n",
        (unsigned long long)GH_LOAD(gh_invalid_requests),
        (unsigned long long)GH_LOAD(gh_denied_platform));
    for (unsigned i = 0; i < GH_DOMAIN_COUNT; i++) {
        struct gh_mailbox *box = &gh_mailboxes[i];
        struct gh_counters *s = &box->stats;
        printf("NLH gateway domain=%u name=%s busy=%u poisoned=%u high_epoch=%llu attempts=%llu dispatches=%llu executed=%llu calls=%llu fallbacks=%llu rejected=%llu busy_fallbacks=%llu stale=%llu unavailable=%llu module_errors=%llu protocol_errors=%llu platform_denied=%llu request_reads=%llu response_writes=%llu last_error=%d image_version=%u image_cookie=%llu activation_epoch=%llu generation=%u invocation=%llu\n",
            i + 1, gh_names[i], __atomic_load_n(&box->admission, __ATOMIC_ACQUIRE),
            __atomic_load_n(&box->poisoned, __ATOMIC_ACQUIRE),
            (unsigned long long)GH_LOAD(box->high_epoch),
            (unsigned long long)GH_LOAD(s->attempts), (unsigned long long)GH_LOAD(s->dispatches),
            (unsigned long long)GH_LOAD(s->executed), (unsigned long long)GH_LOAD(s->calls),
            (unsigned long long)GH_LOAD(s->fallbacks), (unsigned long long)GH_LOAD(s->rejected),
            (unsigned long long)GH_LOAD(s->busy), (unsigned long long)GH_LOAD(s->stale),
            (unsigned long long)GH_LOAD(s->unavailable), (unsigned long long)GH_LOAD(s->module_errors),
            (unsigned long long)GH_LOAD(s->protocol_errors), (unsigned long long)GH_LOAD(s->platform_denied),
            (unsigned long long)GH_LOAD(s->request_reads), (unsigned long long)GH_LOAD(s->response_writes),
            GH_LOAD(s->last_error), GH_LOAD(s->image_version), (unsigned long long)GH_LOAD(s->image_cookie),
            (unsigned long long)GH_LOAD(s->activation_epoch), GH_LOAD(s->slot_generation),
            (unsigned long long)GH_LOAD(s->invocation_id));
    }
}
