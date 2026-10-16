/* Lazy decompression keeps 6903 graphics modules out of resident memory until
 * their program is used. Reflection JSON is preserved for the graphics binder. */
#include <zlib.h>
#include <stdatomic.h>
#include "vk_fuse_shader.h"
static _Atomic uint64_t graphics_generation=1;
uint64_t dm_vk_graphics_generation(void) { return atomic_load(&graphics_generation); }
typedef struct graphics_entry {
    struct graphics_entry *next;dm_graphics_shader shader;void *allocation;
} graphics_entry;
static graphics_entry *graphics_loaded;
static FILE *graphics_file;
static uint32_t *graphics_index;
static uint32_t graphics_count;
static uint32_t graphics_guarded_uniform_abi;
static int graphics_has_fuse;
#include "vk_fuse_catalog.inc"
static int graphics_open(void) {
    if(graphics_file)return 0;
    const char *root=getenv("DM_ROOT");if(!root)return -1;
    char path[1100];snprintf(path,sizeof path,"%s/graphics.vkpack",root);
    FILE *file=fopen(path,"rb");if(!file)return -1;
    unsigned char magic[8];uint32_t count=0;
    if(fread(magic,1,8,file)!=8 || (memcmp(magic,"D2RVKSH2",8) && memcmp(magic,"D2RVKSH3",8) && memcmp(magic,"D2RVKSH4",8)) || fread(&count,4,1,file)!=1 || !count || count>100000){fclose(file);return -1;}
    uint32_t *index=malloc((size_t)count*48);
    if(!index || fread(index,48,count,file)!=count){free(index);fclose(file);return -1;}
    graphics_file=file;graphics_index=index;graphics_count=count;
    graphics_guarded_uniform_abi=!memcmp(magic,"D2RVKSH3",8);
    graphics_has_fuse=!memcmp(magic,"D2RVKSH4",8);
    LOG("Graphics archive opened: shaders=%u guardedUniformABI=%u perShaderABI=%d fuse=%d",count,graphics_guarded_uniform_abi,graphics_has_fuse,graphics_has_fuse);return 0;
}
static void graphics_catalog_close(void) {
    atomic_fetch_add(&graphics_generation,1);
    while(graphics_loaded){graphics_entry *e=graphics_loaded;graphics_loaded=e->next;graphics_fuse_free(e);free(e->allocation);free(e);}
    if(graphics_file)fclose(graphics_file);graphics_file=NULL;free(graphics_index);graphics_index=NULL;graphics_count=0;graphics_guarded_uniform_abi=0;
    graphics_has_fuse=0;graphics_fuse_bytes=0;
}
const dm_graphics_shader *dm_vk_graphics_lookup(const char *key) {
    pthread_mutex_lock(&lock);const dm_graphics_shader *result=NULL;
    if(!key || strlen(key)!=16)goto done;
    for(graphics_entry *e=graphics_loaded;e;e=e->next)if(!strcmp(key,e->shader.key)){result=&e->shader;goto done;}
    if(graphics_open())goto done;
    uint32_t low=0,high=graphics_count;
    while(low<high){uint32_t mid=low+(high-low)/2;int cmp=memcmp(key,graphics_index+mid*12,16);if(cmp>0)low=mid+1;else high=mid;}
    if(low==graphics_count || memcmp(key,graphics_index+low*12,16))goto done;
    const uint32_t *r=graphics_index+low*12;uint32_t stage=r[4],code=r[5],json=r[6],constants=r[7],map=r[8],offset=r[9],compressed=r[10],size=r[11];
    if((stage!=0 && stage!=1) || code<20 || (code&3) || !json || (uint64_t)code+json+constants+4>size || size>64*1024*1024 || !compressed || compressed>64*1024*1024)goto done;
    unsigned char *raw=malloc(size),*packed=malloc(compressed);graphics_entry *entry=calloc(1,sizeof *entry);
    if(!raw || !packed || !entry){free(raw);free(packed);free(entry);goto done;}
    uLongf unpacked=size;
    int ok=!fseek(graphics_file,offset,SEEK_SET) && fread(packed,1,compressed,graphics_file)==compressed &&
        uncompress(raw,&unpacked,packed,compressed)==Z_OK && unpacked==size && *(uint32_t *)raw==0x07230203 && raw[code+json-1]==0;
    free(packed);if(!ok){free(raw);free(entry);goto done;}
    unsigned nb=0;size_t tail=(size_t)code+json+constants;memcpy(&nb,raw+tail,4);
    if(nb>64 || tail+4+nb*sizeof(dm_shader_binding)>size ||
       (!graphics_has_fuse && tail+4+nb*sizeof(dm_shader_binding)!=size)){free(raw);free(entry);goto done;}
    entry->shader.nbindings=nb;memcpy(entry->shader.bindings,raw+tail+4,nb*sizeof(dm_shader_binding));
    memcpy(entry->shader.key,key,16);entry->shader.stage=stage;entry->shader.code=(uint32_t *)raw;entry->shader.code_size=code;
    entry->shader.guarded_uniform_abi=graphics_guarded_uniform_abi;
    entry->shader.reflection=(const char *)raw+code;entry->shader.constants=raw+code+json;entry->shader.constants_size=constants;entry->shader.fragment_map=map;
    if(graphics_has_fuse && graphics_fuse_parse(entry,raw,size,tail+4+nb*sizeof(dm_shader_binding))){graphics_fuse_free(entry);free(raw);free(entry);goto done;}
    entry->allocation=raw;entry->next=graphics_loaded;graphics_loaded=entry;result=&entry->shader;
done:
    pthread_mutex_unlock(&lock);return result;
}
