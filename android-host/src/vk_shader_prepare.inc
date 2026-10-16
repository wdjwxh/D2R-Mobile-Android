/* Versioned, little-endian ARM64 Metal pso_skey seed decoder. Read explicit
 * offsets rather than importing ObjC pointers/implementation-defined bitfields.
 * Vulkan has additional fixed-function state absent from the Metal PSO key.
 * These representative pipelines prime the driver's disk cache only; they are
 * never inserted into the exact guest draw-state cache. */
#define PREP_SEED_BYTES 432u
static uint32_t prep_u32(const unsigned char *p) {
    return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
}
static uint64_t prep_u64(const unsigned char *p) {return prep_u32(p)|((uint64_t)prep_u32(p+4)<<32);}
static int prep_hex(const char *s,unsigned char out[PREP_SEED_BYTES]) {
    if(strlen(s)!=PREP_SEED_BYTES*2)return -1;
    for(unsigned i=0;i<PREP_SEED_BYTES;i++) {
        unsigned value=0;
        for(unsigned j=0;j<2;j++) {unsigned c=(unsigned char)s[2*i+j];
            if(c>='0' && c<='9')c-='0';else if(c>='a' && c<='f')c=c-'a'+10;else return -1;
            value=value*16+c;
        }out[i]=(unsigned char)value;
    }return 0;
}
/* Upstream shader files use a stage suffix; Android catalogs index the hash. */
static int prep_key(const char *src,char out[17]) {
    size_t n=strlen(src);if(n!=16 && (n!=18 || strcmp(src+16,"_b")))return -1;
    for(unsigned i=0;i<16;i++)if(!((src[i]>='0' && src[i]<='9') || (src[i]>='a' && src[i]<='f')))return -1;
    memcpy(out,src,16);out[16]=0;return 0;
}
static VkFormat prep_format(unsigned metal) {
    switch(metal) {
    case 0:return VK_FORMAT_UNDEFINED;
    case 10:return VK_FORMAT_R8_UNORM;case 20:return VK_FORMAT_R16_UNORM;case 25:return VK_FORMAT_R16_SFLOAT;
    case 30:return VK_FORMAT_R8G8_UNORM;case 55:return VK_FORMAT_R16G16_SFLOAT;
    case 70:return VK_FORMAT_R8G8B8A8_UNORM;case 71:return VK_FORMAT_R8G8B8A8_SRGB;
    case 80:return VK_FORMAT_B8G8R8A8_UNORM;case 81:return VK_FORMAT_B8G8R8A8_SRGB;
    case 92:return VK_FORMAT_B10G11R11_UFLOAT_PACK32;case 115:return VK_FORMAT_R16G16B16A16_SFLOAT;
    case 250:return VK_FORMAT_D32_SFLOAT;case 260:return VK_FORMAT_D32_SFLOAT_S8_UINT;
    default:return VK_FORMAT_UNDEFINED;
    }
}
/* 0 compiled, 1 unsupported/unavailable, -1 actual driver/allocation failure. */
static int prep_graphics(const unsigned char *s) {
    if(s[50] || s[51] || s[48]!=0 || s[49]!=1 || prep_u32(s+144)>1)return 1;
    char vskey[25]={0},fskey[25]={0};memcpy(vskey,s,24);memcpy(fskey,s+24,24);
    char vkkey[17],fkkey[17];if(prep_key(vskey,vkkey) || prep_key(fskey,fkkey))return 1;
    dm_draw d={0};
    d.shader[0]=dm_vk_graphics_lookup(vkkey);d.shader[1]=dm_vk_graphics_lookup(fkkey);
    if(!d.shader[0] || !d.shader[1])return 1;
    if(d.shader[0]->stage!=0 || d.shader[1]->stage!=1)return 1;
    d.depth_compare=8;d.nattributes=16;d.alpha_to_coverage=prep_u32(s+156)!=0;
    for(unsigned i=0;i<2;i++)d.stencil[i]=(dm_stencil){.fail=1,.zfail=1,.zpass=1,.compare=8};
    VkAttachmentDescription ad[9]={0};VkAttachmentReference refs[8],depth={VK_ATTACHMENT_UNUSED,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    unsigned ncolor=0,na=0;
    for(unsigned i=0;i<8;i++) {
        unsigned fmt=prep_u32(s+104+4*i);refs[i]=(VkAttachmentReference){VK_ATTACHMENT_UNUSED,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        if(fmt) {VkFormat vk=prep_format(fmt);if(!vk)return 1;
            ad[na]=(VkAttachmentDescription){.format=vk,.samples=VK_SAMPLE_COUNT_1_BIT,.loadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE,.storeOp=VK_ATTACHMENT_STORE_OP_DONT_CARE,.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED,.finalLayout=VK_IMAGE_LAYOUT_GENERAL};
            refs[i].attachment=na++;ncolor=i+1;
        }
        uint64_t b=prep_u64(s+160+8*i);dm_blend *dst=&d.blend[i];
        dst->enabled=(prep_u32(s+148)>>i)&1;dst->mask=(prep_u32(s+152)>>(4*i))&15;
        dst->src_rgb=(b>>3)&127;dst->dst_rgb=(b>>10)&127;dst->src_alpha=(b>>17)&127;dst->dst_alpha=(b>>24)&127;dst->eq_rgb=(b>>31)&7;dst->eq_alpha=(b>>34)&7;
        if(!dst->enabled){dst->src_rgb=dst->src_alpha=2;dst->dst_rgb=dst->dst_alpha=1;dst->eq_rgb=dst->eq_alpha=1;}
    }
    unsigned df=prep_u32(s+136),sf=prep_u32(s+140);
    if(df || sf) {if(df && sf && df!=sf)return 1;VkFormat vk=prep_format(df?df:sf);if(!vk)return 1;
        depth.attachment=na;ad[na++]=(VkAttachmentDescription){.format=vk,.samples=VK_SAMPLE_COUNT_1_BIT,.loadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE,.storeOp=VK_ATTACHMENT_STORE_OP_DONT_CARE,.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE,.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE,.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED,.finalLayout=VK_IMAGE_LAYOUT_GENERAL};
    }
    if(!na)return 1;
    VkVertexInputAttributeDescription attrs[16];VkVertexInputBindingDescription streams[16];unsigned natt=0,nstream=0;uint32_t locations=0,used=0;
    if(draw_vertex_locations(d.shader[0],&locations))return 1;
    for(unsigned i=0;i<16;i++)if(locations&(1u<<i)) {
        uint32_t a=prep_u32(s+224+4*i);unsigned binding=a>>27;VkFormat format=image_format(a&255);
        if(!format || binding>=16)return 1;
        attrs[natt++]=(VkVertexInputAttributeDescription){i,binding,format,(a>>8)&0x7ffff};used|=1u<<binding;
    }
    for(unsigned i=0;i<16;i++)if(used&(1u<<i)) {
        unsigned stride=prep_u32(s+288+8*i),divisor=prep_u32(s+292+8*i);
        if(divisor>1)return 1;
        streams[nstream++]=(VkVertexInputBindingDescription){i,stride,divisor?VK_VERTEX_INPUT_RATE_INSTANCE:VK_VERTEX_INPUT_RATE_VERTEX};
    }
    VkRenderPass pass=VK_NULL_HANDLE;VkPipeline pipeline=VK_NULL_HANDLE;VkPipelineLayout layout=VK_NULL_HANDLE;
    VkDescriptorSetLayout sets[2]={0};VkShaderModule modules[2]={0};int result=-1;
    pthread_mutex_lock(&lock);
    for(unsigned i=0;i<natt;i++){VkFormatProperties props;vkGetPhysicalDeviceFormatProperties(v.gpu,attrs[i].format,&props);if(!(props.bufferFeatures&VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)){result=1;goto done;}}
    for(unsigned i=0;i<na;i++){VkFormatProperties props;vkGetPhysicalDeviceFormatProperties(v.gpu,ad[i].format,&props);VkFormatFeatureFlags need=i==depth.attachment?VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT:VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;if(!(props.optimalTilingFeatures&need)){result=1;goto done;}}
    VkSubpassDescription sub={.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS,.colorAttachmentCount=ncolor,.pColorAttachments=refs,.pDepthStencilAttachment=depth.attachment!=VK_ATTACHMENT_UNUSED?&depth:NULL};
    VkRenderPassCreateInfo rp={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,.attachmentCount=na,.pAttachments=ad,.subpassCount=1,.pSubpasses=&sub};
    if(vkCreateRenderPass(v.device,&rp,NULL,&pass))goto done;
    uint64_t profiles[2];graphics_dynamic_profiles(d.shader,profiles);
    for(unsigned stage=0;stage<2;stage++) {
        const dm_graphics_shader *sh=d.shader[stage];VkDescriptorSetLayoutBinding bindings[64];if(sh->nbindings>64)goto done;
        for(unsigned i=0;i<sh->nbindings;i++)bindings[i]=(VkDescriptorSetLayoutBinding){sh->bindings[i].binding,graphics_binding_type(&sh->bindings[i],profiles[stage],i),1,stage?VK_SHADER_STAGE_FRAGMENT_BIT:VK_SHADER_STAGE_VERTEX_BIT,NULL};
        VkDescriptorSetLayoutCreateInfo dc={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=sh->nbindings,.pBindings=bindings};
        if(vkCreateDescriptorSetLayout(v.device,&dc,NULL,&sets[stage]))goto done;
        modules[stage]=draw_module(sh,stage);if(!modules[stage])goto done;
    }
    VkPushConstantRange push={VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,128};
    VkPipelineLayoutCreateInfo lc={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=2,.pSetLayouts=sets,.pushConstantRangeCount=1,.pPushConstantRanges=&push};
    if(vkCreatePipelineLayout(v.device,&lc,NULL,&layout))goto done;
    result=draw_pipeline_create(&d,pass,layout,modules,VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,nstream,streams,natt,attrs,ncolor,&pipeline);
done:
    if(pipeline)vkDestroyPipeline(v.device,pipeline,NULL);
    if(layout)vkDestroyPipelineLayout(v.device,layout,NULL);
    for(unsigned i=0;i<2;i++){if(modules[i])vkDestroyShaderModule(v.device,modules[i],NULL);if(sets[i])vkDestroyDescriptorSetLayout(v.device,sets[i],NULL);}
    if(pass)vkDestroyRenderPass(v.device,pass,NULL);
    pthread_mutex_unlock(&lock);return result;
}
static uint64_t prep_hash(uint64_t h,const void *bytes,size_t n) {
    const unsigned char *p=bytes;while(n--){h^=*p++;h*=UINT64_C(1099511628211);}return h;
}
typedef struct {uint64_t identity,size;uint32_t crc,version;} prep_certificate;
static uint64_t prep_qualified_identity;
static char prep_qualified_stamp[1100];
/* Called under the backend lock, after the exact cache bytes have been closed,
 * fsynced and renamed. Runtime checkpoints maintain only an already qualified
 * identity; partial/cold preparation can never mint a certificate. */
static int prep_cache_saved(const void *data,size_t size) {
    if(!prep_qualified_stamp[0])return 0;
    uint32_t header=0;if(size>=32)memcpy(&header,data,4);
    if(header<32 || size<=header)return -1;
    uLong crc=crc32(0L,Z_NULL,0);const unsigned char *bytes=data;
    for(size_t at=0;at<size;){size_t n=size-at;if(n>65536)n=65536;crc=crc32(crc,bytes+at,(uInt)n);at+=n;}
    prep_certificate certificate={prep_qualified_identity,size,(uint32_t)crc,2};
    char temp[1140];snprintf(temp,sizeof temp,"%s.tmp",prep_qualified_stamp);
    FILE *f=fopen(temp,"wb");if(!f)return -1;
    int ok=fwrite(&certificate,1,sizeof certificate,f)==sizeof certificate;
    if(ok && (fflush(f) || fsync(fileno(f))))ok=0;
    if(fclose(f))ok=0;
    return ok && !rename(temp,prep_qualified_stamp)?0:-1;
}
static int prep_file_crc(const char *path,uint64_t *size,uint32_t *checksum) {
    FILE *f=fopen(path,"rb");if(!f)return -1;unsigned char bytes[65536];size_t n;
    uint64_t count=0;uLong crc=crc32(0L,Z_NULL,0);
    while((n=fread(bytes,1,sizeof bytes,f))){crc=crc32(crc,bytes,(uInt)n);count+=n;}
    int bad=ferror(f);fclose(f);*size=count;*checksum=(uint32_t)crc;return bad?-1:0;
}
static int prep_hash_file(const char *path,uint64_t *hash) {
    uint64_t size;uint32_t crc;if(prep_file_crc(path,&size,&crc))return -1;
    *hash=prep_hash(*hash,&size,sizeof size);*hash=prep_hash(*hash,&crc,sizeof crc);return 0;
}
int dm_vk_prepare_shaders(const char *root,const char *manifest,dm_vk_prepare_progress cb,void *ctx) {
    prep_qualified_stamp[0]=0;prep_qualified_identity=0;
    if(cb)cb(ctx,0,0,"Checking pipeline cache");
    if(!root || !*root || !manifest || !*manifest)return -1;
    const char *active=getenv("DM_ROOT");if(!active || strcmp(active,root))return -1;
    char archive[1100],stamp[1100];
    if(snprintf(archive,sizeof archive,"%s/graphics.vkpack",root)>=(int)sizeof archive)return -1;
    uint64_t hash=UINT64_C(14695981039346656037);const char version[]="android-pso-seeds-v2-cache-certificate-default-fixed-state";
    hash=prep_hash(hash,version,sizeof version);
    if(prep_hash_file(archive,&hash) || prep_hash_file(manifest,&hash)){if(cb)cb(ctx,0,0,"ERROR: Shader archive or pipeline manifest unavailable");return -1;}
    for(unsigned i=0;i<sizeof compute_catalog/sizeof *compute_catalog;i++) {
        const dm_compute_shader *s=&compute_catalog[i];hash=prep_hash(hash,s->key,strlen(s->key));hash=prep_hash(hash,s->code,s->code_size);
        hash=prep_hash(hash,&s->code_size,sizeof s->code_size);
        hash=prep_hash(hash,&s->nbindings,sizeof s->nbindings);hash=prep_hash(hash,s->bindings,s->nbindings*sizeof *s->bindings);
        hash=prep_hash(hash,&s->constants_size,sizeof s->constants_size);hash=prep_hash(hash,s->constants,s->constants_size);
        hash=prep_hash(hash,s->local,sizeof s->local);hash=prep_hash(hash,&s->builtin,sizeof s->builtin);
    }
    if(dm_vk_open()){if(cb)cb(ctx,0,0,"ERROR: Vulkan device unavailable");return -1;}
    VkPhysicalDeviceProperties props;pthread_mutex_lock(&lock);vkGetPhysicalDeviceProperties(v.gpu,&props);
    int cache_error=compute_cache_open(),imported=compute_cache_imported;pthread_mutex_unlock(&lock);
    hash=prep_hash(hash,&props.vendorID,sizeof props.vendorID);hash=prep_hash(hash,&props.deviceID,sizeof props.deviceID);
    hash=prep_hash(hash,&props.driverVersion,sizeof props.driverVersion);hash=prep_hash(hash,props.pipelineCacheUUID,VK_UUID_SIZE);
    if(cache_error){if(cb)cb(ctx,0,0,"ERROR: Vulkan pipeline cache unavailable");return -1;}
    snprintf(stamp,sizeof stamp,"%s/vk-prepare-%016llx.stamp",root,(unsigned long long)hash);
    FILE *f=fopen(stamp,"rb");prep_certificate saved={0};
    if(f){size_t n=fread(&saved,1,sizeof saved,f);int extra=fgetc(f);fclose(f);
        uint64_t cache_size=0;uint32_t cache_crc=0;
        if(n==sizeof saved && extra==EOF && saved.version==2 && saved.identity==hash && imported &&
           !prep_file_crc(compute_cache_path,&cache_size,&cache_crc) && saved.size==cache_size && saved.crc==cache_crc) {
            prep_qualified_identity=hash;snprintf(prep_qualified_stamp,sizeof prep_qualified_stamp,"%s",stamp);
            LOG("Vulkan warmup cache-fast=1 graphics=0 compute=0 skipped=0 failed=0");
            if(cb)cb(ctx,1,1,"Pipeline cache ready");return 0;
        }
    }
    f=fopen(manifest,"rb");if(!f)return -1;
    char line[2048];unsigned total=0,done_count=0,graphics=0,compute=0,skip=0,fail=0,ordinary=0;
    while(fgets(line,sizeof line,f))if((line[0]=='R' || line[0]=='C') && line[1]=='\t')total++;
    if(ferror(f)){fclose(f);return -1;}rewind(f);
    if(cb)cb(ctx,0,total,"Compiling common Vulkan pipelines");
    while(fgets(line,sizeof line,f)) {
        if((line[0]!='R' && line[0]!='C') || line[1]!='\t')continue;
        line[strcspn(line,"\r\n")]=0;int result=1;
        if(line[0]=='R') {unsigned char seed[PREP_SEED_BYTES];if(!prep_hex(line+2,seed)){if(!seed[50] && !seed[51] && seed[48]==0 && seed[49]==1)ordinary++;result=prep_graphics(seed);}if(!result)graphics++;}
        else {
            char key[17];const dm_compute_shader *s=prep_key(line+2,key)?NULL:dm_vk_compute_lookup(key);
            if(s){pthread_mutex_lock(&lock);result=compute_pipeline_get(s)?0:-1;pthread_mutex_unlock(&lock);}
            if(!result)compute++;
        }
        if(result>0)skip++;else if(result<0)fail++;
        done_count++;
        if(cb)cb(ctx,done_count,total,"Compiling common Vulkan pipelines");
    }
    if(ferror(f))fail++;fclose(f);
    int unusable=!total || !(graphics+compute) || (ordinary && !graphics);
    if(unusable)fail++;
    /* Qualify only a complete preparation. The save observer binds the exact
     * cache bytes and will refresh the certificate after runtime checkpoints. */
    if(!fail){prep_qualified_identity=hash;snprintf(prep_qualified_stamp,sizeof prep_qualified_stamp,"%s",stamp);}
    pthread_mutex_lock(&lock);int persisted=compute_cache_save();pthread_mutex_unlock(&lock);
    if(persisted)fail++;
    if(fail){prep_qualified_stamp[0]=0;prep_qualified_identity=0;}
    LOG("Vulkan warmup cache-fast=0 graphics=%u compute=%u skipped=%u failed=%u total=%u (representative fixed state; runtime variants remain lazy)",graphics,compute,skip,fail,total);
    int result=unusable?-1:fail?1:0;
    char summary[192];snprintf(summary,sizeof summary,"%sgraphics=%u compute=%u skipped=%u failed=%u",result<0?"ERROR: ":result>0?"WARNING: ":"Ready: ",graphics,compute,skip,fail);
    if(cb)cb(ctx,done_count,total,summary);
    return result;
}
