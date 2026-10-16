/* Backend-lock-owned index of immutable uniform arena CPU snapshots.
 * Entries may remain indexed while idle; this index owns no GPU references.
 * The caller controls refs, active/shadow_valid, and publication after flush. */
enum { GRAPHICS_UNIFORM_CONTENT_SLOTS=GRAPHICS_UNIFORM_BANKS*GRAPHICS_UNIFORM_BANK_SLOTS,
       GRAPHICS_UNIFORM_CONTENT_BUCKETS=2048,GRAPHICS_UNIFORM_CONTENT_PROBE_LIMIT=16 };
typedef struct {
    unsigned refs,free_position,next,linked;
    uint64_t hash;
} graphics_uniform_content_entry;
static graphics_uniform_content_entry graphics_uniform_content[GRAPHICS_UNIFORM_CONTENT_SLOTS];
static unsigned graphics_uniform_content_buckets[GRAPHICS_UNIFORM_CONTENT_BUCKETS];

static uint64_t graphics_uniform_content_hash_mix(uint64_t hash,uint64_t value) {
    return (hash^value)*UINT64_C(1099511628211);
}
static uint64_t graphics_uniform_content_hash(const void *bytes,size_t payload) {
    const uint8_t *p=bytes;
    uint64_t hash=graphics_uniform_content_hash_mix(UINT64_C(14695981039346656037),payload);
    if(payload<=8) {
        for(size_t i=0;i<payload;i++)hash=graphics_uniform_content_hash_mix(hash,p[i]);
    } else {
        size_t last=payload-8;
        // All eight reads are bounded, even when several offsets coincide.
        // Work is independent of payload size; full equality decides a hit.
        size_t offsets[8]={0,last<8?last:8,last<16?last:16,last<24?last:24,
                           last/4,last/2,last-last/4,last};
        for(unsigned i=0;i<8;i++) {
            uint64_t word;__builtin_memcpy(&word,p+offsets[i],sizeof word);
            hash=graphics_uniform_content_hash_mix(hash,word);
        }
    }
    hash^=hash>>33;hash*=UINT64_C(0xff51afd7ed558ccd);
    hash^=hash>>33;hash*=UINT64_C(0xc4ceb9fe1a85ec53);
    return hash^(hash>>33);
}
// Return slot+1 without pinning or changing either the index or free stack.
static unsigned graphics_uniform_content_find(const void *bytes,size_t payload,uint64_t hash) {
    unsigned link=graphics_uniform_content_buckets[hash&(GRAPHICS_UNIFORM_CONTENT_BUCKETS-1)];
    unsigned probes=0;
    while(link && probes<GRAPHICS_UNIFORM_CONTENT_PROBE_LIMIT) {
        unsigned slot=link-1;
        if(slot>=GRAPHICS_UNIFORM_CONTENT_SLOTS)die("Vulkan uniform content index out of range");
        graphics_uniform_content_entry *entry=&graphics_uniform_content[slot];
        graphics_uniform_bank *bank=&graphics_uniform_banks[slot/GRAPHICS_UNIFORM_BANK_SLOTS];
        unsigned local=slot%GRAPHICS_UNIFORM_BANK_SLOTS;
        probes++;gpu_metrics.uniform_content_probes++;
        if(entry->linked && entry->hash==hash && bank->payload[local]==payload &&
           (bank->shadow_valid&(UINT64_C(1)<<local)) &&
           (!payload || (bank->shadow[local] && dm_vk_bytes_equal(bank->shadow[local],bytes,payload))))return link;
        link=entry->next;
    }
    if(link)gpu_metrics.uniform_content_limits++;
    return 0;
}
static void graphics_uniform_content_unlink(unsigned slot) {
    if(slot>=GRAPHICS_UNIFORM_CONTENT_SLOTS)die("Vulkan uniform content unlink out of range");
    graphics_uniform_content_entry *entry=&graphics_uniform_content[slot];
    if(entry->linked) {
        unsigned *link=&graphics_uniform_content_buckets[entry->hash&(GRAPHICS_UNIFORM_CONTENT_BUCKETS-1)];
        while(*link && *link!=slot+1)link=&graphics_uniform_content[*link-1].next;
        if(!*link)die("Vulkan uniform content entry missing from index");
        *link=entry->next;
    }
    entry->linked=0;entry->next=0;entry->hash=0;
}
// Publish only a successfully uploaded/flushed shadow, with its captured hash.
static void graphics_uniform_content_publish(unsigned slot,uint64_t hash) {
    if(slot>=GRAPHICS_UNIFORM_CONTENT_SLOTS)die("Vulkan uniform content publish out of range");
    graphics_uniform_content_entry *entry=&graphics_uniform_content[slot];
    if(entry->linked)die("Vulkan uniform content entry published twice");
    unsigned bucket=(unsigned)(hash&(GRAPHICS_UNIFORM_CONTENT_BUCKETS-1));
    entry->hash=hash;entry->next=graphics_uniform_content_buckets[bucket];entry->linked=1;
    graphics_uniform_content_buckets[bucket]=slot+1;
}
// Initialize a newly added bank's free_position to ~0u before its first push.
// A released/failed slot must be unpinned before push; contents may stay indexed.
static void graphics_uniform_free_push(unsigned slot) {
    if(slot>=GRAPHICS_UNIFORM_CONTENT_SLOTS || graphics_uniform_free_count>=GRAPHICS_UNIFORM_CONTENT_SLOTS)
        die("Vulkan uniform free stack overflow");
    graphics_uniform_content_entry *entry=&graphics_uniform_content[slot];
    graphics_uniform_bank *bank=&graphics_uniform_banks[slot/GRAPHICS_UNIFORM_BANK_SLOTS];
    uint64_t bit=UINT64_C(1)<<(slot%GRAPHICS_UNIFORM_BANK_SLOTS);
    if(entry->free_position!=~0u || entry->refs || (bank->active&bit))
        die("Vulkan uniform free slot already queued or pinned");
    entry->free_position=graphics_uniform_free_count;
    graphics_uniform_free_slots[graphics_uniform_free_count++]=slot;
}
// O(1) removal supports both the top slot on a miss and an arbitrary idle hit.
// The caller establishes refs/active only after taking the slot successfully.
static void graphics_uniform_free_take(unsigned slot) {
    if(slot>=GRAPHICS_UNIFORM_CONTENT_SLOTS)die("Vulkan uniform free take out of range");
    graphics_uniform_content_entry *entry=&graphics_uniform_content[slot];
    unsigned position=entry->free_position;
    graphics_uniform_bank *bank=&graphics_uniform_banks[slot/GRAPHICS_UNIFORM_BANK_SLOTS];
    uint64_t bit=UINT64_C(1)<<(slot%GRAPHICS_UNIFORM_BANK_SLOTS);
    if(position>=graphics_uniform_free_count || graphics_uniform_free_slots[position]!=slot ||
       entry->refs || (bank->active&bit))die("Vulkan uniform free take missing or pinned slot");
    unsigned last=graphics_uniform_free_slots[--graphics_uniform_free_count];
    if(position<graphics_uniform_free_count) {
        graphics_uniform_free_slots[position]=last;
        graphics_uniform_content[last].free_position=position;
    }
    entry->free_position=~0u;
}
